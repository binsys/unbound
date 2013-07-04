/*
 * addrtree -- radix tree for vandergaast cache.
 *
 * Copyright (c) 2013, NLnet Labs.  See LICENSE for license.
 */

/** \file see addrtree.h */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "util/data/msgreply.h"
#include "util/module.h"
#include "addrtree.h"

/** 
 * Create a new edge
 * @param node: Child node this edge will connect to.
 * @param addr: full key to this edge.
 * @param addrlen: length of relevant part of key for this node
 * @return new addredge or NULL on failure
 * */
struct addredge* 
edge_create(struct addrnode* node, const addrkey_t* addr, addrlen_t addrlen)
{
	size_t n;
	struct addredge* edge = (struct addredge*)malloc( sizeof(*edge) );
	if (!edge)
		return NULL;
	edge->node = node;
	edge->len = addrlen;
	n = (addrlen / KEYWIDTH) + ((addrlen % KEYWIDTH)!=0);
	edge->str = (addrkey_t*)calloc(n, sizeof(addrkey_t));
	if (!edge->str) {
		free(edge);
		return NULL;
	}
	memcpy(edge->str, addr, n * sizeof(addrkey_t));
	return edge;
}

/** 
 * Create a new node
 * @param elem: Element to store at this node
 * @param scope: Scopemask from server reply
 * @return new addrnode or NULL on failure
 * */
struct addrnode* 
node_create(struct reply_info* elem, addrlen_t scope)
{
	struct addrnode* node = (struct addrnode*)malloc( sizeof(*node) );
	if (!node)
		return NULL;
	node->elem = elem;
	node->scope = scope;
	node->edge[0] = NULL;
	node->edge[1] = NULL;
	return node;
}

struct addrtree* addrtree_create(addrlen_t max_depth, struct module_env* env)
{
	struct addrtree* tree;
	assert(env != NULL);
	tree = (struct addrtree*)malloc( sizeof(*tree) );
	if(!tree)
		return NULL;
	tree->root = node_create(NULL, 0);
	if (!tree->root) {
		free(tree);
		return NULL;
	}
	tree->max_depth = max_depth;
	tree->env = env;
	return tree;
}

void addrtree_clean_node(const struct addrtree* tree, struct addrnode* node)
{
	if (node->elem) {
		reply_info_parsedelete(node->elem, tree->env->alloc);
		node->elem = NULL;
	}
}

/** 
 * Free node and all nodes below
 * @param tree: Tree the node lives in.
 * @param node: Node to be freed
 * */
void freenode_recursive(struct addrtree* tree, struct addrnode* node)
{
	struct addredge* edge;
	int i;
	
	for (i = 0; i < 2; i++) {
		edge = node->edge[i];
		if (edge) {
			assert(edge->node != NULL);
			assert(edge->str != NULL);
			freenode_recursive(tree, edge->node);
			free(edge->str);
		}
		free(edge);
	}
	addrtree_clean_node(tree, node);
	free(node);
}

void addrtree_delete(struct addrtree* tree)
{
	if (tree) {
		if (tree->root)
			freenode_recursive(tree, tree->root);
		free(tree);
	}
}

/** Get N'th bit from address */
int getbit(const addrkey_t* addr, addrlen_t addrlen, addrlen_t n)
{
	assert(addrlen > n);
	return (int)(addr[n/KEYWIDTH]>>((KEYWIDTH-1)-(n%KEYWIDTH))) & 1;
}

/** Test for equality on N'th bit.
 * @return 0 for equal, 1 otherwise 
 * */
inline int 
cmpbit(const addrkey_t* key1, const addrkey_t* key2, addrlen_t n)
{
	addrkey_t c = key1[n/KEYWIDTH] ^ key2[n/KEYWIDTH];
	return (int)(c >> ((KEYWIDTH-1)-(n%KEYWIDTH))) & 1;
}

/**
 * Common number of bits in prefix
 * @param s1: 
 * @param l1: Length of s1 in bits
 * @param s2:
 * @param l2: Length of s2 in bits
 * @param skip: Nr of bits already checked.
 * @return Common number of bits.
 * */
addrlen_t bits_common(const addrkey_t* s1, addrlen_t l1, 
	const addrkey_t* s2, addrlen_t l2, addrlen_t skip)
{
	addrlen_t len, i;
	len = (l1 > l2) ? l2 : l1;
	assert(skip < len);
	for (i = skip; i < len; i++) {
		if (cmpbit(s1, s2, i)) return i;
	}
	return len;
} 

/**
 * Tests if s1 is a substring of s2
 * @param s1: 
 * @param l1: Length of s1 in bits
 * @param s2:
 * @param l2: Length of s2 in bits
 * @param skip: Nr of bits already checked.
 * @return 1 for substring, 0 otherwise 
 * */
int issub(const addrkey_t* s1, addrlen_t l1, 
	const addrkey_t* s2, addrlen_t l2,  addrlen_t skip)
{
	return bits_common(s1, l1, s2, l2, skip) == l1;
}

void
addrtree_insert(struct addrtree* tree, const addrkey_t* addr, 
	addrlen_t sourcemask, addrlen_t scope, struct reply_info* elem)
{
	struct addrnode* newnode, *node;
	struct addredge* edge, *newedge;
	size_t index;
	addrlen_t common, depth;

	node = tree->root;
	assert(node != NULL);

	/* Protect our cache against to much fine-grained data */
	if (tree->max_depth < scope) scope = tree->max_depth;
	/* Server answer was less specific than question */
	if (scope < sourcemask) sourcemask = scope;

	depth = 0;
	while (1) {
		assert(depth <= sourcemask);
		/* Case 1: update existing node */
		if (depth == sourcemask) {
			/* update this node's scope and data */
			if (node->elem)
				reply_info_parsedelete(node->elem, tree->env->alloc);
			node->elem = elem;
			node->scope = scope;
			return;
		}
		index = (size_t)getbit(addr, sourcemask, depth);
		edge = node->edge[index];
		/* Case 2: New leafnode */
		if (!edge) {
			newnode = node_create(elem, scope);
			node->edge[index] = edge_create(newnode, addr, sourcemask);
			return;
		}
		/* Case 3: Traverse edge */
		common = bits_common(edge->str, edge->len, addr, sourcemask, depth);
		if (common == edge->len) {
			/* We update the scope of intermediate nodes. Apparently the
			 * authority changed its mind. If we would not do this we 
			 * might not be able to reach our new node */
			node->scope = scope;
			depth = edge->len;
			node = edge->node;
			continue;
		}
		/* Case 4: split. */
		newnode = node_create(NULL, 0);
		newedge = edge_create(newnode, addr, common);
		node->edge[index] = newedge;
		index = (size_t)getbit(edge->str, edge->len, common);
		newnode->edge[index] = edge;
		
		if (common == sourcemask) {
			/* Data is stored in the node */
			newnode->elem = elem;
			newnode->scope = scope;
		} else {
			/* Data is stored in other leafnode */
			node = newnode;
			newnode = node_create(elem, scope);
			node->edge[index^1] = edge_create(newnode, addr, sourcemask);
		}
		return;
	}
}

struct addrnode*
addrtree_find(const struct addrtree* tree, const addrkey_t* addr, 
	addrlen_t sourcemask)
{
	struct addrnode* node = tree->root;
	struct addredge* edge;
	addrlen_t depth = 0;

	assert(node != NULL);
	while (1) {
		/* Current node more specific then question. */
		assert(depth <= sourcemask);
		/* does this node have data? if yes, see if we have a match */
		if (node->elem) {
			assert(node->scope >= depth); /* saved at wrong depth */
			if (depth == node->scope ||
					(node->scope > sourcemask && depth == sourcemask)) {
				/* Authority indicates it does not have a more precise
				 * answer or we cannot ask a more specific question */
				return node;
			}
		}
		/* This is our final depth, but we haven't found an answer. */
		if (depth == sourcemask)
			return NULL;
		/* Find an edge to traverse */
		edge = node->edge[getbit(addr, sourcemask, depth)];
		if (!edge || !edge->node)
			return NULL;
		if (edge->len > sourcemask )
			return NULL;
		if (!issub(edge->str, edge->len, addr, sourcemask, depth))
			return NULL;
		assert(depth < edge->len);
		depth = edge->len;
		node = edge->node;
	}
}

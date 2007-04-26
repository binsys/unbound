/*
 * util/storage/slabhash.h - hashtable consisting of several smaller tables.
 *
 * Copyright (c) 2007, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * Hash table that consists of smaller hash tables.
 * It cannot grow, but that gives it the ability to have multiple
 * locks. Also this means there are multiple LRU lists.
 */

#ifndef UTIL_STORAGE_SLABHASH_H
#define UTIL_STORAGE_SLABHASH_H
#include "util/storage/lruhash.h"

/** default number of slabs */
#define HASH_DEFAULT_SLABS 4

/**
 * Hash table formed from several smaller ones. 
 * This results in a partitioned lruhash table, a 'slashtable'.
 * None of the data inside the slabhash may be altered.
 * Therefore, no locks are needed to access the structure.
 */
struct slabhash {
	/** the size of the array - must be power of 2 */
	size_t size;
	/** size bitmask - uses high bits. */
	uint32_t mask;
	/** shift right this many bits to get index into array. */
	unsigned int shift;
	/** lookup array of hash tables */
	struct lruhash** array;
};

/**
 * Create new slabbed hash table.
 * @param numtables: number of hash tables to use, other parameters used to
 *	initialize these smaller hashtables.
 * @param start_size: size of hashtable array at start, must be power of 2.
 * @param maxmem: maximum amount of memory this table is allowed to use.
 *	so every table gets maxmem/numtables to use for itself.
 * @param sizefunc: calculates memory usage of entries.
 * @param compfunc: compares entries, 0 on equality.
 * @param delkeyfunc: deletes key.
 * @param deldatafunc: deletes data. 
 * @param arg: user argument that is passed to user function calls.
 * @return: new hash table or NULL on malloc failure.
 */
struct slabhash* slabhash_create(size_t numtables, size_t start_size, 
	size_t maxmem, lruhash_sizefunc_t sizefunc, 
	lruhash_compfunc_t compfunc, lruhash_delkeyfunc_t delkeyfunc, 
	lruhash_deldatafunc_t deldatafunc, void* arg);

/**
 * Delete hash table. Entries are all deleted.
 * @param table: to delete.
 */
void slabhash_delete(struct slabhash* table);

/**
 * Insert a new element into the hashtable, uses lruhash_insert. 
 * If key is already present data pointer in that entry is updated.
 *
 * @param table: hash table.
 * @param hash: hash value. User calculates the hash.
 * @param entry: identifies the entry.
 * 	If key already present, this entry->key is deleted immediately.
 *	But entry->data is set to NULL before deletion, and put into
 * 	the existing entry. The data is then freed.
 * @param data: the data.
 */
void slabhash_insert(struct slabhash* table, hashvalue_t hash, 
	struct lruhash_entry* entry, void* data);

/**
 * Lookup an entry in the hashtable. Uses lruhash_lookup.
 * At the end of the function you hold a (read/write)lock on the entry.
 * The LRU is updated for the entry (if found).
 * @param table: hash table.
 * @param hash: hash of key.
 * @param key: what to look for, compared against entries in overflow chain.
 *    the hash value must be set, and must work with compare function.
 * @param wr: set to true if you desire a writelock on the entry.
 *    with a writelock you can update the data part.
 * @return: pointer to the entry or NULL. The entry is locked.
 *    The user must unlock the entry when done.
 */
struct lruhash_entry* slabhash_lookup(struct slabhash* table, 
	hashvalue_t hash, void* key, int wr);

/**
 * Remove entry from hashtable. Does nothing if not found in hashtable.
 * Delfunc is called for the entry. Uses lruhash_remove.
 * @param table: hash table.
 * @param hash: hash of key.
 * @param key: what to look for. 
 */
void slabhash_remove(struct slabhash* table, hashvalue_t hash, void* key);

/**
 * Output debug info to the log as to state of the hash table.
 * @param table: hash table.
 * @param id: string printed with table to identify the hash table.
 * @param extended: set to true to print statistics on overflow bin lengths.
 */
void slabhash_status(struct slabhash* table, const char* id, int extended);

/**
 * Retrieve slab hash total size.
 * @param table: hash table.
 */
size_t slabhash_get_size(struct slabhash* table);

#endif /* UTIL_STORAGE_SLABHASH_H */

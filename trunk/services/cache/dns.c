/*
 * services/cache/dns.c - Cache services for DNS using msg and rrset caches.
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
 * This file contains the DNS cache.
 */
#include "config.h"
#include "iterator/iter_delegpt.h"
#include "services/cache/dns.h"
#include "services/cache/rrset.h"
#include "util/data/msgreply.h"
#include "util/data/packed_rrset.h"
#include "util/data/dname.h"
#include "util/module.h"
#include "util/net_help.h"
#include "util/region-allocator.h"

/** store rrsets in the rrset cache. */
static void
store_rrsets(struct module_env* env, struct reply_info* rep, uint32_t now)
{
        size_t i;
        /* see if rrset already exists in cache, if not insert it. */
        for(i=0; i<rep->rrset_count; i++) {
                rep->ref[i].key = rep->rrsets[i];
                rep->ref[i].id = rep->rrsets[i]->id;
                if(rrset_cache_update(env->rrset_cache, &rep->ref[i],
                        env->alloc, now)) /* it was in the cache */
                        rep->rrsets[i] = rep->ref[i].key;
        }
}

void 
dns_cache_store_msg(struct module_env* env, struct query_info* qinfo,
	hashvalue_t hash, struct reply_info* rep)
{
	struct msgreply_entry* e;
	uint32_t now = time(NULL), ttl = rep->ttl;
	size_t i;

	/* store RRsets */
        for(i=0; i<rep->rrset_count; i++) {
                rep->ref[i].key = rep->rrsets[i];
                rep->ref[i].id = rep->rrsets[i]->id;
	}
	reply_info_sortref(rep);
	reply_info_set_ttls(rep, now);
	store_rrsets(env, rep, now);
	if(ttl == 0) {
		/* we do not store the message, but we did store the RRs,
		 * which could be useful for delegation information */
		verbose(VERB_ALGO, "TTL 0: dropped msg from cache");
		return;
	}

	/* store msg in the cache */
	reply_info_sortref(rep);
	if(!(e = query_info_entrysetup(qinfo, rep, hash))) {
		log_err("store_msg: malloc failed");
		return;
	}
	slabhash_insert(env->msg_cache, hash, &e->entry, rep, env->alloc);
}

/** allocate rrset in region - no more locks needed */
static struct ub_packed_rrset_key*
copy_rrset(struct ub_packed_rrset_key* key, struct region* region, 
	uint32_t now)
{
	struct ub_packed_rrset_key* ck = region_alloc(region, 
		sizeof(struct ub_packed_rrset_key));
	struct packed_rrset_data* d;
	struct packed_rrset_data* data = (struct packed_rrset_data*)
		key->entry.data;
	size_t dsize, i;
	if(!ck)
		return NULL;
	ck->id = key->id;
	memset(&ck->entry, 0, sizeof(ck->entry));
	ck->entry.hash = key->entry.hash;
	ck->entry.key = ck;
	ck->rk = key->rk;
	ck->rk.dname = region_alloc_init(region, key->rk.dname, 
		key->rk.dname_len);
	if(!ck->rk.dname)
		return NULL;
	dsize = packed_rrset_sizeof(data);
	d = (struct packed_rrset_data*)region_alloc_init(region, data, dsize);
	if(!d)
		return NULL;
	ck->entry.data = d;
	packed_rrset_ptr_fixup(d);
	/* make TTLs relative - once per rrset */
	for(i=0; i<d->count + d->rrsig_count; i++)
		d->rr_ttl[i] -= now;
	d->ttl -= now;
	return ck;
}

/** find closest NS and returns the rrset (locked) */
static struct ub_packed_rrset_key*
find_deleg_ns(struct module_env* env, uint8_t* qname, size_t qnamelen, 
	uint16_t qclass, uint32_t now)
{
	struct ub_packed_rrset_key *rrset;
	uint8_t lablen;

	/* snip off front part of qname until NS is found */
	while(qnamelen > 0) {
		if((rrset = rrset_cache_lookup(env->rrset_cache, qname, 
			qnamelen, LDNS_RR_TYPE_NS, qclass, 0, now, 0)))
			return rrset;

		/* snip off front label */
		lablen = *qname;
		qname += lablen + 1;
		qnamelen -= lablen + 1;
	}
	return NULL;
}

/** add addr to additional section */
static void
addr_to_additional(struct ub_packed_rrset_key* rrset, struct region* region,
	struct dns_msg* msg, uint32_t now)
{
	if((msg->rep->rrsets[msg->rep->rrset_count] = 
		copy_rrset(rrset, region, now))) {
		msg->rep->ar_numrrsets++;
		msg->rep->rrset_count++;
	}
}

/** find and add A and AAAA records for nameservers in delegpt */
static int
find_add_addrs(struct module_env* env, uint16_t qclass, struct region* region,
	struct delegpt* dp, uint32_t now, struct dns_msg** msg)
{
	struct delegpt_ns* ns;
	struct ub_packed_rrset_key* akey;
	for(ns = dp->nslist; ns; ns = ns->next) {
		akey = rrset_cache_lookup(env->rrset_cache, ns->name, 
			ns->namelen, LDNS_RR_TYPE_A, qclass, 0, now, 0);
		if(akey) {
			if(!delegpt_add_rrset_A(dp, region, akey)) {
				lock_rw_unlock(&akey->entry.lock);
				return 0;
			}
			if(msg)
				addr_to_additional(akey, region, *msg, now);
			lock_rw_unlock(&akey->entry.lock);
		}
		akey = rrset_cache_lookup(env->rrset_cache, ns->name, 
			ns->namelen, LDNS_RR_TYPE_AAAA, qclass, 0, now, 0);
		if(akey) {
			if(!delegpt_add_rrset_AAAA(dp, region, akey)) {
				lock_rw_unlock(&akey->entry.lock);
				return 0;
			}
			if(msg)
				addr_to_additional(akey, region, *msg, now);
			lock_rw_unlock(&akey->entry.lock);
		}
	}
	return 1;
}

/** find and add DS or NSEC to delegation msg */
static void
find_add_ds(struct module_env* env, struct region* region, 
	struct dns_msg* msg, struct delegpt* dp, uint32_t now)
{
	/* Lookup the DS or NSEC at the delegation point. */
	struct ub_packed_rrset_key* rrset = rrset_cache_lookup(
		env->rrset_cache, dp->name, dp->namelen, LDNS_RR_TYPE_DS, 
		msg->qinfo.qclass, 0, now, 0);
	if(!rrset) {
		/* NOTE: this won't work for alternate NSEC schemes 
		 *	(opt-in, NSEC3) */
		rrset = rrset_cache_lookup(env->rrset_cache, dp->name, 
			dp->namelen, LDNS_RR_TYPE_NSEC, msg->qinfo.qclass, 
			0, now, 0);
		/* Note: the PACKED_RRSET_NSEC_AT_APEX flag is not used.
		 * since this is a referral, we need the NSEC at the parent
		 * side of the zone cut, not the NSEC at apex side. */
	}
	if(rrset) {
		/* add it to auth section. This is the second rrset. */
		if((msg->rep->rrsets[msg->rep->rrset_count] = 
			copy_rrset(rrset, region, now))) {
			msg->rep->ns_numrrsets++;
			msg->rep->rrset_count++;
		}
		lock_rw_unlock(&rrset->entry.lock);
	}
}

/** create referral message with NS and query */
static struct dns_msg*
create_msg(uint8_t* qname, size_t qnamelen, uint16_t qtype, uint16_t qclass, 
	struct region* region, struct ub_packed_rrset_key* nskey, 
	struct packed_rrset_data* nsdata, uint32_t now)
{
	struct dns_msg* msg = (struct dns_msg*)region_alloc(region,
		sizeof(struct dns_msg));
	if(!msg)
		return NULL;
	msg->qinfo.qname = region_alloc_init(region, qname, qnamelen);
	if(!msg->qinfo.qname)
		return NULL;
	msg->qinfo.qname_len = qnamelen;
	msg->qinfo.qtype = qtype;
	msg->qinfo.qclass = qclass;
	/* non-packed reply_info, because it needs to grow the array */
	msg->rep = (struct reply_info*)region_alloc(region, 
		sizeof(struct reply_info)-sizeof(struct rrset_ref));
	if(!msg->rep)
		return NULL;
	memset(msg->rep, 0, 
		sizeof(struct reply_info)-sizeof(struct rrset_ref));
	msg->rep->flags = BIT_QR; /* with QR, no AA */
	msg->rep->qdcount = 1;
	/* allocate the array to as much as we could need:
	 *	NS rrset + DS/NSEC rrset +
	 *	A rrset for every NS RR
	 *	AAAA rrset for every NS RR
	 */
	msg->rep->rrsets = (struct ub_packed_rrset_key**)region_alloc(region,
		(2 + nsdata->count*2)*sizeof(struct ub_packed_rrset_key*));
	if(!msg->rep->rrsets)
		return NULL;
	msg->rep->rrsets[0] = copy_rrset(nskey, region, now);
	if(!msg->rep->rrsets[0])
		return NULL;
	msg->rep->ns_numrrsets++;
	msg->rep->rrset_count++;
	return msg;
}

struct delegpt* 
dns_cache_find_delegation(struct module_env* env, uint8_t* qname, 
	size_t qnamelen, uint16_t qtype, uint16_t qclass, 
	struct region* region, struct dns_msg** msg)
{
	/* try to find closest NS rrset */
	struct ub_packed_rrset_key* nskey;
	struct packed_rrset_data* nsdata;
	struct delegpt* dp;
	uint32_t now = (uint32_t)time(NULL);

	nskey = find_deleg_ns(env, qname, qnamelen, qclass, now);
	if(!nskey) /* hope the caller has hints to prime or something */
		return NULL;
	nsdata = (struct packed_rrset_data*)nskey->entry.data;
	/* got the NS key, create delegation point */
	dp = delegpt_create(region);
	if(!dp || !delegpt_set_name(dp, region, nskey->rk.dname)) {
		lock_rw_unlock(&nskey->entry.lock);
		log_err("find_delegation: out of memory");
		return NULL;
	}
	/* create referral message */
	if(msg) {
		*msg = create_msg(qname, qnamelen, qtype, qclass, region, 
			nskey, nsdata, now);
		if(!*msg) {
			lock_rw_unlock(&nskey->entry.lock);
			log_err("find_delegation: out of memory");
			return NULL;
		}
	}
	if(!delegpt_rrset_add_ns(dp, region, nskey))
		log_err("find_delegation: addns out of memory");
	lock_rw_unlock(&nskey->entry.lock); /* first unlock before next lookup*/
	/* find and add DS/NSEC (if any) */
	if(msg)
		find_add_ds(env, region, *msg, dp, now);
	/* find and add A entries */
	if(!find_add_addrs(env, qclass, region, dp, now, msg))
		log_err("find_delegation: addrs out of memory");
	log_info("dns_cache_find_delegation returns delegpt");
	delegpt_log(dp);
	return dp;
}

/** allocate dns_msg from query_info and reply_info */
static struct dns_msg*
tomsg(struct module_env* env, struct msgreply_entry* e, struct reply_info* r, 
	struct region* region, uint32_t now, struct region* scratch)
{
	struct dns_msg* msg;
	size_t i;
	if(now > r->ttl)
		return NULL;
	msg = (struct dns_msg*)region_alloc(region, sizeof(struct dns_msg));
	if(!msg)
		return NULL;
	memcpy(&msg->qinfo, &e->key, sizeof(struct query_info));
	msg->qinfo.qname = region_alloc_init(region, e->key.qname, 
		e->key.qname_len);
	if(!msg->qinfo.qname)
		return NULL;
	/* allocate replyinfo struct and rrset key array separately */
	msg->rep = (struct reply_info*)region_alloc(region, 
		sizeof(struct reply_info) - sizeof(struct rrset_ref));
	if(!msg->rep)
		return NULL;
	memcpy(msg->rep, r, 
		sizeof(struct reply_info) - sizeof(struct rrset_ref));
	msg->rep->rrsets = (struct ub_packed_rrset_key**)region_alloc(region,
		msg->rep->rrset_count * sizeof(struct ub_packed_rrset_key*));
	if(!msg->rep->rrsets)
		return NULL;
	if(!rrset_array_lock(r->ref, r->rrset_count, now))
		return NULL;
	for(i=0; i<msg->rep->rrset_count; i++) {
		msg->rep->rrsets[i] = copy_rrset(r->rrsets[i], region, now);
		if(!msg->rep->rrsets[i]) {
			rrset_array_unlock(r->ref, r->rrset_count);
			return NULL;
		}
	}
	rrset_array_unlock_touch(env->rrset_cache, scratch, r->ref, 
		r->rrset_count);
	return msg;
}

struct dns_msg* 
dns_cache_lookup(struct module_env* env,
	uint8_t* qname, size_t qnamelen, uint16_t qtype, uint16_t qclass,
	struct region* region, struct region* scratch)
{
	struct lruhash_entry* e;
	struct query_info k;
	hashvalue_t h;
	uint32_t now = (uint32_t)time(NULL);

	/* lookup first, this has both NXdomains and ANSWER responses */
	k.qname = qname;
	k.qname_len = qnamelen;
	k.qtype = qtype;
	k.qclass = qclass;
	h = query_info_hash(&k);
	e = slabhash_lookup(env->msg_cache, h, &k, 0);
	if(e) {
		struct msgreply_entry* key = (struct msgreply_entry*)e->key;
		struct reply_info* data = (struct reply_info*)e->data;
		struct dns_msg* msg = tomsg(env, key, data, region, now, 
			scratch);
		if(msg) {
			lock_rw_unlock(&e->lock);
			return msg;
		}
		/* could be msg==NULL; due to TTL or not all rrsets available */
		lock_rw_unlock(&e->lock);
	}

	/* see if we have CNAME for this domain */
	k.qtype = LDNS_RR_TYPE_CNAME;
	h = query_info_hash(&k);
	e = slabhash_lookup(env->msg_cache, h, &k, 0);
	if(e) {
		struct msgreply_entry* key = (struct msgreply_entry*)e->key;
		struct reply_info* data = (struct reply_info*)e->data;
		struct dns_msg* msg = tomsg(env, key, data, region, now, 
			scratch);
		if(msg) {
			lock_rw_unlock(&e->lock);
			return msg;
		}
		/* could be msg==NULL; due to TTL or not all rrsets available */
		lock_rw_unlock(&e->lock);
	}

	/* construct DS, DNSKEY messages from rrset cache. TODO */

	return NULL;
}

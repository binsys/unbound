/*
 * libunbound/worker.h - worker thread or process that resolves
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
 * This file contains the worker process or thread that performs
 * the DNS resolving and validation. The worker is called by a procedure
 * and if in the background continues until exit, if in the foreground
 * returns from the procedure when done.
 */
#ifndef LIBUNBOUND_WORKER_H
#define LIBUNBOUND_WORKER_H
struct ub_val_ctx;
struct module_env;
struct comm_base;
struct outside_network;
struct ub_randstate;
struct ctx_query;

/** 
 * The library-worker status structure
 * Internal to the worker.
 */
struct libworker {
	/** every worker has a unique thread_num. (first in struct) */
	int thread_num;
	/** context we are operating under */
	struct ub_val_ctx* ctx;

	/** is this a background worker? */
	int is_bg;

	/** copy of the module environment with worker local entries. */
	struct module_env* env;
	/** the event base this worker works with */
	struct comm_base* base;
	/** the backside outside network interface to the auth servers */
	struct outside_network* back;

	/** random() table for this worker. */
	struct ub_randstate* rndstate;
	/** do we need to exit (when done) */
	int need_to_exit;
};

/**
 * Create a background worker
 * @param ctx: is updated with pid/tid of the background worker.
 *	a new allocation cache is obtained from ctx. It contains the
 *	threadnumber and unique id for further (shared) cache insertions.
 * @return 0 if OK, else error.
 *	Further communication is done via the pipes in ctx. 
 */
int libworker_bg(struct ub_val_ctx* ctx);

/**
 * Create a foreground worker.
 * This worker will join the threadpool of resolver threads.
 * It exits when the query answer has been obtained (or error).
 * This routine blocks until the worker is finished.
 * @param ctx: new allocation cache obtained and returned to it.
 * @param q: query (result is stored in here).
 * @return 0 if finished OK, else error.
 */
int libworker_fg(struct ub_val_ctx* ctx, struct ctx_query* q);

#endif /* LIBUNBOUND_WORKER_H */

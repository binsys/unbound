/*
 * util/netevent.h - event notification
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
 * This file contains event notification functions.
 *
 * There are three types of communication points
 *    o UDP socket - perthread buffer.
 *    o TCP-accept socket - array of TCP-sockets, socketcount.
 *    o TCP socket - own buffer, parent-TCPaccept, read/write state,
 *                   number of bytes read/written, timeout.
 *
 * There are sockets aimed towards our clients and towards the internet.
 *    o frontside - aimed towards our clients, queries come in, answers back.
 *    o behind - aimed towards internet, to the authoritative DNS servers.
 *
 * Several event types are available:
 *    o comm_base - for thread safety of the comm points, one per thread.
 *    o comm_point - udp and tcp networking, with callbacks.
 *    o comm_timer - a timeout with callback.
 *    o comm_signal - callbacks when signal is caught.
 *    o comm_reply - holds reply info during networking callback.
 *
 */

#ifndef NET_EVENT_H
#define NET_EVENT_H

#include "config.h"
struct comm_point;
struct comm_reply;

/* internal event notification data storage structure. */
struct internal_event;
struct internal_base;
struct internal_timer;

/** callback from communication point function type */
typedef int comm_point_callback_t(struct comm_point*, void*, int, 
	struct comm_reply*);

/** to pass no_error to callback function */
#define NETEVENT_NOERROR 0
/** to pass closed connection to callback function */
#define NETEVENT_CLOSED -1
/** to pass timeout happened to callback function */
#define NETEVENT_TIMEOUT -2 

/**
 * A communication point dispatcher. Thread specific.
 */
struct comm_base {
	/** behind the scenes structure. with say libevent info. alloced */
	struct internal_base* eb;
};

/** 
 * Communication point to the network 
 * These behaviours can be accomplished by setting the flags
 * and passing return values from the callback.
 *    udp frontside: called after readdone. sendafter.
 *    tcp frontside: called readdone, sendafter. close.
 *    udp behind: called after readdone. No send after.
 *    tcp behind: write done, read done, then called. No send after.
 */
struct comm_point {
	/** behind the scenes structure, with say libevent info. alloced. */
	struct internal_event* ev;

	/** file descriptor for communication point */
	int fd;

	/** timeout (NULL if it does not). Malloced. */
	struct timeval* timeout;

	/** buffer pointer. Either to perthread, or own buffer or NULL */
	ldns_buffer* buffer;

	/* -------- TCP Handler -------- */
	/** Read/Write state for TCP */
	int tcp_is_reading;
	/** The current read/write count for TCP */
	size_t tcp_byte_count;
	/** parent communication point (for TCP sockets) */
	struct comm_point* tcp_parent;

	/* -------- TCP Accept -------- */
	/** the number of TCP handlers for this tcp-accept socket */
	int max_tcp_count;
	/** malloced array of tcp handlers for a tcp-accept, 
	    of size max_tcp_count. */
	struct comm_point** tcp_handlers;
	/** linked list of free tcp_handlers to use for new queries.
	    For tcp_accept the first entry, for tcp_handlers the next one. */
	struct comm_point* tcp_free;

	/** is this a UDP, TCP-accept or TCP socket. */
	enum comm_point_type {
		/** UDP socket - handle datagrams. */
		comm_udp, 
		/** TCP accept socket - only creates handlers if readable. */
		comm_tcp_accept, 
		/** TCP handler socket - handle byteperbyte readwrite. */
		comm_tcp,
		/** AF_UNIX socket - for internal commands. */
		comm_local
	} type;

	/* ---------- Behaviour ----------- */
	/** if set the connection is NOT closed on delete. */
	int do_not_close;

	/** if set, the connection is closed on error, on timeout, 
	    and after read/write completes. No callback is done. */
	int tcp_do_close;

	/** if set, read/write completes:
		read/write state of tcp is toggled.
		buffer reset/bytecount reset.
		this flag cleared.
	    So that when that is done the callback is called. */
	int tcp_do_toggle_rw;

	/** if set, checks for pending error from nonblocking connect() call.*/
	int tcp_check_nb_connect;

	/** callback when done.
	    tcp_accept does not get called back, is NULL then.
	    If a timeout happens, callback with timeout=1 is called.
	    If an error happens, callback is called with error set 
	    nonzero. If not NETEVENT_NOERROR, it is an errno value.
	    If the connection is closed (by remote end) then the
	    callback is called with error set to NETEVENT_CLOSED=-1.
	    If a timeout happens on the connection, the error is set to 
	    NETEVENT_TIMEOUT=-2.
	    The reply_info can be copied if the reply needs to happen at a
	    later time. It consists of a struct with commpoint and address.
	    It can be passed to a msg send routine some time later.
	    Note the reply information is temporary and must be copied.
	    NULL is passed for_reply info, in cases where error happened.

	    declare as: 
	    int my_callback(struct comm_point* c, void* my_arg, int error,
		struct comm_reply *reply_info);

	    if the routine returns 0, nothing is done.
	    Notzero, the buffer will be sent back to client.
	    		For UDP this is done without changing the commpoint.
			In TCP it sets write state.
	*/
	comm_point_callback_t* callback;
	/** argument to pass to callback. */
	void *cb_arg;
};

/**
 * Reply information for a communication point.
 */
struct comm_reply {
	/** the comm_point with fd to send reply on to. */
	struct comm_point* c;
	/** the address (for UDP based communication) */
	struct sockaddr_storage addr;
	/** length of address */
	socklen_t addrlen;
};

/**
 * Structure only for making timeout events.
 */
struct comm_timer {
	/** the internal event stuff */
	struct internal_timer* ev_timer;

	/** callback function, takes user arg only */
	void (*callback)(void*);

	/** callback user argument */
	void* cb_arg;
};

/**
 * Structure only for signal events.
 */
struct comm_signal {
	/** the communication base */
	struct comm_base* base;

	/** the internal event stuff */
	struct internal_signal* ev_signal;

	/** callback function, takes signal number and user arg */
	void (*callback)(int, void*);

	/** callback user argument */
	void* cb_arg;
};

/**
 * Create a new comm base.
 * @return: the new comm base. NULL on error.
 */
struct comm_base* comm_base_create();

/**
 * Destroy a comm base.
 * All comm points must have been deleted.
 * @param b: the base to delete.
 */
void comm_base_delete(struct comm_base* b);

/**
 * Dispatch the comm base events.
 * @param b: the communication to perform.
 */
void comm_base_dispatch(struct comm_base* b);

/**
 * Exit from dispatch loop.
 * @param b: the communicatio base that is in dispatch().
 */
void comm_base_exit(struct comm_base* b);

/**
 * Create an UDP comm point. Calls malloc.
 * setups the structure with the parameters you provide.
 * @param base: in which base to alloc the commpoint.
 * @param fd : file descriptor of open UDP socket.
 * @param buffer: shared buffer by UDP sockets from this thread.
 * @param callback: callback function pointer.
 * @param callback_arg: will be passed to your callback function.
 * @return: returns the allocated communication point. NULL on error.
 * Sets timeout to NULL. Turns off TCP options.
 */
struct comm_point* comm_point_create_udp(struct comm_base* base,
	int fd, ldns_buffer* buffer, 
	comm_point_callback_t* callback, void* callback_arg);

/**
 * Create a TCP listener comm point. Calls malloc.
 * Setups the structure with the parameters you provide.
 * Also Creates TCP Handlers, pre allocated for you.
 * Uses the parameters you provide.
 * @param base: in which base to alloc the commpoint.
 * @param fd: file descriptor of open TCP socket set to listen nonblocking.
 * @param num: becomes max_tcp_count, the routine allocates that
 *	many tcp handler commpoints.
 * @param bufsize: size of buffer to create for handlers.
 * @param callback: callback function pointer for TCP handlers.
 * @param callback_arg: will be passed to your callback function.
 * @return: returns the TCP listener commpoint. You can find the
 *  	TCP handlers in the array inside the listener commpoint.
 *	returns NULL on error.
 * Inits timeout to NULL. All handlers are on the free list.
 */
struct comm_point* comm_point_create_tcp(struct comm_base* base,
	int fd, int num, size_t bufsize, 
	comm_point_callback_t* callback, void* callback_arg);

/**
 * Create an outgoing TCP commpoint. No file descriptor is opened, left at -1.
 * @param bufsize: size of buffer to create for handlers.
 * @param callback: callback function pointer for the handler.
 * @param callback_arg: will be passed to your callback function.
 * @return: the commpoint or NULL on error.
 */
struct comm_point* comm_point_create_tcp_out(size_t bufsize, 
	comm_point_callback_t* callback, void* callback_arg);

/**
 * Create commpoint to listen to a local domain file descriptor.
 * @param base: in which base to alloc the commpoint.
 * @param fd: file descriptor of open AF_UNIX socket set to listen nonblocking.
 * @param bufsize: size of buffer to create for handlers.
 * @param callback: callback function pointer for the handler.
 * @param callback_arg: will be passed to your callback function.
 * @return: the commpoint or NULL on error.
 */
struct comm_point* comm_point_create_local(struct comm_base* base,
	int fd, size_t bufsize, 
	comm_point_callback_t* callback, void* callback_arg);

/**
 * Close a comm point fd.
 * @param c: comm point to close.
 */
void comm_point_close(struct comm_point* c);

/**
 * Close and deallocate (free) the comm point. If the comm point is
 * a tcp-accept point, also its tcp-handler points are deleted.
 * @param c: comm point to delete.
 */
void comm_point_delete(struct comm_point* c);

/**
 * Reset the callback argument for a comm point.
 * @param c: the comm point to change.
 * @param arg: the new callback user argument.
 */
void comm_point_set_cb_arg(struct comm_point* c, void* arg);

/**
 * Send reply. Put message into commpoint buffer.
 * @param repinfo: The reply info copied from a commpoint callback call.
 */
void comm_point_send_reply(struct comm_reply* repinfo);

/**
 * Send reply. Message is not put into commpoint buffer, but in iovec.
 * If it cannot be sent immediately (TCP) the message is copied to the buffer.
 * @param repinfo: reply info copied from commpoint callback call.
 * @param iov: iovector, array of base, len parts to send out.
 *	caller must keep entry 0 free for use by tcp handler. Start at entry 1.
 * @param iovlen: number of iov items to concatenate and send out.
 *	this includes the entry 0, which is not filled in by caller.
 */
void comm_point_send_reply_iov(struct comm_reply* repinfo, struct iovec* iov,
	size_t iovlen);

/**
 * Drop reply. Cleans up.
 * @param repinfo: The reply info copied from a commpoint callback call.
 */
void comm_point_drop_reply(struct comm_reply* repinfo);

/**
 * Send an udp message over a commpoint.
 * @param c: commpoint to send it from.
 * @param packet: what to send.
 * @param addr: where to send it to.
 * @param addrlen: length of addr.
 * @return: false on a failure.
 */
int comm_point_send_udp_msg(struct comm_point* c, ldns_buffer* packet,
	struct sockaddr* addr, socklen_t addrlen);

/**
 * Stop listening for input on the commpoint. No callbacks will happen.
 * @param c: commpoint to disable. The fd is not closed.
 */
void comm_point_stop_listening(struct comm_point* c);

/**
 * Start listening again for input on the comm point.
 * @param c: commpoint to enable again.
 * @param newfd: new fd, or -1 to leave fd be.
 * @param sec: timeout in seconds, or -1 for no (change to the) timeout.
 */
void comm_point_start_listening(struct comm_point* c, int newfd, int sec);

/**
 * create timer. Not active upon creation.
 * @param base: event handling base.
 * @param cb: callback function: void myfunc(void* myarg);
 * @param cb_arg: user callback argument.
 * @return: the new timer or NULL on error.
 */
struct comm_timer* comm_timer_create(struct comm_base* base, 
	void (*cb)(void*), void* cb_arg);

/**
 * disable timer. Stops callbacks from happening.
 * @param timer: to disable.
 */
void comm_timer_disable(struct comm_timer* timer);

/**
 * reset timevalue for timer.
 * @param timer: timer to (re)set.
 * @param tv: when the timer should activate. if NULL timer is disabled.
 */
void comm_timer_set(struct comm_timer* timer, struct timeval* tv);

/**
 * delete timer.
 * @param timer: to delete.
 */
void comm_timer_delete(struct comm_timer* timer);

/**
 * see if timeout has been set to a value.
 * @param timer: the timer to examine.
 * @return: false if disabled or not set.
 */
int comm_timer_is_set(struct comm_timer* timer);

/**
 * Create a signal handler. Call signal_bind() later to bind to a signal.
 * @param base: communication base to use.
 * @param callback: called when signal is caught.
 * @param cb_arg: user argument to callback
 * @return: the signal struct or NULL on error.
 */
struct comm_signal* comm_signal_create(struct comm_base* base,
	void (*callback)(int, void*), void* cb_arg);

/**
 * Bind signal struct to catch a signal. A signle comm_signal can be bound
 * to multiple signals, calling comm_signal_bind multiple times.
 * @param comsig: the communication point, with callback information.
 * @param sig: signal number.
 * @return: true on success. false on error.
 */
int comm_signal_bind(struct comm_signal* comsig, int sig);

/**
 * Delete the signal communication point.
 * @param comsig: to delete.
 */
void comm_signal_delete(struct comm_signal* comsig);

/**
 * Prints the sockaddr in readable format with log_info. Debug helper.
 * @param addr: the sockaddr to print. Can be ip4 or ip6.
 * @param addrlen: length of addr.
 */
void log_addr(struct sockaddr_storage* addr, socklen_t addrlen);

#endif /* NET_EVENT_H */

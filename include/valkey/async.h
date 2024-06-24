/*
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2011, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef VALKEY_ASYNC_H
#define VALKEY_ASYNC_H
#include "valkey.h"

#ifdef __cplusplus
extern "C" {
#endif

/* For the async cluster attach functions. */
#if defined(__GNUC__) || defined(__clang__)
#define VALKEY_UNUSED __attribute__((unused))
#else
#define VALKEY_UNUSED
#endif

struct valkeyAsyncContext; /* need forward declaration of valkeyAsyncContext */
struct dict; /* dictionary header is included in async.c */

/* Reply callback prototype and container */
typedef void (valkeyCallbackFn)(struct valkeyAsyncContext*, void*, void*);
typedef struct valkeyCallback {
    struct valkeyCallback *next; /* simple singly linked list */
    valkeyCallbackFn *fn;
    int pending_subs;
    int unsubscribe_sent;
    void *privdata;
} valkeyCallback;

/* List of callbacks for either regular replies or pub/sub */
typedef struct valkeyCallbackList {
    valkeyCallback *head, *tail;
} valkeyCallbackList;

/* Connection callback prototypes */
typedef void (valkeyDisconnectCallback)(const struct valkeyAsyncContext*, int status);
typedef void (valkeyConnectCallback)(const struct valkeyAsyncContext*, int status);
typedef void (valkeyConnectCallbackNC)(struct valkeyAsyncContext *, int status);
typedef void(valkeyTimerCallback)(void *timer, void *privdata);

/* Context for an async connection to Valkey */
typedef struct valkeyAsyncContext {
    /* Hold the regular context, so it can be realloc'ed. */
    valkeyContext c;

    /* Setup error flags so they can be used directly. */
    int err;
    char *errstr;

    /* Not used by libvalkey */
    void *data;
    void (*dataCleanup)(void *privdata);

    /* Event library data and hooks */
    struct {
        void *data;

        /* Hooks that are called when the library expects to start
         * reading/writing. These functions should be idempotent. */
        void (*addRead)(void *privdata);
        void (*delRead)(void *privdata);
        void (*addWrite)(void *privdata);
        void (*delWrite)(void *privdata);
        void (*cleanup)(void *privdata);
        void (*scheduleTimer)(void *privdata, struct timeval tv);
    } ev;

    /* Called when either the connection is terminated due to an error or per
     * user request. The status is set accordingly (VALKEY_OK, VALKEY_ERR). */
    valkeyDisconnectCallback *onDisconnect;

    /* Called when the first write event was received. */
    valkeyConnectCallback *onConnect;
    valkeyConnectCallbackNC *onConnectNC;

    /* Regular command callbacks */
    valkeyCallbackList replies;

    /* Address used for connect() */
    struct sockaddr *saddr;
    size_t addrlen;

    /* Subscription callbacks */
    struct {
        valkeyCallbackList replies;
        struct dict *channels;
        struct dict *patterns;
        int pending_unsubs;
    } sub;

    /* Any configured RESP3 PUSH handler */
    valkeyAsyncPushFn *push_cb;
} valkeyAsyncContext;

/* Functions that proxy to libvalkey */
valkeyAsyncContext *valkeyAsyncConnectWithOptions(const valkeyOptions *options);
valkeyAsyncContext *valkeyAsyncConnect(const char *ip, int port);
valkeyAsyncContext *valkeyAsyncConnectBind(const char *ip, int port, const char *source_addr);
valkeyAsyncContext *valkeyAsyncConnectBindWithReuse(const char *ip, int port,
                                                  const char *source_addr);
valkeyAsyncContext *valkeyAsyncConnectUnix(const char *path);
int valkeyAsyncSetConnectCallback(valkeyAsyncContext *ac, valkeyConnectCallback *fn);
int valkeyAsyncSetConnectCallbackNC(valkeyAsyncContext *ac, valkeyConnectCallbackNC *fn);
int valkeyAsyncSetDisconnectCallback(valkeyAsyncContext *ac, valkeyDisconnectCallback *fn);

valkeyAsyncPushFn *valkeyAsyncSetPushCallback(valkeyAsyncContext *ac, valkeyAsyncPushFn *fn);
int valkeyAsyncSetTimeout(valkeyAsyncContext *ac, struct timeval tv);
void valkeyAsyncDisconnect(valkeyAsyncContext *ac);
void valkeyAsyncFree(valkeyAsyncContext *ac);

/* Handle read/write events */
void valkeyAsyncHandleRead(valkeyAsyncContext *ac);
void valkeyAsyncHandleWrite(valkeyAsyncContext *ac);
void valkeyAsyncHandleTimeout(valkeyAsyncContext *ac);
void valkeyAsyncRead(valkeyAsyncContext *ac);
void valkeyAsyncWrite(valkeyAsyncContext *ac);

/* Command functions for an async context. Write the command to the
 * output buffer and register the provided callback. */
int valkeyvAsyncCommand(valkeyAsyncContext *ac, valkeyCallbackFn *fn, void *privdata, const char *format, va_list ap);
int valkeyAsyncCommand(valkeyAsyncContext *ac, valkeyCallbackFn *fn, void *privdata, const char *format, ...);
int valkeyAsyncCommandArgv(valkeyAsyncContext *ac, valkeyCallbackFn *fn, void *privdata, int argc, const char **argv, const size_t *argvlen);
int valkeyAsyncFormattedCommand(valkeyAsyncContext *ac, valkeyCallbackFn *fn, void *privdata, const char *cmd, size_t len);

#ifdef __cplusplus
}
#endif

#endif

/*
 * Copyright (c) 2026, the libvalkey contributors
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
 *   * Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
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

#include "fmacros.h"
#include "win32.h"

#include "dns.h"

#include <stdio.h>
#include <string.h>

#ifdef VALKEY_USE_CARES
#include "alloc.h"

#include <ares.h>

#if ARES_VERSION < 0x011000
#error "c-ares >= 1.16.0 is required for ares_getaddrinfo"
#endif

#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>

/* Default DNS timeout when no connect_timeout is set (5 seconds). */
#define VALKEY_DNS_DEFAULT_TIMEOUT_MS 5000

static pthread_once_t cares_init_once = PTHREAD_ONCE_INIT;

static void valkeyCaresLibraryInit(void) {
    /* Use system malloc for c-ares rather than libvalkey's allocators.
     * c-ares leaks internally when custom allocators return NULL during OOM,
     * and its allocations are small and short-lived (freed per-resolve).
     * TODO: switch to ares_library_init_mem() if c-ares fixes OOM handling. */
    ares_library_init(ARES_LIB_INIT_NONE);
}

static long valkeyDnsPollMillis(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec * 1000) + now.tv_nsec / 1000000;
}

/* Callback state for synchronous ares_getaddrinfo. */
struct caresResult {
    int done;
    int status;
    struct ares_addrinfo *ai;
};

/* c-ares callback invoked when ares_getaddrinfo completes. */
static void caresCallback(void *arg, int status, int timeouts, struct ares_addrinfo *res) {
    struct caresResult *r = (struct caresResult *)arg;
    (void)timeouts;
    r->done = 1;
    r->status = status;
    r->ai = res;
}

/* State for tracking c-ares socket interest via ARES_OPT_SOCK_STATE_CB. */
struct caresSockState {
    struct pollfd pfds[ARES_GETSOCK_MAXNUM];
    int nfds;
};

/* c-ares socket state callback (ARES_OPT_SOCK_STATE_CB). Tracks which fds
 * c-ares needs polled so the sync poll loop knows what to watch. */
static void caresSockStateCb(void *data, ares_socket_t fd,
                             int readable, int writable) {
    struct caresSockState *st = (struct caresSockState *)data;
    int i;

    if (!readable && !writable) {
        /* Remove fd. */
        for (i = 0; i < st->nfds; i++) {
            if (st->pfds[i].fd == fd) {
                st->pfds[i] = st->pfds[st->nfds - 1];
                st->nfds--;
                break;
            }
        }
        return;
    }

    /* Find existing or add new. */
    for (i = 0; i < st->nfds; i++) {
        if (st->pfds[i].fd == fd)
            break;
    }
    if (i == st->nfds) {
        if (st->nfds >= ARES_GETSOCK_MAXNUM)
            return;
        st->nfds++;
    }
    st->pfds[i].fd = fd;
    st->pfds[i].events = 0;
    if (readable)
        st->pfds[i].events |= POLLIN;
    if (writable)
        st->pfds[i].events |= POLLOUT;
    st->pfds[i].revents = 0;
}

/* Convert ares_addrinfo to struct addrinfo. Caller must use valkeyFreeAddrInfo()
 * to free. Returns 0 on success, -1 on failure (OOM or empty result). */
static int caresAddrInfoToAddrInfo(struct ares_addrinfo *cai,
                                   struct addrinfo **out) {
    struct addrinfo *head = NULL, *tail = NULL;
    struct ares_addrinfo_node *node;

    for (node = cai->nodes; node != NULL; node = node->ai_next) {
        struct addrinfo *ai = vk_calloc(1, sizeof(*ai) + node->ai_addrlen);
        if (ai == NULL) {
            while (head) {
                struct addrinfo *next = head->ai_next;
                vk_free(head);
                head = next;
            }
            return -1;
        }
        ai->ai_family = node->ai_family;
        ai->ai_socktype = node->ai_socktype;
        ai->ai_protocol = node->ai_protocol;
        ai->ai_addrlen = node->ai_addrlen;
        ai->ai_addr = (struct sockaddr *)((char *)ai + sizeof(*ai));
        memcpy(ai->ai_addr, node->ai_addr, node->ai_addrlen);
        ai->ai_next = NULL;

        if (tail)
            tail->ai_next = ai;
        else
            head = ai;
        tail = ai;
    }
    if (head == NULL)
        return -1;
    *out = head;
    return 0;
}

void valkeyFreeAddrInfo(struct addrinfo *ai) {
    while (ai) {
        struct addrinfo *next = ai->ai_next;
        vk_free(ai);
        ai = next;
    }
}

/* Map c-ares status to getaddrinfo error codes for consistent error reporting. */
static int caresStatusToEai(int status) {
    switch (status) {
    case ARES_ENOTFOUND:
    case ARES_ENODATA:
        return EAI_NONAME;
    case ARES_ETIMEOUT:
    case ARES_ECANCELLED:
        return EAI_AGAIN;
    case ARES_ENOMEM:
        return EAI_MEMORY;
    default:
        return EAI_FAIL;
    }
}

/* Drive c-ares poll loop until res->done or deadline exceeded. */
static void caresPollLoop(ares_channel_t *channel, struct caresSockState *st,
                          struct caresResult *res, long deadline) {
    while (!res->done) {
        if (st->nfds == 0)
            break;

        long now = valkeyDnsPollMillis();
        long remaining = deadline - now;
        if (remaining <= 0) {
            ares_cancel(channel);
            break;
        }

        struct timeval maxtv, tv;
        maxtv.tv_sec = remaining / 1000;
        maxtv.tv_usec = (remaining % 1000) * 1000;
        struct timeval *tvp = ares_timeout(channel, &maxtv, &tv);
        long lval_ms = tvp->tv_sec * 1000 + tvp->tv_usec / 1000;
        if (lval_ms <= 0)
            lval_ms = 1;
        else if (lval_ms > INT_MAX)
            lval_ms = INT_MAX;
        int poll_ms = (int)lval_ms;

        int ret = poll(st->pfds, (nfds_t)st->nfds, poll_ms);
        if (ret > 0) {
            for (int i = 0; i < st->nfds; i++) {
                ares_socket_t rfd = (st->pfds[i].revents & (POLLIN | POLLERR | POLLHUP)) ? st->pfds[i].fd : ARES_SOCKET_BAD;
                ares_socket_t wfd = (st->pfds[i].revents & POLLOUT) ? st->pfds[i].fd : ARES_SOCKET_BAD;
                ares_process_fd(channel, rfd, wfd);
            }
        } else {
            ares_process_fd(channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
        }
    }
}

/* Resolve hostname using c-ares with a poll loop bounded by timeout_ms.
 * Returns 0 on success (result set), or a getaddrinfo-compatible error code. */
static int valkeyResolveCares(const char *host, int port, int flags,
                              long timeout_ms, struct addrinfo **result) {
    ares_channel_t *channel = NULL;
    struct ares_options opts;
    struct ares_addrinfo_hints hints;
    struct caresResult res = {0, 0, NULL};
    struct caresSockState sockstate = {{{0}}, 0};
    int optmask;
    int rv;
    long effective_timeout = timeout_ms;
    if (effective_timeout <= 0 || effective_timeout >= INT_MAX)
        effective_timeout = VALKEY_DNS_DEFAULT_TIMEOUT_MS;

    pthread_once(&cares_init_once, valkeyCaresLibraryInit);

    memset(&opts, 0, sizeof(opts));
    opts.timeout = (int)effective_timeout;
    opts.tries = 2;
    opts.sock_state_cb = caresSockStateCb;
    opts.sock_state_cb_data = &sockstate;
    optmask = ARES_OPT_TIMEOUTMS | ARES_OPT_TRIES | ARES_OPT_SOCK_STATE_CB;

    rv = ares_init_options(&channel, &opts, optmask);
    if (rv != ARES_SUCCESS)
        return (rv == ARES_ENOMEM) ? EAI_MEMORY : EAI_FAIL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;

    if ((flags & VALKEY_PREFER_IPV6) && (flags & VALKEY_PREFER_IPV4))
        hints.ai_family = AF_UNSPEC;
    else if (flags & VALKEY_PREFER_IPV6)
        hints.ai_family = AF_INET6;
    else if (strchr(host, ':') != NULL)
        hints.ai_family = AF_INET6; /* IPv6 literal */
    else
        hints.ai_family = AF_INET;

    char portstr[6];
    snprintf(portstr, sizeof(portstr), "%d", port);

    ares_getaddrinfo(channel, host, portstr, &hints, caresCallback, &res);

    long deadline = valkeyDnsPollMillis() + effective_timeout;
    caresPollLoop(channel, &sockstate, &res, deadline);

    rv = EAI_FAIL;
    if (res.done && res.status == ARES_SUCCESS && res.ai) {
        if (caresAddrInfoToAddrInfo(res.ai, result) == 0)
            rv = 0;
        else
            rv = EAI_MEMORY;
    } else if (res.done && (res.status == ARES_ENOTFOUND || res.status == ARES_ENODATA) &&
               hints.ai_family != AF_UNSPEC) {
        /* ENOTFOUND: domain doesn't exist (NXDOMAIN).
         * ENODATA: domain exists but has no records for the requested family.
         * In both cases, retry with the other address family. */
        if (res.ai)
            ares_freeaddrinfo(res.ai);
        res.ai = NULL;

        hints.ai_family = (hints.ai_family == AF_INET) ? AF_INET6 : AF_INET;
        res.done = 0;
        res.status = 0;

        ares_getaddrinfo(channel, host, portstr, &hints, caresCallback, &res);
        deadline = valkeyDnsPollMillis() + effective_timeout;
        caresPollLoop(channel, &sockstate, &res, deadline);

        if (res.done && res.status == ARES_SUCCESS && res.ai) {
            if (caresAddrInfoToAddrInfo(res.ai, result) == 0)
                rv = 0;
            else
                rv = EAI_MEMORY;
        } else {
            /* Retry also failed. */
            rv = caresStatusToEai(res.status);
        }
    } else {
        /* First attempt failed with non-retryable error. */
        rv = caresStatusToEai(res.status);
    }

    if (res.ai)
        ares_freeaddrinfo(res.ai);
    ares_destroy(channel);
    return rv;
}
#endif /* VALKEY_USE_CARES */

int valkeyResolveSync(const char *host, int port, int flags,
                      long timeout_ms, struct addrinfo **result) {
#ifdef VALKEY_USE_CARES
    return valkeyResolveCares(host, port, flags, timeout_ms, result);
#else
    (void)timeout_ms;
    char portstr[6]; /* strlen("65535") + 1 */
    struct addrinfo hints;
    int rv;

    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;

    /* Determine address family from flags. By default, try IPv4 first and
     * fall back to IPv6. If both PREFER flags are set, use AF_UNSPEC. */
    if ((flags & VALKEY_PREFER_IPV6) && (flags & VALKEY_PREFER_IPV4))
        hints.ai_family = AF_UNSPEC;
    else if (flags & VALKEY_PREFER_IPV6)
        hints.ai_family = AF_INET6;
    else
        hints.ai_family = AF_INET;

    rv = getaddrinfo(host, portstr, &hints, result);
    if (rv != 0 && hints.ai_family != AF_UNSPEC) {
        /* Try again with the other IP version. */
        hints.ai_family = (hints.ai_family == AF_INET) ? AF_INET6 : AF_INET;
        rv = getaddrinfo(host, portstr, &hints, result);
    }
    return rv;
#endif
}

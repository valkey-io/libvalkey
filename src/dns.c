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
#include "async.h"
#include "async_private.h"
#include "net.h"
#include "valkey_private.h"

#include <ares.h>

#if ARES_VERSION < 0x011000
#error "c-ares >= 1.16.0 is required for ares_getaddrinfo"
#endif

#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
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

static uint64_t valkeyDnsPollMillis(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((uint64_t)now.tv_sec * 1000) + now.tv_nsec / 1000000;
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

/* Drive c-ares poll loop until res->done or timeout_ms has elapsed since
 * start.  timeout_ms is in (0, INT_MAX). */
static void caresPollLoop(ares_channel_t *channel, struct caresSockState *st,
                          struct caresResult *res, uint64_t start, long timeout_ms) {
    while (!res->done) {
        if (st->nfds == 0)
            break;

        uint64_t elapsed = valkeyDnsPollMillis() - start;
        if (elapsed >= (uint64_t)timeout_ms) {
            ares_cancel(channel);
            break;
        }
        long remaining = timeout_ms - (long)elapsed;

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

/* Determine address family from context flags and hostname. */
static int caresHintsFamily(const char *host, int flags) {
    if ((flags & VALKEY_PREFER_IPV6) && (flags & VALKEY_PREFER_IPV4))
        return AF_UNSPEC;
    if (flags & VALKEY_PREFER_IPV6)
        return AF_INET6;
    if (strchr(host, ':') != NULL)
        return AF_INET6; /* IPv6 literal */
    return AF_INET;
}

/* True when a failed lookup may succeed for the other address family.
 * ENOTFOUND is NXDOMAIN, ENODATA means no records of the requested family. */
static int caresShouldRetryOtherFamily(const struct caresResult *res, int ai_family) {
    if (ai_family == AF_UNSPEC || !res->done)
        return 0;

    return res->status == ARES_ENOTFOUND || res->status == ARES_ENODATA;
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
    hints.ai_family = caresHintsFamily(host, flags);

    char portstr[6];
    snprintf(portstr, sizeof(portstr), "%d", port);

    ares_getaddrinfo(channel, host, portstr, &hints, caresCallback, &res);
    caresPollLoop(channel, &sockstate, &res, valkeyDnsPollMillis(), effective_timeout);

    if (caresShouldRetryOtherFamily(&res, hints.ai_family)) {
        if (res.ai)
            ares_freeaddrinfo(res.ai);
        res.ai = NULL;
        res.done = 0;
        res.status = 0;

        hints.ai_family = (hints.ai_family == AF_INET) ? AF_INET6 : AF_INET;

        ares_getaddrinfo(channel, host, portstr, &hints, caresCallback, &res);
        caresPollLoop(channel, &sockstate, &res, valkeyDnsPollMillis(), effective_timeout);
    }

    /* A query that never completed leaves status at its initial ARES_SUCCESS,
     * which caresStatusToEai() maps to EAI_FAIL. */
    if (!res.done || res.status != ARES_SUCCESS || res.ai == NULL)
        rv = caresStatusToEai(res.status);
    else
        rv = caresAddrInfoToAddrInfo(res.ai, result) == 0 ? 0 : EAI_MEMORY;

    if (res.ai)
        ares_freeaddrinfo(res.ai);
    ares_destroy(channel);
    return rv;
}

/* --- Async DNS resolution --- */

typedef struct valkeyAsyncDns {
    ares_channel_t *channel;
    struct valkeyAsyncContext *ac;
    struct valkeyTimer *timer;
    char *host;
    int port;
    int ai_family;
    int retried_family; /* The other address family has been tried. */
    int done;
    int failed;
} valkeyAsyncDns;

static void caresAsyncSockStateCb(void *data, ares_socket_t fd, int readable, int writable) {
    valkeyAsyncDns *dns = (valkeyAsyncDns *)data;
    valkeyAsyncContext *ac = dns->ac;

    if (!readable && !writable) {
        ac->ev.delCaresSocket(ac->ev.data, (int)fd);
    } else {
        ac->ev.addCaresSocket(ac->ev.data, (int)fd, readable, writable);
    }
}

static void caresAsyncTimerCb(void *privdata);

static void caresAsyncScheduleTimer(valkeyAsyncDns *dns) {
    valkeyAsyncContext *ac = dns->ac;
    struct timeval tv, maxtv;

    maxtv.tv_sec = 1;
    maxtv.tv_usec = 0;
    struct timeval *tvp = ares_timeout(dns->channel, &maxtv, &tv);

    /* Cancel previous timer if active. */
    if (dns->timer) {
        valkeyTimerDel(ac->timer_list, dns->timer);
        dns->timer = NULL;
    }
    dns->timer = valkeyAsyncAddTimer(ac, *tvp, caresAsyncTimerCb, dns);
}

/* Forward declaration. */
static void caresAsyncCallback(void *arg, int status, int timeouts, struct ares_addrinfo *res);

/* Mark DNS resolution as complete and record failure. */
static void caresAsyncFail(valkeyAsyncDns *dns) {
    valkeyAsyncContext *ac = dns->ac;
    valkeyAsyncCopyError(ac);
    dns->done = 1;
    dns->failed = 1;
}

static void caresAsyncConnectWithResult(valkeyAsyncDns *dns, struct ares_addrinfo *ai) {
    valkeyAsyncContext *ac = dns->ac;
    valkeyContext *c = &ac->c;
    struct addrinfo *servinfo = NULL;

    if (caresAddrInfoToAddrInfo(ai, &servinfo) != 0) {
        valkeySetError(c, VALKEY_ERR_OOM, "Out of memory");
        ares_freeaddrinfo(ai);
        caresAsyncFail(dns);
        return;
    }
    ares_freeaddrinfo(ai);

    if (valkeyTcpConnectNonBlock(c, servinfo) != VALKEY_OK) {
        valkeyFreeAddrInfo(servinfo);
        caresAsyncFail(dns);
        return;
    }

    c->flags &= ~VALKEY_CONNECT_DEFERRED;
    valkeyFreeAddrInfo(servinfo);
    dns->done = 1;
    _EL_ADD_WRITE(ac);
}

static void caresAsyncCallback(void *arg, int status, int timeouts, struct ares_addrinfo *res) {
    (void)timeouts;
    valkeyAsyncDns *dns = (valkeyAsyncDns *)arg;
    valkeyAsyncContext *ac = dns->ac;
    valkeyContext *c = &ac->c;

    /* Channel is being destroyed (e.g. during valkeyAsyncFree). */
    if (status == ARES_EDESTRUCTION) {
        if (res)
            ares_freeaddrinfo(res);
        return;
    }

    if (status == ARES_SUCCESS && res) {
        caresAsyncConnectWithResult(dns, res);
        return;
    }

    /* Not found for the preferred family, so try the other one once. */
    if ((status == ARES_ENOTFOUND || status == ARES_ENODATA) &&
        dns->ai_family != AF_UNSPEC && !dns->retried_family) {
        if (res)
            ares_freeaddrinfo(res);

        dns->retried_family = 1;
        dns->ai_family = (dns->ai_family == AF_INET) ? AF_INET6 : AF_INET;
        struct ares_addrinfo_hints hints = {0};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = dns->ai_family;

        char portstr[6];
        snprintf(portstr, sizeof(portstr), "%d", dns->port);
        ares_getaddrinfo(dns->channel, dns->host, portstr, &hints, caresAsyncCallback, dns);
        caresAsyncScheduleTimer(dns);
        return;
    }

    /* DNS failed. */
    if (res)
        ares_freeaddrinfo(res);

    int eai = caresStatusToEai(status);
    if (eai == EAI_MEMORY)
        valkeySetError(c, VALKEY_ERR_OOM, "Out of memory");
    else
        valkeySetError(c, VALKEY_ERR_OTHER, gai_strerror(eai));
    caresAsyncFail(dns);
}

int valkeyResolveAsyncStart(struct valkeyAsyncContext *ac, const char *host, int port) {
    valkeyContext *c = &ac->c;
    valkeyAsyncDns *dns;

    /* Async DNS needs the adapter to drive the c-ares sockets. Adapters without
     * these hooks are expected to fall back to a synchronous resolve. */
    if (ac->ev.addCaresSocket == NULL || ac->ev.delCaresSocket == NULL)
        return VALKEY_ERR;

    pthread_once(&cares_init_once, valkeyCaresLibraryInit);

    dns = vk_calloc(1, sizeof(*dns));
    if (dns == NULL)
        return VALKEY_ERR;

    dns->ac = ac;
    dns->host = vk_strdup(host);
    if (dns->host == NULL) {
        vk_free(dns);
        return VALKEY_ERR;
    }
    dns->port = port;
    dns->ai_family = caresHintsFamily(host, c->flags);

    struct ares_options opts = {0};
    opts.sock_state_cb = caresAsyncSockStateCb;
    opts.sock_state_cb_data = dns;
    int optmask = ARES_OPT_SOCK_STATE_CB;

    int rv = ares_init_options(&dns->channel, &opts, optmask);
    if (rv != ARES_SUCCESS) {
        vk_free(dns->host);
        vk_free(dns);
        return VALKEY_ERR;
    }

    /* Store dns state in the context for later access. */
    ac->dns_state = dns;

    struct ares_addrinfo_hints hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = dns->ai_family;

    char portstr[6];
    snprintf(portstr, sizeof(portstr), "%d", port);

    ares_getaddrinfo(dns->channel, host, portstr, &hints, caresAsyncCallback, dns);

    /* If c-ares resolved synchronously (e.g. IP literals), the callback
     * has already fired and set dns->done. Clean up now. */
    if (dns->done) {
        int failed = dns->failed;
        valkeyResolveAsyncFree(ac);
        if (failed)
            return VALKEY_ERR;
    } else {
        caresAsyncScheduleTimer(dns);
    }

    return VALKEY_OK;
}

void valkeyResolveAsyncHandleEvent(struct valkeyAsyncContext *ac, int fd, int readable, int writable) {
    valkeyAsyncDns *dns = (valkeyAsyncDns *)ac->dns_state;
    if (dns == NULL || dns->channel == NULL)
        return;
    ares_socket_t rfd = readable ? (ares_socket_t)fd : ARES_SOCKET_BAD;
    ares_socket_t wfd = writable ? (ares_socket_t)fd : ARES_SOCKET_BAD;
    ares_process_fd(dns->channel, rfd, wfd);
    if (dns->done) {
        int failed = dns->failed;
        valkeyResolveAsyncFree(ac);
        if (failed)
            valkeyAsyncHandleConnectFailure(ac);
    } else {
        caresAsyncScheduleTimer(dns);
    }
}

static void caresAsyncTimerCb(void *privdata) {
    valkeyAsyncDns *dns = (valkeyAsyncDns *)privdata;
    valkeyAsyncContext *ac = dns->ac;
    dns->timer = NULL; /* one-shot, already removed */
    if (dns->channel == NULL)
        return;
    ares_process_fd(dns->channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
    if (dns->done) {
        int failed = dns->failed;
        valkeyResolveAsyncFree(ac);
        if (failed)
            valkeyAsyncHandleConnectFailure(ac);
    } else
        caresAsyncScheduleTimer(dns);
}

void valkeyResolveAsyncFree(struct valkeyAsyncContext *ac) {
    valkeyAsyncDns *dns = (valkeyAsyncDns *)ac->dns_state;
    if (dns == NULL)
        return;
    if (dns->timer) {
        valkeyTimerDel(ac->timer_list, dns->timer);
        dns->timer = NULL;
    }
    if (dns->channel) {
        ares_destroy(dns->channel);
        dns->channel = NULL;
    }
    vk_free(dns->host);
    vk_free(dns);
    ac->dns_state = NULL;
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

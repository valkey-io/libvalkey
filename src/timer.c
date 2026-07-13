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

#include "timer.h"

#include <string.h>
#ifndef _MSC_VER
#include <time.h>
#else
#include <stdint.h>
#include <windows.h>
#endif

static void valkeyTimerGetMonotonic(struct timeval *tv) {
#ifndef _MSC_VER
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = (int)(ts.tv_nsec / 1000);
#else
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    int64_t usec = counter.QuadPart * 1000000 / frequency.QuadPart;
    tv->tv_sec = (long)(usec / 1000000);
    tv->tv_usec = (long)(usec % 1000000);
#endif
}

static long long tvdiff_us(const struct timeval *a, const struct timeval *b) {
    return (long long)(a->tv_sec - b->tv_sec) * 1000000 +
           (a->tv_usec - b->tv_usec);
}

static void tvadd(struct timeval *result, const struct timeval *a, const struct timeval *b) {
    result->tv_sec = a->tv_sec + b->tv_sec;
    result->tv_usec = a->tv_usec + b->tv_usec;
    if (result->tv_usec >= 1000000) {
        result->tv_sec++;
        result->tv_usec -= 1000000;
    }
}

/* Insert timer into sorted active list (earliest deadline first). */
static void timerInsert(valkeyTimerList *list, valkeyTimer *timer) {
    valkeyTimer **pp = &list->head;
    while (*pp && tvdiff_us(&(*pp)->deadline, &timer->deadline) <= 0)
        pp = &(*pp)->next;
    timer->next = *pp;
    *pp = timer;
}

void valkeyTimerListInit(valkeyTimerList *list) {
    memset(list, 0, sizeof(*list));
}

valkeyTimer *valkeyTimerAdd(valkeyTimerList *list, struct timeval timeout,
                            valkeyTimerProc proc, void *privdata) {
    /* Find a free slot. */
    valkeyTimer *t = NULL;
    for (int i = 0; i < VALKEY_MAX_TIMERS; i++) {
        if (list->timers[i].proc == NULL) {
            t = &list->timers[i];
            break;
        }
    }
    if (t == NULL || proc == NULL)
        return NULL;

    struct timeval now;
    valkeyTimerGetMonotonic(&now);
    tvadd(&t->deadline, &now, &timeout);
    t->proc = proc;
    t->privdata = privdata;
    t->next = NULL;

    timerInsert(list, t);
    return t;
}

void valkeyTimerDel(valkeyTimerList *list, valkeyTimer *timer) {
    if (timer == NULL || timer->proc == NULL)
        return;

    /* Remove from active list. */
    valkeyTimer **pp = &list->head;
    while (*pp) {
        if (*pp == timer) {
            *pp = timer->next;
            break;
        }
        pp = &(*pp)->next;
    }

    /* Mark slot as free. */
    timer->proc = NULL;
    timer->next = NULL;
}

struct timeval *valkeyProcessTimers(valkeyTimerList *list, struct timeval *remaining) {
    struct timeval now;
    valkeyTimerGetMonotonic(&now);

    /* Process at most one expired timer per call. The callback may free
     * the context (and this list), so we must not access list after. */
    if (list->head && tvdiff_us(&now, &list->head->deadline) >= 0) {
        valkeyTimer *t = list->head;
        list->head = t->next;
        t->next = NULL;

        valkeyTimerProc proc = t->proc;
        void *privdata = t->privdata;

        t->proc = NULL;

        proc(privdata);
        /* Context may be freed here, caller must not access list. */
        return NULL;
    }

    if (list->head == NULL)
        return NULL;

    long long diff_us = tvdiff_us(&list->head->deadline, &now);
    remaining->tv_sec = (long)(diff_us / 1000000);
    remaining->tv_usec = (long)(diff_us % 1000000);
    return remaining;
}

void valkeyTimerListFree(valkeyTimerList *list) {
    for (int i = 0; i < VALKEY_MAX_TIMERS; i++)
        list->timers[i].proc = NULL;
    list->head = NULL;
}

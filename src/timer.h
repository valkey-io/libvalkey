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

#ifndef VALKEY_TIMER_H
#define VALKEY_TIMER_H

#ifndef _MSC_VER
#include <sys/time.h>
#else
#include <stdint.h>
#include <winsock2.h>
#endif

#define VALKEY_MAX_TIMERS 4

typedef void (*valkeyTimerProc)(void *privdata);

typedef struct valkeyTimer {
    struct timeval deadline;
    valkeyTimerProc proc; /* NULL = slot is free */
    void *privdata;
    struct valkeyTimer *next; /* sorted active list link */
} valkeyTimer;

typedef struct valkeyTimerList {
    valkeyTimer timers[VALKEY_MAX_TIMERS];
    valkeyTimer *head;
} valkeyTimerList;

/* Initialize a timer list (all slots free). */
void valkeyTimerListInit(valkeyTimerList *list);

/* Activate a timer. Returns handle or NULL if pool exhausted. */
valkeyTimer *valkeyTimerAdd(valkeyTimerList *list, struct timeval timeout,
                            valkeyTimerProc proc, void *privdata);

/* Deactivate a timer. */
void valkeyTimerDel(valkeyTimerList *list, valkeyTimer *timer);

/* Process expired timers. Returns time until next deadline, or NULL if none. */
struct timeval *valkeyProcessTimers(valkeyTimerList *list, struct timeval *remaining);

/* Deactivate all timers. */
void valkeyTimerListFree(valkeyTimerList *list);

#endif /* VALKEY_TIMER_H */

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

#ifndef VALKEY_DNS_H
#define VALKEY_DNS_H

#include "sockcompat.h"
#include "valkey.h"

/* Resolve hostname synchronously. Returns 0 on success.
 * On failure returns a getaddrinfo error code; the caller can use
 * gai_strerror() to get the error message.
 * The caller must free the result with valkeyFreeAddrInfo().
 * flags: context flags (VALKEY_PREFER_IPV4, VALKEY_PREFER_IPV6).
 * timeout_ms: DNS resolution timeout in milliseconds (used with c-ares). */
int valkeyResolveSync(const char *host, int port, int flags,
                      long timeout_ms, struct addrinfo **result);

/* Free addrinfo returned by valkeyResolveSync. Safe to call on either
 * freeaddrinfo-compatible or c-ares-allocated results. */
#ifdef VALKEY_USE_CARES
void valkeyFreeAddrInfo(struct addrinfo *ai);
#else
#define valkeyFreeAddrInfo(ai) freeaddrinfo(ai)
#endif

#endif /* VALKEY_DNS_H */

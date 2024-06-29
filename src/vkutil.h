/*
 * Copyright (c) 2015-2017, Ieshen Zheng <ieshen.zheng at 163 dot com>
 * Copyright (c) 2020, Nick <heronr1 at gmail dot com>
 * Copyright (c) 2020-2021, Bjorn Svensson <bjorn.a.svensson at est dot tech>
 * Copyright (c) 2020-2021, Viktor Söderqvist <viktor.soderqvist at est dot tech>
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

#ifndef __VKUTIL_H_
#define __VKUTIL_H_

#include <stdint.h>
#include <sys/types.h>

#define VK_OK 0

typedef int rstatus_t; /* return type */

/*
 * Wrapper to workaround well known, safe, implicit type conversion when
 * invoking system calls.
 */
#define vk_atoi(_line, _n) _vk_atoi((uint8_t *)_line, (size_t)_n)

int _vk_atoi(uint8_t *line, size_t n);

int vk_valid_port(int n);

/*
 * Wrappers for defining custom assert based on whether macro
 * VK_ASSERT_PANIC or VK_ASSERT_LOG was defined at the moment
 * ASSERT was called.
 */
#ifdef VK_ASSERT_PANIC

#define ASSERT(_x)                                                             \
    do {                                                                       \
        if (!(_x)) {                                                           \
            vk_assert(#_x, __FILE__, __LINE__, 1);                             \
        }                                                                      \
    } while (0)

#elif VK_ASSERT_LOG

#define ASSERT(_x)                                                             \
    do {                                                                       \
        if (!(_x)) {                                                           \
            vk_assert(#_x, __FILE__, __LINE__, 0);                             \
        }                                                                      \
    } while (0)

#else

#define ASSERT(_x)

#endif

void vk_assert(const char *cond, const char *file, int line, int panic);
void vk_stacktrace(int skip_count);

int64_t vk_usec_now(void);

uint16_t crc16(const char *buf, int len);

#endif

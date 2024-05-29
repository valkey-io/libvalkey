/*
 * Copyright (c) 2015-2017, Ieshen Zheng <ieshen.zheng at 163 dot com>
 * Copyright (c) 2020-2021, Bjorn Svensson <bjorn.a.svensson at est dot tech>
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

#ifndef __VKARRAY_H_
#define __VKARRAY_H_

#include <stdint.h>

typedef int (*vkarray_compare_t)(const void *, const void *);
typedef int (*vkarray_each_t)(void *, void *);

struct vkarray {
    uint32_t nelem;  /* # element */
    void *elem;      /* element */
    size_t size;     /* element size */
    uint32_t nalloc; /* # allocated element */
};

#define null_vkarray                                                           \
    { 0, NULL, 0, 0 }

static inline void vkarray_null(struct vkarray *a) {
    a->nelem = 0;
    a->elem = NULL;
    a->size = 0;
    a->nalloc = 0;
}

static inline void vkarray_set(struct vkarray *a, void *elem, size_t size,
                               uint32_t nalloc) {
    a->nelem = 0;
    a->elem = elem;
    a->size = size;
    a->nalloc = nalloc;
}

static inline uint32_t vkarray_n(const struct vkarray *a) { return a->nelem; }

struct vkarray *vkarray_create(uint32_t n, size_t size);
void vkarray_destroy(struct vkarray *a);
void vkarray_deinit(struct vkarray *a);

uint32_t vkarray_idx(struct vkarray *a, void *elem);
void *vkarray_push(struct vkarray *a);
void *vkarray_pop(struct vkarray *a);
void *vkarray_get(struct vkarray *a, uint32_t idx);
void *vkarray_top(struct vkarray *a);
void vkarray_swap(struct vkarray *a, struct vkarray *b);
void vkarray_sort(struct vkarray *a, vkarray_compare_t compare);
int vkarray_each(struct vkarray *a, vkarray_each_t func, void *data);

#endif

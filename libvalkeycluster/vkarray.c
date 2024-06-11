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
#include <stdlib.h>
#include <valkey/alloc.h>

#include "vkarray.h"
#include "vkutil.h"

struct vkarray *vkarray_create(uint32_t n, size_t size) {
    struct vkarray *a;

    ASSERT(n != 0 && size != 0);

    a = vk_malloc(sizeof(*a));
    if (a == NULL) {
        return NULL;
    }

    a->elem = vk_malloc(n * size);
    if (a->elem == NULL) {
        vk_free(a);
        return NULL;
    }

    a->nelem = 0;
    a->size = size;
    a->nalloc = n;

    return a;
}

void vkarray_destroy(struct vkarray *a) {
    vkarray_deinit(a);
    vk_free(a);
}

void vkarray_deinit(struct vkarray *a) {
    ASSERT(a->nelem == 0);

    vk_free(a->elem);
    a->elem = NULL;
}

uint32_t vkarray_idx(struct vkarray *a, void *elem) {
    uint8_t *p, *q;
    uint32_t off, idx;

    ASSERT(elem >= a->elem);

    p = a->elem;
    q = elem;
    off = (uint32_t)(q - p);

    ASSERT(off % (uint32_t)a->size == 0);

    idx = off / (uint32_t)a->size;

    return idx;
}

void *vkarray_push(struct vkarray *a) {
    void *elem, *new;
    size_t size;

    if (a->nelem == a->nalloc) {

        /* the array is full; allocate new array */
        size = a->size * a->nalloc;
        new = vk_realloc(a->elem, 2 * size);
        if (new == NULL) {
            return NULL;
        }

        a->elem = new;
        a->nalloc *= 2;
    }

    elem = (uint8_t *)a->elem + a->size * a->nelem;
    a->nelem++;

    return elem;
}

void *vkarray_pop(struct vkarray *a) {
    void *elem;

    ASSERT(a->nelem != 0);

    a->nelem--;
    elem = (uint8_t *)a->elem + a->size * a->nelem;

    return elem;
}

void *vkarray_get(struct vkarray *a, uint32_t idx) {
    void *elem;

    ASSERT(a->nelem != 0);
    ASSERT(idx < a->nelem);

    elem = (uint8_t *)a->elem + (a->size * idx);

    return elem;
}

void *vkarray_top(struct vkarray *a) {
    ASSERT(a->nelem != 0);

    return vkarray_get(a, a->nelem - 1);
}

void vkarray_swap(struct vkarray *a, struct vkarray *b) {
    struct vkarray tmp;

    tmp = *a;
    *a = *b;
    *b = tmp;
}

/*
 * Sort nelem elements of the array in ascending order based on the
 * compare comparator.
 */
void vkarray_sort(struct vkarray *a, vkarray_compare_t compare) {
    ASSERT(a->nelem != 0);

    qsort(a->elem, a->nelem, a->size, compare);
}

/*
 * Calls the func once for each element in the array as long as func returns
 * success. On failure short-circuits and returns the error status.
 */
int vkarray_each(struct vkarray *a, vkarray_each_t func, void *data) {
    uint32_t i, nelem;

    ASSERT(vkarray_n(a) != 0);
    ASSERT(func != NULL);

    for (i = 0, nelem = vkarray_n(a); i < nelem; i++) {
        void *elem = vkarray_get(a, i);
        rstatus_t status;

        status = func(elem, data);
        if (status != VK_OK) {
            return status;
        }
    }

    return VK_OK;
}

/*
 * Copyright (c) 2021, Björn Svensson <bjorn.a.svensson@est.tech>
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

#ifndef __VALKEYCLUSTER_GLIB_H__
#define __VALKEYCLUSTER_GLIB_H__

#include "../valkeycluster.h"
#include <valkey/adapters/glib.h>

typedef struct valkeyClusterGlibAdapter {
    GMainContext *context;
} valkeyClusterGlibAdapter;

static int valkeyGlibAttach_link(valkeyAsyncContext *ac, void *adapter) {
    GMainContext *context = ((valkeyClusterGlibAdapter *)adapter)->context;
    if (g_source_attach(valkey_source_new(ac), context) > 0) {
        return VALKEY_OK;
    }
    return VALKEY_ERR;
}

static int valkeyClusterGlibAttach(valkeyClusterAsyncContext *acc,
                                  valkeyClusterGlibAdapter *adapter) {
    if (acc == NULL || adapter == NULL) {
        return VALKEY_ERR;
    }

    acc->adapter = adapter;
    acc->attach_fn = valkeyGlibAttach_link;

    return VALKEY_OK;
}

#endif

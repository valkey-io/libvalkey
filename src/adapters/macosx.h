//
//  Created by Дмитрий Бахвалов on 13.07.15.
//  Copyright (c) 2015 Dmitry Bakhvalov. All rights reserved.
//

#ifndef VALKEY_MACOSX_H
#define VALKEY_MACOSX_H

#include <CoreFoundation/CoreFoundation.h>

#include "../valkey.h"
#include "../async.h"

typedef struct {
    valkeyAsyncContext *context;
    CFSocketRef socketRef;
    CFRunLoopSourceRef sourceRef;
} ValkeyRunLoop;

static int freeValkeyRunLoop(ValkeyRunLoop* valkeyRunLoop) {
    if( valkeyRunLoop != NULL ) {
        if( valkeyRunLoop->sourceRef != NULL ) {
            CFRunLoopSourceInvalidate(valkeyRunLoop->sourceRef);
            CFRelease(valkeyRunLoop->sourceRef);
        }
        if( valkeyRunLoop->socketRef != NULL ) {
            CFSocketInvalidate(valkeyRunLoop->socketRef);
            CFRelease(valkeyRunLoop->socketRef);
        }
        vk_free(valkeyRunLoop);
    }
    return VALKEY_ERR;
}

static void valkeyMacOSAddRead(void *privdata) {
    ValkeyRunLoop *valkeyRunLoop = (ValkeyRunLoop*)privdata;
    CFSocketEnableCallBacks(valkeyRunLoop->socketRef, kCFSocketReadCallBack);
}

static void valkeyMacOSDelRead(void *privdata) {
    ValkeyRunLoop *valkeyRunLoop = (ValkeyRunLoop*)privdata;
    CFSocketDisableCallBacks(valkeyRunLoop->socketRef, kCFSocketReadCallBack);
}

static void valkeyMacOSAddWrite(void *privdata) {
    ValkeyRunLoop *valkeyRunLoop = (ValkeyRunLoop*)privdata;
    CFSocketEnableCallBacks(valkeyRunLoop->socketRef, kCFSocketWriteCallBack);
}

static void valkeyMacOSDelWrite(void *privdata) {
    ValkeyRunLoop *valkeyRunLoop = (ValkeyRunLoop*)privdata;
    CFSocketDisableCallBacks(valkeyRunLoop->socketRef, kCFSocketWriteCallBack);
}

static void valkeyMacOSCleanup(void *privdata) {
    ValkeyRunLoop *valkeyRunLoop = (ValkeyRunLoop*)privdata;
    freeValkeyRunLoop(valkeyRunLoop);
}

static void valkeyMacOSAsyncCallback(CFSocketRef __unused s, CFSocketCallBackType callbackType, CFDataRef __unused address, const void __unused *data, void *info) {
    valkeyAsyncContext* context = (valkeyAsyncContext*) info;

    switch (callbackType) {
        case kCFSocketReadCallBack:
            valkeyAsyncHandleRead(context);
            break;

        case kCFSocketWriteCallBack:
            valkeyAsyncHandleWrite(context);
            break;

        default:
            break;
    }
}

static int valkeyMacOSAttach(valkeyAsyncContext *valkeyAsyncCtx, CFRunLoopRef runLoop) {
    valkeyContext *valkeyCtx = &(valkeyAsyncCtx->c);

    /* Nothing should be attached when something is already attached */
    if( valkeyAsyncCtx->ev.data != NULL ) return VALKEY_ERR;

    ValkeyRunLoop* valkeyRunLoop = (ValkeyRunLoop*) vk_calloc(1, sizeof(ValkeyRunLoop));
    if (valkeyRunLoop == NULL)
        return VALKEY_ERR;

    /* Setup valkey stuff */
    valkeyRunLoop->context = valkeyAsyncCtx;

    valkeyAsyncCtx->ev.addRead  = valkeyMacOSAddRead;
    valkeyAsyncCtx->ev.delRead  = valkeyMacOSDelRead;
    valkeyAsyncCtx->ev.addWrite = valkeyMacOSAddWrite;
    valkeyAsyncCtx->ev.delWrite = valkeyMacOSDelWrite;
    valkeyAsyncCtx->ev.cleanup  = valkeyMacOSCleanup;
    valkeyAsyncCtx->ev.data     = valkeyRunLoop;

    /* Initialize and install read/write events */
    CFSocketContext socketCtx = { 0, valkeyAsyncCtx, NULL, NULL, NULL };

    valkeyRunLoop->socketRef = CFSocketCreateWithNative(NULL, valkeyCtx->fd,
                                                       kCFSocketReadCallBack | kCFSocketWriteCallBack,
                                                       valkeyMacOSAsyncCallback,
                                                       &socketCtx);
    if( !valkeyRunLoop->socketRef ) return freeValkeyRunLoop(valkeyRunLoop);

    valkeyRunLoop->sourceRef = CFSocketCreateRunLoopSource(NULL, valkeyRunLoop->socketRef, 0);
    if( !valkeyRunLoop->sourceRef ) return freeValkeyRunLoop(valkeyRunLoop);

    CFRunLoopAddSource(runLoop, valkeyRunLoop->sourceRef, kCFRunLoopDefaultMode);

    return VALKEY_OK;
}

#endif


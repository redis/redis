//
//  Created by Дмитрий Бахвалов on 13.07.15.
//  Copyright (c) 2015 Dmitry Bakhvalov. All rights reserved.
//

#ifndef __HIREDIS_MACOSX_H__
#define __HIREDIS_MACOSX_H__

#include <CoreFoundation/CoreFoundation.h>

#include "../hiredis.h"
#include "../async.h"

typedef struct {
    redisAsyncContext *context;
    CFSocketRef socketRef;
    CFRunLoopSourceRef sourceRef;
} RedisRunLoop;

static int freeRedisRunLoop(RedisRunLoop* redisRunLoop) {
    if( redisRunLoop != NULL ) {
        if( redisRunLoop->sourceRef != NULL ) {
            CFRunLoopSourceInvalidate(redisRunLoop->sourceRef);
            CFRelease(redisRunLoop->sourceRef);
        }
        if( redisRunLoop->socketRef != NULL ) {
            CFSocketInvalidate(redisRunLoop->socketRef);
            CFRelease(redisRunLoop->socketRef);
        }
        free(redisRunLoop);
    }
    return REDIS_ERR;
}

static void redisMacOSAddRead(void *privdata) {
    RedisRunLoop *redisRunLoop = (RedisRunLoop*)privdata;
    CFSocketEnableCallBacks(redisRunLoop->socketRef, kCFSocketReadCallBack);
}

static void redisMacOSDelRead(void *privdata) {
    RedisRunLoop *redisRunLoop = (RedisRunLoop*)privdata;
    CFSocketDisableCallBacks(redisRunLoop->socketRef, kCFSocketReadCallBack);
}

static void redisMacOSAddWrite(void *privdata) {
    RedisRunLoop *redisRunLoop = (RedisRunLoop*)privdata;
    CFSocketEnableCallBacks(redisRunLoop->socketRef, kCFSocketWriteCallBack);
}

static void redisMacOSDelWrite(void *privdata) {
    RedisRunLoop *redisRunLoop = (RedisRunLoop*)privdata;
    CFSocketDisableCallBacks(redisRunLoop->socketRef, kCFSocketWriteCallBack);
}

static void redisMacOSCleanup(void *privdata) {
    RedisRunLoop *redisRunLoop = (RedisRunLoop*)privdata;
    freeRedisRunLoop(redisRunLoop);
}

static void redisMacOSAsyncCallback(CFSocketRef __unused s, CFSocketCallBackType callbackType, CFDataRef __unused address, const void __unused *data, void *info) {
    redisAsyncContext* context = (redisAsyncContext*) info;

    switch (callbackType) {
        case kCFSocketReadCallBack:
            redisAsyncHandleRead(context);
            break;

        case kCFSocketWriteCallBack:
            redisAsyncHandleWrite(context);
            break;

        default:
            break;
    }
}

static int redisMacOSAttach(redisAsyncContext *redisAsyncCtx, CFRunLoopRef runLoop) {
    redisContext *redisCtx = &(redisAsyncCtx->c);

    /* Nothing should be attached when something is already attached */
    if( redisAsyncCtx->ev.data != NULL ) return REDIS_ERR;

    RedisRunLoop* redisRunLoop = (RedisRunLoop*) calloc(1, sizeof(RedisRunLoop));
    if( !redisRunLoop ) return REDIS_ERR;

    /* Setup redis stuff */
    redisRunLoop->context = redisAsyncCtx;

    redisAsyncCtx->ev.addRead  = redisMacOSAddRead;
    redisAsyncCtx->ev.delRead  = redisMacOSDelRead;
    redisAsyncCtx->ev.addWrite = redisMacOSAddWrite;
    redisAsyncCtx->ev.delWrite = redisMacOSDelWrite;
    redisAsyncCtx->ev.cleanup  = redisMacOSCleanup;
    redisAsyncCtx->ev.data     = redisRunLoop;

    /* Initialize and install read/write events */
    CFSocketContext socketCtx = { 0, redisAsyncCtx, NULL, NULL, NULL };

    redisRunLoop->socketRef = CFSocketCreateWithNative(NULL, redisCtx->fd,
                                                       kCFSocketReadCallBack | kCFSocketWriteCallBack,
                                                       redisMacOSAsyncCallback,
                                                       &socketCtx);
    if( !redisRunLoop->socketRef ) return freeRedisRunLoop(redisRunLoop);

    redisRunLoop->sourceRef = CFSocketCreateRunLoopSource(NULL, redisRunLoop->socketRef, 0);
    if( !redisRunLoop->sourceRef ) return freeRedisRunLoop(redisRunLoop);

    CFRunLoopAddSource(runLoop, redisRunLoop->sourceRef, kCFRunLoopDefaultMode);

    return REDIS_OK;
}

#endif


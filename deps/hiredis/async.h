/*
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010, Pieter Noordhuis <pcnoordhuis at gmail dot com>
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

#ifndef __HIREDIS_ASYNC_H
#define __HIREDIS_ASYNC_H
#include "hiredis.h"

#ifdef __cplusplus
extern "C" {
#endif

struct redisAsyncContext; /* need forward declaration of redisAsyncContext */
struct dict; /* dictionary header is included in async.c */

/* Reply callback prototype and container */
typedef void (redisCallbackFn)(struct redisAsyncContext*, void*, void*);
typedef struct redisCallback {
    struct redisCallback *next; /* simple singly linked list */
    redisCallbackFn *fn;
    void *privdata;
} redisCallback;

/* List of callbacks for either regular replies or pub/sub */
typedef struct redisCallbackList {
    redisCallback *head, *tail;
} redisCallbackList;

/* Connection callback prototypes */
typedef void (redisDisconnectCallback)(const struct redisAsyncContext*, int status);
typedef void (redisConnectCallback)(const struct redisAsyncContext*);

/* Context for an async connection to Redis */
typedef struct redisAsyncContext {
    /* Hold the regular context, so it can be realloc'ed. */
    redisContext c;

    /* Setup error flags so they can be used directly. */
    int err;
    char *errstr;

    /* Not used by hiredis */
    void *data;

    /* Event library data and hooks */
    struct {
        void *data;

        /* Hooks that are called when the library expects to start
         * reading/writing. These functions should be idempotent. */
        void (*addRead)(void *privdata);
        void (*delRead)(void *privdata);
        void (*addWrite)(void *privdata);
        void (*delWrite)(void *privdata);
        void (*cleanup)(void *privdata);
    } ev;

    /* Called when either the connection is terminated due to an error or per
     * user request. The status is set accordingly (REDIS_OK, REDIS_ERR). */
    redisDisconnectCallback *onDisconnect;

    /* Called when the first write event was received. */
    redisConnectCallback *onConnect;

    /* Regular command callbacks */
    redisCallbackList replies;

    /* Subscription callbacks */
    struct {
        redisCallbackList invalid;
        struct dict *channels;
        struct dict *patterns;
    } sub;
} redisAsyncContext;

/* Functions that proxy to hiredis */
redisAsyncContext *redisAsyncConnect(const char *ip, int port);
redisAsyncContext *redisAsyncConnectUnix(const char *path);
int redisAsyncSetReplyObjectFunctions(redisAsyncContext *ac, redisReplyObjectFunctions *fn);
int redisAsyncSetConnectCallback(redisAsyncContext *ac, redisConnectCallback *fn);
int redisAsyncSetDisconnectCallback(redisAsyncContext *ac, redisDisconnectCallback *fn);
void redisAsyncDisconnect(redisAsyncContext *ac);
void redisAsyncFree(redisAsyncContext *ac);

/* Handle read/write events */
void redisAsyncHandleRead(redisAsyncContext *ac);
void redisAsyncHandleWrite(redisAsyncContext *ac);

/* Command functions for an async context. Write the command to the
 * output buffer and register the provided callback. */
int redisvAsyncCommand(redisAsyncContext *ac, redisCallbackFn *fn, void *privdata, const char *format, va_list ap);
int redisAsyncCommand(redisAsyncContext *ac, redisCallbackFn *fn, void *privdata, const char *format, ...);
int redisAsyncCommandArgv(redisAsyncContext *ac, redisCallbackFn *fn, void *privdata, int argc, const char **argv, const size_t *argvlen);

#ifdef __cplusplus
}
#endif

#endif

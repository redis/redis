/* This module is used to test the propagation (replication + AOF) of
 * commands, via the RedisModule_Replicate() interface, in asynchronous
 * contexts, such as callbacks not implementing commands, and thread safe
 * contexts.
 *
 * We create a timer callback and a threads using a thread safe context.
 * Using both we try to propagate counters increments, and later we check
 * if the replica contains the changes as expected.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2019, Salvatore Sanfilippo <antirez at gmail dot com>
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

#define REDISMODULE_EXPERIMENTAL_API
#include "redismodule.h"
#include <pthread.h>

/* Timer callback. */
void timerHandler(RedisModuleCtx *ctx, void *data) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(data);

    static int times = 0;

    RedisModule_Replicate(ctx,"INCR","c","timer");
    times++;

    if (times < 3)
        RedisModule_CreateTimer(ctx,100,timerHandler,NULL);
    else
        times = 0;
}

int propagateTestTimerCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModuleTimerID timer_id =
        RedisModule_CreateTimer(ctx,100,timerHandler,NULL);
    REDISMODULE_NOT_USED(timer_id);

    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;
}

/* Timer callback. */
void timerNestedHandler(RedisModuleCtx *ctx, void *data) {
    int repl = (long long)data;

    /* The goal is the trigger a module command that calls RM_Replicate
     * in order to test MULTI/EXEC structre */
    RedisModule_Replicate(ctx,"INCRBY","cc","timer-nested-start","1");
    RedisModuleCallReply *reply = RedisModule_Call(ctx,"propagate-test.nested", repl? "!" : "");
    RedisModule_FreeCallReply(reply);
    RedisModule_Replicate(ctx,"INCRBY","cc","timer-nested-end","1");
}

int propagateTestTimerNestedCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModuleTimerID timer_id =
        RedisModule_CreateTimer(ctx,100,timerNestedHandler,(void*)0);
    REDISMODULE_NOT_USED(timer_id);

    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;
}

int propagateTestTimerNestedReplCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModuleTimerID timer_id =
        RedisModule_CreateTimer(ctx,100,timerNestedHandler,(void*)1);
    REDISMODULE_NOT_USED(timer_id);

    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;
}

/* The thread entry point. */
void *threadMain(void *arg) {
    REDISMODULE_NOT_USED(arg);
    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(NULL);
    RedisModule_SelectDb(ctx,9); /* Tests ran in database number 9. */
    for (int i = 0; i < 3; i++) {
        RedisModule_ThreadSafeContextLock(ctx);
        RedisModule_Replicate(ctx,"INCR","c","a-from-thread");
        RedisModule_Replicate(ctx,"INCR","c","b-from-thread");
        RedisModule_ThreadSafeContextUnlock(ctx);
    }
    RedisModule_FreeThreadSafeContext(ctx);
    return NULL;
}

int propagateTestThreadCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    pthread_t tid;
    if (pthread_create(&tid,NULL,threadMain,NULL) != 0)
        return RedisModule_ReplyWithError(ctx,"-ERR Can't start thread");
    REDISMODULE_NOT_USED(tid);

    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;
}

int propagateTestSimpleCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    /* Replicate two commands to test MULTI/EXEC wrapping. */
    RedisModule_Replicate(ctx,"INCR","c","counter-1");
    RedisModule_Replicate(ctx,"INCR","c","counter-2");
    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;
}

int propagateTestMixedCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModuleCallReply *reply;

    /* This test mixes multiple propagation systems. */
    reply = RedisModule_Call(ctx, "INCR", "c!", "using-call");
    RedisModule_FreeCallReply(reply);

    RedisModule_Replicate(ctx,"INCR","c","counter-1");
    RedisModule_Replicate(ctx,"INCR","c","counter-2");

    reply = RedisModule_Call(ctx, "INCR", "c!", "after-call");
    RedisModule_FreeCallReply(reply);

    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;
}

int propagateTestNestedCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModuleCallReply *reply;

    /* This test mixes multiple propagation systems. */
    reply = RedisModule_Call(ctx, "INCR", "c!", "using-call");
    RedisModule_FreeCallReply(reply);

    reply = RedisModule_Call(ctx,"propagate-test.simple", "!");
    RedisModule_FreeCallReply(reply);

    RedisModule_Replicate(ctx,"INCR","c","counter-3");
    RedisModule_Replicate(ctx,"INCR","c","counter-4");

    reply = RedisModule_Call(ctx, "INCR", "c!", "after-call");
    RedisModule_FreeCallReply(reply);

    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"propagate-test",1,REDISMODULE_APIVER_1)
            == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"propagate-test.timer",
                propagateTestTimerCommand,
                "",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"propagate-test.timer-nested",
                propagateTestTimerNestedCommand,
                "",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"propagate-test.timer-nested-repl",
                propagateTestTimerNestedReplCommand,
                "",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"propagate-test.thread",
                propagateTestThreadCommand,
                "",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"propagate-test.simple",
                propagateTestSimpleCommand,
                "",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"propagate-test.mixed",
                propagateTestMixedCommand,
                "",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"propagate-test.nested",
                propagateTestNestedCommand,
                "",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

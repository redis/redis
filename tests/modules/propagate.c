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
 * Copyright (c) 2019-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#include "redismodule.h"
#include <pthread.h>
#include <errno.h>

#define UNUSED(V) ((void) V)

RedisModuleCtx *detached_ctx = NULL;

static int KeySpace_NotificationGeneric(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);
    REDISMODULE_NOT_USED(key);

    RedisModuleCallReply* rep = RedisModule_Call(ctx, "INCR", "c!", "notifications");
    RedisModule_FreeCallReply(rep);

    return REDISMODULE_OK;
}

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
     * in order to test MULTI/EXEC structure */
    RedisModule_Replicate(ctx,"INCRBY","cc","timer-nested-start","1");
    RedisModuleCallReply *reply = RedisModule_Call(ctx,"propagate-test.nested", repl? "!" : "");
    RedisModule_FreeCallReply(reply);
    reply = RedisModule_Call(ctx, "INCR", repl? "c!" : "c", "timer-nested-middle");
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

void timerHandlerMaxmemory(RedisModuleCtx *ctx, void *data) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(data);

    RedisModuleCallReply *reply = RedisModule_Call(ctx,"SETEX","ccc!","timer-maxmemory-volatile-start","100","1");
    RedisModule_FreeCallReply(reply);
    reply = RedisModule_Call(ctx, "CONFIG", "ccc!", "SET", "maxmemory", "1");
    RedisModule_FreeCallReply(reply);

    RedisModule_Replicate(ctx, "INCR", "c", "timer-maxmemory-middle");

    reply = RedisModule_Call(ctx,"SETEX","ccc!","timer-maxmemory-volatile-end","100","1");
    RedisModule_FreeCallReply(reply);
}

int propagateTestTimerMaxmemoryCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModuleTimerID timer_id =
        RedisModule_CreateTimer(ctx,100,timerHandlerMaxmemory,(void*)1);
    REDISMODULE_NOT_USED(timer_id);

    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;
}

void timerHandlerEval(RedisModuleCtx *ctx, void *data) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(data);

    RedisModuleCallReply *reply = RedisModule_Call(ctx,"INCRBY","cc!","timer-eval-start","1");
    RedisModule_FreeCallReply(reply);
    reply = RedisModule_Call(ctx, "EVAL", "cccc!", "redis.call('set',KEYS[1],ARGV[1])", "1", "foo", "bar");
    RedisModule_FreeCallReply(reply);

    RedisModule_Replicate(ctx, "INCR", "c", "timer-eval-middle");

    reply = RedisModule_Call(ctx,"INCRBY","cc!","timer-eval-end","1");
    RedisModule_FreeCallReply(reply);
}

int propagateTestTimerEvalCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModuleTimerID timer_id =
        RedisModule_CreateTimer(ctx,100,timerHandlerEval,(void*)1);
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
        RedisModuleCallReply *reply = RedisModule_Call(ctx,"INCR","c!","thread-call");
        RedisModule_FreeCallReply(reply);
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
    pthread_detach(tid);

    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;
}

/* The thread entry point. */
void *threadDetachedMain(void *arg) {
    REDISMODULE_NOT_USED(arg);
    RedisModule_SelectDb(detached_ctx,9); /* Tests ran in database number 9. */

    RedisModule_ThreadSafeContextLock(detached_ctx);
    RedisModule_Replicate(detached_ctx,"INCR","c","thread-detached-before");
    RedisModuleCallReply *reply = RedisModule_Call(detached_ctx,"INCR","c!","thread-detached-1");
    RedisModule_FreeCallReply(reply);
    reply = RedisModule_Call(detached_ctx,"INCR","c!","thread-detached-2");
    RedisModule_FreeCallReply(reply);
    RedisModule_Replicate(detached_ctx,"INCR","c","thread-detached-after");
    RedisModule_ThreadSafeContextUnlock(detached_ctx);

    return NULL;
}

int propagateTestDetachedThreadCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    pthread_t tid;
    if (pthread_create(&tid,NULL,threadDetachedMain,NULL) != 0)
        return RedisModule_ReplyWithError(ctx,"-ERR Can't start thread");
    pthread_detach(tid);

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

    reply = RedisModule_Call(ctx, "INCR", "c!", "before-call-2");
    RedisModule_FreeCallReply(reply);

    reply = RedisModule_Call(ctx, "keyspace.incr_case1", "c!", "asdf"); /* Propagates INCR */
    RedisModule_FreeCallReply(reply);

    reply = RedisModule_Call(ctx, "keyspace.del_key_copy", "c!", "asdf"); /* Propagates DEL */
    RedisModule_FreeCallReply(reply);

    reply = RedisModule_Call(ctx, "INCR", "c!", "after-call-2");
    RedisModule_FreeCallReply(reply);

    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;
}

int propagateTestIncr(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argc);
    RedisModuleCallReply *reply;

    /* This test propagates the module command, not the INCR it executes. */
    reply = RedisModule_Call(ctx, "INCR", "s", argv[1]);
    RedisModule_ReplyWithCallReply(ctx,reply);
    RedisModule_FreeCallReply(reply);
    RedisModule_ReplicateVerbatim(ctx);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"propagate-test",1,REDISMODULE_APIVER_1)
            == REDISMODULE_ERR) return REDISMODULE_ERR;

    detached_ctx = RedisModule_GetDetachedThreadSafeContext(ctx);

    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_ALL, KeySpace_NotificationGeneric) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

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

    if (RedisModule_CreateCommand(ctx,"propagate-test.timer-maxmemory",
                propagateTestTimerMaxmemoryCommand,
                "",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"propagate-test.timer-eval",
                propagateTestTimerEvalCommand,
                "",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"propagate-test.thread",
                propagateTestThreadCommand,
                "",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"propagate-test.detached-thread",
                propagateTestDetachedThreadCommand,
                "",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"propagate-test.simple",
                propagateTestSimpleCommand,
                "",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"propagate-test.mixed",
                propagateTestMixedCommand,
                "write",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"propagate-test.nested",
                propagateTestNestedCommand,
                "write",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"propagate-test.incr",
                propagateTestIncr,
                "write",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    UNUSED(ctx);

    if (detached_ctx)
        RedisModule_FreeThreadSafeContext(detached_ctx);

    return REDISMODULE_OK;
}

#define REDISMODULE_EXPERIMENTAL_API
#define _XOPEN_SOURCE 700
#include "redismodule.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#define UNUSED(x) (void)(x)

/* Reply callback for blocking command BLOCK.DEBUG */
int HelloBlock_Reply(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    int *myint = RedisModule_GetBlockedClientPrivateData(ctx);
    return RedisModule_ReplyWithLongLong(ctx,*myint);
}

/* Timeout callback for blocking command BLOCK.DEBUG */
int HelloBlock_Timeout(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    RedisModuleBlockedClient *bc = RedisModule_GetBlockedClientHandle(ctx);
    RedisModule_BlockedClientMeasureTimeEnd(bc);
    return RedisModule_ReplyWithSimpleString(ctx,"Request timedout");
}

/* Private data freeing callback for BLOCK.DEBUG command. */
void HelloBlock_FreeData(RedisModuleCtx *ctx, void *privdata) {
    UNUSED(ctx);
    RedisModule_Free(privdata);
}

/* The thread entry point that actually executes the blocking part
 * of the command BLOCK.DEBUG. */
void *BlockDebug_ThreadMain(void *arg) {
    void **targ = arg;
    RedisModuleBlockedClient *bc = targ[0];
    long long delay = (unsigned long)targ[1];
    long long enable_time_track = (unsigned long)targ[2];
    if (enable_time_track)
        RedisModule_BlockedClientMeasureTimeStart(bc);
    RedisModule_Free(targ);

    struct timespec ts;
    ts.tv_sec = delay / 1000;
    ts.tv_nsec = (delay % 1000) * 1000000;
    nanosleep(&ts, NULL);
    int *r = RedisModule_Alloc(sizeof(int));
    *r = rand();
    if (enable_time_track)
        RedisModule_BlockedClientMeasureTimeEnd(bc);
    RedisModule_UnblockClient(bc,r);
    return NULL;
}

/* The thread entry point that actually executes the blocking part
 * of the command BLOCK.DOUBLE_DEBUG. */
void *DoubleBlock_ThreadMain(void *arg) {
    void **targ = arg;
    RedisModuleBlockedClient *bc = targ[0];
    long long delay = (unsigned long)targ[1];
    RedisModule_BlockedClientMeasureTimeStart(bc);
    RedisModule_Free(targ);
    struct timespec ts;
    ts.tv_sec = delay / 1000;
    ts.tv_nsec = (delay % 1000) * 1000000;
    nanosleep(&ts, NULL);
    int *r = RedisModule_Alloc(sizeof(int));
    *r = rand();
    RedisModule_BlockedClientMeasureTimeEnd(bc);
    /* call again RedisModule_BlockedClientMeasureTimeStart() and
     * RedisModule_BlockedClientMeasureTimeEnd and ensure that the
     * total execution time is 2x the delay. */
    RedisModule_BlockedClientMeasureTimeStart(bc);
    nanosleep(&ts, NULL);
    RedisModule_BlockedClientMeasureTimeEnd(bc);

    RedisModule_UnblockClient(bc,r);
    return NULL;
}

void HelloBlock_Disconnected(RedisModuleCtx *ctx, RedisModuleBlockedClient *bc) {
    RedisModule_Log(ctx,"warning","Blocked client %p disconnected!",
        (void*)bc);
}

/* BLOCK.DEBUG <delay_ms> <timeout_ms> -- Block for <count> milliseconds, then reply with
 * a random number. Timeout is the command timeout, so that you can test
 * what happens when the delay is greater than the timeout. */
int HelloBlock_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) return RedisModule_WrongArity(ctx);
    long long delay;
    long long timeout;

    if (RedisModule_StringToLongLong(argv[1],&delay) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx,"ERR invalid count");
    }

    if (RedisModule_StringToLongLong(argv[2],&timeout) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx,"ERR invalid count");
    }

    pthread_t tid;
    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx,HelloBlock_Reply,HelloBlock_Timeout,HelloBlock_FreeData,timeout);

    /* Here we set a disconnection handler, however since this module will
     * block in sleep() in a thread, there is not much we can do in the
     * callback, so this is just to show you the API. */
    RedisModule_SetDisconnectCallback(bc,HelloBlock_Disconnected);

    /* Now that we setup a blocking client, we need to pass the control
     * to the thread. However we need to pass arguments to the thread:
     * the delay and a reference to the blocked client handle. */
    void **targ = RedisModule_Alloc(sizeof(void*)*3);
    targ[0] = bc;
    targ[1] = (void*)(unsigned long) delay;
    // pass 1 as flag to enable time tracking
    targ[2] = (void*)(unsigned long) 1;

    if (pthread_create(&tid,NULL,BlockDebug_ThreadMain,targ) != 0) {
        RedisModule_AbortBlock(bc);
        return RedisModule_ReplyWithError(ctx,"-ERR Can't start thread");
    }
    return REDISMODULE_OK;
}

/* BLOCK.DEBUG_NOTRACKING <delay_ms> <timeout_ms> -- Block for <count> milliseconds, then reply with
 * a random number. Timeout is the command timeout, so that you can test
 * what happens when the delay is greater than the timeout.
 * this command does not track background time so the background time should no appear in stats*/
int HelloBlockNoTracking_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) return RedisModule_WrongArity(ctx);
    long long delay;
    long long timeout;

    if (RedisModule_StringToLongLong(argv[1],&delay) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx,"ERR invalid count");
    }

    if (RedisModule_StringToLongLong(argv[2],&timeout) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx,"ERR invalid count");
    }

    pthread_t tid;
    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx,HelloBlock_Reply,HelloBlock_Timeout,HelloBlock_FreeData,timeout);

    /* Here we set a disconnection handler, however since this module will
     * block in sleep() in a thread, there is not much we can do in the
     * callback, so this is just to show you the API. */
    RedisModule_SetDisconnectCallback(bc,HelloBlock_Disconnected);

    /* Now that we setup a blocking client, we need to pass the control
     * to the thread. However we need to pass arguments to the thread:
     * the delay and a reference to the blocked client handle. */
    void **targ = RedisModule_Alloc(sizeof(void*)*3);
    targ[0] = bc;
    targ[1] = (void*)(unsigned long) delay;
    // pass 0 as flag to enable time tracking
    targ[2] = (void*)(unsigned long) 0;

    if (pthread_create(&tid,NULL,BlockDebug_ThreadMain,targ) != 0) {
        RedisModule_AbortBlock(bc);
        return RedisModule_ReplyWithError(ctx,"-ERR Can't start thread");
    }
    return REDISMODULE_OK;
}

/* BLOCK.DOUBLE_DEBUG <delay_ms> -- Block for 2 x <count> milliseconds,
 * then reply with a random number.
 * This command is used to test multiple calls to RedisModule_BlockedClientMeasureTimeStart()
 * and RedisModule_BlockedClientMeasureTimeEnd() within the same execution. */
int HelloDoubleBlock_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);
    long long delay;

    if (RedisModule_StringToLongLong(argv[1],&delay) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx,"ERR invalid count");
    }

    pthread_t tid;
    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx,HelloBlock_Reply,HelloBlock_Timeout,HelloBlock_FreeData,0);

    /* Now that we setup a blocking client, we need to pass the control
     * to the thread. However we need to pass arguments to the thread:
     * the delay and a reference to the blocked client handle. */
    void **targ = RedisModule_Alloc(sizeof(void*)*2);
    targ[0] = bc;
    targ[1] = (void*)(unsigned long) delay;

    if (pthread_create(&tid,NULL,DoubleBlock_ThreadMain,targ) != 0) {
        RedisModule_AbortBlock(bc);
        return RedisModule_ReplyWithError(ctx,"-ERR Can't start thread");
    }
    return REDISMODULE_OK;
}


int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    if (RedisModule_Init(ctx,"block",1,REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"block.debug",
        HelloBlock_RedisCommand,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"block.double_debug",
        HelloDoubleBlock_RedisCommand,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"block.debug_no_track",
        HelloBlockNoTracking_RedisCommand,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

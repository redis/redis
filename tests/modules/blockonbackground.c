#define _XOPEN_SOURCE 700
#include "redismodule.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#define UNUSED(x) (void)(x)

typedef struct {
    /* Mutex for protecting RedisModule_BlockedClientMeasureTime*() API from race
     * conditions due to timeout callback triggered in the main thread. */
    pthread_mutex_t measuretime_mutex;
    int measuretime_completed; /* Indicates that time measure has ended and will not continue further */
    int myint; /* Used for replying */
} BlockPrivdata;

void blockClientPrivdataInit(RedisModuleBlockedClient *bc) {
    BlockPrivdata *block_privdata = RedisModule_Calloc(1, sizeof(*block_privdata));
    block_privdata->measuretime_mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    RedisModule_BlockClientSetPrivateData(bc, block_privdata);
}

void blockClientMeasureTimeStart(RedisModuleBlockedClient *bc, BlockPrivdata *block_privdata) {
    pthread_mutex_lock(&block_privdata->measuretime_mutex);
    RedisModule_BlockedClientMeasureTimeStart(bc);
    pthread_mutex_unlock(&block_privdata->measuretime_mutex);
}

void blockClientMeasureTimeEnd(RedisModuleBlockedClient *bc, BlockPrivdata *block_privdata, int completed) {
    pthread_mutex_lock(&block_privdata->measuretime_mutex);
    if (!block_privdata->measuretime_completed) {
        RedisModule_BlockedClientMeasureTimeEnd(bc);
        if (completed) block_privdata->measuretime_completed = 1;
    }
    pthread_mutex_unlock(&block_privdata->measuretime_mutex);
}

/* Reply callback for blocking command BLOCK.DEBUG */
int HelloBlock_Reply(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    BlockPrivdata *block_privdata = RedisModule_GetBlockedClientPrivateData(ctx);
    return RedisModule_ReplyWithLongLong(ctx,block_privdata->myint);
}

/* Timeout callback for blocking command BLOCK.DEBUG */
int HelloBlock_Timeout(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    RedisModuleBlockedClient *bc = RedisModule_GetBlockedClientHandle(ctx);
    BlockPrivdata *block_privdata = RedisModule_GetBlockedClientPrivateData(ctx);
    blockClientMeasureTimeEnd(bc, block_privdata, 1);
    return RedisModule_ReplyWithSimpleString(ctx,"Request timedout");
}

/* Private data freeing callback for BLOCK.DEBUG command. */
void HelloBlock_FreeData(RedisModuleCtx *ctx, void *privdata) {
    UNUSED(ctx);
    BlockPrivdata *block_privdata = privdata;
    pthread_mutex_destroy(&block_privdata->measuretime_mutex);
    RedisModule_Free(privdata);
}

/* Private data freeing callback for BLOCK.BLOCK command. */
void HelloBlock_FreeStringData(RedisModuleCtx *ctx, void *privdata) {
    RedisModule_FreeString(ctx, (RedisModuleString*)privdata);
}

/* The thread entry point that actually executes the blocking part
 * of the command BLOCK.DEBUG. */
void *BlockDebug_ThreadMain(void *arg) {
    void **targ = arg;
    RedisModuleBlockedClient *bc = targ[0];
    long long delay = (unsigned long)targ[1];
    long long enable_time_track = (unsigned long)targ[2];
    BlockPrivdata *block_privdata = RedisModule_BlockClientGetPrivateData(bc);

    if (enable_time_track)
        blockClientMeasureTimeStart(bc, block_privdata);
    RedisModule_Free(targ);

    struct timespec ts;
    ts.tv_sec = delay / 1000;
    ts.tv_nsec = (delay % 1000) * 1000000;
    nanosleep(&ts, NULL);
    if (enable_time_track)
        blockClientMeasureTimeEnd(bc, block_privdata, 0);
    block_privdata->myint = rand();
    RedisModule_UnblockClient(bc,block_privdata);
    return NULL;
}

/* The thread entry point that actually executes the blocking part
 * of the command BLOCK.DOUBLE_DEBUG. */
void *DoubleBlock_ThreadMain(void *arg) {
    void **targ = arg;
    RedisModuleBlockedClient *bc = targ[0];
    long long delay = (unsigned long)targ[1];
    BlockPrivdata *block_privdata = RedisModule_BlockClientGetPrivateData(bc);
    blockClientMeasureTimeStart(bc, block_privdata);
    RedisModule_Free(targ);
    struct timespec ts;
    ts.tv_sec = delay / 1000;
    ts.tv_nsec = (delay % 1000) * 1000000;
    nanosleep(&ts, NULL);
    blockClientMeasureTimeEnd(bc, block_privdata, 0);
    /* call again RedisModule_BlockedClientMeasureTimeStart() and
     * RedisModule_BlockedClientMeasureTimeEnd and ensure that the
     * total execution time is 2x the delay. */
    blockClientMeasureTimeStart(bc, block_privdata);
    nanosleep(&ts, NULL);
    blockClientMeasureTimeEnd(bc, block_privdata, 0);
    block_privdata->myint = rand();
    RedisModule_UnblockClient(bc,block_privdata);
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
    blockClientPrivdataInit(bc);

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
    pthread_detach(tid);
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
    blockClientPrivdataInit(bc);

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
    pthread_detach(tid);
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
    blockClientPrivdataInit(bc);

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
    pthread_detach(tid);
    return REDISMODULE_OK;
}

RedisModuleBlockedClient *blocked_client = NULL;

/* BLOCK.BLOCK [TIMEOUT] -- Blocks the current client until released
 * or TIMEOUT seconds. If TIMEOUT is zero, no timeout function is
 * registered.
 */
int Block_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_IsBlockedReplyRequest(ctx)) {
        RedisModuleString *r = RedisModule_GetBlockedClientPrivateData(ctx);
        return RedisModule_ReplyWithString(ctx, r);
    } else if (RedisModule_IsBlockedTimeoutRequest(ctx)) {
        RedisModule_UnblockClient(blocked_client, NULL); /* Must be called to avoid leaks. */
        blocked_client = NULL;
        return RedisModule_ReplyWithSimpleString(ctx, "Timed out");
    }

    if (argc != 2) return RedisModule_WrongArity(ctx);
    long long timeout;

    if (RedisModule_StringToLongLong(argv[1], &timeout) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx, "ERR invalid timeout");
    }
    if (blocked_client) {
        return RedisModule_ReplyWithError(ctx, "ERR another client already blocked");
    }

    /* Block client. We use this function as both a reply and optional timeout
     * callback and differentiate the different code flows above.
     */
    blocked_client = RedisModule_BlockClient(ctx, Block_RedisCommand,
            timeout > 0 ? Block_RedisCommand : NULL, HelloBlock_FreeStringData, timeout);
    return REDISMODULE_OK;
}

/* BLOCK.IS_BLOCKED -- Returns 1 if we have a blocked client, or 0 otherwise.
 */
int IsBlocked_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    RedisModule_ReplyWithLongLong(ctx, blocked_client ? 1 : 0);
    return REDISMODULE_OK;
}

/* BLOCK.RELEASE [reply] -- Releases the blocked client and produce the specified reply.
 */
int Release_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);
    if (!blocked_client) {
        return RedisModule_ReplyWithError(ctx, "ERR No blocked client");
    }

    RedisModuleString *replystr = argv[1];
    RedisModule_RetainString(ctx, replystr);
    RedisModule_UnblockClient(blocked_client, replystr);
    blocked_client = NULL;

    RedisModule_ReplyWithSimpleString(ctx, "OK");

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

    if (RedisModule_CreateCommand(ctx, "block.block",
        Block_RedisCommand, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"block.is_blocked",
        IsBlocked_RedisCommand,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"block.release",
        Release_RedisCommand,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

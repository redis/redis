#define REDISMODULE_EXPERIMENTAL_API
#include "redismodule.h"
#include <assert.h>
#include <stdio.h>
#include <pthread.h>

#define UNUSED(V) ((void) V)

void *sub_worker(void *arg) {
    // Get Redis module context
    RedisModuleCtx *ctx = (RedisModuleCtx *)arg;

    // Try acquiring GIL
    int res = RedisModule_ThreadSafeContextTryLock(ctx);

    // GIL is already taken by the calling thread expecting to fail.
    assert(res != REDISMODULE_OK);

    return NULL;
}

void *worker(void *arg) {
    // Retrieve blocked client
    RedisModuleBlockedClient *bc = (RedisModuleBlockedClient *)arg;

    // Get Redis module context
    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(bc);

    // Acquire GIL
    RedisModule_ThreadSafeContextLock(ctx);

    // Create another thread which will try to acquire the GIL
    pthread_t tid;
    int res = pthread_create(&tid, NULL, sub_worker, ctx);
    assert(res == 0);

    // Wait for thread
    pthread_join(tid, NULL);

    // Release GIL
    RedisModule_ThreadSafeContextUnlock(ctx);

    // Reply to client
    RedisModule_ReplyWithSimpleString(ctx, "OK");

    // Unblock client
    RedisModule_UnblockClient(bc, NULL);

    // Free the Redis module context
    RedisModule_FreeThreadSafeContext(ctx);

    return NULL;
}

int acquire_gil(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    UNUSED(argv);
    UNUSED(argc);

    int flags = RedisModule_GetContextFlags(ctx);
    int allFlags = RedisModule_GetContextFlagsAll();
    if ((allFlags & REDISMODULE_CTX_FLAGS_MULTI) &&
        (flags & REDISMODULE_CTX_FLAGS_MULTI)) {
        RedisModule_ReplyWithSimpleString(ctx, "Blocked client is not supported inside multi");
        return REDISMODULE_OK;
    }

    if ((allFlags & REDISMODULE_CTX_FLAGS_DENY_BLOCKING) &&
        (flags & REDISMODULE_CTX_FLAGS_DENY_BLOCKING)) {
        RedisModule_ReplyWithSimpleString(ctx, "Blocked client is not allowed");
        return REDISMODULE_OK;
    }

    /* This command handler tries to acquire the GIL twice
     * once in the worker thread using "RedisModule_ThreadSafeContextLock"
     * second in the sub-worker thread
     * using "RedisModule_ThreadSafeContextTryLock"
     * as the GIL is already locked. */
    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);

    pthread_t tid;
    int res = pthread_create(&tid, NULL, worker, bc);
    assert(res == 0);

    return REDISMODULE_OK;
}

typedef struct {
    RedisModuleString **argv;
    int argc;
    RedisModuleBlockedClient *bc;
} bg_call_data;

void *bg_call_worker(void *arg) {
    bg_call_data *bg = arg;

    // Get Redis module context
    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(bg->bc);

    // Acquire GIL
    RedisModule_ThreadSafeContextLock(ctx);

    // Call the command
    const char* cmd = RedisModule_StringPtrLen(bg->argv[1], NULL);
    RedisModuleCallReply* rep = RedisModule_Call(ctx, cmd, "v", bg->argv + 2, bg->argc - 2);

    // Release GIL
    RedisModule_ThreadSafeContextUnlock(ctx);

    // Reply to client
    if (!rep) {
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }

    // Unblock client
    RedisModule_UnblockClient(bg->bc, NULL);

    /* Free the arguments */
    for (int i=0; i<bg->argc; i++)
        RedisModule_FreeString(ctx, bg->argv[i]);
    RedisModule_Free(bg->argv);
    RedisModule_Free(bg);

    // Free the Redis module context
    RedisModule_FreeThreadSafeContext(ctx);

    return NULL;
}

int do_bg_rm_call(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    UNUSED(argv);
    UNUSED(argc);

    /* Make sure we're not trying to block a client when we shouldn't */
    int flags = RedisModule_GetContextFlags(ctx);
    int allFlags = RedisModule_GetContextFlagsAll();
    if ((allFlags & REDISMODULE_CTX_FLAGS_MULTI) &&
        (flags & REDISMODULE_CTX_FLAGS_MULTI)) {
        RedisModule_ReplyWithSimpleString(ctx, "Blocked client is not supported inside multi");
        return REDISMODULE_OK;
    }
    if ((allFlags & REDISMODULE_CTX_FLAGS_DENY_BLOCKING) &&
        (flags & REDISMODULE_CTX_FLAGS_DENY_BLOCKING)) {
        RedisModule_ReplyWithSimpleString(ctx, "Blocked client is not allowed");
        return REDISMODULE_OK;
    }

    /* Make a copy of the arguments and pass them to the thread. */
    bg_call_data *bg = RedisModule_Alloc(sizeof(bg_call_data));
    bg->argv = RedisModule_Alloc(sizeof(RedisModuleString*)*argc);
    bg->argc = argc;
    for (int i=0; i<argc; i++)
        bg->argv[i] = RedisModule_HoldString(ctx, argv[i]);

    /* Block the client */
    bg->bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);

    /* Start a thread to handle the request */
    pthread_t tid;
    int res = pthread_create(&tid, NULL, bg_call_worker, bg);
    assert(res == 0);

    return REDISMODULE_OK;
}

int do_rm_call(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    UNUSED(argv);
    UNUSED(argc);

    if(argc < 2){
        return RedisModule_WrongArity(ctx);
    }

    const char* cmd = RedisModule_StringPtrLen(argv[1], NULL);

    RedisModuleCallReply* rep = RedisModule_Call(ctx, cmd, "v", argv + 2, argc - 2);
    if(!rep){
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    }else{
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }

    return REDISMODULE_OK;
}


int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "blockedclient", 1, REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "acquire_gil", acquire_gil, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "do_rm_call", do_rm_call, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "do_bg_rm_call", do_bg_rm_call, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

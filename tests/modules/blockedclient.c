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

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "blockedclient", 1, REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "acquire_gil", acquire_gil, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

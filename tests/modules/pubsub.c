/*
 * This module is used to test the pubsub API.
 *
 * Created by Harkrishn Patro.
 * -----------------------------------------------------------------------------
*/
#define REDISMODULE_EXPERIMENTAL_API

#include "redismodule.h"

int Pubsub_Publish(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModuleString *channel = RedisModule_CreateString(ctx, "universe", 8);
    RedisModuleString *message = RedisModule_CreateString(ctx, "42", 2);

    int count = RedisModule_PublishMessage(ctx, channel, message);
    RedisModule_FreeString(ctx, channel);
    RedisModule_FreeString(ctx, message);
    return RedisModule_ReplyWithLongLong(ctx, count);
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "pubsub", 1, REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"pubsub.publish",
        Pubsub_Publish,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}


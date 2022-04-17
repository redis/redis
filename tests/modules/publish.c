#include "redismodule.h"
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define UNUSED(V) ((void) V)

int cmd_publish_classic(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 3)
        return RedisModule_WrongArity(ctx);
    
    int receivers = RedisModule_PublishMessage(ctx, argv[1], argv[2]);
    RedisModule_ReplyWithLongLong(ctx, receivers);
    return REDISMODULE_OK;
}

int cmd_publish_shard(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 3)
        return RedisModule_WrongArity(ctx);
    
    int receivers = RedisModule_PublishMessageShard(ctx, argv[1], argv[2]);
    RedisModule_ReplyWithLongLong(ctx, receivers);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    
    if (RedisModule_Init(ctx,"publish",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"publish.classic",cmd_publish_classic,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"publish.shard",cmd_publish_shard,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
        
    return REDISMODULE_OK;
}

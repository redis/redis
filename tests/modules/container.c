#include "redismodule.h"

#define UNUSED(V) ((void) V)

int cmd_config_set(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int cmd_config_get(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "container", 1, REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"container.config",NULL,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateSubcommand(ctx,"set",cmd_config_set,"",0,0,0,"container.config") == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateSubcommand(ctx,"get",cmd_config_get,"",0,0,0,"container.config") == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* Sanity */
    RedisModule_Assert(RedisModule_CreateSubcommand(ctx,"get",NULL,"",0,0,0,"config") == REDISMODULE_ERR);
    RedisModule_Assert(RedisModule_CreateSubcommand(ctx,"get",NULL,"",0,0,0,"container.config") == REDISMODULE_ERR);
    RedisModule_Assert(RedisModule_CreateSubcommand(ctx,"get",NULL,"",0,0,0,"container.config|get") == REDISMODULE_ERR);

    return REDISMODULE_OK;
}

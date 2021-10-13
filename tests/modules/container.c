#include "redismodule.h"

#define UNUSED(V) ((void) V)

int cmd_set(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int cmd_get(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    int spec_id;

    if (RedisModule_Init(ctx, "container", 1, REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"container.bitarray",NULL,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateSubcommand(ctx,"set",cmd_set,"",0,0,0,"container.bitarray") == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_AddCommandKeySpec(ctx,"container.bitarray|set","write",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(ctx,"container.bitarray|set",spec_id,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(ctx,"container.bitarray|set",spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateSubcommand(ctx,"get",cmd_get,"",0,0,0,"container.bitarray") == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_AddCommandKeySpec(ctx,"container.bitarray|get","read",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(ctx,"container.bitarray|get",spec_id,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(ctx,"container.bitarray|get",spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* Sanity */
    RedisModule_Assert(RedisModule_CreateSubcommand(ctx,"get",NULL,"",0,0,0,"bitarray") == REDISMODULE_ERR);
    RedisModule_Assert(RedisModule_CreateSubcommand(ctx,"get",NULL,"",0,0,0,"container.bitarray") == REDISMODULE_ERR);
    RedisModule_Assert(RedisModule_CreateSubcommand(ctx,"get",NULL,"",0,0,0,"container.bitarray|get") == REDISMODULE_ERR);

    return REDISMODULE_OK;
}

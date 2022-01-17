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

    if (RedisModule_Init(ctx, "subcommands", 1, REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"subcommands.bitarray",NULL,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    RedisModuleCommand *parent = RedisModule_GetCommand(ctx,"subcommands.bitarray");

    if (RedisModule_CreateSubcommand(parent,"set",cmd_set,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    RedisModuleCommand *subcmd = RedisModule_GetCommand(ctx,"subcommands.bitarray|set");

    if (RedisModule_AddCommandKeySpec(subcmd,"RW UPDATE",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(subcmd,spec_id,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(subcmd,spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateSubcommand(parent,"get",cmd_get,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    subcmd = RedisModule_GetCommand(ctx,"subcommands.bitarray|get");

    if (RedisModule_AddCommandKeySpec(subcmd,"RO ACCESS",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(subcmd,spec_id,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(subcmd,spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* Sanity */

    /* Trying to create the same subcommand fails */
    RedisModule_Assert(RedisModule_CreateSubcommand(parent,"get",NULL,"",0,0,0) == REDISMODULE_ERR);

    /* Trying to create a sub-subcommand fails */
    RedisModule_Assert(RedisModule_CreateSubcommand(subcmd,"get",NULL,"",0,0,0) == REDISMODULE_ERR);

    return REDISMODULE_OK;
}

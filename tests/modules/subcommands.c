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

int cmd_get_fullname(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    const char *command_name = RedisModule_GetCurrentCommandName(ctx);
    RedisModule_ReplyWithSimpleString(ctx, command_name);
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

#ifdef INCLUDE_UNRELEASED_KEYSPEC_API
    if (RedisModule_AddCommandKeySpec(subcmd,"RW UPDATE",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(subcmd,spec_id,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(subcmd,spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
#endif

    if (RedisModule_CreateSubcommand(parent,"get",cmd_get,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    subcmd = RedisModule_GetCommand(ctx,"subcommands.bitarray|get");

#ifdef INCLUDE_UNRELEASED_KEYSPEC_API
    if (RedisModule_AddCommandKeySpec(subcmd,"RO ACCESS",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(subcmd,spec_id,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(subcmd,spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
#endif

    /* Get the name of the command currently running. */
    if (RedisModule_CreateCommand(ctx,"subcommands.parent_get_fullname",cmd_get_fullname,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* Get the name of the subcommand currently running. */
    if (RedisModule_CreateCommand(ctx,"subcommands.sub",NULL,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModuleCommand *fullname_parent = RedisModule_GetCommand(ctx,"subcommands.sub");
    if (RedisModule_CreateSubcommand(fullname_parent,"get_fullname",cmd_get_fullname,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* Sanity */

    /* Trying to create the same subcommand fails */
    RedisModule_Assert(RedisModule_CreateSubcommand(parent,"get",NULL,"",0,0,0) == REDISMODULE_ERR);

    /* Trying to create a sub-subcommand fails */
    RedisModule_Assert(RedisModule_CreateSubcommand(subcmd,"get",NULL,"",0,0,0) == REDISMODULE_ERR);

    return REDISMODULE_OK;
}

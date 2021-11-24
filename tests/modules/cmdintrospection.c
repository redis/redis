#include "redismodule.h"

#define UNUSED(V) ((void) V)

int cmd_xadd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    int spec_id;

    if (RedisModule_Init(ctx, "cmdintrospection", 1, REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"cmdintrospection.xadd",NULL,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    RedisModuleCommandProxy *xadd = RedisModule_GetCommandProxy(ctx,"cmdintrospection.xadd");

    RedisModule_AppendCommandHistoryEntry(xadd, "6.2", "Added the NOMKSTREAM option, MINID trimming strategy and the LIMIT option");

    return REDISMODULE_OK;
}

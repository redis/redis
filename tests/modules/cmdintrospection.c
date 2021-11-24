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

    RedisModule_SetCommandArity(xadd, -5);
    RedisModule_SetCommandSummary(xadd, "Appends a new entry to a stream");
    RedisModule_SetCommandDebutVersion(xadd, "5.0.0");
    RedisModule_SetCommandComplexity(xadd, "O(1) when adding a new entry, O(N) when trimming where N being the number of entries evicted.");
    RedisModule_AppendCommandHistoryEntry(xadd, "6.2", "Added the NOMKSTREAM option, MINID trimming strategy and the LIMIT option");
    RedisModule_SetCommandHints(xadd, "hint1 hint2 hint3");

    return REDISMODULE_OK;
}

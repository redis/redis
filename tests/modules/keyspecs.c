#include "redismodule.h"

#define UNUSED(V) ((void) V)

int kspec_legacy(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int kspec_complex1(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int kspec_complex2(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "keyspecs", 1, REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* Test legacy range "gluing" */
    if (RedisModule_CreateCommand(ctx,"kspec.legacy",kspec_legacy,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_AddCommandKeySpecRange(ctx,"kspec.legacy","read",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_AddCommandKeySpecRange(ctx,"kspec.legacy","write",2,2,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* First is legacy, rest are new specs */
    if (RedisModule_CreateCommand(ctx,"kspec.complex1",kspec_complex1,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_AddCommandKeySpecRange(ctx,"kspec.complex1","read",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_AddCommandKeySpecKeyword(ctx,"kspec.complex1","read","KEYS",1,2,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* First is not legacy, more than STATIC_KEYS_SPECS_NUM specs */
    if (RedisModule_CreateCommand(ctx,"kspec.complex2",kspec_complex2,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_AddCommandKeySpecKeyword(ctx,"kspec.complex2","read","KEYS",1,2,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_AddCommandKeySpecRange(ctx,"kspec.complex2","read",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_AddCommandKeySpecRange(ctx,"kspec.complex2","write",2,2,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_AddCommandKeySpecKeynum(ctx,"kspec.complex2","write",3,4,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_AddCommandKeySpecKeyword(ctx,"kspec.complex2","read","MOREKEYS",1,2,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

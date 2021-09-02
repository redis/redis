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

    int spec_id;

    if (RedisModule_Init(ctx, "keyspecs", 1, REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* Test legacy range "gluing" */
    if (RedisModule_CreateCommand(ctx,"kspec.legacy",kspec_legacy,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_AddCommandKeySpec(ctx,"kspec.legacy","read",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(ctx,"kspec.legacy",spec_id,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(ctx,"kspec.legacy",spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_AddCommandKeySpec(ctx,"kspec.legacy","write",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(ctx,"kspec.legacy",spec_id,2) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(ctx,"kspec.legacy",spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* First is legacy, rest are new specs */
    if (RedisModule_CreateCommand(ctx,"kspec.complex1",kspec_complex1,"",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_AddCommandKeySpec(ctx,"kspec.complex1","write",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchKeyword(ctx,"kspec.complex1",spec_id,"STORE",2) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(ctx,"kspec.complex1",spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_AddCommandKeySpec(ctx,"kspec.complex1","read",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchKeyword(ctx,"kspec.complex1",spec_id,"KEYS",2) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysKeynum(ctx,"kspec.complex1",spec_id,0,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* First is not legacy, more than STATIC_KEYS_SPECS_NUM specs */
    if (RedisModule_CreateCommand(ctx,"kspec.complex2",kspec_complex2,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_AddCommandKeySpec(ctx,"kspec.complex2","write",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchKeyword(ctx,"kspec.complex2",spec_id,"STORE",5) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(ctx,"kspec.complex2",spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_AddCommandKeySpec(ctx,"kspec.complex2","read",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(ctx,"kspec.complex2",spec_id,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(ctx,"kspec.complex2",spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_AddCommandKeySpec(ctx,"kspec.complex2","read",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(ctx,"kspec.complex2",spec_id,2) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(ctx,"kspec.complex2",spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_AddCommandKeySpec(ctx,"kspec.complex2","write",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(ctx,"kspec.complex2",spec_id,3) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysKeynum(ctx,"kspec.complex2",spec_id,0,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_AddCommandKeySpec(ctx,"kspec.complex2","write",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchKeyword(ctx,"kspec.complex2",spec_id,"MOREKEYS",5) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(ctx,"kspec.complex2",spec_id,-1,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

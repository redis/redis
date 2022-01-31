#include "redismodule.h"
#ifdef INCLUDE_UNRELEASED_KEYSPEC_API

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
    RedisModuleCommand *legacy = RedisModule_GetCommand(ctx,"kspec.legacy");

    if (RedisModule_AddCommandKeySpec(legacy,"RO ACCESS",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(legacy,spec_id,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(legacy,spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_AddCommandKeySpec(legacy,"RW UPDATE",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(legacy,spec_id,2) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(legacy,spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* First is legacy, rest are new specs */
    if (RedisModule_CreateCommand(ctx,"kspec.complex1",kspec_complex1,"",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    RedisModuleCommand *complex1 = RedisModule_GetCommand(ctx,"kspec.complex1");

    if (RedisModule_AddCommandKeySpec(complex1,"RW UPDATE",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchKeyword(complex1,spec_id,"STORE",2) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(complex1,spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_AddCommandKeySpec(complex1,"RO ACCESS",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchKeyword(complex1,spec_id,"KEYS",2) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysKeynum(complex1,spec_id,0,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* First is not legacy, more than STATIC_KEYS_SPECS_NUM specs */
    if (RedisModule_CreateCommand(ctx,"kspec.complex2",kspec_complex2,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    RedisModuleCommand *complex2 = RedisModule_GetCommand(ctx,"kspec.complex2");

    if (RedisModule_AddCommandKeySpec(complex2,"RW UPDATE",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchKeyword(complex2,spec_id,"STORE",5) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(complex2,spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_AddCommandKeySpec(complex2,"RO ACCESS",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(complex2,spec_id,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(complex2,spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_AddCommandKeySpec(complex2,"RO ACCESS",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(complex2,spec_id,2) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(complex2,spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_AddCommandKeySpec(complex2,"RW UPDATE",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(complex2,spec_id,3) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysKeynum(complex2,spec_id,0,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_AddCommandKeySpec(complex2,"RW UPDATE",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchKeyword(complex2,spec_id,"MOREKEYS",5) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(complex2,spec_id,-1,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
#endif

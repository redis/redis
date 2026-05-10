#include "redismodule.h"
#include <stdlib.h>
#include <string.h>

int HashNull_NormalGet(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    if (!key || RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_HASH)
        return RedisModule_ReplyWithError(ctx, "not a hash");
    RedisModuleString *val = NULL;
    RedisModule_HashGet(key, REDISMODULE_HASH_NONE, argv[2], &val, NULL);
    if (val) RedisModule_ReplyWithString(ctx, val);
    else RedisModule_ReplyWithNull(ctx);
    return REDISMODULE_OK;
}

int HashNull_GetFromLongLong(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    if (!key || RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_HASH)
        return RedisModule_ReplyWithError(ctx, "not a hash");

    long long field_num;
    if (RedisModule_StringToLongLong(argv[2], &field_num) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "invalid field number");

    RedisModuleString *field = RedisModule_CreateStringFromLongLong(ctx, field_num);
    RedisModuleString *val = NULL;
    RedisModule_HashGet(key, REDISMODULE_HASH_NONE, field, &val, NULL);
    if (val) RedisModule_ReplyWithString(ctx, val);
    else RedisModule_ReplyWithNull(ctx);
    return REDISMODULE_OK;
}

int HashNull_GetFromDouble(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    if (!key || RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_HASH)
        return RedisModule_ReplyWithError(ctx, "not a hash");

    double field_num;
    if (RedisModule_StringToDouble(argv[2], &field_num) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "invalid field double");

    RedisModuleString *field = RedisModule_CreateStringFromDouble(ctx, field_num);
    RedisModuleString *val = NULL;
    RedisModule_HashGet(key, REDISMODULE_HASH_NONE, field, &val, NULL);
    if (val) RedisModule_ReplyWithString(ctx, val);
    else RedisModule_ReplyWithNull(ctx);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    (void) argv;
    (void) argc;
    if (RedisModule_Init(ctx, "hashnull", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "hashnull.normalget", HashNull_NormalGet,
        "readonly",0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "hashnull.getfromll", HashNull_GetFromLongLong,
        "readonly",0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "hashnull.getfromdouble", HashNull_GetFromDouble,
        "readonly",0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

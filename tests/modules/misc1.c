#define REDISMODULE_EXPERIMENTAL_API
#include "redismodule.h"

#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>

#define UNUSED(x) (void)(x)

RedisModuleKey *open_key_or_reply(RedisModuleCtx *ctx, RedisModuleString *keyname, int mode);

int test_getlru(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc<2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }
    RedisModuleKey *key = open_key_or_reply(ctx, argv[1], REDISMODULE_READ|REDISMODULE_OPEN_KEY_NOTOUCH);
    mstime_t lru;
    RedisModule_GetLRU(key, &lru);
    RedisModule_ReplyWithLongLong(ctx, lru);
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

int test_setlru(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc<3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }
    RedisModuleKey *key = open_key_or_reply(ctx, argv[1], REDISMODULE_READ|REDISMODULE_OPEN_KEY_NOTOUCH);
    mstime_t lru;
    if (RedisModule_StringToLongLong(argv[2], &lru) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "invalid idle time");
        return REDISMODULE_OK;
    }
    int was_set = RedisModule_SetLRU(key, lru)==REDISMODULE_OK;
    RedisModule_ReplyWithLongLong(ctx, was_set);
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

int test_getlfu(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc<2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }
    RedisModuleKey *key = open_key_or_reply(ctx, argv[1], REDISMODULE_READ|REDISMODULE_OPEN_KEY_NOTOUCH);
    mstime_t lfu;
    RedisModule_GetLFU(key, &lfu);
    RedisModule_ReplyWithLongLong(ctx, lfu);
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

int test_setlfu(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc<3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }
    RedisModuleKey *key = open_key_or_reply(ctx, argv[1], REDISMODULE_READ|REDISMODULE_OPEN_KEY_NOTOUCH);
    mstime_t lfu;
    if (RedisModule_StringToLongLong(argv[2], &lfu) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "invalid freq");
        return REDISMODULE_OK;
    }
    int was_set = RedisModule_SetLFU(key, lfu)==REDISMODULE_OK;
    RedisModule_ReplyWithLongLong(ctx, was_set);
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

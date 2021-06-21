#include "redismodule.h"

/* ZSET.REM key element
 *
 * Removes an occurrence of an element from a sorted set. Replies with the
 * number of removed elements (0 or 1).
 */
int zset_rem(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    int keymode = REDISMODULE_READ | REDISMODULE_WRITE;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], keymode);
    int deleted;
    if (RedisModule_ZsetRem(key, argv[2], &deleted) == REDISMODULE_OK)
        return RedisModule_ReplyWithLongLong(ctx, deleted);
    else
        return RedisModule_ReplyWithError(ctx, "ERR ZsetRem failed");
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "zset", 1, REDISMODULE_APIVER_1) ==
        REDISMODULE_OK &&
        RedisModule_CreateCommand(ctx, "zset.rem", zset_rem, "",
                                  1, 1, 1) == REDISMODULE_OK)
        return REDISMODULE_OK;
    else
        return REDISMODULE_ERR;
}

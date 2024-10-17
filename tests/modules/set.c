#include "redismodule.h"
#include <math.h>
#include <errno.h>

/* SET.REM key element [element ...]
 *
 * Removes elements from a set. Replies with the
 * number of removed elements.
 */
int set_rem(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    int keymode = REDISMODULE_READ | REDISMODULE_WRITE;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], keymode);
    size_t deleted;
    if (RedisModule_SetRem(key, &argv[2], argc-2, &deleted) == REDISMODULE_OK) {
        RedisModule_ReplyWithLongLong(ctx, deleted);
    } else {
        RedisModule_ReplyWithError(ctx, "ERR SetRem failed");
    }
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

/* SET.ADD key member [member ...]
 *
 * Adds members to the set stored at key. Replies with the
 * number of added elements.
 */
int set_add(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    int keymode = REDISMODULE_READ | REDISMODULE_WRITE;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], keymode);
    size_t added;
    if (RedisModule_SetAdd(key, &argv[2], argc-2, &added) == REDISMODULE_OK) {
        RedisModule_ReplyWithLongLong(ctx, added);
    } else {
        RedisModule_ReplyWithError(ctx, "ERR SetAdd failed");
    }
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

/* SET.ISMEMBER key member
 *
 * Is member of the set stored at key. Replies with 1 as member of the key
 * or 0 as not member of the key
 */
int set_ismember(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    int keymode = REDISMODULE_READ;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], keymode);
    RedisModule_ReplyWithLongLong(ctx, RedisModule_SetIsMember(key, argv[2]));
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "set", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "set.rem", set_rem, "write",
                                  1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "set.add", set_add, "write",
                                  1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "set.ismember", set_ismember, "readonly",
                                  1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

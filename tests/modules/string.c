#include "redismodule.h"

#define UNUSED(x) (void)(x)

/* STRING.SET key value */
int string_set(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3)
        return RedisModule_WrongArity(ctx);

    RedisModule_AutoMemory(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);

    int result = RedisModule_StringSet(key, argv[2]);
    return RedisModule_ReplyWithSimpleString(ctx,"OK");
}

/* STRING.GET_INTERGER key. Return the interger value if the string as an integer,
 * otherwise the ERROR "ERR not an integer" is returned.
 */
int string_get_interger(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2)
        return RedisModule_WrongArity(ctx);

    RedisModule_AutoMemory(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);

    long long value;
    int result = RedisModule_StringGetAsInteger(key, &value);
    if (result != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx, "ERR not an integer");
    }
    return RedisModule_ReplyWithLongLong(ctx, value);
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "string", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "string.set", string_set, "write", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "string.get_interger", string_get_interger,"", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
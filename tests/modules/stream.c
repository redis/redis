#include "redismodule.h"

#include <string.h>
#include <assert.h>
#include <unistd.h>

/* Command which adds a stream entry with automatic ID, like XADD *.
 *
 * Syntax: STREAM.ADD key field1 value1 [ field2 value2 ... ]
 *
 * The response is the ID of the added stream entry or an error message.
 */
int stream_add(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2 || argc % 2 != 0) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    RedisModuleStreamID id;
    if (RedisModule_StreamAdd(key, REDISMODULE_STREAM_AUTOID, &id,
                              &argv[2], (argc-2)/2) == REDISMODULE_OK) {
        RedisModuleString *id_str = RedisModule_StreamFormatID(ctx, &id);
        RedisModule_ReplyWithString(ctx, id_str);
        RedisModule_FreeString(ctx, id_str);
    } else {
        RedisModule_ReplyWithError(ctx, "ERR StreamAdd failed");
    }
    RedisModule_CloseKey(key);
    /* RedisModule_SignalKeyAsReady(ctx, argv[1]); */
    return REDISMODULE_OK;
}

/* Command which adds a stream entry N times.
 *
 * Syntax: STREAM.ADD key N field1 value1 [ field2 value2 ... ]
 *
 * Returns the number of successfully added entries.
 */
int stream_addn(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3 || argc % 2 == 0) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    long long n, i;
    if (RedisModule_StringToLongLong(argv[2], &n) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "N must be a number");
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    for (i = 0; i < n; i++) {
        if (RedisModule_StreamAdd(key, REDISMODULE_STREAM_AUTOID, NULL,
                                  &argv[3], (argc-3)/2) == REDISMODULE_ERR)
            break;
    }
    RedisModule_ReplyWithLongLong(ctx, i);
    RedisModule_CloseKey(key);
    /* if (i > 0) RedisModule_SignalKeyAsReady(ctx, argv[1]); */
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "stream", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "stream.add", stream_add, "",
                                  1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "stream.addn", stream_addn, "",
                                  1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

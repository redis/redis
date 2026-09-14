#include "redismodule.h"
#include <math.h>
#include <errno.h>
#include <strings.h>

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

/* ZSET.ADD key score member
 *
 * Adds a specified member with the specified score to the sorted
 * set stored at key.
 */
int zset_add(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    int keymode = REDISMODULE_READ | REDISMODULE_WRITE;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], keymode);

    size_t len;
    double score;
    char *endptr;
    const char *str = RedisModule_StringPtrLen(argv[2], &len);
    score = strtod(str, &endptr);
    if (*endptr != '\0' || errno == ERANGE)
        return RedisModule_ReplyWithError(ctx, "value is not a valid float");

    if (RedisModule_ZsetAdd(key, score, argv[3], NULL) == REDISMODULE_OK)
        return RedisModule_ReplyWithSimpleString(ctx, "OK");
    else
        return RedisModule_ReplyWithError(ctx, "ERR ZsetAdd failed");
}

/* ZSET.INCRBY key member increment
 *
 * Increments the score stored at member in the sorted set stored at key by increment.
 * Replies with the new score of this element.
 */
int zset_incrby(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    int keymode = REDISMODULE_READ | REDISMODULE_WRITE;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], keymode);

    size_t len;
    double score, newscore;
    char *endptr;
    const char *str = RedisModule_StringPtrLen(argv[3], &len);
    score = strtod(str, &endptr);
    if (*endptr != '\0' || errno == ERANGE)
        return RedisModule_ReplyWithError(ctx, "value is not a valid float");

    if (RedisModule_ZsetIncrby(key, score, argv[2], NULL, &newscore) == REDISMODULE_OK)
        return RedisModule_ReplyWithDouble(ctx, newscore);
    else
        return RedisModule_ReplyWithError(ctx, "ERR ZsetIncrby failed");
}

/* ZSET.RANGE key score first|last min max minex maxex
 * ZSET.RANGE key lex   first|last min max
 *
 * Exposes the module range iterator to the test suite. The reply contains
 * member, score pairs in the direction selected by first or last. */
int zset_range(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 6 && argc != 8) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);

    size_t mode_len, direction_len;
    const char *mode = RedisModule_StringPtrLen(argv[2], &mode_len);
    const char *direction = RedisModule_StringPtrLen(argv[3], &direction_len);
    int reverse;
    if (direction_len == 5 && !strncasecmp(direction, "first", 5))
        reverse = 0;
    else if (direction_len == 4 && !strncasecmp(direction, "last", 4))
        reverse = 1;
    else
        return RedisModule_ReplyWithError(ctx, "ERR invalid direction");

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int status;
    if (mode_len == 5 && !strncasecmp(mode, "score", 5) && argc == 8) {
        double min, max;
        long long minex, maxex;
        if (RedisModule_StringToDouble(argv[4], &min) == REDISMODULE_ERR ||
            RedisModule_StringToDouble(argv[5], &max) == REDISMODULE_ERR ||
            RedisModule_StringToLongLong(argv[6], &minex) == REDISMODULE_ERR ||
            RedisModule_StringToLongLong(argv[7], &maxex) == REDISMODULE_ERR ||
            (minex != 0 && minex != 1) || (maxex != 0 && maxex != 1))
            return RedisModule_ReplyWithError(ctx, "ERR invalid score range");
        status = reverse ?
            RedisModule_ZsetLastInScoreRange(key, min, max, minex, maxex) :
            RedisModule_ZsetFirstInScoreRange(key, min, max, minex, maxex);
    } else if (mode_len == 3 && !strncasecmp(mode, "lex", 3) && argc == 6) {
        status = reverse ?
            RedisModule_ZsetLastInLexRange(key, argv[4], argv[5]) :
            RedisModule_ZsetFirstInLexRange(key, argv[4], argv[5]);
    } else {
        return RedisModule_ReplyWithError(ctx, "ERR invalid range mode");
    }
    if (status == REDISMODULE_ERR)
        return RedisModule_ReplyWithError(ctx, "ERR range setup failed");

    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
    long count = 0;
    while (!RedisModule_ZsetRangeEndReached(key)) {
        double score;
        RedisModuleString *member =
            RedisModule_ZsetRangeCurrentElement(key, &score);
        if (member == NULL) break;
        RedisModule_ReplyWithString(ctx, member);
        RedisModule_ReplyWithDouble(ctx, score);
        RedisModule_FreeString(ctx, member);
        count += 2;
        if (reverse)
            RedisModule_ZsetRangePrev(key);
        else
            RedisModule_ZsetRangeNext(key);
    }
    RedisModule_ZsetRangeStop(key);
    RedisModule_ReplySetArrayLength(ctx, count);
    return REDISMODULE_OK;
}

/* Invalidate the rank saved by a B+ tree range iterator. The iterator API
 * should report the end of the range rather than aborting Redis. */
int zset_range_mutate(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1],
                                              REDISMODULE_READ | REDISMODULE_WRITE);
    if (RedisModule_ZsetLastInScoreRange(key, REDISMODULE_NEGATIVE_INFINITE,
                                         REDISMODULE_POSITIVE_INFINITE, 0, 0) == REDISMODULE_ERR)
        return RedisModule_ReplyWithError(ctx, "ERR range setup failed");

    /* Keep one member so the open key handle itself remains valid. */
    RedisModuleCallReply *reply = RedisModule_Call(ctx, "ZREMRANGEBYRANK", "sll",
                                                   argv[1], (long long)0, (long long)-2);
    if (reply == NULL || RedisModule_CallReplyType(reply) == REDISMODULE_REPLY_ERROR)
        return RedisModule_ReplyWithError(ctx, "ERR range mutation failed");

    RedisModuleString *member = RedisModule_ZsetRangeCurrentElement(key, NULL);
    if (member != NULL || RedisModule_ZsetRangePrev(key) != 0 ||
        !RedisModule_ZsetRangeEndReached(key))
        return RedisModule_ReplyWithError(ctx, "ERR invalid iterator state");

    RedisModule_ZsetRangeStop(key);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* Structure to hold data for the delall scan callback */
typedef struct {
    RedisModuleCtx *ctx;
    RedisModuleString **keys_to_delete;
    size_t keys_capacity;
    size_t keys_count;
} zset_delall_data;

/* Callback function for scanning keys and collecting zset keys to delete */
void zset_delall_callback(RedisModuleCtx *ctx, RedisModuleString *keyname, RedisModuleKey *key, void *privdata) {
    zset_delall_data *data = privdata;
    int was_opened = 0;

    /* Open the key if it wasn't already opened */
    if (!key) {
        key = RedisModule_OpenKey(ctx, keyname, REDISMODULE_READ);
        was_opened = 1;
    }

    /* Check if the key is a zset and add it to the list */
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_ZSET) {
        /* Expand the array if needed */
        if (data->keys_count >= data->keys_capacity) {
            data->keys_capacity = data->keys_capacity ? data->keys_capacity * 2 : 16;
            data->keys_to_delete = RedisModule_Realloc(data->keys_to_delete,
                                                       data->keys_capacity * sizeof(RedisModuleString*));
        }

        /* Store the key name (retain it so it doesn't get freed) */
        data->keys_to_delete[data->keys_count] = keyname;
        RedisModule_RetainString(ctx, keyname);
        data->keys_count++;
    }

    /* Close the key if we opened it */
    if (was_opened) {
        RedisModule_CloseKey(key);
    }
}

/* ZSET.DELALL
 *
 * Iterates through the keyspace and deletes all keys of type "zset".
 * Returns the number of deleted keys.
 */
int zset_delall(RedisModuleCtx *ctx, REDISMODULE_ATTR_UNUSED RedisModuleString **argv, int argc) {
    if (argc != 1) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);

    zset_delall_data data = {
        .ctx = ctx,
        .keys_to_delete = NULL,
        .keys_capacity = 0,
        .keys_count = 0
    };

    /* Create a scan cursor and iterate through all keys */
    RedisModuleScanCursor *cursor = RedisModule_ScanCursorCreate();
    while (RedisModule_Scan(ctx, cursor, zset_delall_callback, &data));
    RedisModule_ScanCursorDestroy(cursor);

    /* Delete all the collected zset keys after scan is complete */
    size_t deleted_count = 0;
    for (size_t i = 0; i < data.keys_count; i++) {
        RedisModuleCallReply *reply = RedisModule_Call(ctx, "DEL", "s!", data.keys_to_delete[i]);
        if (reply && RedisModule_CallReplyType(reply) == REDISMODULE_REPLY_INTEGER) {
            long long del_result = RedisModule_CallReplyInteger(reply);
            if (del_result > 0) {
                deleted_count++;
            }
        }
        if (reply) {
            RedisModule_FreeCallReply(reply);
        }
        RedisModule_FreeString(ctx, data.keys_to_delete[i]);
    }

    /* Free the keys array */
    if (data.keys_to_delete) {
        RedisModule_Free(data.keys_to_delete);
    }

    return RedisModule_ReplyWithLongLong(ctx, deleted_count);
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "zset", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "zset.rem", zset_rem, "write",
                                  1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "zset.add", zset_add, "write",
                                  1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "zset.incrby", zset_incrby, "write",
                                  1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "zset.range", zset_range, "readonly",
                                  1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "zset.range-mutate", zset_range_mutate, "write",
                                  1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "zset.delall", zset_delall, "write touches-arbitrary-keys",
                                  0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

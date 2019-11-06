#define REDISMODULE_EXPERIMENTAL_API
#include "redismodule.h"

#include <string.h>
#include <assert.h>
#include <unistd.h>

int notify_callback(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    const char *ckey = RedisModule_StringPtrLen(key, NULL);
    if (strcmp(ckey, "notifications") == 0) {
        /* Prevent recursion. */
        return REDISMODULE_OK;
    }
    RedisModule_Log(ctx, "notice", "Got event type %d, event %s, key %s", type, event);
    RedisModule_Call(ctx, "HINCRBY", "ccc", "notifications", event, "1");
    return REDISMODULE_OK;
}

/* NN.SET <key> <value> - Wrapper of native string API */
int nn_set(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 3)
        return RedisModule_WrongArity(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    RedisModule_StringSet(key, argv[2]);

    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* NN.TRUNCATE <key> <length> - Wrapper of native string API */
int nn_truncate(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 3)
        return RedisModule_WrongArity(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);

    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_STRING && type != REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    long long length;
    if (RedisModule_StringToLongLong(argv[2], &length) == REDISMODULE_ERR)
        return RedisModule_ReplyWithError(ctx, "Invalid length");

    RedisModule_StringTruncate(key, length);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* NN.RPUSH <key> <ele> - Wrapper of native list API */
int nn_rpush(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 3)
        return RedisModule_WrongArity(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);

    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_LIST && type != REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    RedisModule_ListPush(key, REDISMODULE_LIST_TAIL, argv[2]);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* NN.LPUSH <key> <ele> - Wrapper of native list API */
int nn_lpush(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 3)
        return RedisModule_WrongArity(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);

    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_LIST && type != REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    RedisModule_ListPush(key, REDISMODULE_LIST_HEAD, argv[2]);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* NN.RPOP <key> - Wrapper of native list API */
int nn_rpop(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 2)
        return RedisModule_WrongArity(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);

    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_LIST && type != REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    RedisModule_ListPop(key, REDISMODULE_LIST_TAIL);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* NN.LPOP <key> - Wrapper of native list API */
int nn_lpop(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 2)
        return RedisModule_WrongArity(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);

    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_LIST && type != REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    RedisModule_ListPop(key, REDISMODULE_LIST_HEAD);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* NN.ZADD <key> <score> <ele> - Wrapper of native zset API */
int nn_zadd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 4)
        return RedisModule_WrongArity(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);

    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_ZSET && type != REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    double score;
    if (RedisModule_StringToDouble(argv[2], &score) == REDISMODULE_ERR)
        return RedisModule_ReplyWithError(ctx, "Invalid score");

    RedisModule_ZsetAdd(key, score, argv[3], NULL);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* NN.ZINCRBY <key> <score> <ele> - Wrapper of native zset API */
int nn_zincrby(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 4)
        return RedisModule_WrongArity(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);

    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_ZSET && type != REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    double score;
    if (RedisModule_StringToDouble(argv[2], &score) == REDISMODULE_ERR)
        return RedisModule_ReplyWithError(ctx, "Invalid score");

    RedisModule_ZsetIncrby(key, score, argv[3], NULL, NULL);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* NN.ZREM <key> <ele> - Wrapper of native zset API */
int nn_zrem(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 3)
        return RedisModule_WrongArity(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);

    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_ZSET && type != REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    RedisModule_ZsetRem(key, argv[2], NULL);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* NN.HSET <key> <field> <value> - Wrapper of native zset API */
int nn_hset(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 4)
        return RedisModule_WrongArity(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);

    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_HASH && type != REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    RedisModule_HashSet(key, REDISMODULE_HASH_NONE, argv[2], argv[3], NULL);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* NN.HDEL <key> <field> - Wrapper of native zset API */
int nn_hdel(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 3)
        return RedisModule_WrongArity(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);

    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_HASH && type != REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    RedisModule_HashSet(key, REDISMODULE_HASH_NONE, argv[2], REDISMODULE_HASH_DELETE, NULL);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "nn", 1, REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"nn.set",nn_set,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"nn.truncate",nn_truncate,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"nn.rpush",nn_rpush,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"nn.lpush",nn_lpush,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"nn.rpop",nn_rpop,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"nn.lpop",nn_lpop,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"nn.zadd",nn_zadd,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"nn.zincrby",nn_zincrby,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"nn.zrem",nn_zrem,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"nn.hset",nn_hset,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"nn.hdel",nn_hdel,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTION_NOTIFY_NATIVE_KEYSPACE_EVENTS);

    RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_ALL, notify_callback);
    return REDISMODULE_OK;
}

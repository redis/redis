#define REDISMODULE_EXPERIMENTAL_API
#include "redismodule.h"

#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>

int test_call_generic(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc<2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    const char* cmdname = RedisModule_StringPtrLen(argv[1], NULL);
    RedisModuleCallReply *reply = RedisModule_Call(ctx, cmdname, "v", argv+2, argc-2);
    if (reply) {
        RedisModule_ReplyWithCallReply(ctx, reply);
        RedisModule_FreeCallReply(reply);
    } else {
        RedisModule_ReplyWithError(ctx, strerror(errno));
    }
    return REDISMODULE_OK;
}

int test_call_info(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisModuleCallReply *reply;
    if (argc>1)
        reply = RedisModule_Call(ctx, "info", "s", argv[1]);
    else
        reply = RedisModule_Call(ctx, "info", "");
    if (reply) {
        RedisModule_ReplyWithCallReply(ctx, reply);
        RedisModule_FreeCallReply(reply);
    } else {
        RedisModule_ReplyWithError(ctx, strerror(errno));
    }
    return REDISMODULE_OK;
}

int test_flushall(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModule_ResetDataset(1, 0);
    RedisModule_ReplyWithCString(ctx, "Ok");
    return REDISMODULE_OK;
}

int test_dbsize(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    long long ll = RedisModule_DbSize(ctx);
    RedisModule_ReplyWithLongLong(ctx, ll);
    return REDISMODULE_OK;
}

int test_randomkey(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModuleString *str = RedisModule_RandomKey(ctx);
    RedisModule_ReplyWithString(ctx, str);
    RedisModule_FreeString(ctx, str);
    return REDISMODULE_OK;
}

int test_getlru(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc<2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }
    RedisModuleString *keyname = argv[1];
    RedisModuleKey *key = RedisModule_OpenKey(ctx, keyname, REDISMODULE_READ|REDISMODULE_OPEN_KEY_NOTOUCH);
    long long lru, lfu;
    RedisModule_GetLRUOrLFU(key, &lfu, &lru);
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
    RedisModuleString *keyname = argv[1];
    RedisModuleKey *key = RedisModule_OpenKey(ctx, keyname, REDISMODULE_WRITE|REDISMODULE_OPEN_KEY_NOTOUCH);
    long long lru;
    RedisModule_StringToLongLong(argv[2], &lru);
    RedisModule_SetLRUOrLFU(key, -1, lru);
    RedisModule_ReplyWithCString(ctx, "Ok");
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx,"misc",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.call_generic", test_call_generic,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.call_info", test_call_info,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.flushall", test_flushall,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.dbsize", test_dbsize,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.randomkey", test_randomkey,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.setlru", test_setlru,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.getlru", test_getlru,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

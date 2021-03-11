#define REDISMODULE_EXPERIMENTAL_API
#include "redismodule.h"

#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>

#define UNUSED(x) (void)(x)

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

int test_ld_conv(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    long double ld = 0.00000000000000001L;
    const char *ldstr = "0.00000000000000001";
    RedisModuleString *s1 = RedisModule_CreateStringFromLongDouble(ctx, ld, 1);
    RedisModuleString *s2 =
        RedisModule_CreateString(ctx, ldstr, strlen(ldstr));
    if (RedisModule_StringCompare(s1, s2) != 0) {
        char err[4096];
        snprintf(err, 4096,
            "Failed to convert long double to string ('%s' != '%s')",
            RedisModule_StringPtrLen(s1, NULL),
            RedisModule_StringPtrLen(s2, NULL));
        RedisModule_ReplyWithError(ctx, err);
        goto final;
    }
    long double ld2 = 0;
    if (RedisModule_StringToLongDouble(s2, &ld2) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx,
            "Failed to convert string to long double");
        goto final;
    }
    if (ld2 != ld) {
        char err[4096];
        snprintf(err, 4096,
            "Failed to convert string to long double (%.40Lf != %.40Lf)",
            ld2,
            ld);
        RedisModule_ReplyWithError(ctx, err);
        goto final;
    }

    /* Make sure we can't convert a string that has \0 in it */
    char buf[4] = "123";
    buf[1] = '\0';
    RedisModuleString *s3 = RedisModule_CreateString(ctx, buf, 3);
    long double ld3;
    if (RedisModule_StringToLongDouble(s3, &ld3) == REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "Invalid string successfully converted to long double");
        RedisModule_FreeString(ctx, s3);
        goto final;
    }
    RedisModule_FreeString(ctx, s3);

    RedisModule_ReplyWithLongDouble(ctx, ld2);
final:
    RedisModule_FreeString(ctx, s1);
    RedisModule_FreeString(ctx, s2);
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

RedisModuleKey *open_key_or_reply(RedisModuleCtx *ctx, RedisModuleString *keyname, int mode) {
    RedisModuleKey *key = RedisModule_OpenKey(ctx, keyname, mode);
    if (!key) {
        RedisModule_ReplyWithError(ctx, "key not found");
        return NULL;
    }
    return key;
}

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

int test_redisversion(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    (void) argv;
    (void) argc;

    int version = RedisModule_GetServerVersion();
    int patch = version & 0x000000ff;
    int minor = (version & 0x0000ff00) >> 8;
    int major = (version & 0x00ff0000) >> 16;

    RedisModuleString* vStr = RedisModule_CreateStringPrintf(ctx, "%d.%d.%d", major, minor, patch);
    RedisModule_ReplyWithString(ctx, vStr);
    RedisModule_FreeString(ctx, vStr);
  
    return REDISMODULE_OK;
}

int test_getclientcert(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    (void) argv;
    (void) argc;

    RedisModuleString *cert = RedisModule_GetClientCertificate(ctx,
            RedisModule_GetClientId(ctx));
    if (!cert) {
        RedisModule_ReplyWithNull(ctx);
    } else {
        RedisModule_ReplyWithString(ctx, cert);
        RedisModule_FreeString(ctx, cert);
    }

    return REDISMODULE_OK;
}

int test_clientinfo(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    (void) argv;
    (void) argc;

    RedisModuleClientInfo ci = { .version = REDISMODULE_CLIENTINFO_VERSION };

    if (RedisModule_GetClientInfoById(&ci, RedisModule_GetClientId(ctx)) == REDISMODULE_ERR) {
            RedisModule_ReplyWithError(ctx, "failed to get client info");
            return REDISMODULE_OK;
    }

    RedisModule_ReplyWithArray(ctx, 10);
    char flags[512];
    snprintf(flags, sizeof(flags) - 1, "%s:%s:%s:%s:%s:%s",
        ci.flags & REDISMODULE_CLIENTINFO_FLAG_SSL ? "ssl" : "",
        ci.flags & REDISMODULE_CLIENTINFO_FLAG_PUBSUB ? "pubsub" : "",
        ci.flags & REDISMODULE_CLIENTINFO_FLAG_BLOCKED ? "blocked" : "",
        ci.flags & REDISMODULE_CLIENTINFO_FLAG_TRACKING ? "tracking" : "",
        ci.flags & REDISMODULE_CLIENTINFO_FLAG_UNIXSOCKET ? "unixsocket" : "",
        ci.flags & REDISMODULE_CLIENTINFO_FLAG_MULTI ? "multi" : "");

    RedisModule_ReplyWithCString(ctx, "flags");
    RedisModule_ReplyWithCString(ctx, flags);
    RedisModule_ReplyWithCString(ctx, "id");
    RedisModule_ReplyWithLongLong(ctx, ci.id);
    RedisModule_ReplyWithCString(ctx, "addr");
    RedisModule_ReplyWithCString(ctx, ci.addr);
    RedisModule_ReplyWithCString(ctx, "port");
    RedisModule_ReplyWithLongLong(ctx, ci.port);
    RedisModule_ReplyWithCString(ctx, "db");
    RedisModule_ReplyWithLongLong(ctx, ci.db);

    return REDISMODULE_OK;
}

int test_log_tsctx(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisModuleCtx *tsctx = RedisModule_GetDetachedThreadSafeContext(ctx);

    if (argc != 3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    char level[50];
    size_t level_len;
    const char *level_str = RedisModule_StringPtrLen(argv[1], &level_len);
    snprintf(level, sizeof(level) - 1, "%.*s", (int) level_len, level_str);

    size_t msg_len;
    const char *msg_str = RedisModule_StringPtrLen(argv[2], &msg_len);

    RedisModule_Log(tsctx, level, "%.*s", (int) msg_len, msg_str);
    RedisModule_FreeThreadSafeContext(tsctx);

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int test_aligned_alloc(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    size_t size = 100;

    for(int i = 0; i < 6; i++) {
        // Alignment, which must be a power of two and a multiple of sizeof(void *)
        int alignment = sizeof(void*) * (2 << i);
        void* ptr = RedisModule_AllocAligned(alignment, size);
        if(!ptr) {
            return REDISMODULE_ERR;
        }
        // Check  the address % alignment is 0
        if((size_t)ptr % alignment) {
            RedisModule_Free(ptr);
            return REDISMODULE_ERR;
        }
        RedisModule_Free(ptr);

    }
    RedisModule_ReplyWithSimpleString(ctx, "OK");
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
    if (RedisModule_CreateCommand(ctx,"test.ld_conversion", test_ld_conv, "",0,0,0) == REDISMODULE_ERR)
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
    if (RedisModule_CreateCommand(ctx,"test.setlfu", test_setlfu,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.getlfu", test_getlfu,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.clientinfo", test_clientinfo,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.redisversion", test_redisversion,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.getclientcert", test_getclientcert,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.log_tsctx", test_log_tsctx,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.aligned_alloc", test_aligned_alloc,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

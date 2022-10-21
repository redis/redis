#include "redismodule.h"

#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

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

int test_keyexists(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) return RedisModule_WrongArity(ctx);
    RedisModuleString *key = argv[1];
    int exists = RedisModule_KeyExists(ctx, key);
    return RedisModule_ReplyWithBool(ctx, exists);
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

    RedisModuleClientInfoV1 ci = REDISMODULE_CLIENTINFO_INITIALIZER_V1;
    uint64_t client_id = RedisModule_GetClientId(ctx);

    /* Check expected result from the V1 initializer. */
    assert(ci.version == 1);
    /* Trying to populate a future version of the struct should fail. */
    ci.version = REDISMODULE_CLIENTINFO_VERSION + 1;
    assert(RedisModule_GetClientInfoById(&ci, client_id) == REDISMODULE_ERR);

    ci.version = 1;
    if (RedisModule_GetClientInfoById(&ci, client_id) == REDISMODULE_ERR) {
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

int test_getname(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    (void)argv;
    if (argc != 1) return RedisModule_WrongArity(ctx);
    unsigned long long id = RedisModule_GetClientId(ctx);
    RedisModuleString *name = RedisModule_GetClientNameById(ctx, id);
    if (name == NULL)
        return RedisModule_ReplyWithError(ctx, "-ERR No name");
    RedisModule_ReplyWithString(ctx, name);
    RedisModule_FreeString(ctx, name);
    return REDISMODULE_OK;
}

int test_setname(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);
    unsigned long long id = RedisModule_GetClientId(ctx);
    if (RedisModule_SetClientNameById(id, argv[1]) == REDISMODULE_OK)
        return RedisModule_ReplyWithSimpleString(ctx, "OK");
    else
        return RedisModule_ReplyWithError(ctx, strerror(errno));
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

int test_weird_cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int test_monotonic_time(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_ReplyWithLongLong(ctx, RedisModule_MonotonicMicroseconds());
    return REDISMODULE_OK;
}

/* wrapper for RM_Call */
int test_rm_call(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    if(argc < 2){
        return RedisModule_WrongArity(ctx);
    }

    const char* cmd = RedisModule_StringPtrLen(argv[1], NULL);

    RedisModuleCallReply* rep = RedisModule_Call(ctx, cmd, "Ev", argv + 2, argc - 2);
    if(!rep){
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    }else{
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }

    return REDISMODULE_OK;
}

/* wrapper for RM_Call with flags */
int test_rm_call_flags(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    if(argc < 3){
        return RedisModule_WrongArity(ctx);
    }

    /* Append Ev to the provided flags. */
    RedisModuleString *flags = RedisModule_CreateStringFromString(ctx, argv[1]);
    RedisModule_StringAppendBuffer(ctx, flags, "Ev", 2);

    const char* flg = RedisModule_StringPtrLen(flags, NULL);
    const char* cmd = RedisModule_StringPtrLen(argv[2], NULL);

    RedisModuleCallReply* rep = RedisModule_Call(ctx, cmd, flg, argv + 3, argc - 3);
    if(!rep){
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    }else{
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }
    RedisModule_FreeString(ctx, flags);

    return REDISMODULE_OK;
}

int test_ull_conv(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    unsigned long long ull = 18446744073709551615ULL;
    const char *ullstr = "18446744073709551615";

    RedisModuleString *s1 = RedisModule_CreateStringFromULongLong(ctx, ull);
    RedisModuleString *s2 =
        RedisModule_CreateString(ctx, ullstr, strlen(ullstr));
    if (RedisModule_StringCompare(s1, s2) != 0) {
        char err[4096];
        snprintf(err, 4096,
            "Failed to convert unsigned long long to string ('%s' != '%s')",
            RedisModule_StringPtrLen(s1, NULL),
            RedisModule_StringPtrLen(s2, NULL));
        RedisModule_ReplyWithError(ctx, err);
        goto final;
    }
    unsigned long long ull2 = 0;
    if (RedisModule_StringToULongLong(s2, &ull2) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx,
            "Failed to convert string to unsigned long long");
        goto final;
    }
    if (ull2 != ull) {
        char err[4096];
        snprintf(err, 4096,
            "Failed to convert string to unsigned long long (%llu != %llu)",
            ull2,
            ull);
        RedisModule_ReplyWithError(ctx, err);
        goto final;
    }
    
    /* Make sure we can't convert a string more than ULLONG_MAX or less than 0 */
    ullstr = "18446744073709551616";
    RedisModuleString *s3 = RedisModule_CreateString(ctx, ullstr, strlen(ullstr));
    unsigned long long ull3;
    if (RedisModule_StringToULongLong(s3, &ull3) == REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "Invalid string successfully converted to unsigned long long");
        RedisModule_FreeString(ctx, s3);
        goto final;
    }
    RedisModule_FreeString(ctx, s3);
    ullstr = "-1";
    RedisModuleString *s4 = RedisModule_CreateString(ctx, ullstr, strlen(ullstr));
    unsigned long long ull4;
    if (RedisModule_StringToULongLong(s4, &ull4) == REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "Invalid string successfully converted to unsigned long long");
        RedisModule_FreeString(ctx, s4);
        goto final;
    }
    RedisModule_FreeString(ctx, s4);
   
    RedisModule_ReplyWithSimpleString(ctx, "ok");

final:
    RedisModule_FreeString(ctx, s1);
    RedisModule_FreeString(ctx, s2);
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
    if (RedisModule_CreateCommand(ctx,"test.ull_conversion", test_ull_conv, "",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.flushall", test_flushall,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.dbsize", test_dbsize,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.randomkey", test_randomkey,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.keyexists", test_keyexists,"",1,1,1) == REDISMODULE_ERR)
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
    if (RedisModule_CreateCommand(ctx,"test.getname", test_getname,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.setname", test_setname,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.redisversion", test_redisversion,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.getclientcert", test_getclientcert,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.log_tsctx", test_log_tsctx,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    /* Add a command with ':' in it's name, so that we can check commandstats sanitization. */
    if (RedisModule_CreateCommand(ctx,"test.weird:cmd", test_weird_cmd,"readonly",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.monotonic_time", test_monotonic_time,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "test.rm_call", test_rm_call,"allow-stale", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "test.rm_call_flags", test_rm_call_flags,"allow-stale", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

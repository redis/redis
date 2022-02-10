#include "redismodule.h"

#include <string.h>
#include <assert.h>
#include <unistd.h>

typedef struct {
    size_t nkeys;
} scan_strings_pd;

void scan_strings_callback(RedisModuleCtx *ctx, RedisModuleString* keyname, RedisModuleKey* key, void *privdata) {
    scan_strings_pd* pd = privdata;
    int was_opened = 0;
    if (!key) {
        key = RedisModule_OpenKey(ctx, keyname, REDISMODULE_READ);
        was_opened = 1;
    }

    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_STRING) {
        size_t len;
        char * data = RedisModule_StringDMA(key, &len, REDISMODULE_READ);
        RedisModule_ReplyWithArray(ctx, 2);
        RedisModule_ReplyWithString(ctx, keyname);
        RedisModule_ReplyWithStringBuffer(ctx, data, len);
        pd->nkeys++;
    }
    if (was_opened)
        RedisModule_CloseKey(key);
}

int scan_strings(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    scan_strings_pd pd = {
        .nkeys = 0,
    };

    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_LEN);

    RedisModuleScanCursor* cursor = RedisModule_ScanCursorCreate();
    while(RedisModule_Scan(ctx, cursor, scan_strings_callback, &pd));
    RedisModule_ScanCursorDestroy(cursor);

    RedisModule_ReplySetArrayLength(ctx, pd.nkeys);
    return REDISMODULE_OK;
}

typedef struct {
    RedisModuleCtx *ctx;
    size_t nreplies;
} scan_key_pd;

void scan_key_callback(RedisModuleKey *key, RedisModuleString* field, RedisModuleString* value, void *privdata) {
    REDISMODULE_NOT_USED(key);
    scan_key_pd* pd = privdata;
    RedisModule_ReplyWithArray(pd->ctx, 2);
    size_t fieldCStrLen;

    // The implementation of RedisModuleString is robj with lots of encodings.
    // We want to make sure the robj that passes to this callback in
    // String encoded, this is why we use RedisModule_StringPtrLen and
    // RedisModule_ReplyWithStringBuffer instead of directly use
    // RedisModule_ReplyWithString.
    const char* fieldCStr = RedisModule_StringPtrLen(field, &fieldCStrLen);
    RedisModule_ReplyWithStringBuffer(pd->ctx, fieldCStr, fieldCStrLen);
    if(value){
        size_t valueCStrLen;
        const char* valueCStr = RedisModule_StringPtrLen(value, &valueCStrLen);
        RedisModule_ReplyWithStringBuffer(pd->ctx, valueCStr, valueCStrLen);
    } else {
        RedisModule_ReplyWithNull(pd->ctx);
    }

    pd->nreplies++;
}

int scan_key(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }
    scan_key_pd pd = {
        .ctx = ctx,
        .nreplies = 0,
    };

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    if (!key) {
        RedisModule_ReplyWithError(ctx, "not found");
        return REDISMODULE_OK;
    }

    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

    RedisModuleScanCursor* cursor = RedisModule_ScanCursorCreate();
    while(RedisModule_ScanKey(key, cursor, scan_key_callback, &pd));
    RedisModule_ScanCursorDestroy(cursor);

    RedisModule_ReplySetArrayLength(ctx, pd.nreplies);
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "scan", 1, REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "scan.scan_strings", scan_strings, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "scan.scan_key", scan_key, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}



#define REDISMODULE_EXPERIMENTAL_API
#include "redismodule.h"

#include <string.h>
#include <assert.h>
#include <unistd.h>

#define UNUSED(V) ((void) V)

typedef struct scan_pd{
        size_t nkeys;
        RedisModuleCtx *ctx;
} scan_pd;

void scan_callback(void *privdata, RedisModuleString* keyname, RedisModuleKey* key){
    scan_pd* pd = privdata;
    RedisModule_ReplyWithArray(pd->ctx, 2);

    RedisModule_ReplyWithString(pd->ctx, keyname);
    if(key && RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_STRING){
        size_t len;
        char * data = RedisModule_StringDMA(key, &len, REDISMODULE_READ);
        RedisModule_ReplyWithStringBuffer(pd->ctx, data, len);
    }else{
        RedisModule_ReplyWithNull(pd->ctx);
    }
    pd->nkeys++;
}

int scan_keys_values(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    scan_pd pd = {
            .nkeys = 0,
            .ctx = ctx,
    };

    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

    RedisModuleCursor* cursor = RedisModule_CursorCreate();
    while(RedisModule_Scan(ctx, cursor, scan_callback, &pd));
    RedisModule_CursorDestroy(cursor);

    RedisModule_ReplySetArrayLength(ctx, pd.nkeys);
    return 0;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    if (RedisModule_Init(ctx, "scan", 1, REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "scan.scankeysvalues", scan_keys_values, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}






#include "redismodule.h"

#include <string.h>

void InfoFunc(RedisModuleInfoCtx *ctx, int for_crash_report) {
    RedisModule_InfoAddSection(ctx, "");
    RedisModule_InfoAddFieldLongLong(ctx, "global", -2);

    RedisModule_InfoAddSection(ctx, "Spanish");
    RedisModule_InfoAddFieldCString(ctx, "uno", "one");
    RedisModule_InfoAddFieldLongLong(ctx, "dos", 2);

    RedisModule_InfoAddSection(ctx, "Italian");
    RedisModule_InfoAddFieldLongLong(ctx, "due", 2);
    RedisModule_InfoAddFieldDouble(ctx, "tre", 3.3);

    RedisModule_InfoAddSection(ctx, "keyspace");
    RedisModule_InfoBeginDictField(ctx, "db0");
    RedisModule_InfoAddFieldLongLong(ctx, "keys", 3);
    RedisModule_InfoAddFieldLongLong(ctx, "expires", 1);
    RedisModule_InfoEndDictField(ctx);

    if (for_crash_report) {
        RedisModule_InfoAddSection(ctx, "Klingon");
        RedisModule_InfoAddFieldCString(ctx, "one", "wa’");
        RedisModule_InfoAddFieldCString(ctx, "two", "cha’");
        RedisModule_InfoAddFieldCString(ctx, "three", "wej");
    }

}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx,"infotest",1,REDISMODULE_APIVER_1)
            == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_RegisterInfoFunc(ctx, InfoFunc) == REDISMODULE_ERR) return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

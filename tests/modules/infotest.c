#include "redismodule.h"

#include <string.h>

void InfoFunc(RedisModuleInfoCtx *ctx, int for_crash_report) {
    RedisModule_AddInfoSection(ctx, "Spanish");
    RedisModule_AddInfoFieldCString(ctx, "uno", "one");
    RedisModule_AddInfoFieldLongLong(ctx, "dos", 2);

    RedisModule_AddInfoSection(ctx, "Italian");
    RedisModule_AddInfoFieldLongLong(ctx, "due", 2);
    RedisModule_AddInfoFieldDouble(ctx, "tre", 3.3);

    if (for_crash_report) {
        RedisModule_AddInfoSection(ctx, "Klingon");
        RedisModule_AddInfoFieldCString(ctx, "one", "wa’");
        RedisModule_AddInfoFieldCString(ctx, "two", "cha’");
        RedisModule_AddInfoFieldCString(ctx, "three", "wej");
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

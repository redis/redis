#include "redismodule.h"
#include <strings.h>

/* Second module configs module, for testing.
 * Need to make sure that multiple modules with configs don't interfere with each other */
int bool_config = 1;

int getBoolConfigCommand(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(privdata);
    if (!strcasecmp(name, "test")) {
        return bool_config;
    }
    return 0;
}

int setBoolConfigCommand(const char *name, int new, void *privdata, RedisModuleConfigSetContext is_startup, const char **err) {
    REDISMODULE_NOT_USED(privdata);
    REDISMODULE_NOT_USED(err);
    REDISMODULE_NOT_USED(is_startup);
    if (!strcasecmp(name, "test")) {
        bool_config = new;
        return 1;
    }
    return 0;
}

/* No arguments are expected */ 
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "configs", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_RegisterBoolConfig(ctx, "test", REDISMODULE_CONFIG_MODIFIABLE, &getBoolConfigCommand, &setBoolConfigCommand, &argc) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    if (RedisModule_ApplyConfigs(ctx) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}
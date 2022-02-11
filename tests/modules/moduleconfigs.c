#include "redismodule.h"
int mutable_bool_val = 1;
int immutable_bool_val = 0;
long long longval = -1;
long long memval = 1024;
RedisModuleString *strval;
RedisModuleString *enumval;

/* Series of get and set callbacks for each type of config, these rely on the privdata ptr
 * to point to the config, and they register the configs as such. Note that one could also just
 * use names if they wanted, and store anything in privdata. */
int getBoolConfigCommand(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(name);
    return (*(int *)privdata);
}

int setBoolConfigCommand(const char *name, int new, void *privdata, RedisModuleConfigSetContext is_startup, const char **err) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(err);
    REDISMODULE_NOT_USED(is_startup);
    *(int *)privdata = new;
    return 1;
}

long long getNumericConfigCommand(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(name);
    return (*(long long *) privdata);
}

int setNumericConfigCommand(const char *name, long long new, void *privdata, RedisModuleConfigSetContext is_startup, const char **err) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(err);
    REDISMODULE_NOT_USED(is_startup);
    *(long long *)privdata = new;
    return 1;
}

RedisModuleString *getStringConfigCommand(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(name);
    return (*(RedisModuleString **) privdata);
}
int setStringConfigCommand(const char *name, RedisModuleString *new, void *privdata, RedisModuleConfigSetContext is_startup, const char **err) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(err);
    REDISMODULE_NOT_USED(is_startup);
    *(RedisModuleString **)privdata = new;
    return 1;
}

RedisModuleString *getEnumConfigCommand(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(name);
    return (*(RedisModuleString **) privdata);
}

int setEnumConfigCommand(const char *name, RedisModuleString *new, void *privdata, RedisModuleConfigSetContext is_startup, const char **err) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(err);
    REDISMODULE_NOT_USED(is_startup);
    *(RedisModuleString **)privdata = new;
    return 1;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "moduleconfigs", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) return REDISMODULE_ERR;

    strval = RedisModule_CreateString(ctx, "log4j", 5);
    enumval = RedisModule_CreateString(ctx, "one", 3);
    
    /* enum_vals is initialized as a stack variable in order to ensure we're copying them over in the core. */
    const char *enum_vals[3] = {"one", "two", "three"};

    if (RedisModule_RegisterBoolConfig(ctx, "mutable_bool", REDISMODULE_CONFIG_MODIFIABLE, &getBoolConfigCommand, &setBoolConfigCommand, &mutable_bool_val) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    /* Immutable config here. */
    if (RedisModule_RegisterBoolConfig(ctx, "immutable_bool", REDISMODULE_CONFIG_IMMUTABLE, &getBoolConfigCommand, &setBoolConfigCommand, &immutable_bool_val) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    /* Memory config here. */
    if (RedisModule_RegisterNumericConfig(ctx, "memory_numeric", REDISMODULE_CONFIG_MODIFIABLE | REDISMODULE_CONFIG_MEMORY, 0, 3000000, &getNumericConfigCommand, &setNumericConfigCommand, &memval) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    if (RedisModule_RegisterStringConfig(ctx, "string", REDISMODULE_CONFIG_MODIFIABLE, &getStringConfigCommand, &setStringConfigCommand, &strval) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    if (RedisModule_RegisterEnumConfig(ctx, "enum", REDISMODULE_CONFIG_MODIFIABLE, enum_vals, 3, &getEnumConfigCommand, &setEnumConfigCommand, &enumval) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    if (RedisModule_RegisterNumericConfig(ctx, "numeric", REDISMODULE_CONFIG_MODIFIABLE, -5, 2000, &getNumericConfigCommand, &setNumericConfigCommand, &longval) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    if (RedisModule_ApplyConfigs(ctx) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}
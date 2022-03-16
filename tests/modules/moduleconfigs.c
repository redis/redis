#include "redismodule.h"
#include <strings.h>
int mutable_bool_val;
int immutable_bool_val;
long long longval;
long long memval;
RedisModuleString *strval;
int enumval;
const char *enum_vals[3] = {"one", "two", "three"};

/* Series of get and set callbacks for each type of config, these rely on the privdata ptr
 * to point to the config, and they register the configs as such. Note that one could also just
 * use names if they wanted, and store anything in privdata. */
int getBoolConfigCommand(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(name);
    return (*(int *)privdata);
}

int setBoolConfigCommand(const char *name, int new, void *privdata, const char **err) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(err);
    *(int *)privdata = new;
    return REDISMODULE_OK;
}

long long getNumericConfigCommand(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(name);
    return (*(long long *) privdata);
}

int setNumericConfigCommand(const char *name, long long new, void *privdata, const char **err) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(err);
    *(long long *)privdata = new;
    return REDISMODULE_OK;
}

RedisModuleString *getStringConfigCommand(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(name);
    return (*(RedisModuleString **) privdata);
}
int setStringConfigCommand(const char *name, RedisModuleString *new, void *privdata, const char **err) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(err);
    size_t len;
    if (!strcasecmp(RedisModule_StringPtrLen(new, &len), "rejectisfreed")) {
        return REDISMODULE_ERR;
    }
    RedisModule_Free(*(RedisModuleString **)privdata);
    RedisModule_RetainString(NULL, new);
    *(RedisModuleString **)privdata = new;
    return REDISMODULE_OK;
}

int getEnumConfigCommand(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(privdata);
    return enumval;
}

int setEnumConfigCommand(const char *name, int val, void *privdata, const char **err) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(err);
    REDISMODULE_NOT_USED(privdata);
    /* We don't have to do any verification here, the core makes sure its in the range of enum_vals we provide */
    enumval = val;
    return REDISMODULE_OK;
}

int boolApplyFunc(RedisModuleCtx *ctx, void *privdata, const char **err) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(privdata);
    if (mutable_bool_val && immutable_bool_val) {
        *err = "Bool configs cannot both be yes.";
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}

int longlongApplyFunc(RedisModuleCtx *ctx, void *privdata, const char **err) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(privdata);
    if (longval == memval) {
        *err = "These configs cannot equal each other.";
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    const int int_vals[3] = {0, 2, 4};

    if (RedisModule_Init(ctx, "moduleconfigs", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_RegisterBoolConfig(ctx, "mutable_bool", 1, REDISMODULE_CONFIG_DEFAULT, getBoolConfigCommand, setBoolConfigCommand, boolApplyFunc, &mutable_bool_val) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    /* Immutable config here. */
    if (RedisModule_RegisterBoolConfig(ctx, "immutable_bool", 0, REDISMODULE_CONFIG_IMMUTABLE, getBoolConfigCommand, setBoolConfigCommand, boolApplyFunc, &immutable_bool_val) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    RedisModuleString *default_string = RedisModule_CreateString(ctx, "log4j", 5);
    if (RedisModule_RegisterStringConfig(ctx, "string", default_string, REDISMODULE_CONFIG_DEFAULT, getStringConfigCommand, setStringConfigCommand, NULL, &strval) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    RedisModule_Free(default_string);
    if (RedisModule_RegisterEnumConfig(ctx, "enum", 0, REDISMODULE_CONFIG_DEFAULT, enum_vals, int_vals, 3, getEnumConfigCommand, setEnumConfigCommand, NULL, NULL) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    /* Memory config here. */
    if (RedisModule_RegisterNumericConfig(ctx, "memory_numeric", 1024, REDISMODULE_CONFIG_DEFAULT | REDISMODULE_CONFIG_MEMORY, 0, 3000000, getNumericConfigCommand, setNumericConfigCommand, longlongApplyFunc, &memval) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    if (RedisModule_RegisterNumericConfig(ctx, "numeric", -1, REDISMODULE_CONFIG_DEFAULT, -5, 2000, getNumericConfigCommand, setNumericConfigCommand, longlongApplyFunc, &longval) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    
    if (RedisModule_LoadConfigs(ctx) == REDISMODULE_ERR) {
        if (strval) {
            RedisModule_Free(strval);
        }
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    REDISMODULE_NOT_USED(ctx);
    if (strval) RedisModule_Free(strval);
    return REDISMODULE_OK;
}
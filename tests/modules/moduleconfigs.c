#include "redismodule.h"
#include <string.h>
#include <strings.h>
#include <assert.h>
int mutable_bool_val, no_prefix_bool, no_prefix_bool2;
int immutable_bool_val;
long long longval, no_prefix_longval;
long long memval, no_prefix_memval;
RedisModuleString *strval = NULL;
RedisModuleString *strval2 = NULL;
int enumval, no_prefix_enumval;
int flagsval;

/* Series of get and set callbacks for each type of config, these rely on the privdata ptr
 * to point to the config, and they register the configs as such. Note that one could also just
 * use names if they wanted, and store anything in privdata. */
int getBoolConfigCommand(const char *name, void *privdata) {
    assert(strcmp(name, "mutable_bool") == 0 || strcmp(name, "immutable_bool") == 0);
    return (*(int *)privdata);
}

int setBoolConfigCommand(const char *name, int new, void *privdata, RedisModuleString **err) {
    REDISMODULE_NOT_USED(err);
    assert(strcmp(name, "mutable_bool") == 0 || strcmp(name, "immutable_bool") == 0);
    *(int *)privdata = new;
    return REDISMODULE_OK;
}

long long getNumericConfigCommand(const char *name, void *privdata) {
    assert(strcmp(name, "numeric") == 0 || strcmp(name, "memory_numeric") == 0);
    return (*(long long *) privdata);
}

int setNumericConfigCommand(const char *name, long long new, void *privdata, RedisModuleString **err) {
    REDISMODULE_NOT_USED(err);
    assert(strcmp(name, "numeric") == 0 || strcmp(name, "memory_numeric") == 0);
    *(long long *)privdata = new;
    return REDISMODULE_OK;
}

RedisModuleString *getStringConfigCommand(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(privdata);
    assert(strcmp(name, "string") == 0);
    return strval;
}
int setStringConfigCommand(const char *name, RedisModuleString *new, void *privdata, RedisModuleString **err) {
    REDISMODULE_NOT_USED(privdata);
    assert(strcmp(name, "string") == 0);
    size_t len;
    if (!strcasecmp(RedisModule_StringPtrLen(new, &len), "rejectisfreed")) {
        *err = RedisModule_CreateString(NULL, "Cannot set string to 'rejectisfreed'", 36);
        return REDISMODULE_ERR;
    }
    if (strval) RedisModule_FreeString(NULL, strval);
    RedisModule_RetainString(NULL, new);
    strval = new;
    return REDISMODULE_OK;
}

int getEnumConfigCommand(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(privdata);
    assert(strcmp(name, "enum") == 0);
    return enumval;
}

int setEnumConfigCommand(const char *name, int val, void *privdata, RedisModuleString **err) {
    REDISMODULE_NOT_USED(privdata);
    REDISMODULE_NOT_USED(err);
    assert(strcmp(name, "enum") == 0);
    enumval = val;
    return REDISMODULE_OK;
}

int getFlagsConfigCommand(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(privdata);
    assert(strcmp(name, "flags") == 0);
    return flagsval;
}

int setFlagsConfigCommand(const char *name, int val, void *privdata, RedisModuleString **err) {
    REDISMODULE_NOT_USED(err);
    REDISMODULE_NOT_USED(privdata);
    assert(strcmp(name, "flags") == 0);
    flagsval = val;
    return REDISMODULE_OK;
}

int boolApplyFunc(RedisModuleCtx *ctx, void *privdata, RedisModuleString **err) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(privdata);
    if (mutable_bool_val && immutable_bool_val) {
        *err = RedisModule_CreateString(NULL, "Bool configs cannot both be yes.", 32);
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}

int longlongApplyFunc(RedisModuleCtx *ctx, void *privdata, RedisModuleString **err) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(privdata);
    if (longval == memval) {
        *err = RedisModule_CreateString(NULL, "These configs cannot equal each other.", 38);
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}    

RedisModuleString *getStringConfigUnprefix(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(privdata);
    assert(strcmp(name, "unprefix-string") == 0 || strcmp(name, "unprefix.string-alias") == 0);
    return strval2;
}

int setStringConfigUnprefix(const char *name, RedisModuleString *new, void *privdata, RedisModuleString **err) {
    REDISMODULE_NOT_USED(privdata);
    REDISMODULE_NOT_USED(err);
    assert(strcmp(name, "unprefix-string") == 0 || strcmp(name, "unprefix.string-alias") == 0);
    if (strval2) RedisModule_FreeString(NULL, strval2);
    RedisModule_RetainString(NULL, new);
    strval2 = new;
    return REDISMODULE_OK;
}

int getBoolConfigUnprefix(const char *name, void *privdata) {
    assert(strcmp(name, "unprefix-bool") == 0 || strcmp(name, "unprefix-bool-alias") == 0 ||
           strcmp(name, "unprefix-noalias-bool") == 0);
    return (*(int *)privdata);
}

int setBoolConfigUnprefix(const char *name, int new, void *privdata, RedisModuleString **err) {
    REDISMODULE_NOT_USED(err);
    assert(strcmp(name, "unprefix-bool") == 0 || strcmp(name, "unprefix-bool-alias") == 0 ||
           strcmp(name, "unprefix-noalias-bool") == 0);
    *(int *)privdata = new;
    return REDISMODULE_OK;
}

long long getNumericConfigUnprefix(const char *name, void *privdata) {
    assert(strcmp(name, "unprefix.numeric") == 0 || strcmp(name, "unprefix.numeric-alias") == 0);
    return (*(long long *) privdata);
}

int setNumericConfigUnprefix(const char *name, long long new, void *privdata, RedisModuleString **err) {
    REDISMODULE_NOT_USED(err);
    assert(strcmp(name, "unprefix.numeric") == 0 || strcmp(name, "unprefix.numeric-alias") == 0);
    *(long long *)privdata = new;
    return REDISMODULE_OK;
}

int getEnumConfigUnprefix(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(privdata);
    assert(strcmp(name, "unprefix-enum") == 0 || strcmp(name, "unprefix-enum-alias") == 0);
    return no_prefix_enumval;
}

int setEnumConfigUnprefix(const char *name, int val, void *privdata, RedisModuleString **err) {
    REDISMODULE_NOT_USED(privdata);
    REDISMODULE_NOT_USED(err);
    assert(strcmp(name, "unprefix-enum") == 0 || strcmp(name, "unprefix-enum-alias") == 0);
    no_prefix_enumval = val;
    return REDISMODULE_OK;
}

int registerBlockCheck(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    int response_ok = 0;
    int result = RedisModule_RegisterBoolConfig(ctx, "mutable_bool", 1, REDISMODULE_CONFIG_DEFAULT, getBoolConfigCommand, setBoolConfigCommand, boolApplyFunc, &mutable_bool_val);
    response_ok |= (result == REDISMODULE_OK);

    result = RedisModule_RegisterStringConfig(ctx, "string", "secret password", REDISMODULE_CONFIG_DEFAULT, getStringConfigCommand, setStringConfigCommand, NULL, NULL);
    response_ok |= (result == REDISMODULE_OK);

    const char *enum_vals[] = {"none", "five", "one", "two", "four"};
    const int int_vals[] = {0, 5, 1, 2, 4};
    result = RedisModule_RegisterEnumConfig(ctx, "enum", 1, REDISMODULE_CONFIG_DEFAULT, enum_vals, int_vals, 5, getEnumConfigCommand, setEnumConfigCommand, NULL, NULL);
    response_ok |= (result == REDISMODULE_OK);

    result = RedisModule_RegisterNumericConfig(ctx, "numeric", -1, REDISMODULE_CONFIG_DEFAULT, -5, 2000, getNumericConfigCommand, setNumericConfigCommand, longlongApplyFunc, &longval);
    response_ok |= (result == REDISMODULE_OK);

    result = RedisModule_LoadConfigs(ctx);
    response_ok |= (result == REDISMODULE_OK);
    
    /* This validates that it's not possible to register/load configs outside OnLoad,
     * thus returns an error if they succeed. */
    if (response_ok) {
        RedisModule_ReplyWithError(ctx, "UNEXPECTEDOK");
    } else {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    }
    return REDISMODULE_OK;
}

void cleanup(RedisModuleCtx *ctx) {
    if (strval) {
        RedisModule_FreeString(ctx, strval);
        strval = NULL;
    }
    if (strval2) {
        RedisModule_FreeString(ctx, strval2);
        strval2 = NULL;
    }
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "moduleconfigs", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to init module");
        return REDISMODULE_ERR;
    }

    if (RedisModule_RegisterBoolConfig(ctx, "mutable_bool", 1, REDISMODULE_CONFIG_DEFAULT, getBoolConfigCommand, setBoolConfigCommand, boolApplyFunc, &mutable_bool_val) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to register mutable_bool");
        return REDISMODULE_ERR;
    }
    /* Immutable config here. */
    if (RedisModule_RegisterBoolConfig(ctx, "immutable_bool", 0, REDISMODULE_CONFIG_IMMUTABLE, getBoolConfigCommand, setBoolConfigCommand, boolApplyFunc, &immutable_bool_val) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to register immutable_bool");
        return REDISMODULE_ERR;
    }
    if (RedisModule_RegisterStringConfig(ctx, "string", "secret password", REDISMODULE_CONFIG_DEFAULT, getStringConfigCommand, setStringConfigCommand, NULL, NULL) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to register string");
        return REDISMODULE_ERR;
    }

    /* On the stack to make sure we're copying them. */
    const char *enum_vals[] = {"none", "five", "one", "two", "four"};
    const int int_vals[] = {0, 5, 1, 2, 4};

    if (RedisModule_RegisterEnumConfig(ctx, "enum", 1, REDISMODULE_CONFIG_DEFAULT, enum_vals, int_vals, 5, getEnumConfigCommand, setEnumConfigCommand, NULL, NULL) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to register enum");
        return REDISMODULE_ERR;
    }
    if (RedisModule_RegisterEnumConfig(ctx, "flags", 3, REDISMODULE_CONFIG_DEFAULT | REDISMODULE_CONFIG_BITFLAGS, enum_vals, int_vals, 5, getFlagsConfigCommand, setFlagsConfigCommand, NULL, NULL) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to register flags");
        return REDISMODULE_ERR;
    }
    /* Memory config here. */
    if (RedisModule_RegisterNumericConfig(ctx, "memory_numeric", 1024, REDISMODULE_CONFIG_DEFAULT | REDISMODULE_CONFIG_MEMORY, 0, 3000000, getNumericConfigCommand, setNumericConfigCommand, longlongApplyFunc, &memval) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to register memory_numeric");
        return REDISMODULE_ERR;
    }
    if (RedisModule_RegisterNumericConfig(ctx, "numeric", -1, REDISMODULE_CONFIG_DEFAULT, -5, 2000, getNumericConfigCommand, setNumericConfigCommand, longlongApplyFunc, &longval) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to register numeric");
        return REDISMODULE_ERR;
    }

    /*** unprefixed and aliased configuration ***/
    if (RedisModule_RegisterBoolConfig(ctx, "unprefix-bool|unprefix-bool-alias", 1, REDISMODULE_CONFIG_DEFAULT|REDISMODULE_CONFIG_UNPREFIXED,
                                       getBoolConfigUnprefix, setBoolConfigUnprefix, NULL, &no_prefix_bool) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to register unprefix-bool");
        return REDISMODULE_ERR;
    }
    if (RedisModule_RegisterBoolConfig(ctx, "unprefix-noalias-bool", 1, REDISMODULE_CONFIG_DEFAULT|REDISMODULE_CONFIG_UNPREFIXED,
                                       getBoolConfigUnprefix, setBoolConfigUnprefix, NULL, &no_prefix_bool2) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to register unprefix-noalias-bool");
        return REDISMODULE_ERR;
    }
    if (RedisModule_RegisterNumericConfig(ctx, "unprefix.numeric|unprefix.numeric-alias", -1, REDISMODULE_CONFIG_DEFAULT|REDISMODULE_CONFIG_UNPREFIXED,
                                          -5, 2000, getNumericConfigUnprefix, setNumericConfigUnprefix, NULL, &no_prefix_longval) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to register unprefix.numeric");
        return REDISMODULE_ERR;
    }
    if (RedisModule_RegisterStringConfig(ctx, "unprefix-string|unprefix.string-alias", "secret unprefix", REDISMODULE_CONFIG_DEFAULT|REDISMODULE_CONFIG_UNPREFIXED, 
                                         getStringConfigUnprefix, setStringConfigUnprefix, NULL, NULL) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to register unprefix-string");
        return REDISMODULE_ERR;
    }    
    if (RedisModule_RegisterEnumConfig(ctx, "unprefix-enum|unprefix-enum-alias", 1, REDISMODULE_CONFIG_DEFAULT|REDISMODULE_CONFIG_UNPREFIXED, 
                                       enum_vals, int_vals, 5, getEnumConfigUnprefix, setEnumConfigUnprefix, NULL, NULL) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to register unprefix-enum");
        return REDISMODULE_ERR;
    }

    RedisModule_Log(ctx, "debug", "Registered configuration");
    size_t len;
    if (argc && !strcasecmp(RedisModule_StringPtrLen(argv[0], &len), "noload")) {
        return REDISMODULE_OK;
    } else if (argc && !strcasecmp(RedisModule_StringPtrLen(argv[0], &len), "override-default")) {
        if (RedisModule_LoadDefaultConfigs(ctx) == REDISMODULE_ERR) {
            RedisModule_Log(ctx, "warning", "Failed to load default configuration");
            goto err;
        }
        // simulate configuration values being overwritten by the command line
        RedisModule_Log(ctx, "debug", "Overriding configuration values");
        if (strval) RedisModule_FreeString(ctx, strval);
        strval = RedisModule_CreateString(ctx, "foo", 3);
        longval = memval = 123;
    }
    RedisModule_Log(ctx, "debug", "Loading configuration");
    if (RedisModule_LoadConfigs(ctx) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to load configuration");
        goto err;
    }
    /* Creates a command which registers configs outside OnLoad() function. */
    if (RedisModule_CreateCommand(ctx,"block.register.configs.outside.onload", registerBlockCheck, "write", 0, 0, 0) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to register command");
        goto err;
    }
  
    return REDISMODULE_OK;
err:
    cleanup(ctx);
    return REDISMODULE_ERR;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    REDISMODULE_NOT_USED(ctx);
    cleanup(ctx);
    return REDISMODULE_OK;
}

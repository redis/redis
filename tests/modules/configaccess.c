#include "redismodule.h"
#include <assert.h>
#include <string.h>

/* See moduleconfigs.c for registering module configs. We need to register some
 * module configs with our module in order to test the interaction between
 * module configs and the RM_Get/Set*Config APIs. */
int configaccess_bool;

int getBoolConfigCommand(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(name);
    return (*(int *)privdata);
}

int setBoolConfigCommand(const char *name, int new, void *privdata, RedisModuleString **err) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(err);
    *(int *)privdata = new;
    return REDISMODULE_OK;
}

/* Test command for RM_GetConfigType */
int TestGetConfigType_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    size_t len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &len);

    RedisModuleConfigType type;
    int res = RedisModule_ConfigGetType(config_name, &type);
    if (res == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "ERR Config does not exist");
        return REDISMODULE_ERR;
    }

    const char *type_str;
    switch (type) {
    case REDISMODULE_CONFIG_TYPE_BOOL:
        type_str = "bool";
        break;
    case REDISMODULE_CONFIG_TYPE_NUMERIC:
        type_str = "numeric";
        break;
    case REDISMODULE_CONFIG_TYPE_STRING:
        type_str = "string";
        break;
    case REDISMODULE_CONFIG_TYPE_ENUM:
        type_str = "enum";
        break;
    default:
        assert(0);
        break;
    }

    RedisModule_ReplyWithSimpleString(ctx, type_str);
    return REDISMODULE_OK;
}

/* Test command for config iteration */
int TestConfigIteration_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);

    if (argc > 2) {
        return RedisModule_WrongArity(ctx);
    }

    const char *pattern = NULL;
    if (argc == 2) {
        pattern = RedisModule_StringPtrLen(argv[1], NULL);
    }

    RedisModuleConfigIterator *iter = RedisModule_ConfigIteratorCreate(ctx, pattern);
    if (!iter) {
        RedisModule_ReplyWithError(ctx, "ERR Failed to get config iterator");
        return REDISMODULE_ERR;
    }

    /* Start array reply for the configs */
    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

    /* Iterate through the dictionary */
    const char *config_name = NULL;
    long count = 0;
    while ((config_name = RedisModule_ConfigIteratorNext(iter)) != NULL) {
        RedisModuleString *value = NULL;
        RedisModule_ConfigGet(ctx, config_name, &value);

        RedisModule_ReplyWithArray(ctx, 2);
        RedisModule_ReplyWithStringBuffer(ctx, config_name, strlen(config_name));
        RedisModule_ReplyWithString(ctx, value);

        RedisModule_FreeString(ctx, value);
        ++count;
    }
    RedisModule_ReplySetArrayLength(ctx, count);

    /* Free the iterator */
    RedisModule_ConfigIteratorRelease(ctx, iter);

    return REDISMODULE_OK;
}

/* Test command for RM_GetBoolConfig */
int TestGetBoolConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    size_t len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &len);

    int value;
    if (RedisModule_ConfigGetBool(ctx, config_name, &value) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "ERR Failed to get bool config");
        return REDISMODULE_ERR;
    }

    RedisModule_ReplyWithLongLong(ctx, value);
    return REDISMODULE_OK;
}

/* Test command for RM_GetNumericConfig */
int TestGetNumericConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    size_t len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &len);

    long long value;
    if (RedisModule_ConfigGetNumeric(ctx, config_name, &value) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "ERR Failed to get numeric config");
        return REDISMODULE_ERR;
    }

    RedisModule_ReplyWithLongLong(ctx, value);
    return REDISMODULE_OK;
}

/* Test command for RM_GetConfig */
int TestGetConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    size_t len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &len);

    RedisModuleString *value;
    if (RedisModule_ConfigGet(ctx, config_name, &value) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "ERR Failed to get string config");
        return REDISMODULE_ERR;
    }

    RedisModule_ReplyWithString(ctx, value);
    RedisModule_FreeString(ctx,value);
    return REDISMODULE_OK;
}

/* Test command for RM_GetEnumConfig */
int TestGetEnumConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    size_t len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &len);

    RedisModuleString *value;
    if (RedisModule_ConfigGetEnum(ctx, config_name, &value) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "ERR Failed to get enum name config");
        return REDISMODULE_ERR;
    }

    RedisModule_ReplyWithString(ctx, value);
    RedisModule_Free(value);
    return REDISMODULE_OK;
}

/* Test command for RM_SetBoolConfig */
int TestSetBoolConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    size_t name_len, value_len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &name_len);
    const char *config_value = RedisModule_StringPtrLen(argv[2], &value_len);

    int bool_value;
    if (!strcasecmp(config_value, "yes")) {
        bool_value = 1;
    } else if (!strcasecmp(config_value, "no")) {
        bool_value = 0;
    } else {
        bool_value = -1;
    }

    RedisModuleString *error = NULL;
    int result = RedisModule_ConfigSetBool(ctx, config_name, bool_value, &error);
    if (result == REDISMODULE_ERR) {
        RedisModule_ReplyWithErrorFormat(ctx, "ERR Failed to set bool config %s: %s", config_name, RedisModule_StringPtrLen(error, NULL));
        RedisModule_FreeString(ctx, error);
        return REDISMODULE_ERR;
    }

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* Test command for RM_SetNumericConfig */
int TestSetNumericConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    size_t name_len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &name_len);

    long long value;
    if (RedisModule_StringToLongLong(argv[2], &value) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "ERR Invalid numeric value");
        return REDISMODULE_ERR;
    }

    RedisModuleString *error = NULL;
    int result = RedisModule_ConfigSetNumeric(ctx, config_name, value, &error);
    if (result == REDISMODULE_OK) {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        RedisModule_ReplyWithErrorFormat(ctx, "ERR Failed to set numeric config %s: %s", config_name, RedisModule_StringPtrLen(error, NULL));
        RedisModule_FreeString(ctx, error);
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

/* Test command for RM_SetConfig */
int TestSetConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    size_t name_len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &name_len);

    RedisModuleString *error = NULL;
    int result = RedisModule_ConfigSet(ctx, config_name, argv[2], &error);
    if (result == REDISMODULE_OK) {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        RedisModule_ReplyWithErrorFormat(ctx, "ERR Failed to set string config %s: %s", config_name, RedisModule_StringPtrLen(error, NULL));
        RedisModule_FreeString(ctx, error);
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

/* Test command for RM_SetEnumConfig with name */
int TestSetEnumConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }

    const char *config_name = RedisModule_StringPtrLen(argv[1], NULL);

    RedisModuleString *error = NULL;
    int result = RedisModule_ConfigSetEnum(ctx, config_name, argv[2], &error);

    if (result == REDISMODULE_OK) {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        RedisModule_ReplyWithErrorFormat(ctx, "ERR Failed to set enum config %s: %s", config_name, RedisModule_StringPtrLen(error, NULL));
        RedisModule_FreeString(ctx, error);
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "configaccess", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "configaccess.getconfigs",
                                 TestConfigIteration_RedisCommand, "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "configaccess.getbool",
                                 TestGetBoolConfig_RedisCommand, "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "configaccess.getnumeric",
                                 TestGetNumericConfig_RedisCommand, "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "configaccess.get",
                                 TestGetConfig_RedisCommand, "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "configaccess.getenum",
                                 TestGetEnumConfig_RedisCommand, "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "configaccess.setbool",
                                 TestSetBoolConfig_RedisCommand, "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "configaccess.setnumeric",
                                 TestSetNumericConfig_RedisCommand, "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "configaccess.set",
                                 TestSetConfig_RedisCommand, "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "configaccess.setenum",
                                 TestSetEnumConfig_RedisCommand, "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "configaccess.getconfigtype", TestGetConfigType_RedisCommand, "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_RegisterBoolConfig(ctx, "bool", 1, REDISMODULE_CONFIG_DEFAULT,
                                       getBoolConfigCommand, setBoolConfigCommand, NULL, &configaccess_bool) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to register configaccess_bool");
        return REDISMODULE_ERR;
    }

    RedisModule_Log(ctx, "debug", "Loading configaccess module configuration");
    if (RedisModule_LoadConfigs(ctx) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to load configaccess module configuration");
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

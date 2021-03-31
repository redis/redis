/* This module current tests a small subset but should be extended in the future
 * for general ModuleDataType coverage.
 */

#include "redismodule.h"

static RedisModuleType *datatype = NULL;

typedef struct {
    long long intval;
    RedisModuleString *strval;
} DataType;

static void *datatype_load(RedisModuleIO *io, int encver) {
    (void) encver;

    int intval = RedisModule_LoadSigned(io);
    if (RedisModule_IsIOError(io)) return NULL;

    RedisModuleString *strval = RedisModule_LoadString(io);
    if (RedisModule_IsIOError(io)) return NULL;

    DataType *dt = (DataType *) RedisModule_Alloc(sizeof(DataType));
    dt->intval = intval;
    dt->strval = strval;
    return dt;
}

static void datatype_save(RedisModuleIO *io, void *value) {
    DataType *dt = (DataType *) value;
    RedisModule_SaveSigned(io, dt->intval);
    RedisModule_SaveString(io, dt->strval);
}

static void datatype_rewrite(RedisModuleIO *aof, RedisModuleString *key, void *value) {
    /*  do-nothing callback. */
}

static void datatype_free(void *value) {
    if (value) {
        DataType *dt = (DataType *) value;

        if (dt->strval) RedisModule_FreeString(NULL, dt->strval);
        RedisModule_Free(dt);
    }
}

static void *datatype_copy(RedisModuleString *fromkey, RedisModuleString *tokey, const void *value) {
    const DataType *old = value;

    /* Answers to ultimate questions cannot be copied! */
    if (old->intval == 42)
        return NULL;

    DataType *new = (DataType *) RedisModule_Alloc(sizeof(DataType));

    new->intval = old->intval;
    new->strval = RedisModule_CreateStringFromString(NULL, old->strval);

    /* Breaking the rules here! We return a copy that also includes traces
     * of fromkey/tokey to confirm we get what we expect.
     */
    size_t len;
    const char *str = RedisModule_StringPtrLen(fromkey, &len);
    RedisModule_StringAppendBuffer(NULL, new->strval, "/", 1);
    RedisModule_StringAppendBuffer(NULL, new->strval, str, len);
    RedisModule_StringAppendBuffer(NULL, new->strval, "/", 1);
    str = RedisModule_StringPtrLen(tokey, &len);
    RedisModule_StringAppendBuffer(NULL, new->strval, str, len);

    return new;
}

static int datatype_set(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    long long intval;

    if (RedisModule_StringToLongLong(argv[2], &intval) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "Invalid integr value");
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    DataType *dt = RedisModule_Calloc(sizeof(DataType), 1);
    dt->intval = intval;
    dt->strval = argv[3];
    RedisModule_RetainString(ctx, dt->strval);

    RedisModule_ModuleTypeSetValue(key, datatype, dt);
    RedisModule_CloseKey(key);
    RedisModule_ReplyWithSimpleString(ctx, "OK");

    return REDISMODULE_OK;
}

static int datatype_restore(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    DataType *dt = RedisModule_LoadDataTypeFromString(argv[2], datatype);
    if (!dt) {
        RedisModule_ReplyWithError(ctx, "Invalid data");
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    RedisModule_ModuleTypeSetValue(key, datatype, dt);
    RedisModule_CloseKey(key);
    RedisModule_ReplyWithSimpleString(ctx, "OK");

    return REDISMODULE_OK;
}

static int datatype_get(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    DataType *dt = RedisModule_ModuleTypeGetValue(key);
    RedisModule_CloseKey(key);

    if (!dt) {
        RedisModule_ReplyWithNullArray(ctx);
    } else {
        RedisModule_ReplyWithArray(ctx, 2);
        RedisModule_ReplyWithLongLong(ctx, dt->intval);
        RedisModule_ReplyWithString(ctx, dt->strval);
    }
    return REDISMODULE_OK;
}

static int datatype_dump(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    DataType *dt = RedisModule_ModuleTypeGetValue(key);
    RedisModule_CloseKey(key);

    RedisModuleString *reply = RedisModule_SaveDataTypeToString(ctx, dt, datatype);
    if (!reply) {
        RedisModule_ReplyWithError(ctx, "Failed to save");
        return REDISMODULE_OK;
    }

    RedisModule_ReplyWithString(ctx, reply);
    RedisModule_FreeString(ctx, reply);
    return REDISMODULE_OK;
}

static int datatype_swap(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RedisModuleKey *a = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    RedisModuleKey *b = RedisModule_OpenKey(ctx, argv[2], REDISMODULE_WRITE);
    void *val = RedisModule_ModuleTypeGetValue(a);

    int error = (RedisModule_ModuleTypeReplaceValue(b, datatype, val, &val) == REDISMODULE_ERR ||
                 RedisModule_ModuleTypeReplaceValue(a, datatype, val, NULL) == REDISMODULE_ERR);
    if (!error)
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    else
        RedisModule_ReplyWithError(ctx, "ERR failed");

    RedisModule_CloseKey(a);
    RedisModule_CloseKey(b);

    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"datatype",1,REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_HANDLE_IO_ERRORS);

    RedisModuleTypeMethods datatype_methods = {
        .version = REDISMODULE_TYPE_METHOD_VERSION,
        .rdb_load = datatype_load,
        .rdb_save = datatype_save,
        .aof_rewrite = datatype_rewrite,
        .free = datatype_free,
        .copy = datatype_copy
    };

    datatype = RedisModule_CreateDataType(ctx, "test___dt", 1, &datatype_methods);
    if (datatype == NULL)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"datatype.set", datatype_set,"deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"datatype.get", datatype_get,"",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"datatype.restore", datatype_restore,"deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"datatype.dump", datatype_dump,"",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"datatype.swap", datatype_swap,"",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

/* Regression test for RM_SaveDataTypeToString error path.
 * Verifies that when rdb_save sets io.error, the internal sds buffer
 * is properly freed before returning NULL. */

#include "redismodule.h"

static RedisModuleType *saveerror_type = NULL;

/* Mock struct matching RedisModuleIO (src/server.h) to access error field.
 * No public API exists to set io.error, so we cast to this layout.
 * Note: depends on field layout - inserting/deleting fields before 'error'
 * in RedisModuleIO will break this. */
typedef struct {
    size_t bytes;
    void *rio;
    void *entity;
    int error;
} MockRedisModuleIO;

typedef struct {
    long long value;
} SaveErrorData;

/* rdb_save callback that sets io.error to trigger the error path. */
static void saveerror_rdb_save(RedisModuleIO *rdb, void *value) {
    (void)value;
    MockRedisModuleIO *io = (MockRedisModuleIO *)rdb;
    io->error = 1;
}

static void *saveerror_rdb_load(RedisModuleIO *rdb, int encver) {
    (void)encver;
    long long val = RedisModule_LoadSigned(rdb);
    if (RedisModule_IsIOError(rdb)) return NULL;

    SaveErrorData *data = RedisModule_Alloc(sizeof(SaveErrorData));
    data->value = val;
    return data;
}

static void saveerror_free(void *value) {
    RedisModule_Free(value);
}

/* saveerror.set <key> <value> -- create a key with our data type */
static int saveerror_set_cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    long long val;
    if (RedisModule_StringToLongLong(argv[2], &val) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "ERR invalid integer");
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    SaveErrorData *data = RedisModule_Alloc(sizeof(SaveErrorData));
    data->value = val;
    RedisModule_ModuleTypeSetValue(key, saveerror_type, data);
    RedisModule_CloseKey(key);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* saveerror.dump <key> -- test SaveDataTypeToString error path */
static int saveerror_dump_cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    void *data = RedisModule_ModuleTypeGetValue(key);
    RedisModule_CloseKey(key);

    if (!data) {
        RedisModule_ReplyWithError(ctx, "ERR key not found");
        return REDISMODULE_OK;
    }

    RedisModuleString *reply = RedisModule_SaveDataTypeToString(ctx, data, saveerror_type);
    if (!reply) {
        /* Error path: io.error was set by rdb_save */
        RedisModule_ReplyWithError(ctx, "ERR io error");
        return REDISMODULE_OK;
    }

    RedisModule_ReplyWithString(ctx, reply);
    RedisModule_FreeString(ctx, reply);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "saveerror", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_HANDLE_IO_ERRORS);

    RedisModuleTypeMethods tm = {
        .version = REDISMODULE_TYPE_METHOD_VERSION,
        .rdb_load = saveerror_rdb_load,
        .rdb_save = saveerror_rdb_save,
        .free = saveerror_free,
    };

    saveerror_type = RedisModule_CreateDataType(ctx, "saverr_dt", 1, &tm);
    if (saveerror_type == NULL)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "saveerror.set", saveerror_set_cmd,
                                  "write deny-oom", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "saveerror.dump", saveerror_dump_cmd,
                                  "", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

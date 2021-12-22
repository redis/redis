#include "redismodule.h"

#include <string.h>
#include <assert.h>

/* Module configuration, save aux or not? */
long long conf_aux_count = 0;

/* Registered type */
RedisModuleType *testrdb_type = NULL;

/* Global values to store and persist to aux */
RedisModuleString *before_str = NULL;
RedisModuleString *after_str = NULL;

/* Global values used to keep aux from db being loaded (in case of async_loading) */
RedisModuleString *before_str_temp = NULL;
RedisModuleString *after_str_temp = NULL;

/* Indicates whether there is an async replication in progress.
 * We control this value from RedisModuleEvent_ReplAsyncLoad events. */
int async_loading = 0;

void replAsyncLoadCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(e);
    REDISMODULE_NOT_USED(data);

    switch (sub) {
    case REDISMODULE_SUBEVENT_REPL_ASYNC_LOAD_STARTED:
        assert(async_loading == 0);
        async_loading = 1;
        break;
    case REDISMODULE_SUBEVENT_REPL_ASYNC_LOAD_ABORTED:
        /* Discard temp aux */
        if (before_str_temp)
            RedisModule_FreeString(ctx, before_str_temp);
        if (after_str_temp)
            RedisModule_FreeString(ctx, after_str_temp);
        before_str_temp = NULL;
        after_str_temp = NULL;

        async_loading = 0;
        break;
    case REDISMODULE_SUBEVENT_REPL_ASYNC_LOAD_COMPLETED:
        if (before_str)
            RedisModule_FreeString(ctx, before_str);
        if (after_str)
            RedisModule_FreeString(ctx, after_str);
        before_str = before_str_temp;
        after_str = after_str_temp;

        before_str_temp = NULL;
        after_str_temp = NULL;

        async_loading = 0;
        break;
    default:
        assert(0);
    }
}

void *testrdb_type_load(RedisModuleIO *rdb, int encver) {
    int count = RedisModule_LoadSigned(rdb);
    RedisModuleString *str = RedisModule_LoadString(rdb);
    float f = RedisModule_LoadFloat(rdb);
    long double ld = RedisModule_LoadLongDouble(rdb);
    if (RedisModule_IsIOError(rdb)) {
        RedisModuleCtx *ctx = RedisModule_GetContextFromIO(rdb);
        if (str)
            RedisModule_FreeString(ctx, str);
        return NULL;
    }
    /* Using the values only after checking for io errors. */
    assert(count==1);
    assert(encver==1);
    assert(f==1.5f);
    assert(ld==0.333333333333333333L);
    return str;
}

void testrdb_type_save(RedisModuleIO *rdb, void *value) {
    RedisModuleString *str = (RedisModuleString*)value;
    RedisModule_SaveSigned(rdb, 1);
    RedisModule_SaveString(rdb, str);
    RedisModule_SaveFloat(rdb, 1.5);
    RedisModule_SaveLongDouble(rdb, 0.333333333333333333L);
}

void testrdb_aux_save(RedisModuleIO *rdb, int when) {
    if (conf_aux_count==1) assert(when == REDISMODULE_AUX_AFTER_RDB);
    if (conf_aux_count==0) assert(0);
    if (when == REDISMODULE_AUX_BEFORE_RDB) {
        if (before_str) {
            RedisModule_SaveSigned(rdb, 1);
            RedisModule_SaveString(rdb, before_str);
        } else {
            RedisModule_SaveSigned(rdb, 0);
        }
    } else {
        if (after_str) {
            RedisModule_SaveSigned(rdb, 1);
            RedisModule_SaveString(rdb, after_str);
        } else {
            RedisModule_SaveSigned(rdb, 0);
        }
    }
}

int testrdb_aux_load(RedisModuleIO *rdb, int encver, int when) {
    assert(encver == 1);
    if (conf_aux_count==1) assert(when == REDISMODULE_AUX_AFTER_RDB);
    if (conf_aux_count==0) assert(0);
    RedisModuleCtx *ctx = RedisModule_GetContextFromIO(rdb);
    if (when == REDISMODULE_AUX_BEFORE_RDB) {
        if (async_loading == 0) {
            if (before_str)
                RedisModule_FreeString(ctx, before_str);
            before_str = NULL;
            int count = RedisModule_LoadSigned(rdb);
            if (RedisModule_IsIOError(rdb))
                return REDISMODULE_ERR;
            if (count)
                before_str = RedisModule_LoadString(rdb);
        } else {
            if (before_str_temp)
                RedisModule_FreeString(ctx, before_str_temp);
            before_str_temp = NULL;
            int count = RedisModule_LoadSigned(rdb);
            if (RedisModule_IsIOError(rdb))
                return REDISMODULE_ERR;
            if (count)
                before_str_temp = RedisModule_LoadString(rdb);
        }
    } else {
        if (async_loading == 0) {
            if (after_str)
                RedisModule_FreeString(ctx, after_str);
            after_str = NULL;
            int count = RedisModule_LoadSigned(rdb);
            if (RedisModule_IsIOError(rdb))
                return REDISMODULE_ERR;
            if (count)
                after_str = RedisModule_LoadString(rdb);
        } else {
            if (after_str_temp)
                RedisModule_FreeString(ctx, after_str_temp);
            after_str_temp = NULL;
            int count = RedisModule_LoadSigned(rdb);
            if (RedisModule_IsIOError(rdb))
                return REDISMODULE_ERR;
            if (count)
                after_str_temp = RedisModule_LoadString(rdb);
        }
    }

    if (RedisModule_IsIOError(rdb))
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

void testrdb_type_free(void *value) {
    if (value)
        RedisModule_FreeString(NULL, (RedisModuleString*)value);
}

int testrdb_set_before(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    if (before_str)
        RedisModule_FreeString(ctx, before_str);
    before_str = argv[1];
    RedisModule_RetainString(ctx, argv[1]);
    RedisModule_ReplyWithLongLong(ctx, 1);
    return REDISMODULE_OK;
}

int testrdb_get_before(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    if (argc != 1){
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }
    if (before_str)
        RedisModule_ReplyWithString(ctx, before_str);
    else
        RedisModule_ReplyWithStringBuffer(ctx, "", 0);
    return REDISMODULE_OK;
}

/* For purpose of testing module events, expose variable state during async_loading. */
int testrdb_async_loading_get_before(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    if (argc != 1){
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }
    if (before_str_temp)
        RedisModule_ReplyWithString(ctx, before_str_temp);
    else
        RedisModule_ReplyWithStringBuffer(ctx, "", 0);
    return REDISMODULE_OK;
}

int testrdb_set_after(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2){
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    if (after_str)
        RedisModule_FreeString(ctx, after_str);
    after_str = argv[1];
    RedisModule_RetainString(ctx, argv[1]);
    RedisModule_ReplyWithLongLong(ctx, 1);
    return REDISMODULE_OK;
}

int testrdb_get_after(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argv);
    if (argc != 1){
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }
    if (after_str)
        RedisModule_ReplyWithString(ctx, after_str);
    else
        RedisModule_ReplyWithStringBuffer(ctx, "", 0);
    return REDISMODULE_OK;
}

int testrdb_set_key(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 3){
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    RedisModuleString *str = RedisModule_ModuleTypeGetValue(key);
    if (str)
        RedisModule_FreeString(ctx, str);
    RedisModule_ModuleTypeSetValue(key, testrdb_type, argv[2]);
    RedisModule_RetainString(ctx, argv[2]);
    RedisModule_CloseKey(key);
    RedisModule_ReplyWithLongLong(ctx, 1);
    return REDISMODULE_OK;
}

int testrdb_get_key(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2){
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    RedisModuleString *str = RedisModule_ModuleTypeGetValue(key);
    RedisModule_CloseKey(key);
    RedisModule_ReplyWithString(ctx, str);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx,"testrdb",1,REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_HANDLE_IO_ERRORS | REDISMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD);

    if (argc > 0)
        RedisModule_StringToLongLong(argv[0], &conf_aux_count);

    if (conf_aux_count==0) {
        RedisModuleTypeMethods datatype_methods = {
            .version = 1,
            .rdb_load = testrdb_type_load,
            .rdb_save = testrdb_type_save,
            .aof_rewrite = NULL,
            .digest = NULL,
            .free = testrdb_type_free,
        };

        testrdb_type = RedisModule_CreateDataType(ctx, "test__rdb", 1, &datatype_methods);
        if (testrdb_type == NULL)
            return REDISMODULE_ERR;
    } else {
        RedisModuleTypeMethods datatype_methods = {
            .version = REDISMODULE_TYPE_METHOD_VERSION,
            .rdb_load = testrdb_type_load,
            .rdb_save = testrdb_type_save,
            .aof_rewrite = NULL,
            .digest = NULL,
            .free = testrdb_type_free,
            .aux_load = testrdb_aux_load,
            .aux_save = testrdb_aux_save,
            .aux_save_triggers = (conf_aux_count == 1 ?
                                  REDISMODULE_AUX_AFTER_RDB :
                                  REDISMODULE_AUX_BEFORE_RDB | REDISMODULE_AUX_AFTER_RDB)
        };

        testrdb_type = RedisModule_CreateDataType(ctx, "test__rdb", 1, &datatype_methods);
        if (testrdb_type == NULL)
            return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx,"testrdb.set.before", testrdb_set_before,"deny-oom",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"testrdb.get.before", testrdb_get_before,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"testrdb.async_loading.get.before", testrdb_async_loading_get_before,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"testrdb.set.after", testrdb_set_after,"deny-oom",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"testrdb.get.after", testrdb_get_after,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"testrdb.set.key", testrdb_set_key,"deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"testrdb.get.key", testrdb_get_key,"",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_ReplAsyncLoad, replAsyncLoadCallback);

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    if (before_str)
        RedisModule_FreeString(ctx, before_str);
    if (after_str)
        RedisModule_FreeString(ctx, after_str);
    if (before_str_temp)
        RedisModule_FreeString(ctx, before_str_temp);
    if (after_str_temp)
        RedisModule_FreeString(ctx, after_str_temp);
    return REDISMODULE_OK;
}

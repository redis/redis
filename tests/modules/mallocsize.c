#include "redismodule.h"
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define UNUSED(V) ((void) V)

/* Registered type */
RedisModuleType *mallocsize_type = NULL;

typedef struct {
    void *raw;
    size_t raw_len;
    RedisModuleString *str;
    RedisModuleDict *d;
} rsd_t;

void rsd_free(void *value) {
    rsd_t *rsd = value;
    RedisModule_Free(rsd->raw);
    RedisModule_FreeString(NULL, rsd->str);
    RedisModuleString *dk, *dv;
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(rsd->d, "^", NULL, 0);
    while((dk = RedisModule_DictNext(NULL, iter, (void **)&dv)) != NULL) {
        RedisModule_FreeString(NULL, dk);
        RedisModule_FreeString(NULL, dv);
    }
    RedisModule_DictIteratorStop(iter);
    RedisModule_FreeDict(NULL, rsd->d);
    RedisModule_Free(rsd);
}

void *rsd_rdb_load(RedisModuleIO *rdb, int encver) {
    if (encver != 0)
        return NULL;
    rsd_t *rsd = RedisModule_Alloc(sizeof(*rsd));
    rsd->raw = RedisModule_LoadStringBuffer(rdb, &rsd->raw_len);
    rsd->str = RedisModule_LoadString(rdb);
    long long dict_len = RedisModule_LoadUnsigned(rdb);
    rsd->d = RedisModule_CreateDict(NULL);
    for (int i = 0; i < dict_len; i += 2) {
        RedisModuleString *key  = RedisModule_LoadString(rdb);
        RedisModuleString *val  = RedisModule_LoadString(rdb);
        RedisModule_DictSet(rsd->d, key, val);
        
    }

    return rsd;
}

void rsd_rdb_save(RedisModuleIO *rdb, void *value) {
    rsd_t *rsd = value;
    RedisModule_SaveStringBuffer(rdb, rsd->raw, rsd->raw_len);
    RedisModule_SaveString(rdb, rsd->str);
    RedisModule_SaveUnsigned(rdb, RedisModule_DictSize(rsd->d));
    RedisModuleString *dk, *dv;
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(rsd->d, "^", NULL, 0);
    while((dk = RedisModule_DictNext(NULL, iter, (void **)&dv)) != NULL) {
        RedisModule_SaveString(rdb, dk);
        RedisModule_SaveString(rdb, dv);
        RedisModule_FreeString(dk);
    }
    RedisModule_DictIteratorStop(iter);
}

size_t rsd_mem_usage(RedisModuleKeyOptCtx *ctx, const void *value, size_t sample_size) {
    UNUSED(ctx);
    UNUSED(sample_size);
    
    size_t size = 0;
    
    const rsd_t *rsd = value;
    size += RedisModule_MallocSize(rsd->raw);
    size += RedisModule_MallocSizeString(rsd->str);
    RedisModuleString *dk, *dv;
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(rsd->d, "^", NULL, 0);
    while((dk = RedisModule_DictNext(NULL, iter, (void **)&dv)) != NULL) {
        size += RedisModule_MallocSizeString(dk);
        size += RedisModule_MallocSizeString(dv);
        RedisModule_FreeString(dk);
    }
    RedisModule_DictIteratorStop(iter);
    
    return size;
}


int cmd_mallocsize_set(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc % 2)
        return RedisModule_WrongArity(ctx);
        
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    rsd_t *rsd = RedisModule_ModuleTypeGetValue(key);
    if (rsd) {
        rsd_free(rsd);
        rsd = NULL;
    }
    rsd = RedisModule_Alloc(sizeof(*rsd));
    
    /* raw */
    long long raw_len;
    RedisModule_StringToLongLong(argv[2], &raw_len);
    rsd->raw = RedisModule_Alloc(raw_len);
    rsd->raw_len = raw_len;

    /* string */
    rsd->str = argv[3];
    RedisModule_RetainString(ctx, argv[3]);
    
    /* dict */
    rsd->d = RedisModule_CreateDict(ctx);
    for (int i = 4; i < argc; i += 2) {
        RedisModule_DictSet(rsd->d, argv[i], argv[i+1]);
        RedisModule_RetainString(ctx, argv[i]);
        RedisModule_RetainString(ctx, argv[i+1]);
    }
        
    RedisModule_ModuleTypeSetValue(key, mallocsize_type, rsd);
    RedisModule_CloseKey(key);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    if (RedisModule_Init(ctx,"mallocsize",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;
        
    RedisModuleTypeMethods tm = {
        .version = REDISMODULE_TYPE_METHOD_VERSION,
        .rdb_load = rsd_rdb_load,
        .rdb_save = rsd_rdb_save,
        .free = rsd_free,
        .mem_usage2 = rsd_mem_usage,
    };

    mallocsize_type = RedisModule_CreateDataType(ctx, "allocsize", 1, &tm);
    if (mallocsize_type == NULL)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"mallocsize.set",cmd_mallocsize_set,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

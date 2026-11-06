#include "redismodule.h"
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define UNUSED(V) ((void) V)

/* Registered type */
RedisModuleType *mallocsize_type = NULL;

typedef enum {
    UDT_RAW,
    UDT_STRING,
    UDT_DICT
} udt_type_t;

typedef struct {
    void *ptr;
    size_t len;
} raw_t;

typedef struct {
    udt_type_t type;
    union {
        raw_t raw;
        RedisModuleString *str;
        RedisModuleDict *dict;
    } data;
} udt_t;

void udt_free(void *value) {
    udt_t *udt = value;
    switch (udt->type) {
        case (UDT_RAW): {
            RedisModule_Free(udt->data.raw.ptr);
            break;
        }
        case (UDT_STRING): {
            RedisModule_FreeString(NULL, udt->data.str);
            break;
        }
        case (UDT_DICT): {
            RedisModuleString *dk, *dv;
            RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(udt->data.dict, "^", NULL, 0);
            while((dk = RedisModule_DictNext(NULL, iter, (void **)&dv)) != NULL) {
                RedisModule_FreeString(NULL, dk);
                RedisModule_FreeString(NULL, dv);
            }
            RedisModule_DictIteratorStop(iter);
            RedisModule_FreeDict(NULL, udt->data.dict);
            break;
        }
    }
    RedisModule_Free(udt);
}

void udt_rdb_save(RedisModuleIO *rdb, void *value) {
    udt_t *udt = value;
    RedisModule_SaveUnsigned(rdb, udt->type);
    switch (udt->type) {
        case (UDT_RAW): {
            RedisModule_SaveStringBuffer(rdb, udt->data.raw.ptr, udt->data.raw.len);
            break;
        }
        case (UDT_STRING): {
            RedisModule_SaveString(rdb, udt->data.str);
            break;
        }
        case (UDT_DICT): {
            RedisModule_SaveUnsigned(rdb, RedisModule_DictSize(udt->data.dict));
            RedisModuleString *dk, *dv;
            RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(udt->data.dict, "^", NULL, 0);
            while((dk = RedisModule_DictNext(NULL, iter, (void **)&dv)) != NULL) {
                RedisModule_SaveString(rdb, dk);
                RedisModule_SaveString(rdb, dv);
                RedisModule_FreeString(NULL, dk); /* Allocated by RedisModule_DictNext */
            }
            RedisModule_DictIteratorStop(iter);
            break;
        }
    }
}

void *udt_rdb_load(RedisModuleIO *rdb, int encver) {
    if (encver != 0)
        return NULL;
    udt_t *udt = RedisModule_Alloc(sizeof(*udt));
    udt->type = RedisModule_LoadUnsigned(rdb);
    switch (udt->type) {
        case (UDT_RAW): {
            udt->data.raw.ptr = RedisModule_LoadStringBuffer(rdb, &udt->data.raw.len);
            break;
        }
        case (UDT_STRING): {
            udt->data.str = RedisModule_LoadString(rdb);
            break;
        }
        case (UDT_DICT): {
            long long dict_len = RedisModule_LoadUnsigned(rdb);
            udt->data.dict = RedisModule_CreateDict(NULL);
            for (int i = 0; i < dict_len; i += 2) {
                RedisModuleString *key = RedisModule_LoadString(rdb);
                RedisModuleString *val = RedisModule_LoadString(rdb);
                RedisModule_DictSet(udt->data.dict, key, val);
            }
            break;
        }
    }

    return udt;
}

size_t udt_mem_usage(RedisModuleKeyOptCtx *ctx, const void *value, size_t sample_size) {
    UNUSED(ctx);
    UNUSED(sample_size);
    
    const udt_t *udt = value;
    size_t size = sizeof(*udt);
    
    switch (udt->type) {
        case (UDT_RAW): {
            size += RedisModule_MallocSize(udt->data.raw.ptr);
            break;
        }
        case (UDT_STRING): {
            size += RedisModule_MallocSizeString(udt->data.str);
            break;
        }
        case (UDT_DICT): {
            void *dk;
            size_t keylen;
            RedisModuleString *dv;
            RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(udt->data.dict, "^", NULL, 0);
            while((dk = RedisModule_DictNextC(iter, &keylen, (void **)&dv)) != NULL) {
                size += keylen;
                size += RedisModule_MallocSizeString(dv);
            }
            RedisModule_DictIteratorStop(iter);
            break;
        }
    }
    
    return size;
}

/* MALLOCSIZE.SETRAW key len */
int cmd_setraw(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3)
        return RedisModule_WrongArity(ctx);
        
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);

    udt_t *udt = RedisModule_Alloc(sizeof(*udt));
    udt->type = UDT_RAW;
    
    long long raw_len;
    RedisModule_StringToLongLong(argv[2], &raw_len);
    udt->data.raw.ptr = RedisModule_Alloc(raw_len);
    udt->data.raw.len = raw_len;
    
    RedisModule_ModuleTypeSetValue(key, mallocsize_type, udt);
    RedisModule_CloseKey(key);

    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* MALLOCSIZE.SETSTR key string */
int cmd_setstr(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3)
        return RedisModule_WrongArity(ctx);
        
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);

    udt_t *udt = RedisModule_Alloc(sizeof(*udt));
    udt->type = UDT_STRING;
    
    udt->data.str = argv[2];
    RedisModule_RetainString(ctx, argv[2]);
    
    RedisModule_ModuleTypeSetValue(key, mallocsize_type, udt);
    RedisModule_CloseKey(key);

    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* MALLOCSIZE.SETDICT key field value [field value ...] */
int cmd_setdict(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 4 || argc % 2)
        return RedisModule_WrongArity(ctx);
        
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);

    udt_t *udt = RedisModule_Alloc(sizeof(*udt));
    udt->type = UDT_DICT;
    
    udt->data.dict = RedisModule_CreateDict(ctx);
    for (int i = 2; i < argc; i += 2) {
        RedisModule_DictSet(udt->data.dict, argv[i], argv[i+1]);
        /* No need to retain argv[i], it is copied as the rax key */
        RedisModule_RetainString(ctx, argv[i+1]);   
    }
    
    RedisModule_ModuleTypeSetValue(key, mallocsize_type, udt);
    RedisModule_CloseKey(key);

    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    if (RedisModule_Init(ctx,"mallocsize",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;
        
    RedisModuleTypeMethods tm = {
        .version = REDISMODULE_TYPE_METHOD_VERSION,
        .rdb_load = udt_rdb_load,
        .rdb_save = udt_rdb_save,
        .free = udt_free,
        .mem_usage2 = udt_mem_usage,
    };

    mallocsize_type = RedisModule_CreateDataType(ctx, "allocsize", 0, &tm);
    if (mallocsize_type == NULL)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "mallocsize.setraw", cmd_setraw, "", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
        
    if (RedisModule_CreateCommand(ctx, "mallocsize.setstr", cmd_setstr, "", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
        
    if (RedisModule_CreateCommand(ctx, "mallocsize.setdict", cmd_setdict, "", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

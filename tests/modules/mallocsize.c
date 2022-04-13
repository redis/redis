#include "redismodule.h"
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define UNUSED(V) ((void) V)

int cmd_raw(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2)
        return RedisModule_WrongArity(ctx);
        
    long long len;
    RedisModule_StringToLongLong(argv[1], &len);
    void *p = RedisModule_Alloc(len);
    long long size = RedisModule_MallocSize(p);
    RedisModule_Free(p);
    RedisModule_ReplyWithLongLong(ctx, size);
    return REDISMODULE_OK;
}

int cmd_string(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2)
        return RedisModule_WrongArity(ctx);
        
    void *s = RedisModule_CreateStringFromString(ctx, argv[1]);
    long long size = RedisModule_MallocSizeString(s);
    RedisModule_FreeString(ctx, s);
    RedisModule_ReplyWithLongLong(ctx, size);
    return REDISMODULE_OK;
}

int cmd_dict(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (!(argc % 2))
        return RedisModule_WrongArity(ctx);
        
    RedisModuleDict *d = RedisModule_CreateDict(ctx);
    for (int i = 1; i < argc; i += 2)
        RedisModule_DictSet(d, argv[i], argv[i+1]);
    long long size = RedisModule_MallocSizeDict(d);
    RedisModule_FreeDict(ctx, d);
    RedisModule_ReplyWithLongLong(ctx, size);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    if (RedisModule_Init(ctx,"mallocsize",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"mallocsize.raw",cmd_raw,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
        
    if (RedisModule_CreateCommand(ctx,"mallocsize.string",cmd_string,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
        
    if (RedisModule_CreateCommand(ctx,"mallocsize.dict",cmd_dict,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;


    return REDISMODULE_OK;
}

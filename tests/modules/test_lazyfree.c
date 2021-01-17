/* This module emulates a linked list for lazyfree testing of modules, which
 is a simplified version of 'hellotype.c'
 */
#include "redismodule.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdint.h>

static RedisModuleType *LazyFreeLinkType;

struct LazyFreeLinkNode {
    int64_t value;
    struct LazyFreeLinkNode *next;
};

struct LazyFreeLinkObject {
    struct LazyFreeLinkNode *head;
    size_t len; /* Number of elements added. */
};

struct LazyFreeLinkObject *createLazyFreeLinkObject(void) {
    struct LazyFreeLinkObject *o;
    o = RedisModule_Alloc(sizeof(*o));
    o->head = NULL;
    o->len = 0;
    return o;
}

void LazyFreeLinkInsert(struct LazyFreeLinkObject *o, int64_t ele) {
    struct LazyFreeLinkNode *next = o->head, *newnode, *prev = NULL;

    while(next && next->value < ele) {
        prev = next;
        next = next->next;
    }
    newnode = RedisModule_Alloc(sizeof(*newnode));
    newnode->value = ele;
    newnode->next = next;
    if (prev) {
        prev->next = newnode;
    } else {
        o->head = newnode;
    }
    o->len++;
}

void LazyFreeLinkReleaseObject(struct LazyFreeLinkObject *o) {
    struct LazyFreeLinkNode *cur, *next;
    cur = o->head;
    while(cur) {
        next = cur->next;
        RedisModule_Free(cur);
        cur = next;
    }
    RedisModule_Free(o);
}

/* LAZYFREELINK.INSERT key value */
int LazyFreeLinkInsert_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx); /* Use automatic memory management. */

    if (argc != 3) return RedisModule_WrongArity(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx,argv[1],
        REDISMODULE_READ|REDISMODULE_WRITE);
    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_EMPTY &&
        RedisModule_ModuleTypeGetType(key) != LazyFreeLinkType)
    {
        return RedisModule_ReplyWithError(ctx,REDISMODULE_ERRORMSG_WRONGTYPE);
    }

    long long value;
    if ((RedisModule_StringToLongLong(argv[2],&value) != REDISMODULE_OK)) {
        return RedisModule_ReplyWithError(ctx,"ERR invalid value: must be a signed 64 bit integer");
    }

    struct LazyFreeLinkObject *hto;
    if (type == REDISMODULE_KEYTYPE_EMPTY) {
        hto = createLazyFreeLinkObject();
        RedisModule_ModuleTypeSetValue(key,LazyFreeLinkType,hto);
    } else {
        hto = RedisModule_ModuleTypeGetValue(key);
    }

    LazyFreeLinkInsert(hto,value);
    RedisModule_SignalKeyAsReady(ctx,argv[1]);

    RedisModule_ReplyWithLongLong(ctx,hto->len);
    RedisModule_ReplicateVerbatim(ctx);
    return REDISMODULE_OK;
}

/* LAZYFREELINK.LEN key */
int LazyFreeLinkLen_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx); /* Use automatic memory management. */

    if (argc != 2) return RedisModule_WrongArity(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx,argv[1],
        REDISMODULE_READ|REDISMODULE_WRITE);
    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_EMPTY &&
        RedisModule_ModuleTypeGetType(key) != LazyFreeLinkType)
    {
        return RedisModule_ReplyWithError(ctx,REDISMODULE_ERRORMSG_WRONGTYPE);
    }

    struct LazyFreeLinkObject *hto = RedisModule_ModuleTypeGetValue(key);
    RedisModule_ReplyWithLongLong(ctx,hto ? hto->len : 0);
    return REDISMODULE_OK;
}

void *LazyFreeLinkRdbLoad(RedisModuleIO *rdb, int encver) {
    if (encver != 0) {
        return NULL;
    }
    uint64_t elements = RedisModule_LoadUnsigned(rdb);
    struct LazyFreeLinkObject *hto = createLazyFreeLinkObject();
    while(elements--) {
        int64_t ele = RedisModule_LoadSigned(rdb);
        LazyFreeLinkInsert(hto,ele);
    }
    return hto;
}

void LazyFreeLinkRdbSave(RedisModuleIO *rdb, void *value) {
    struct LazyFreeLinkObject *hto = value;
    struct LazyFreeLinkNode *node = hto->head;
    RedisModule_SaveUnsigned(rdb,hto->len);
    while(node) {
        RedisModule_SaveSigned(rdb,node->value);
        node = node->next;
    }
}

void LazyFreeLinkAofRewrite(RedisModuleIO *aof, RedisModuleString *key, void *value) {
    struct LazyFreeLinkObject *hto = value;
    struct LazyFreeLinkNode *node = hto->head;
    while(node) {
        RedisModule_EmitAOF(aof,"LAZYFREELINK.INSERT","sl",key,node->value);
        node = node->next;
    }
}

void LazyFreeLinkFree(void *value) {
    LazyFreeLinkReleaseObject(value);
}

size_t LazyFreeLinkFreeEffort(RedisModuleString *key, const void *value) {
    REDISMODULE_NOT_USED(key);
    const struct LazyFreeLinkObject *hto = value;
    return hto->len;
}

void LazyFreeLinkUnlink(RedisModuleString *key, const void *value) {
    REDISMODULE_NOT_USED(key);
    REDISMODULE_NOT_USED(value);
    /* Here you can know which key and value is about to be freed. */
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"lazyfreetest",1,REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    /* We only allow our module to be loaded when the redis core version is greater than the version of my module */
    if (RedisModule_GetTypeMethodVersion() < REDISMODULE_TYPE_METHOD_VERSION) {
        return REDISMODULE_ERR;
    }

    RedisModuleTypeMethods tm = {
        .version = REDISMODULE_TYPE_METHOD_VERSION,
        .rdb_load = LazyFreeLinkRdbLoad,
        .rdb_save = LazyFreeLinkRdbSave,
        .aof_rewrite = LazyFreeLinkAofRewrite,
        .free = LazyFreeLinkFree,
        .free_effort = LazyFreeLinkFreeEffort,
        .unlink = LazyFreeLinkUnlink,
    };

    LazyFreeLinkType = RedisModule_CreateDataType(ctx,"test_lazy",0,&tm);
    if (LazyFreeLinkType == NULL) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"lazyfreelink.insert",
        LazyFreeLinkInsert_RedisCommand,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"lazyfreelink.len",
        LazyFreeLinkLen_RedisCommand,"readonly",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

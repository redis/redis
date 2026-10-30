/* This module is used to test the server events hooks API.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2019-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "redismodule.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <assert.h>

/* We need to store events to be able to test and see what we got, and we can't
 * store them in the key-space since that would mess up rdb loading (duplicates)
 * and be lost of flushdb. */
RedisModuleDict *event_log = NULL;
/* stores all the keys on which we got 'removed' event */
RedisModuleDict *removed_event_log = NULL;
/* stores all the subevent on which we got 'removed' event */
RedisModuleDict *removed_subevent_type = NULL;
/* stores all the keys on which we got 'removed' event with expiry information */
RedisModuleDict *removed_expiry_log = NULL;

typedef struct EventElement {
    long count;
    RedisModuleString *last_val_string;
    long last_val_int;
} EventElement;

void LogStringEvent(RedisModuleCtx *ctx, const char* keyname, const char* data) {
    EventElement *event = RedisModule_DictGetC(event_log, (void*)keyname, strlen(keyname), NULL);
    if (!event) {
        event = RedisModule_Alloc(sizeof(EventElement));
        memset(event, 0, sizeof(EventElement));
        RedisModule_DictSetC(event_log, (void*)keyname, strlen(keyname), event);
    }
    if (event->last_val_string) RedisModule_FreeString(ctx, event->last_val_string);
    event->last_val_string = RedisModule_CreateString(ctx, data, strlen(data));
    event->count++;
}

void LogNumericEvent(RedisModuleCtx *ctx, const char* keyname, long data) {
    REDISMODULE_NOT_USED(ctx);
    EventElement *event = RedisModule_DictGetC(event_log, (void*)keyname, strlen(keyname), NULL);
    if (!event) {
        event = RedisModule_Alloc(sizeof(EventElement));
        memset(event, 0, sizeof(EventElement));
        RedisModule_DictSetC(event_log, (void*)keyname, strlen(keyname), event);
    }
    event->last_val_int = data;
    event->count++;
}

void FreeEvent(RedisModuleCtx *ctx, EventElement *event) {
    if (event->last_val_string)
        RedisModule_FreeString(ctx, event->last_val_string);
    RedisModule_Free(event);
}

int cmdEventCount(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2){
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    EventElement *event = RedisModule_DictGet(event_log, argv[1], NULL);
    RedisModule_ReplyWithLongLong(ctx, event? event->count: 0);
    return REDISMODULE_OK;
}

int cmdEventLast(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2){
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    EventElement *event = RedisModule_DictGet(event_log, argv[1], NULL);
    if (event && event->last_val_string)
        RedisModule_ReplyWithString(ctx, event->last_val_string);
    else if (event)
        RedisModule_ReplyWithLongLong(ctx, event->last_val_int);
    else
        RedisModule_ReplyWithNull(ctx);
    return REDISMODULE_OK;
}

void clearEvents(RedisModuleCtx *ctx)
{
    RedisModuleString *key;
    EventElement *event;
    RedisModuleDictIter *iter = RedisModule_DictIteratorStart(event_log, "^", NULL);
    while((key = RedisModule_DictNext(ctx, iter, (void**)&event)) != NULL) {
        event->count = 0;
        event->last_val_int = 0;
        if (event->last_val_string) RedisModule_FreeString(ctx, event->last_val_string);
        event->last_val_string = NULL;
        RedisModule_DictDel(event_log, key, NULL);
        RedisModule_Free(event);
    }
    RedisModule_DictIteratorStop(iter);
}

int cmdEventsClear(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    REDISMODULE_NOT_USED(argc);
    REDISMODULE_NOT_USED(argv);
    clearEvents(ctx);
    return REDISMODULE_OK;
}

/* Client state change callback. */
void clientChangeCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(e);

    RedisModuleClientInfo *ci = data;
    char *keyname = (sub == REDISMODULE_SUBEVENT_CLIENT_CHANGE_CONNECTED) ?
        "client-connected" : "client-disconnected";
    LogNumericEvent(ctx, keyname, ci->id);
}

void flushdbCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(e);

    RedisModuleFlushInfo *fi = data;
    char *keyname = (sub == REDISMODULE_SUBEVENT_FLUSHDB_START) ?
        "flush-start" : "flush-end";
    LogNumericEvent(ctx, keyname, fi->dbnum);
}

void roleChangeCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(e);
    REDISMODULE_NOT_USED(data);

    RedisModuleReplicationInfo *ri = data;
    char *keyname = (sub == REDISMODULE_EVENT_REPLROLECHANGED_NOW_MASTER) ?
        "role-master" : "role-replica";
    LogStringEvent(ctx, keyname, ri->masterhost);
}

void replicationChangeCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(e);
    REDISMODULE_NOT_USED(data);

    char *keyname = (sub == REDISMODULE_SUBEVENT_REPLICA_CHANGE_ONLINE) ?
        "replica-online" : "replica-offline";
    LogNumericEvent(ctx, keyname, 0);
}

void rasterLinkChangeCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(e);
    REDISMODULE_NOT_USED(data);

    char *keyname = (sub == REDISMODULE_SUBEVENT_MASTER_LINK_UP) ?
        "masterlink-up" : "masterlink-down";
    LogNumericEvent(ctx, keyname, 0);
}

void persistenceCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(e);
    REDISMODULE_NOT_USED(data);

    char *keyname = NULL;
    switch (sub) {
        case REDISMODULE_SUBEVENT_PERSISTENCE_RDB_START: keyname = "persistence-rdb-start"; break;
        case REDISMODULE_SUBEVENT_PERSISTENCE_AOF_START: keyname = "persistence-aof-start"; break;
        case REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_AOF_START: keyname = "persistence-syncaof-start"; break;
        case REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_RDB_START: keyname = "persistence-syncrdb-start"; break;
        case REDISMODULE_SUBEVENT_PERSISTENCE_ENDED: keyname = "persistence-end"; break;
        case REDISMODULE_SUBEVENT_PERSISTENCE_FAILED: keyname = "persistence-failed"; break;
    }
    /* modifying the keyspace from the fork child is not an option, using log instead */
    RedisModule_Log(ctx, "warning", "module-event-%s", keyname);
    if (sub == REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_RDB_START ||
        sub == REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_AOF_START) 
    {
        LogNumericEvent(ctx, keyname, 0);
    }
}

void loadingCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(e);
    REDISMODULE_NOT_USED(data);

    char *keyname = NULL;
    switch (sub) {
        case REDISMODULE_SUBEVENT_LOADING_RDB_START: keyname = "loading-rdb-start"; break;
        case REDISMODULE_SUBEVENT_LOADING_AOF_START: keyname = "loading-aof-start"; break;
        case REDISMODULE_SUBEVENT_LOADING_REPL_START: keyname = "loading-repl-start"; break;
        case REDISMODULE_SUBEVENT_LOADING_ENDED: keyname = "loading-end"; break;
        case REDISMODULE_SUBEVENT_LOADING_FAILED: keyname = "loading-failed"; break;
    }
    LogNumericEvent(ctx, keyname, 0);
}

void loadingProgressCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(e);

    RedisModuleLoadingProgress *ei = data;
    char *keyname = (sub == REDISMODULE_SUBEVENT_LOADING_PROGRESS_RDB) ?
        "loading-progress-rdb" : "loading-progress-aof";
    LogNumericEvent(ctx, keyname, ei->progress);
}

void shutdownCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(e);
    REDISMODULE_NOT_USED(data);
    REDISMODULE_NOT_USED(sub);

    RedisModule_Log(ctx, "warning", "module-event-%s", "shutdown");
}

void cronLoopCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(e);
    REDISMODULE_NOT_USED(sub);

    RedisModuleCronLoop *ei = data;
    LogNumericEvent(ctx, "cron-loop", ei->hz);
}

void moduleChangeCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(e);

    RedisModuleModuleChange *ei = data;
    char *keyname = (sub == REDISMODULE_SUBEVENT_MODULE_LOADED) ?
        "module-loaded" : "module-unloaded";
    LogStringEvent(ctx, keyname, ei->module_name);
}

void swapDbCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(e);
    REDISMODULE_NOT_USED(sub);

    RedisModuleSwapDbInfo *ei = data;
    LogNumericEvent(ctx, "swapdb-first", ei->dbnum_first);
    LogNumericEvent(ctx, "swapdb-second", ei->dbnum_second);
}

void configChangeCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(e);
    if (sub != REDISMODULE_SUBEVENT_CONFIG_CHANGE) {
        return;
    }

    RedisModuleConfigChangeV1 *ei = data;
    LogNumericEvent(ctx, "config-change-count", ei->num_changes);
    LogStringEvent(ctx, "config-change-first", ei->config_names[0]);
}

void keyInfoCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(e);

    RedisModuleKeyInfoV1 *ei = data;
    RedisModuleKey *kp = ei->key;
    RedisModuleString *key = (RedisModuleString *) RedisModule_GetKeyNameFromModuleKey(kp);
    const char *keyname = RedisModule_StringPtrLen(key, NULL);
    RedisModuleString *event_keyname = RedisModule_CreateStringPrintf(ctx, "key-info-%s", keyname);
    LogStringEvent(ctx, RedisModule_StringPtrLen(event_keyname, NULL), keyname);
    RedisModule_FreeString(ctx, event_keyname);

    /* Despite getting a key object from the callback, we also try to re-open it
     * to make sure the callback is called before it is actually removed from the keyspace. */
    RedisModuleKey *kp_open = RedisModule_OpenKey(ctx, key, REDISMODULE_READ);
    assert(RedisModule_ValueLength(kp) == RedisModule_ValueLength(kp_open));
    RedisModule_CloseKey(kp_open);

    /* We also try to RM_Call a command that accesses that key, also to make sure it's still in the keyspace. */
    char *size_command = NULL;
    int key_type = RedisModule_KeyType(kp);
    if (key_type == REDISMODULE_KEYTYPE_STRING) {
        size_command = "STRLEN";
    } else if (key_type == REDISMODULE_KEYTYPE_LIST) {
        size_command = "LLEN";
    } else if (key_type == REDISMODULE_KEYTYPE_HASH) {
        size_command = "HLEN";
    } else if (key_type == REDISMODULE_KEYTYPE_SET) {
        size_command = "SCARD";
    } else if (key_type == REDISMODULE_KEYTYPE_ZSET) {
        size_command = "ZCARD";
    } else if (key_type == REDISMODULE_KEYTYPE_STREAM) {
        size_command = "XLEN";
    }
    if (size_command != NULL) {
        RedisModuleCallReply *reply = RedisModule_Call(ctx, size_command, "s", key);
        assert(reply != NULL);
        assert(RedisModule_ValueLength(kp) == (size_t) RedisModule_CallReplyInteger(reply));
        RedisModule_FreeCallReply(reply);
    }

    /* Now use the key object we got from the callback for various validations. */
    RedisModuleString *prev = RedisModule_DictGetC(removed_event_log, (void*)keyname, strlen(keyname), NULL);
    /* We keep object length */
    RedisModuleString *v = RedisModule_CreateStringPrintf(ctx, "%zd", RedisModule_ValueLength(kp));
    /* For string type, we keep value instead of length */
    if (RedisModule_KeyType(kp) == REDISMODULE_KEYTYPE_STRING) {
        RedisModule_FreeString(ctx, v);
        size_t len;
        /* We need to access the string value with RedisModule_StringDMA.
         * RedisModule_StringDMA may call dbUnshareStringValue to free the origin object,
         * so we also can test it. */
        char *s = RedisModule_StringDMA(kp, &len, REDISMODULE_READ);
        v = RedisModule_CreateString(ctx, s, len);
    }
    RedisModule_DictReplaceC(removed_event_log, (void*)keyname, strlen(keyname), v);
    if (prev != NULL) {
        RedisModule_FreeString(ctx, prev);
    }

    const char *subevent = "deleted";
    if (sub == REDISMODULE_SUBEVENT_KEY_EXPIRED) {
        subevent = "expired";
    } else if (sub == REDISMODULE_SUBEVENT_KEY_EVICTED) {
        subevent = "evicted";
    } else if (sub == REDISMODULE_SUBEVENT_KEY_OVERWRITTEN) {
        subevent = "overwritten";
    }
    RedisModule_DictReplaceC(removed_subevent_type, (void*)keyname, strlen(keyname), (void *)subevent);

    RedisModuleString *prevexpire = RedisModule_DictGetC(removed_expiry_log, (void*)keyname, strlen(keyname), NULL);
    RedisModuleString *expire = RedisModule_CreateStringPrintf(ctx, "%lld", RedisModule_GetAbsExpire(kp));
    RedisModule_DictReplaceC(removed_expiry_log, (void*)keyname, strlen(keyname), (void *)expire);
    if (prevexpire != NULL) {
        RedisModule_FreeString(ctx, prevexpire);
    }
}

static int cmdIsKeyRemoved(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    if(argc != 2){
        return RedisModule_WrongArity(ctx);
    }

    const char *key  = RedisModule_StringPtrLen(argv[1], NULL);

    RedisModuleString *value = RedisModule_DictGetC(removed_event_log, (void*)key, strlen(key), NULL);

    if (value == NULL) {
        return RedisModule_ReplyWithError(ctx, "ERR Key was not removed");
    }

    const char *subevent = RedisModule_DictGetC(removed_subevent_type, (void*)key, strlen(key), NULL);
    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithString(ctx, value);
    RedisModule_ReplyWithSimpleString(ctx, subevent);

    return REDISMODULE_OK;
}

static int cmdKeyExpiry(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    if(argc != 2){
        return RedisModule_WrongArity(ctx);
    }

    const char* key  = RedisModule_StringPtrLen(argv[1], NULL);
    RedisModuleString *expire = RedisModule_DictGetC(removed_expiry_log, (void*)key, strlen(key), NULL);
    if (expire == NULL) {
        return RedisModule_ReplyWithError(ctx, "ERR Key was not removed");
    }
    RedisModule_ReplyWithString(ctx, expire);
    return REDISMODULE_OK;
}

/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
#define VerifySubEventSupported(e, s) \
    if (!RedisModule_IsSubEventSupported(e, s)) { \
        return REDISMODULE_ERR; \
    }

    if (RedisModule_Init(ctx,"testhook",1,REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    /* Example on how to check if a server sub event is supported */
    if (!RedisModule_IsSubEventSupported(RedisModuleEvent_ReplicationRoleChanged, REDISMODULE_EVENT_REPLROLECHANGED_NOW_MASTER)) {
        return REDISMODULE_ERR;
    }

    /* replication related hooks */
    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_ReplicationRoleChanged, roleChangeCallback);
    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_ReplicaChange, replicationChangeCallback);
    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_MasterLinkChange, rasterLinkChangeCallback);

    /* persistence related hooks */
    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_Persistence, persistenceCallback);
    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_Loading, loadingCallback);
    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_LoadingProgress, loadingProgressCallback);

    /* other hooks */
    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_ClientChange, clientChangeCallback);
    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_FlushDB, flushdbCallback);
    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_Shutdown, shutdownCallback);
    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_CronLoop, cronLoopCallback);

    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_ModuleChange, moduleChangeCallback);
    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_SwapDB, swapDbCallback);

    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_Config, configChangeCallback);

    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_Key, keyInfoCallback);

    event_log = RedisModule_CreateDict(ctx);
    removed_event_log = RedisModule_CreateDict(ctx);
    removed_subevent_type = RedisModule_CreateDict(ctx);
    removed_expiry_log = RedisModule_CreateDict(ctx);

    if (RedisModule_CreateCommand(ctx,"hooks.event_count", cmdEventCount,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"hooks.event_last", cmdEventLast,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"hooks.clear", cmdEventsClear,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"hooks.is_key_removed", cmdIsKeyRemoved,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"hooks.pexpireat", cmdKeyExpiry,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (argc == 1) {
        const char *ptr = RedisModule_StringPtrLen(argv[0], NULL);
        if (!strcasecmp(ptr, "noload")) {
            /* This is a hint that we return ERR at the last moment of OnLoad. */
            RedisModule_FreeDict(ctx, event_log);
            RedisModule_FreeDict(ctx, removed_event_log);
            RedisModule_FreeDict(ctx, removed_subevent_type);
            RedisModule_FreeDict(ctx, removed_expiry_log);
            return REDISMODULE_ERR;
        }
    }

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    clearEvents(ctx);
    RedisModule_FreeDict(ctx, event_log);
    event_log = NULL;

    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(removed_event_log, "^", NULL, 0);
    char* key;
    size_t keyLen;
    RedisModuleString* val;
    while((key = RedisModule_DictNextC(iter, &keyLen, (void**)&val))){
        RedisModule_FreeString(ctx, val);
    }
    RedisModule_FreeDict(ctx, removed_event_log);
    RedisModule_DictIteratorStop(iter);
    removed_event_log = NULL;

    RedisModule_FreeDict(ctx, removed_subevent_type);
    removed_subevent_type = NULL;

    iter = RedisModule_DictIteratorStartC(removed_expiry_log, "^", NULL, 0);
    while((key = RedisModule_DictNextC(iter, &keyLen, (void**)&val))){
        RedisModule_FreeString(ctx, val);
    }
    RedisModule_FreeDict(ctx, removed_expiry_log);
    RedisModule_DictIteratorStop(iter);
    removed_expiry_log = NULL;

    return REDISMODULE_OK;
}


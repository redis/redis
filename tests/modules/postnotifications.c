/* This module is used to test the server post keyspace jobs API.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2020-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* This module supports both the regular post-notification API
 * (RedisModule_AddPostNotificationJob) and the per-key API
 * (RedisModule_AddPostNotificationJobForKey). A load arg —
 * "regular" or "perkey" — selects which API the keyspace handlers register
 * against (defaults to "regular" if omitted). The keyspace handlers and post-job
 * side effects are otherwise unchanged: only the registration call differs
 * between modes. */

#define _BSD_SOURCE
#define _DEFAULT_SOURCE /* For usleep */

#include "redismodule.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

enum api_mode { MODE_REGULAR, MODE_PERKEY };
static int g_api_mode = MODE_REGULAR;

static void KeySpace_PostNotificationStringFreePD(void *pd) {
    RedisModule_FreeString(NULL, pd);
}

static void KeySpace_PostNotificationReadKey(RedisModuleCtx *ctx, void *pd) {
    RedisModuleCallReply* rep = RedisModule_Call(ctx, "get", "!s", pd);
    RedisModule_FreeCallReply(rep);
}

static void KeySpace_PostNotificationString(RedisModuleCtx *ctx, void *pd) {
    REDISMODULE_NOT_USED(ctx);
    RedisModuleCallReply* rep = RedisModule_Call(ctx, "incr", "!s", pd);
    RedisModule_FreeCallReply(rep);
}

/* Per-key-API trampolines: the per-key API's callback takes an extra `key`
 * argument; we ignore it and delegate to the regular-API callback so the
 * post-job effect stays identical across modes. */
static void KeySpace_PostNotificationStringPerKey(RedisModuleCtx *ctx, RedisModuleString *key, void *pd) {
    REDISMODULE_NOT_USED(key);
    KeySpace_PostNotificationString(ctx, pd);
}

static void KeySpace_PostNotificationReadKeyPerKey(RedisModuleCtx *ctx, RedisModuleString *key, void *pd) {
    REDISMODULE_NOT_USED(key);
    KeySpace_PostNotificationReadKey(ctx, pd);
}

/* Register a post-notification job through the API selected by g_api_mode. */
static int RegisterIncrJob(RedisModuleCtx *ctx, RedisModuleString *trigger_key, RedisModuleString *target) {
    if (g_api_mode == MODE_REGULAR) {
        return RedisModule_AddPostNotificationJob(ctx, KeySpace_PostNotificationString, target, KeySpace_PostNotificationStringFreePD);
    } else {
        return RedisModule_AddPostNotificationJobForKey(ctx, KeySpace_PostNotificationStringPerKey, trigger_key, target, KeySpace_PostNotificationStringFreePD);
    }
}

static int RegisterGetJob(RedisModuleCtx *ctx, RedisModuleString *trigger_key, RedisModuleString *target) {
    if (g_api_mode == MODE_REGULAR) {
        return RedisModule_AddPostNotificationJob(ctx, KeySpace_PostNotificationReadKey, target, KeySpace_PostNotificationStringFreePD);
    } else {
        return RedisModule_AddPostNotificationJobForKey(ctx, KeySpace_PostNotificationReadKeyPerKey, trigger_key, target, KeySpace_PostNotificationStringFreePD);
    }
}

static int KeySpace_NotificationExpired(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key){
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    RedisModuleString *new_key = RedisModule_CreateString(NULL, "expired", 7);
    int res = RegisterIncrJob(ctx, key, new_key);
    if (res == REDISMODULE_ERR) KeySpace_PostNotificationStringFreePD(new_key);
    return REDISMODULE_OK;
}

static int KeySpace_NotificationEvicted(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key){
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);
    REDISMODULE_NOT_USED(key);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);

    if (strncmp(key_str, "evicted", 7) == 0) {
        return REDISMODULE_OK; /* do not count the evicted key */
    }

    if (strncmp(key_str, "before_evicted", 14) == 0) {
        return REDISMODULE_OK; /* do not count the before_evicted key */
    }

    RedisModuleString *new_key = RedisModule_CreateString(NULL, "evicted", 7);
    int res = RegisterIncrJob(ctx, key, new_key);
    if (res == REDISMODULE_ERR) KeySpace_PostNotificationStringFreePD(new_key);
    return REDISMODULE_OK;
}

static int KeySpace_NotificationString(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key){
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);

    if (strncmp(key_str, "string_", 7) != 0) {
        return REDISMODULE_OK;
    }

    if (strcmp(key_str, "string_total") == 0) {
        return REDISMODULE_OK;
    }

    RedisModuleString *new_key;
    if (strncmp(key_str, "string_changed{", 15) == 0) {
        new_key = RedisModule_CreateString(NULL, "string_total", 12);
    } else {
        new_key = RedisModule_CreateStringPrintf(NULL, "string_changed{%s}", key_str);
    }

    int res = RegisterIncrJob(ctx, key, new_key);
    if (res == REDISMODULE_ERR) KeySpace_PostNotificationStringFreePD(new_key);
    return REDISMODULE_OK;
}

static int KeySpace_LazyExpireInsidePostNotificationJob(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key){
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);

    if (strncmp(key_str, "read_", 5) != 0) {
        return REDISMODULE_OK;
    }

    RedisModuleString *new_key = RedisModule_CreateString(NULL, key_str + 5, strlen(key_str) - 5);;
    int res = RegisterGetJob(ctx, key, new_key);
    if (res == REDISMODULE_ERR) KeySpace_PostNotificationStringFreePD(new_key);
    return REDISMODULE_OK;
}

static int KeySpace_NestedNotification(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key){
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);

    if (strncmp(key_str, "write_sync_", 11) != 0) {
        return REDISMODULE_OK;
    }

    /* This test was only meant to check REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS.
     * In general it is wrong and discourage to perform any writes inside a notification callback.  */
    RedisModuleString *new_key = RedisModule_CreateString(NULL, key_str + 11, strlen(key_str) - 11);;
    RedisModuleCallReply* rep = RedisModule_Call(ctx, "set", "!sc", new_key, "1");
    RedisModule_FreeCallReply(rep);
    RedisModule_FreeString(NULL, new_key);
    return REDISMODULE_OK;
}

typedef struct AsyncSetArgs {
    RedisModuleBlockedClient *bc;
    RedisModuleString *key;
} AsyncSetArgs;

static void *KeySpace_PostNotificationsAsyncSetInner(void *arg) {
    AsyncSetArgs *args = arg;
    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(args->bc);
    RedisModule_ThreadSafeContextLock(ctx);
    RedisModuleCallReply* rep = RedisModule_Call(ctx, "set", "!sc", args->key, "1");
    RedisModule_ThreadSafeContextUnlock(ctx);
    RedisModule_ReplyWithCallReply(ctx, rep);
    RedisModule_FreeCallReply(rep);

    RedisModule_UnblockClient(args->bc, NULL);
    RedisModule_FreeThreadSafeContext(ctx);
    RedisModule_FreeString(NULL, args->key);
    RedisModule_Free(args);
    return NULL;
}

static int KeySpace_PostNotificationsAsyncSet(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2)
        return RedisModule_WrongArity(ctx);

    AsyncSetArgs *args = RedisModule_Alloc(sizeof(*args));
    args->bc = RedisModule_BlockClient(ctx,NULL,NULL,NULL,0);
    args->key = RedisModule_HoldString(NULL, argv[1]);

    pthread_t tid;
    if (pthread_create(&tid,NULL,KeySpace_PostNotificationsAsyncSetInner,args) != 0) {
        RedisModule_AbortBlock(args->bc);
        RedisModule_FreeString(NULL, args->key);
        RedisModule_Free(args);
        return RedisModule_ReplyWithError(ctx,"-ERR Can't start thread");
    }
    pthread_detach(tid);
    return REDISMODULE_OK;
}

typedef struct KeySpace_EventPostNotificationCtx {
    RedisModuleString *triggered_on;
    RedisModuleString *new_key;
} KeySpace_EventPostNotificationCtx;

static void KeySpace_ServerEventPostNotificationFree(void *pd) {
    KeySpace_EventPostNotificationCtx *pn_ctx = pd;
    RedisModule_FreeString(NULL, pn_ctx->new_key);
    RedisModule_FreeString(NULL, pn_ctx->triggered_on);
    RedisModule_Free(pn_ctx);
}

static void KeySpace_ServerEventPostNotification(RedisModuleCtx *ctx, void *pd) {
    REDISMODULE_NOT_USED(ctx);
    KeySpace_EventPostNotificationCtx *pn_ctx = pd;
    RedisModuleCallReply* rep = RedisModule_Call(ctx, "lpush", "!ss", pn_ctx->new_key, pn_ctx->triggered_on);
    RedisModule_FreeCallReply(rep);
}

static void KeySpace_ServerEventCallback(RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent, void *data) {
    REDISMODULE_NOT_USED(eid);
    REDISMODULE_NOT_USED(data);
    if (subevent > 3) {
        RedisModule_Log(ctx, "warning", "Got an unexpected subevent '%llu'", (unsigned long long)subevent);
        return;
    }
    static const char* events[] = {
            "before_deleted",
            "before_expired",
            "before_evicted",
            "before_overwritten",
    };

    const RedisModuleString *key_name = RedisModule_GetKeyNameFromModuleKey(((RedisModuleKeyInfo*)data)->key);
    const char *key_str = RedisModule_StringPtrLen(key_name, NULL);

    for (int i = 0 ; i < 4 ; ++i) {
        const char *event = events[i];
        if (strncmp(key_str, event , strlen(event)) == 0) {
            return; /* don't log any event on our tracking keys */
        }
    }

    KeySpace_EventPostNotificationCtx *pn_ctx = RedisModule_Alloc(sizeof(*pn_ctx));
    pn_ctx->triggered_on = RedisModule_HoldString(NULL, (RedisModuleString*)key_name);
    pn_ctx->new_key = RedisModule_CreateString(NULL, events[subevent], strlen(events[subevent]));
    int res = RedisModule_AddPostNotificationJob(ctx, KeySpace_ServerEventPostNotification, pn_ctx, KeySpace_ServerEventPostNotificationFree);
    if (res == REDISMODULE_ERR) KeySpace_ServerEventPostNotificationFree(pn_ctx);
}

/* Per-key-only fixtures: behaviors with no regular-API equivalent. Registered
 * only when the module is loaded in "perkey" mode.
 *
 * NOTE: these callbacks intentionally WRITE to the keyspace (RM_Call "!...")
 * from inside a per-key job. That deliberately VIOLATES the documented
 * RM_AddPostNotificationJobForKey contract (callbacks must touch only
 * non-replicated state). The violation is the point: the keyspace write is
 * what makes the firing order/granularity observable in
 * assert_replication_stream on a standalone master. These fixtures are valid
 * ONLY on a single master; they must never be exercised under a replica or
 * AOF-consistency assertion, where they would (correctly) amplify the AOF and
 * diverge the replica. For cross-phase / AOF / replica testing use the
 * separate postnotifications_perkey_metadata.c module, whose callback touches
 * only non-replicated key metadata. */

static void KeySpace_PostNotificationBatchedKey(RedisModuleCtx *ctx, RedisModuleString *key, void *pd) {
    REDISMODULE_NOT_USED(pd);
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "lpush", "!cs", "batched_keys", key);
    if (rep) RedisModule_FreeCallReply(rep);
}

static int KeySpace_NotificationBatched(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);
    if (strncmp(key_str, "batched_", 8) != 0) return REDISMODULE_OK;
    if (strcmp(key_str, "batched_keys") == 0) return REDISMODULE_OK; /* skip our sink list */

    RedisModule_AddPostNotificationJobForKey(ctx, KeySpace_PostNotificationBatchedKey, key, NULL, NULL);
    return REDISMODULE_OK;
}

static void KeySpace_PostNotificationHashKey(RedisModuleCtx *ctx, RedisModuleString *key, void *pd) {
    REDISMODULE_NOT_USED(pd);
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "lpush", "!cs", "hash_keys", key);
    if (rep) RedisModule_FreeCallReply(rep);
}

static int KeySpace_NotificationHash(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);
    if (strncmp(key_str, "hash_", 5) != 0) return REDISMODULE_OK;

    RedisModule_AddPostNotificationJobForKey(ctx, KeySpace_PostNotificationHashKey, key, NULL, NULL);
    return REDISMODULE_OK;
}

static int reentrance_in_outer_callback = 0;

static void KeySpace_PostNotificationReentranceProbe(RedisModuleCtx *ctx, RedisModuleString *key, void *pd) {
    REDISMODULE_NOT_USED(pd);
    const char *key_str = RedisModule_StringPtrLen(key, NULL);
    RedisModuleCallReply *rep;

    if (strcmp(key_str, "reentrant_outer") == 0) {
        reentrance_in_outer_callback = 1;
        rep = RedisModule_Call(ctx, "set", "!cc", "reentrant_inner", "1");
        if (rep) RedisModule_FreeCallReply(rep);
        reentrance_in_outer_callback = 0;
        rep = RedisModule_Call(ctx, "lpush", "!cc", "reentrance_log", "outer_done");
        if (rep) RedisModule_FreeCallReply(rep);
    } else if (strcmp(key_str, "reentrant_inner") == 0) {
        const char *marker = reentrance_in_outer_callback ? "REENTRANCE_DETECTED" : "inner_after_outer";
        rep = RedisModule_Call(ctx, "lpush", "!cc", "reentrance_log", marker);
        if (rep) RedisModule_FreeCallReply(rep);
    }
}

static int KeySpace_NotificationReentrance(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);
    if (strncmp(key_str, "reentrant_", 10) != 0) return REDISMODULE_OK;

    RedisModule_AddPostNotificationJobForKey(ctx, KeySpace_PostNotificationReentranceProbe, key, NULL, NULL);
    return REDISMODULE_OK;
}

static void KeySpace_PostNotificationMissKey(RedisModuleCtx *ctx, RedisModuleString *key, void *pd) {
    REDISMODULE_NOT_USED(pd);
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "lpush", "!cs", "mget_misses", key);
    if (rep) RedisModule_FreeCallReply(rep);
}

static int KeySpace_NotificationMiss(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);
    if (strncmp(key_str, "miss_", 5) != 0) return REDISMODULE_OK;

    RedisModule_AddPostNotificationJobForKey(ctx, KeySpace_PostNotificationMissKey, key, NULL, NULL);
    return REDISMODULE_OK;
}

/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx,"postnotifications",1,REDISMODULE_APIVER_1) == REDISMODULE_ERR){
        return REDISMODULE_ERR;
    }

    if (!(RedisModule_GetModuleOptionsAll() & REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS)) {
        return REDISMODULE_ERR;
    }

    int with_key_events = 0;
    for (int i = 0; i < argc; i++) {
        const char *arg = RedisModule_StringPtrLen(argv[i], 0);
        if (strcmp(arg, "with_key_events") == 0) {
            with_key_events = 1;
        } else if (strcmp(arg, "perkey") == 0) {
            g_api_mode = MODE_PERKEY;
        } else if (strcmp(arg, "regular") == 0) {
            g_api_mode = MODE_REGULAR;
        }
    }

    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS);

    if(RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_STRING, KeySpace_NotificationString) != REDISMODULE_OK){
        return REDISMODULE_ERR;
    }

    if(RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_STRING, KeySpace_LazyExpireInsidePostNotificationJob) != REDISMODULE_OK){
        return REDISMODULE_ERR;
    }

    if(RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_STRING, KeySpace_NestedNotification) != REDISMODULE_OK){
        return REDISMODULE_ERR;
    }

    if(RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_EXPIRED, KeySpace_NotificationExpired) != REDISMODULE_OK){
        return REDISMODULE_ERR;
    }

    if(RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_EVICTED, KeySpace_NotificationEvicted) != REDISMODULE_OK){
        return REDISMODULE_ERR;
    }

    if (with_key_events) {
        if(RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_Key, KeySpace_ServerEventCallback) != REDISMODULE_OK){
            return REDISMODULE_ERR;
        }
    }

    /* Per-key-only fixtures (behaviors with no regular API equivalent). */
    if (g_api_mode == MODE_PERKEY) {
        if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_STRING, KeySpace_NotificationBatched) != REDISMODULE_OK) {
            return REDISMODULE_ERR;
        }
        if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_HASH, KeySpace_NotificationHash) != REDISMODULE_OK) {
            return REDISMODULE_ERR;
        }
        if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_STRING, KeySpace_NotificationReentrance) != REDISMODULE_OK) {
            return REDISMODULE_ERR;
        }
        if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_KEY_MISS, KeySpace_NotificationMiss) != REDISMODULE_OK) {
            return REDISMODULE_ERR;
        }
    }

    if (RedisModule_CreateCommand(ctx, "postnotification.async_set", KeySpace_PostNotificationsAsyncSet,
                                      "write", 1, 1, 1) == REDISMODULE_ERR){
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    REDISMODULE_NOT_USED(ctx);
    return REDISMODULE_OK;
}

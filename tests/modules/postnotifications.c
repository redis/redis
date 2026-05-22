/* This module is used to test the server post-notification keyspace jobs API.
 *
 * It supports both APIs from a single .so:
 *   `RedisModule_AddPostNotificationJob`        (the "regular" API)
 *   `RedisModule_AddPostNotificationJobForKey`  (the "per-key" API)
 *
 * The API to register against is chosen via a required load arg: "regular"
 * or "perkey". The keyspace handlers use the same key prefixes and produce
 * the same post-job side effects in either mode — only the registration
 * call differs. This lets the common tests parametrize over the two APIs
 * without diverging in keys, asserts, or expected streams.
 *
 * An optional `with_key_events` arg subscribes to RedisModuleEvent_Key so
 * tests can additionally observe `before_deleted`/`before_expired`/
 * `before_evicted`/`before_overwritten` interleaving with the
 * post-notification drain. The server-event-driven post-jobs are always
 * registered through the regular API (server events are not API-specific).
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

#define _BSD_SOURCE
#define _DEFAULT_SOURCE /* For usleep */

#include "redismodule.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* ===========================================================================
 * Mode dispatcher: KSN handlers compute (key, target) and forward to one of
 * the two APIs based on g_api_mode. The post-job effect is the same in both
 * modes; only the registration call differs.
 * ======================================================================== */

enum api_mode {
    MODE_REGULAR,
    MODE_PERKEY,
};
static int g_api_mode = MODE_REGULAR;

static void FreeHeldString(void *pd) {
    RedisModule_FreeString(NULL, pd);
}

/* Effects */

static void DoIncr(RedisModuleCtx *ctx, RedisModuleString *target) {
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "incr", "!s", target);
    if (rep) RedisModule_FreeCallReply(rep);
}

static void DoGet(RedisModuleCtx *ctx, RedisModuleString *target) {
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "get", "!s", target);
    if (rep) RedisModule_FreeCallReply(rep);
}

/* Regular-API callback wrappers — pd is the target RedisModuleString. */

static void RegularJob_Incr(RedisModuleCtx *ctx, void *pd) {
    DoIncr(ctx, (RedisModuleString *)pd);
}

static void RegularJob_Get(RedisModuleCtx *ctx, void *pd) {
    DoGet(ctx, (RedisModuleString *)pd);
}

/* Per-key-API callback wrappers — pd is the target; the key argument is the
 * notifying key (used only to satisfy the per-key API's signature). */

static void PerKeyJob_Incr(RedisModuleCtx *ctx, RedisModuleString *key, void *pd) {
    REDISMODULE_NOT_USED(key);
    DoIncr(ctx, (RedisModuleString *)pd);
}

static void PerKeyJob_Get(RedisModuleCtx *ctx, RedisModuleString *key, void *pd) {
    REDISMODULE_NOT_USED(key);
    DoGet(ctx, (RedisModuleString *)pd);
}

/* Dispatchers — register a post-notification job using the active API. */

static int RegisterIncrJob(RedisModuleCtx *ctx, RedisModuleString *key, RedisModuleString *target) {
    if (g_api_mode == MODE_REGULAR) {
        return RedisModule_AddPostNotificationJob(ctx, RegularJob_Incr, target, FreeHeldString);
    }
    return RedisModule_AddPostNotificationJobForKey(ctx, PerKeyJob_Incr, key, target, FreeHeldString);
}

static int RegisterGetJob(RedisModuleCtx *ctx, RedisModuleString *key, RedisModuleString *target) {
    if (g_api_mode == MODE_REGULAR) {
        return RedisModule_AddPostNotificationJob(ctx, RegularJob_Get, target, FreeHeldString);
    }
    return RedisModule_AddPostNotificationJobForKey(ctx, PerKeyJob_Get, key, target, FreeHeldString);
}

/* ===========================================================================
 * Mode-aware KSN handlers (registered in both modes).
 * ======================================================================== */

/* "expired" event: register a post-job that INCRs an "expired" counter. */
static int KeySpace_NotificationExpired(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    RedisModuleString *target = RedisModule_CreateString(NULL, "expired", 7);
    int res = RegisterIncrJob(ctx, key, target);
    if (res == REDISMODULE_ERR) FreeHeldString(target);
    return REDISMODULE_OK;
}

/* "evicted" event: register a post-job that INCRs an "evicted" counter. */
static int KeySpace_NotificationEvicted(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);
    if (strncmp(key_str, "evicted", 7) == 0) return REDISMODULE_OK; /* skip our sink */
    if (strncmp(key_str, "before_evicted", 14) == 0) return REDISMODULE_OK; /* skip server-event sink */

    RedisModuleString *target = RedisModule_CreateString(NULL, "evicted", 7);
    int res = RegisterIncrJob(ctx, key, target);
    if (res == REDISMODULE_ERR) FreeHeldString(target);
    return REDISMODULE_OK;
}

/* "string" event on `string_<x>` keys: register a post-job that INCRs the
 * paired `string_changed{<x>}` counter, which itself fires another KSN that
 * cascades into INCR `string_total`. */
static int KeySpace_NotificationString(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);
    if (strncmp(key_str, "string_", 7) != 0) return REDISMODULE_OK;
    if (strcmp(key_str, "string_total") == 0) return REDISMODULE_OK;

    RedisModuleString *target;
    if (strncmp(key_str, "string_changed{", 15) == 0) {
        target = RedisModule_CreateString(NULL, "string_total", 12);
    } else {
        target = RedisModule_CreateStringPrintf(NULL, "string_changed{%s}", key_str);
    }

    int res = RegisterIncrJob(ctx, key, target);
    if (res == REDISMODULE_ERR) FreeHeldString(target);
    return REDISMODULE_OK;
}

/* "string" event on `read_<x>` keys: register a post-job that GETs `<x>` —
 * used to drive lazy expire on `<x>` from inside a post-notification
 * callback. */
static int KeySpace_LazyExpireInsidePostNotificationJob(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);
    if (strncmp(key_str, "read_", 5) != 0) return REDISMODULE_OK;

    RedisModuleString *target = RedisModule_CreateString(NULL, key_str + 5, strlen(key_str) - 5);
    int res = RegisterGetJob(ctx, key, target);
    if (res == REDISMODULE_ERR) FreeHeldString(target);
    return REDISMODULE_OK;
}

/* "string" event on `write_sync_<x>` keys: directly RM_Call SET <x> 1 from
 * inside the handler (no post-job). Exercises
 * REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS, which is
 * orthogonal to the post-notification APIs. */
static int KeySpace_NestedNotification(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);
    if (strncmp(key_str, "write_sync_", 11) != 0) return REDISMODULE_OK;

    /* This test was only meant to check REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS.
     * In general it is wrong and discouraged to perform any writes inside a notification callback. */
    RedisModuleString *new_key = RedisModule_CreateString(NULL, key_str + 11, strlen(key_str) - 11);
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "set", "!sc", new_key, "1");
    RedisModule_FreeCallReply(rep);
    RedisModule_FreeString(NULL, new_key);
    return REDISMODULE_OK;
}

/* ===========================================================================
 * Async write from a module thread (mode-independent).
 * ======================================================================== */

typedef struct AsyncSetArgs {
    RedisModuleBlockedClient *bc;
    RedisModuleString *key;
} AsyncSetArgs;

static void *KeySpace_PostNotificationsAsyncSetInner(void *arg) {
    AsyncSetArgs *args = arg;
    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(args->bc);
    RedisModule_ThreadSafeContextLock(ctx);
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "set", "!sc", args->key, "1");
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
    args->bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);
    args->key = RedisModule_HoldString(NULL, argv[1]);

    pthread_t tid;
    if (pthread_create(&tid, NULL, KeySpace_PostNotificationsAsyncSetInner, args) != 0) {
        RedisModule_AbortBlock(args->bc);
        RedisModule_FreeString(NULL, args->key);
        RedisModule_Free(args);
        return RedisModule_ReplyWithError(ctx, "-ERR Can't start thread");
    }
    pthread_detach(tid);
    return REDISMODULE_OK;
}

/* ===========================================================================
 * Server-event handler: subscribes to RedisModuleEvent_Key when
 * `with_key_events` is passed at load time. Always uses the regular API —
 * server events are not API-specific.
 * ======================================================================== */

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
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "lpush", "!ss", pn_ctx->new_key, pn_ctx->triggered_on);
    RedisModule_FreeCallReply(rep);
}

static void KeySpace_ServerEventCallback(RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent, void *data) {
    REDISMODULE_NOT_USED(eid);
    REDISMODULE_NOT_USED(data);
    if (subevent > 3) {
        RedisModule_Log(ctx, "warning", "Got an unexpected subevent '%llu'", (unsigned long long)subevent);
        return;
    }
    static const char *events[] = {
            "before_deleted",
            "before_expired",
            "before_evicted",
            "before_overwritten",
    };

    const RedisModuleString *key_name = RedisModule_GetKeyNameFromModuleKey(((RedisModuleKeyInfo*)data)->key);
    const char *key_str = RedisModule_StringPtrLen(key_name, NULL);

    for (int i = 0; i < 4; ++i) {
        const char *event = events[i];
        if (strncmp(key_str, event, strlen(event)) == 0) {
            return; /* don't log any event on our tracking keys */
        }
    }

    KeySpace_EventPostNotificationCtx *pn_ctx = RedisModule_Alloc(sizeof(*pn_ctx));
    pn_ctx->triggered_on = RedisModule_HoldString(NULL, (RedisModuleString*)key_name);
    pn_ctx->new_key = RedisModule_CreateString(NULL, events[subevent], strlen(events[subevent]));
    int res = RedisModule_AddPostNotificationJob(ctx, KeySpace_ServerEventPostNotification, pn_ctx, KeySpace_ServerEventPostNotificationFree);
    if (res == REDISMODULE_ERR) KeySpace_ServerEventPostNotificationFree(pn_ctx);
}

/* ===========================================================================
 * Per-key-only fixtures: behaviors that don't exist on the regular API
 * (firing inside MULTI/EXEC, reentrance guard, multi-key refusal, hash
 * subkey interleaving). Registered only in MODE_PERKEY.
 * ======================================================================== */

/* "batched_" path: each keyed callback appends the touched key to a sink
 * list. Used to assert per-sub-command firing inside MULTI/EXEC and the
 * single-key guard on multi-key commands. */
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

/* "hash_" path: HSET/HEXPIRE both fire NOTIFY_HASH against a single hash
 * key, so they pass the single-key guard. Used to assert per-sub-command
 * firing between HSET and HEXPIRE on the same hash inside MULTI/EXEC. */
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

/* "reentrant_" path: probes that the firing function does not re-enter
 * while a nested RM_Call is in flight. */
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

/* ===========================================================================
 * OnLoad: parse args, set mode, subscribe handlers.
 * Required arg: "regular" | "perkey".
 * Optional arg: "with_key_events".
 * ======================================================================== */

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx, "postnotifications", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (!(RedisModule_GetModuleOptionsAll() & REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS)) {
        return REDISMODULE_ERR;
    }

    int mode_set = 0;
    int with_key_events = 0;
    for (int i = 0; i < argc; i++) {
        const char *arg = RedisModule_StringPtrLen(argv[i], NULL);
        if (strcmp(arg, "regular") == 0) {
            g_api_mode = MODE_REGULAR;
            mode_set = 1;
        } else if (strcmp(arg, "perkey") == 0) {
            g_api_mode = MODE_PERKEY;
            mode_set = 1;
        } else if (strcmp(arg, "with_key_events") == 0) {
            with_key_events = 1;
        } else {
            RedisModule_Log(ctx, "warning", "Unknown load arg '%s'", arg);
            return REDISMODULE_ERR;
        }
    }
    if (!mode_set) {
        RedisModule_Log(ctx, "warning", "postnotifications module requires a mode arg ('regular' or 'perkey').");
        return REDISMODULE_ERR;
    }

    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS);

    /* Mode-aware KSN handlers — registered in both modes, dispatch internally. */
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_STRING, KeySpace_NotificationString) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_STRING, KeySpace_LazyExpireInsidePostNotificationJob) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_STRING, KeySpace_NestedNotification) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_EXPIRED, KeySpace_NotificationExpired) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_EVICTED, KeySpace_NotificationEvicted) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
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
    }

    /* Optional server-event subscription. Always registers regular post-jobs. */
    if (with_key_events) {
        if (RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_Key, KeySpace_ServerEventCallback) != REDISMODULE_OK) {
            return REDISMODULE_ERR;
        }
    }

    if (RedisModule_CreateCommand(ctx, "postnotification.async_set", KeySpace_PostNotificationsAsyncSet,
                                  "write", 1, 1, 1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    REDISMODULE_NOT_USED(ctx);
    return REDISMODULE_OK;
}

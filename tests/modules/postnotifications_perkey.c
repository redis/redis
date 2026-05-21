/* This module is used to test the per-key post-notification jobs API
 * (RedisModule_AddPostNotificationJobForKey).
 *
 * Unlike the single-shot post-notification jobs (covered by postnotifications.c),
 * per-key callbacks fire at the tail of every call() — including each sub-command
 * inside MULTI/EXEC — and registrations are restricted to commands that touch
 * exactly one key. This module focuses on those behaviors only.
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

#include "redismodule.h"
#include <string.h>

/* ----------------------------------------------------------------------------
 * "batched_" path: each keyed callback appends the touched key to a sink list.
 * Used to assert per-sub-command firing inside MULTI/EXEC and to assert the
 * single-key guard on multi-key commands.
 * ------------------------------------------------------------------------- */

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

/* ----------------------------------------------------------------------------
 * "hash_" path: HSET/HEXPIRE both fire NOTIFY_HASH against a single hash key,
 * so they pass the single-key guard. The callback LPUSHes the touched key to
 * a sink list, letting us assert that the per-key callback fires between
 * successive HSET/HEXPIRE sub-commands on the same hash inside MULTI/EXEC
 * (the original motivation for this API — RED-197766).
 * ------------------------------------------------------------------------- */

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

/* ----------------------------------------------------------------------------
 * "expire_" path: NOTIFY_EXPIRED fires on the lazy-DEL path with
 * server.executing_client still pointing at the command that touched the key,
 * so the single-key guard accepts the registration. The callback LPUSHes the
 * expired key name to a sink list, letting us assert that lazy expire drives
 * a per-key job. Combined with the "read_" path below, this also exercises
 * lazy expire triggered from inside an outer per-key callback's RM_Call.
 * ------------------------------------------------------------------------- */

static void KeySpace_PostNotificationExpiredKey(RedisModuleCtx *ctx, RedisModuleString *key, void *pd) {
    REDISMODULE_NOT_USED(pd);
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "lpush", "!cs", "expired_keys", key);
    if (rep) RedisModule_FreeCallReply(rep);
}

static int KeySpace_NotificationExpired(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);
    if (strncmp(key_str, "expire_", 7) != 0) return REDISMODULE_OK;

    RedisModule_AddPostNotificationJobForKey(ctx, KeySpace_PostNotificationExpiredKey, key, NULL, NULL);
    return REDISMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * "read_" path: the outer per-key callback for a "read_<target>" key issues a
 * GET on <target>. If <target> is TTL-expired, that GET triggers a lazy DEL,
 * which fires NOTIFY_EXPIRED and registers a second per-key job from inside
 * the outer callback. The second job must fire from the outer drain (not
 * nested inside the outer callback's stack) — the reentrance guard combined
 * with the per-call() firing hook is what makes that work.
 * ------------------------------------------------------------------------- */

static void KeySpace_PostNotificationReadKey(RedisModuleCtx *ctx, RedisModuleString *key, void *pd) {
    REDISMODULE_NOT_USED(pd);
    const char *key_str = RedisModule_StringPtrLen(key, NULL);
    const char *target = key_str + 5; /* strip "read_" */
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "get", "!c", target);
    if (rep) RedisModule_FreeCallReply(rep);
}

static int KeySpace_NotificationRead(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);
    if (strncmp(key_str, "read_", 5) != 0) return REDISMODULE_OK;

    RedisModule_AddPostNotificationJobForKey(ctx, KeySpace_PostNotificationReadKey, key, NULL, NULL);
    return REDISMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * "reentrant_" path: probes that the firing function does not re-enter while a
 * nested RM_Call is in flight. The outer branch raises a marker, issues a
 * nested SET (which registers a second keyed job via KSN), then lowers the
 * marker. If re-entrance happened, the inner branch would observe marker==1
 * and log REENTRANCE_DETECTED. With the guard, the inner job is picked up by
 * the outer drain only after the outer callback has returned.
 * ------------------------------------------------------------------------- */

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

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "postnotifications_perkey", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (!(RedisModule_GetModuleOptionsAll() & REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS)) {
        return REDISMODULE_ERR;
    }
    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS);

    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_STRING, KeySpace_NotificationBatched) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_HASH, KeySpace_NotificationHash) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_EXPIRED, KeySpace_NotificationExpired) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_STRING, KeySpace_NotificationRead) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_STRING, KeySpace_NotificationReentrance) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    REDISMODULE_NOT_USED(ctx);
    return REDISMODULE_OK;
}

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
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_STRING, KeySpace_NotificationReentrance) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    REDISMODULE_NOT_USED(ctx);
    return REDISMODULE_OK;
}

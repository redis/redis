/* Test module for RM_AddPostNotificationJobForKey firing across phases.
 *
 * Used by tests/unit/moduleapi/postnotifications_perkey_aof_repl.tcl. The
 * module exercises the contract that a per-key post-notification callback
 * MUST only touch non-replicated, non-AOF-persisted state — here, module
 * key metadata. This lets the same callback fire on a master, on a replica
 * receiving master-propagated commands, and during AOF replay, with each
 * instance maintaining its per-key state independently.
 *
 * Subscribes to KSN events for STRING / HASH / GENERIC / EXPIRED / EVICTED.
 * For each notification the KSN handler enqueues a per-key job; the job
 * later attaches metadata via RM_SetKeyMeta. A module-internal counter
 * (NOT a Redis key — to avoid AOF / replication pollution) records how
 * many times the per-key job actually ran.
 *
 * Commands:
 *   pkmeta.getmeta <key>      - Return the metadata string, or nil.
 *   pkmeta.firecount          - Return the module-internal fire counter.
 *   pkmeta.reset              - Zero the fire counter.
 *   pkmeta.try_outside        - Call RM_AddPostNotificationJobForKey from
 *                               outside a KSN handler; reply OK/ERR for the
 *                               negative-coverage test.
 *
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "redismodule.h"
#include <string.h>
#include <stdlib.h>

static RedisModuleKeyMetaClassId meta_class_id = -1;

/* Module-internal counter — kept out of the keyspace on purpose so it is
 * neither replicated nor AOF-persisted. Tests assert on it to confirm the
 * per-key callback actually ran. */
static long long fire_count = 0;

static void MetaFreeCallback(const char *keyname, uint64_t meta) {
    REDISMODULE_NOT_USED(keyname);
    if (meta != 0) free((char *)meta);
}

/* Per-key post-notification job: attaches a "notified" string as metadata.
 * Runs at the tail of the originating command (or sub-command for
 * MULTI/EXEC), outside the KSN handler stack. */
static void PerKeyMetadataJob(RedisModuleCtx *ctx, RedisModuleString *key, void *pd) {
    REDISMODULE_NOT_USED(pd);
    if (meta_class_id < 0) return;

    RedisModuleKey *k = RedisModule_OpenKey(ctx, key, REDISMODULE_WRITE);
    if (!k) return;
    if (RedisModule_KeyType(k) == REDISMODULE_KEYTYPE_EMPTY) {
        RedisModule_CloseKey(k);
        return;
    }

    uint64_t existing = 0;
    if (RedisModule_GetKeyMeta(meta_class_id, k, &existing) == REDISMODULE_OK &&
        existing != 0) {
        free((char *)existing);
    }

    char *new_str = strdup("notified");
    if (RedisModule_SetKeyMeta(meta_class_id, k, (uint64_t)new_str) == REDISMODULE_OK) {
        fire_count++;
    } else {
        free(new_str);
    }
    RedisModule_CloseKey(k);
}

/* KSN handler: defers SetKeyMeta into a per-key job rather than calling it
 * inline. This is the path under test — the per-key API is what makes the
 * write happen at a safe firing point, including during AOF replay and on
 * a replica receiving propagated commands. */
static int NotifyCallback(RedisModuleCtx *ctx, int type, const char *event,
                          RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);
    RedisModule_AddPostNotificationJobForKey(ctx, PerKeyMetadataJob, key, NULL, NULL);
    return REDISMODULE_OK;
}

static int GetMetaCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);
    RedisModuleKey *k = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    if (!k || RedisModule_KeyType(k) == REDISMODULE_KEYTYPE_EMPTY) {
        if (k) RedisModule_CloseKey(k);
        return RedisModule_ReplyWithNull(ctx);
    }
    uint64_t meta = 0;
    if (RedisModule_GetKeyMeta(meta_class_id, k, &meta) == REDISMODULE_OK && meta != 0) {
        RedisModule_ReplyWithCString(ctx, (const char *)meta);
    } else {
        RedisModule_ReplyWithNull(ctx);
    }
    RedisModule_CloseKey(k);
    return REDISMODULE_OK;
}

static int FireCountCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    return RedisModule_ReplyWithLongLong(ctx, fire_count);
}

static int ResetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    fire_count = 0;
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* Calls RM_AddPostNotificationJobForKey from outside a KSN handler — must
 * return REDISMODULE_ERR. Used by the negative coverage test. */
static int TryOutsideCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);
    int rc = RedisModule_AddPostNotificationJobForKey(ctx, PerKeyMetadataJob,
                                                     argv[1], NULL, NULL);
    if (rc == REDISMODULE_OK) {
        return RedisModule_ReplyWithSimpleString(ctx, "OK");
    }
    return RedisModule_ReplyWithError(ctx, "ERR registration refused");
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "pkmeta", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModuleKeyMetaClassConfig config = {0};
    config.version = REDISMODULE_KEY_META_VERSION;
    config.flags = (1 << REDISMODULE_META_ALLOW_IGNORE);
    config.reset_value = (uint64_t)NULL;
    config.free = MetaFreeCallback;
    meta_class_id = RedisModule_CreateKeyMetaClass(ctx, "pkmc", 1, &config);
    if (meta_class_id < 0) return REDISMODULE_ERR;

    int notifyFlags = REDISMODULE_NOTIFY_GENERIC | REDISMODULE_NOTIFY_HASH |
                      REDISMODULE_NOTIFY_STRING | REDISMODULE_NOTIFY_EXPIRED |
                      REDISMODULE_NOTIFY_EVICTED;
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, notifyFlags, NotifyCallback) != REDISMODULE_OK)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "pkmeta.getmeta", GetMetaCommand,
                                  "readonly", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "pkmeta.firecount", FireCountCommand,
                                  "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "pkmeta.reset", ResetCommand,
                                  "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "pkmeta.try_outside", TryOutsideCommand,
                                  "readonly", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    REDISMODULE_NOT_USED(ctx);
    if (meta_class_id >= 0) {
        RedisModule_ReleaseKeyMetaClass(meta_class_id);
        meta_class_id = -1;
    }
    return REDISMODULE_OK;
}

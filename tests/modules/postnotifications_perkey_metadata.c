/* Test module for RM_AddPostNotificationJobForKey.
 *
 * On each KSN event the handler enqueues a per-key job that attaches module
 * key metadata via RM_SetKeyMeta. Metadata is not in the AOF/RDB and is not
 * replicated, so its presence after a reload proves the callback re-ran. Keys
 * with the "probe_" prefix instead enqueue a job that attempts a (forbidden)
 * RM_Call, to check the runtime refuses it. Keys with the "notifyprobe_" prefix
 * enqueue a job that attempts a (forbidden) RM_NotifyKeyspaceEvent, to check the
 * runtime refuses that too.
 *
 * Commands:
 *   pkmeta.getmeta <key>   - metadata string, or nil
 *   pkmeta.firecount       - how many times the per-key job ran
 *   pkmeta.firelog         - key names the job fired for, in order
 *   pkmeta.rmcall_blocked  - how many RM_Call attempts were refused
 *   pkmeta.notify_blocked  - how many RM_NotifyKeyspaceEvent attempts were refused
 *   pkmeta.reset           - zero the counters and clear the log
 *   pkmeta.try_outside     - call the API outside a KSN handler (must fail)
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
#include <errno.h>

static RedisModuleKeyMetaClassId meta_class_id = -1;

/* Module-internal (not a Redis key, so not replicated/persisted). */
static long long fire_count = 0;

/* Key names the job fired for, in order. Module-internal like fire_count.
 * dbsize_log records the DB size observed at the moment each job fired. */
#define FIRELOG_CAP 256
static char *fire_log[FIRELOG_CAP];
static long long dbsize_log[FIRELOG_CAP];
static int fire_log_len = 0;

static void FireLogAppend(const char *name, long long dbsize) {
    if (fire_log_len < FIRELOG_CAP) {
        dbsize_log[fire_log_len] = dbsize;
        fire_log[fire_log_len++] = strdup(name);
    }
}

static void FireLogClear(void) {
    for (int i = 0; i < fire_log_len; i++) free(fire_log[i]);
    fire_log_len = 0;
}

/* RM_Call attempts from inside a per-key callback that the runtime refused. */
static long long rmcall_blocked_count = 0;

/* RM_NotifyKeyspaceEvent attempts from inside a per-key callback that the
 * runtime refused. */
static long long notify_blocked_count = 0;

/* Firings the job observed on an already-removed key (expired or evicted): the
 * key is gone by the time the job runs, so it can't attach metadata, but the
 * job DID fire. Recorded separately so the live-key fire_count/fire_log
 * assertions are unaffected, and expire/evict coverage has something to assert. */
static long long empty_fire_count = 0;
static char *empty_fire_log[FIRELOG_CAP];
static int empty_fire_log_len = 0;

static void EmptyFireLogAppend(const char *name) {
    if (empty_fire_log_len < FIRELOG_CAP)
        empty_fire_log[empty_fire_log_len++] = strdup(name);
}

static void EmptyFireLogClear(void) {
    for (int i = 0; i < empty_fire_log_len; i++) free(empty_fire_log[i]);
    empty_fire_log_len = 0;
}

static void MetaFreeCallback(const char *keyname, uint64_t meta) {
    REDISMODULE_NOT_USED(keyname);
    if (meta != 0) free((char *)meta);
}

/* Per-key job: attaches a "notified" string as metadata. */
static void PerKeyMetadataJob(RedisModuleCtx *ctx, RedisModuleString *key, void *pd) {
    REDISMODULE_NOT_USED(pd);
    if (meta_class_id < 0) return;

    RedisModuleKey *k = RedisModule_OpenKey(ctx, key, REDISMODULE_WRITE);
    if (!k) return;
    if (RedisModule_KeyType(k) == REDISMODULE_KEYTYPE_EMPTY) {
        /* Key already removed (expired/evicted). Can't attach metadata, but the
         * per-key job fired for it — record so expire/evict tests can assert. */
        empty_fire_count++;
        size_t klen;
        const char *kname = RedisModule_StringPtrLen(key, &klen);
        EmptyFireLogAppend(kname);
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
        size_t klen;
        const char *kname = RedisModule_StringPtrLen(key, &klen);
        FireLogAppend(kname, (long long)RedisModule_DbSize(ctx));
    } else {
        free(new_str);
    }
    RedisModule_CloseKey(k);
}

/* Per-key job that issues a (forbidden) RM_Call. The runtime must refuse it:
 * the well-formed call returns NULL with errno == EINVAL, so EINVAL can only
 * come from the per-key guard. Registered for "probe_" keys. */
static void PerKeyRMCallProbeJob(RedisModuleCtx *ctx, RedisModuleString *key, void *pd) {
    REDISMODULE_NOT_USED(key);
    REDISMODULE_NOT_USED(pd);
    errno = 0;
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "incr", "!c", "pkmeta_rmcall_sink");
    if (rep == NULL && errno == EINVAL) {
        rmcall_blocked_count++;
    } else if (rep != NULL) {
        /* Call was not blocked; free the reply (test asserts on the count). */
        RedisModule_FreeCallReply(rep);
    }
}

/* Per-key job that fires a (forbidden) keyspace notification. The runtime must
 * refuse it: RM_NotifyKeyspaceEvent returns REDISMODULE_ERR while a per-key
 * callback runs. Registered for "notifyprobe_" keys. */
static void PerKeyNotifyProbeJob(RedisModuleCtx *ctx, RedisModuleString *key, void *pd) {
    REDISMODULE_NOT_USED(pd);
    if (RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_GENERIC,
                                        "pkmeta.probe", key) == REDISMODULE_ERR) {
        notify_blocked_count++;
    }
}

/* KSN handler: enqueues a per-key job instead of writing inline. */
static int NotifyCallback(RedisModuleCtx *ctx, int type, const char *event,
                          RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);
    const char *kname = RedisModule_StringPtrLen(key, NULL);
    if (strncmp(kname, "probe_", 6) == 0) {
        RedisModule_AddPostNotificationJobForKey(ctx, PerKeyRMCallProbeJob, key, NULL, NULL);
    } else if (strncmp(kname, "notifyprobe_", 12) == 0) {
        RedisModule_AddPostNotificationJobForKey(ctx, PerKeyNotifyProbeJob, key, NULL, NULL);
    } else {
        RedisModule_AddPostNotificationJobForKey(ctx, PerKeyMetadataJob, key, NULL, NULL);
    }
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

static int FireLogCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModule_ReplyWithArray(ctx, fire_log_len);
    for (int i = 0; i < fire_log_len; i++) {
        RedisModule_ReplyWithCString(ctx, fire_log[i]);
    }
    return REDISMODULE_OK;
}

/* DB size observed at each firing, in order. */
static int DbSizeLogCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModule_ReplyWithArray(ctx, fire_log_len);
    for (int i = 0; i < fire_log_len; i++) {
        RedisModule_ReplyWithLongLong(ctx, dbsize_log[i]);
    }
    return REDISMODULE_OK;
}

static int RMCallBlockedCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    return RedisModule_ReplyWithLongLong(ctx, rmcall_blocked_count);
}

static int NotifyBlockedCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    return RedisModule_ReplyWithLongLong(ctx, notify_blocked_count);
}

/* How many times the per-key job fired for an already-removed (expired/evicted)
 * key. */
static int EmptyFireCountCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    return RedisModule_ReplyWithLongLong(ctx, empty_fire_count);
}

/* Names of the keys the per-key job fired for while they were already removed. */
static int EmptyFireLogCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModule_ReplyWithArray(ctx, empty_fire_log_len);
    for (int i = 0; i < empty_fire_log_len; i++) {
        RedisModule_ReplyWithCString(ctx, empty_fire_log[i]);
    }
    return REDISMODULE_OK;
}

static int ResetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    fire_count = 0;
    rmcall_blocked_count = 0;
    notify_blocked_count = 0;
    empty_fire_count = 0;
    FireLogClear();
    EmptyFireLogClear();
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* Calls the API outside a KSN handler — must return REDISMODULE_ERR. */
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
                      REDISMODULE_NOTIFY_EVICTED | REDISMODULE_NOTIFY_SET;
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, notifyFlags, NotifyCallback) != REDISMODULE_OK)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "pkmeta.getmeta", GetMetaCommand,
                                  "readonly", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "pkmeta.firecount", FireCountCommand,
                                  "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "pkmeta.firelog", FireLogCommand,
                                  "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "pkmeta.dbsizelog", DbSizeLogCommand,
                                  "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "pkmeta.rmcall_blocked", RMCallBlockedCommand,
                                  "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "pkmeta.notify_blocked", NotifyBlockedCommand,
                                  "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "pkmeta.empty_firecount", EmptyFireCountCommand,
                                  "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "pkmeta.empty_firelog", EmptyFireLogCommand,
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
    FireLogClear();
    EmptyFireLogClear();
    return REDISMODULE_OK;
}

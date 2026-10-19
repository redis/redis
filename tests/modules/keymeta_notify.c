/* Test module for KSN paths that must tolerate keymeta writes.
 *
 * In general, keyspace notification callbacks must not perform write
 * operations. However, Search module modifies key metadata as part of KSN, so 
 * this module exercises the subset of KSN flows that must remain resilient to
 * such keymeta modifications, including cases that may trigger kvobj
 * reallocation.
 *
 * Commands:
 *   KEYMETANOTIFY.GET <key>      - Get the metadata value attached to a key
 *   KEYMETANOTIFY.SETCOUNT       - Get how many times metadata was set in notifications
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

/* Counter incremented each time we successfully set metadata in a notification */
static long long meta_set_count = 0;

/* Notification callback: sets metadata on the key during notifications. */
static int HashNotifyCallback(RedisModuleCtx *ctx, int type, const char *event,
                               RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    if (meta_class_id < 0) return REDISMODULE_OK;

    RedisModuleKey *k = RedisModule_OpenKey(ctx, key, REDISMODULE_WRITE);
    if (!k) return REDISMODULE_OK;

    if (RedisModule_KeyType(k) == REDISMODULE_KEYTYPE_EMPTY) {
        RedisModule_CloseKey(k);
        return REDISMODULE_OK;
    }

    /* Free existing metadata if any */
    uint64_t existing = 0;
    if (RedisModule_GetKeyMeta(meta_class_id, k, &existing) == REDISMODULE_OK) {
        if (existing != 0) {
            free((char *)existing);
        }
    }

    /* Set new metadata - a simple string "notified" */
    char *new_str = strdup("notified");
    if (RedisModule_SetKeyMeta(meta_class_id, k, (uint64_t)new_str) == REDISMODULE_OK) {
        meta_set_count++;
    } else {
        free(new_str);
    }

    RedisModule_CloseKey(k);
    return REDISMODULE_OK;
}

/* Free callback for metadata */
static void MetaFreeCallback(const char *keyname, uint64_t meta) {
    REDISMODULE_NOT_USED(keyname);
    if (meta != 0) {
        free((char *)meta);
    }
}

/* KEYMETANOTIFY.GET <key> - Get the metadata string attached to a key */
static int GetMetaCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    RedisModuleKey *k = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    if (!k || RedisModule_KeyType(k) == REDISMODULE_KEYTYPE_EMPTY) {
        if (k) RedisModule_CloseKey(k);
        RedisModule_ReplyWithNull(ctx);
        return REDISMODULE_OK;
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

/* KEYMETANOTIFY.SETCOUNT - Get how many times we successfully set metadata in notifications */
static int SetCountCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModule_ReplyWithLongLong(ctx, meta_set_count);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "keymetanotify", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* Register a metadata class */
    RedisModuleKeyMetaClassConfig config = {0};
    config.version = REDISMODULE_KEY_META_VERSION;
    config.flags = (1 << REDISMODULE_META_ALLOW_IGNORE);
    config.reset_value = (uint64_t)NULL;
    config.free = MetaFreeCallback;
    config.rdb_load = NULL;
    config.rdb_save = NULL;
    config.aof_rewrite = NULL;
    config.copy = NULL;
    config.rename = NULL;
    config.move = NULL;
    config.defrag = NULL;
    config.unlink = NULL;
    config.mem_usage = NULL;
    config.free_effort = NULL;

    meta_class_id = RedisModule_CreateKeyMetaClass(ctx, "kmno", 1, &config);
    if (meta_class_id < 0) return REDISMODULE_ERR;

    /* Subscribe to keyspace events matching RediSearch's notification types:
     * GENERIC, HASH, STRING, EXPIRED, and EVICTED. */
    int notifyFlags = REDISMODULE_NOTIFY_GENERIC | REDISMODULE_NOTIFY_HASH |
                      REDISMODULE_NOTIFY_STRING | REDISMODULE_NOTIFY_EXPIRED |
                      REDISMODULE_NOTIFY_EVICTED;
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, notifyFlags,
                                              HashNotifyCallback) != REDISMODULE_OK)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "keymetanotify.get", GetMetaCommand,
                                  "readonly", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "keymetanotify.setcount", SetCountCommand,
                                  "readonly", 0, 0, 0) == REDISMODULE_ERR)
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

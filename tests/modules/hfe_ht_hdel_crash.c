/* hfe_ht_hdel_crash.c
 *
 * Regression-test module for two use-after-free crashes in onFieldExpire()
 * for OBJ_ENCODING_HT hashes, caused by module post-notification jobs.
 *
 * Root cause (shared with hfe_ht_hset_crash.c):
 *   onFieldExpire() stores `field = entryGetField(e)`.  Because
 *   entryGetField() returns (sds)entry, `field` and `e` share the same
 *   heap allocation.  After propagateHashFieldDeletion() flushes the job
 *   queue via postExecutionUnitOperations(), the running job may free that
 *   allocation, leaving `field` dangling before hashTypeDelete() is called.
 *
 * Scenario A -- HT + HDEL:
 *   The job calls HDEL on "g1" -- the field currently being expired.
 *   hashTypeDelete() inside HDEL calls dictEntryDestructor() -> entryFree(),
 *   freeing the Entry that `field` points to.
 *   Back in onFieldExpire():
 *       serverAssert(hashTypeDelete(expCtx->hashObj, field) == 1)
 *   uses the freed `field` pointer -- use-after-free / assertion failure.
 *
 * Scenario B -- HT + DEL:
 *   The job calls DEL on the entire victim hash.  The dict and the robj are
 *   freed synchronously.  expCtx->hashObj becomes a dangling pointer.
 *   hashTypeDelete(expCtx->hashObj, field) then dereferences freed memory.
 *
 * Copyright (c) 2024-Present, Redis Ltd. All rights reserved.
 * Licensed under your choice of RSALv2 or SSPLv1.
 */

#include "redismodule.h"
#include <string.h>

/* Scenario A: HDEL the field being expired */
#define TRIGGER_HDEL "hfe_hdel_ht_trigger"
#define VICTIM_HDEL  "hfe_hdel_ht_victim"

/* Scenario B: DEL the entire hash */
#define TRIGGER_DEL  "hfe_del_ht_trigger"
#define VICTIM_DEL   "hfe_del_ht_victim"

/* Post-notification job for Scenario A.
 * Deletes "g1" -- the first-to-expire field -- while onFieldExpire() holds
 * a raw pointer to its Entry.  dictEntryDestructor() frees the Entry,
 * making `field = (sds)e` a dangling pointer. */
static void HfeHtHdel_PostNotificationJob(RedisModuleCtx *ctx, void *pd) {
    REDISMODULE_NOT_USED(pd);
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "hdel", "!cc",
            VICTIM_HDEL, "g1");
    RedisModule_FreeCallReply(rep);
}

/* Post-notification job for Scenario B.
 * Deletes the victim hash entirely; expCtx->hashObj becomes dangling. */
static void HfeHtDel_PostNotificationJob(RedisModuleCtx *ctx, void *pd) {
    REDISMODULE_NOT_USED(pd);
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "del", "!c",
            VICTIM_DEL);
    RedisModule_FreeCallReply(rep);
}

/* Keyspace notification callback.
 * TRIGGER_HDEL / TRIGGER_DEL expire before the corresponding victim hash.
 * Their 'hexpired' events queue the jobs; the jobs fire during the victim's
 * first onFieldExpire() -> propagateHashFieldDeletion() call. */
static int HfeHtHdel_KeyspaceNotification(RedisModuleCtx *ctx, int type,
                                           const char *event,
                                           RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    if (strcmp(event, "hexpired") != 0) return REDISMODULE_OK;

    const char *key_str = RedisModule_StringPtrLen(key, NULL);

    if (strcmp(key_str, TRIGGER_HDEL) == 0) {
        RedisModule_AddPostNotificationJob(ctx,
                HfeHtHdel_PostNotificationJob, NULL, NULL);
    } else if (strcmp(key_str, TRIGGER_DEL) == 0) {
        RedisModule_AddPostNotificationJob(ctx,
                HfeHtDel_PostNotificationJob, NULL, NULL);
    }
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "hfe_ht_hdel_crash", 1,
                         REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_HASH,
                HfeHtHdel_KeyspaceNotification) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    REDISMODULE_NOT_USED(ctx);
    return REDISMODULE_OK;
}


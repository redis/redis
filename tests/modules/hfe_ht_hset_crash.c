/* hfe_ht_hset_crash.c
 *
 * Regression-test module that reproduces a use-after-free crash in
 * onFieldExpire() for OBJ_ENCODING_HT hashes.
 *
 * Root cause:
 *   onFieldExpire() obtains `field = entryGetField(e)`.  Because the Entry
 *   pointer IS the field sds (see entry.h: entryGetField returns (sds)entry),
 *   `field` and `e` share the same allocation.
 *
 *   propagateHashFieldDeletion() then calls postExecutionUnitOperations(),
 *   which runs any queued post-notification jobs.  If such a job calls HSET
 *   on the same field with a value whose embedded-vs-pointer layout differs
 *   from the original (e.g. the field previously had an embedded small value
 *   + ExpireMeta, but now the value is large and there is no ExpireMeta),
 *   hashTypeSet() calls entryUpdate(), which detects the layout change and
 *   allocates a NEW Entry, then calls entryFree() on the OLD one.
 *
 *   Back in onFieldExpire(), `field` (= the old sds / freed allocation) is
 *   now a dangling pointer.  The subsequent call:
 *       serverAssert(hashTypeDelete(expCtx->hashObj, field) == 1)
 *   performs a dict lookup through a freed pointer -- undefined behaviour /
 *   assertion failure.
 *
 * Copyright (c) 2024-Present, Redis Ltd. All rights reserved.
 * Licensed under your choice of RSALv2 or SSPLv1.
 */

#include "redismodule.h"
#include <string.h>

#define TRIGGER_KEY "hfe_hset_ht_trigger"
#define VICTIM_KEY  "hfe_hset_ht_victim"

/* A 1000-byte value ensures the new entry uses a value pointer rather than
 * an embedded value, guaranteeing a layout change from the original
 * (small embedded value + ExpireMeta) to (large value ptr, no ExpireMeta).
 * entryUpdate() will therefore call entryFree(oldEntry) and return a new
 * Entry pointer, leaving `field` in onFieldExpire() dangling. */
#define OVERSIZE_VALUE \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" /* 1000 */

/* Post-notification job: HSET the victim's "g1" field with a large value.
 * "g1" is the first field to expire in the victim (given an earlier TTL in
 * the test), so this job fires exactly when onFieldExpire() is processing g1
 * -- i.e. after propagateHashFieldDeletion() but before hashTypeDelete(). */
static void HfeHtHset_PostNotificationJob(RedisModuleCtx *ctx, void *pd) {
    REDISMODULE_NOT_USED(pd);
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "hset", "!ccc",
            VICTIM_KEY, "g1", OVERSIZE_VALUE);
    RedisModule_FreeCallReply(rep);
}

/* Keyspace notification callback.
 * When TRIGGER_KEY fires 'hexpired', queue the job that will HSET the victim.
 * The job runs inside the victim's first onFieldExpire() call, triggering the
 * use-after-free on the now-freed old Entry for field "g1". */
static int HfeHtHset_KeyspaceNotification(RedisModuleCtx *ctx, int type,
                                           const char *event,
                                           RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    if (strcmp(event, "hexpired") != 0) return REDISMODULE_OK;
    const char *key_str = RedisModule_StringPtrLen(key, NULL);
    if (strcmp(key_str, TRIGGER_KEY) != 0) return REDISMODULE_OK;
    RedisModule_AddPostNotificationJob(ctx,
            HfeHtHset_PostNotificationJob, NULL, NULL);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "hfe_ht_hset_crash", 1,
                         REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_HASH,
                HfeHtHset_KeyspaceNotification) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    REDISMODULE_NOT_USED(ctx);
    return REDISMODULE_OK;
}


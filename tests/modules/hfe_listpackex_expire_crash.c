/* hfe_listpackex_expire_crash.c
 *
 * Regression-test module that reproduces the dangling-pointer crash in
 * listpackExExpire() (listpack.c:1691, assert(lpValidateNext(...))).
 *
 * Root cause: listpackExExpire() iterates the listpack with a raw pointer
 * `ptr`.  For each expired field it calls propagateHashFieldDeletion(), which
 * calls postExecutionUnitOperations().  If a module has registered a
 * post-notification job, that job runs synchronously here and may write to the
 * same hash (e.g. HSET), causing lpRealloc() to move the listpack buffer.
 * The local `ptr` now points into the freed old buffer; the subsequent
 * lpNext(lpt->lp_new, ptr_old) detects the out-of-range pointer and fires the
 * assertion.
 *
 * Copyright (c) 2024-Present, Redis Ltd. All rights reserved.
 * Licensed under your choice of RSALv2 or SSPLv1.
 */

#include "redismodule.h"
#include <string.h>

#define TRIGGER_KEY "hfe_crash_trigger"
#define VICTIM_KEY  "hfe_crash_victim"

/* 1000-byte value keeps the hash within LISTPACK_EX encoding
 * (hash-max-listpack-value is set to 1100 in the test), so no encoding
 * conversion occurs -- lpRealloc() alone is enough to move the buffer.
 * On macOS this crosses the tiny→small malloc-zone boundary (1008 bytes),
 * guaranteeing a new address.  On Linux glibc realloc similarly returns a
 * new pointer whenever the adjacent chunk is occupied. */
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

static void HfeCrash_PostNotificationJob(RedisModuleCtx *ctx, void *pd) {
    REDISMODULE_NOT_USED(pd);
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "hset", "!ccc",
            VICTIM_KEY, "xfield", OVERSIZE_VALUE);
    RedisModule_FreeCallReply(rep);
}

/* Keyspace notification callback -- the "fuse" for the crash reproducer.
 *
 * The bug requires a post-notification job to already be queued before
 * victim's listpackExExpire() starts iterating.  In production this job
 * could come from any module; here we manufacture it artificially:
 *
 *   1. active-expire processes TRIGGER_KEY first (earlier expiry time).
 *   2. That fires an 'hexpired' keyspace event, which calls this function.
 *   3. We queue HfeCrash_PostNotificationJob targeting VICTIM_KEY.
 *   4. active-expire then processes VICTIM_KEY; the very first
 *      propagateHashFieldDeletion() call flushes the job queue (nesting
 *      drops to 0), running the job mid-iteration -- triggering the crash.
 */
static int HfeCrash_KeyspaceNotification(RedisModuleCtx *ctx, int type,
                                          const char *event,
                                          RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    if (strcmp(event, "hexpired") != 0) return REDISMODULE_OK;
    const char *key_str = RedisModule_StringPtrLen(key, NULL);
    if (strcmp(key_str, TRIGGER_KEY) != 0) return REDISMODULE_OK;
    RedisModule_AddPostNotificationJob(ctx, HfeCrash_PostNotificationJob, NULL, NULL);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "hfe_listpackex_expire_crash", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_HASH,
                HfeCrash_KeyspaceNotification) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    REDISMODULE_NOT_USED(ctx);
    return REDISMODULE_OK;
}


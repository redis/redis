/* hfe_listpackex_hdel_crash.c
 *
 * Regression-test module for two dangling-pointer crashes in
 * listpackExExpire() triggered by module post-notification jobs.
 *
 * Root cause (same as hfe_listpackex_expire_crash.c):
 *   propagateHashFieldDeletion() calls postExecutionUnitOperations(), which
 *   runs any queued post-notification jobs synchronously.  If such a job
 *   modifies the victim hash, the loop's local `ptr` becomes invalid.
 *
 * Scenario A -- LISTPACK_EX + HDEL:
 *   The job calls HDEL on the field currently being expired (g1).
 *   lpDeleteRangeWithEntry() removes g1's three listpack elements.
 *   If the buffer is reallocated, lpt->lp changes address while the loop's
 *   local `ptr` still holds the old (freed) address.  Even without realloc,
 *   the content at ptr's byte offset is shifted/corrupted, so lpNext() lands
 *   on a structurally invalid position and lpValidateNext() fires the assert.
 *
 * Scenario B -- LISTPACK_EX + DEL:
 *   The job calls DEL on the entire victim hash.
 *   The listpackEx struct and its lp buffer are freed immediately.
 *   The loop's `ptr` and the local `lpt` variable become dangling pointers;
 *   the subsequent lpNext(lpt->lp, ptr) is a use-after-free.
 *
 * Copyright (c) 2024-Present, Redis Ltd. All rights reserved.
 * Licensed under your choice of RSALv2 or SSPLv1.
 */

#include "redismodule.h"
#include <string.h>

/* Scenario A: HDEL -- delete the field being expired */
#define TRIGGER_HDEL "hfe_hdel_lp_trigger"
#define VICTIM_HDEL  "hfe_hdel_lp_victim"

/* Scenario B: DEL -- delete the entire hash */
#define TRIGGER_DEL  "hfe_del_lp_trigger"
#define VICTIM_DEL   "hfe_del_lp_victim"

/* Post-notification job for Scenario A.
 * Deletes "g1" -- the first-to-expire field in the victim -- while
 * listpackExExpire() is mid-iteration with `ptr` pointing at g1's expiry
 * element.  The resulting listpack mutation invalidates ptr. */
static void HfeLpHdel_PostNotificationJob(RedisModuleCtx *ctx, void *pd) {
    REDISMODULE_NOT_USED(pd);
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "hdel", "!cc",
            VICTIM_HDEL, "g1");
    RedisModule_FreeCallReply(rep);
}

/* Post-notification job for Scenario B.
 * Deletes the entire victim hash, freeing the listpackEx struct and its lp
 * buffer while the loop still holds raw pointers into them. */
static void HfeLpDel_PostNotificationJob(RedisModuleCtx *ctx, void *pd) {
    REDISMODULE_NOT_USED(pd);
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "del", "!c",
            VICTIM_DEL);
    RedisModule_FreeCallReply(rep);
}

/* Keyspace notification callback.
 *
 * When TRIGGER_HDEL or TRIGGER_DEL fires an 'hexpired' event (they expire
 * slightly before the victim), the corresponding job is queued.  The job
 * then runs inside the victim's first propagateHashFieldDeletion() call --
 * i.e. mid-iteration of listpackExExpire() -- triggering the crash.
 */
static int HfeLpHdel_KeyspaceNotification(RedisModuleCtx *ctx, int type,
                                           const char *event,
                                           RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    if (strcmp(event, "hexpired") != 0) return REDISMODULE_OK;

    const char *key_str = RedisModule_StringPtrLen(key, NULL);

    if (strcmp(key_str, TRIGGER_HDEL) == 0) {
        RedisModule_AddPostNotificationJob(ctx,
                HfeLpHdel_PostNotificationJob, NULL, NULL);
    } else if (strcmp(key_str, TRIGGER_DEL) == 0) {
        RedisModule_AddPostNotificationJob(ctx,
                HfeLpDel_PostNotificationJob, NULL, NULL);
    }
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "hfe_listpackex_hdel_crash", 1,
                         REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_HASH,
                HfeLpHdel_KeyspaceNotification) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    REDISMODULE_NOT_USED(ctx);
    return REDISMODULE_OK;
}


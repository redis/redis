/* hfe_crash.c
 *
 * Merged regression-test module covering all known dangling-pointer /
 * use-after-free crashes in Hash Field Expiration (HFE) active expiration,
 * caused by module post-notification jobs mutating the hash mid-iteration.
 *
 * Six scenarios are covered (trigger key → action on victim):
 *
 *  1. LISTPACK_EX + HSET  hfe_crash_trigger    → HSET victim "xfield" <big>
 *  2. LISTPACK_EX + HDEL  hfe_hdel_lp_trigger  → HDEL victim "g1"
 *  3. LISTPACK_EX + DEL   hfe_del_lp_trigger   → DEL  victim
 *  4. HT         + HSET   hfe_hset_ht_trigger  → HSET victim "g1" <big>
 *  5. HT         + HDEL   hfe_hdel_ht_trigger  → HDEL victim "g1"
 *  6. HT         + DEL    hfe_del_ht_trigger   → DEL  victim
 *
 * Copyright (c) 2024-Present, Redis Ltd. All rights reserved.
 * Licensed under your choice of RSALv2 or SSPLv1.
 */

#include "redismodule.h"
#include <string.h>

/* ── trigger / victim key names ─────────────────────────────────────── */
#define TRIGGER_LP_HSET  "hfe_crash_trigger"
#define VICTIM_LP_HSET   "hfe_crash_victim"

#define TRIGGER_LP_HDEL  "hfe_hdel_lp_trigger"
#define VICTIM_LP_HDEL   "hfe_hdel_lp_victim"

#define TRIGGER_LP_DEL   "hfe_del_lp_trigger"
#define VICTIM_LP_DEL    "hfe_del_lp_victim"

#define TRIGGER_HT_HSET  "hfe_hset_ht_trigger"
#define VICTIM_HT_HSET   "hfe_hset_ht_victim"

#define TRIGGER_HT_HDEL  "hfe_hdel_ht_trigger"
#define VICTIM_HT_HDEL   "hfe_hdel_ht_victim"

#define TRIGGER_HT_DEL   "hfe_del_ht_trigger"
#define VICTIM_HT_DEL    "hfe_del_ht_victim"

/* 1000-byte value: crosses malloc-zone boundary on macOS, guaranteeing a
 * new address on lpRealloc(); also forces Entry layout change in HT path. */
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
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

/* ── post-notification job functions ─────────────────────────────────── */
static void Job_LpHset(RedisModuleCtx *ctx, void *pd) {
    REDISMODULE_NOT_USED(pd);
    RedisModuleCallReply *r = RedisModule_Call(ctx, "hset", "!ccc",
            VICTIM_LP_HSET, "xfield", OVERSIZE_VALUE);
    RedisModule_FreeCallReply(r);
}

static void Job_LpHdel(RedisModuleCtx *ctx, void *pd) {
    REDISMODULE_NOT_USED(pd);
    RedisModuleCallReply *r = RedisModule_Call(ctx, "hdel", "!cc",
            VICTIM_LP_HDEL, "g1");
    RedisModule_FreeCallReply(r);
}

static void Job_LpDel(RedisModuleCtx *ctx, void *pd) {
    REDISMODULE_NOT_USED(pd);
    RedisModuleCallReply *r = RedisModule_Call(ctx, "del", "!c",
            VICTIM_LP_DEL);
    RedisModule_FreeCallReply(r);
}

static void Job_HtHset(RedisModuleCtx *ctx, void *pd) {
    REDISMODULE_NOT_USED(pd);
    RedisModuleCallReply *r = RedisModule_Call(ctx, "hset", "!ccc",
            VICTIM_HT_HSET, "g1", OVERSIZE_VALUE);
    RedisModule_FreeCallReply(r);
}

static void Job_HtHdel(RedisModuleCtx *ctx, void *pd) {
    REDISMODULE_NOT_USED(pd);
    RedisModuleCallReply *r = RedisModule_Call(ctx, "hdel", "!cc",
            VICTIM_HT_HDEL, "g1");
    RedisModule_FreeCallReply(r);
}

static void Job_HtDel(RedisModuleCtx *ctx, void *pd) {
    REDISMODULE_NOT_USED(pd);
    RedisModuleCallReply *r = RedisModule_Call(ctx, "del", "!c",
            VICTIM_HT_DEL);
    RedisModule_FreeCallReply(r);
}

/* ── unified keyspace notification callback ──────────────────────────── */
static int HfeCrash_KeyspaceNotification(RedisModuleCtx *ctx, int type,
                                          const char *event,
                                          RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);
    if (strcmp(event, "hexpired") != 0) return REDISMODULE_OK;

    const char *k = RedisModule_StringPtrLen(key, NULL);

    if      (!strcmp(k, TRIGGER_LP_HSET)) RedisModule_AddPostNotificationJob(ctx, Job_LpHset, NULL, NULL);
    else if (!strcmp(k, TRIGGER_LP_HDEL)) RedisModule_AddPostNotificationJob(ctx, Job_LpHdel, NULL, NULL);
    else if (!strcmp(k, TRIGGER_LP_DEL))  RedisModule_AddPostNotificationJob(ctx, Job_LpDel,  NULL, NULL);
    else if (!strcmp(k, TRIGGER_HT_HSET)) RedisModule_AddPostNotificationJob(ctx, Job_HtHset, NULL, NULL);
    else if (!strcmp(k, TRIGGER_HT_HDEL)) RedisModule_AddPostNotificationJob(ctx, Job_HtHdel, NULL, NULL);
    else if (!strcmp(k, TRIGGER_HT_DEL))  RedisModule_AddPostNotificationJob(ctx, Job_HtDel,  NULL, NULL);

    return REDISMODULE_OK;
}

/* ── module lifecycle ────────────────────────────────────────────────── */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "hfe_crash", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
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


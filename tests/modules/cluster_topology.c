/* This module is used to test the RedisModuleEvent_ClusterTopologyChange
 * server event: it subscribes to the event and counts how many times each
 * change reason (carried as a bitmask in the event data) fired, exposing the
 * counters via a command so the TCL tests can assert that modules are notified
 * on cluster topology changes.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "redismodule.h"

static long long event_count = 0;      /* Total notifications received. */
static long long slot_count = 0;       /* Notifications with the SLOT flag. */
static long long role_count = 0;       /* Notifications with the ROLE flag. */
static long long state_count = 0;      /* Notifications with the STATE flag. */
static long long node_count = 0;       /* Notifications with the NODE flag. */

static void clusterTopologyCallback(RedisModuleCtx *ctx, RedisModuleEvent e,
                                    uint64_t subevent, void *data)
{
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(subevent); /* Single subevent; reasons are in the data. */
    if (e.id != REDISMODULE_EVENT_CLUSTER_TOPOLOGY_CHANGE) return;

    RedisModuleClusterTopologyChangeInfo *info = data;
    event_count++;
    if (info->change_flags & REDISMODULE_CLUSTER_TOPOLOGY_CHANGE_FLAG_SLOT)
        slot_count++;
    if (info->change_flags & REDISMODULE_CLUSTER_TOPOLOGY_CHANGE_FLAG_ROLE)
        role_count++;
    if (info->change_flags & REDISMODULE_CLUSTER_TOPOLOGY_CHANGE_FLAG_STATE)
        state_count++;
    if (info->change_flags & REDISMODULE_CLUSTER_TOPOLOGY_CHANGE_FLAG_NODE)
        node_count++;
}

/* cluster_topology.stats -> {events, slot, role, state, node} */
static int statsCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);
    RedisModule_ReplyWithMap(ctx, 5);
    RedisModule_ReplyWithCString(ctx, "events");
    RedisModule_ReplyWithLongLong(ctx, event_count);
    RedisModule_ReplyWithCString(ctx, "slot");
    RedisModule_ReplyWithLongLong(ctx, slot_count);
    RedisModule_ReplyWithCString(ctx, "role");
    RedisModule_ReplyWithLongLong(ctx, role_count);
    RedisModule_ReplyWithCString(ctx, "state");
    RedisModule_ReplyWithLongLong(ctx, state_count);
    RedisModule_ReplyWithCString(ctx, "node");
    RedisModule_ReplyWithLongLong(ctx, node_count);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "cluster_topology", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_SubscribeToServerEvent(ctx,
            RedisModuleEvent_ClusterTopologyChange, clusterTopologyCallback) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "cluster_topology.stats", statsCommand,
            "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

/* This module is used to test the RedisModuleEvent_ClusterTopologyChange
 * server event: it subscribes to the event and counts how many times each
 * subevent fired, exposing the counters via a command so the TCL tests can
 * assert that modules are notified on cluster topology changes.
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

static long long startup_count = 0;
static long long topology_count = 0;
static long long role_count = 0;
static long long other_count = 0;

static void clusterTopologyCallback(RedisModuleCtx *ctx, RedisModuleEvent e,
                                    uint64_t subevent, void *data)
{
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(data); /* The event carries no payload. */
    if (e.id != REDISMODULE_EVENT_CLUSTER_TOPOLOGY_CHANGE) return;

    switch (subevent) {
    case REDISMODULE_SUBEVENT_CLUSTER_TOPOLOGY_CHANGE_STARTUP:
        startup_count++; break;
    case REDISMODULE_SUBEVENT_CLUSTER_TOPOLOGY_CHANGE_TOPOLOGY_CHANGED:
        topology_count++; break;
    case REDISMODULE_SUBEVENT_CLUSTER_TOPOLOGY_CHANGE_ROLE_CHANGED:
        role_count++; break;
    default:
        other_count++; break;
    }
}

/* cluster_topology.stats -> [startup, topology, role, other] */
static int statsCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);
    RedisModule_ReplyWithArray(ctx, 4);
    RedisModule_ReplyWithLongLong(ctx, startup_count);
    RedisModule_ReplyWithLongLong(ctx, topology_count);
    RedisModule_ReplyWithLongLong(ctx, role_count);
    RedisModule_ReplyWithLongLong(ctx, other_count);
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

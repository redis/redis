/* Module to test RM_RegisterClusterMessageReceiver linked list management.
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "redismodule.h"

static void receiverCallback(RedisModuleCtx *ctx, const char *sender_id,
                              uint8_t type, const unsigned char *payload,
                              uint32_t len) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(sender_id);
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(payload);
    REDISMODULE_NOT_USED(len);
}

/* CLUSTERRECEIVER.REGISTER <type>
 * Register a cluster message receiver for the given message type (0-255). */
static int ClusterReceiverRegister(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);
    long long type;
    if (RedisModule_StringToLongLong(argv[1], &type) != REDISMODULE_OK ||
        type < 0 || type > 255)
        return RedisModule_ReplyWithError(ctx, "ERR invalid type, must be 0-255");
    RedisModule_RegisterClusterMessageReceiver(ctx, (uint8_t)type, receiverCallback);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* CLUSTERRECEIVER.UNREGISTER <type>
 * Unregister the cluster message receiver for the given message type.
 * Passing NULL as callback removes the entry from the clusterReceivers linked list. */
static int ClusterReceiverUnregister(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);
    long long type;
    if (RedisModule_StringToLongLong(argv[1], &type) != REDISMODULE_OK ||
        type < 0 || type > 255)
        return RedisModule_ReplyWithError(ctx, "ERR invalid type, must be 0-255");
    RedisModule_RegisterClusterMessageReceiver(ctx, (uint8_t)type, NULL);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "clusterreceiver", 1, REDISMODULE_APIVER_1) != REDISMODULE_OK)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "clusterreceiver.register",
                                   ClusterReceiverRegister, "readonly", 0, 0, 0) != REDISMODULE_OK)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "clusterreceiver.unregister",
                                   ClusterReceiverUnregister, "readonly", 0, 0, 0) != REDISMODULE_OK)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

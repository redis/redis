/* Server hooks API example
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2019-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "../redismodule.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

/* Client state change callback. */
void clientChangeCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(e);

    RedisModuleClientInfo *ci = data;
    printf("Client %s event for client #%llu %s:%d\n",
        (sub == REDISMODULE_SUBEVENT_CLIENT_CHANGE_CONNECTED) ?
            "connection" : "disconnection",
        (unsigned long long)ci->id,ci->addr,ci->port);
}

void flushdbCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data)
{
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(e);

    RedisModuleFlushInfo *fi = data;
    if (sub == REDISMODULE_SUBEVENT_FLUSHDB_START) {
        if (fi->dbnum != -1) {
            RedisModuleCallReply *reply;
            reply = RedisModule_Call(ctx,"DBSIZE","");
            long long numkeys = RedisModule_CallReplyInteger(reply);
            printf("FLUSHDB event of database %d started (%lld keys in DB)\n",
                fi->dbnum, numkeys);
            RedisModule_FreeCallReply(reply);
        } else {
            printf("FLUSHALL event started\n");
        }
    } else {
        if (fi->dbnum != -1) {
            printf("FLUSHDB event of database %d ended\n",fi->dbnum);
        } else {
            printf("FLUSHALL event ended\n");
        }
    }
}

/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"hellohook",1,REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_ClientChange, clientChangeCallback);
    RedisModule_SubscribeToServerEvent(ctx,
        RedisModuleEvent_FlushDB, flushdbCallback);
    return REDISMODULE_OK;
}

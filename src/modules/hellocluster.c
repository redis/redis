/* Helloworld cluster -- A ping/pong cluster API example.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2018, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define REDISMODULE_EXPERIMENTAL_API
#include "../redismodule.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#define MSGTYPE_PING 1
#define MSGTYPE_PONG 2

/* HELLOCLUSTER.PINGALL */
int PingallCommand_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_SendClusterMessage(ctx,NULL,MSGTYPE_PING,(unsigned char*)"Hey",3);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* HELLOCLUSTER.LIST */
int ListCommand_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    size_t numnodes;
    char **ids = RedisModule_GetClusterNodesList(ctx,&numnodes);
    if (ids == NULL) {
        return RedisModule_ReplyWithError(ctx,"Cluster not enabled");
    }

    RedisModule_ReplyWithArray(ctx,numnodes);
    for (size_t j = 0; j < numnodes; j++) {
        int port;
        RedisModule_GetClusterNodeInfo(ctx,ids[j],NULL,NULL,&port,NULL);
        RedisModule_ReplyWithArray(ctx,2);
        RedisModule_ReplyWithStringBuffer(ctx,ids[j],REDISMODULE_NODE_ID_LEN);
        RedisModule_ReplyWithLongLong(ctx,port);
    }
    RedisModule_FreeClusterNodesList(ids);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* Callback for message MSGTYPE_PING */
void PingReceiver(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len) {
    RedisModule_Log(ctx,"notice","PING (type %d) RECEIVED from %.*s: '%.*s'",
        type,REDISMODULE_NODE_ID_LEN,sender_id,(int)len, payload);
    RedisModule_SendClusterMessage(ctx,NULL,MSGTYPE_PONG,(unsigned char*)"Ohi!",4);
}

/* Callback for message MSGTYPE_PONG. */
void PongReceiver(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len) {
    RedisModule_Log(ctx,"notice","PONG (type %d) RECEIVED from %.*s: '%.*s'",
        type,REDISMODULE_NODE_ID_LEN,sender_id,(int)len, payload);
}

/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"hellocluster",1,REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"hellocluster.pingall",
        PingallCommand_RedisCommand,"readonly",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"hellocluster.list",
        ListCommand_RedisCommand,"readonly",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModule_RegisterClusterMessageReceiver(ctx,MSGTYPE_PING,PingReceiver);
    RedisModule_RegisterClusterMessageReceiver(ctx,MSGTYPE_PONG,PongReceiver);
    return REDISMODULE_OK;
}

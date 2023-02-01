/* This module is used to test the channel subscribe/unsubscribe API.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright 2021 Harkrishn Patro, Amazon.com, Inc. or its affiliates.
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

#include "redismodule.h"

RedisModuleString *event;

void ChannelSubscriptionCallback(RedisModuleCtx *ctx, RedisModuleString *channel, RedisModuleString *message) {
    RedisModuleString *msg = RedisModule_CreateString(ctx, "clear", 5);
    RedisModuleString *unsubscribe_msg = RedisModule_CreateString(ctx, "unsubscribe", 11);
    if (!RedisModule_StringCompare(event, channel)) {
        if (!RedisModule_StringCompare(msg, message)) {
            RedisModuleCallReply* rep = RedisModule_Call(ctx, "FLUSHALL", "");
            RedisModule_FreeCallReply(rep);
        } else if (!RedisModule_StringCompare(unsubscribe_msg, message)) {
            RedisModule_UnsubscribeFromChannel(ctx, channel);
        }
    }
    RedisModule_FreeString(ctx, msg);
    RedisModule_FreeString(ctx, unsubscribe_msg);
}

static int subscribeToChannel(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if(argc != 2){
        return RedisModule_WrongArity(ctx);
    }
    if(RedisModule_SubscribeToChannel(ctx, argv[1], ChannelSubscriptionCallback) != REDISMODULE_OK){
        RedisModule_ReplyWithError(ctx, "Channel subscription failed");
        return REDISMODULE_ERR;
    }
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

static int unsubscribeFromChannel(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if(argc != 2){
        return RedisModule_WrongArity(ctx);
    }
    if(RedisModule_UnsubscribeFromChannel(ctx, argv[1]) != REDISMODULE_OK){
        RedisModule_ReplyWithError(ctx, "Channel unsubscription failed");
        return REDISMODULE_ERR;
    }
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "subscribech", 1, REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    event = RedisModule_CreateString(ctx, "event", 5);
    if(RedisModule_SubscribeToChannel(ctx, event, ChannelSubscriptionCallback) != REDISMODULE_OK){
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx,"subscribech.subscribe_to_channel", subscribeToChannel, "", 1, 1, 0) == REDISMODULE_ERR){
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx,"subscribech.unsubscribe_from_channel", unsubscribeFromChannel, "", 1, 1, 0) == REDISMODULE_ERR){
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    RedisModule_FreeString(ctx, event);
    return REDISMODULE_OK;
}


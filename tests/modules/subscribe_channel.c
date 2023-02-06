/*
 * This module is used to test the channel subscribe/unsubscribe API.
 */

#define REDISMODULE_EXPERIMENTAL_API

#include "redismodule.h"

#define UNUSED(x) (void)(x)

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

int listChannels(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    size_t numchannels;
    RedisModuleString **channels = RedisModule_GlobalChannelSubscriptionList(ctx, &numchannels);
    if (numchannels == 0) {
        RedisModule_ReplyWithNull(ctx);
        return REDISMODULE_OK;
    }
    RedisModule_ReplyWithArray(ctx, numchannels);
    for (size_t i = 0; i < numchannels; i++) {
        RedisModule_ReplyWithString(ctx, channels[i]);
    }
    RedisModule_FreeGlobalChannelSubscriptionList(channels);
    return REDISMODULE_OK;
}

/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "subscribech", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    event = RedisModule_CreateString(ctx, "event", 5);
    if(RedisModule_SubscribeToChannel(ctx, event, ChannelSubscriptionCallback) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "subscribech.subscribe_to_channel", subscribeToChannel, "", 1, 1, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "subscribech.unsubscribe_from_channel", unsubscribeFromChannel, "", 1, 1, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "subscribech.list", listChannels, "", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    RedisModule_FreeString(ctx, event);
    return REDISMODULE_OK;
}


/*
 * This module is used to test the channel subscribe/unsubscribe API.
 */

#define REDISMODULE_EXPERIMENTAL_API

#include <strings.h>
#include "redismodule.h"

#define UNUSED(x) (void)(x)

void ChannelSubscriptionCallback(RedisModuleCtx *ctx, RedisModuleString *channel, RedisModuleString *message) {
    RedisModuleString *eventChannel = RedisModule_CreateString(ctx, "event", 5);
    RedisModuleString *msg = RedisModule_CreateString(ctx, "clear", 5);
    RedisModuleString *unsubscribe_msg = RedisModule_CreateString(ctx, "unsubscribe", 11);
    if (!RedisModule_StringCompare(eventChannel, channel)) {
        if (!RedisModule_StringCompare(msg, message)) {
            RedisModuleCallReply* rep = RedisModule_Call(ctx, "FLUSHALL", "");
            RedisModule_FreeCallReply(rep);
        } else if (!RedisModule_StringCompare(unsubscribe_msg, message)) {
            RedisModule_UnsubscribeFromChannel(ctx, channel, 0);
        }
    }
    RedisModule_FreeString(ctx, eventChannel);
    RedisModule_FreeString(ctx, msg);
    RedisModule_FreeString(ctx, unsubscribe_msg);
}

void ShardChannelSubscriptionCallback(RedisModuleCtx *ctx, RedisModuleString *channel, RedisModuleString *message) {
    RedisModuleString *eventChannel = RedisModule_CreateString(ctx, "shardevent", 10);
    RedisModuleString *msg = RedisModule_CreateString(ctx, "clear", 5);
    RedisModuleString *unsubscribe_msg = RedisModule_CreateString(ctx, "unsubscribe", 11);
    if (!RedisModule_StringCompare(eventChannel, channel)) {
        if (!RedisModule_StringCompare(msg, message)) {
            RedisModuleCallReply* rep = RedisModule_Call(ctx, "FLUSHALL", "");
            RedisModule_FreeCallReply(rep);
        } else if (!RedisModule_StringCompare(unsubscribe_msg, message)) {
            RedisModule_UnsubscribeFromChannel(ctx, channel, 1);
        }
    }
    RedisModule_FreeString(ctx, eventChannel);
    RedisModule_FreeString(ctx, msg);
    RedisModule_FreeString(ctx, unsubscribe_msg);
}

static int subscribeToAllChannels(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    if (argc != 1) {
        return RedisModule_WrongArity(ctx);
    }
    RedisModule_SubscribeToAllChannels(ctx, ChannelSubscriptionCallback);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

static int unsubscribeFromAllChannels(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    if (argc != 1) {
        return RedisModule_WrongArity(ctx);
    }
    RedisModule_UnsubscribeFromAllChannels(ctx);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

static int subscribeToChannel(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if(argc != 3){
        return RedisModule_WrongArity(ctx);
    }
    const char *type = RedisModule_StringPtrLen(argv[1], NULL);
    if (!strcasecmp(type, "classic")) {
        RedisModule_SubscribeToChannel(ctx, argv[2], ChannelSubscriptionCallback, 0);
    } else if (!strcasecmp(type, "shard")) {
        RedisModule_SubscribeToChannel(ctx, argv[2], ShardChannelSubscriptionCallback, 1);
    } else {
        RedisModule_ReplyWithError(ctx, "Invalid arguments!");
        return REDISMODULE_ERR;
    }
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

static int unsubscribeFromChannel(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if(argc != 3){
        return RedisModule_WrongArity(ctx);
    }
    const char *type = RedisModule_StringPtrLen(argv[1], NULL);
    if (!strcasecmp(type, "classic")) {
        RedisModule_UnsubscribeFromChannel(ctx, argv[2], 0);
    } else if (!strcasecmp(type, "shard")) {
        RedisModule_UnsubscribeFromChannel(ctx, argv[2], 1);
    } else {
        RedisModule_ReplyWithError(ctx, "Invalid arguments!");
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

    if (RedisModule_Init(ctx, "subscribech", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModuleString *event = RedisModule_CreateString(ctx, "event", 5);
    RedisModuleString *shardevent = RedisModule_CreateString(ctx, "shardevent", 10);
    RedisModule_SubscribeToChannel(ctx, event, ChannelSubscriptionCallback, 0);
    RedisModule_SubscribeToChannel(ctx, shardevent, ShardChannelSubscriptionCallback, 1);
    RedisModule_FreeString(ctx, event);
    RedisModule_FreeString(ctx, shardevent);

    if (RedisModule_CreateCommand(ctx, "subscribech.subscribe_to_channel", subscribeToChannel, "", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "subscribech.unsubscribe_from_channel", unsubscribeFromChannel, "", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "subscribech.subscribe_to_all_channels", subscribeToAllChannels, "", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "subscribech.unsubscribe_from_all_channels", unsubscribeFromAllChannels, "", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    UNUSED(ctx);

    return REDISMODULE_OK;
}


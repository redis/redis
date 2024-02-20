/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "server.h"
#include "cluster.h"

/* Structure to hold the pubsub related metadata. Currently used
 * for pubsub and pubsubshard feature. */
typedef struct pubsubtype {
    int shard;
    dict *(*clientPubSubChannels)(client*);
    int (*subscriptionCount)(client*);
    dict **(*serverPubSubChannels)(unsigned int);
    robj **subscribeMsg;
    robj **unsubscribeMsg;
    robj **messageBulk;
}pubsubtype;

/*
 * Get client's global Pub/Sub channels subscription count.
 */
int clientSubscriptionsCount(client *c);

/*
 * Get client's shard level Pub/Sub channels subscription count.
 */
int clientShardSubscriptionsCount(client *c);

/*
 * Get client's global Pub/Sub channels dict.
 */
dict* getClientPubSubChannels(client *c);

/*
 * Get client's shard level Pub/Sub channels dict.
 */
dict* getClientPubSubShardChannels(client *c);

/*
 * Get server's global Pub/Sub channels dict.
 */
dict **getServerPubSubChannels(unsigned int slot);

/*
 * Get server's shard level Pub/Sub channels dict.
 */
dict **getServerPubSubShardChannels(unsigned int slot);

/*
 * Get list of channels client is subscribed to.
 * If a pattern is provided, the subset of channels is returned
 * matching the pattern.
 */
void channelList(client *c, sds pat, dict** pubsub_channels, int is_sharded);

/*
 * Pub/Sub type for global channels.
 */
pubsubtype pubSubType = {
    .shard = 0,
    .clientPubSubChannels = getClientPubSubChannels,
    .subscriptionCount = clientSubscriptionsCount,
    .serverPubSubChannels = getServerPubSubChannels,
    .subscribeMsg = &shared.subscribebulk,
    .unsubscribeMsg = &shared.unsubscribebulk,
    .messageBulk = &shared.messagebulk,
};

/*
 * Pub/Sub type for shard level channels bounded to a slot.
 */
pubsubtype pubSubShardType = {
    .shard = 1,
    .clientPubSubChannels = getClientPubSubShardChannels,
    .subscriptionCount = clientShardSubscriptionsCount,
    .serverPubSubChannels = getServerPubSubShardChannels,
    .subscribeMsg = &shared.ssubscribebulk,
    .unsubscribeMsg = &shared.sunsubscribebulk,
    .messageBulk = &shared.smessagebulk,
};

/*-----------------------------------------------------------------------------
 * Pubsub client replies API
 *----------------------------------------------------------------------------*/

/* Send a pubsub message of type "message" to the client.
 * Normally 'msg' is a Redis object containing the string to send as
 * message. However if the caller sets 'msg' as NULL, it will be able
 * to send a special message (for instance an Array type) by using the
 * addReply*() API family. */
void addReplyPubsubMessage(client *c, robj *channel, robj *msg, robj *message_bulk) {
    uint64_t old_flags = c->flags;
    c->flags |= CLIENT_PUSHING;
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,message_bulk);
    addReplyBulk(c,channel);
    if (msg) addReplyBulk(c,msg);
    if (!(old_flags & CLIENT_PUSHING)) c->flags &= ~CLIENT_PUSHING;
}

/* Send a pubsub message of type "pmessage" to the client. The difference
 * with the "message" type delivered by addReplyPubsubMessage() is that
 * this message format also includes the pattern that matched the message. */
void addReplyPubsubPatMessage(client *c, robj *pat, robj *channel, robj *msg) {
    uint64_t old_flags = c->flags;
    c->flags |= CLIENT_PUSHING;
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[4]);
    else
        addReplyPushLen(c,4);
    addReply(c,shared.pmessagebulk);
    addReplyBulk(c,pat);
    addReplyBulk(c,channel);
    addReplyBulk(c,msg);
    if (!(old_flags & CLIENT_PUSHING)) c->flags &= ~CLIENT_PUSHING;
}

/* Send the pubsub subscription notification to the client. */
void addReplyPubsubSubscribed(client *c, robj *channel, pubsubtype type) {
    uint64_t old_flags = c->flags;
    c->flags |= CLIENT_PUSHING;
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,*type.subscribeMsg);
    addReplyBulk(c,channel);
    addReplyLongLong(c,type.subscriptionCount(c));
    if (!(old_flags & CLIENT_PUSHING)) c->flags &= ~CLIENT_PUSHING;
}

/* Send the pubsub unsubscription notification to the client.
 * Channel can be NULL: this is useful when the client sends a mass
 * unsubscribe command but there are no channels to unsubscribe from: we
 * still send a notification. */
void addReplyPubsubUnsubscribed(client *c, robj *channel, pubsubtype type) {
    uint64_t old_flags = c->flags;
    c->flags |= CLIENT_PUSHING;
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c, *type.unsubscribeMsg);
    if (channel)
        addReplyBulk(c,channel);
    else
        addReplyNull(c);
    addReplyLongLong(c,type.subscriptionCount(c));
    if (!(old_flags & CLIENT_PUSHING)) c->flags &= ~CLIENT_PUSHING;
}

/* Send the pubsub pattern subscription notification to the client. */
void addReplyPubsubPatSubscribed(client *c, robj *pattern) {
    uint64_t old_flags = c->flags;
    c->flags |= CLIENT_PUSHING;
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,shared.psubscribebulk);
    addReplyBulk(c,pattern);
    addReplyLongLong(c,clientSubscriptionsCount(c));
    if (!(old_flags & CLIENT_PUSHING)) c->flags &= ~CLIENT_PUSHING;
}

/* Send the pubsub pattern unsubscription notification to the client.
 * Pattern can be NULL: this is useful when the client sends a mass
 * punsubscribe command but there are no pattern to unsubscribe from: we
 * still send a notification. */
void addReplyPubsubPatUnsubscribed(client *c, robj *pattern) {
    uint64_t old_flags = c->flags;
    c->flags |= CLIENT_PUSHING;
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,shared.punsubscribebulk);
    if (pattern)
        addReplyBulk(c,pattern);
    else
        addReplyNull(c);
    addReplyLongLong(c,clientSubscriptionsCount(c));
    if (!(old_flags & CLIENT_PUSHING)) c->flags &= ~CLIENT_PUSHING;
}

/*-----------------------------------------------------------------------------
 * Pubsub low level API
 *----------------------------------------------------------------------------*/

/* Return the number of pubsub channels + patterns is handled. */
int serverPubsubSubscriptionCount(void) {
    return dictSize(server.pubsub_channels) + dictSize(server.pubsub_patterns);
}

/* Return the number of pubsub shard level channels is handled. */
int serverPubsubShardSubscriptionCount(void) {
    return server.shard_channel_count;
}


/* Return the number of channels + patterns a client is subscribed to. */
int clientSubscriptionsCount(client *c) {
    return dictSize(c->pubsub_channels) + dictSize(c->pubsub_patterns);
}

/* Return the number of shard level channels a client is subscribed to. */
int clientShardSubscriptionsCount(client *c) {
    return dictSize(c->pubsubshard_channels);
}

dict* getClientPubSubChannels(client *c) {
    return c->pubsub_channels;
}

dict* getClientPubSubShardChannels(client *c) {
    return c->pubsubshard_channels;
}

dict **getServerPubSubChannels(unsigned int slot) {
    UNUSED(slot);
    return &server.pubsub_channels;
}

dict **getServerPubSubShardChannels(unsigned int slot) {
    serverAssert(server.cluster_enabled || slot == 0);
    return &server.pubsubshard_channels[slot];
}

/* Return the number of pubsub + pubsub shard level channels
 * a client is subscribed to. */
int clientTotalPubSubSubscriptionCount(client *c) {
    return clientSubscriptionsCount(c) + clientShardSubscriptionsCount(c);
}

void markClientAsPubSub(client *c) {
    if (!(c->flags & CLIENT_PUBSUB)) {
        c->flags |= CLIENT_PUBSUB;
        server.pubsub_clients++;
    }
}

void unmarkClientAsPubSub(client *c) {
    if (c->flags & CLIENT_PUBSUB) {
        c->flags &= ~CLIENT_PUBSUB;
        server.pubsub_clients--;
    }
}

/* Subscribe a client to a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was already subscribed to that channel. */
int pubsubSubscribeChannel(client *c, robj *channel, pubsubtype type) {
    dict **d_ptr;
    dictEntry *de;
    dict *clients = NULL;
    int retval = 0;
    unsigned int slot = 0;

    /* Add the channel to the client -> channels hash table */
    if (dictAdd(type.clientPubSubChannels(c),channel,NULL) == DICT_OK) {
        retval = 1;
        incrRefCount(channel);
        /* Add the client to the channel -> list of clients hash table */
        if (server.cluster_enabled && type.shard) {
            slot = getKeySlot(channel->ptr);
        }
        d_ptr = type.serverPubSubChannels(slot);
        if (*d_ptr == NULL) {
            *d_ptr = dictCreate(&objToDictDictType);
            de = NULL;
        } else {
            de = dictFind(*d_ptr, channel);
        }
        if (de == NULL) {
            clients = dictCreate(&clientDictType);
            dictAdd(*d_ptr, channel, clients);
            incrRefCount(channel);
            if (type.shard) {
                server.shard_channel_count++;
            }
        } else {
            clients = dictGetVal(de);
        }
        serverAssert(dictAdd(clients, c, NULL) != DICT_ERR);
    }
    /* Notify the client */
    addReplyPubsubSubscribed(c,channel,type);
    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
int pubsubUnsubscribeChannel(client *c, robj *channel, int notify, pubsubtype type) {
    dict *d;
    dictEntry *de;
    dict *clients;
    int retval = 0;
    int slot = 0;

    /* Remove the channel from the client -> channels hash table */
    incrRefCount(channel); /* channel may be just a pointer to the same object
                            we have in the hash tables. Protect it... */
    if (dictDelete(type.clientPubSubChannels(c),channel) == DICT_OK) {
        retval = 1;
        /* Remove the client from the channel -> clients list hash table */
        if (server.cluster_enabled && type.shard) {
            slot = getKeySlot(channel->ptr);
        }
        d = *type.serverPubSubChannels(slot);
        serverAssertWithInfo(c,NULL,d != NULL);
        de = dictFind(d, channel);
        serverAssertWithInfo(c,NULL,de != NULL);
        clients = dictGetVal(de);
        serverAssertWithInfo(c, NULL, dictDelete(clients, c) == DICT_OK);
        if (dictSize(clients) == 0) {
            /* Free the dict and associated hash entry at all if this was
             * the latest client, so that it will be possible to abuse
             * Redis PUBSUB creating millions of channels. */
            dictDelete(d, channel);
            if (type.shard) {
                if (dictSize(d) == 0) {
                    dictRelease(d);
                    dict **d_ptr = type.serverPubSubChannels(slot);
                    *d_ptr = NULL;
                }
                server.shard_channel_count--;
            }
        }
    }
    /* Notify the client */
    if (notify) {
        addReplyPubsubUnsubscribed(c,channel,type);
    }
    decrRefCount(channel); /* it is finally safe to release it */
    return retval;
}

/* Unsubscribe all shard channels in a slot. */
void pubsubShardUnsubscribeAllChannelsInSlot(unsigned int slot) {
    dict *d = server.pubsubshard_channels[slot];
    if (!d) {
        return;
    }
    dictIterator *di = dictGetSafeIterator(d);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        robj *channel = dictGetKey(de);
        dict *clients = dictGetVal(de);
        /* For each client subscribed to the channel, unsubscribe it. */
        dictIterator *iter = dictGetIterator(clients);
        dictEntry *entry;
        while ((entry = dictNext(iter)) != NULL) {
            client *c = dictGetKey(entry);
            int retval = dictDelete(c->pubsubshard_channels, channel);
            serverAssertWithInfo(c,channel,retval == DICT_OK);
            addReplyPubsubUnsubscribed(c, channel, pubSubShardType);
            /* If the client has no other pubsub subscription,
             * move out of pubsub mode. */
            if (clientTotalPubSubSubscriptionCount(c) == 0) {
                unmarkClientAsPubSub(c);
            }
        }
        dictReleaseIterator(iter);
        server.shard_channel_count--;
        dictDelete(d, channel);
    }
    dictReleaseIterator(di);
    dictRelease(d);
    server.pubsubshard_channels[slot] = NULL;
}

/* Subscribe a client to a pattern. Returns 1 if the operation succeeded, or 0 if the client was already subscribed to that pattern. */
int pubsubSubscribePattern(client *c, robj *pattern) {
    dictEntry *de;
    dict *clients;
    int retval = 0;

    if (dictAdd(c->pubsub_patterns, pattern, NULL) == DICT_OK) {
        retval = 1;
        incrRefCount(pattern);
        /* Add the client to the pattern -> list of clients hash table */
        de = dictFind(server.pubsub_patterns,pattern);
        if (de == NULL) {
            clients = dictCreate(&clientDictType);
            dictAdd(server.pubsub_patterns,pattern,clients);
            incrRefCount(pattern);
        } else {
            clients = dictGetVal(de);
        }
        serverAssert(dictAdd(clients, c, NULL) != DICT_ERR);
    }
    /* Notify the client */
    addReplyPubsubPatSubscribed(c,pattern);
    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
int pubsubUnsubscribePattern(client *c, robj *pattern, int notify) {
    dictEntry *de;
    dict *clients;
    int retval = 0;

    incrRefCount(pattern); /* Protect the object. May be the same we remove */
    if (dictDelete(c->pubsub_patterns, pattern) == DICT_OK) {
        retval = 1;
        /* Remove the client from the pattern -> clients list hash table */
        de = dictFind(server.pubsub_patterns,pattern);
        serverAssertWithInfo(c,NULL,de != NULL);
        clients = dictGetVal(de);
        serverAssertWithInfo(c, NULL, dictDelete(clients, c) == DICT_OK);
        if (dictSize(clients) == 0) {
            /* Free the dict and associated hash entry at all if this was
             * the latest client. */
            dictDelete(server.pubsub_patterns,pattern);
        }
    }
    /* Notify the client */
    if (notify) addReplyPubsubPatUnsubscribed(c,pattern);
    decrRefCount(pattern);
    return retval;
}

/* Unsubscribe from all the channels. Return the number of channels the
 * client was subscribed to. */
int pubsubUnsubscribeAllChannelsInternal(client *c, int notify, pubsubtype type) {
    int count = 0;
    if (dictSize(type.clientPubSubChannels(c)) > 0) {
        dictIterator *di = dictGetSafeIterator(type.clientPubSubChannels(c));
        dictEntry *de;

        while((de = dictNext(di)) != NULL) {
            robj *channel = dictGetKey(de);

            count += pubsubUnsubscribeChannel(c,channel,notify,type);
        }
        dictReleaseIterator(di);
    }
    /* We were subscribed to nothing? Still reply to the client. */
    if (notify && count == 0) {
        addReplyPubsubUnsubscribed(c,NULL,type);
    }
    return count;
}

/*
 * Unsubscribe a client from all global channels.
 */
int pubsubUnsubscribeAllChannels(client *c, int notify) {
    int count = pubsubUnsubscribeAllChannelsInternal(c,notify,pubSubType);
    return count;
}

/*
 * Unsubscribe a client from all shard subscribed channels.
 */
int pubsubUnsubscribeShardAllChannels(client *c, int notify) {
    int count = pubsubUnsubscribeAllChannelsInternal(c, notify, pubSubShardType);
    return count;
}

/* Unsubscribe from all the patterns. Return the number of patterns the
 * client was subscribed from. */
int pubsubUnsubscribeAllPatterns(client *c, int notify) {
    int count = 0;

    if (dictSize(c->pubsub_patterns) > 0) {
        dictIterator *di = dictGetSafeIterator(c->pubsub_patterns);
        dictEntry *de;

        while ((de = dictNext(di)) != NULL) {
            robj *pattern = dictGetKey(de);
            count += pubsubUnsubscribePattern(c, pattern, notify);
        }
        dictReleaseIterator(di);
    }

    /* We were subscribed to nothing? Still reply to the client. */
    if (notify && count == 0) addReplyPubsubPatUnsubscribed(c,NULL);
    return count;
}

/*
 * Publish a message to all the subscribers.
 */
int pubsubPublishMessageInternal(robj *channel, robj *message, pubsubtype type) {
    int receivers = 0;
    dict *d;
    dictEntry *de;
    dictIterator *di;
    unsigned int slot = 0;

    /* Send to clients listening for that channel */
    if (server.cluster_enabled && type.shard) {
        slot = keyHashSlot(channel->ptr, sdslen(channel->ptr));
    }
    d = *type.serverPubSubChannels(slot);
    de = d ? dictFind(d, channel) : NULL;
    if (de) {
        dict *clients = dictGetVal(de);
        dictEntry *entry;
        dictIterator *iter = dictGetIterator(clients);
        while ((entry = dictNext(iter)) != NULL) {
            client *c = dictGetKey(entry);
            addReplyPubsubMessage(c,channel,message,*type.messageBulk);
            updateClientMemUsageAndBucket(c);
            receivers++;
        }
        dictReleaseIterator(iter);
    }

    if (type.shard) {
        /* Shard pubsub ignores patterns. */
        return receivers;
    }

    /* Send to clients listening to matching channels */
    di = dictGetIterator(server.pubsub_patterns);
    if (di) {
        channel = getDecodedObject(channel);
        while((de = dictNext(di)) != NULL) {
            robj *pattern = dictGetKey(de);
            dict *clients = dictGetVal(de);
            if (!stringmatchlen((char*)pattern->ptr,
                                sdslen(pattern->ptr),
                                (char*)channel->ptr,
                                sdslen(channel->ptr),0)) continue;

            dictEntry *entry;
            dictIterator *iter = dictGetIterator(clients);
            while ((entry = dictNext(iter)) != NULL) {
                client *c = dictGetKey(entry);
                addReplyPubsubPatMessage(c,pattern,channel,message);
                updateClientMemUsageAndBucket(c);
                receivers++;
            }
            dictReleaseIterator(iter);
        }
        decrRefCount(channel);
        dictReleaseIterator(di);
    }
    return receivers;
}

/* Publish a message to all the subscribers. */
int pubsubPublishMessage(robj *channel, robj *message, int sharded) {
    return pubsubPublishMessageInternal(channel, message, sharded? pubSubShardType : pubSubType);
}

/*-----------------------------------------------------------------------------
 * Pubsub commands implementation
 *----------------------------------------------------------------------------*/

/* SUBSCRIBE channel [channel ...] */
void subscribeCommand(client *c) {
    int j;
    if ((c->flags & CLIENT_DENY_BLOCKING) && !(c->flags & CLIENT_MULTI)) {
        /**
         * A client that has CLIENT_DENY_BLOCKING flag on
         * expect a reply per command and so can not execute subscribe.
         *
         * Notice that we have a special treatment for multi because of
         * backward compatibility
         */
        addReplyError(c, "SUBSCRIBE isn't allowed for a DENY BLOCKING client");
        return;
    }
    for (j = 1; j < c->argc; j++)
        pubsubSubscribeChannel(c,c->argv[j],pubSubType);
    markClientAsPubSub(c);
}

/* UNSUBSCRIBE [channel ...] */
void unsubscribeCommand(client *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeAllChannels(c,1);
    } else {
        int j;

        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribeChannel(c,c->argv[j],1,pubSubType);
    }
    if (clientTotalPubSubSubscriptionCount(c) == 0) {
        unmarkClientAsPubSub(c);
    }
}

/* PSUBSCRIBE pattern [pattern ...] */
void psubscribeCommand(client *c) {
    int j;
    if ((c->flags & CLIENT_DENY_BLOCKING) && !(c->flags & CLIENT_MULTI)) {
        /**
         * A client that has CLIENT_DENY_BLOCKING flag on
         * expect a reply per command and so can not execute subscribe.
         *
         * Notice that we have a special treatment for multi because of
         * backward compatibility
         */
        addReplyError(c, "PSUBSCRIBE isn't allowed for a DENY BLOCKING client");
        return;
    }

    for (j = 1; j < c->argc; j++)
        pubsubSubscribePattern(c,c->argv[j]);
    markClientAsPubSub(c);
}

/* PUNSUBSCRIBE [pattern [pattern ...]] */
void punsubscribeCommand(client *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeAllPatterns(c,1);
    } else {
        int j;

        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribePattern(c,c->argv[j],1);
    }
    if (clientTotalPubSubSubscriptionCount(c) == 0) {
        unmarkClientAsPubSub(c);
    }
}

/* This function wraps pubsubPublishMessage and also propagates the message to cluster.
 * Used by the commands PUBLISH/SPUBLISH and their respective module APIs.*/
int pubsubPublishMessageAndPropagateToCluster(robj *channel, robj *message, int sharded) {
    int receivers = pubsubPublishMessage(channel, message, sharded);
    if (server.cluster_enabled)
        clusterPropagatePublish(channel, message, sharded);
    return receivers;
}

/* PUBLISH <channel> <message> */
void publishCommand(client *c) {
    if (server.sentinel_mode) {
        sentinelPublishCommand(c);
        return;
    }

    int receivers = pubsubPublishMessageAndPropagateToCluster(c->argv[1],c->argv[2],0);
    if (!server.cluster_enabled)
        forceCommandPropagation(c,PROPAGATE_REPL);
    addReplyLongLong(c,receivers);
}

/* PUBSUB command for Pub/Sub introspection. */
void pubsubCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"CHANNELS [<pattern>]",
"    Return the currently active channels matching a <pattern> (default: '*').",
"NUMPAT",
"    Return number of subscriptions to patterns.",
"NUMSUB [<channel> ...]",
"    Return the number of subscribers for the specified channels, excluding",
"    pattern subscriptions(default: no channels).",
"SHARDCHANNELS [<pattern>]",
"    Return the currently active shard level channels matching a <pattern> (default: '*').",
"SHARDNUMSUB [<shardchannel> ...]",
"    Return the number of subscribers for the specified shard level channel(s)",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"channels") &&
        (c->argc == 2 || c->argc == 3))
    {
        /* PUBSUB CHANNELS [<pattern>] */
        sds pat = (c->argc == 2) ? NULL : c->argv[2]->ptr;
        channelList(c, pat, &server.pubsub_channels, 0);
    } else if (!strcasecmp(c->argv[1]->ptr,"numsub") && c->argc >= 2) {
        /* PUBSUB NUMSUB [Channel_1 ... Channel_N] */
        int j;

        addReplyArrayLen(c,(c->argc-2)*2);
        for (j = 2; j < c->argc; j++) {
            dict *d = dictFetchValue(server.pubsub_channels, c->argv[j]);

            addReplyBulk(c,c->argv[j]);
            addReplyLongLong(c, d ? dictSize(d) : 0);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"numpat") && c->argc == 2) {
        /* PUBSUB NUMPAT */
        addReplyLongLong(c,dictSize(server.pubsub_patterns));
    } else if (!strcasecmp(c->argv[1]->ptr,"shardchannels") &&
        (c->argc == 2 || c->argc == 3)) 
    {
        /* PUBSUB SHARDCHANNELS */
        sds pat = (c->argc == 2) ? NULL : c->argv[2]->ptr;
        channelList(c,pat,server.pubsubshard_channels,server.cluster_enabled);
    } else if (!strcasecmp(c->argv[1]->ptr,"shardnumsub") && c->argc >= 2) {
        /* PUBSUB SHARDNUMSUB [ShardChannel_1 ... ShardChannel_N] */
        int j;
        addReplyArrayLen(c, (c->argc-2)*2);
        for (j = 2; j < c->argc; j++) {
            unsigned int slot = calculateKeySlot(c->argv[j]->ptr);
            dict *d = server.pubsubshard_channels[slot];
            dict *clients = d ? dictFetchValue(d, c->argv[j]) : NULL;

            addReplyBulk(c,c->argv[j]);
            addReplyLongLong(c, d ? dictSize(clients) : 0);
        }
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

void channelList(client *c, sds pat, dict **pubsub_channels, int is_sharded) {
    long mblen = 0;
    void *replylen;
    unsigned int slot_cnt = is_sharded ? CLUSTER_SLOTS : 1;

    replylen = addReplyDeferredLen(c);
    for (unsigned int i = 0; i < slot_cnt; i++) {
        if (pubsub_channels[i] == NULL) {
            continue;
        }
        dictIterator *di = dictGetIterator(pubsub_channels[i]);
        dictEntry *de;
        while((de = dictNext(di)) != NULL) {
            robj *cobj = dictGetKey(de);
            sds channel = cobj->ptr;

            if (!pat || stringmatchlen(pat, sdslen(pat),
                                    channel, sdslen(channel),0))
            {
                addReplyBulk(c,cobj);
                mblen++;
            }
        }
        dictReleaseIterator(di);
    }
    setDeferredArrayLen(c,replylen,mblen);
}

/* SPUBLISH <shardchannel> <message> */
void spublishCommand(client *c) {
    int receivers = pubsubPublishMessageAndPropagateToCluster(c->argv[1],c->argv[2],1);
    if (!server.cluster_enabled)
        forceCommandPropagation(c,PROPAGATE_REPL);
    addReplyLongLong(c,receivers);
}

/* SSUBSCRIBE shardchannel [shardchannel ...] */
void ssubscribeCommand(client *c) {
    if (c->flags & CLIENT_DENY_BLOCKING) {
        /* A client that has CLIENT_DENY_BLOCKING flag on
         * expect a reply per command and so can not execute subscribe. */
        addReplyError(c, "SSUBSCRIBE isn't allowed for a DENY BLOCKING client");
        return;
    }

    for (int j = 1; j < c->argc; j++) {
        pubsubSubscribeChannel(c, c->argv[j], pubSubShardType);
    }
    markClientAsPubSub(c);
}

/* SUNSUBSCRIBE [shardchannel [shardchannel ...]] */
void sunsubscribeCommand(client *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeShardAllChannels(c, 1);
    } else {
        for (int j = 1; j < c->argc; j++) {
            pubsubUnsubscribeChannel(c, c->argv[j], 1, pubSubShardType);
        }
    }
    if (clientTotalPubSubSubscriptionCount(c) == 0) {
        unmarkClientAsPubSub(c);
    }
}

size_t pubsubMemOverhead(client *c) {
    /* PubSub patterns */
    size_t mem = dictMemUsage(c->pubsub_patterns);
    /* Global PubSub channels */
    mem += dictMemUsage(c->pubsub_channels);
    /* Sharded PubSub channels */
    mem += dictMemUsage(c->pubsubshard_channels);
    return mem;
}

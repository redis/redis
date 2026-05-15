/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
 */

#include "server.h"
#include "cluster.h"
#include "cluster_slot_stats.h"

/* Structure to hold the pubsub related metadata. Currently used
 * for pubsub and pubsubshard feature. */
typedef struct pubsubtype {
    int shard;
    int (*subscriptionCount)(client*);
    kvstore **serverPubSubChannels;
    robj **subscribeMsg;
    robj **unsubscribeMsg;
    robj **messageBulk;
}pubsubtype;

void channelList(client *c, sds pat, kvstore *pubsub_channels);

/* --------------------------------------------------------------------------
 * Per-user subscription dict helpers
 * -------------------------------------------------------------------------- */

static void freePubsubUserSubs(dict *d, void *val) {
    UNUSED(d);
    pubsubUserSubs *subs = val;
    dictRelease(subs->channels);
    dictRelease(subs->patterns);
    dictRelease(subs->shard_channels);
    zfree(subs);
}

dictType pubsubSubscriptionsDictType = {
    dictSdsHash,
    NULL,
    NULL,
    dictSdsKeyCompare,
    dictSdsDestructor,
    freePubsubUserSubs,
    NULL
};

static pubsubUserSubs *createPubsubUserSubs(void) {
    pubsubUserSubs *subs = zmalloc(sizeof(*subs));
    subs->channels = dictCreate(&objectKeyPointerValueDictType);
    subs->patterns = dictCreate(&objectKeyPointerValueDictType);
    subs->shard_channels = dictCreate(&objectKeyPointerValueDictType);
    return subs;
}

pubsubUserSubs *pubsubGetOrCreateUserSubs(client *c, const char *username, size_t username_len) {
    dictEntry *de = dictFind(c->pubsub_subscriptions, username);
    if (de) return dictGetVal(de);
    pubsubUserSubs *subs = createPubsubUserSubs();
    sds key = sdsnewlen(username, username_len);
    dictAdd(c->pubsub_subscriptions, key, subs);
    return subs;
}

int pubsubUserSubsIsEmpty(pubsubUserSubs *subs) {
    return dictSize(subs->channels) == 0
        && dictSize(subs->patterns) == 0
        && dictSize(subs->shard_channels) == 0;
}

static dict *pubsubUserSubsGetDict(pubsubUserSubs *subs, pubsubtype type) {
    return type.shard ? subs->shard_channels : subs->channels;
}

static size_t *pubsubClientCountPtr(client *c, pubsubtype type) {
    return type.shard ? &c->pubsubshard_channels_count : &c->pubsub_channels_count;
}

pubsubtype pubSubType = {
    .shard = 0,
    .subscriptionCount = clientSubscriptionsCount,
    .serverPubSubChannels = &server.pubsub_channels,
    .subscribeMsg = &shared.subscribebulk,
    .unsubscribeMsg = &shared.unsubscribebulk,
    .messageBulk = &shared.messagebulk,
};

pubsubtype pubSubShardType = {
    .shard = 1,
    .subscriptionCount = clientShardSubscriptionsCount,
    .serverPubSubChannels = &server.pubsubshard_channels,
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
    return kvstoreSize(server.pubsub_channels) + dictSize(server.pubsub_patterns);
}

/* Return the number of pubsub shard level channels is handled. */
int serverPubsubShardSubscriptionCount(void) {
    return kvstoreSize(server.pubsubshard_channels);
}

/* Return the number of channels + patterns a client is subscribed to. */
int clientSubscriptionsCount(client *c) {
    return (int)(c->pubsub_channels_count + c->pubsub_patterns_count);
}

/* Return the number of shard level channels a client is subscribed to. */
int clientShardSubscriptionsCount(client *c) {
    return (int)c->pubsubshard_channels_count;
}

/* Return the number of pubsub + pubsub shard level channels
 * a client is subscribed to. */
int clientTotalPubSubSubscriptionCount(client *c) {
    return (int)(c->pubsub_channels_count + c->pubsub_patterns_count
               + c->pubsubshard_channels_count);
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

/* Check if a client is subscribed to a channel/shard-channel under any user. */
static int pubsubClientIsSubscribedChannel(client *c, robj *channel, pubsubtype type) {
    dictIterator di;
    dictEntry *userEntry;
    dictInitIterator(&di, c->pubsub_subscriptions);
    while ((userEntry = dictNext(&di)) != NULL) {
        pubsubUserSubs *subs = dictGetVal(userEntry);
        dict *innerDict = pubsubUserSubsGetDict(subs, type);
        if (dictFind(innerDict, channel)) {
            dictResetIterator(&di);
            return 1;
        }
    }
    dictResetIterator(&di);
    return 0;
}

/* Subscribe a client to a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was already subscribed to that channel. */
int pubsubSubscribeChannel(client *c, robj *channel, pubsubtype type) {
    dictEntry *de, *existing;
    dict *clients = NULL;
    int retval = 0;
    unsigned int slot = 0;

    /* Dedup: check if subscribed under any user */
    if (!pubsubClientIsSubscribedChannel(c, channel, type)) {
        retval = 1;

        /* Look up or create per-user entry for current user */
        pubsubUserSubs *subs = pubsubGetOrCreateUserSubs(c, c->user->name, sdslen(c->user->name));
        dict *innerDict = pubsubUserSubsGetDict(subs, type);

        /* Add the client to the channel -> list of clients hash table */
        if (server.cluster_enabled && type.shard) {
            slot = getKeySlot(channel->ptr);
        }

        de = kvstoreDictAddRaw(*type.serverPubSubChannels, slot, channel, &existing);

        if (existing) {
            clients = dictGetVal(existing);
            channel = dictGetKey(existing);
        } else {
            clients = dictCreate(&clientDictType);
            kvstoreDictSetVal(*type.serverPubSubChannels, slot, de, clients);
            incrRefCount(channel);
        }

        serverAssert(dictAdd(clients, c, NULL) != DICT_ERR);
        serverAssert(dictAdd(innerDict, channel, NULL) != DICT_ERR);
        incrRefCount(channel);

        (*pubsubClientCountPtr(c, type))++;
    }
    /* Notify the client */
    addReplyPubsubSubscribed(c,channel,type);
    return retval;
}

/* Remove a channel from a known inner dict + server-side reverse mapping.
 * Does NOT scan the outer dict or delete outer entries.
 * Caller is responsible for outer entry lifecycle. */
static void pubsubUnsubscribeKnownChannel(client *c, dict *innerDict,
                                          robj *channel, int notify, pubsubtype type) {
    dictEntry *de;
    dict *clients;
    int slot = 0;

    incrRefCount(channel);
    serverAssert(dictDelete(innerDict, channel) == DICT_OK);

    if (server.cluster_enabled && type.shard) {
        slot = keyHashSlot(channel->ptr, sdslen(channel->ptr));
    }
    de = kvstoreDictFind(*type.serverPubSubChannels, slot, channel);
    serverAssertWithInfo(c,NULL,de != NULL);
    clients = dictGetVal(de);
    serverAssertWithInfo(c, NULL, dictDelete(clients, c) == DICT_OK);
    if (dictSize(clients) == 0) {
        kvstoreDictDelete(*type.serverPubSubChannels, slot, channel);
    }

    serverAssert(*pubsubClientCountPtr(c, type) > 0);
    (*pubsubClientCountPtr(c, type))--;

    if (notify) {
        addReplyPubsubUnsubscribed(c,channel,type);
    }
    decrRefCount(channel);
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
int pubsubUnsubscribeChannel(client *c, robj *channel, int notify, pubsubtype type) {
    int retval = 0;

    incrRefCount(channel);

    /* Scan per-user entries to find which one holds this channel */
    dictIterator outer;
    dictEntry *userEntry;
    dictInitSafeIterator(&outer, c->pubsub_subscriptions);
    while ((userEntry = dictNext(&outer)) != NULL) {
        pubsubUserSubs *subs = dictGetVal(userEntry);
        dict *innerDict = pubsubUserSubsGetDict(subs, type);
        if (dictFind(innerDict, channel)) {
            pubsubUnsubscribeKnownChannel(c, innerDict, channel, notify, type);
            if (pubsubUserSubsIsEmpty(subs)) {
                dictDelete(c->pubsub_subscriptions, dictGetKey(userEntry));
            }
            retval = 1;
            break;
        }
    }
    dictResetIterator(&outer);

    if (!retval && notify) {
        addReplyPubsubUnsubscribed(c,channel,type);
    }
    decrRefCount(channel);
    return retval;
}

/* Unsubscribe all shard channels in a slot. */
void pubsubShardUnsubscribeAllChannelsInSlot(unsigned int slot) {
    if (!kvstoreDictSize(server.pubsubshard_channels, slot))
        return;

    dictEntry *de;
    kvstoreDictIterator kvs_di;
    kvstoreInitDictSafeIterator(&kvs_di, server.pubsubshard_channels, slot);
    while ((de = kvstoreDictIteratorNext(&kvs_di)) != NULL) {
        robj *channel = dictGetKey(de);
        dict *clients = dictGetVal(de);
        dictIterator iter;
        dictEntry *entry;

        dictInitIterator(&iter, clients);
        while ((entry = dictNext(&iter)) != NULL) {
            client *c = dictGetKey(entry);
            /* Find and remove from the per-user entry that holds it. */
            dictIterator di;
            dictEntry *userEntry;
            dictInitSafeIterator(&di, c->pubsub_subscriptions);
            while ((userEntry = dictNext(&di)) != NULL) {
                pubsubUserSubs *subs = dictGetVal(userEntry);
                if (dictDelete(subs->shard_channels, channel) == DICT_OK) {
                    serverAssert(c->pubsubshard_channels_count > 0);
                    c->pubsubshard_channels_count--;
                    if (pubsubUserSubsIsEmpty(subs)) {
                        dictDelete(c->pubsub_subscriptions, dictGetKey(userEntry));
                    }
                    break;
                }
            }
            dictResetIterator(&di);
            addReplyPubsubUnsubscribed(c, channel, pubSubShardType);
            if (clientTotalPubSubSubscriptionCount(c) == 0) {
                unmarkClientAsPubSub(c);
            }
        }
        dictResetIterator(&iter);
        kvstoreDictDelete(server.pubsubshard_channels, slot, channel);
    }
    kvstoreResetDictIterator(&kvs_di);
}

/* Check if a client is subscribed to a pattern under any user. */
static int pubsubClientIsSubscribedPattern(client *c, robj *pattern) {
    dictIterator di;
    dictEntry *userEntry;
    dictInitIterator(&di, c->pubsub_subscriptions);
    while ((userEntry = dictNext(&di)) != NULL) {
        pubsubUserSubs *subs = dictGetVal(userEntry);
        if (dictFind(subs->patterns, pattern)) {
            dictResetIterator(&di);
            return 1;
        }
    }
    dictResetIterator(&di);
    return 0;
}

/* Subscribe a client to a pattern. Returns 1 if the operation succeeded, or 0 if the client was already subscribed to that pattern. */
int pubsubSubscribePattern(client *c, robj *pattern) {
    dictEntry *de;
    dict *clients;
    int retval = 0;

    if (!pubsubClientIsSubscribedPattern(c, pattern)) {
        retval = 1;
        pubsubUserSubs *subs = pubsubGetOrCreateUserSubs(c, c->user->name, sdslen(c->user->name));

        serverAssert(dictAdd(subs->patterns, pattern, NULL) != DICT_ERR);
        incrRefCount(pattern);

        de = dictFind(server.pubsub_patterns,pattern);
        if (de == NULL) {
            clients = dictCreate(&clientDictType);
            dictAdd(server.pubsub_patterns,pattern,clients);
            incrRefCount(pattern);
        } else {
            clients = dictGetVal(de);
        }
        serverAssert(dictAdd(clients, c, NULL) != DICT_ERR);
        c->pubsub_patterns_count++;
    }
    /* Notify the client */
    addReplyPubsubPatSubscribed(c,pattern);
    return retval;
}

/* Helper: unsubscribe a pattern from a known inner dict + server-side mapping.
 * Does NOT scan the outer dict or delete outer entries. */
static void pubsubUnsubscribeKnownPattern(client *c, dict *innerDict,
                                          robj *pattern, int notify) {
    dictEntry *de;
    dict *clients;

    incrRefCount(pattern);
    serverAssert(dictDelete(innerDict, pattern) == DICT_OK);

    de = dictFind(server.pubsub_patterns,pattern);
    serverAssertWithInfo(c,NULL,de != NULL);
    clients = dictGetVal(de);
    serverAssertWithInfo(c, NULL, dictDelete(clients, c) == DICT_OK);
    if (dictSize(clients) == 0) {
        dictDelete(server.pubsub_patterns,pattern);
    }

    serverAssert(c->pubsub_patterns_count > 0);
    c->pubsub_patterns_count--;

    if (notify) addReplyPubsubPatUnsubscribed(c,pattern);
    decrRefCount(pattern);
}

/* Unsubscribe a client from a pattern. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified pattern. */
int pubsubUnsubscribePattern(client *c, robj *pattern, int notify) {
    int retval = 0;

    incrRefCount(pattern);

    dictIterator outer;
    dictEntry *userEntry;
    dictInitSafeIterator(&outer, c->pubsub_subscriptions);
    while ((userEntry = dictNext(&outer)) != NULL) {
        pubsubUserSubs *subs = dictGetVal(userEntry);
        if (dictFind(subs->patterns, pattern)) {
            pubsubUnsubscribeKnownPattern(c, subs->patterns, pattern, notify);
            if (pubsubUserSubsIsEmpty(subs)) {
                dictDelete(c->pubsub_subscriptions, dictGetKey(userEntry));
            }
            retval = 1;
            break;
        }
    }
    dictResetIterator(&outer);

    if (!retval && notify) addReplyPubsubPatUnsubscribed(c,pattern);
    decrRefCount(pattern);
    return retval;
}

/* Unsubscribe from all the channels of a given type. Return the number of
 * channels the client was subscribed to. */
int pubsubUnsubscribeAllChannelsInternal(client *c, int notify, pubsubtype type) {
    int count = 0;
    dictIterator outer;
    dictEntry *userEntry;
    dictInitSafeIterator(&outer, c->pubsub_subscriptions);
    while ((userEntry = dictNext(&outer)) != NULL) {
        pubsubUserSubs *subs = dictGetVal(userEntry);
        dict *innerDict = pubsubUserSubsGetDict(subs, type);
        if (dictSize(innerDict) > 0) {
            dictIterator inner;
            dictEntry *de;
            dictInitSafeIterator(&inner, innerDict);
            while ((de = dictNext(&inner)) != NULL) {
                robj *channel = dictGetKey(de);
                pubsubUnsubscribeKnownChannel(c, innerDict, channel, notify, type);
                count++;
            }
            dictResetIterator(&inner);
        }
        if (pubsubUserSubsIsEmpty(subs)) {
            dictDelete(c->pubsub_subscriptions, dictGetKey(userEntry));
        }
    }
    dictResetIterator(&outer);
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

    dictIterator outer;
    dictEntry *userEntry;
    dictInitSafeIterator(&outer, c->pubsub_subscriptions);
    while ((userEntry = dictNext(&outer)) != NULL) {
        pubsubUserSubs *subs = dictGetVal(userEntry);
        if (dictSize(subs->patterns) > 0) {
            dictIterator inner;
            dictEntry *de;
            dictInitSafeIterator(&inner, subs->patterns);
            while ((de = dictNext(&inner)) != NULL) {
                robj *pattern = dictGetKey(de);
                pubsubUnsubscribeKnownPattern(c, subs->patterns, pattern, notify);
                count++;
            }
            dictResetIterator(&inner);
        }
        if (pubsubUserSubsIsEmpty(subs)) {
            dictDelete(c->pubsub_subscriptions, dictGetKey(userEntry));
        }
    }
    dictResetIterator(&outer);

    if (notify && count == 0) addReplyPubsubPatUnsubscribed(c,NULL);
    return count;
}

/*
 * Publish a message to all the subscribers.
 */
int pubsubPublishMessageInternal(robj *channel, robj *message, pubsubtype type) {
    int receivers = 0;
    dictEntry *de;
    dictIterator di;
    unsigned int slot = 0;

    /* Send to clients listening for that channel */
    if (server.cluster_enabled && type.shard) {
        slot = keyHashSlot(channel->ptr, sdslen(channel->ptr));
    }
    de = kvstoreDictFind(*type.serverPubSubChannels, slot, channel);
    if (de) {
        dict *clients = dictGetVal(de);
        dictEntry *entry;
        dictIterator iter;

        dictInitIterator(&iter, clients);
        while ((entry = dictNext(&iter)) != NULL) {
            client *c = dictGetKey(entry);
            addReplyPubsubMessage(c,channel,message,*type.messageBulk);
            if (clusterSlotStatsEnabled(CLUSTER_SLOT_STATS_NET))
                clusterSlotStatsAddNetworkBytesOutForShardedPubSubInternalPropagation(c, slot);
            updateClientMemUsageAndBucket(c);
            receivers++;
        }
        dictResetIterator(&iter);
    }

    if (type.shard) {
        /* Shard pubsub ignores patterns. */
        return receivers;
    }

    /* Send to clients listening to matching channels */
    if (dictSize(server.pubsub_patterns) > 0) {
        channel = getDecodedObject(channel);
        dictInitIterator(&di, server.pubsub_patterns);
        while((de = dictNext(&di)) != NULL) {
            robj *pattern = dictGetKey(de);
            dict *clients = dictGetVal(de);
            if (!stringmatchlen((char*)pattern->ptr,
                                sdslen(pattern->ptr),
                                (char*)channel->ptr,
                                sdslen(channel->ptr),0)) continue;

            dictEntry *entry;
            dictIterator iter;

            dictInitIterator(&iter, clients);
            while ((entry = dictNext(&iter)) != NULL) {
                client *c = dictGetKey(entry);
                addReplyPubsubPatMessage(c,pattern,channel,message);
                updateClientMemUsageAndBucket(c);
                receivers++;
            }
            dictResetIterator(&iter);
        }
        decrRefCount(channel);
        dictResetIterator(&di);
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
        channelList(c, pat, server.pubsub_channels);
    } else if (!strcasecmp(c->argv[1]->ptr,"numsub") && c->argc >= 2) {
        /* PUBSUB NUMSUB [Channel_1 ... Channel_N] */
        int j;

        addReplyArrayLen(c,(c->argc-2)*2);
        for (j = 2; j < c->argc; j++) {
            dict *d = kvstoreDictFetchValue(server.pubsub_channels, 0, c->argv[j]);

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
        channelList(c,pat,server.pubsubshard_channels);
    } else if (!strcasecmp(c->argv[1]->ptr,"shardnumsub") && c->argc >= 2) {
        /* PUBSUB SHARDNUMSUB [ShardChannel_1 ... ShardChannel_N] */
        int j;
        addReplyArrayLen(c, (c->argc-2)*2);
        for (j = 2; j < c->argc; j++) {
            unsigned int slot = calculateKeySlot(c->argv[j]->ptr);
            dict *clients = kvstoreDictFetchValue(server.pubsubshard_channels, slot, c->argv[j]);

            addReplyBulk(c,c->argv[j]);
            addReplyLongLong(c, clients ? dictSize(clients) : 0);
        }
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

void channelList(client *c, sds pat, kvstore *pubsub_channels) {
    long mblen = 0;
    void *replylen;
    unsigned int slot_cnt = kvstoreNumDicts(pubsub_channels);

    replylen = addReplyDeferredLen(c);
    for (unsigned int i = 0; i < slot_cnt; i++) {
        if (!kvstoreDictSize(pubsub_channels, i))
            continue;
        dictEntry *de;
        kvstoreDictIterator kvs_di;
        kvstoreInitDictIterator(&kvs_di, pubsub_channels, i);
        while((de = kvstoreDictIteratorNext(&kvs_di)) != NULL) {
            robj *cobj = dictGetKey(de);
            sds channel = cobj->ptr;

            if (!pat || stringmatchlen(pat, sdslen(pat),
                                    channel, sdslen(channel),0))
            {
                addReplyBulk(c,cobj);
                mblen++;
            }
        }
        kvstoreResetDictIterator(&kvs_di);
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
    size_t mem = dictMemUsage(c->pubsub_subscriptions);
    dictIterator di;
    dictEntry *de;
    dictInitIterator(&di, c->pubsub_subscriptions);
    while ((de = dictNext(&di)) != NULL) {
        pubsubUserSubs *subs = dictGetVal(de);
        mem += sizeof(*subs);
        mem += dictMemUsage(subs->channels);
        mem += dictMemUsage(subs->patterns);
        mem += dictMemUsage(subs->shard_channels);
    }
    dictResetIterator(&di);
    return mem;
}

int pubsubTotalSubscriptions(void) {
    return dictSize(server.pubsub_patterns) +
           kvstoreSize(server.pubsub_channels) +
           kvstoreSize(server.pubsubshard_channels);
}

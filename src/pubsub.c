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

/*
 * Get client's local Pub/Sub channels subscription count.
 */
int clientSubscriptionsCount(client *c);

/*
 * Get client's global Pub/Sub channels subscription count.
 */
int clientLocalSubscriptionsCount(client *c);

/*
 * Get client's global Pub/Sub channels dict.
 */
dict* getClientPubSubChannels(client *c);

/*
 * Get client's local Pub/Sub channels dict.
 */
dict* getClientPubSubLocalChannels(client *c);

/*
 * Get client's global Pub/Sub channels dict.
 */
dict* getServerPubSubChannels();

/*
 * Get client's response on global Pub/Sub channel subscription.
 */
robj* getSubscribeMsgGlobal();

/*
 * Get client's response on global Pub/Sub channel subscription.
 */
robj* getSubscribeMsgLocal();

/*
 * Get client's response on global Pub/Sub channel unsubscription.
 */
robj* getUnsubscribeMsgGlobal();

/*
 * Get client's response on global Pub/Sub channel unsubscription.
 */
robj* getUnsubscribeMsgLocal();



/*
 * Get client's local Pub/Sub channels dict.
 */
dict* getServerPubSubLocalChannels();

void channelList(client *c, sds pat, dict* pubsub_channels);

/*
 * Pub/Sub meta for global channels.
 */
pubsubmeta pubSubMeta = {
    .local = 0,
    .serverPubSubChannels = getServerPubSubChannels,
    .clientPubSubChannels = getClientPubSubChannels,
    .subscriptionCount = clientSubscriptionsCount,
    .subscribeMsg = getSubscribeMsgGlobal,
    .unsubscribeMsg = getUnsubscribeMsgGlobal
};

/*
 * Pub/Sub meta for local channels bounded to a slot.
 */
pubsubmeta pubSubLocalMeta = {
    .local = 1,
    .serverPubSubChannels = getServerPubSubLocalChannels,
    .clientPubSubChannels = getClientPubSubLocalChannels,
    .subscriptionCount = clientLocalSubscriptionsCount,
    .subscribeMsg = getSubscribeMsgLocal,
    .unsubscribeMsg = getUnsubscribeMsgLocal
};

/*-----------------------------------------------------------------------------
 * Pubsub client replies API
 *----------------------------------------------------------------------------*/

/* Send a pubsub message of type "message" to the client.
 * Normally 'msg' is a Redis object containing the string to send as
 * message. However if the caller sets 'msg' as NULL, it will be able
 * to send a special message (for instance an Array type) by using the
 * addReply*() API family. */
void addReplyPubsubMessage(client *c, robj *channel, robj *msg) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,shared.messagebulk);
    addReplyBulk(c,channel);
    if (msg) addReplyBulk(c,msg);
}

/* Send a pubsub message of type "pmessage" to the client. The difference
 * with the "message" type delivered by addReplyPubsubMessage() is that
 * this message format also includes the pattern that matched the message. */
void addReplyPubsubPatMessage(client *c, robj *pat, robj *channel, robj *msg) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[4]);
    else
        addReplyPushLen(c,4);
    addReply(c,shared.pmessagebulk);
    addReplyBulk(c,pat);
    addReplyBulk(c,channel);
    addReplyBulk(c,msg);
}

/* Send the pubsub subscription notification to the client. */
void addReplyPubsubSubscribed(client *c, robj *channel, pubsubmeta meta) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,meta.subscribeMsg());
    addReplyBulk(c,channel);
    addReplyLongLong(c,meta.subscriptionCount(c));
}

/* Send the pubsub unsubscription notification to the client.
 * Channel can be NULL: this is useful when the client sends a mass
 * unsubscribe command but there are no channels to unsubscribe from: we
 * still send a notification. */
void addReplyPubsubUnsubscribed(client *c, robj *channel, pubsubmeta meta) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,meta.unsubscribeMsg());
    if (channel)
        addReplyBulk(c,channel);
    else
        addReplyNull(c);
    addReplyLongLong(c,meta.subscriptionCount(c));
}

/* Send the pubsub pattern subscription notification to the client. */
void addReplyPubsubPatSubscribed(client *c, robj *pattern) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,shared.psubscribebulk);
    addReplyBulk(c,pattern);
    addReplyLongLong(c,clientSubscriptionsCount(c));
}

/* Send the pubsub pattern unsubscription notification to the client.
 * Pattern can be NULL: this is useful when the client sends a mass
 * punsubscribe command but there are no pattern to unsubscribe from: we
 * still send a notification. */
void addReplyPubsubPatUnsubscribed(client *c, robj *pattern) {
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
}

/*-----------------------------------------------------------------------------
 * Pubsub low level API
 *----------------------------------------------------------------------------*/

/* Return the number of channels + patterns a client is subscribed to. */
int clientSubscriptionsCount(client *c) {
    return dictSize(c->pubsub_channels)+
           listLength(c->pubsub_patterns);
}

/* Return the number of local channels a client is subscribed to. */
int clientLocalSubscriptionsCount(client *c) {
    return dictSize(c->pubsublocal_channels);
}

dict* getClientPubSubChannels(client *c) {
    return c->pubsub_channels;
}

dict* getClientPubSubLocalChannels(client *c) {
    return c->pubsublocal_channels;
}

dict* getServerPubSubChannels() {
    return server.pubsub_channels;
}

dict* getServerPubSubLocalChannels() {
    return server.pubsublocal_channels;
}

robj* getSubscribeMsgGlobal() {
    return shared.subscribebulk;
}

robj* getSubscribeMsgLocal() {
    return shared.subscribelocalbulk;
}

robj* getUnsubscribeMsgGlobal() {
    return shared.unsubscribebulk;
}

robj* getUnsubscribeMsgLocal() {
    return shared.unsubscribelocalbulk;
}

/* Return the number of pubsub + pubsub local channels
 * a client is subscribed to. */
int clientTotalPubSubSubscriptionCount(client *c) {
    return clientSubscriptionsCount(c) + clientLocalSubscriptionsCount(c);
}

/* Subscribe a client to a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was already subscribed to that channel. */
int pubsubSubscribeChannel(client *c, robj *channel, pubsubmeta meta) {
    dictEntry *de;
    list *clients = NULL;
    int retval = 0;

    /* Add the channel to the client -> channels hash table */
    if (dictAdd(meta.clientPubSubChannels(c),channel,NULL) == DICT_OK) {
        retval = 1;
        incrRefCount(channel);
        /* Add the client to the channel -> list of clients hash table */
        de = dictFind(meta.serverPubSubChannels(), channel);
        if (de == NULL) {
            clients = listCreate();
            dictAdd(meta.serverPubSubChannels(), channel, clients);
            incrRefCount(channel);
        } else {
            clients = dictGetVal(de);
        }
        listAddNodeTail(clients,c);
    }
    /* Notify the client */
    addReplyPubsubSubscribed(c,channel,meta);
    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
int pubsubUnsubscribeChannel(client *c, robj *channel, int notify, pubsubmeta meta) {
    dictEntry *de;
    list *clients;
    listNode *ln;
    int retval = 0;

    /* Remove the channel from the client -> channels hash table */
    incrRefCount(channel); /* channel may be just a pointer to the same object
                            we have in the hash tables. Protect it... */
    if (dictDelete(meta.clientPubSubChannels(c),channel) == DICT_OK) {
        retval = 1;
        /* Remove the client from the channel -> clients list hash table */
        de = dictFind(meta.serverPubSubChannels(), channel);
        serverAssertWithInfo(c,NULL,de != NULL);
        clients = dictGetVal(de);
        ln = listSearchKey(clients,c);
        serverAssertWithInfo(c,NULL,ln != NULL);
        listDelNode(clients,ln);
        if (listLength(clients) == 0) {
            /* Free the list and associated hash entry at all if this was
             * the latest client, so that it will be possible to abuse
             * Redis PUBSUB creating millions of channels. */
            dictDelete(meta.serverPubSubChannels(), channel);
            /* As this channel isn't subscribed by anyone, it's safe
             * to remove the channel from the slot. */
            if (server.cluster_enabled & meta.local) {
                slotToChannelDel(channel->ptr);
            }
        }
    }
    /* Notify the client */
    if (notify) {
        addReplyPubsubUnsubscribed(c,channel,meta);
    }
    decrRefCount(channel); /* it is finally safe to release it */
    return retval;
}

void pubsuLocalbUnsubscribeAllClients(robj *channel, int notify) {
    int retval;
    dictEntry *de = dictFind(server.pubsublocal_channels,channel);
    serverAssertWithInfo(NULL,channel,de != NULL);
    list *clients = dictGetVal(de);
    if (listLength(clients) > 0) {
        /* For each client subscribed to the channel, unsubscribe it. */
        listIter li;
        listNode *ln;
        listRewind(clients, &li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = listNodeValue(ln);
            retval = dictDelete(c->pubsublocal_channels,channel);
            serverAssertWithInfo(c,channel,retval == DICT_OK);
            if (notify) {
                addReplyPubsubUnsubscribed(c,channel,pubSubLocalMeta);
            }
            /* If the client has no other pubsub subscription,
             * move out of pubsub mode. */
            if (clientTotalPubSubSubscriptionCount(c) == 0) {
                c->flags &= ~CLIENT_PUBSUB;
            }
        }
    }
    /* Delete the channel from server pubsublocal channels hash table. */
    retval = dictDelete(server.pubsublocal_channels,channel);
    /* Delete the channel from slots_to_channel mapping. */
    slotToChannelDel(channel->ptr);
    serverAssertWithInfo(NULL,channel,retval == DICT_OK);
    decrRefCount(channel); /* it is finally safe to release it */
}


/* Subscribe a client to a pattern. Returns 1 if the operation succeeded, or 0 if the client was already subscribed to that pattern. */
int pubsubSubscribePattern(client *c, robj *pattern) {
    dictEntry *de;
    list *clients;
    int retval = 0;

    if (listSearchKey(c->pubsub_patterns,pattern) == NULL) {
        retval = 1;
        listAddNodeTail(c->pubsub_patterns,pattern);
        incrRefCount(pattern);
        /* Add the client to the pattern -> list of clients hash table */
        de = dictFind(server.pubsub_patterns,pattern);
        if (de == NULL) {
            clients = listCreate();
            dictAdd(server.pubsub_patterns,pattern,clients);
            incrRefCount(pattern);
        } else {
            clients = dictGetVal(de);
        }
        listAddNodeTail(clients,c);
    }
    /* Notify the client */
    addReplyPubsubPatSubscribed(c,pattern);
    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
int pubsubUnsubscribePattern(client *c, robj *pattern, int notify) {
    dictEntry *de;
    list *clients;
    listNode *ln;
    int retval = 0;

    incrRefCount(pattern); /* Protect the object. May be the same we remove */
    if ((ln = listSearchKey(c->pubsub_patterns,pattern)) != NULL) {
        retval = 1;
        listDelNode(c->pubsub_patterns,ln);
        /* Remove the client from the pattern -> clients list hash table */
        de = dictFind(server.pubsub_patterns,pattern);
        serverAssertWithInfo(c,NULL,de != NULL);
        clients = dictGetVal(de);
        ln = listSearchKey(clients,c);
        serverAssertWithInfo(c,NULL,ln != NULL);
        listDelNode(clients,ln);
        if (listLength(clients) == 0) {
            /* Free the list and associated hash entry at all if this was
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
int pubsubUnsubscribeAllChannelsInternal(client *c, int notify, pubsubmeta meta) {
    int count = 0;
    if (dictSize(meta.clientPubSubChannels(c)) > 0) {
        dictIterator *di = dictGetSafeIterator(meta.clientPubSubChannels(c));
        dictEntry *de;

        while((de = dictNext(di)) != NULL) {
            robj *channel = dictGetKey(de);

            count += pubsubUnsubscribeChannel(c,channel,notify,meta);
        }
        dictReleaseIterator(di);
    }
    /* We were subscribed to nothing? Still reply to the client. */
    if (notify && count == 0) {
        addReplyPubsubUnsubscribed(c,NULL,meta);
    }
    return count;
}

int pubsubUnsubscribeAllChannels(client *c, int notify) {
    int count = pubsubUnsubscribeAllChannelsInternal(c, notify, pubSubMeta);
    return count;
}

int pubsubUnsubscribeLocalAllChannels(client *c, int notify) {
    int count = pubsubUnsubscribeAllChannelsInternal(c, notify, pubSubLocalMeta);
    return count;
}

void pubsubUnsubscribeLocalAllChannelsInSlot(robj **channels, unsigned int count) {
    for (unsigned int j = 0; j < count; j++) {
        /* Remove the channel from server and from the clients
         * subscribed to it as well as notify them. */
        pubsuLocalbUnsubscribeAllClients(channels[j], 1);
    }
}

/* Unsubscribe from all the patterns. Return the number of patterns the
 * client was subscribed from. */
int pubsubUnsubscribeAllPatterns(client *c, int notify) {
    listNode *ln;
    listIter li;
    int count = 0;

    listRewind(c->pubsub_patterns,&li);
    while ((ln = listNext(&li)) != NULL) {
        robj *pattern = ln->value;

        count += pubsubUnsubscribePattern(c,pattern,notify);
    }
    if (notify && count == 0) addReplyPubsubPatUnsubscribed(c,NULL);
    return count;
}

int pubsubPublishMessageInternal(robj *channel, robj *message, pubsubmeta meta) {
    int receivers = 0;
    dictEntry *de;
    dictIterator *di;
    listNode *ln;
    listIter li;

    /* Send to clients listening for that channel */
    de = dictFind(meta.serverPubSubChannels(),channel);
    if (de) {
        list *list = dictGetVal(de);
        listNode *ln;
        listIter li;

        listRewind(list,&li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = ln->value;
            addReplyPubsubMessage(c,channel,message);
            receivers++;
        }
    }

    if (!meta.local) {
        /* Send to clients listening to matching channels */
        di = dictGetIterator(server.pubsub_patterns);
        if (di) {
            channel = getDecodedObject(channel);
            while((de = dictNext(di)) != NULL) {
                robj *pattern = dictGetKey(de);
                list *clients = dictGetVal(de);
                if (!stringmatchlen((char*)pattern->ptr,
                                    sdslen(pattern->ptr),
                                    (char*)channel->ptr,
                                    sdslen(channel->ptr),0)) continue;

                listRewind(clients,&li);
                while ((ln = listNext(&li)) != NULL) {
                    client *c = listNodeValue(ln);
                    addReplyPubsubPatMessage(c,pattern,channel,message);
                    receivers++;
                }
            }
            decrRefCount(channel);
            dictReleaseIterator(di);
        }
    }
    return receivers;
}

/* Publish a message */
int pubsubPublishMessage(robj *channel, robj *message) {
    return pubsubPublishMessageInternal(channel,message,pubSubMeta);
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
         * backword compatibility
         */
        addReplyError(c, "SUBSCRIBE isn't allowed for a DENY BLOCKING client");
        return;
    }
    for (j = 1; j < c->argc; j++)
        pubsubSubscribeChannel(c,c->argv[j],pubSubMeta);
    c->flags |= CLIENT_PUBSUB;
}

/* UNSUBSCRIBE [channel [channel ...]] */
void unsubscribeCommand(client *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeAllChannels(c,1);
    } else {
        int j;

        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribeChannel(c,c->argv[j],1,pubSubMeta);
    }
    if (clientTotalPubSubSubscriptionCount(c) == 0) c->flags &= ~CLIENT_PUBSUB;
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
         * backword compatibility
         */
        addReplyError(c, "PSUBSCRIBE isn't allowed for a DENY BLOCKING client");
        return;
    }

    for (j = 1; j < c->argc; j++)
        pubsubSubscribePattern(c,c->argv[j]);
    c->flags |= CLIENT_PUBSUB;
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
    if (clientTotalPubSubSubscriptionCount(c) == 0) c->flags &= ~CLIENT_PUBSUB;
}

/* PUBLISH <channel> <message> */
void publishCommand(client *c) {
    int receivers = pubsubPublishMessage(c->argv[1],c->argv[2]);
    if (server.cluster_enabled)
        clusterPropagatePublish(c->argv[1],c->argv[2]);
    else
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
            list *l = dictFetchValue(server.pubsub_channels,c->argv[j]);

            addReplyBulk(c,c->argv[j]);
            addReplyLongLong(c,l ? listLength(l) : 0);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"numpat") && c->argc == 2) {
        /* PUBSUB NUMPAT */
        addReplyLongLong(c,dictSize(server.pubsub_patterns));
    } else if (!strcasecmp(c->argv[1]->ptr,"local") && c->argc >= 2) {
        /* PUBSUB LOCAL CHANNELS [PATTERN] */
        if (!strcasecmp(c->argv[2]->ptr,"channels")) {
            sds pat = (c->argc == 3) ? NULL : c->argv[3]->ptr;
            channelList(c, pat, server.pubsublocal_channels);

        } else if (!strcasecmp(c->argv[2]->ptr,"numsub")) {
            /* PUBSUB LOCAL NUMSUB [Channel_1 ... Channel_N] */
            int j;

            addReplyArrayLen(c, (c->argc - 3) * 2);
            for (j = 3; j < c->argc; j++) {
                list *l = dictFetchValue(server.pubsublocal_channels, c->argv[j]);

                addReplyBulk(c, c->argv[j]);
                addReplyLongLong(c, l ? listLength(l) : 0);
            }
        } else {
            addReplySubcommandSyntaxError(c);
        }
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

void channelList(client *c, sds pat, dict *pubsub_channels) {
    dictIterator *di = dictGetIterator(pubsub_channels);
    dictEntry *de;
    long mblen = 0;
    void *replylen;

    replylen = addReplyDeferredLen(c);
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
    setDeferredArrayLen(c,replylen,mblen);
}

/* PUBLISHLOCAL <channel> <message> */
void publishLocalCommand(client *c) {
    int receivers = pubsubPublishMessageInternal(c->argv[1],c->argv[2],pubSubLocalMeta);
    forceCommandPropagation(c,PROPAGATE_REPL);
    addReplyLongLong(c,receivers);
}

/* SUBSCRIBELOCAL channel [channel ...] */
void subscribeLocalCommand(client *c) {
    if (c->flags & CLIENT_DENY_BLOCKING) {
        /**
         * A client that has CLIENT_DENY_BLOCKING flag on
         * expect a reply per command and so can not execute subscribe.
         *
         */
        addReplyError(c, "SUBSCRIBELOCAL isn't allowed for a DENY BLOCKING client");
        return;
    }

    for (int j = 1; j < c->argc; j++) {
        /* A channel is only considered to be added, if a
         * subscriber exists for it. And if a subscriber
         * already exists the slotToChannel doesn't needs
         * to be incremented. */
        if (server.cluster_enabled &
            (dictFind(pubSubLocalMeta.serverPubSubChannels(), c->argv[j]) == NULL)) {
            slotToChannelAdd(c->argv[j]->ptr);
        }
        pubsubSubscribeChannel(c,c->argv[j],pubSubLocalMeta);
    }
    c->flags |= CLIENT_PUBSUB;
}


/* UNSUBSCRIBELOCAL [channel [channel ...]] */
void unsubscribeLocalCommand(client *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeLocalAllChannels(c,1);
    } else {
        for (int j = 1; j < c->argc; j++) {
            pubsubUnsubscribeChannel(c,c->argv[j],1,pubSubLocalMeta);
        }
    }
    if (clientTotalPubSubSubscriptionCount(c) == 0) c->flags &= ~CLIENT_PUBSUB;
}

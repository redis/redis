/* tracking.c - Client side caching: keys tracking and invalidation
 *
 * Copyright (c) 2019, Salvatore Sanfilippo <antirez at gmail dot com>
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

/* The tracking table is constituted by a radix tree of keys, each pointing
 * to a radix tree of client IDs, used to track the clients that may have
 * certain keys in their local, client side, cache.
 *
 * When a client enables tracking with "CLIENT TRACKING on", each key served to
 * the client is remembered in the table mapping the keys to the client IDs.
 * Later, when a key is modified, all the clients that may have local copy
 * of such key will receive an invalidation message.
 *
 * Clients will normally take frequently requested objects in memory, removing
 * them when invalidation messages are received. */
rax *TrackingTable = NULL;
rax *PrefixTable = NULL;
uint64_t TrackingTableTotalItems = 0; /* Total number of IDs stored across
                                         the whole tracking table. This gives
                                         an hint about the total memory we
                                         are using server side for CSC. */
robj *TrackingChannelName;

/* This is the structure that we have as value of the PrefixTable, and
 * represents the list of keys modified, and the list of clients that need
 * to be notified, for a given prefix. */
typedef struct bcastState {
    rax *keys;      /* Keys modified in the current event loop cycle. */
    rax *clients;   /* Clients subscribed to the notification events for this
                       prefix. */
} bcastState;

/* Remove the tracking state from the client 'c'. Note that there is not much
 * to do for us here, if not to decrement the counter of the clients in
 * tracking mode, because we just store the ID of the client in the tracking
 * table, so we'll remove the ID reference in a lazy way. Otherwise when a
 * client with many entries in the table is removed, it would cost a lot of
 * time to do the cleanup. */
void disableTracking(client *c) {
    /* If this client is in broadcasting mode, we need to unsubscribe it
     * from all the prefixes it is registered to. */
    if (c->flags & CLIENT_TRACKING_BCAST) {
        raxIterator ri;
        raxStart(&ri,c->client_tracking_prefixes);
        raxSeek(&ri,"^",NULL,0);
        while(raxNext(&ri)) {
            bcastState *bs = raxFind(PrefixTable,ri.key,ri.key_len);
            serverAssert(bs != raxNotFound);
            raxRemove(bs->clients,(unsigned char*)&c,sizeof(c),NULL);
            /* Was it the last client? Remove the prefix from the
             * table. */
            if (raxSize(bs->clients) == 0) {
                raxFree(bs->clients);
                raxFree(bs->keys);
                zfree(bs);
                raxRemove(PrefixTable,ri.key,ri.key_len,NULL);
            }
        }
        raxStop(&ri);
        raxFree(c->client_tracking_prefixes);
        c->client_tracking_prefixes = NULL;
    }

    /* Clear flags and adjust the count. */
    if (c->flags & CLIENT_TRACKING) {
        server.tracking_clients--;
        c->flags &= ~(CLIENT_TRACKING|CLIENT_TRACKING_BROKEN_REDIR|
                      CLIENT_TRACKING_BCAST|CLIENT_TRACKING_OPTIN|
                      CLIENT_TRACKING_OPTOUT|CLIENT_TRACKING_CACHING|
                      CLIENT_TRACKING_NOLOOP);
    }
}

static int stringCheckPrefix(unsigned char *s1, size_t s1_len, unsigned char *s2, size_t s2_len) {
    size_t min_length = s1_len < s2_len ? s1_len : s2_len;
    return memcmp(s1,s2,min_length) == 0;   
}

/* Check if any of the provided prefixes collide with one another or
 * with an existing prefix for the client. A collision is defined as two 
 * prefixes that will emit an invalidation for the same key. If no prefix 
 * collision is found, 1 is return, otherwise 0 is returned and the client 
 * has an error emitted describing the error. */
int checkPrefixCollisionsOrReply(client *c, robj **prefixes, size_t numprefix) {
    for (size_t i = 0; i < numprefix; i++) {
        /* Check input list has no overlap with existing prefixes. */
        if (c->client_tracking_prefixes) {
            raxIterator ri;
            raxStart(&ri,c->client_tracking_prefixes);
            raxSeek(&ri,"^",NULL,0);
            while(raxNext(&ri)) {
                if (stringCheckPrefix(ri.key,ri.key_len,
                    prefixes[i]->ptr,sdslen(prefixes[i]->ptr))) 
                {
                    sds collision = sdsnewlen(ri.key,ri.key_len);
                    addReplyErrorFormat(c,
                        "Prefix '%s' overlaps with an existing prefix '%s'. "
                        "Prefixes for a single client must not overlap.",
                        (unsigned char *)prefixes[i]->ptr,
                        (unsigned char *)collision);
                    sdsfree(collision);
                    raxStop(&ri);
                    return 0;
                }
            }
            raxStop(&ri);
        }
        /* Check input has no overlap with itself. */
        for (size_t j = i + 1; j < numprefix; j++) {
            if (stringCheckPrefix(prefixes[i]->ptr,sdslen(prefixes[i]->ptr),
                prefixes[j]->ptr,sdslen(prefixes[j]->ptr)))
            {
                addReplyErrorFormat(c,
                    "Prefix '%s' overlaps with another provided prefix '%s'. "
                    "Prefixes for a single client must not overlap.",
                    (unsigned char *)prefixes[i]->ptr,
                    (unsigned char *)prefixes[j]->ptr);
                return i;
            }
        }
    }
    return 1;
}

/* Set the client 'c' to track the prefix 'prefix'. If the client 'c' is
 * already registered for the specified prefix, no operation is performed. */
void enableBcastTrackingForPrefix(client *c, char *prefix, size_t plen) {
    bcastState *bs = raxFind(PrefixTable,(unsigned char*)prefix,plen);
    /* If this is the first client subscribing to such prefix, create
     * the prefix in the table. */
    if (bs == raxNotFound) {
        bs = zmalloc(sizeof(*bs));
        bs->keys = raxNew();
        bs->clients = raxNew();
        raxInsert(PrefixTable,(unsigned char*)prefix,plen,bs,NULL);
    }
    if (raxTryInsert(bs->clients,(unsigned char*)&c,sizeof(c),NULL,NULL)) {
        if (c->client_tracking_prefixes == NULL)
            c->client_tracking_prefixes = raxNew();
        raxInsert(c->client_tracking_prefixes,
                  (unsigned char*)prefix,plen,NULL,NULL);
    }
}

/* Enable the tracking state for the client 'c', and as a side effect allocates
 * the tracking table if needed. If the 'redirect_to' argument is non zero, the
 * invalidation messages for this client will be sent to the client ID
 * specified by the 'redirect_to' argument. Note that if such client will
 * eventually get freed, we'll send a message to the original client to
 * inform it of the condition. Multiple clients can redirect the invalidation
 * messages to the same client ID. */
void enableTracking(client *c, uint64_t redirect_to, uint64_t options, robj **prefix, size_t numprefix) {
    if (!(c->flags & CLIENT_TRACKING)) server.tracking_clients++;
    c->flags |= CLIENT_TRACKING;
    c->flags &= ~(CLIENT_TRACKING_BROKEN_REDIR|CLIENT_TRACKING_BCAST|
                  CLIENT_TRACKING_OPTIN|CLIENT_TRACKING_OPTOUT|
                  CLIENT_TRACKING_NOLOOP);
    c->client_tracking_redirection = redirect_to;

    /* This may be the first client we ever enable. Create the tracking
     * table if it does not exist. */
    if (TrackingTable == NULL) {
        TrackingTable = raxNew();
        PrefixTable = raxNew();
        TrackingChannelName = createStringObject("__redis__:invalidate",20);
    }

    /* For broadcasting, set the list of prefixes in the client. */
    if (options & CLIENT_TRACKING_BCAST) {
        c->flags |= CLIENT_TRACKING_BCAST;
        if (numprefix == 0) enableBcastTrackingForPrefix(c,"",0);
        for (size_t j = 0; j < numprefix; j++) {
            sds sdsprefix = prefix[j]->ptr;
            enableBcastTrackingForPrefix(c,sdsprefix,sdslen(sdsprefix));
        }
    }

    /* Set the remaining flags that don't need any special handling. */
    c->flags |= options & (CLIENT_TRACKING_OPTIN|CLIENT_TRACKING_OPTOUT|
                           CLIENT_TRACKING_NOLOOP);
}

/* This function is called after the execution of a readonly command in the
 * case the client 'c' has keys tracking enabled and the tracking is not
 * in BCAST mode. It will populate the tracking invalidation table according
 * to the keys the user fetched, so that Redis will know what are the clients
 * that should receive an invalidation message with certain groups of keys
 * are modified. */
void trackingRememberKeys(client *c) {
    /* Return if we are in optin/out mode and the right CACHING command
     * was/wasn't given in order to modify the default behavior. */
    uint64_t optin = c->flags & CLIENT_TRACKING_OPTIN;
    uint64_t optout = c->flags & CLIENT_TRACKING_OPTOUT;
    uint64_t caching_given = c->flags & CLIENT_TRACKING_CACHING;
    if ((optin && !caching_given) || (optout && caching_given)) return;

    getKeysResult result = GETKEYS_RESULT_INIT;
    int numkeys = getKeysFromCommand(c->cmd,c->argv,c->argc,&result);
    if (!numkeys) {
        getKeysFreeResult(&result);
        return;
    }

    int *keys = result.keys;

    for(int j = 0; j < numkeys; j++) {
        int idx = keys[j];
        sds sdskey = c->argv[idx]->ptr;
        rax *ids = raxFind(TrackingTable,(unsigned char*)sdskey,sdslen(sdskey));
        if (ids == raxNotFound) {
            ids = raxNew();
            int inserted = raxTryInsert(TrackingTable,(unsigned char*)sdskey,
                                        sdslen(sdskey),ids, NULL);
            serverAssert(inserted == 1);
        }
        if (raxTryInsert(ids,(unsigned char*)&c->id,sizeof(c->id),NULL,NULL))
            TrackingTableTotalItems++;
    }
    getKeysFreeResult(&result);
}

/* Given a key name, this function sends an invalidation message in the
 * proper channel (depending on RESP version: PubSub or Push message) and
 * to the proper client (in case of redirection), in the context of the
 * client 'c' with tracking enabled.
 *
 * In case the 'proto' argument is non zero, the function will assume that
 * 'keyname' points to a buffer of 'keylen' bytes already expressed in the
 * form of Redis RESP protocol. This is used for:
 * - In BCAST mode, to send an array of invalidated keys to all
 *   applicable clients
 * - Following a flush command, to send a single RESP NULL to indicate
 *   that all keys are now invalid. */
void sendTrackingMessage(client *c, char *keyname, size_t keylen, int proto) {
    int using_redirection = 0;
    if (c->client_tracking_redirection) {
        client *redir = lookupClientByID(c->client_tracking_redirection);
        if (!redir) {
            c->flags |= CLIENT_TRACKING_BROKEN_REDIR;
            /* We need to signal to the original connection that we
             * are unable to send invalidation messages to the redirected
             * connection, because the client no longer exist. */
            if (c->resp > 2) {
                addReplyPushLen(c,2);
                addReplyBulkCBuffer(c,"tracking-redir-broken",21);
                addReplyLongLong(c,c->client_tracking_redirection);
            }
            return;
        }
        c = redir;
        using_redirection = 1;
    }

    /* Only send such info for clients in RESP version 3 or more. However
     * if redirection is active, and the connection we redirect to is
     * in Pub/Sub mode, we can support the feature with RESP 2 as well,
     * by sending Pub/Sub messages in the __redis__:invalidate channel. */
    if (c->resp > 2) {
        addReplyPushLen(c,2);
        addReplyBulkCBuffer(c,"invalidate",10);
    } else if (using_redirection && c->flags & CLIENT_PUBSUB) {
        /* We use a static object to speedup things, however we assume
         * that addReplyPubsubMessage() will not take a reference. */
        addReplyPubsubMessage(c,TrackingChannelName,NULL);
    } else {
        /* If are here, the client is not using RESP3, nor is
         * redirecting to another client. We can't send anything to
         * it since RESP2 does not support push messages in the same
         * connection. */
        return;
    }

    /* Send the "value" part, which is the array of keys. */
    if (proto) {
        addReplyProto(c,keyname,keylen);
    } else {
        addReplyArrayLen(c,1);
        addReplyBulkCBuffer(c,keyname,keylen);
    }
}

/* This function is called when a key is modified in Redis and in the case
 * we have at least one client with the BCAST mode enabled.
 * Its goal is to set the key in the right broadcast state if the key
 * matches one or more prefixes in the prefix table. Later when we
 * return to the event loop, we'll send invalidation messages to the
 * clients subscribed to each prefix. */
void trackingRememberKeyToBroadcast(client *c, char *keyname, size_t keylen) {
    raxIterator ri;
    raxStart(&ri,PrefixTable);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        if (ri.key_len > keylen) continue;
        if (ri.key_len != 0 && memcmp(ri.key,keyname,ri.key_len) != 0)
            continue;
        bcastState *bs = ri.data;
        /* We insert the client pointer as associated value in the radix
         * tree. This way we know who was the client that did the last
         * change to the key, and can avoid sending the notification in the
         * case the client is in NOLOOP mode. */
        raxTryInsert(bs->keys,(unsigned char*)keyname,keylen,c,NULL);
    }
    raxStop(&ri);
}

/* This function is called from signalModifiedKey() or other places in Redis
 * when a key changes value. In the context of keys tracking, our task here is
 * to send a notification to every client that may have keys about such caching
 * slot.
 *
 * Note that 'c' may be NULL in case the operation was performed outside the
 * context of a client modifying the database (for instance when we delete a
 * key because of expire).
 *
 * The last argument 'bcast' tells the function if it should also schedule
 * the key for broadcasting to clients in BCAST mode. This is the case when
 * the function is called from the Redis core once a key is modified, however
 * we also call the function in order to evict keys in the key table in case
 * of memory pressure: in that case the key didn't really change, so we want
 * just to notify the clients that are in the table for this key, that would
 * otherwise miss the fact we are no longer tracking the key for them. */
void trackingInvalidateKeyRaw(client *c, char *key, size_t keylen, int bcast) {
    if (TrackingTable == NULL) return;

    if (bcast && raxSize(PrefixTable) > 0)
        trackingRememberKeyToBroadcast(c,key,keylen);

    rax *ids = raxFind(TrackingTable,(unsigned char*)key,keylen);
    if (ids == raxNotFound) return;

    raxIterator ri;
    raxStart(&ri,ids);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        uint64_t id;
        memcpy(&id,ri.key,sizeof(id));
        client *target = lookupClientByID(id);
        /* Note that if the client is in BCAST mode, we don't want to
         * send invalidation messages that were pending in the case
         * previously the client was not in BCAST mode. This can happen if
         * TRACKING is enabled normally, and then the client switches to
         * BCAST mode. */
        if (target == NULL ||
            !(target->flags & CLIENT_TRACKING)||
            target->flags & CLIENT_TRACKING_BCAST)
        {
            continue;
        }

        /* If the client enabled the NOLOOP mode, don't send notifications
         * about keys changed by the client itself. */
        if (target->flags & CLIENT_TRACKING_NOLOOP &&
            target == c)
        {
            continue;
        }

        sendTrackingMessage(target,key,keylen,0);
    }
    raxStop(&ri);

    /* Free the tracking table: we'll create the radix tree and populate it
     * again if more keys will be modified in this caching slot. */
    TrackingTableTotalItems -= raxSize(ids);
    raxFree(ids);
    raxRemove(TrackingTable,(unsigned char*)key,keylen,NULL);
}

/* Wrapper (the one actually called across the core) to pass the key
 * as object. */
void trackingInvalidateKey(client *c, robj *keyobj) {
    trackingInvalidateKeyRaw(c,keyobj->ptr,sdslen(keyobj->ptr),1);
}

/* This function is called when one or all the Redis databases are
 * flushed. Caching keys are not specific for each DB but are global: 
 * currently what we do is send a special notification to clients with 
 * tracking enabled, sending a RESP NULL, which means, "all the keys", 
 * in order to avoid flooding clients with many invalidation messages 
 * for all the keys they may hold.
 */
void freeTrackingRadixTreeCallback(void *rt) {
    raxFree(rt);
}

void freeTrackingRadixTree(rax *rt) {
    raxFreeWithCallback(rt,freeTrackingRadixTreeCallback);
}

/* A RESP NULL is sent to indicate that all keys are invalid */
void trackingInvalidateKeysOnFlush(int async) {
    if (server.tracking_clients) {
        listNode *ln;
        listIter li;
        listRewind(server.clients,&li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = listNodeValue(ln);
            if (c->flags & CLIENT_TRACKING) {
                sendTrackingMessage(c,shared.null[c->resp]->ptr,sdslen(shared.null[c->resp]->ptr),1);
            }
        }
    }

    /* In case of FLUSHALL, reclaim all the memory used by tracking. */
    if (TrackingTable) {
        if (async) {
            freeTrackingRadixTreeAsync(TrackingTable);
        } else {
            freeTrackingRadixTree(TrackingTable);
        }
        TrackingTable = raxNew();
        TrackingTableTotalItems = 0;
    }
}

/* Tracking forces Redis to remember information about which client may have
 * certain keys. In workloads where there are a lot of reads, but keys are
 * hardly modified, the amount of information we have to remember server side
 * could be a lot, with the number of keys being totally not bound.
 *
 * So Redis allows the user to configure a maximum number of keys for the
 * invalidation table. This function makes sure that we don't go over the
 * specified fill rate: if we are over, we can just evict information about
 * a random key, and send invalidation messages to clients like if the key was
 * modified. */
void trackingLimitUsedSlots(void) {
    static unsigned int timeout_counter = 0;
    if (TrackingTable == NULL) return;
    if (server.tracking_table_max_keys == 0) return; /* No limits set. */
    size_t max_keys = server.tracking_table_max_keys;
    if (raxSize(TrackingTable) <= max_keys) {
        timeout_counter = 0;
        return; /* Limit not reached. */
    }

    /* We have to invalidate a few keys to reach the limit again. The effort
     * we do here is proportional to the number of times we entered this
     * function and found that we are still over the limit. */
    int effort = 100 * (timeout_counter+1);

    /* We just remove one key after another by using a random walk. */
    raxIterator ri;
    raxStart(&ri,TrackingTable);
    while(effort > 0) {
        effort--;
        raxSeek(&ri,"^",NULL,0);
        raxRandomWalk(&ri,0);
        if (raxEOF(&ri)) break;
        trackingInvalidateKeyRaw(NULL,(char*)ri.key,ri.key_len,0);
        if (raxSize(TrackingTable) <= max_keys) {
            timeout_counter = 0;
            raxStop(&ri);
            return; /* Return ASAP: we are again under the limit. */
        }
    }

    /* If we reach this point, we were not able to go under the configured
     * limit using the maximum effort we had for this run. */
    raxStop(&ri);
    timeout_counter++;
}

/* Generate Redis protocol for an array containing all the key names
 * in the 'keys' radix tree. If the client is not NULL, the list will not
 * include keys that were modified the last time by this client, in order
 * to implement the NOLOOP option.
 *
 * If the resulting array would be empty, NULL is returned instead. */
sds trackingBuildBroadcastReply(client *c, rax *keys) {
    raxIterator ri;
    uint64_t count;

    if (c == NULL) {
        count = raxSize(keys);
    } else {
        count = 0;
        raxStart(&ri,keys);
        raxSeek(&ri,"^",NULL,0);
        while(raxNext(&ri)) {
            if (ri.data != c) count++;
        }
        raxStop(&ri);

        if (count == 0) return NULL;
    }

    /* Create the array reply with the list of keys once, then send
    * it to all the clients subscribed to this prefix. */
    char buf[32];
    size_t len = ll2string(buf,sizeof(buf),count);
    sds proto = sdsempty();
    proto = sdsMakeRoomFor(proto,count*15);
    proto = sdscatlen(proto,"*",1);
    proto = sdscatlen(proto,buf,len);
    proto = sdscatlen(proto,"\r\n",2);
    raxStart(&ri,keys);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        if (c && ri.data == c) continue;
        len = ll2string(buf,sizeof(buf),ri.key_len);
        proto = sdscatlen(proto,"$",1);
        proto = sdscatlen(proto,buf,len);
        proto = sdscatlen(proto,"\r\n",2);
        proto = sdscatlen(proto,ri.key,ri.key_len);
        proto = sdscatlen(proto,"\r\n",2);
    }
    raxStop(&ri);
    return proto;
}

/* This function will run the prefixes of clients in BCAST mode and
 * keys that were modified about each prefix, and will send the
 * notifications to each client in each prefix. */
void trackingBroadcastInvalidationMessages(void) {
    raxIterator ri, ri2;

    /* Return ASAP if there is nothing to do here. */
    if (TrackingTable == NULL || !server.tracking_clients) return;

    raxStart(&ri,PrefixTable);
    raxSeek(&ri,"^",NULL,0);

    /* For each prefix... */
    while(raxNext(&ri)) {
        bcastState *bs = ri.data;

        if (raxSize(bs->keys)) {
            /* Generate the common protocol for all the clients that are
             * not using the NOLOOP option. */
            sds proto = trackingBuildBroadcastReply(NULL,bs->keys);

            /* Send this array of keys to every client in the list. */
            raxStart(&ri2,bs->clients);
            raxSeek(&ri2,"^",NULL,0);
            while(raxNext(&ri2)) {
                client *c;
                memcpy(&c,ri2.key,sizeof(c));
                if (c->flags & CLIENT_TRACKING_NOLOOP) {
                    /* This client may have certain keys excluded. */
                    sds adhoc = trackingBuildBroadcastReply(c,bs->keys);
                    if (adhoc) {
                        sendTrackingMessage(c,adhoc,sdslen(adhoc),1);
                        sdsfree(adhoc);
                    }
                } else {
                    sendTrackingMessage(c,proto,sdslen(proto),1);
                }
            }
            raxStop(&ri2);

            /* Clean up: we can remove everything from this state, because we
             * want to only track the new keys that will be accumulated starting
             * from now. */
            sdsfree(proto);
        }
        raxFree(bs->keys);
        bs->keys = raxNew();
    }
    raxStop(&ri);
}

/* This is just used in order to access the amount of used slots in the
 * tracking table. */
uint64_t trackingGetTotalItems(void) {
    return TrackingTableTotalItems;
}

uint64_t trackingGetTotalKeys(void) {
    if (TrackingTable == NULL) return 0;
    return raxSize(TrackingTable);
}

uint64_t trackingGetTotalPrefixes(void) {
    if (PrefixTable == NULL) return 0;
    return raxSize(PrefixTable);
}

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

/* The tracking table is constituted by 2^24 radix trees (each tree, and the
 * table itself, are allocated in a lazy way only when needed) tracking
 * clients that may have certain keys in their local, client side, cache.
 *
 * Keys are grouped into 2^24 slots, in a way similar to Redis Cluster hash
 * slots, however here the function we use is crc64, taking the least
 * significant 24 bits of the output.
 *
 * When a client enables tracking with "CLIENT TRACKING on", each key served to
 * the client is hashed to one of such slots, and Redis will remember what
 * client may have keys about such slot. Later, when a key in a given slot is
 * modified, all the clients that may have local copies of keys in that slot
 * will receive an invalidation message. There is no distinction of database
 * number: a single table is used.
 *
 * Clients will normally take frequently requested objects in memory, removing
 * them when invalidation messages are received. A strategy clients may use is
 * to just cache objects in a dictionary, associating to each cached object
 * some incremental epoch, or just a timestamp. When invalidation messages are
 * received clients may store, in a different table, the timestamp (or epoch)
 * of the invalidation of such given slot: later when accessing objects, the
 * eviction of stale objects may be performed in a lazy way by checking if the
 * cached object timestamp is older than the invalidation timestamp for such
 * objects.
 *
 * The output of the 24 bit hash function is very large (more than 16 million
 * possible slots), so clients that may want to use less resources may only
 * use the most significant bits instead of the full 24 bits. */
#define TRACKING_TABLE_SIZE (1<<24)
rax **TrackingTable = NULL;
unsigned long TrackingTableUsedSlots = 0;
robj *TrackingChannelName;

/* Remove the tracking state from the client 'c'. Note that there is not much
 * to do for us here, if not to decrement the counter of the clients in
 * tracking mode, because we just store the ID of the client in the tracking
 * table, so we'll remove the ID reference in a lazy way. Otherwise when a
 * client with many entries in the table is removed, it would cost a lot of
 * time to do the cleanup. */
void disableTracking(client *c) {
    if (c->flags & CLIENT_TRACKING) {
        server.tracking_clients--;
        c->flags &= ~(CLIENT_TRACKING|CLIENT_TRACKING_BROKEN_REDIR);
    }
}

/* Enable the tracking state for the client 'c', and as a side effect allocates
 * the tracking table if needed. If the 'redirect_to' argument is non zero, the
 * invalidation messages for this client will be sent to the client ID
 * specified by the 'redirect_to' argument. Note that if such client will
 * eventually get freed, we'll send a message to the original client to
 * inform it of the condition. Multiple clients can redirect the invalidation
 * messages to the same client ID. */
void enableTracking(client *c, uint64_t redirect_to) {
    if (c->flags & CLIENT_TRACKING) return;
    c->flags |= CLIENT_TRACKING;
    c->flags &= ~CLIENT_TRACKING_BROKEN_REDIR;
    c->client_tracking_redirection = redirect_to;
    server.tracking_clients++;
    if (TrackingTable == NULL) {
        TrackingTable = zcalloc(sizeof(rax*) * TRACKING_TABLE_SIZE);
        TrackingChannelName = createStringObject("__redis__:invalidate",20);
    }
}

/* This function is called after the excution of a readonly command in the
 * case the client 'c' has keys tracking enabled. It will populate the
 * tracking ivalidation table according to the keys the user fetched, so that
 * Redis will know what are the clients that should receive an invalidation
 * message with certain groups of keys are modified. */
void trackingRememberKeys(client *c) {
    int numkeys;
    int *keys = getKeysFromCommand(c->cmd,c->argv,c->argc,&numkeys);
    if (keys == NULL) return;

    for(int j = 0; j < numkeys; j++) {
        int idx = keys[j];
        sds sdskey = c->argv[idx]->ptr;
        uint64_t hash = crc64(0,
            (unsigned char*)sdskey,sdslen(sdskey))&(TRACKING_TABLE_SIZE-1);
        if (TrackingTable[hash] == NULL) {
            TrackingTable[hash] = raxNew();
            TrackingTableUsedSlots++;
        }
        raxTryInsert(TrackingTable[hash],
            (unsigned char*)&c->id,sizeof(c->id),NULL,NULL);
    }
    getKeysFreeResult(keys);
}

void sendTrackingMessage(client *c, long long hash) {
    int using_redirection = 0;
    if (c->client_tracking_redirection) {
        client *redir = lookupClientByID(c->client_tracking_redirection);
        if (!redir) {
            /* We need to signal to the original connection that we
             * are unable to send invalidation messages to the redirected
             * connection, because the client no longer exist. */
            if (c->resp > 2) {
                addReplyPushLen(c,3);
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
        addReplyLongLong(c,hash);
    } else if (using_redirection && c->flags & CLIENT_PUBSUB) {
        robj *msg = createStringObjectFromLongLong(hash);
        addReplyPubsubMessage(c,TrackingChannelName,msg);
        decrRefCount(msg);
    }
}

/* Invalidates a caching slot: this is actually the low level implementation
 * of the API that Redis calls externally, that is trackingInvalidateKey(). */
void trackingInvalidateSlot(uint64_t slot) {
    if (TrackingTable == NULL || TrackingTable[slot] == NULL) return;

    raxIterator ri;
    raxStart(&ri,TrackingTable[slot]);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        uint64_t id;
        memcpy(&id,ri.key,ri.key_len);
        client *c = lookupClientByID(id);
        if (c == NULL || !(c->flags & CLIENT_TRACKING)) continue;
        sendTrackingMessage(c,slot);
    }
    raxStop(&ri);

    /* Free the tracking table: we'll create the radix tree and populate it
     * again if more keys will be modified in this caching slot. */
    raxFree(TrackingTable[slot]);
    TrackingTable[slot] = NULL;
    TrackingTableUsedSlots--;
}

/* This function is called from signalModifiedKey() or other places in Redis
 * when a key changes value. In the context of keys tracking, our task here is
 * to send a notification to every client that may have keys about such caching
 * slot. */
void trackingInvalidateKey(robj *keyobj) {
    if (TrackingTable == NULL || TrackingTableUsedSlots == 0) return;

    sds sdskey = keyobj->ptr;
    uint64_t hash = crc64(0,
        (unsigned char*)sdskey,sdslen(sdskey))&(TRACKING_TABLE_SIZE-1);
    trackingInvalidateSlot(hash);
}

/* This function is called when one or all the Redis databases are flushed
 * (dbid == -1 in case of FLUSHALL). Caching slots are not specific for
 * each DB but are global: currently what we do is sending a special
 * notification to clients with tracking enabled, invalidating the caching
 * slot "-1", which means, "all the keys", in order to avoid flooding clients
 * with many invalidation messages for all the keys they may hold.
 *
 * However trying to flush the tracking table here is very costly:
 * we need scanning 16 million caching slots in the table to check
 * if they are used, this introduces a big delay. So what we do is to really
 * flush the table in the case of FLUSHALL. When a FLUSHDB is called instead
 * we just send the invalidation message to all the clients, but don't
 * flush the table: it will slowly get garbage collected as more keys
 * are modified in the used caching slots. */
void trackingInvalidateKeysOnFlush(int dbid) {
    if (server.tracking_clients) {
        listNode *ln;
        listIter li;
        listRewind(server.clients,&li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = listNodeValue(ln);
            if (c->flags & CLIENT_TRACKING) {
                sendTrackingMessage(c,-1);
            }
        }
    }

    /* In case of FLUSHALL, reclaim all the memory used by tracking. */
    if (dbid == -1 && TrackingTable) {
        for (int j = 0; j < TRACKING_TABLE_SIZE && TrackingTableUsedSlots > 0; j++) {
            if (TrackingTable[j] != NULL) {
                raxFree(TrackingTable[j]);
                TrackingTable[j] = NULL;
                TrackingTableUsedSlots--;
            }
        }

        /* If there are no clients with tracking enabled, we can even
         * reclaim the memory used by the table itself. The code assumes
         * the table is allocated only if there is at least one client alive
         * with tracking enabled. */
        if (server.tracking_clients == 0) {
            zfree(TrackingTable);
            TrackingTable = NULL;
        }
    }
}

/* Tracking forces Redis to remember information about which client may have
 * keys about certian caching slots. In workloads where there are a lot of
 * reads, but keys are hardly modified, the amount of information we have
 * to remember server side could be a lot: for each 16 millions of caching
 * slots we may end with a radix tree containing many entries.
 *
 * So Redis allows the user to configure a maximum fill rate for the
 * invalidation table. This function makes sure that we don't go over the
 * specified fill rate: if we are over, we can just evict informations about
 * random caching slots, and send invalidation messages to clients like if
 * the key was modified. */
void trackingLimitUsedSlots(void) {
    static unsigned int timeout_counter = 0;

    if (server.tracking_table_max_fill == 0) return; /* No limits set. */
    unsigned int max_slots =
        (TRACKING_TABLE_SIZE/100) * server.tracking_table_max_fill;
    if (TrackingTableUsedSlots <= max_slots) {
        timeout_counter = 0;
        return; /* Limit not reached. */
    }

    /* We have to invalidate a few slots to reach the limit again. The effort
     * we do here is proportional to the number of times we entered this
     * function and found that we are still over the limit. */
    int effort = 100 * (timeout_counter+1);

    /* Let's start at a random position, and perform linear probing, in order
     * to improve cache locality. However once we are able to find an used
     * slot, jump again randomly, in order to avoid creating big holes in the
     * table (that will make this funciton use more resourced later). */
    while(effort > 0) {
        unsigned int idx = rand() % TRACKING_TABLE_SIZE;
        do {
            effort--;
            idx = (idx+1) % TRACKING_TABLE_SIZE;
            if (TrackingTable[idx] != NULL) {
                trackingInvalidateSlot(idx);
                if (TrackingTableUsedSlots <= max_slots) {
                    timeout_counter = 0;
                    return; /* Return ASAP: we are again under the limit. */
                } else {
                    break; /* Jump to next random position. */
                }
            }
        } while(effort > 0);
    }
    timeout_counter++;
}

/* This is just used in order to access the amount of used slots in the
 * tracking table. */
unsigned long long trackingGetUsedSlots(void) {
    return TrackingTableUsedSlots;
}

/*
 * Copyright (c) 2014-2015, pythias <pythias at gmail dot com>
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

#include "redis.h"
#include <math.h>

/*-----------------------------------------------------------------------------
 * Queue API
 *----------------------------------------------------------------------------*/
void queuePopMessage(redisClient *c) {
    if (c->queue == NULL) {
        addReplyError(c, "Queue has been deleted.");
        return;
    }

    int count = 0;
    queue *q = (queue *)c->queue->ptr;
    queueEntry *entry = queueIndex(q, c->queue_index);

    while (entry && count++ < QUEUE_SEND_MAX_SIZE) {
        addReply(c, shared.mbulkhdr[2]);
        addReplyBulk(c, createStringObjectFromLongLong(c->queue_index));
        addReplyBulk(c, entry->value);
        entry = entry->next;
        c->queue_index++;
    }

    /* Ready for comet */
    if (c->queue_index == q->len) {
        c->queue_ready = 0;
    } else {
        c->queue_ready = 1;
    }

    redisLog(REDIS_DEBUG, "Queue pop, Size:%lld, Current:%lld, Ready:%i", q->len, c->queue_index, c->queue_ready);
}

void queuePopComet(redisClient *c) {
    dictEntry *de;
    list *clients = NULL;

    de = dictFind(server.queue_clients, c->queue_key);
    if (de == NULL) {
        clients = listCreate();
        dictAdd(server.queue_clients, c->queue_key, clients);
        incrRefCount(c->queue_key);
    } else {
        clients = dictGetVal(de);
    }

    listAddNodeTail(clients, c);
}

void queueUnpopComet(redisClient *c, robj *key) {
    dictEntry *de;
    list *clients = NULL;
    listNode *ln;

    de = dictFind(server.queue_clients, key);
    if (de == NULL) return;

    clients = dictGetVal(de);
    ln = listSearchKey(clients, c);
    if (ln == NULL) return;

    listDelNode(clients, ln);

    if (listLength(clients) == 0) {
        incrRefCount(key);
        dictDelete(server.queue_clients, key);
        decrRefCount(key);
    }    
}

void queueUnpopClient(redisClient *c) {
    queueUnpopComet(c, c->queue_key);
}

int queueUnpopKey(robj *key) {
    dictEntry *de = dictFind(server.queue_clients, key);
    if (de == NULL) return 0;

    list *clients = dictGetVal(de);
    listNode *ln;
    listIter li;

    listRewind(clients, &li);
    while ((ln = listNext(&li))) {
        redisClient *c = listNodeValue(ln);
        addReplyError(c, "Queue has been removed.");
        //freeClient(c);
    }

    incrRefCount(key);
    dictDelete(server.queue_clients, key);
    decrRefCount(key);

    return 1;
}

int queuePushMessage(robj *key, robj *value, long long index) {
    int receivers = 0;
    dictEntry *de = dictFind(server.queue_clients, key);
    if (de == NULL) return receivers;

    list *list = dictGetVal(de);
    listNode *ln;
    listIter li;

    listRewind(list, &li);
    while ((ln = listNext(&li)) != NULL) {
        redisClient *c = listNodeValue(ln);
        if (c->queue_index + 1 == index) {
            addReplyMultiBulkLen(c, 2);
            addReplyBulk(c, createStringObjectFromLongLong(c->queue_index));
            addReplyBulk(c, value);
            c->queue_index++;

            receivers++;
        }
    }
    
    return receivers;
}

/*-----------------------------------------------------------------------------
 * Queue commands implementation
 *----------------------------------------------------------------------------*/

void qposCommand(redisClient *c) {
    long long index;
    robj *qobj = lookupKeyRead(c->db, c->argv[1]);
    if (qobj == NULL || qobj->type != REDIS_QUEUE) {
        addReply(c, shared.wrongtypeerr);
        return;
    }

    if (getLongLongFromObjectOrReply(c, c->argv[2], &index, NULL) != REDIS_OK) return;

    queue *q = qobj->ptr;

    if (index < 0) {
        c->queue_index = q->len + index;
    } else if (index > q->len) {
        c->queue_index = q->len;
    } else {
        c->queue_index = index;
    }

    c->queue_key = c->argv[1];
    incrRefCount(c->argv[1]);

    queuePopComet(c);

    addReplyStatus(c, "Done");
}

void qpushCommand(redisClient *c) {
    int j, pushed = 0;
    robj *qobj = lookupKeyWrite(c->db, c->argv[1]);

    if (qobj && qobj->type != REDIS_QUEUE) {
        addReply(c, shared.wrongtypeerr);
        return;
    }

    if (qobj && ((queue *)qobj->ptr)->len + c->argc >= LLONG_MAX) {
        addReplyError(c, "Queue size is out of range.");
        return;
    }

    for (j = 2; j < c->argc; j++) {
        c->argv[j] = tryObjectEncoding(c->argv[j]);
        if (!qobj) {
            qobj = createQueueObject();
            dbAdd(c->db, c->argv[1], qobj);
        }

        if (queueAdd(qobj->ptr, c->argv[j]) != NULL) {
            incrRefCount(c->argv[j]);
            queuePushMessage(c->argv[1], c->argv[j], ((queue *)qobj->ptr)->len);
            pushed++;
        }
    }

    if (pushed) {
        signalModifiedKey(c->db, c->argv[1]);
        notifyKeyspaceEvent(REDIS_NOTIFY_QUEUE, "qpush", c->argv[1], c->db->id);
        server.dirty += pushed;
    }
    
    addReplyLongLong(c, pushed);
}

void qpopCommand(redisClient *c) {
    robj *qobj = lookupKeyWrite(c->db, c->argv[1]);
    if (qobj == NULL || qobj->type != REDIS_QUEUE) {
        addReply(c, shared.wrongtypeerr);
        return;
    }

    queue *q = (queue *)qobj->ptr;

    if (c->queue_key == NULL || compareStringObjects(c->queue_key, c->argv[1]) != 0) {
        if (c->queue_key) decrRefCount(c->queue_key);

        c->queue_index = q->len;
    }

    c->queue = qobj;
    c->queue_key = c->argv[1];
    incrRefCount(qobj);
    incrRefCount(c->argv[1]);

    queuePopComet(c);
    queuePopMessage(c);
}

void qinfoCommand(redisClient *c) {
    robj *qobj = lookupKeyRead(c->db, c->argv[1]);
    if (qobj == NULL || qobj->type != REDIS_QUEUE) {
        addReply(c, shared.wrongtypeerr);
        return;
    }

    queue *q = (queue *)qobj->ptr;

    addReplyMultiBulkLen(c, 2);
    addReplyStatus(c, "length");
    addReplyBulk(c, createStringObjectFromLongLong(q->len));
}

void qrangeCommand(redisClient *c) {
    robj *qobj = lookupKeyRead(c->db, c->argv[1]);
    if (qobj == NULL || qobj->type != REDIS_QUEUE) {
        addReply(c, shared.wrongtypeerr);
        return;
    }

    queue *q = (queue *)qobj->ptr;

    long long start, end, qlen, rangelen;
    if ((getLongLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
        (getLongLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;

    qlen = q->len;

    if (start < 0) start = qlen + start;
    if (end < 0) end = qlen + end;
    if (start < 0) start = 0;

    if (start > end || start >= qlen) {
        addReply(c, shared.emptymultibulk);
        return;
    }

    if (end >= qlen) end = qlen - 1;
    rangelen = (end - start) + 1;

    addReplyMultiBulkLen(c, rangelen);
    
    queueEntry *entry = queueIndex(q, start);
    while (rangelen--) {
        if (entry) {
            addReplyBulk(c, entry->value);
            entry = entry->next;    
        } else {
            addReplyBulk(c, shared.nullbulk);
        }
    }
}

void qdelCommand(redisClient *c) {
    robj *qobj = lookupKeyRead(c->db, c->argv[1]);
    if (qobj == NULL || qobj->type != REDIS_QUEUE) {
        addReply(c, shared.wrongtypeerr);
        return;
    }

    //unpop queue key
    queueUnpopKey(c->argv[1]);

    if (dbDelete(c->db, c->argv[1])) {
        signalModifiedKey(c->db, c->argv[1]);
        notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        server.dirty++;

        addReplyLongLong(c, 1);
    } else {
        addReplyLongLong(c, 0);
    }
}

void qgetCommand(redisClient *c) {
    robj *qobj = lookupKeyRead(c->db, c->argv[1]);
    if (qobj == NULL || qobj->type != REDIS_QUEUE) {
        addReply(c, shared.wrongtypeerr);
        return;
    }

    queue *q = (queue *)qobj->ptr;

    long long index;
    if (getLongLongFromObjectOrReply(c, c->argv[2], &index, NULL) != REDIS_OK) return;
    
    queueEntry *entry = queueIndex(q, index);
    if (entry == NULL) {
        addReplyBulk(c, shared.nullbulk);
    }
    else {
        addReplyBulk(c, entry->value);
    }
}
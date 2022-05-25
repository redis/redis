/* Copyright (c) 2021, ctrip.com
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

#include "ctrip_swap.h"

requestListener *requestListenerCreate(redisDb *db, robj *key,
        requestProceed cb, client *c, void *pd) {
    requestListener *listener = zmalloc(sizeof(requestListener));
    listener->db = db;
    if (key) incrRefCount(key);
    listener->key = key;
    listener->proceed = cb;
    listener->c = c;
    listener->pd = pd;
    return listener;
}

void requestListenerRelease(requestListener *listener) {
    if (!listener) return;
    decrRefCount(listener->key);
    zfree(listener);
}

void requestListenersRelease(requestListeners *listeners);
void requestListenersDestructor(void *privdata, void *val) {
    DICT_NOTUSED(privdata);
    requestListenersRelease(val);
}

dictType requestListenersDictType = {
    dictSdsHash,                    /* hash function */
    NULL,                           /* key dup */
    NULL,                           /* val dup */
    dictSdsKeyCompare,              /* key compare */
    dictSdsDestructor,              /* key destructor */
    requestListenersDestructor,     /* val destructor */
    NULL                            /* allow to expand */
};

static requestListeners *requestListenersCreate(int level, redisDb *db,
        robj *key, requestListeners *parent) {
    requestListeners *listeners;

    listeners = zmalloc(sizeof(requestListeners));
    listeners->listeners = listCreate();
    listeners->nlisteners = 0;
    listeners->parent = parent;
    listeners->level = level;

    switch (level) {
    case REQUEST_LISTENERS_LEVEL_SVR:
        listeners->svr.dbnum = server.dbnum;
        listeners->svr.dbs = zmalloc(server.dbnum*sizeof(requestListeners));
        break;
    case REQUEST_LISTENERS_LEVEL_DB:
        serverAssert(db);
        listeners->db.db = db;
        listeners->db.keys = dictCreate(&requestListenersDictType, NULL);
        break;
    case REQUEST_LISTENERS_LEVEL_KEY:
        serverAssert(key);
        incrRefCount(key);
        listeners->key.key = key;
        break;
    default:
        break;
    }

    return listeners;
}

void requestListenersRelease(requestListeners *listeners) {
    if (!listeners) return;
    serverAssert(!listLength(listeners->listeners));
    listRelease(listeners->listeners);

    switch (listeners->level) {
    case REQUEST_LISTENERS_LEVEL_SVR:
        zfree(listeners->svr.dbs);
        break;
    case REQUEST_LISTENERS_LEVEL_DB:
        dictRelease(listeners->db.keys);
        break;
    case REQUEST_LISTENERS_LEVEL_KEY:
        decrRefCount(listeners->key.key);
        break;
    default:
        break;
    }
    zfree(listeners);
}

sds requestListenersDump(requestListeners *listeners) {
    listIter li;
    listNode *ln;
    sds result = sdsempty();
    char *intentions[] = {"NOP", "IN", "OUT", "DEL"};

    result = sdscat(result, "[");
    listRewind(listeners->listeners,&li);
    while ((ln = listNext(&li))) {
        requestListener *listener = listNodeValue(ln);
        client *c = listener->c;
        if (ln != listFirst(listeners->listeners)) result = sdscat(result,",");
        result = sdscat(result,"("); 
        if (c->cmd) result = sdscat(result,intentions[c->cmd->intention]); 
        result = sdscat(result,":"); 
        if (listeners->level == REQUEST_LISTENERS_LEVEL_KEY)
            result = sdscatsds(result,listeners->key.key->ptr); 
        result = sdscat(result,":"); 
        if (c->cmd) result = sdscat(result,c->cmd->name); 
        result = sdscat(result,")"); 
    }
    result = sdscat(result, "]");
    return result;
}

static inline void requestListenersLink(requestListeners *listeners) {
    while (listeners) {
        listeners->nlisteners++;
        listeners = listeners->parent;
    }
}

static inline void requestListenersUnlink(requestListeners *listeners) {
    while (listeners) {
        listeners->nlisteners--;
        listeners = listeners->parent;
    }
}

static void requestListenersPush(requestListeners *listeners,
        requestListener *listener) {
    serverAssert(listeners);
    listAddNodeTail(listeners->listeners, listener);
    requestListenersLink(listeners);
}

requestListener *requestListenersPop(requestListeners *listeners) {
    serverAssert(listeners);
    if (!listLength(listeners->listeners)) return NULL;
    listNode *ln = listFirst(listeners->listeners);
    requestListener *listener = listNodeValue(ln);
    listDelNode(listeners->listeners, ln);
    requestListenersUnlink(listeners);
    return listener;
}

requestListener *requestListenersPeek(requestListeners *listeners) {
    serverAssert(listeners);
    if (!listLength(listeners->listeners)) return NULL;
    listNode *ln = listFirst(listeners->listeners);
    requestListener *listener = listNodeValue(ln);
    return listener;
}

/* return true if current or lower level listeners not finished.
 * - swap should not proceed if current or lower level listeners exists.
 *   (e.g. flushdb shoul not proceed if SWAP GET key exits.)
 * - can't release listeners if current or lower level listeners exists. */
int requestListenersTreeBlocking(requestListeners *listeners) {
    if (listeners && (listLength(listeners->listeners) ||
                listeners->nlisteners > 0)) {
        return 1;
    } else {
        return 0;
    }
}

requestListeners *serverRequestListenersCreate() {
    int i;
    requestListeners *s = requestListenersCreate(
            REQUEST_LISTENERS_LEVEL_SVR, NULL, NULL, NULL);

    for (i = 0; i < server.dbnum; i++) {
        redisDb *db = server.db + i;
        s->svr.dbs[i] = requestListenersCreate(
                REQUEST_LISTENERS_LEVEL_DB, db, NULL, s);
    }
    return s;
}

void serverRequestListenersRelease(requestListeners *s) {
    int i;
    for (i = 0; i < s->svr.dbnum; i++) {
        requestListenersRelease(s->svr.dbs[i]);
    }
    requestListenersRelease(s);
    zfree(s);
}

static requestListeners *requestBindListeners(redisDb *db, robj *key,
        int create) {
    requestListeners *svr_listeners, *db_listeners, *key_listeners;

    svr_listeners = server.request_listeners;
    if (db == NULL || listLength(svr_listeners->listeners)) {
        return svr_listeners;
    }

    db_listeners = svr_listeners->svr.dbs[db->id];
    if (key == NULL || listLength(db_listeners->listeners)) {
        return db_listeners;
    }

    key_listeners = dictFetchValue(db_listeners->db.keys,key);
    if (key_listeners == NULL) {
        if (create) {
            key_listeners = requestListenersCreate(
                    REQUEST_LISTENERS_LEVEL_KEY,db,key,db_listeners);
        }
    }

    return key_listeners;
}

static inline int proceed(requestListeners *listeners,
        requestListener *listener) {
    return listener->proceed(listeners,listener->db,
            listener->key,listener->c,listener->pd);
}

int requestBlocked(redisDb *db, robj *key) {
    requestListeners *listeners = requestBindListeners(db,key,0);
    if (listeners == NULL) return 0;
    return listeners->nlisteners > 0;
}

int requestWait(redisDb *db, robj *key, requestProceed cb, client *c,
        void *pd) {
    int blocking;
    requestListeners *listeners;
    requestListener *listener;

    listeners = requestBindListeners(db,key,1);
    blocking = listeners->nlisteners > 0;
    listener = requestListenerCreate(db,key,cb,c,pd);
    requestListenersPush(listeners, listener);

    /* Proceed right away if request key is not blocking, otherwise
     * execution is defered. */
    if (!blocking) proceed(listeners, listener);
    return 0;
}

int requestNotify(void *listeners_) {
    requestListeners *listeners = listeners_;
    requestListener *current, *next;

    current = requestListenersPop(listeners);
    requestListenerRelease(current);

    while (listeners) {
        /* First, try proceed current level listener. */
        if (listLength(listeners->listeners)) {
            next = requestListenersPeek(listeners);
            proceed(listeners,next);
            break;
        }

        /* If current level drained, try proceed parent listener. */ 
        if (listeners->nlisteners) {
            /* child listeners exists, wait untill all child finished. */
            break;
        } else {
            if (listeners->level == REQUEST_LISTENERS_LEVEL_KEY) {
                /* Only key level listeners releases, DB or server level
                 * key released only when server exit. */
                requestListenersRelease(listeners);
            }
            listeners = listeners->parent;
        }
    }

    return 0;
}

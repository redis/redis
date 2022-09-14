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

static inline void requestListenersLinkListener(requestListeners *listeners,
        requestListener *listener) {
    while (listeners) {
        listeners->nlistener++;
        if (listeners->cur_txid != listener->txid) {
            listeners->cur_txid = listener->txid;
            listeners->cur_ntxlistener = 0;
            listeners->cur_ntxrequest = 0;
            listeners->cur_ntxacked = 0;
        }
        listeners->cur_ntxlistener++;
        listeners = listeners->parent;
    }
}

static inline void requestListenersUnlink(requestListeners *listeners) {
    while (listeners) {
        listeners->nlistener--;
        listeners = listeners->parent;
    }
}

static inline void requestListenersLinkEntry(requestListeners *listeners,
        int64_t txid) {
    while (listeners) {
        /* Must link listener before link entry */
        serverAssert(listeners->cur_txid == txid);
        listeners->cur_ntxrequest++;
        listeners = listeners->parent;
    }
}

static void requestListenersPush(requestListeners *listeners,
        requestListener *listener) {
    serverAssert(listeners);
    listAddNodeTail(listeners->listeners, listener);
    requestListenersLinkListener(listeners, listener);
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

requestListener *requestListenerCreate(requestListeners *listeners,
        int64_t txid) {
    requestListener *listener = zcalloc(sizeof(requestListener));
    UNUSED(listeners);
    listener->txid = txid;
    listener->entries = listener->buf;
    listener->capacity = sizeof(listener->buf)/sizeof(requestListenerEntry);
    listener->count = 0;
    listener->proceeded = 0;
    listener->acked = 0;
    listener->notified = 0;
    requestListenersPush(listeners, listener);
    listener->ntxlistener = listeners->cur_ntxlistener;
    listener->ntxrequest = listeners->cur_ntxrequest;
    listener->ntxacked = listeners->cur_ntxacked;
    serverAssert(listener->txid == listeners->cur_txid);
    return listener;
}

/* Normally pd is swapCtx, which should not be freed untill binded listener
 * released. so we pass pdfree to listener to free it. */
void requestListenerPushEntry(requestListeners *listeners,
        requestListener *listener, redisDb *db, robj *key, requestProceed cb,
        client *c, void *pd, freefunc pdfree, void *msgs) {
    requestListenerEntry *entry;
    UNUSED(msgs);

    if (listener->count == listener->capacity) {
        size_t orig_capacity = listener->capacity;
        listener->capacity *= 2;
        if (listener->buf == listener->entries) {
            listener->entries = zcalloc(
                    listener->capacity * sizeof(requestListenerEntry));
            memcpy(listener->entries, listener->buf,
                    sizeof(requestListenerEntry) * orig_capacity);
        } else {
            listener->entries = zrealloc(listener->entries,
                    listener->capacity * sizeof(requestListenerEntry));
        }
        serverAssert(listener->capacity > listener->count);
    }

    entry = listener->entries + listener->count;

    entry->db = db;
    if (key) incrRefCount(key);
    entry->key = key;
    entry->proceed = cb;
    entry->c = c;
    entry->pd = pd;
    entry->pdfree = pdfree;
#ifdef SWAP_DEBUG
    entry->msgs = msgs;
#endif

    listener->count++;
    requestListenersLinkEntry(listeners, listener->txid);
}

void requestListenerRelease(requestListener *listener) {
    int i;
    requestListenerEntry *entry;
    if (!listener) return;
    for (i = 0; i < listener->count; i++) {
       entry = listener->entries + i; 
       if (entry->key) decrRefCount(entry->key);
       if (entry->pdfree) entry->pdfree(entry->pd);
    }
    if (listener->buf != listener->entries) zfree(listener->entries);
    zfree(listener);
}

char *requestListenerEntryDump(requestListenerEntry *entry) {
    static char repr[64];
    const char *intention = (entry->c && entry->c->cmd) ? swapIntentionName(entry->c->cmd->intention) : "<nil>";
    char *cmd = (entry->c && entry->c->cmd) ? entry->c->cmd->name : "<nil>";
    char *key = entry->key ? entry->key->ptr : "<nil>";
    snprintf(repr,sizeof(repr)-1,"(%s:%s:%s)",intention,cmd,key);
    return repr;
}

char *requestListenerDump(requestListener *listener) {
    static char repr[256];
    char *ptr = repr, *end = repr + sizeof(repr) - 1;
    
    ptr += snprintf(ptr,end-ptr,
            "txid=%ld,count=%d,proceeded=%d,notified=%d,ntxlistener=%d,entries=[",
            listener->txid,listener->count,listener->proceeded,listener->notified,
            listener->ntxlistener);

    for (int i = listener->proceeded; i < listener->count && ptr < end; i++) {
        requestListenerEntry *entry = listener->entries+i;
        ptr += snprintf(ptr,end-ptr,"%s,",requestListenerEntryDump(entry));
    }

    if (ptr < end) snprintf(ptr, end-ptr, "]");
    return repr;
}

dictType requestListenersDictType = {
    dictSdsHash,                    /* hash function */
    NULL,                           /* key dup */
    NULL,                           /* val dup */
    dictSdsKeyCompare,              /* key compare */
    dictSdsDestructor,              /* key destructor */
    NULL,                           /* val destructor */
    NULL                            /* allow to expand */
};

static requestListeners *requestListenersCreate(int level, redisDb *db,
        robj *key, requestListeners *parent) {
    requestListeners *listeners;

    listeners = zmalloc(sizeof(requestListeners));
    listeners->listeners = listCreate();
    listeners->nlistener = 0;
    listeners->parent = parent;
    listeners->level = level;
    listeners->cur_txid = -1;
    listeners->cur_ntxlistener = 0;
    listeners->cur_ntxrequest = 0;
    listeners->cur_ntxacked = 0;

    switch (level) {
    case REQUEST_LEVEL_SVR:
        listeners->svr.dbnum = server.dbnum;
        listeners->svr.dbs = zmalloc(server.dbnum*sizeof(requestListeners));
        break;
    case REQUEST_LEVEL_DB:
        serverAssert(db);
        listeners->db.db = db;
        listeners->db.keys = dictCreate(&requestListenersDictType, NULL);
        break;
    case REQUEST_LEVEL_KEY:
        serverAssert(key);
        serverAssert(parent->level == REQUEST_LEVEL_DB);
        incrRefCount(key);
        listeners->key.key = key;
        dictAdd(parent->db.keys,sdsdup(key->ptr),listeners);
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
    listeners->listeners = NULL;

    switch (listeners->level) {
    case REQUEST_LEVEL_SVR:
        zfree(listeners->svr.dbs);
        break;
    case REQUEST_LEVEL_DB:
        dictRelease(listeners->db.keys);
        break;
    case REQUEST_LEVEL_KEY:
        serverAssert(listeners->parent->level == REQUEST_LEVEL_DB);
        dictDelete(listeners->parent->db.keys,listeners->key.key->ptr);
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
    char *key;

    switch (listeners->level) {
    case REQUEST_LEVEL_SVR:
        key = "<svr>";
        break;
    case REQUEST_LEVEL_DB:
        key = "<db>";
        break;
    case REQUEST_LEVEL_KEY:
        key = listeners->key.key->ptr;
        break;
    default:
        key = "?";
        break;
    }
    result = sdscatprintf(result,"(level=%s,len=%ld,key=%s):",
            requestLevelName(listeners->level),
            listLength(listeners->listeners), key);

    result = sdscat(result, "[");
    listRewind(listeners->listeners,&li);
    while ((ln = listNext(&li))) {
        requestListener *listener = listNodeValue(ln);
        if (ln != listFirst(listeners->listeners)) result = sdscat(result,",");
        result = sdscat(result,requestListenerDump(listener));
    }
    result = sdscat(result, "]");
    return result;
}


requestListeners *serverRequestListenersCreate() {
    int i;
    requestListeners *s = requestListenersCreate(
            REQUEST_LEVEL_SVR,NULL,NULL,NULL);

    for (i = 0; i < server.dbnum; i++) {
        redisDb *db = server.db + i;
        s->svr.dbs[i] = requestListenersCreate(
                REQUEST_LEVEL_DB,db,NULL,s);
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

    key_listeners = dictFetchValue(db_listeners->db.keys,key->ptr);
    if (key_listeners == NULL) {
        if (create) {
            key_listeners = requestListenersCreate(
                    REQUEST_LEVEL_KEY,db,key,db_listeners);
        }
    }

    return key_listeners;
}

static inline int proceed(requestListeners *listeners,
        requestListener *listener) {
    int proceeded = 0;

    if (listener->proceeded < listener->count) {
        requestListenerEntry *entry = listener->entries+listener->proceeded;

        DEBUG_MSGS_APPEND(entry->msgs,"wait-proceed","entry=%s",
                requestListenerEntryDump(entry));
        listener->proceeded++;
        entry->proceed(listeners,entry->db,entry->key,
                entry->c,entry->pd);
        proceeded++;
    }

    return proceeded;
}

static inline requestListener *requestListenersLast(requestListeners *listeners) {
    listNode *ln = listLast(listeners->listeners);
    return ln ? listNodeValue(ln) : NULL;
}

/* request of txid wait on listeners would be blocked from proceeding? */
static int listenersWaitWouldBlock(int64_t txid, requestListeners *listeners) {
    /* listener, request ack that already registered to txid. */
    int ntxlistener, ntxrequest, ntxacked;

    if (listeners->cur_txid == txid) {
        ntxlistener = listeners->cur_ntxlistener;
        ntxrequest = listeners->cur_ntxrequest;
        ntxacked = listeners->cur_ntxacked;
    } else {
        ntxlistener = 0, ntxrequest = 0, ntxacked = 0;
    }

    /* There are other listener of different tx blocking txid, or
     * there are pending(not acked) request of the same tx blocking txid
     * from proceeding. */
    return listeners->nlistener > ntxlistener || ntxrequest > ntxacked;
}

int requestLockWouldBlock(int64_t txid, redisDb *db, robj *key) {
    requestListeners *listeners = requestBindListeners(db,key,0);
    if (listeners == NULL) return 0;
    return listenersWaitWouldBlock(txid, listeners);
}

requestListener *requestBindListener(int64_t txid,
        requestListeners *listeners) {
    requestListener *last = requestListenersLast(listeners);
    if (last == NULL || last->txid != txid) {
        last = requestListenerCreate(listeners,txid);
    }
    return last;
}

/* Restrictions:
 * - requestGetIOAndLock for one txid MUST next to each other.
 * - requestGetIOAndLock for one txid MUST not trigger requestGetIOAndLock/requestReleaseIO for
 *   other txid in between. */
void requestGetIOAndLock(int64_t txid, redisDb *db, robj *key, requestProceed cb,
        client *c, void *pd, freefunc pdfree, void *msgs) {
    int blocking;
    requestListeners *listeners;
    requestListener *listener;

    listeners = requestBindListeners(db,key,1);
    blocking = listenersWaitWouldBlock(txid,listeners);

    listener = requestBindListener(txid,listeners);
    requestListenerPushEntry(listeners,listener,db,key,cb,c,pd,pdfree,msgs);

#ifdef SWAP_DEBUG
    sds dump = requestListenersDump(listeners);
    DEBUG_MSGS_APPEND(msgs,"wait-bind","listener = %s", dump);
    sdsfree(dump);
#endif

    /* Proceed right away if request key is not blocking, otherwise
     * proceed is defered. */
    if (!blocking) proceed(listeners,listener);
}

int proceedChain(requestListeners *listeners, requestListener *listener) {
    int nchilds, proceeded = 0;
    requestListeners *parent;
    requestListener *first;
    int64_t txid = listener->txid;

    while (1) {
        parent = listeners->parent;

        if (listener != NULL) {
            if (proceed(listeners,listener)) {
                proceeded++;
                break;
            }
        }

        if (parent == NULL) break;

        first = requestListenersPeek(parent);
        if (first) {
            nchilds = parent->nlistener-(int)listLength(parent->listeners);
        }

        /* Proceed upwards if:
         * - parent is empty.
         * - all childs and parent are in the same tx and there no preeceding
         *   un-acked requests. */ 
        if (first == NULL || (first->txid == txid &&
                    first->ntxlistener > nchilds &&
                    first->ntxacked == first->ntxrequest)) {
            listeners = parent;
            listener = first;
        } else {
            break;
        }
    }

    return proceeded;
}

static inline void requestListenersAcked(requestListeners *listeners, int64_t txid) {
    requestListeners *parent;
    requestListener *listener;

    while (listeners) {
        parent = listeners->parent;

        if (listeners->cur_txid == txid)
            listeners->cur_ntxacked++;

        listener = requestListenersPeek(listeners);
        if (listener != NULL && listener->txid == txid) {
            listener->ntxacked++;
        }

        listeners = parent;
    }
}

int requestReleaseIO(void *listeners_) {
    requestListeners *listeners = listeners_;
    requestListener *current = requestListenersPeek(listeners);
    current->acked++;
    requestListenersAcked(listeners,current->txid);
    proceedChain(listeners,current);
    return 0;
}

int requestReleaseLock(void *listeners_) {
    requestListeners *listeners = listeners_, *parent;
    requestListener *current, *next;

    current = requestListenersPeek(listeners);

#ifdef SWAP_DEBUG
    sds dump = requestListenersDump(listeners);
    requestListenerEntry *entry = current->entries + current->notified;
    DEBUG_MSGS_APPEND(entry->msgs,"wait-unbind","listener=%s", dump);
    sdsfree(dump);
#endif

    /* Must ack before notify. */ 
    serverAssert(current->acked > current->notified);
    serverAssert(current->count > current->notified);
    current->notified++;
    if (current->notified < current->count) {
        /* wait untill all notified for reentrant listener. */
        return 0;
    } else {
        requestListenersPop(listeners);
        requestListenerRelease(current);
    }

    while (listeners) {
        if (listLength(listeners->listeners)) {
            next = requestListenersPeek(listeners);
            proceedChain(listeners,next);
            break;
        }

        parent = listeners->parent;
        if (listeners->level == REQUEST_LEVEL_KEY) {
            /* Only key level listeners releases, DB or server level
             * key released only when server exit. */
            requestListenersRelease(listeners);
        }

        if (parent == NULL) {
            listeners = NULL;
            break;
        }

        /* Go upwards if all sibling listeners notified. */
        if (parent->nlistener > (int)listLength(parent->listeners)) {
            listeners = NULL; 
            break;
        }

        listeners = parent;
    }

    return 0;
}

#ifdef REDIS_TEST

static int blocked;

int proceedNotifyLater(void *listeners, redisDb *db, robj *key, client *c, void *pd_) {
    UNUSED(db), UNUSED(key), UNUSED(c);
    void **pd = pd_;
    *pd = listeners;
    blocked--;
    requestReleaseIO(listeners);
    return 0;
}

#define wait_init_suite() do {  \
    if (server.hz != 10) {  \
        server.hz = 10; \
        server.dbnum = 4;   \
        server.db = zmalloc(sizeof(redisDb)*server.dbnum);  \
        for (int i = 0; i < server.dbnum; i++) server.db[i].id = i; \
        server.request_listeners = serverRequestListenersCreate(); \
    }   \
} while (0)

int swapWaitTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;
    redisDb *db, *db2;
    robj *key1, *key2, *key3;
    void *handle1, *handle2, *handle3, *handledb, *handledb2, *handlesvr;
    int64_t txid = 0;

    wait_init_suite();

    TEST("wait: init") {
        db = server.db, db2 = server.db+1;
        key1 = createStringObject("key-1",5);
        key2 = createStringObject("key-2",5);
        key3 = createStringObject("key-3",5);
    }

   TEST("wait: parallel key") {
       handle1 = NULL, handle2 = NULL, handle3 = NULL, handlesvr = NULL;
       requestGetIOAndLock(txid++,db,key1,proceedNotifyLater,NULL,&handle1,NULL,NULL), blocked++;
       requestGetIOAndLock(txid++,db,key2,proceedNotifyLater,NULL,&handle2,NULL,NULL), blocked++;
       requestGetIOAndLock(txid++,db,key3,proceedNotifyLater,NULL,&handle3,NULL,NULL), blocked++;
       test_assert(!blocked);
       test_assert(requestLockWouldBlock(txid++,db,key1));
       test_assert(requestLockWouldBlock(txid++,db,key2));
       test_assert(requestLockWouldBlock(txid++,db,key3));
       test_assert(requestLockWouldBlock(txid++,db,NULL));
       requestReleaseLock(handle1);
       test_assert(!requestLockWouldBlock(txid++,db,key1));
       requestReleaseLock(handle2);
       test_assert(!requestLockWouldBlock(txid++,db,key2));
       requestReleaseLock(handle3);
       test_assert(!requestLockWouldBlock(txid++,db,key3));
       test_assert(!requestLockWouldBlock(txid++,NULL,NULL));
   } 

   TEST("wait: pipelined key") {
       int i;
       for (i = 0; i < 3; i++) {
           blocked++;
           requestGetIOAndLock(txid++,db,key1,proceedNotifyLater,NULL,&handle1,NULL,NULL);
       }
       test_assert(requestLockWouldBlock(txid++,db,key1));
       /* first one proceeded, others blocked */
       test_assert(blocked == 2);
       for (i = 0; i < 2; i++) {
           requestReleaseLock(handle1);
           test_assert(requestLockWouldBlock(txid++,db,key1));
       }
       test_assert(blocked == 0);
       requestReleaseLock(handle1);
       test_assert(!requestLockWouldBlock(txid++,db,key1));
   }

   TEST("wait: parallel db") {
       requestGetIOAndLock(txid++,db,NULL,proceedNotifyLater,NULL,&handledb,NULL,NULL), blocked++;
       requestGetIOAndLock(txid++,db2,NULL,proceedNotifyLater,NULL,&handledb2,NULL,NULL), blocked++;
       test_assert(!blocked);
       test_assert(requestLockWouldBlock(txid++,db,NULL));
       test_assert(requestLockWouldBlock(txid++,db2,NULL));
       requestReleaseLock(handledb);
       requestReleaseLock(handledb2);
       test_assert(!requestLockWouldBlock(txid++,db,NULL));
       test_assert(!requestLockWouldBlock(txid++,db2,NULL));
   }

    TEST("wait: mixed parallel-key/db/parallel-key") {
        handle1 = NULL, handle2 = NULL, handle3 = NULL, handledb = NULL;
        requestGetIOAndLock(txid++,db,key1,proceedNotifyLater,NULL,&handle1,NULL,NULL),blocked++;
        requestGetIOAndLock(txid++,db,key2,proceedNotifyLater,NULL,&handle2,NULL,NULL),blocked++;
        requestGetIOAndLock(txid++,db,NULL,proceedNotifyLater,NULL,&handledb,NULL,NULL),blocked++;
        requestGetIOAndLock(txid++,db,key3,proceedNotifyLater,NULL,&handle3,NULL,NULL),blocked++;
        /* key1/key2 proceeded, db/key3 blocked */
        test_assert(requestLockWouldBlock(txid++,db,NULL));
        test_assert(blocked == 2);
        /* key1/key2 notify */
        requestReleaseLock(handle1);
        test_assert(requestLockWouldBlock(txid++,db,NULL));
        requestReleaseLock(handle2);
        test_assert(requestLockWouldBlock(txid++,db,NULL));
        /* db proceeded, key3 still blocked. */
        test_assert(blocked == 1);
        test_assert(handle3 == NULL);
        /* db notified, key3 proceeds but still blocked */
        requestReleaseLock(handledb);
        test_assert(!blocked);
        test_assert(requestLockWouldBlock(txid++,db,NULL));
        /* db3 proceed, noting would block */
        requestReleaseLock(handle3);
        test_assert(!requestLockWouldBlock(txid++,db,NULL));
    }

    TEST("wait: mixed parallel-key/server/parallel-key") {
        handle1 = NULL, handle2 = NULL, handle3 = NULL, handlesvr = NULL;
        requestGetIOAndLock(txid++,db,key1,proceedNotifyLater,NULL,&handle1,NULL,NULL),blocked++;
        requestGetIOAndLock(txid++,db,key2,proceedNotifyLater,NULL,&handle2,NULL,NULL),blocked++;
        requestGetIOAndLock(txid++,NULL,NULL,proceedNotifyLater,NULL,&handlesvr,NULL,NULL),blocked++;
        requestGetIOAndLock(txid++,db,key3,proceedNotifyLater,NULL,&handle3,NULL,NULL),blocked++;
        /* key1/key2 proceeded, svr/key3 blocked */
        test_assert(requestLockWouldBlock(txid++,NULL,NULL));
        test_assert(requestLockWouldBlock(txid++,db,NULL));
        test_assert(blocked == 2);
        /* key1/key2 notify */
        requestReleaseLock(handle1);
        test_assert(requestLockWouldBlock(txid++,NULL,NULL));
        requestReleaseLock(handle2);
        test_assert(requestLockWouldBlock(txid++,NULL,NULL));
        /* svr proceeded, key3 still blocked. */
        test_assert(blocked == 1);
        test_assert(handle3 == NULL);
        /* svr notified, db3 proceeds but still would block */
        requestReleaseLock(handlesvr);
        test_assert(!blocked);
        test_assert(requestLockWouldBlock(txid++,NULL,NULL));
        /* db3 proceed, noting would block */
        requestReleaseLock(handle3);
        test_assert(!requestLockWouldBlock(txid++,NULL,NULL));
    }

    return error;
}


static int proceeded = 0;
int proceededCounter(void *listeners, redisDb *db, robj *key, client *c, void *pd_) {
    UNUSED(db), UNUSED(key), UNUSED(c);
    void **pd = pd_;
    *pd = listeners;
    proceeded++;
    requestReleaseIO(listeners);
    return 0;
}

#define reentrant_case_reset() do { \
    proceeded = 0; \
    handle1 = NULL, handle2 = NULL, handle3 = NULL, handle4 = NULL; \
    handle5 = NULL, handle6 = NULL, handle7 = NULL, handle8 = NULL;\
} while (0)

int swapWaitReentrantTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;
    redisDb *db, *db2;
    robj *key1, *key2;
    void *handle1, *handle2, *handle3, *handle4, *handle5, *handle6,
         *handle7, *handle8;

    wait_init_suite();

    TEST("wait-reentrant: init") {
        db = server.db, db2 = server.db+1;
        key1 = createStringObject("key-1",5);
        key2 = createStringObject("key-2",5);
    }

   TEST("wait-reentrant: key (without prceeding listener)") {
       test_assert(!requestLockWouldBlock(10,db,key1));
       requestGetIOAndLock(10,db,key1,proceededCounter,NULL,&handle1,NULL,NULL);
       test_assert(proceeded == 1);
       test_assert(!requestLockWouldBlock(10,db,key1));
       requestGetIOAndLock(10,db,key1,proceededCounter,NULL,&handle2,NULL,NULL);
       test_assert(proceeded == 2);
       test_assert(handle1 != NULL && handle1 == handle2);
       test_assert(requestLockWouldBlock(11,db,key1));
       requestGetIOAndLock(11,db,key1,proceededCounter,NULL,&handle3,NULL,NULL);
       requestReleaseLock(handle1);
       requestReleaseLock(handle2);
       test_assert(proceeded == 3);
       test_assert(handle1 == handle3);
       test_assert(requestBindListeners(db,key1,0) != NULL);
       requestReleaseLock(handle3);
       test_assert(requestBindListeners(db,key1,0) == NULL);
       test_assert(proceeded == 3);
       reentrant_case_reset();
   }

   TEST("wait-reentrant: key (with prceeding listener)") {
       test_assert(!requestLockWouldBlock(20,db,key1));
       requestGetIOAndLock(20,db,key1,proceededCounter,NULL,&handle1,NULL,NULL);
       test_assert(handle1 != NULL);
       test_assert(proceeded == 1);
       test_assert(requestLockWouldBlock(21,db,key1));
       requestGetIOAndLock(21,db,key1,proceededCounter,NULL,&handle2,NULL,NULL);
       test_assert(proceeded == 1);
       test_assert(requestLockWouldBlock(21,db,key1));
       requestGetIOAndLock(21,db,key1,proceededCounter,NULL,&handle3,NULL,NULL);
       test_assert(proceeded == 1);
       test_assert(requestLockWouldBlock(22,db,key1));
       requestGetIOAndLock(22,db,key1,proceededCounter,NULL,&handle4,NULL,NULL);
       test_assert(proceeded == 1);
       requestReleaseLock(handle1);
       test_assert(proceeded == 3);
       test_assert(handle1 == handle2);
       test_assert(handle1 == handle3);
       requestReleaseLock(handle2);
       test_assert(proceeded == 3);
       requestReleaseLock(handle3);
       test_assert(proceeded == 4);
       test_assert(handle1 == handle4);
       test_assert(requestBindListeners(db,key1,0) != NULL);
       requestReleaseLock(handle4);
       test_assert(requestBindListeners(db,key1,0) == NULL);
       reentrant_case_reset();
   }

   TEST("wait-reentrant: db listener") {
       requestGetIOAndLock(30,db,NULL,proceededCounter,NULL,&handle1,NULL,NULL);
       test_assert(proceeded == 1);
       test_assert(handle1 != NULL);
       requestGetIOAndLock(30,db,NULL,proceededCounter,NULL,&handle2,NULL,NULL);
       test_assert(proceeded == 2);
       test_assert(handle1 == handle2);
       requestGetIOAndLock(31,db,NULL,proceededCounter,NULL,&handle3,NULL,NULL);
       test_assert(proceeded == 2);
       requestGetIOAndLock(31,db2,NULL,proceededCounter,NULL,&handle4,NULL,NULL);
       test_assert(proceeded == 3);
       test_assert(handle4 != NULL && handle1 != handle4);
       requestReleaseLock(handle1);
       requestReleaseLock(handle2);
       test_assert(proceeded == 4);
       test_assert(handle1 == handle3);
       requestReleaseLock(handle3);
       requestReleaseLock(handle4);
       test_assert(proceeded == 4);
       reentrant_case_reset();
   }

   TEST("wait-reentrant: svr listener") {
       requestGetIOAndLock(40,NULL,NULL,proceededCounter,NULL,&handle1,NULL,NULL);
       test_assert(proceeded == 1);
       test_assert(handle1 != NULL);
       requestGetIOAndLock(40,NULL,NULL,proceededCounter,NULL,&handle2,NULL,NULL);
       test_assert(proceeded == 2);
       test_assert(handle1 == handle2);
       requestGetIOAndLock(41,NULL,NULL,proceededCounter,NULL,&handle3,NULL,NULL);
       test_assert(proceeded == 2);
       requestGetIOAndLock(41,NULL,NULL,proceededCounter,NULL,&handle4,NULL,NULL);
       test_assert(proceeded == 2);
       requestReleaseLock(handle1);
       test_assert(proceeded == 2);
       requestReleaseLock(handle2);
       test_assert(proceeded == 4);
       test_assert(handle1 == handle3);
       test_assert(handle1 == handle4);
       requestReleaseLock(handle3);
       test_assert(proceeded == 4);
       requestReleaseLock(handle4);
       test_assert(proceeded == 4);
       reentrant_case_reset();
   }

   TEST("wait-reentrant: db and svr listener") {
       requestGetIOAndLock(50,db,NULL,proceededCounter,NULL,&handle1,NULL,NULL);
       test_assert(proceeded == 1);
       test_assert(handle1 != NULL);
       requestGetIOAndLock(51,db,NULL,proceededCounter,NULL,&handle2,NULL,NULL);
       test_assert(proceeded == 1);
       test_assert(handle2 == NULL);
       requestGetIOAndLock(51,db2,NULL,proceededCounter,NULL,&handle3,NULL,NULL);
       test_assert(proceeded == 2);
       test_assert(handle3 != NULL);
       requestGetIOAndLock(51,NULL,NULL,proceededCounter,NULL,&handle4,NULL,NULL);
       test_assert(handle4 == NULL);
       test_assert(proceeded == 2);
       requestReleaseLock(handle1);
       test_assert(handle1 == handle2);
       test_assert(handle4 != NULL);
       test_assert(proceeded == 4);
       requestReleaseLock(handle2);
       requestReleaseLock(handle3);
       requestReleaseLock(handle4);
       test_assert(proceeded == 4);
       reentrant_case_reset();
   }

   TEST("wait-reentrant: multi-level (with key & db listener)") {
       requestGetIOAndLock(60,db,key1,proceededCounter,NULL,&handle1,NULL,NULL);
       test_assert(proceeded == 1);
       test_assert(handle1 != NULL);
       requestGetIOAndLock(61,db,key1,proceededCounter,NULL,&handle2,NULL,NULL);
       test_assert(proceeded == 1);
       requestGetIOAndLock(61,db,key2,proceededCounter,NULL,&handle3,NULL,NULL);
       test_assert(proceeded == 2);
       test_assert(handle3 != NULL && handle1 != handle3);
       requestGetIOAndLock(61,db,key1,proceededCounter,NULL,&handle4,NULL,NULL);
       test_assert(proceeded == 2);
       requestGetIOAndLock(61,db,NULL,proceededCounter,NULL,&handle5,NULL,NULL);
       test_assert(proceeded == 2);
       requestGetIOAndLock(61,db,key1,proceededCounter,NULL,&handle6,NULL,NULL);
       test_assert(proceeded == 2);
       requestGetIOAndLock(62,db,key2,proceededCounter,NULL,&handle7,NULL,NULL);
       test_assert(proceeded == 2);
       requestReleaseLock(handle1);
       test_assert(proceeded == 6);
       test_assert(handle1 == handle2);
       test_assert(handle1 == handle4);
       test_assert(handle5 != NULL && handle1 != handle5 && handle3 != handle5);
       test_assert(handle6 == handle5);
       requestReleaseLock(handle2);
       requestReleaseLock(handle3);
       requestReleaseLock(handle4);
       requestReleaseLock(handle5);
       test_assert(proceeded == 6);
       requestReleaseLock(handle6);
       test_assert(proceeded == 7);
       test_assert(handle7 == handle5);
       requestReleaseLock(handle7);
       test_assert(proceeded == 7);
       reentrant_case_reset();
   }

   TEST("wait-reentrant: multi-level (with key & svr listener)") {
       requestGetIOAndLock(70,db,key1,proceededCounter,NULL,&handle1,NULL,NULL);
       test_assert(proceeded == 1 && handle1 != NULL);
       requestGetIOAndLock(70,db,key2,proceededCounter,NULL,&handle2,NULL,NULL);
       test_assert(proceeded == 2 && handle2 != NULL);
       test_assert(handle1 != handle2);
       requestGetIOAndLock(71,db,key1,proceededCounter,NULL,&handle3,NULL,NULL);
       test_assert(proceeded == 2);
       requestGetIOAndLock(71,db,key2,proceededCounter,NULL,&handle4,NULL,NULL);
       test_assert(proceeded == 2);
       requestGetIOAndLock(71,NULL,NULL,proceededCounter,NULL,&handle5,NULL,NULL);
       test_assert(proceeded == 2 && handle5 == NULL);
       requestGetIOAndLock(72,NULL,NULL,proceededCounter,NULL,&handle6,NULL,NULL);
       test_assert(proceeded == 2);
       requestReleaseLock(handle1);
       test_assert(proceeded == 3);
       test_assert(handle3 != NULL && handle3 == handle1);
       requestReleaseLock(handle2);
       test_assert(proceeded == 5);
       test_assert(handle4 != NULL && handle4 == handle2);
       test_assert(handle5 != NULL && handle5 != handle4 && handle5 != handle3);
       requestReleaseLock(handle3);
       requestReleaseLock(handle4);
       test_assert(proceeded == 5);
       requestReleaseLock(handle5);
       test_assert(proceeded == 6);
       test_assert(handle5 == handle6);
       requestReleaseLock(handle6);
       test_assert(proceeded == 6);
       reentrant_case_reset();
   }

   TEST("wait-reentrant: multi-level (with key & db & svr listener)") {
       requestGetIOAndLock(80,db,key2,proceededCounter,NULL,&handle1,NULL,NULL);
       test_assert(handle1 != NULL);
       requestGetIOAndLock(80,db2,NULL,proceededCounter,NULL,&handle2,NULL,NULL);
       test_assert(handle2 != NULL);
       test_assert(proceeded == 2);
       requestGetIOAndLock(81,db,key1,proceededCounter,NULL,&handle3,NULL,NULL);
       test_assert(handle3 != NULL);
       test_assert(handle1 != handle2 && handle1 != handle3);
       test_assert(proceeded == 3);
       requestGetIOAndLock(81,db,key2,proceededCounter,NULL,&handle4,NULL,NULL);
       test_assert(proceeded == 3);
       requestGetIOAndLock(81,db,NULL,proceededCounter,NULL,&handle5,NULL,NULL);
       test_assert(proceeded == 3);
       requestGetIOAndLock(81,db2,NULL,proceededCounter,NULL,&handle6,NULL,NULL);
       test_assert(proceeded == 3);
       requestGetIOAndLock(81,NULL,NULL,proceededCounter,NULL,&handle7,NULL,NULL);
       test_assert(proceeded == 3);
       requestGetIOAndLock(82,NULL,NULL,proceededCounter,NULL,&handle8,NULL,NULL);
       test_assert(proceeded == 3);
       requestReleaseLock(handle1);
       test_assert(proceeded == 5);
       test_assert(handle4 != NULL && handle4 == handle1);
       test_assert(handle5 != NULL && handle5 != handle4);
       requestReleaseLock(handle2);
       test_assert(proceeded == 7);
       test_assert(handle6 == handle2);
       test_assert(handle7 != handle6);
       requestReleaseLock(handle3);
       requestReleaseLock(handle4);
       requestReleaseLock(handle5);
       requestReleaseLock(handle6);
       test_assert(proceeded == 7);
       requestReleaseLock(handle7);
       test_assert(proceeded == 8);
       test_assert(handle8 == handle7);
       requestReleaseLock(handle8);
       test_assert(proceeded == 8);
       reentrant_case_reset();
   }

   TEST("wait-reentrant: expand entries buf") {
       int i, count = DEFAULT_REQUEST_LISTENER_REENTRANT_SIZE*4;
       for (i = 0; i < count; i++) {
           requestGetIOAndLock(90,db,key1,proceededCounter,NULL,&handle1,NULL,NULL);
       }
       test_assert(proceeded == count);
       for (i = 0; i < count; i++) {
           requestReleaseLock(handle1);
       }
       test_assert(proceeded == count);
       reentrant_case_reset();
   }

    return error;
}

int proceedWithoutAck(void *listeners, redisDb *db, robj *key, client *c, void *pd_) {
    UNUSED(db), UNUSED(key), UNUSED(c);
    void **pd = pd_;
    *pd = listeners;
    proceeded++;
    return 0;
}

int proceedRightaway(void *listeners, redisDb *db, robj *key, client *c, void *pd_) {
    UNUSED(db), UNUSED(key), UNUSED(c);
    void **pd = pd_;
    *pd = listeners;
    proceeded++;
    requestReleaseIO(listeners);
    requestReleaseLock(listeners);
    return 0;
}

#define ack_case_reset() do { \
    proceeded = 0; \
    handle1 = NULL, handle2 = NULL, handle3 = NULL, handle4 = NULL; \
    handle5 = NULL, handle6 = NULL, handle7 = NULL, handle8 = NULL; \
    handle9 = NULL, handle10 = NULL; \
} while (0)

int swapWaitAckTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;
    redisDb *db, *db2;
    robj *key1, *key2;
    void *handle1, *handle2, *handle3, *handle4, *handle5, *handle6,
         *handle7, *handle8, *handle9, *handle10;

    wait_init_suite();

    TEST("wait-ack: init") {
        db = server.db, db2 = server.db+1;
        key1 = createStringObject("key-1",5);
        key2 = createStringObject("key-2",5);
        ack_case_reset();
        UNUSED(db2);
    }

    TEST("wait-ack: multi-level (db & svr)") {
        test_assert(requestBindListeners(db,NULL,0) != NULL);
        test_assert(!requestLockWouldBlock(10,db,NULL));
        requestGetIOAndLock(10,db,NULL,proceedWithoutAck,NULL,&handle1,NULL,NULL);
        test_assert(handle1 != NULL && proceeded == 1);
        test_assert(requestLockWouldBlock(10,db,NULL));
        requestGetIOAndLock(10,db,NULL,proceedWithoutAck,NULL,&handle2,NULL,NULL);
        test_assert(handle2 == NULL && proceeded == 1);
        requestGetIOAndLock(10,db,key1,proceedWithoutAck,NULL,&handle3,NULL,NULL);
        test_assert(handle3 == NULL && proceeded == 1);
        requestGetIOAndLock(10,db2,NULL,proceedWithoutAck,NULL,&handle4,NULL,NULL);
        test_assert(handle4 != NULL && proceeded == 2);
        requestGetIOAndLock(10,db2,NULL,proceedWithoutAck,NULL,&handle5,NULL,NULL);
        test_assert(handle5 == NULL && proceeded == 2);
        requestGetIOAndLock(10,NULL,NULL,proceedWithoutAck,NULL,&handle6,NULL,NULL);
        test_assert(handle6 == NULL && proceeded == 2);
        requestGetIOAndLock(10,db2,key2,proceedWithoutAck,NULL,&handle7,NULL,NULL);
        test_assert(handle7 == NULL && proceeded == 2);
        requestGetIOAndLock(11,db,key1,proceedWithoutAck,NULL,&handle8,NULL,NULL);
        test_assert(handle8 == NULL && proceeded == 2);

        requestReleaseIO(handle1);
        test_assert(handle2 == handle1 && proceeded == 3);
        requestReleaseIO(handle4);
        test_assert(handle5 == handle4 && proceeded == 4);
        requestReleaseIO(handle5);
        test_assert(handle6 == NULL && proceeded == 4);
        requestReleaseIO(handle2);
        test_assert(handle3 == handle1 && proceeded == 5);
        requestReleaseIO(handle3);
        test_assert(handle6 != handle1 && proceeded == 6);
        requestReleaseIO(handle6);
        test_assert(handle7 == handle6 && proceeded == 7);
        requestReleaseIO(handle7);
        test_assert(handle8 == NULL && proceeded == 7);

        requestReleaseLock(handle1), requestReleaseLock(handle2), requestReleaseLock(handle3),
            requestReleaseLock(handle4), requestReleaseLock(handle5), requestReleaseLock(handle6),
            requestReleaseLock(handle7);

        test_assert(handle8 == handle6 && proceeded == 8);
        requestReleaseIO(handle8);
        requestReleaseLock(handle8);

        ack_case_reset();
    }

    TEST("wait-ack: multi-level (key & svr)") {
        requestGetIOAndLock(20,db,key1,proceedWithoutAck,NULL,&handle1,NULL,NULL);
        test_assert(handle1 != NULL && proceeded == 1);
        requestGetIOAndLock(20,db,key1,proceedWithoutAck,NULL,&handle2,NULL,NULL);
        test_assert(handle2 == NULL && proceeded == 1);
        requestGetIOAndLock(20,db,key2,proceedWithoutAck,NULL,&handle3,NULL,NULL);
        test_assert(handle3 != NULL && proceeded == 2);
        requestGetIOAndLock(20,db,key2,proceedWithoutAck,NULL,&handle4,NULL,NULL);
        test_assert(handle4 == NULL && proceeded == 2);
        requestGetIOAndLock(20,NULL,NULL,proceedWithoutAck,NULL,&handle5,NULL,NULL);
        test_assert(handle5 == NULL && proceeded == 2);
        requestReleaseIO(handle3);
        test_assert(handle4 != NULL && proceeded == 3);
        requestReleaseIO(handle1);
        test_assert(handle2 != NULL && handle5 == NULL && proceeded == 4);
        requestReleaseIO(handle2);
        test_assert(handle5 == NULL && proceeded == 4);
        requestReleaseIO(handle4);
        test_assert(handle5 != NULL && proceeded == 5);
        requestReleaseIO(handle5);
        requestReleaseLock(handle1), requestReleaseLock(handle2), requestReleaseLock(handle3),
            requestReleaseLock(handle4), requestReleaseLock(handle5);
        test_assert(requestBindListeners(db,key1,0) == NULL);
        ack_case_reset();
    }

    TEST("wait-ack: multi-level (key & db & svr)") {
        requestGetIOAndLock(30,db,key1,proceedWithoutAck,NULL,&handle1,NULL,NULL);
        requestGetIOAndLock(30,db,key2,proceedWithoutAck,NULL,&handle2,NULL,NULL);
        test_assert(handle1 != handle2 && handle1 && handle2 && proceeded == 2);
        requestGetIOAndLock(30,db2,key1,proceedWithoutAck,NULL,&handle3,NULL,NULL);
        requestGetIOAndLock(30,db2,key2,proceedWithoutAck,NULL,&handle4,NULL,NULL);
        test_assert(handle3 != handle4 && handle3 && handle4 && proceeded == 4);
        requestGetIOAndLock(30,db,NULL,proceedWithoutAck,NULL,&handle5,NULL,NULL);
        test_assert(handle5 == NULL && proceeded == 4);
        requestGetIOAndLock(30,NULL,NULL,proceedWithoutAck,NULL,&handle6,NULL,NULL);
        test_assert(handle6 == NULL && proceeded == 4);
        requestGetIOAndLock(30,db,key1,proceedWithoutAck,NULL,&handle7,NULL,NULL);
        test_assert(handle6 == NULL && proceeded == 4);
        
        requestReleaseIO(handle4), requestReleaseIO(handle3), requestReleaseIO(handle2), requestReleaseIO(handle1);
        test_assert(handle5 != NULL && proceeded == 5);
        requestReleaseIO(handle5);
        test_assert(handle6 != NULL && handle6 != handle5 && proceeded == 6);
        requestReleaseIO(handle6);

        requestReleaseLock(handle1), requestReleaseLock(handle2), requestReleaseLock(handle3),
            requestReleaseLock(handle4), requestReleaseLock(handle5), requestReleaseLock(handle6);
        requestReleaseIO(handle7);
        requestReleaseLock(handle7);

        ack_case_reset();
    }

    TEST("wait-ack: proceed ack disorder") {
        test_assert(!requestLockWouldBlock(40,db,key1));
        requestGetIOAndLock(40,db,key1,proceedWithoutAck,NULL,&handle1,NULL,NULL);
        test_assert(handle1 != NULL && proceeded == 1);
        test_assert(requestLockWouldBlock(40,db,key1));
        requestGetIOAndLock(40,db,key1,proceedWithoutAck,NULL,&handle2,NULL,NULL);
        test_assert(handle2 == NULL && proceeded == 1);
        requestReleaseIO(handle1);
        test_assert(handle2 != NULL && proceeded == 2);
        test_assert(requestLockWouldBlock(41,db,key1));
        requestReleaseIO(handle2);
        test_assert(proceeded == 2);
        test_assert(requestLockWouldBlock(41,db,key1));
        requestReleaseLock(handle1);
        test_assert(requestLockWouldBlock(41,db,key1));
        requestGetIOAndLock(41,db,key1,proceedWithoutAck,NULL,&handle3,NULL,NULL);
        test_assert(handle3 == NULL && proceeded == 2);
        /* proceed iff previous tx finished. */
        requestReleaseLock(handle2);
        test_assert(handle3 != NULL && proceeded == 3);
        requestGetIOAndLock(41,db,key1,proceedWithoutAck,NULL,&handle4,NULL,NULL);
        test_assert(handle4 == NULL && proceeded == 3);
        requestGetIOAndLock(41,db,key2,proceedWithoutAck,NULL,&handle5,NULL,NULL);
        test_assert(handle5 != NULL && proceeded == 4);
        requestGetIOAndLock(41,db,NULL,proceedWithoutAck,NULL,&handle6,NULL,NULL);
        test_assert(handle6 == NULL && proceeded == 4);
        requestGetIOAndLock(41,db,key1,proceedWithoutAck,NULL,&handle7,NULL,NULL);
        test_assert(handle7 == NULL && proceeded == 4);
        requestGetIOAndLock(42,db,key2,proceedWithoutAck,NULL,&handle8,NULL,NULL);
        test_assert(handle8 == NULL && proceeded == 4);

        requestReleaseIO(handle3);
        test_assert(handle4 != NULL && handle6 == NULL && proceeded == 5);
        requestReleaseIO(handle5);
        test_assert(handle6 == NULL && proceeded == 5);
        requestReleaseIO(handle4);
        test_assert(handle6 != NULL && proceeded == 6);
        requestReleaseIO(handle6);
        test_assert(handle7 != NULL && handle8 == NULL && proceeded == 7);
        requestReleaseIO(handle7);
        requestReleaseLock(handle3), requestReleaseLock(handle4), requestReleaseLock(handle5),
            requestReleaseLock(handle6), requestReleaseLock(handle7);
        test_assert(handle8 != NULL && proceeded == 8);
        requestReleaseIO(handle8);
        requestReleaseLock(handle8);
        test_assert(proceeded == 8);
        ack_case_reset();
    }

    TEST("wait-ack: proceed rightaway") {
        test_assert(!requestLockWouldBlock(50,db,key1));
        requestGetIOAndLock(50,db,key1,proceedRightaway,NULL,&handle1,NULL,NULL);
        test_assert(handle1 != NULL && proceeded == 1);
        test_assert(!requestLockWouldBlock(50,db,key1));
        requestGetIOAndLock(50,db,key1,proceedRightaway,NULL,&handle2,NULL,NULL);
        test_assert(handle2 != NULL && proceeded == 2);
        test_assert(!requestLockWouldBlock(51,db,key1));
        requestGetIOAndLock(51,db,key1,proceedRightaway,NULL,&handle3,NULL,NULL);
        test_assert(handle3 != NULL && proceeded == 3);
        test_assert(!requestLockWouldBlock(51,db,NULL));
        requestGetIOAndLock(51,db,NULL,proceedRightaway,NULL,&handle4,NULL,NULL);
        test_assert(handle4 != NULL && proceeded == 4);
        test_assert(!requestLockWouldBlock(51,NULL,NULL));
        requestGetIOAndLock(51,NULL,NULL,proceedRightaway,NULL,&handle5,NULL,NULL);
        test_assert(handle5 != NULL && proceeded == 5);
        ack_case_reset();
    }

    TEST("wait-ack: proceed mixed later & rightaway") {
        requestGetIOAndLock(60,db2,key1,proceedWithoutAck,NULL,&handle1,NULL,NULL);
        test_assert(handle1 != NULL && proceeded == 1);
        requestGetIOAndLock(60,db2,key2,proceedRightaway,NULL,&handle2,NULL,NULL);
        test_assert(handle2 != NULL && proceeded == 2);

        requestGetIOAndLock(61,db,key1,proceedRightaway,NULL,&handle3,NULL,NULL);
        test_assert(handle3 != NULL && proceeded == 3);
        requestGetIOAndLock(61,db,key1,proceedWithoutAck,NULL,&handle4,NULL,NULL);
        test_assert(handle4 != NULL && proceeded == 4);

        requestGetIOAndLock(61,db,key1,proceedWithoutAck,NULL,&handle5,NULL,NULL);
        test_assert(handle5 == NULL && proceeded == 4);
        requestGetIOAndLock(61,db,key2,proceedRightaway,NULL,&handle6,NULL,NULL);
        test_assert(handle6 != NULL && proceeded == 5);

        requestGetIOAndLock(61,db2,key1,proceedWithoutAck,NULL,&handle7,NULL,NULL);
        test_assert(handle7 == NULL && proceeded == 5);

        requestGetIOAndLock(61,db,NULL,proceedWithoutAck,NULL,&handle8,NULL,NULL);
        test_assert(handle8 == NULL && proceeded == 5);

        requestGetIOAndLock(61,NULL,NULL,proceedWithoutAck,NULL,&handle9,NULL,NULL);
        test_assert(handle9 == NULL && proceeded == 5);

        requestGetIOAndLock(61,db,key1,proceedWithoutAck,NULL,&handle10,NULL,NULL);
        test_assert(handle10 == NULL && proceeded == 5);

        requestReleaseIO(handle4);
        test_assert(handle5 != NULL && proceeded == 6);
        requestReleaseIO(handle5);
        test_assert(handle8 != NULL && handle9 == NULL && proceeded == 7);

        requestReleaseIO(handle1);
        test_assert(handle7 == NULL && proceeded == 7);
        requestReleaseLock(handle1);
        test_assert(handle7 != NULL && proceeded == 8);

        requestReleaseIO(handle7), requestReleaseIO(handle8);

        test_assert(handle9 != NULL && proceeded == 9);
        requestReleaseIO(handle9);
        test_assert(handle10 != NULL && proceeded == 10);
        requestReleaseIO(handle10);

        requestReleaseLock(handle4), requestReleaseLock(handle5), requestReleaseLock(handle7),
            requestReleaseLock(handle8), requestReleaseLock(handle9), requestReleaseLock(handle10);

        test_assert(!requestLockWouldBlock(62,db,key1));
        ack_case_reset();
    }

   return error;
}

#endif

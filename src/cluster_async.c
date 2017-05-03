#include "server.h"

/* ============================ Iterators: singleObjectIterator ============================ */

#define STAGE_PREPARE 0
#define STAGE_PAYLOAD 1
#define STAGE_CHUNKED 2
#define STAGE_FILLTTL 3
#define STAGE_DONE    4

typedef struct {
    int stage;
    robj *key;
    robj *val;
    long long expire;
    unsigned long cursor;
    unsigned long lindex;
    unsigned long zindex;
} singleObjectIterator;

static singleObjectIterator *
createSingleObjectIterator(robj *key) {
    singleObjectIterator *it = zmalloc(sizeof(singleObjectIterator));
    it->stage = STAGE_PREPARE;
    it->key = key;
    incrRefCount(it->key);
    it->val = NULL;
    it->expire = 0;
    it->cursor = 0;
    it->lindex = 0;
    it->zindex = 0;
    return it;
}

static void
freeSingleObjectIterator(singleObjectIterator *it) {
    if (it->val != NULL) {
        decrRefCount(it->val);
    }
    decrRefCount(it->key);
    zfree(it);
}

static void
freeSingleObjectIteratorVoid(void *it) {
    freeSingleObjectIterator(it);
}

static int
singleObjectIteratorHasNext(singleObjectIterator *it) {
    return it->stage != STAGE_DONE;
}

static void
singleObjectIteratorScanCallback(void *data, const dictEntry *de) {
    void **pd = (void **)data;
    list *l = pd[0];
    robj *o = pd[1];
    long long *n = pd[2];

    sds s[2] = {NULL, NULL};
    switch (o->type) {
    case OBJ_HASH:
        s[0] = dictGetKey(de);
        s[1] = dictGetVal(de);
        break;
    case OBJ_SET:
        s[0] = dictGetKey(de);
        break;
    }
    for (int i = 0; i < 2; i ++) {
        if (s[i] != NULL) {
            sds dup = sdsdup(s[i]);
            *n += sdslen(dup);
            listAddNodeTail(l, dup);
        }
    }
}

static uint64_t
convertDoubleToRawBits(double value) {
    union {
        double d;
        uint64_t u;
    } fp;
    fp.d = value;
    return fp.u;
}

static double
convertRawBitsToDouble(uint64_t value) {
    union {
        double d;
        uint64_t u;
    } fp;
    fp.u = value;
    return fp.d;
}

static sds
createRawStringFromUint64(uint64_t v) {
    uint64_t p = intrev64ifbe(v);
    return sdsnewlen((const char *)&p, sizeof(p));
}

static int
decodeUint64FromRawStringObject(robj *o, uint64_t *p) {
    if (sdsEncodedObject(o) && sdslen(o->ptr) == sizeof(uint64_t)) {
        *p = intrev64ifbe(*(uint64_t *)(o->ptr));
        return C_OK;
    }
    return C_ERR;
}

static long
estimateNumberOfRestoreCommandsObject(robj *val, long long maxbulks) {
    long long numbulks = 0;
    switch (val->type) {
    case OBJ_LIST:
        if (val->encoding == OBJ_ENCODING_QUICKLIST) {
            numbulks = listTypeLength(val);
        }
        break;
    case OBJ_HASH:
        if (val->encoding == OBJ_ENCODING_HT) {
            numbulks = hashTypeLength(val) * 2;
        }
        break;
    case OBJ_SET:
        if (val->encoding == OBJ_ENCODING_HT) {
            numbulks = setTypeSize(val);
        }
        break;
    case OBJ_ZSET:
        if (val->encoding == OBJ_ENCODING_SKIPLIST) {
            numbulks = zsetLength(val) * 2;
        }
        break;
    }

    /* 1x RESTORE-PAYLOAD */
    if (numbulks <= maxbulks) {
        return 1;
    }

    /* nx RESTORE-CHUNKED + 1x RESTORE-FILLTTL */
    return 1 + (numbulks + maxbulks - 1) / maxbulks;
}

static long
estimateNumberOfRestoreCommands(redisDb *db, robj *key, long long maxbulks) {
    robj *val = lookupKeyWrite(db, key);
    if (val == NULL) {
        return 0;
    }
    return 1 + estimateNumberOfRestoreCommandsObject(val, maxbulks);
}

static asyncMigrationClient *getAsyncMigrationClient(int db);

static int
singleObjectIteratorNextStagePrepare(client *c, singleObjectIterator *it, unsigned int maxbulks) {
    serverAssert(it->stage == STAGE_PREPARE);
    robj *key = it->key;
    robj *val = lookupKeyWrite(c->db, key);
    if (val == NULL) {
        it->stage = STAGE_DONE;
        return 0;
    }
    it->val = val;
    incrRefCount(it->val);
    it->expire = getExpire(c->db, key);

    int msgs = 0;

    asyncMigrationClient *ac = getAsyncMigrationClient(c->db->id);
    if (ac->c == c) {
        if (ac->init == 0) {
            ac->init = 1;
            if (server.requirepass != NULL) {
                /* RESTORE-ASYNC-AUTH $passwd */
                addReplyMultiBulkLen(c, 2);
                addReplyBulkCString(c, "RESTORE-ASYNC-AUTH");
                addReplyBulkCString(c, server.requirepass);
                msgs ++;
            }
            do {
                /* RESTORE-ASYNC-SELECT $db */
                addReplyMultiBulkLen(c, 2);
                addReplyBulkCString(c, "RESTORE-ASYNC-SELECT");
                addReplyBulkLongLong(c, c->db->id);
                msgs ++;
            } while (0);
        }
    }

    do {
        /* RESTORE-ASYNC delete $key */
        addReplyMultiBulkLen(c, 3);
        addReplyBulkCString(c, "RESTORE-ASYNC");
        addReplyBulkCString(c, "delete");
        addReplyBulk(c, key);
        msgs ++;
    } while(0);

    long n = estimateNumberOfRestoreCommandsObject(val, maxbulks);
    if (n != 1) {
        it->stage = STAGE_CHUNKED;
    } else {
        it->stage = STAGE_PAYLOAD;
    }
    return msgs;
}

extern void createDumpPayload(rio *payload, robj *o);

static int
singleObjectIteratorNextStagePayload(client *c, singleObjectIterator *it) {
    serverAssert(it->stage == STAGE_PAYLOAD);
    robj *key = it->key;
    robj *val = it->val;
    long long ttlms = 0;
    if (it->expire != -1) {
        ttlms = it->expire - mstime();
        if (ttlms < 1) {
            ttlms = 1;
        }
    }

    if (val->type != OBJ_STRING) {
        rio payload;
        createDumpPayload(&payload, val);
        do {
            /* RESTORE-ASYNC object $key $ttlms $payload */
            addReplyMultiBulkLen(c, 5);
            addReplyBulkCString(c, "RESTORE-ASYNC");
            addReplyBulkCString(c, "object");
            addReplyBulk(c, key);
            addReplyBulkLongLong(c, ttlms);
            addReplyBulkSds(c, payload.io.buffer.ptr);
        } while (0);
    } else {
        do {
            /* RESTORE-ASYNC string $key $ttlms $payload */
            addReplyMultiBulkLen(c, 5);
            addReplyBulkCString(c, "RESTORE-ASYNC");
            addReplyBulkCString(c, "string");
            addReplyBulk(c, key);
            addReplyBulkLongLong(c, ttlms);
            addReplyBulk(c, val);
        } while (0);
    }

    it->stage = STAGE_DONE;
    return 1;
}

static int
singleObjectIteratorNextStageFillTTL(client *c, singleObjectIterator *it) {
    serverAssert(it->stage == STAGE_FILLTTL);
    robj *key = it->key;
    long long ttlms = 0;
    if (it->expire != -1) {
        ttlms = it->expire - mstime();
        if (ttlms < 1) {
            ttlms = 1;
        }
    }

    do {
        /* RESTORE-ASYNC expire $key $ttlms */
        addReplyMultiBulkLen(c, 4);
        addReplyBulkCString(c, "RESTORE-ASYNC");
        addReplyBulkCString(c, "expire");
        addReplyBulk(c, key);
        addReplyBulkLongLong(c, ttlms);
    } while (0);

    it->stage = STAGE_DONE;
    return 1;
}

extern zskiplistNode* zslGetElementByRank(zskiplist *zsl, unsigned long rank);

static int
singleObjectIteratorNextStageChunkedTypeList(singleObjectIterator *it,
        list *l, robj *o, long long *psize, unsigned int maxbulks, unsigned int maxbytes) {
    serverAssert(o->type == OBJ_LIST);
    serverAssert(o->encoding == OBJ_ENCODING_QUICKLIST);

    long long done = 0, nn = 0;

    listTypeIterator *li = listTypeInitIterator(o, it->lindex, LIST_TAIL);

    do {
        listTypeEntry entry;
        if (listTypeNext(li, &entry)) {
            quicklistEntry *qe = &(entry.entry);
            sds ele;
            if (qe->value) {
                ele = sdsnewlen((const char *)qe->value, qe->sz);
            } else {
                ele = sdsfromlonglong(qe->longval);
            }
            nn += sdslen(ele);
            listAddNodeTail(l, ele);

            it->lindex ++;
        } else {
            done = 1;
        }
    } while (!done && listLength(l) < maxbulks && nn < maxbytes);

    listTypeReleaseIterator(li);

    *psize = listTypeLength(o);
    return done != 0;
}

static int
singleObjectIteratorNextStageChunkedTypeZSet(singleObjectIterator *it,
        list *l, robj *o, long long *psize, unsigned int maxbulks, unsigned int maxbytes) {
    serverAssert(o->type == OBJ_ZSET);
    serverAssert(o->encoding == OBJ_ENCODING_SKIPLIST);

    long long done = 0, nn = 0;

    long long rank = (long long)zsetLength(o) - it->zindex;
    zset *zs = o->ptr;
    zskiplistNode *node = (rank >= 1) ? zslGetElementByRank(zs->zsl, rank) : NULL;

    do {
        if (node != NULL) {
            sds field = sdsdup(node->ele);
            nn += sdslen(field);
            listAddNodeTail(l, field);

            uint64_t u64 = convertDoubleToRawBits(node->score);
            sds score = createRawStringFromUint64(u64);
            nn += sdslen(score);
            listAddNodeTail(l, score);

            node = node->backward;
            it->zindex ++;
        } else {
            done = 1;
        }
    } while (!done && listLength(l) < maxbulks && nn < maxbytes);

    *psize = zsetLength(o);
    return done != 0;
}

static int
singleObjectIteratorNextStageChunkedTypeHashOrDict(singleObjectIterator *it,
        list *l, robj *o, long long *psize, unsigned int maxbulks, unsigned int maxbytes) {
    serverAssert(o->type == OBJ_HASH || o->type == OBJ_SET);
    serverAssert(o->encoding == OBJ_ENCODING_HT);

    long long done = 0, nn = 0;

    int loop = maxbulks * 10;
    if (loop < 100) {
        loop = 100;
    }
    dict *ht = o->ptr;
    void *pd[] = {l, o, &nn};

    do {
        it->cursor = dictScan(ht, it->cursor, singleObjectIteratorScanCallback, NULL, pd);
        if (it->cursor == 0) {
            done = 1;
        }
    } while (!done && listLength(l) < maxbulks && nn < maxbytes && (-- loop) >= 0);

    *psize = dictSize(ht);
    return done != 0;
}

static int
singleObjectIteratorNextStageChunked(client *c, singleObjectIterator *it,
        long long timeout, unsigned int maxbulks, unsigned int maxbytes) {
    serverAssert(it->stage == STAGE_CHUNKED);
    robj *key = it->key;
    robj *val = it->val;
    long long ttlms = timeout * 3;
    if (ttlms < 1000) {
        ttlms = 1000;
    }

    const char *type = NULL;
    switch (val->type) {
    case OBJ_LIST:
        type = "list"; break;
    case OBJ_HASH:
        type = "hash"; break;
    case OBJ_SET:
        type = "dict"; break;
    case OBJ_ZSET:
        type = "zset"; break;
    default:
        serverPanic("unknown object type = %d", val->type);
    }

    list *ll = listCreate();

    long long done = 0, maxsize = 0;

    switch (val->type) {
    case OBJ_LIST:
        done = singleObjectIteratorNextStageChunkedTypeList(it, ll, val,
                &maxsize, maxbulks, maxbytes);
        break;
    case OBJ_ZSET:
        done = singleObjectIteratorNextStageChunkedTypeZSet(it, ll, val,
                &maxsize, maxbulks, maxbytes);
        break;
    case OBJ_HASH: case OBJ_SET:
        done = singleObjectIteratorNextStageChunkedTypeHashOrDict(it, ll, val,
                &maxsize, maxbulks, maxbytes);
        break;
    }

    int msgs = 0;

    if (listLength(ll) != 0) {
        /* RESTORE-ASYNC list/hash/zset/dict $key $ttlms $maxsize [$arg1 ...] */
        addReplyMultiBulkLen(c, 5 + listLength(ll));
        addReplyBulkCString(c, "RESTORE-ASYNC");
        addReplyBulkCString(c, type);
        addReplyBulk(c, key);
        addReplyBulkLongLong(c, ttlms);
        addReplyBulkLongLong(c, maxsize);

        while (listLength(ll) != 0) {
            listNode *head = listFirst(ll);
            sds s = listNodeValue(head);
            addReplyBulkSds(c, s);
            listDelNode(ll, head);
        }
        msgs ++;
    }
    listRelease(ll);

    if (done) {
        it->stage = STAGE_FILLTTL;
    }
    return msgs;
}

static int
singleObjectIteratorNext(client *c, singleObjectIterator *it,
        long long timeout, unsigned int maxbulks, unsigned int maxbytes) {
    /* *
     * STAGE_PREPARE ---> STAGE_PAYLOAD ---> STAGE_DONE
     *     |                                      A
     *     V                                      |
     *     +------------> STAGE_CHUNKED ---> STAGE_FILLTTL
     *                      A       |
     *                      |       V
     *                      +-------+
     * */
    switch (it->stage) {
    case STAGE_PREPARE:
        return singleObjectIteratorNextStagePrepare(c, it, maxbulks);
    case STAGE_PAYLOAD:
        return singleObjectIteratorNextStagePayload(c, it);
    case STAGE_CHUNKED:
        return singleObjectIteratorNextStageChunked(c, it, timeout, maxbulks, maxbytes);
    case STAGE_FILLTTL:
        return singleObjectIteratorNextStageFillTTL(c, it);
    case STAGE_DONE:
        return 0;
    default:
        serverPanic("invalid stage=%d of singleObjectIterator", it->stage);
    }
}

static void
singleObjectIteratorStatus(client *c, singleObjectIterator *it) {
    if (it == NULL) {
        addReply(c, shared.nullmultibulk);
        return;
    }
    void *ptr = addDeferredMultiBulkLength(c);
    int total = 0;

    total ++; addReplyBulkCString(c, "key");
    addReplyBulk(c, it->key);

    total ++; addReplyBulkCString(c, "object.type");
    addReplyBulkLongLong(c, it->val == NULL ? -1 : it->val->type);

    total ++; addReplyBulkCString(c, "object.encoding");
    addReplyBulkLongLong(c, it->val == NULL ? -1 : it->val->encoding);

    total ++; addReplyBulkCString(c, "stage");
    addReplyBulkLongLong(c, it->stage);

    total ++; addReplyBulkCString(c, "expire");
    addReplyBulkLongLong(c, it->expire);

    total ++; addReplyBulkCString(c, "cursor");
    addReplyBulkLongLong(c, it->cursor);

    total ++; addReplyBulkCString(c, "lindex");
    addReplyBulkLongLong(c, it->lindex);

    total ++; addReplyBulkCString(c, "zindex");
    addReplyBulkLongLong(c, it->zindex);

    setDeferredMultiBulkLength(c, ptr, total * 2);
}

/* ============================ Iterators: batchedObjectIterator =========================== */

typedef struct {
    long long timeout;
    long long maxbulks;
    long long maxbytes;
    dict *keys;
    list *iterator_list;
    list *released_keys;
    long delivered_msgs;
    long estimated_msgs;
} batchedObjectIterator;

static batchedObjectIterator *
createBatchedObjectIterator(long long timeout, unsigned int maxbulks, unsigned int maxbytes) {
    batchedObjectIterator *it = zmalloc(sizeof(batchedObjectIterator));
    it->timeout = timeout;
    it->maxbulks = maxbulks;
    it->maxbytes = maxbytes;
    it->keys = dictCreate(&setDictType, NULL);
    it->iterator_list = listCreate();
    listSetFreeMethod(it->iterator_list, freeSingleObjectIteratorVoid);
    it->released_keys = listCreate();
    listSetFreeMethod(it->released_keys, decrRefCountVoid);
    it->delivered_msgs = 0;
    it->estimated_msgs = 0;
    return it;
}

static void
freeBatchedObjectIterator(batchedObjectIterator *it) {
    dictRelease(it->keys);
    listRelease(it->iterator_list);
    listRelease(it->released_keys);
    zfree(it);
}

static int
batchedObjectIteratorHasNext(batchedObjectIterator *it) {
    list *ll = it->iterator_list;
    while (listLength(ll) != 0) {
        listNode *head = listFirst(ll);
        singleObjectIterator *sp = listNodeValue(head);
        if (singleObjectIteratorHasNext(sp)) {
            return 1;
        }
        if (sp->val != NULL) {
            incrRefCount(sp->key);
            listAddNodeTail(it->released_keys, sp->key);
        }
        listDelNode(ll, head);
    }
    return 0;
}

static int
batchedObjectIteratorNext(client *c, batchedObjectIterator *it) {
    list *ll = it->iterator_list;
    if (listLength(ll) != 0) {
        listNode *head = listFirst(ll);
        singleObjectIterator *sp = listNodeValue(head);
        return singleObjectIteratorNext(c, sp, it->timeout, it->maxbulks, it->maxbytes);
    }
    return 0;
}

static int
batchedObjectIteratorContains(batchedObjectIterator *it, robj *key) {
    return dictFind(it->keys, key->ptr) != NULL;
}

static int
batchedObjectIteratorAddKey(redisDb *db, batchedObjectIterator *it, robj *key) {
    if (batchedObjectIteratorContains(it, key)) {
        return 0;
    }
    dictAdd(it->keys, sdsdup(key->ptr), NULL);

    listAddNodeTail(it->iterator_list, createSingleObjectIterator(key));
    it->estimated_msgs += estimateNumberOfRestoreCommands(db, key, it->maxbulks);
    return 1;
}

static void
batchedObjectIteratorStatus(client *c, batchedObjectIterator *it) {
    if (it == NULL) {
        addReply(c, shared.nullmultibulk);
        return;
    }
    void *ptr = addDeferredMultiBulkLength(c);
    int total = 0;

    total ++; addReplyBulkCString(c, "keys");
    addReplyMultiBulkLen(c, 2);
    addReplyBulkLongLong(c, dictSize(it->keys));
    do {
        addReplyMultiBulkLen(c, dictSize(it->keys));
        dictIterator *di = dictGetIterator(it->keys);
        dictEntry *de;
        while((de = dictNext(di)) != NULL) {
            sds s = dictGetKey(de);
            addReplyBulkCBuffer(c, s, sdslen(s));
        }
        dictReleaseIterator(di);
    } while (0);

    total ++; addReplyBulkCString(c, "timeout");
    addReplyBulkLongLong(c, it->timeout);

    total ++; addReplyBulkCString(c, "maxbulks");
    addReplyBulkLongLong(c, it->maxbulks);

    total ++; addReplyBulkCString(c, "maxbytes");
    addReplyBulkLongLong(c, it->maxbytes);

    total ++; addReplyBulkCString(c, "estimated_msgs");
    addReplyBulkLongLong(c, it->estimated_msgs);

    total ++; addReplyBulkCString(c, "delivered_msgs");
    addReplyBulkLongLong(c, it->delivered_msgs);

    total ++; addReplyBulkCString(c, "released_keys");
    addReplyBulkLongLong(c, listLength(it->released_keys));

    total ++; addReplyBulkCString(c, "iterator_list");
    addReplyMultiBulkLen(c, 2);
    addReplyBulkLongLong(c, listLength(it->iterator_list));
    do {
        list *ll = it->iterator_list;
        if (listLength(ll) != 0) {
            listNode *head = listFirst(ll);
            singleObjectIteratorStatus(c, listNodeValue(head));
        } else {
            singleObjectIteratorStatus(c, NULL);
        }
    } while (0);

    setDeferredMultiBulkLength(c, ptr, total * 2);
}

/* ============================ Clients for Asynchronous Migration ========================= */

static asyncMigrationClient*
getAsyncMigrationClient(int db) {
    return &server.async_migration_clients[db];
}

static void
asyncMigrationClientInterrupt(asyncMigrationClient *ac, const char *errmsg) {
    batchedObjectIterator *it = ac->iterator;
    long ret = (it != NULL) ? (long)listLength(it->released_keys) : -1;

    list *ll = ac->bclients;
    while (listLength(ll) != 0) {
        listNode *head = listFirst(ll);
        client *c = listNodeValue(head);
        serverAssert(c->migration_waitq == ll);

        if (errmsg != NULL) {
            addReplyError(c, errmsg);
        } else {
            addReplyLongLong(c, ret);
        }

        c->migration_waitq = NULL;
        listDelNode(ll, head);

        unblockClient(c);
    }
}

void
unblockClientFromAsyncMigration(client *c) {
    list *ll = c->migration_waitq;
    if (ll != NULL) {
        listNode *node = listSearchKey(ll, c);
        serverAssert(node != NULL);

        c->migration_waitq = NULL;
        listDelNode(ll, node);
    }
}

void
releaseClientFromAsyncMigration(client *c) {
    asyncMigrationClient *ac = getAsyncMigrationClient(c->db->id);
    serverAssert(ac->c == c);

    batchedObjectIterator *it = ac->iterator;

    serverLog(LL_WARNING, "async_migration: release connection %s:%d (DB=%d): "
            "sending_msgs = %ld, bclients = %ld, iterator = %ld, "
            "timeout = %lld(ms), elapsed = %lld(ms)",
            ac->host, ac->port, c->db->id, ac->sending_msgs,
            (long)listLength(ac->bclients), (it != NULL) ? (long)listLength(it->iterator_list) : -1,
            ac->timeout, mstime() - ac->lastuse);

    asyncMigrationClientInterrupt(ac, "interrupted: released connection");

    sdsfree(ac->host);
    if (it != NULL) {
        freeBatchedObjectIterator(it);
    }
    listRelease(ac->bclients);

    c->flags &= ~CLIENT_ASYNC_MIGRATION;

    memset(ac, 0, sizeof(*ac));
}

static int
asyncMigartionClientCancelErrorFormat(int db, const char *fmt, ...) {
    asyncMigrationClient *ac = getAsyncMigrationClient(db);
    if (ac->c == NULL) {
        return 0;
    }
    va_list ap;
    va_start(ap, fmt);
    sds errmsg = sdscatvprintf(sdsempty(), fmt, ap);
    va_end(ap);

    serverLog(LL_WARNING, "async_migration: release connection %s:%d (DB=%d) (%s)",
            ac->host, ac->port, db, errmsg);

    asyncMigrationClientInterrupt(ac, errmsg);
    freeClient(ac->c);

    sdsfree(errmsg);

    serverAssert(ac->c == NULL && ac->iterator == NULL);
    return 1;
}

static asyncMigrationClient *
asyncMigrationClientInit(int db, sds host, int port, long long timeout) {
    asyncMigrationClient *ac = getAsyncMigrationClient(db);
    if (ac->c != NULL) {
        if (ac->port == port && !strcmp(ac->host, host)) {
            return ac;
        }
    }

    int fd = anetTcpNonBlockConnect(server.neterr, host, port);
    if (fd == -1) {
        serverLog(LL_WARNING, "async_migration: anetTcpNonBlockConnect %s:%d (DB=%d) (%s)",
            host, port, db, server.neterr);
        return NULL;
    }

    anetEnableTcpNoDelay(NULL, fd);

    int wait = timeout;
    if (wait > 10) {
        wait = 10;
    }
    if ((aeWait(fd, AE_WRITABLE, wait) & AE_WRITABLE) == 0) {
        serverLog(LL_WARNING, "async_migration: aeWait %s:%d (DB=%d) (io error or timeout)",
                host, port, db);
        close(fd);
        return NULL;
    }

    client *c = createClient(fd);
    if (c == NULL) {
        serverLog(LL_WARNING, "async_migration: createClient %s:%d (DB=%d) (%s)",
                host, port, db, server.neterr);
        return NULL;
    }
    if (selectDb(c, db) != C_OK) {
        serverLog(LL_WARNING, "async_migration: selectDb %s:%d (DB=%d) (invalid DB index)",
                host, port, db);
        freeClient(c);
        return NULL;
    }
    asyncMigartionClientCancelErrorFormat(db, "interrupted: replaced by %s:%d (DB=%d)",
            host, port, db);

    c->flags |= CLIENT_ASYNC_MIGRATION;
    c->authenticated = 1;

    ac->c = c;
    ac->init = 0;
    ac->host = sdsdup(host);
    ac->port = port;
    ac->timeout = timeout;
    ac->lastuse = mstime();
    ac->sending_msgs = 0;
    ac->bclients = listCreate();
    ac->iterator = NULL;

    serverLog(LL_WARNING, "async_migration: connect to %s:%d (DB=%d) OK",
            host, port, db);
    return ac;
}

static int
asyncMigrationClientStatusOrBlock(client *c, int block) {
    asyncMigrationClient *ac = getAsyncMigrationClient(c->db->id);
    if (ac->c == NULL || ac->iterator == NULL) {
        return 0;
    }
    if (!block) {
        return 1;
    }
    serverAssert(c->migration_waitq == NULL);

    list *ll = ac->bclients;

    c->migration_waitq = ll;
    listAddNodeTail(ll, c);

    blockClient(c, BLOCKED_ASYNC_MIGRATION);
    return 1;
}

void
cleanupClientsForAsyncMigration() {
    for (int db = 0; db < server.dbnum; db ++) {
        asyncMigrationClient *ac = getAsyncMigrationClient(db);
        if (ac->c == NULL) {
            continue;
        }
        batchedObjectIterator *it = ac->iterator;
        long long timeout = (it != NULL) ? ac->timeout : 1000 * 60;
        long long elapsed = mstime() - ac->lastuse;
        if (elapsed <= timeout) {
            continue;
        }
        asyncMigartionClientCancelErrorFormat(db, (it != NULL) ?
                "interrupted: migration timeout" : "interrupted: idle timeout");
    }
}

int
inConflictWithAsyncMigration(client *c, struct redisCommand *cmd, robj **argv, int argc) {
    asyncMigrationClient *ac = getAsyncMigrationClient(c->db->id);
    if (ac->c == NULL || ac->iterator == NULL) {
        return 0;
    }
    batchedObjectIterator *it = ac->iterator;

    multiState _ms, *ms = &_ms;
    multiCmd mc;
    if (cmd->proc != execCommand) {
        mc.cmd = cmd;
        mc.argv = argv;
        mc.argc = argc;
        ms->commands = &mc;
        ms->count = 1;
    } else if (c->flags & CLIENT_MULTI) {
        ms = &c->mstate;
    } else {
        return 0;
    }

    for (int i = 0; i < ms->count; i ++) {
        robj **margv;
        int margc, numkeys;
        struct redisCommand *mcmd = ms->commands[i].cmd;
        if (mcmd->flags & CMD_READONLY) {
            continue;
        }
        margv = ms->commands[i].argv;
        margc = ms->commands[i].argc;

        int migrating = 0;
        int *keyindex = getKeysFromCommand(mcmd, margv, margc, &numkeys);
        for (int j = 0; j < numkeys && !migrating; j ++) {
            robj *key = margv[keyindex[j]];
            migrating = batchedObjectIteratorContains(it, key);
        }
        getKeysFreeResult(keyindex);

        if (migrating) {
            return 1;
        }
    }
    return 0;
}

/* ============================ Command: MIGRATE-ASNYC-DUMP ================================ */

/* *
 * MIGRATE-ASYNC-DUMP $timeout $maxbulks $key1 [$key2 ...]
 * */
void
migrateAsyncDumpCommand(client *c) {
    long long timeout;
    if (getLongLongFromObject(c->argv[1], &timeout) != C_OK ||
            !(timeout >= 0 && timeout <= INT_MAX)) {
        addReplyErrorFormat(c, "invalid value of timeout (%s)",
                (char *)c->argv[1]->ptr);
        return;
    }
    if (timeout == 0) {
        timeout = 1000 * 10;
    }

    long long maxbulks;
    if (getLongLongFromObject(c->argv[2], &maxbulks) != C_OK ||
            !(maxbulks >= 0 && maxbulks <= INT_MAX / 2)) {
        addReplyErrorFormat(c, "invalid value of maxbulks (%s)",
                (char *)c->argv[2]->ptr);
        return;
    }
    if (maxbulks == 0) {
        maxbulks = 200;
    }

    long long maxbytes = INT_MAX / 2;

    batchedObjectIterator *it = createBatchedObjectIterator(timeout, maxbulks, maxbytes);
    for (int i = 3; i < c->argc; i ++) {
        batchedObjectIteratorAddKey(c->db, it, c->argv[i]);
    }

    void *ptr = addDeferredMultiBulkLength(c);
    int total = 0;
    while (batchedObjectIteratorHasNext(it)) {
        total += batchedObjectIteratorNext(c, it);
    }
    setDeferredMultiBulkLength(c, ptr, total);

    freeBatchedObjectIterator(it);
}

/* ============================ Command: MIGRATE-ASNYC ===================================== */

static unsigned int
asyncMigrationClientBufferLimit(unsigned int maxbytes) {
    clientBufferLimitsConfig *p = &server.client_obuf_limits[CLIENT_TYPE_NORMAL];
    if (p->soft_limit_bytes != 0 && p->soft_limit_bytes < maxbytes) {
        maxbytes = p->soft_limit_bytes;
    }
    if (p->hard_limit_bytes != 0 && p->hard_limit_bytes < maxbytes) {
        maxbytes = p->hard_limit_bytes;
    }
    return maxbytes;
}

static int
asyncMigrationNextInMicroseconds(asyncMigrationClient *ac, int atleast, long long usecs) {
    batchedObjectIterator *it = ac->iterator;
    long long deadline = ustime() + usecs;
    int n = 0;
    while (batchedObjectIteratorHasNext(it)) {
        long long usage = getClientOutputBufferMemoryUsage(ac->c);
        if (ac->sending_msgs != 0 && it->maxbytes <= usage) {
            break;
        }
        if ((n += batchedObjectIteratorNext(ac->c, it)) >= atleast && deadline <= ustime()) {
            break;
        }
    }
    return n;
}

int *
migrateAsyncGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    (void)cmd;
    (void)argv;

    int num = 0, *pos = NULL;
    if (argc <= 6) {
        goto out;
    }
    num = argc - 6;
    pos = zmalloc(sizeof(int) * num);
    for (int i = 0; i < num; i ++) {
        pos[i] = i + 6;
    }

out:
    *numkeys = num;
    return pos;
}

/* *
 * MIGRATE-ASYNC $host $port $timeout $maxbulks $maxbytes $key1 [$key2 ...]
 * */
void
migrateAsyncCommand(client *c) {
    if (asyncMigrationClientStatusOrBlock(c, 0)) {
        addReplyError(c, "the specified DB is being migrated");
        return;
    }

    sds host = c->argv[1]->ptr;

    long long port;
    if (getLongLongFromObject(c->argv[2], &port) != C_OK ||
            !(port >= 1 && port < 65536)) {
        addReplyErrorFormat(c, "invalid value of port (%s)",
                (char *)c->argv[2]->ptr);
        return;
    }

    long long timeout;
    if (getLongLongFromObject(c->argv[3], &timeout) != C_OK ||
            !(timeout >= 0 && timeout <= INT_MAX)) {
        addReplyErrorFormat(c, "invalid value of timeout (%s)",
                (char *)c->argv[3]->ptr);
        return;
    }
    if (timeout == 0) {
        timeout = 1000 * 10;
    }

    long long maxbulks;
    if (getLongLongFromObject(c->argv[4], &maxbulks) != C_OK ||
            !(maxbulks >= 0 && maxbulks <= INT_MAX / 2)) {
        addReplyErrorFormat(c, "invalid value of maxbulks (%s)",
                (char *)c->argv[4]->ptr);
        return;
    }
    if (maxbulks == 0) {
        maxbulks = 200;
    }
    if (maxbulks > 2000) {
        maxbulks = 2000;
    }

    long long maxbytes;
    if (getLongLongFromObject(c->argv[5], &maxbytes) != C_OK ||
            !(maxbytes >= 0 && maxbytes <= INT_MAX / 2)) {
        addReplyErrorFormat(c, "invalid value of maxbytes (%s)",
                (char *)c->argv[5]->ptr);
        return;
    }
    if (maxbytes == 0) {
        maxbytes = 1024 * 1024;
    }
    maxbytes = asyncMigrationClientBufferLimit(maxbytes);

    asyncMigrationClient *ac = asyncMigrationClientInit(c->db->id, host, port, timeout);
    if (ac == NULL) {
        addReplyErrorFormat(c, "connect to %s:%d failed", host, (int)port);
        return;
    }
    serverAssert(ac->sending_msgs == 0);
    serverAssert(listLength(ac->bclients) == 0 && ac->iterator == NULL);

    batchedObjectIterator *it = createBatchedObjectIterator(timeout, maxbulks, maxbytes);
    for (int i = 6; i < c->argc; i ++) {
        batchedObjectIteratorAddKey(c->db, it, c->argv[i]);
    }
    ac->iterator = it;

    ac->timeout = timeout;
    ac->lastuse = mstime();
    ac->sending_msgs += asyncMigrationNextInMicroseconds(ac, 4, 500);

    asyncMigrationClientStatusOrBlock(c, 1);

    if (ac->sending_msgs != 0) {
        return;
    }
    asyncMigrationClientInterrupt(ac, NULL);

    ac->iterator = NULL;
    freeBatchedObjectIterator(it);
}

/* ============================ Command: MIGRATE-ASNYC-{FENCE/CANCEL/STATUS} =============== */

/* *
 * MIGRATE-ASYNC-FENCE
 * */
void
migrateAsyncFenceCommand(client *c) {
    if (asyncMigrationClientStatusOrBlock(c, 1)) {
        return;
    }
    addReplyLongLong(c, -1);
}

/* *
 * MIGRATE-ASYNC-CANCEL
 * */
void
migrateAsyncCancelCommand(client *c) {
    int retval = asyncMigartionClientCancelErrorFormat(c->db->id, "interrupted: canceled");
    addReplyLongLong(c, retval);
}

/* *
 * MIGRATE-ASYNC-STATUS
 * */
void
migrateAsyncStatusCommand(client *c) {
    asyncMigrationClient *ac = getAsyncMigrationClient(c->db->id);
    if (ac->c == NULL) {
        addReply(c, shared.nullmultibulk);
        return;
    }
    void *ptr = addDeferredMultiBulkLength(c);
    int total = 0;

    total ++; addReplyBulkCString(c, "host");
    addReplyBulkCString(c, ac->host);

    total ++; addReplyBulkCString(c, "port");
    addReplyBulkLongLong(c, ac->port);

    total ++; addReplyBulkCString(c, "init");
    addReplyBulkLongLong(c, ac->init);

    total ++; addReplyBulkCString(c, "timeout");
    addReplyBulkLongLong(c, ac->timeout);

    total ++; addReplyBulkCString(c, "lastuse");
    addReplyBulkLongLong(c, ac->lastuse);

    total ++; addReplyBulkCString(c, "since_lastuse");
    addReplyBulkLongLong(c, mstime() - ac->lastuse);

    total ++; addReplyBulkCString(c, "sending_msgs");
    addReplyBulkLongLong(c, ac->sending_msgs);

    total ++; addReplyBulkCString(c, "memory_usage");
    addReplyBulkLongLong(c, getClientOutputBufferMemoryUsage(ac->c));

    total ++; addReplyBulkCString(c, "bclients");
    addReplyBulkLongLong(c, listLength(ac->bclients));

    total ++; addReplyBulkCString(c, "iterator");
    batchedObjectIteratorStatus(c, ac->iterator);

    setDeferredMultiBulkLength(c, ptr, total * 2);
}

/* ============================ Command: RESTORE-ASYNC-AUTH ================================ */

static void
asyncMigrationReplyAckString(client *c, const char *msg) {
    do {
        /* RESTORE-ASYNC-ACK $errno $message */
        addReplyMultiBulkLen(c, 3);
        addReplyBulkCString(c, "RESTORE-ASYNC-ACK");
        addReplyBulkLongLong(c, 0);
        addReplyBulkCString(c, msg);
    } while (0);
}

static void
asyncMigrationReplyAckErrorFormat(client *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sds errmsg = sdscatvprintf(sdsempty(), fmt, ap);
    va_end(ap);

    do {
        /* RESTORE-ASYNC-ACK $errno $message */
        addReplyMultiBulkLen(c, 3);
        addReplyBulkCString(c, "RESTORE-ASYNC-ACK");
        addReplyBulkLongLong(c, 1);
        addReplyBulkSds(c, errmsg);
    } while (0);

    c->flags |= CLIENT_CLOSE_AFTER_REPLY;
}

extern int time_independent_strcmp(const char *a, const char *b);

/* *
 * RESTORE-ASYNC-AUTH $passwd
 * */
void
restoreAsyncAuthCommand(client *c) {
    if (!server.requirepass) {
        asyncMigrationReplyAckErrorFormat(c, "Client sent AUTH, but no password is set");
        return;
    }
    if (!time_independent_strcmp(c->argv[1]->ptr, server.requirepass)) {
        c->authenticated = 1;
        asyncMigrationReplyAckString(c, "OK");
    } else {
        c->authenticated = 0;
        asyncMigrationReplyAckErrorFormat(c, "invalid password");
    }
}

/* ============================ Command: RESTORE-ASYNC-SELECT ============================== */

/* *
 * RESTORE-ASYNC-SELECT $db
 * */
void
restoreAsyncSelectCommand(client *c) {
    long long db;
    if (getLongLongFromObject(c->argv[1], &db) != C_OK ||
            !(db >= 0 && db <= INT_MAX) || selectDb(c, db) != C_OK) {
        asyncMigrationReplyAckErrorFormat(c, "invalid DB index (%s)", c->argv[1]->ptr);
    } else {
        asyncMigrationReplyAckString(c, "OK");
    }
}

/* ============================ Command: RESTORE-ASYNC ===================================== */

/* RESTORE-ASYNC delete $key */
static int
restoreAsyncHandleOrReplyDeleteKey(client *c, robj *key) {
    if (dbAsyncDelete(c->db, key)) {
        signalModifiedKey(c->db, key);
        server.dirty ++;
    }
    return C_OK;
}

/* RESTORE-ASYNC expire $key $ttlms */
static int
restoreAsyncHandleOrReplyExpireKey(client *c, robj *key) {
    robj *val = lookupKeyWrite(c->db, key);
    if (val == NULL) {
        asyncMigrationReplyAckErrorFormat(c, "the specified key doesn't exist (%s)",
                key->ptr);
        return C_ERR;
    }
    return C_OK;
}

extern int verifyDumpPayload(unsigned char *p, size_t len);

/* RESTORE-ASYNC object $key $ttlms $payload */
static int
restoreAsyncHandleOrReplyTypeObject(client *c, robj *key) {
    if (lookupKeyWrite(c->db, key) != NULL) {
        asyncMigrationReplyAckErrorFormat(c, "the specified key already exists (%s)",
                key->ptr);
        return C_ERR;
    }

    rio payload;
    void *bytes = c->argv[4]->ptr;
    if (verifyDumpPayload(bytes, sdslen(bytes)) != C_OK) {
        asyncMigrationReplyAckErrorFormat(c, "invalid payload checksum (%s)",
                key->ptr);
        return C_ERR;
    }
    rioInitWithBuffer(&payload, bytes);

    int type = rdbLoadObjectType(&payload);
    if (type == -1) {
        asyncMigrationReplyAckErrorFormat(c, "invalid payload type (%s)",
                key->ptr);
        return C_ERR;
    }

    robj *val = rdbLoadObject(type, &payload);
    if (val == NULL) {
        asyncMigrationReplyAckErrorFormat(c, "invalid payload body (%s)",
                key->ptr);
        return C_ERR;
    }

    dbAdd(c->db, key, val);
    return C_OK;
}

/* RESTORE-ASYNC string $key $ttlms $payload */
static int
restoreAsyncHandleOrReplyTypeString(client *c, robj *key) {
    if (lookupKeyWrite(c->db, key) != NULL) {
        asyncMigrationReplyAckErrorFormat(c, "the specified key already exists (%s)",
                key->ptr);
        return C_ERR;
    }

    robj *val = c->argv[4] = tryObjectEncoding(c->argv[4]);

    incrRefCount(val);
    dbAdd(c->db, key, val);
    return C_OK;
}

/* RESTORE-ASYNC list $key $ttlms $maxsize [$elem1 ...] */
static int
restoreAsyncHandleOrReplyTypeList(client *c, robj *key, int argc, robj **argv) {
    robj *val = lookupKeyWrite(c->db, key);
    if (val != NULL) {
        if (val->type != OBJ_LIST || val->encoding != OBJ_ENCODING_QUICKLIST) {
            asyncMigrationReplyAckErrorFormat(c, "wrong object type (%d/%d,expect=%d/%d)",
                    val->type, val->encoding, OBJ_LIST, OBJ_ENCODING_QUICKLIST);
            return C_ERR;
        }
    } else {
        val = createQuicklistObject();
        quicklistSetOptions(val->ptr, server.list_max_ziplist_size,
                server.list_compress_depth);
        dbAdd(c->db, key, val);
    }

    for (int i = 0; i < argc; i ++) {
        listTypePush(val, argv[i], LIST_TAIL);
    }
    return C_OK;
}

/* RESTORE-ASYNC hash $key $ttlms $maxsize [$hkey1 $hval1 ...] */
static int
restoreAsyncHandleOrReplyTypeHash(client *c, robj *key, int argc, robj **argv, long long size) {
    robj *val = lookupKeyWrite(c->db, key);
    if (val != NULL) {
        if (val->type != OBJ_HASH || val->encoding != OBJ_ENCODING_HT) {
            asyncMigrationReplyAckErrorFormat(c, "wrong object type (%d/%d,expect=%d/%d)",
                    val->type, val->encoding, OBJ_HASH, OBJ_ENCODING_HT);
            return C_ERR;
        }
    } else {
        val = createHashObject();
        if (val->encoding != OBJ_ENCODING_HT) {
            hashTypeConvert(val, OBJ_ENCODING_HT);
        }
        dbAdd(c->db, key, val);
    }

    if (size != 0) {
        dict *ht = val->ptr;
        dictExpand(ht, size);
    }

    for (int i = 0; i < argc; i += 2) {
        hashTypeSet(val, argv[i]->ptr, argv[i+1]->ptr, HASH_SET_COPY);
    }
    return C_OK;
}

/* RESTORE-ASYNC dict $key $ttlms $maxsize [$elem1 ...] */
static int
restoreAsyncHandleOrReplyTypeDict(client *c, robj *key, int argc, robj **argv, long long size) {
    robj *val = lookupKeyWrite(c->db, key);
    if (val != NULL) {
        if (val->type != OBJ_SET || val->encoding != OBJ_ENCODING_HT) {
            asyncMigrationReplyAckErrorFormat(c, "wrong object type (%d/%d,expect=%d/%d)",
                    val->type, val->encoding, OBJ_SET, OBJ_ENCODING_HT);
            return C_ERR;
        }
    } else {
        val = createSetObject();
        if (val->encoding != OBJ_ENCODING_HT) {
            setTypeConvert(val, OBJ_ENCODING_HT);
        }
        dbAdd(c->db, key, val);
    }

    if (size != 0) {
        dict *ht = val->ptr;
        dictExpand(ht, size);
    }

    for (int i = 0; i < argc; i ++) {
        setTypeAdd(val, argv[i]->ptr);
    }
    return C_OK;
}

/* RESTORE-ASYNC zset $key $ttlms $maxsize [$elem1 $score1 ...] */
static int
restoreAsyncHandleOrReplyTypeZSet(client *c, robj *key, int argc, robj **argv, long long size) {
    double *scores = zmalloc(sizeof(double) * (argc / 2));
    for (int i = 1, j = 0; i < argc; i += 2, j ++) {
        uint64_t u64;
        if (decodeUint64FromRawStringObject(argv[i], &u64) != C_OK) {
            asyncMigrationReplyAckErrorFormat(c, "invalid value of score[%d] (%s)",
                    j, argv[i]->ptr);
            zfree(scores);
            return C_ERR;
        }
        scores[j] = convertRawBitsToDouble(u64);
    }

    robj *val = lookupKeyWrite(c->db, key);
    if (val != NULL) {
        if (val->type != OBJ_ZSET || val->encoding != OBJ_ENCODING_SKIPLIST) {
            asyncMigrationReplyAckErrorFormat(c, "wrong object type (%d/%d,expect=%d/%d)",
                    val->type, val->encoding, OBJ_ZSET, OBJ_ENCODING_SKIPLIST);
            zfree(scores);
            return C_ERR;
        }
    } else {
        val = createZsetObject();
        if (val->encoding != OBJ_ENCODING_SKIPLIST) {
            zsetConvert(val, OBJ_ENCODING_SKIPLIST);
        }
        dbAdd(c->db, key, val);
    }

    if (size != 0) {
        zset *zs = val->ptr;
        dict *ht = zs->dict;
        dictExpand(ht, size);
    }

    for (int i = 0, j = 0; i < argc; i += 2, j ++) {
        int flags = ZADD_NONE;
        zsetAdd(val, scores[j], argv[i]->ptr, &flags, NULL);
    }
    zfree(scores);
    return C_OK;
}

/* *
 * RESTORE-ASYNC delete $key
 *               expire $key $ttlms
 *               object $key $ttlms $payload
 *               string $key $ttlms $payload
 *               list   $key $ttlms $maxsize [$elem1 ...]
 *               hash   $key $ttlms $maxsize [$hkey1 $hval1 ...]
 *               dict   $key $ttlms $maxsize [$elem1 ...]
 *               zset   $key $ttlms $maxsize [$elem1 $score1 ...]
 * */
void
restoreAsyncCommand(client *c) {
    if (asyncMigrationClientStatusOrBlock(c, 0)) {
        asyncMigrationReplyAckErrorFormat(c, "the specified DB is being migrated");
        return;
    }

    const char *cmd = "(nil)";
    if (c->argc <= 1) {
        goto bad_arguments_number;
    }
    cmd = c->argv[1]->ptr;

    if (c->argc <= 2) {
        goto bad_arguments_number;
    }
    robj *key = c->argv[2];

    /* RESTORE-ASYNC delete $key */
    if (!strcasecmp(cmd, "delete")) {
        if (c->argc != 3) {
            goto bad_arguments_number;
        }
        if (restoreAsyncHandleOrReplyDeleteKey(c, key) == C_OK) {
            goto success_common_reply;
        }
        return;
    }

    if (c->argc <= 3) {
        goto bad_arguments_number;
    }
    long long ttlms;
    if (getLongLongFromObject(c->argv[3], &ttlms) != C_OK || ttlms < 0) {
        asyncMigrationReplyAckErrorFormat(c, "invalid value of ttlms (%s)",
                c->argv[3]->ptr);
        return;
    }

    /* RESTORE-ASYNC expire $key $ttlms */
    if (!strcasecmp(cmd, "expire")) {
        if (c->argc != 4) {
            goto bad_arguments_number;
        }
        if (restoreAsyncHandleOrReplyExpireKey(c, key) == C_OK) {
            goto success_common_ttlms;
        }
        return;
    }

    /* RESTORE-ASYNC object $key $ttlms $payload */
    if (!strcasecmp(cmd, "object")) {
        if (c->argc != 5) {
            goto bad_arguments_number;
        }
        if (restoreAsyncHandleOrReplyTypeObject(c, key) == C_OK) {
            goto success_common_ttlms;
        }
        return;
    }

    /* RESTORE-ASYNC string $key $ttlms $payload */
    if (!strcasecmp(cmd, "string")) {
        if (c->argc != 5) {
            goto bad_arguments_number;
        }
        if (restoreAsyncHandleOrReplyTypeString(c, key) == C_OK) {
            goto success_common_ttlms;
        }
        return;
    }

    if (c->argc <= 4) {
        goto bad_arguments_number;
    }
    long long maxsize;
    if (getLongLongFromObject(c->argv[4], &maxsize) != C_OK || maxsize < 0) {
        asyncMigrationReplyAckErrorFormat(c, "invalid value of maxsize (%s)",
                c->argv[4]->ptr);
        return;
    }
    int argc = c->argc - 5; robj **argv = &c->argv[5];

    /* RESTORE-ASYNC list $key $ttlms $maxsize [$elem1 ...] */
    if (!strcasecmp(cmd, "list")) {
        if (argc <= 0) {
            goto bad_arguments_number;
        }
        if (restoreAsyncHandleOrReplyTypeList(c, key, argc, argv) == C_OK) {
            goto success_common_ttlms;
        }
        return;
    }

    /* RESTORE-ASYNC hash $key $ttlms $maxsize [$hkey1 $hval1 ...] */
    if (!strcasecmp(cmd, "hash")) {
        if (argc <= 0 || argc % 2 != 0) {
            goto bad_arguments_number;
        }
        if (restoreAsyncHandleOrReplyTypeHash(c, key, argc, argv, maxsize) == C_OK) {
            goto success_common_ttlms;
        }
        return;
    }

    /* RESTORE-ASYNC dict $key $ttlms $maxsize [$elem1 ...] */
    if (!strcasecmp(cmd, "dict")) {
        if (argc <= 0) {
            goto bad_arguments_number;
        }
        if (restoreAsyncHandleOrReplyTypeDict(c, key, argc, argv, maxsize) == C_OK) {
            goto success_common_ttlms;
        }
        return;
    }

    /* RESTORE-ASYNC zset $key $ttlms $maxsize [$elem1 $score1 ...] */
    if (!strcasecmp(cmd, "zset")) {
        if (argc <= 0 || argc % 2 != 0) {
            goto bad_arguments_number;
        }
        if (restoreAsyncHandleOrReplyTypeZSet(c, key, argc, argv, maxsize) == C_OK) {
            goto success_common_ttlms;
        }
        return;
    }

    asyncMigrationReplyAckErrorFormat(c, "unknown command (cmd=%s,argc=%d)",
            cmd, c->argc);
    return;

success_common_ttlms:
    if (ttlms != 0) {
        setExpire(c, c->db, key, mstime() + ttlms);
    } else {
        removeExpire(c->db, key);
    }
    signalModifiedKey(c->db, key);
    server.dirty ++;

success_common_reply:
    asyncMigrationReplyAckString(c, "OK");
    return;

bad_arguments_number:
    asyncMigrationReplyAckErrorFormat(c, "invalid arguments (cmd=%s,argc=%d)",
            cmd, c->argc);
    return;
}

/* ============================ Command: RESTORE-ASYNC-ACK ================================= */

static int
restoreAsyncAckHandle(client *c) {
    asyncMigrationClient *ac = getAsyncMigrationClient(c->db->id);
    if (ac->c != c) {
        addReplyErrorFormat(c, "invalid client, permission denied");
        return C_ERR;
    }

    long long errcode;
    if (getLongLongFromObject(c->argv[1], &errcode) != C_OK) {
        addReplyErrorFormat(c, "invalid value of errcode (%s)",
                (char *)c->argv[1]->ptr);
        return C_ERR;
    }

    if (errcode != 0) {
        serverLog(LL_WARNING, "async_migration: error[%d] (%s)",
                (int)errcode, (char *)c->argv[2]->ptr);
        return C_ERR;
    }

    batchedObjectIterator *it = ac->iterator;
    if (it == NULL) {
        serverLog(LL_WARNING, "async_migration: nil batched iterator");
        addReplyError(c, "invalid iterator (nil)");
        return C_ERR;
    }
    if (ac->sending_msgs == 0) {
        serverLog(LL_WARNING, "async_migration: not sending messages");
        addReplyError(c, "invalid iterator (sending_msgs=0)");
        return C_ERR;
    }
    it->delivered_msgs ++;

    ac->lastuse = mstime();
    ac->sending_msgs -= 1;
    ac->sending_msgs += asyncMigrationNextInMicroseconds(ac, 2, 10);

    if (ac->sending_msgs != 0) {
        return C_OK;
    }
    asyncMigrationClientInterrupt(ac, NULL);

    if (listLength(it->released_keys) != 0) {
        list *ll = it->released_keys;

        for (int i = 0; i < c->argc; i ++) {
            decrRefCount(c->argv[i]);
        }
        zfree(c->argv);

        c->argc = 1 + listLength(ll);
        c->argv = zmalloc(sizeof(robj *) * c->argc);

        for (int i = 1; i < c->argc; i ++) {
            listNode *head = listFirst(ll);
            robj *key = listNodeValue(head);

            if (dbAsyncDelete(c->db, key)) {
                signalModifiedKey(c->db, key);
                server.dirty ++;
            }
            c->argv[i] = key;
            incrRefCount(key);

            listDelNode(ll, head);
        }
        c->argv[0] = createStringObject("DEL", 3);
    }

    ac->iterator = NULL;
    freeBatchedObjectIterator(it);
    return C_OK;
}

/* *
 * RESTORE-ASYNC-ACK $errno $message
 * */
void
restoreAsyncAckCommand(client *c) {
    if (restoreAsyncAckHandle(c) != C_OK) {
        c->flags |= CLIENT_CLOSE_AFTER_REPLY;
    }
}

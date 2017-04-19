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

static size_t
sdslenOrElse(robj *o, size_t len) {
    return sdsEncodedObject(o) ? sdslen(o->ptr) : len;
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
            robj *obj = createStringObject((const char *)s[i], sdslen(s[i]));
            *n += sdslenOrElse(obj, 8);
            listAddNodeTail(l, obj);
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

static robj *
createRawStringObjectFromUint64(uint64_t v) {
    uint64_t p = intrev64ifbe(v);
    return createRawStringObject((char *)&p, sizeof(p));
}

static int
getUint64FromRawStringObject(robj *o, uint64_t *p) {
    if (sdsEncodedObject(o) && sdslen(o->ptr) == sizeof(uint64_t)) {
        *p = intrev64ifbe(*(uint64_t *)(o->ptr));
        return C_OK;
    }
    return C_ERR;
}

static long
numberOfRestoreCommandsFromObject(robj *val, long long maxbulks) {
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

    /* 1 x RESTORE-PAYLOAD */
    if (numbulks <= maxbulks) {
        return 1;
    }

    /* n x RESTORE-CHUNKED + 1 x RESTORE-FILLTTL */
    return 1 + (numbulks + maxbulks - 1) / maxbulks;
}

static long
estimateNumberOfRestoreCommands(redisDb *db, robj *key, long long maxbulks) {
    robj *val = lookupKeyWrite(db, key);
    if (val == NULL) {
        return 0;
    }
    return 1 + numberOfRestoreCommandsFromObject(val, maxbulks);
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

    int sending_msgs = 0;

    asyncMigrationClient *ac = getAsyncMigrationClient(c->db->id);
    if (ac->c == c) {
        if (ac->init == 0) {
            ac->init = 1;
            if (server.requirepass != NULL) {
                /* RESTORE-ASYNC-AUTH $password */
                addReplyMultiBulkLen(c, 2);
                addReplyBulkCString(c, "RESTORE-ASYNC-AUTH");
                addReplyBulkCString(c, server.requirepass);
                sending_msgs ++;
            }
            do {
                /* RESTORE-ASYNC select $db */
                addReplyMultiBulkLen(c, 3);
                addReplyBulkCString(c, "RESTORE-ASYNC");
                addReplyBulkCString(c, "select");
                addReplyBulkLongLong(c, c->db->id);
                sending_msgs ++;
            } while (0);
        }
    }

    do {
        /* RESTORE-ASYNC del $key */
        addReplyMultiBulkLen(c, 3);
        addReplyBulkCString(c, "RESTORE-ASYNC");
        addReplyBulkCString(c, "del");
        addReplyBulk(c, key);
        sending_msgs ++;
    } while(0);

    long n = numberOfRestoreCommandsFromObject(val, maxbulks);
    if (n != 1) {
        it->stage = STAGE_CHUNKED;
    } else {
        it->stage = STAGE_PAYLOAD;
    }
    return sending_msgs;
}

extern void createDumpPayload(rio *payload, robj *o);

static int
singleObjectIteratorNextStagePayload(client *c, singleObjectIterator *it) {
    serverAssert(it->stage == STAGE_PAYLOAD);
    robj *key = it->key;
    robj *val = it->val;
    long long ttl = 0;
    if (it->expire != -1) {
        ttl = it->expire - mstime();
        if (ttl < 1) {
            ttl = 1;
        }
    }
    if (val->type != OBJ_STRING) {
        rio payload;
        createDumpPayload(&payload, val);
        do {
            /* RESTORE-ASYNC object $key $ttl $payload */
            addReplyMultiBulkLen(c, 5);
            addReplyBulkCString(c, "RESTORE-ASYNC");
            addReplyBulkCString(c, "object");
            addReplyBulk(c, key);
            addReplyBulkLongLong(c, ttl);
            addReplyBulkSds(c, payload.io.buffer.ptr);
        } while (0);
    } else {
        do {
            /* RESTORE-ASYNC string $key $ttl $payload */
            addReplyMultiBulkLen(c, 5);
            addReplyBulkCString(c, "RESTORE-ASYNC");
            addReplyBulkCString(c, "string");
            addReplyBulk(c, key);
            addReplyBulkLongLong(c, ttl);
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
    long long ttl = 0;
    if (it->expire != -1) {
        ttl = it->expire - mstime();
        if (ttl < 1) {
            ttl = 1;
        }
    }
    do {
        /* RESTORE-ASYNC expire $key $ttl */
        addReplyMultiBulkLen(c, 4);
        addReplyBulkCString(c, "RESTORE-ASYNC");
        addReplyBulkCString(c, "expire");
        addReplyBulk(c, key);
        addReplyBulkLongLong(c, ttl);
    } while (0);

    it->stage = STAGE_DONE;
    return 1;
}

extern zskiplistNode* zslGetElementByRank(zskiplist *zsl, unsigned long rank);

static int
singleObjectIteratorNextStageChunked(client *c, singleObjectIterator *it,
        long long timeout, unsigned int maxbulks, unsigned int maxbytes) {
    serverAssert(it->stage == STAGE_CHUNKED);
    robj *key = it->key;
    robj *val = it->val;
    long long ttl = timeout * 3;
    if (ttl < 1000) {
        ttl = 1000;
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
        serverPanic("invalid object.type=%d of singleObjectIterator", val->type);
    }

    list *ll = listCreate();
    listSetFreeMethod(ll, decrRefCountVoid);

    long long hint = 0, len = 0, more = 1;

    if (val->type == OBJ_LIST) {
        hint = listTypeLength(val);
        listTypeIterator *li = listTypeInitIterator(val, it->lindex, LIST_TAIL);
        do {
            listTypeEntry entry;
            if (listTypeNext(li, &entry)) {
                quicklistEntry *qe = &(entry.entry);
                robj *obj;
                if (qe->value) {
                    obj = createStringObject((const char *)qe->value, qe->sz);
                } else {
                    obj = createStringObjectFromLongLong(qe->longval);
                }
                len += sdslenOrElse(obj, 8);
                listAddNodeTail(ll, obj);

                it->lindex ++;
            } else {
                more = 0;
            }
        } while (more && listLength(ll) < maxbulks && len < maxbytes);
        listTypeReleaseIterator(li);
    }

    if (val->type == OBJ_ZSET) {
        hint = zsetLength(val);
        zset *zs = val->ptr;
        long long rank = (long long)zsetLength(val) - it->zindex;
        zskiplistNode *node = (rank >= 1) ? zslGetElementByRank(zs->zsl, rank) : NULL;
        do {
            if (node != NULL) {
                robj *field = createStringObject((const char *)node->ele, sdslen(node->ele));
                len += sdslenOrElse(field, 8);
                listAddNodeTail(ll, field);

                uint64_t u8 = convertDoubleToRawBits(node->score);
                robj *score = createRawStringObjectFromUint64(u8);
                len += sdslenOrElse(score, 8);
                listAddNodeTail(ll, score);

                node = node->backward;
                it->zindex ++;
            } else {
                more = 0;
            }
        } while (more && listLength(ll) < maxbulks && len < maxbytes);
    }

    if (val->type == OBJ_HASH || val->type == OBJ_SET) {
        int loop = maxbulks * 10;
        if (loop < 100) {
            loop = 100;
        }
        dict *ht = val->ptr;
        hint = dictSize(ht);
        void *pd[] = {ll, val, &len};
        do {
            it->cursor = dictScan(ht, it->cursor, singleObjectIteratorScanCallback, NULL, pd);
            if (it->cursor == 0) {
                more = 0;
            }
        } while (more && listLength(ll) < maxbulks && len < maxbytes && (-- loop) >= 0);
    }

    int sending_msgs = 0;

    if (listLength(ll) != 0) {
        /* RESTORE-ASYNC list/hash/zset/dict $key $ttl $hint [$arg1 ...] */
        addReplyMultiBulkLen(c, 5 + listLength(ll));
        addReplyBulkCString(c, "RESTORE-ASYNC");
        addReplyBulkCString(c, type);
        addReplyBulk(c, key);
        addReplyBulkLongLong(c, ttl);
        addReplyBulkLongLong(c, hint);

        while (listLength(ll) != 0) {
            listNode *head = listFirst(ll);
            robj *obj = listNodeValue(head);
            addReplyBulk(c, obj);
            listDelNode(ll, head);
        }
        sending_msgs ++;
    }
    listRelease(ll);

    if (!more) {
        it->stage = STAGE_FILLTTL;
    }
    return sending_msgs;
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

/* ============================ Iterators: batchedObjectIterator =========================== */

typedef struct {
    long long timeout;
    long long maxbulks;
    long long maxbytes;
    dict *keys;
    list *iterator_list;
    list *released_keys;
    long estimate_msgs;
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
    it->estimate_msgs = 0;
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
    if (dictFind(it->keys, key->ptr) != NULL) {
        return 0;
    }
    dictAdd(it->keys, sdsdup(key->ptr), NULL);

    listAddNodeTail(it->iterator_list, createSingleObjectIterator(key));
    it->estimate_msgs += estimateNumberOfRestoreCommands(db, key, it->maxbulks);
    return 1;
}

/* ============================ Clients for Asynchronous Migration ========================= */

static asyncMigrationClient*
getAsyncMigrationClient(int db) {
    return &server.async_migration_clients[db];
}

static void
notifyAsyncMigrationClient(asyncMigrationClient *ac, const char *errmsg) {
    batchedObjectIterator *it = ac->batched_iterator;
    list *ll = ac->blocked_clients;
    while (listLength(ll) != 0) {
        listNode *head = listFirst(ll);
        client *c = listNodeValue(head);
        serverAssert(c->migration_fence == ll);

        if (errmsg != NULL) {
            addReplyError(c, errmsg);
        } else {
            addReplyLongLong(c, (it != NULL) ? listLength(it->released_keys) : -1);
        }
        listDelNode(ll, head);

        c->migration_fence = NULL;
        unblockClient(c);
    }
}

void
unblockClientFromAsyncMigration(client *c) {
    list *ll = c->migration_fence;
    if (ll == NULL) {
        return;
    }
    listNode *node = listSearchKey(ll, c);
    serverAssert(node != NULL);

    c->migration_fence = NULL;
    listDelNode(ll, node);
}

void
releaseClientFromAsyncMigration(client *c) {
    asyncMigrationClient *ac = getAsyncMigrationClient(c->db->id);
    serverAssert(ac->c == c);

    batchedObjectIterator *it = ac->batched_iterator;

    serverLog(LL_WARNING, "async_migration: closed connection %s:%d (DB=%d): "
            "sending_msgs = %ld, send/recv = %lld/%lld, "
            "blocked_clients = %ld, batched_iterator = %ld"
            "timeout = %lld(ms), elapsed = %lld(ms)",
            ac->host, ac->port, c->db->id,
            ac->sending_msgs, ac->send_total, ac->recv_total,
            (long)listLength(ac->blocked_clients), (it != NULL) ? (long)listLength(it->iterator_list) : -1,
            ac->timeout, mstime() - ac->lastuse);

    notifyAsyncMigrationClient(ac, "interrupted: closed connection");

    sdsfree(ac->host);
    if (it != NULL) {
        freeBatchedObjectIterator(it);
    }
    listRelease(ac->blocked_clients);

    c->flags &= ~CLIENT_ASYNC_MIGRATION;

    memset(ac, 0, sizeof(*ac));
}

static int
closeAsyncMigartionClientErrorFormat(int db, const char *fmt, ...) {
    asyncMigrationClient *ac = getAsyncMigrationClient(db);
    if (ac->c == NULL) {
        return 0;
    }
    va_list ap;
    va_start(ap, fmt);
    sds errmsg = sdscatvprintf(sdsempty(), fmt, ap);
    va_end(ap);

    serverLog(LL_WARNING, "async_migration: closed connection %s:%d (DB=%d), (%s)",
            ac->host, ac->port, db, errmsg);

    notifyAsyncMigrationClient(ac, errmsg);
    freeClient(ac->c);

    sdsfree(errmsg);

    serverAssert(ac->c == NULL && ac->batched_iterator == NULL);
    return 1;
}

static asyncMigrationClient *
openAsyncMigrationClient(int db, sds host, int port, int reuse, long long timeout) {
    asyncMigrationClient *ac = getAsyncMigrationClient(db);
    if (ac->c != NULL && reuse) {
        if (ac->port == port && !strcmp(ac->host, host)) {
            return ac;
        }
    }

    int fd = anetTcpNonBlockConnect(server.neterr, host, port);
    if (fd == -1) {
        serverLog(LL_WARNING, "async_migration: connect to %s:%d (DB=%d) failed, %s",
            host, port, db, server.neterr);
        return NULL;
    }

    anetEnableTcpNoDelay(server.neterr, fd);

    int wait = timeout;
    if (wait > 10) {
        wait = 10;
    }
    if ((aeWait(fd, AE_WRITABLE, wait) & AE_WRITABLE) == 0) {
        serverLog(LL_WARNING, "async_migration: aeWait %s:%d (DB=%d) failed, io error or timeout",
                host, port, db);
        close(fd);
        return NULL;
    }

    client *c = createClient(fd);
    if (c == NULL) {
        serverLog(LL_WARNING, "async_migration: createClient %s:%d (DB=%d) failed, %s",
                host, port, db, server.neterr);
        return NULL;
    }
    if (selectDb(c, db) != C_OK) {
        serverLog(LL_WARNING, "async_migration: selectDb %s:%d (DB=%d) failed",
                host, port, db);
        freeClient(c);
        return NULL;
    }
    closeAsyncMigartionClientErrorFormat(db, "interrupted: replaced by %s:%d reuse = %d",
            host, port, reuse);

    c->flags |= CLIENT_ASYNC_MIGRATION;
    c->authenticated = 1;

    ac->c = c;
    ac->init = 0;
    ac->host = sdsdup(host);
    ac->port = port;
    ac->timeout = timeout;
    ac->lastuse = mstime();
    ac->sending_msgs = 0;
    ac->send_total = 0;
    ac->recv_total = 0;
    ac->blocked_clients = listCreate();
    ac->batched_iterator = NULL;

    serverLog(LL_WARNING, "async_migration: connect to %s:%d (DB=%d) OK",
            host, port, db);
    return ac;
}

static int
testAsyncMigrationClientStatusOrBlock(client *c, robj *key, int wait) {
    asyncMigrationClient *ac = getAsyncMigrationClient(c->db->id);
    if (ac->c == NULL) {
        return 0;
    }

    batchedObjectIterator *it = ac->batched_iterator;
    if (it == NULL) {
        return 0;
    }
    if (key != NULL && !batchedObjectIteratorContains(it, key)) {
        return 0;
    }
    if (!wait) {
        return 1;
    }
    serverAssert(c->migration_fence == NULL);

    list *ll = ac->blocked_clients;

    c->migration_fence = ll;
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
        batchedObjectIterator *it = ac->batched_iterator;
        long long timeout = (it != NULL) ? ac->timeout : 1000 * 60;
        if (mstime() - ac->lastuse <= timeout) {
            continue;
        }
        closeAsyncMigartionClientErrorFormat(db, (it != NULL) ?
                "interrupted: migration timeout" : "interrupted: idle timeout");
    }
}

/* ============================ Command: MIGRATE-ASNYC-DUMP ================================ */

/* *
 * MIGRATE-ASYNC-DUMP $timeout $maxbulks $key1 [$key2 ...]
 * */
void
migrateAsyncDumpCommand(client *c) {
    if (c->argc <= 3) {
        addReplyError(c, "wrong number of arguments for MIGRATE-ASYNC-DUMP");
        return;
    }

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
            !(maxbulks >= 0 && maxbulks <= INT_MAX)) {
        addReplyErrorFormat(c, "invalid value of maxbulks (%s)",
                (char *)c->argv[2]->ptr);
        return;
    }
    if (maxbulks == 0) {
        maxbulks = 200;
    }

    batchedObjectIterator *it = createBatchedObjectIterator(timeout, maxbulks, INT_MAX);
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
calcAsyncMigrationClientBufferLimit(unsigned int maxbytes) {
    clientBufferLimitsConfig *p = &server.client_obuf_limits[CLIENT_TYPE_NORMAL];
    if (p->soft_limit_bytes != 0 && p->soft_limit_bytes < maxbytes) {
        maxbytes = p->soft_limit_bytes;
    }
    if (p->hard_limit_bytes != 0 && p->hard_limit_bytes < maxbytes) {
        maxbytes = p->hard_limit_bytes;
    }
    return maxbytes;
}

static long
moreAsyncMigrationMessageMicroseconds(asyncMigrationClient *ac, long atleast, long long usecs) {
    batchedObjectIterator *it = ac->batched_iterator;
    long long deadline = ustime() + usecs;
    long msgs = 0;
    while (batchedObjectIteratorHasNext(it)) {
        if ((long long)getClientOutputBufferMemoryUsage(ac->c) >= it->maxbytes) {
            return msgs;
        }
        if ((msgs += batchedObjectIteratorNext(ac->c, it)) >= atleast && deadline <= ustime()) {
            return msgs;
        }
    }
    return msgs;
}

/* *
 * MIGRATE-ASYNC $host $port $timeout $maxbulks $maxbytes $key1 [$key2 ...]
 * */
void
migrateAsyncCommand(client *c) {
    if (c->argc <= 6) {
        addReplyError(c, "wrong number of arguments for MIGRATE-ASYNC");
        return;
    }
    if (testAsyncMigrationClientStatusOrBlock(c, NULL, 0) != 0) {
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
            !(maxbulks >= 0 && maxbulks <= INT_MAX)) {
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
            !(maxbytes >= 0 && maxbytes <= INT_MAX)) {
        addReplyErrorFormat(c, "invalid value of maxbytes (%s)",
                (char *)c->argv[5]->ptr);
        return;
    }
    if (maxbytes == 0) {
        maxbytes = 1024 * 1024;
    }
    if (maxbytes > INT_MAX / 2) {
        maxbytes = INT_MAX / 2;
    }
    maxbytes = calcAsyncMigrationClientBufferLimit(maxbytes);

    asyncMigrationClient *ac = openAsyncMigrationClient(c->db->id, host, port, 1, timeout);
    if (ac == NULL) {
        addReplyErrorFormat(c, "create client to %s:%d failed", host, (int)port);
        return;
    }
    serverAssert(ac->sending_msgs == 0);
    serverAssert(listLength(ac->blocked_clients) == 0 && ac->batched_iterator == NULL);

    batchedObjectIterator *it = createBatchedObjectIterator(timeout, maxbulks, maxbytes);
    for (int i = 6; i < c->argc; i ++) {
        batchedObjectIteratorAddKey(c->db, it, c->argv[i]);
    }

    ac->timeout = timeout;
    ac->lastuse = mstime();
    ac->batched_iterator = it;
    ac->sending_msgs = moreAsyncMigrationMessageMicroseconds(ac, 4, 500);
    ac->send_total += ac->sending_msgs;

    testAsyncMigrationClientStatusOrBlock(c, NULL, 1);

    if (ac->sending_msgs != 0) {
        return;
    }
    notifyAsyncMigrationClient(ac, NULL);

    ac->batched_iterator = NULL;
    freeBatchedObjectIterator(it);
}

/* ============================ TODO == TODO == TODO ======================================= */

int *migrateAsyncGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    /* TODO */
    (void)cmd;
    (void)argv;
    (void)argc;
    (void)numkeys;
    return NULL;
}

void migrateAsyncFenceCommand(client *c) {
    /* TODO */
    (void)c;
}

void migrateAsyncStatusCommand(client *c) {
    /* TODO */
    (void)c;
}

void migrateAsyncCancelCommand(client *c) {
    /* TODO */
    (void)c;
}

int *restoreAsyncGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    /* TODO */
    (void)cmd;
    (void)argv;
    (void)argc;
    (void)numkeys;
    return NULL;
}

void restoreAsyncCommand(client *c) {
    /* TODO */
    (void)c;
}

void restoreAsyncAuthCommand(client *c) {
    /* TODO */
    (void)c;
}

void restoreAsyncAckCommand(client *c) {
    /* TODO */
    (void)c;
}

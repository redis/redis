/*
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * BLESS - protect keys from eviction ("blessed" keys).
 *
 * The NO-EVICT flag lives in the kvBits byte following each kvobj.
 * It persists through RDB and AOF; ASM sends it after RESTORE.
 *
 * Each redisDb also keeps an in-RAM index of its NO-EVICT keys (db->blessed_keys)
 * for BLESS SCAN and INFO's blessed_keys count. It is per-DB (like db->expires)
 * so it stays correct across SWAPDB. The eviction path never consults it -
 * blessIsNoEvict() reads the bit directly from kvBits.
 *
 * Eviction (see evict.c): blessed keys are never chosen as victims. If eviction
 * can't free enough because blessed keys hold the memory, used memory is allowed
 * to overshoot up to 1.25x maxmemory before writes are rejected with OOM - that
 * overshoot factor is the effective ceiling for a heavily-blessed keyspace.
 */

#include "server.h"
#include "cluster.h"
#include "vector.h"

/* Per-DB index of NO-EVICT key names. */
static dictType blessedDictType = {
    dictSdsHash,            /* hash function */
    NULL,                   /* key dup */
    NULL,                   /* val dup */
    dictSdsKeyCompare,      /* key compare */
    dictSdsDestructor,      /* key destructor */
    NULL,                   /* val destructor */
    NULL                    /* allow to resize */
};

/* kvstore-level metadata: running byte total of the sdsdup() key copies the index
 * owns, so MEMORY STATS overhead is O(1) instead of scanning the whole index (a
 * blessed dict can be large and INFO must stay cheap). The counter rides the
 * kvstore, so SWAPDB (pointer swap), async DB-empty (fresh kvstore) and
 * kvstoreEmpty (onEmpty reset) keep it correct with no external bookkeeping. */
typedef struct {
    size_t sds_bytes;
} blessedKvsMeta;

/* Pointer to the running sds-bytes counter in a blessed kvstore's metadata. */
static size_t *blessedBytesRef(kvstore *kvs) {
    return &((blessedKvsMeta *)kvstoreGetMetadata(kvs))->sds_bytes;
}

static size_t blessedKvsMetaBytes(kvstore *kvs) {
    UNUSED(kvs);
    return sizeof(blessedKvsMeta);
}

static void blessedKvsOnEmpty(kvstore *kvs) {
    *blessedBytesRef(kvs) = 0;
}

static kvstoreType blessedKvstoreType = {
    blessedKvsMetaBytes,   /* kvstore metadata size */
    NULL,                  /* dict metadata size */
    NULL,                  /* can free dict */
    blessedKvsOnEmpty,     /* on kvstore empty */
    NULL,                  /* on dict empty */
};

/* Create a per-DB blessed-keys index. Slot-partitioned like db->expires so a
 * slot migration (ASM) can drop a whole slot's entries in O(1). */
kvstore *blessedKvstoreCreate(int slot_count_bits, int flags) {
    return kvstoreCreate(&blessedKvstoreType, &blessedDictType, slot_count_bits, flags);
}

/* ---- per-DB index helpers (main thread only) ---- */

static void blessTrack(redisDb *db, sds keyname) {
    int slot = getKeySlot(keyname);
    dictEntry *de = kvstoreDictFind(db->blessed_keys, slot, keyname);
    if (de) return;
    sds dup = sdsdup(keyname);
    de = kvstoreDictAddRaw(db->blessed_keys, slot, dup, NULL);
    kvstoreDictSetVal(db->blessed_keys, slot, de, NULL);
    *blessedBytesRef(db->blessed_keys) += sdsAllocSize(dup);
}

static void blessUntrack(redisDb *db, sds keyname) {
    int slot = getKeySlot(keyname);
    dictEntry *de = kvstoreDictFind(db->blessed_keys, slot, keyname);
    if (!de) return;
    *blessedBytesRef(db->blessed_keys) -= sdsAllocSize(dictGetKey(de));
    kvstoreDictDelete(db->blessed_keys, slot, keyname);
}

int blessIsNoEvict(kvobj *kv) {
    debugServerAssert(kv->iskvobj);
    return kvobjBits(kv)->no_evict;
}

/* Update the bit and its derived index without reallocating the key. */
void blessSetNoEvict(redisDb *db, kvobj *kv, int enabled) {
    serverAssert(kv->iskvobj);
    kvobjBits(kv)->no_evict = enabled != 0;
    if (enabled) blessTrack(db, kvobjGetKey(kv));
    else blessUntrack(db, kvobjGetKey(kv));
}

/* Instance-wide blessed-key count (sum of the per-DB indexes), for INFO. */
unsigned long long blessedKeysCount(void) {
    unsigned long long n = 0;
    for (int i = 0; i < server.dbnum; i++)
        n += kvstoreSize(server.db[i].blessed_keys);
    return n;
}

/* Memory overhead of a DB's blessed index: the kvstore structure plus the sds
 * key copies it owns (tracked O(1) in the kvstore metadata). For MEMORY STATS. */
size_t blessedIndexMemUsage(redisDb *db) {
    return kvstoreMemUsage(db->blessed_keys) + *blessedBytesRef(db->blessed_keys);
}

/* A slot migration (ASM) moved entries out of db->blessed_keys into `moved` via
 * kvstoreMoveDict, which bypasses blessUntrack; reconcile the byte counter by
 * subtracting the moved keys' sizes. Main thread, before `moved` is freed. */
void blessedIndexReconcileMoved(redisDb *db, kvstore *moved) {
    size_t *bytes = blessedBytesRef(db->blessed_keys);
    kvstoreIterator it;
    kvstoreIteratorInit(&it, moved);
    dictEntry *de;
    while ((de = kvstoreIteratorNext(&it)) != NULL)
        *bytes -= sdsAllocSize(dictGetKey(de));
    kvstoreIteratorReset(&it);
}

/* Re-emit the flag after the value in command-format AOF and ASM. */
int blessRewrite(rio *r, robj *key, kvobj *kv) {
    if (!blessIsNoEvict(kv)) return C_OK;
    if (rioWriteBulkCount(r, '*', 4) == 0 ||
        rioWriteBulkString(r, "BLESS", 5) == 0 ||
        rioWriteBulkString(r, "SET", 3) == 0 ||
        rioWriteBulkObject(r, key) == 0 ||
        rioWriteBulkString(r, "NO-EVICT", 8) == 0)
        return C_ERR;
    return C_OK;
}

/* ---- commands ---- */

/* Shared body of BLESS SET and BLESS CLEAR. */
static void blessGenericCommand(client *c, int add) {
    if (strcasecmp(c->argv[3]->ptr, "no-evict")) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    robj *key = c->argv[2];
    robj *o = lookupKeyWrite(c->db, key);
    if (o == NULL) {
        addReplyErrorObject(c, shared.nokeyerr);
        return;
    }

    if (blessIsNoEvict(o) == add) {
        addReply(c, shared.czero);
        return;
    }
    blessSetNoEvict(c->db, o, add);

    keyModified(c, c->db, key, NULL, 1);
    notifyKeyspaceEvent(NOTIFY_GENERIC, add ? "bless" : "unbless", key, c->db->id);
    server.dirty++;
    addReply(c, shared.cone);
}

/* BLESS GET <key> -> array of the key's active flag names ([] if none).
 * Errors if the key does not exist. */
static void blessGetCommand(client *c) {
    robj *keyobj = c->argv[2];
    robj *o = lookupKeyReadWithFlags(c->db, keyobj, LOOKUP_NOTOUCH);
    if (o == NULL) {
        addReplyErrorObject(c, shared.nokeyerr);
        return;
    }
    int noevict = blessIsNoEvict(o);
    addReplyArrayLen(c, noevict);
    if (noevict)
        addReplyBulkCString(c, "NO-EVICT");
}

/* ---- BLESS SCAN ---- */

typedef struct {
    redisDb *db;
    vec *keys;     /* matches collected so far (index's own sds, not copied) */
    long sampled;  /* entries visited so far, bounds COUNT like SCAN does */
} blessScanData;

static void blessScanCallback(void *privdata, const dictEntry *de, dictEntryLink plink) {
    UNUSED(plink);
    blessScanData *d = privdata;
    d->sampled++;
    if (keyIsExpired(d->db, dictGetKey(de), NULL)) return;
    vecPush(d->keys, dictGetKey(de));
}

/* Same slot-skip rule as SCAN/KEYS/RANDOMKEY (db.c's accessKeysShouldSkipDictIndex):
 * don't enumerate keys in a slot this node can't currently serve, e.g. mid-ASM-import. */
static int blessScanShouldSkipDict(dict *d, int didx) {
    UNUSED(d);
    return !clusterCanAccessKeysInSlot(didx);
}

/* BLESS SCAN <cursor> <NO-EVICT> [COUNT <count>] - cursored scan of the current
 * DB's blessed index, filtered by flag. SCAN-style reply: [next-cursor, [key ...]].
 * Same semantics as scanGenericCommand: COUNT is a non-strict ball-park hint (the
 * loop stops once ~count entries were sampled, with a maxiterations guard against
 * a sparse table) and defaults to 1024 when omitted, just like plain SCAN. */
static void blessScanCommand(client *c) {
    unsigned long long cursor;
    if (parseScanCursorOrReply(c, c->argv[2], &cursor) == C_ERR) return;
    /* The flag is required; only NO-EVICT exists today. */
    if (strcasecmp(c->argv[3]->ptr, "no-evict")) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    long count = 1024; /* ball-park default, like SCAN's COUNT */
    if (c->argc == 6) {
        if (strcasecmp(c->argv[4]->ptr, "count")) {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
        if (getRangeLongFromObjectOrReply(c, c->argv[5], 1, LONG_MAX, &count, NULL) != C_OK)
            return;
    } else if (c->argc != 4) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    vec keys;
    vecInit(&keys, NULL, 0);
    blessScanData data = { .db = c->db, .keys = &keys };
    long maxiterations = (count > LONG_MAX / 10) ? LONG_MAX : count * 10;
    do {
        cursor = kvstoreScan(c->db->blessed_keys, cursor, -1, blessScanCallback,
                             blessScanShouldSkipDict, &data);
    } while (cursor && maxiterations-- && data.sampled < count);

    addReplyArrayLen(c, 2);
    addReplyBulkLongLong(c, cursor);
    addReplyArrayLen(c, vecSize(&keys));
    for (size_t i = 0; i < vecSize(&keys); i++) {
        sds key = vecGet(&keys, i);
        addReplyBulkCBuffer(c, key, sdslen(key));
    }
    vecRelease(&keys);
}

/* BLESS is a container. All subcommands share this dispatcher (OBJECT-style);
 * per-subcommand arity and key specs are enforced by the command table.
 * SCAN reports the current DB only, like SCAN. (The instance-wide blessed-key
 * count is exposed via INFO's blessed_keys field, not a command.) */
void blessCommand(client *c) {
    const char *sub = c->argv[1]->ptr;
    if (!strcasecmp(sub, "set")) {
        blessGenericCommand(c, 1);          /* BLESS SET <key> NO-EVICT - enable protection */
    } else if (!strcasecmp(sub, "clear")) {
        blessGenericCommand(c, 0);          /* BLESS CLEAR <key> NO-EVICT - disable protection */
    } else if (!strcasecmp(sub, "get")) {
        blessGetCommand(c);
    } else if (!strcasecmp(sub, "scan")) {
        blessScanCommand(c);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

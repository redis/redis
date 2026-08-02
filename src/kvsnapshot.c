/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* kvsnapshot.c -- point-in-time snapshots of HASH values.
 *
 * A module can take a consistent, point-in-time view of the HASH keys in a DB
 * and read fields from it later (e.g. from a query worker under the GIL) while
 * the live keyspace keeps changing. Only HASH values are supported; the snapshot
 * read API rejects any other type.
 *
 * Mechanism (value-MVCC via a shared version store):
 *   - A snapshot captures a monotonic `epoch` at creation.
 *   - Before a hash field is changed (hashTypeSet / hashTypeDelete / field TTL
 *     expiry), its pre-image is recorded ONCE into a shared, per-DB store keyed
 *     by (key, field) as a version-ordered chain of {epoch, old-bytes}. One
 *     record serves every open snapshot regardless of how many are open (O(1)
 *     write, gated on server.snapshots_open so there is zero cost when idle).
 *   - A read as-of a snapshot resolves the first chain record with epoch greater
 *     than the snapshot's; if none, the field is unchanged since the snapshot and
 *     is read from the live hash.
 *   - On whole-key delete/overwrite the as-of-V hash is materialized into the
 *     snapshot before the live value is freed.
 *   - Records are reclaimed by a version watermark (the oldest open snapshot's
 *     epoch) when a snapshot is freed -- no per-record refcounts.
 *
 * See docs/design/kvsnapshot-hash-mvcc.md.
 */

#include "server.h"

/* An open point-in-time view of the HASH keys in one DB. Forward-declared as a
 * typedef in server.h; define the body here without re-typedef'ing. */
struct keyspaceSnapshot {
    uint64_t id;         /* handle for the DEBUG command */
    int dbid;            /* the DB this snapshot is for */
    int invalid;         /* snapshotted keyspace was discarded; all reads are absent */
    uint64_t base_epoch; /* reads resolve chain records with epoch > base_epoch */
    dict *preserved;     /* keyname(sds, borrowed from kvobj) -> frozen as-of-V hash
                            kvobj, materialized on whole-key delete/overwrite. */
};

/* --- shared hash version store -------------------------------------------- *
 *   gHStore[dbid]:  keyname(sds,dup) -> fieldstore(dict)
 *   fieldstore:     field(sds,dup)   -> snapHChain*
 * A chain holds a field's version-ordered pre-images; old==NULL is a tombstone
 * ("field did not exist before this epoch"). */
typedef struct { uint64_t epoch; sds old; } snapHRec;
typedef struct { snapHRec *r; int n, cap; } snapHChain;

static uint64_t gEpoch = 1;    /* bumped at each snapshot creation */
static dict **gHStore = NULL;  /* [server.dbnum] keystore dicts, lazily created */
static int *gSnapOpenDb = NULL; /* [server.dbnum] open snapshots per DB */
static uint64_t nextSnapshotId = 1;
static int inMaterialize = 0;  /* guard: materialization must not re-enter capture */

/* Bytes held by chains, store names and frozen kvobjs. Added at the allocation
 * site, subtracted in the dict destructors (plus the trim in hstoreReclaim). */
static void snapBytesAdd(size_t n) { server.stat_snapshot_preserved_bytes += (long long) n; }
static void snapBytesSub(size_t n) {
    server.stat_snapshot_preserved_bytes -= (long long) n;
    debugServerAssert(server.stat_snapshot_preserved_bytes >= 0);
}

static size_t snapHChainBytes(snapHChain *c) {
    size_t sz = zmalloc_usable_size(c);
    if (c->r) sz += zmalloc_usable_size(c->r);
    for (int i = 0; i < c->n; i++) if (c->r[i].old) sz += sdsAllocSize(c->r[i].old);
    return sz;
}

static void snapHChainFree(snapHChain *c) {
    snapBytesSub(snapHChainBytes(c));
    for (int i = 0; i < c->n; i++) if (c->r[i].old) sdsfree(c->r[i].old);
    zfree(c->r); zfree(c);
}
static void hstoreNameDtor(dict *d, void *name) { UNUSED(d); snapBytesSub(sdsAllocSize(name)); sdsfree(name); }
static sds hstoreNameDup(sds name) { sds s = sdsdup(name); snapBytesAdd(sdsAllocSize(s)); return s; }

static void hstoreFieldDtor(dict *d, void *v) { UNUSED(d); snapHChainFree(v); }
static dictType hstoreFieldDictType = {
    .hashFunction = dictSdsHash, .keyCompare = dictSdsKeyCompare,
    .keyDestructor = hstoreNameDtor, .valDestructor = hstoreFieldDtor,
};
static void hstoreKeyDtor(dict *d, void *v) { UNUSED(d); dictRelease(v); }
static dictType hstoreKeyDictType = {
    .hashFunction = dictSdsHash, .keyCompare = dictSdsKeyCompare,
    .keyDestructor = hstoreNameDtor, .valDestructor = hstoreKeyDtor,
};

/* preserved dict: key is the sds embedded in the frozen kvobj (borrowed); the
 * entry holds a reference on the kvobj and drops it on removal. Frozen kvobjs are
 * never mutated, so kvobjAllocSize() here matches the size accounted on add. */
static void preservedValDtor(dict *d, void *v) {
    UNUSED(d);
    if (!v) return;
    snapBytesSub(kvobjAllocSize(v));
    decrRefCount(v);
}
static dictType preservedDictType = {
    .hashFunction = dictSdsHash, .keyCompare = dictSdsKeyCompare,
    .keyDestructor = NULL, .valDestructor = preservedValDtor,
};

static kvobj *snapshotMaterializeHash(keyspaceSnapshot *s, robj *key, kvobj *live);

/* --- store helpers -------------------------------------------------------- */
static dict *hstoreDb(int dbid) {
    if (!gHStore) gHStore = zcalloc(sizeof(dict *) * server.dbnum);
    if (!gHStore[dbid]) gHStore[dbid] = dictCreate(&hstoreKeyDictType);
    return gHStore[dbid];
}
static dict *hstoreFields(int dbid, sds keyname) {
    if (!gHStore || !gHStore[dbid]) return NULL;
    return dictFetchValue(gHStore[dbid], keyname);
}
static void snapHChainPush(snapHChain *c, uint64_t epoch, sds old) {
    if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 2;
        size_t was = c->r ? zmalloc_usable_size(c->r) : 0;
        c->r = zrealloc(c->r, c->cap * sizeof(snapHRec));
        snapBytesAdd(zmalloc_usable_size(c->r) - was); }
    c->r[c->n].epoch = epoch; c->r[c->n].old = old; c->n++;
    if (old) snapBytesAdd(sdsAllocSize(old));
}
/* First record with epoch > base (records are epoch-ascending), or NULL. */
static snapHRec *snapHChainResolve(snapHChain *c, uint64_t base) {
    int lo = 0, hi = c->n;
    while (lo < hi) { int mid = (lo + hi) >> 1;
        if (c->r[mid].epoch > base) hi = mid; else lo = mid + 1; }
    return lo < c->n ? &c->r[lo] : NULL;
}

/* --- lifecycle ------------------------------------------------------------ */
void kvsnapshotInit(void) {
    server.keyspace_snapshots = listCreate();
    server.snapshots_open = 0;
    server.stat_snapshot_hash_deltas = 0;
    server.stat_snapshot_preserved_bytes = 0;
    server.stat_snapshot_evicted_drops = 0;
    gEpoch = 1; gHStore = NULL; nextSnapshotId = 1;
    gSnapOpenDb = zcalloc(sizeof(int) * server.dbnum);
}

keyspaceSnapshot *kvSnapshotCreate(int dbid) {
    keyspaceSnapshot *s = zmalloc(sizeof(*s));
    s->id = nextSnapshotId++;
    s->dbid = dbid;
    s->invalid = 0;
    s->base_epoch = gEpoch;   /* records with epoch > base_epoch are as-of-V */
    gEpoch++;                 /* separate this baseline from later writes */
    s->preserved = dictCreate(&preservedDictType);
    listAddNodeTail(server.keyspace_snapshots, s);
    server.snapshots_open++;
    gSnapOpenDb[dbid]++;
    return s;
}

/* Reclaim records no open snapshot can resolve to: watermark = min open
 * base_epoch, drop chain records with epoch <= watermark (no snapshot is older). */
static void hstoreReclaim(void) {
    if (!gHStore) return;
    uint64_t wm = gEpoch; /* no open snapshot => drop everything */
    listIter li; listNode *ln; listRewind(server.keyspace_snapshots, &li);
    while ((ln = listNext(&li))) { keyspaceSnapshot *x = listNodeValue(ln);
        if (x->invalid) continue;   /* can resolve nothing, must not hold the watermark */
        if (x->base_epoch < wm) wm = x->base_epoch; }
    for (int db = 0; db < server.dbnum; db++) {
        if (!gHStore[db]) continue;
        dictIterator *ki = dictGetSafeIterator(gHStore[db]); dictEntry *ke;
        while ((ke = dictNext(ki))) {
            dict *fs = dictGetVal(ke);
            dictIterator *fi = dictGetSafeIterator(fs); dictEntry *fe;
            while ((fe = dictNext(fi))) {
                snapHChain *c = dictGetVal(fe);
                int keep = c->n; /* first index with epoch > wm */
                for (int i = 0; i < c->n; i++) if (c->r[i].epoch > wm) { keep = i; break; }
                if (keep == c->n) dictDelete(fs, dictGetKey(fe));
                else if (keep > 0) {
                    for (int i = 0; i < keep; i++) if (c->r[i].old) {
                        snapBytesSub(sdsAllocSize(c->r[i].old)); sdsfree(c->r[i].old); }
                    memmove(c->r, c->r + keep, (c->n - keep) * sizeof(snapHRec));
                    c->n -= keep;
                }
            }
            dictReleaseIterator(fi);
            if (dictSize(fs) == 0) dictDelete(gHStore[db], dictGetKey(ke));
        }
        dictReleaseIterator(ki);
    }
}

void kvSnapshotFree(keyspaceSnapshot *s) {
    int dbid = s->dbid, invalid = s->invalid;
    listNode *ln = listSearchKey(server.keyspace_snapshots, s);
    if (ln) listDelNode(server.keyspace_snapshots, ln);
    dictRelease(s->preserved);
    zfree(s);
    /* An invalidated snapshot already gave up its counts at invalidation time. */
    if (!invalid) { server.snapshots_open--; gSnapOpenDb[dbid]--; }
    hstoreReclaim();
}

/* SWAPDB moved a keyspace to another index, so the snapshots of its values follow
 * it. Unconditional: a few assignments and a short walk on a cold admin path. */
void snapshotOnSwapDb(int id1, int id2) {
    if (gHStore) { dict *t = gHStore[id1]; gHStore[id1] = gHStore[id2]; gHStore[id2] = t; }
    int t = gSnapOpenDb[id1]; gSnapOpenDb[id1] = gSnapOpenDb[id2]; gSnapOpenDb[id2] = t;
    listIter li; listNode *ln; listRewind(server.keyspace_snapshots, &li);
    while ((ln = listNext(&li))) {
        keyspaceSnapshot *s = listNodeValue(ln);
        if (s->dbid == id1) s->dbid = id2;
        else if (s->dbid == id2) s->dbid = id1;
    }
}

/* A diskless full-sync swap discards the snapshotted keyspace entirely, so there
 * is nothing to retarget to: mark every open snapshot invalid (its reads become
 * absent) and release what it holds now rather than at the module's leisure. */
void snapshotInvalidateAll(void) {
    listIter li; listNode *ln; listRewind(server.keyspace_snapshots, &li);
    while ((ln = listNext(&li))) {
        keyspaceSnapshot *s = listNodeValue(ln);
        if (s->invalid) continue;
        s->invalid = 1;
        dictEmpty(s->preserved, NULL);
        server.snapshots_open--;
        gSnapOpenDb[s->dbid]--;
    }
    if (gHStore) for (int i = 0; i < server.dbnum; i++)
        if (gHStore[i]) { dictRelease(gHStore[i]); gHStore[i] = NULL; }
}

/* Free every still-open snapshot (graceful shutdown; leak-checker clean). */
void kvsnapshotFreeAll(void) {
    if (!server.keyspace_snapshots) return;
    listNode *ln;
    while ((ln = listFirst(server.keyspace_snapshots)) != NULL)
        kvSnapshotFree(listNodeValue(ln));
}

/* --- capture (write path) ------------------------------------------------- */

/* Record `field`'s pre-write value ONCE into the shared store for the current
 * epoch. Called from the hash write paths before the field is changed; O(1). */
void snapshotHashCapture(redisDb *db, kvobj *o, sds field) {
    if (inMaterialize) return;
    if (server.snapshots_open == 0) return;      /* zero cost when idle */
    if (gSnapOpenDb[db->id] == 0) return;        /* the gate above is not per-DB */
    sds keyname = kvobjGetKey(o);
    dict *ks = hstoreDb(db->id);
    dict *fs = dictFetchValue(ks, keyname);
    if (!fs) { fs = dictCreate(&hstoreFieldDictType); dictAdd(ks, hstoreNameDup(keyname), fs); }
    snapHChain *c = dictFetchValue(fs, field);
    if (c && c->n && c->r[c->n-1].epoch == gEpoch) return; /* already captured this epoch */
    robj *oldval = NULL;
    /* ACCESS_EXPIRED: capture even a TTL-lapsed field (it existed as-of-V).
     * AVOID_FIELD_DEL: never delete during the capture read. */
    hashTypeGetValueObject(db, o, field,
                           HFE_LAZY_ACCESS_EXPIRED | HFE_LAZY_AVOID_FIELD_DEL |
                           HFE_LAZY_NO_NOTIFICATION, &oldval, NULL, NULL);
    sds old = NULL;
    if (oldval) { robj *dec = getDecodedObject(oldval);
        old = sdsdup(dec->ptr); decrRefCount(dec); decrRefCount(oldval); }
    if (!c) { c = zcalloc(sizeof(*c)); snapBytesAdd(zmalloc_usable_size(c));
              dictAdd(fs, hstoreNameDup(field), c); }
    snapHChainPush(c, gEpoch, old);   /* old==NULL => tombstone */
    server.stat_snapshot_hash_deltas++;
}

/* Called from dbGenericDelete / dbSetValue before a HASH key's value is freed or
 * replaced wholesale: materialize the as-of-V hash into every open snapshot in
 * scope so untouched fields (only in `kv`) survive, then drop the shared entry.
 * A key evicted (DB_FLAG_KEY_EVICTED) while a snapshot is open is absent as-of-V,
 * full stop: it is dropped from the snapshots instead of preserved, and an
 * already-frozen copy is dropped too, so eviction actually frees memory. */
void snapshotHashPreserveOnRemove(redisDb *db, robj *key, kvobj *kv, int flags) {
    if (kv->type != OBJ_HASH) return;
    sds keyname = key->ptr;
    int preserve = !(flags & DB_FLAG_KEY_EVICTED);

    if (gSnapOpenDb[db->id]) {
        int dropped = 0;
        listIter li; listNode *ln; listRewind(server.keyspace_snapshots, &li);
        while ((ln = listNext(&li))) {
            keyspaceSnapshot *s = listNodeValue(ln);
            if (s->invalid || s->dbid != db->id) continue;
            if (!preserve) {
                dictDelete(s->preserved, keyname);  /* dtor drops the bytes */
                dropped = 1;
            } else if (dictFind(s->preserved, keyname) == NULL) {
                snapshotMaterializeHash(s, key, kv);
            }
        }
        if (dropped) server.stat_snapshot_evicted_drops++;
    }

    /* Runs on the evict path too, so a dropped key never reads back as a partial hash. */
    if (gHStore && gHStore[db->id]) dictDelete(gHStore[db->id], keyname);
}

/* Called from emptyDbStructure before a whole DB is wiped (FLUSHDB / FLUSHALL),
 * which bypasses the per-key delete hooks above. Materialize the as-of-V view of
 * every HASH key into each open snapshot in scope so untouched keys/fields
 * survive the wipe, then drop the DB's shared version store so a key recreated
 * after the flush cannot inherit a stale delta chain. */
void snapshotHashPreserveOnFlush(redisDb *db) {
    if (server.snapshots_open == 0) return;      /* zero cost when idle */

    /* Only iterate the keyspace if some open snapshot targets this DB. */
    if (gSnapOpenDb[db->id]) {
        listIter li; listNode *ln;
        kvstoreIterator kvs_it;
        kvstoreIteratorInit(&kvs_it, db->keys);
        dictEntry *de;
        while ((de = kvstoreIteratorNext(&kvs_it)) != NULL) {
            kvobj *kv = dictGetKV(de);
            if (kv->type != OBJ_HASH) continue;
            sds keyname = kvobjGetKey(kv);
            robj *keyobj = createStringObject(keyname, sdslen(keyname));
            listRewind(server.keyspace_snapshots, &li);
            while ((ln = listNext(&li))) {
                keyspaceSnapshot *s = listNodeValue(ln);
                if (s->invalid || s->dbid != db->id) continue;
                if (dictFind(s->preserved, keyname) != NULL) continue; /* already frozen */
                snapshotMaterializeHash(s, keyobj, kv);
            }
            decrRefCount(keyobj);
        }
        kvstoreIteratorReset(&kvs_it);
    }

    /* Drop the shared store for this DB (materialized snapshots no longer need it). */
    if (gHStore && gHStore[db->id]) { dictRelease(gHStore[db->id]); gHStore[db->id] = NULL; }
}

/* --- reads ---------------------------------------------------------------- */

/* Build the as-of-V hash: copy live, apply this snapshot's resolved deltas, and
 * cache it as a frozen kvobj in `preserved`. `live` may be NULL. O(live size). */
static kvobj *snapshotMaterializeHash(keyspaceSnapshot *s, robj *key, kvobj *live) {
    dict *fs = hstoreFields(s->dbid, key->ptr);
    uint64_t me = EB_EXPIRE_TIME_INVALID;
    inMaterialize = 1;
    robj *h = (live && live->type == OBJ_HASH) ? hashTypeDup(live, &me)
                                               : createHashObject();
    if (fs) {
        dictIterator *di = dictGetIterator(fs); dictEntry *de;
        while ((de = dictNext(di)) != NULL) {
            sds f = dictGetKey(de);
            snapHRec *rec = snapHChainResolve(dictGetVal(de), s->base_epoch);
            if (!rec) continue;                              /* unchanged as-of-V */
            if (rec->old == NULL) hashTypeDelete(&server.db[s->dbid], h, f);
            else hashTypeSet(&server.db[s->dbid], h, f, rec->old, HASH_SET_COPY);
        }
        dictReleaseIterator(di);
    }
    inMaterialize = 0;
    kvobj *frozen = kvobjSet(key->ptr, h, 0);
    incrRefCount(frozen);
    dictAdd(s->preserved, kvobjGetKey(frozen), frozen);
    snapBytesAdd(kvobjAllocSize(frozen));
    decrRefCount(frozen);
    return dictFetchValue(s->preserved, key->ptr);
}

/* Read one hash field as-of the snapshot. Returns a new robj (caller frees), or
 * NULL if the field is absent as-of-V. No whole-hash materialize on this path. */
robj *kvSnapshotHashField(keyspaceSnapshot *s, robj *key, sds field) {
    if (s->invalid) return NULL;
    redisDb *db = &server.db[s->dbid];
    robj *val = NULL;
    kvobj *frozen = dictFetchValue(s->preserved, key->ptr);
    if (frozen) {
        if (frozen->type != OBJ_HASH) return NULL;
        /* The frozen hash is a point-in-time copy: its fields (and their copied
         * TTL metadata) are as-of-V. ACCESS_EXPIRED so a field whose TTL lapsed
         * in wall-clock time after the snapshot is still returned -- it was alive
         * as-of-V. This mirrors the ACCESS_EXPIRED used when capturing pre-images. */
        hashTypeGetValueObject(db, frozen, field,
                               HFE_LAZY_ACCESS_EXPIRED | HFE_LAZY_AVOID_FIELD_DEL |
                               HFE_LAZY_NO_NOTIFICATION,
                               &val, NULL, NULL);
        return val;
    }
    dict *fs = hstoreFields(s->dbid, key->ptr);
    snapHChain *c = fs ? dictFetchValue(fs, field) : NULL;
    if (c) {
        snapHRec *rec = snapHChainResolve(c, s->base_epoch);
        if (rec) return rec->old ? createStringObject(rec->old, sdslen(rec->old)) : NULL;
    }
    /* Unchanged since V -> read the live field. ACCESS_EXPIRED for the same
     * reason as the frozen path above: a field alive as-of-V whose TTL has since
     * lapsed in wall-clock time (but was not yet reaped, so no delta was recorded)
     * must still be returned. Keeps all three read paths consistent. On the key
     * lookup, ACCESS_EXPIRED + NOEXPIRE (via NOEFFECTS) likewise return a whole
     * key whose key-level TTL lapsed after V without the snapshot read deleting
     * the live key. */
    kvobj *live = lookupKeyReadWithFlags(db, key,
                                         LOOKUP_NOEFFECTS | LOOKUP_ACCESS_EXPIRED);
    if (!live || live->type != OBJ_HASH) return NULL;
    hashTypeGetValueObject(db, live, field,
                           HFE_LAZY_ACCESS_EXPIRED | HFE_LAZY_AVOID_FIELD_DEL |
                           HFE_LAZY_NO_NOTIFICATION,
                           &val, NULL, NULL);
    return val;
}

/* Resolve a snapshot's whole-value view of a key: the frozen as-of-V hash (from
 * `preserved`, materializing on first access if the key has recorded changes),
 * else the live value. Non-hash keys fall through to live (the caller rejects). */
kvobj *kvSnapshotView(keyspaceSnapshot *s, robj *keyobj) {
    if (s->invalid) return NULL;
    kvobj *o = dictFetchValue(s->preserved, keyobj->ptr);
    if (o) return o;
    redisDb *db = &server.db[s->dbid];
    /* ACCESS_EXPIRED + NOEXPIRE (via NOEFFECTS): a key whose key-level TTL lapsed
     * after V but is not yet reaped is still alive as-of-V, and the snapshot read
     * must not delete the live key. */
    kvobj *live = lookupKeyReadWithFlags(db, keyobj,
                                         LOOKUP_NOEFFECTS | LOOKUP_ACCESS_EXPIRED);
    if (hstoreFields(s->dbid, keyobj->ptr))
        return snapshotMaterializeHash(s, keyobj, live);
    return live;
}

/* --- DEBUG KVSNAPSHOT ... (test-only) ------------------------------------- */
static keyspaceSnapshot *snapshotFind(uint64_t id) {
    listIter li; listNode *ln; listRewind(server.keyspace_snapshots, &li);
    while ((ln = listNext(&li))) { keyspaceSnapshot *s = listNodeValue(ln);
        if (s->id == id) return s; }
    return NULL;
}

void debugKvSnapshotCommand(client *c) {
    if (c->argc == 3 && !strcasecmp(c->argv[2]->ptr, "create")) {
        keyspaceSnapshot *s = kvSnapshotCreate(c->db->id);
        addReplyLongLong(c, (long long)s->id);
    } else if (c->argc == 4 && !strcasecmp(c->argv[2]->ptr, "free")) {
        keyspaceSnapshot *s = snapshotFind(strtoull(c->argv[3]->ptr, NULL, 10));
        if (!s) { addReplyError(c, "no such snapshot id"); return; }
        kvSnapshotFree(s); addReply(c, shared.ok);
    } else if (c->argc == 6 && !strcasecmp(c->argv[2]->ptr, "hget")) {
        keyspaceSnapshot *s = snapshotFind(strtoull(c->argv[3]->ptr, NULL, 10));
        if (!s) { addReplyError(c, "no such snapshot id"); return; }
        robj *val = kvSnapshotHashField(s, c->argv[4], c->argv[5]->ptr);
        if (val) { addReplyBulk(c, val); decrRefCount(val); } else addReplyNull(c);
    } else if (c->argc == 5 && !strcasecmp(c->argv[2]->ptr, "len")) {
        keyspaceSnapshot *s = snapshotFind(strtoull(c->argv[3]->ptr, NULL, 10));
        if (!s) { addReplyError(c, "no such snapshot id"); return; }
        kvobj *o = kvSnapshotView(s, c->argv[4]);
        addReplyLongLong(c, (!o || o->type != OBJ_HASH) ? -1 : (long long)getObjectLength(o));
    } else if (c->argc == 3 && !strcasecmp(c->argv[2]->ptr, "stats")) {
        addReplyMapLen(c, 5);
        addReplyBulkCString(c, "epoch");           addReplyLongLong(c, (long long)gEpoch);
        addReplyBulkCString(c, "snapshots_open");  addReplyLongLong(c, server.snapshots_open);
        addReplyBulkCString(c, "hash_deltas");     addReplyLongLong(c, server.stat_snapshot_hash_deltas);
        addReplyBulkCString(c, "preserved_bytes"); addReplyLongLong(c, server.stat_snapshot_preserved_bytes);
        addReplyBulkCString(c, "evicted_drops");   addReplyLongLong(c, server.stat_snapshot_evicted_drops);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

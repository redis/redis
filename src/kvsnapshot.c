/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* kvsnapshot.c -- Keyspace value-MVCC snapshots.
 *
 * See docs/design/keyspace-value-mvcc-snapshots.md.
 *
 * A snapshot captures the current keyspace_version at creation. Before a value
 * is mutated in place, overwritten, or deleted, we "preserve" its pre-write state
 * for every open snapshot that (a) has the key in scope and (b) does not yet hold
 * it. Preservation is copy-on-write: we deep-copy the current value and freeze the
 * copy into each such snapshot's version-store (shared across snapshots by
 * refcount), leaving the live value completely untouched -- no reinstall, so the
 * live object identity and any held dictEntry link stay valid. Reads through a
 * snapshot return the frozen value if present, else the live value (unchanged
 * since the snapshot -> identical to the as-of-snapshot value).
 *
 * Two central hooks feed the preserve helper (see the design doc, "Integration
 * points"):
 *   - the lookupKeyWrite* wrappers -> snapshotPreserveForWrite()  (copy)
 *   - dbGenericDelete              -> snapshotPreserveForDelete() (retain)
 * Both are gated on server.snapshots_open so there is zero cost when idle.
 *
 * Native dup-able types (string/list/set/zset/hash/stream/array) are deep-copied;
 * module-type values are cloned via the type's copy2/copy callback (a type with
 * neither is skipped, and its snapshot read falls through to the live value).
 * Strings are frozen as RAW so read accessors that "unshare" an encoded string
 * (e.g. RM_StringDMA on EMBSTR/INT) don't try to reinstall a detached frozen
 * object against the live keyspace.
 */

#include "server.h"

/* An open point-in-time view of the keyspace. Forward-declared as a typedef in
 * server.h; define the struct body here without re-typedef'ing (repeated
 * typedefs are a C11 feature that older -Werror=pedantic toolchains reject). */
struct keyspaceSnapshot {
    uint64_t id;        /* Handle returned to the DEBUG command. */
    uint64_t version;   /* keyspace_version captured at creation. */
    int dbid;           /* Scope: the DB this snapshot is for. Writes to other
                           DBs never preserve into it, and reads resolve against
                           this DB. */
    list *prefixes;     /* Scope: sds prefixes; a key is in scope if it matches
                           ANY of them. Empty => whole DB (within dbid). */
    dict *preserved;    /* Frozen pre-write values. Keyed by the key sds the kvobj
                           already embeds (borrowed, not duplicated); the entry
                           holds a reference on the kvobj and drops it on removal. */
    uint32_t type_mask; /* Scope: if non-zero, only preserve values whose
                           (1u << OBJ_*) bit is set. 0 => all types. */

    /* --- hash delta-log: opt-in per snapshot. When set, HASH values are
       preserved by recording per-field inverse deltas on write (O(delta)) instead
       of deep-copying the whole hash (O(value)). Reconstruct as-of-V on read. */
    int delta_hash;     /* 1 => use the field delta-log for hashes. */
    dict *hdeltas;      /* keyname(sds,dup) -> (field(sds,dup) -> old value robj |
                           HDELTA_TOMBSTONE). Lazily created. */
    size_t delta_max;   /* Per-key delta cap before collapse-to-frozen; 0 => default
                           (KVSNAPSHOT_HASH_DELTA_MAX). Bounds delta memory. */
};

/* Default per-key delta count above which a delta-logged hash is collapsed into a
 * frozen deep-copy of its as-of-V value (dropping the delta map), bounding memory
 * under heavy field churn. Overridable per snapshot via DEBUG KVSNAPSHOT DELTACAP. */
#define KVSNAPSHOT_HASH_DELTA_MAX 1024

/* Sentinel stored in a field-delta map meaning "field did not exist as-of-V". */
static robj hdeltaTombstoneObj;
#define HDELTA_TOMBSTONE (&hdeltaTombstoneObj)
/* Reentrancy guard: materialization sets fields via hashTypeSet, which must not
 * re-enter the capture hook. */
static int inMaterialize = 0;

static void hdeltaFieldDtor(dict *d, void *v) {
    UNUSED(d);
    if (v && v != HDELTA_TOMBSTONE) decrRefCount(v);
}
static dictType hdeltaFieldDictType = {
    .hashFunction = dictSdsHash, .keyCompare = dictSdsKeyCompare,
    .keyDestructor = dictSdsDestructor, .valDestructor = hdeltaFieldDtor,
};
static void hdeltaKeyDtor(dict *d, void *v) { UNUSED(d); dictRelease(v); }
static dictType hdeltaKeyDictType = {
    .hashFunction = dictSdsHash, .keyCompare = dictSdsKeyCompare,
    .keyDestructor = dictSdsDestructor, .valDestructor = hdeltaKeyDtor,
};

static kvobj *snapshotMaterializeHash(keyspaceSnapshot *s, robj *key, kvobj *live);

/* Version-store dict: key is the sds borrowed from the frozen kvobj
 * (kvobjGetKey), value is the kvobj itself (a reference we own). No key dup: the
 * kvobj outlives the entry because the entry holds a reference on it, and frozen
 * values are immutable so the embedded key pointer is stable. */
static void snapshotPreservedValDestructor(dict *d, void *val) {
    UNUSED(d);
    if (val) decrRefCount(val);
}

static dictType snapshotPreservedDictType = {
    .hashFunction  = dictSdsHash,
    .keyCompare    = dictSdsKeyCompare,
    .keyDestructor = NULL,                       /* key sds is owned by the kvobj */
    .valDestructor = snapshotPreservedValDestructor,
};

static uint64_t nextSnapshotId = 1;

void kvsnapshotInit(void) {
    server.keyspace_snapshots = listCreate();
    server.snapshots_open = 0;
    server.keyspace_version = 1;
    server.stat_snapshot_cow_copies = 0;
    server.stat_snapshot_module_copies = 0;
    server.stat_snapshot_hash_deltas = 0;
    nextSnapshotId = 1;
}

/* --- snapshot lifecycle --------------------------------------------------- */

keyspaceSnapshot *kvSnapshotCreate(int dbid, sds prefix) {
    keyspaceSnapshot *s = zmalloc(sizeof(*s));
    s->id = nextSnapshotId++;
    s->version = server.keyspace_version;
    s->dbid = dbid;
    s->prefixes = listCreate();
    listSetFreeMethod(s->prefixes, (void (*)(void *))sdsfree);
    if (prefix) listAddNodeTail(s->prefixes, sdsdup(prefix));
    s->preserved = dictCreate(&snapshotPreservedDictType);
    s->type_mask = 0;
    s->delta_hash = 0;
    s->hdeltas = NULL;
    s->delta_max = 0;
    listAddNodeTail(server.keyspace_snapshots, s);
    server.snapshots_open++;
    return s;
}

/* Restrict the snapshot to a set of value types (OBJ_*). Additive: each call
 * adds one type; with none added, all types are preserved. */
void kvSnapshotAddType(keyspaceSnapshot *s, int objtype) {
    s->type_mask |= (1u << objtype);
}

/* Opt in to the hash field delta-log: HASH values are preserved by
 * recording per-field inverse deltas on write instead of deep-copying. */
void kvSnapshotSetDeltaHash(keyspaceSnapshot *s, int on) {
    s->delta_hash = on;
}

static keyspaceSnapshot *snapshotFind(uint64_t id) {
    listIter li;
    listNode *ln;
    listRewind(server.keyspace_snapshots, &li);
    while ((ln = listNext(&li))) {
        keyspaceSnapshot *s = listNodeValue(ln);
        if (s->id == id) return s;
    }
    return NULL;
}

void kvSnapshotFree(keyspaceSnapshot *s) {
    listNode *ln = listSearchKey(server.keyspace_snapshots, s);
    if (ln) listDelNode(server.keyspace_snapshots, ln);
    dictRelease(s->preserved); /* val destructor decrRefCounts every frozen value */
    if (s->hdeltas) dictRelease(s->hdeltas); /* frees nested field-delta dicts */
    listRelease(s->prefixes);  /* free method sdsfree's each prefix */
    zfree(s);
    server.snapshots_open--;
}

/* --- helpers -------------------------------------------------------------- */

/* Is `keyname` within this snapshot's scope? */
static inline int snapshotKeyInScope(keyspaceSnapshot *s, sds keyname) {
    if (listLength(s->prefixes) == 0) return 1; /* whole DB */
    size_t klen = sdslen(keyname);
    listIter li;
    listNode *ln;
    listRewind(s->prefixes, &li);
    while ((ln = listNext(&li))) {
        sds p = listNodeValue(ln);
        size_t plen = sdslen(p);
        if (klen >= plen && memcmp(keyname, p, plen) == 0) return 1;
    }
    return 0;
}

/* Does snapshot `s` want to preserve `kv` for `key`? Applies the prefix and type
 * scope filters (the caller has already checked db match and not-already-preserved).
 * Hashes under a delta_hash snapshot are handled by the field delta-log, not here. */
static int snapshotWantsKey(keyspaceSnapshot *s, robj *key, kvobj *kv) {
    if (s->delta_hash && kv->type == OBJ_HASH) return 0;
    if (!snapshotKeyInScope(s, key->ptr)) return 0;
    if (s->type_mask && !(s->type_mask & (1u << kv->type))) return 0;
    return 1;
}

/* Deep-copy a value for the frozen side. Returns NULL for values with no dup
 * path (an unknown type, or a module type registering no copy callback) ->
 * caller skips preservation and that key's snapshot read falls through to live. */
static robj *snapshotDupValue(int dbid, robj *key, kvobj *kv) {
    uint64_t minHashExpire = EB_EXPIRE_TIME_INVALID;
    switch (kv->type) {
        case OBJ_STRING: {
            /* Freeze strings as RAW so read accessors that "unshare"
             * encoded/shared strings (RM_StringDMA on EMBSTR/INT) don't try to
             * reinstall the detached frozen object against the live keyspace. */
            robj *dec = getDecodedObject(kv);
            robj *raw = createRawStringObject(dec->ptr, sdslen(dec->ptr));
            decrRefCount(dec);
            return raw;
        }
        case OBJ_LIST:   return listTypeDup(kv);
        case OBJ_SET:    return setTypeDup(kv);
        case OBJ_ZSET:   return zsetDup(kv);
        case OBJ_HASH:   return hashTypeDup(kv, &minHashExpire);
        case OBJ_STREAM: return streamDup(kv);
        case OBJ_ARRAY:  return arrayTypeDup(kv);
        case OBJ_MODULE: {
            /* Clone the module value via its copy2/copy callback. Returns
             * NULL if the type registered neither callback (e.g. an opaque type
             * that can't be duplicated) — then the key isn't preserved and its
             * snapshot read falls through to the live value. RedisJSON registers
             * `copy`, so JSON documents are snapshotted. */
            robj *dup = moduleTypeDup(key, key, dbid, dbid, kv);
            if (dup) server.stat_snapshot_module_copies++;
            return dup;
        }
        default:         return NULL; /* unknown -> skip */
    }
}

/* Add the frozen kvobj to a snapshot's version-store, taking a reference. */
static void snapshotStore(keyspaceSnapshot *s, kvobj *frozen) {
    incrRefCount(frozen);
    dictAdd(s->preserved, kvobjGetKey(frozen), frozen);
}

/* --- preserve hooks ------------------------------------------------------- */

/* Copy-on-write preserve, called from the lookupKeyWrite* wrappers before an
 * in-place mutation / overwrite. `kv` is the current (live) value.
 *
 * We freeze a detached, independent copy for the snapshots and leave the live
 * value completely untouched: the command mutates the original in place, and the
 * snapshots hold the immutable side copy. Because we do not reinstall, the live
 * object identity and the caller's dictEntryLink stay valid -- no link refresh,
 * no interaction with WATCH/replication/keyspace-notifications. */
void snapshotPreserveForWrite(redisDb *db, robj *key, kvobj *kv) {
    sds keyname = key->ptr;

    /* Single pass over the open snapshots. The frozen copy is built lazily on the
     * first snapshot that needs it (in scope and not already holding the key) and
     * then shared with every other such snapshot by refcount — at most one deep
     * copy, and no auxiliary array. */
    kvobj *frozen = NULL;
    listIter li;
    listNode *ln;
    listRewind(server.keyspace_snapshots, &li);
    while ((ln = listNext(&li))) {
        keyspaceSnapshot *s = listNodeValue(ln);
        if (s->dbid != db->id) continue;
        if (dictFind(s->preserved, keyname) != NULL) continue;
        if (!snapshotWantsKey(s, key, kv)) continue;
        if (frozen == NULL) {
            /* Deep-copy the current value; if the type isn't dup-able yet
             * (module), skip preservation entirely rather than risk corruption. */
            robj *dup = snapshotDupValue(db->id, key, kv);
            if (dup == NULL) return;
            /* Wrap into a standalone kvobj embedding the key (opt #3 keyed store),
             * not installed in the keyspace. kvobjSet reuses dup's ptr and frees
             * the dup shell. keyMetaBits = 0: value-only (no TTL/metadata). */
            frozen = kvobjSet(key->ptr, dup, 0);
        }
        snapshotStore(s, frozen);
    }
    if (frozen) {
        decrRefCount(frozen); /* release the creation ref; snapshots hold the rest */
        server.stat_snapshot_cow_copies++;
    }
}

/* Retain-based preserve, called from dbGenericDelete before the value is freed.
 * The value is being removed wholesale (not mutated in place), so we just keep a
 * reference in each snapshot that needs it -- no copy. */
void snapshotPreserveForDelete(redisDb *db, robj *key, kvobj *kv) {
    sds keyname = key->ptr;

    /* The value is being removed wholesale (not mutated in place), so for
     * aggregate/module types we can retain the original (shared by refcount).
     * Strings, however, must be frozen as a RAW copy so snapshot read accessors
     * (RM_StringDMA) never try to "unshare" a detached object against the live
     * keyspace — the same reason snapshotDupValue decodes strings to RAW.
     *
     * Single pass, building the RAW string copy lazily on first need (shared with
     * later snapshots by refcount); non-strings retain the original directly. */
    int is_string = (kv->type == OBJ_STRING);
    kvobj *frozen = is_string ? NULL : kv; /* non-strings: retain kv directly */
    int created = 0;                       /* did we build a copy to release? */
    listIter li;
    listNode *ln;
    listRewind(server.keyspace_snapshots, &li);
    while ((ln = listNext(&li))) {
        keyspaceSnapshot *s = listNodeValue(ln);
        if (s->dbid != db->id) continue;
        if (dictFind(s->preserved, keyname) != NULL) continue;
        /* Delta-logged hash being removed wholesale: the live value (needed to
         * reconstruct untouched fields) is about to be freed, so materialize the
         * as-of-V hash now (collapse the delta map into a frozen copy). */
        if (s->delta_hash && kv->type == OBJ_HASH) {
            if (snapshotKeyInScope(s, keyname))
                snapshotMaterializeHash(s, key, kv);
            continue;
        }
        if (!snapshotWantsKey(s, key, kv)) continue;
        if (is_string && frozen == NULL) {
            robj *dup = snapshotDupValue(db->id, key, kv); /* RAW copy */
            frozen = kvobjSet(key->ptr, dup, 0);
            created = 1;
        }
        snapshotStore(s, frozen);
    }
    if (created) decrRefCount(frozen); /* release the creation ref */
}

/* --- hash field delta-log ----------------------------------------- */

/* Record the pre-write value of `field` for every open delta_hash snapshot that
 * has this hash in scope. Called from hashTypeSet *before* it mutates the field,
 * so the fetched value is the as-of-V one. O(#delta snapshots) per changed field;
 * the field's old value is fetched at most once and shared by refcount. The
 * per-field record is written once per (snapshot,key,field) — repeated writes to
 * the same field are O(1) no-ops. */
void snapshotHashCapture(redisDb *db, kvobj *o, sds field) {
    if (inMaterialize) return;
    sds keyname = kvobjGetKey(o);
    robj *oldval = NULL;
    int fetched = 0;
    listIter li;
    listNode *ln;
    listRewind(server.keyspace_snapshots, &li);
    while ((ln = listNext(&li))) {
        keyspaceSnapshot *s = listNodeValue(ln);
        if (!s->delta_hash) continue;
        if (s->dbid != db->id) continue;
        if (!snapshotKeyInScope(s, keyname)) continue;
        /* Already frozen (collapsed, or materialized by a read/delete): its as-of-V
         * is fixed, so further field writes need no delta. */
        if (dictFind(s->preserved, keyname) != NULL) continue;
        if (!s->hdeltas) s->hdeltas = dictCreate(&hdeltaKeyDictType);
        dict *fd = dictFetchValue(s->hdeltas, keyname);
        if (!fd) {
            fd = dictCreate(&hdeltaFieldDictType);
            dictAdd(s->hdeltas, sdsdup(keyname), fd);
        }
        if (dictFind(fd, field) != NULL) continue; /* already have as-of-V for it */
        /* Delta log for this key grew too large — collapse it: materialize the
         * as-of-V hash from live `o` + recorded deltas into a frozen deep-copy and
         * drop the delta map, bounding delta memory under heavy field churn. `o` is
         * still pre-mutation here, so the field about to change is captured too. */
        size_t cap = s->delta_max ? s->delta_max : KVSNAPSHOT_HASH_DELTA_MAX;
        if (dictSize(fd) >= cap) {
            robj kbuf;
            initStaticStringObject(kbuf, keyname);
            snapshotMaterializeHash(s, &kbuf, o); /* -> preserved; frees hdeltas[key] */
            continue;                             /* fd now dangling; key is frozen */
        }
        if (!fetched) {
            /* ACCESS_EXPIRED: capture the field's value even if its TTL has already
             * lapsed — this hook fires from the field-expiry paths, and as-of-V the
             * field still existed with this value. AVOID_FIELD_DEL: never delete
             * during the capture read. */
            hashTypeGetValueObject(db, o, field,
                                   HFE_LAZY_ACCESS_EXPIRED | HFE_LAZY_AVOID_FIELD_DEL |
                                   HFE_LAZY_NO_NOTIFICATION,
                                   &oldval, NULL, NULL);
            fetched = 1;
        }
        if (oldval) { incrRefCount(oldval); dictAdd(fd, sdsdup(field), oldval); }
        else         dictAdd(fd, sdsdup(field), HDELTA_TOMBSTONE);
        server.stat_snapshot_hash_deltas++;
    }
    if (oldval) decrRefCount(oldval); /* release the fetch reference */
}

/* Reconstruct the as-of-V hash for `key` from the live value + recorded field
 * deltas, store it as a frozen kvobj in `preserved` (superseding/dropping the
 * delta map), and return it. Used for whole-doc reads and for the "collapse"
 * before the live hash is freed on delete/overwrite. `live` may be NULL (key
 * already gone) — then reconstruction is from deltas alone. O(live size). */
static kvobj *snapshotMaterializeHash(keyspaceSnapshot *s, robj *key, kvobj *live) {
    dict *fd = s->hdeltas ? dictFetchValue(s->hdeltas, key->ptr) : NULL;
    uint64_t me = EB_EXPIRE_TIME_INVALID;
    inMaterialize = 1; /* the hashTypeSet/Delete below must not re-enter capture */
    robj *h = (live && live->type == OBJ_HASH) ? hashTypeDup(live, &me)
                                               : createHashObject();
    if (fd) {
        dictIterator *di = dictGetIterator(fd);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL) {
            sds f = dictGetKey(de);
            robj *v = dictGetVal(de);
            if (v == HDELTA_TOMBSTONE) {
                hashTypeDelete(&server.db[s->dbid], h, f);              /* absent as-of-V -> remove */
            } else {
                robj *dec = getDecodedObject(v);  /* changed/deleted -> old value */
                hashTypeSet(&server.db[s->dbid], h, f, dec->ptr, HASH_SET_COPY);
                decrRefCount(dec);
            }
        }
        dictReleaseIterator(di);
    }
    inMaterialize = 0;
    kvobj *frozen = kvobjSet(key->ptr, h, 0);
    snapshotStore(s, frozen);
    decrRefCount(frozen);                 /* preserved holds the ref now */
    if (fd) dictDelete(s->hdeltas, key->ptr);
    return dictFetchValue(s->preserved, key->ptr);
}

/* Read a hash field as-of the snapshot. Returns a new robj the caller must
 * decrRefCount, or NULL if the field is absent as-of-V (or the key isn't a live
 * hash). A field changed since V returns the recorded old value; an unchanged
 * field is read from the live hash. */
robj *kvSnapshotHashField(keyspaceSnapshot *s, robj *key, sds field) {
    redisDb *db = &server.db[s->dbid];
    robj *val = NULL;
    /* Deep-copy / materialized frozen hash (non-delta path)? */
    kvobj *frozen = dictFetchValue(s->preserved, key->ptr);
    if (frozen) {
        if (frozen->type != OBJ_HASH) return NULL;
        hashTypeGetValueObject(db, frozen, field,
                               HFE_LAZY_AVOID_FIELD_DEL | HFE_LAZY_NO_NOTIFICATION,
                               &val, NULL, NULL);
        return val;
    }
    /* Field changed since V (delta-log path)? */
    if (s->delta_hash && s->hdeltas) {
        dict *fd = dictFetchValue(s->hdeltas, key->ptr);
        if (fd) {
            dictEntry *de = dictFind(fd, field);
            if (de) {
                robj *v = dictGetVal(de);
                if (v == HDELTA_TOMBSTONE) return NULL; /* absent as-of-V */
                incrRefCount(v);
                return v;
            }
        }
    }
    /* Unchanged since V -> read the live field. */
    kvobj *live = lookupKeyReadWithFlags(db, key,
                                         LOOKUP_NONOTIFY | LOOKUP_NOSTATS | LOOKUP_NOTOUCH);
    if (!live || live->type != OBJ_HASH) return NULL;
    hashTypeGetValueObject(db, live, field,
                           HFE_LAZY_AVOID_FIELD_DEL | HFE_LAZY_NO_NOTIFICATION,
                           &val, NULL, NULL);
    return val;
}

/* --- DEBUG KVSNAPSHOT ... ------------------------------------------------- */

/* Resolve a snapshot's view of a key: the frozen value if preserved, else the
 * live value (unchanged since the snapshot). Read-only, side-effect-free. */
kvobj *kvSnapshotView(keyspaceSnapshot *s, robj *keyobj) {
    kvobj *o = dictFetchValue(s->preserved, keyobj->ptr);
    if (o) return o;
    redisDb *db = &server.db[s->dbid];
    /* Delta-logged hash with recorded changes: materialize the as-of-V hash (and
     * cache it as a frozen kvobj) so whole-value read accessors work unchanged. */
    if (s->delta_hash && s->hdeltas && dictFetchValue(s->hdeltas, keyobj->ptr)) {
        kvobj *live = lookupKeyReadWithFlags(db, keyobj,
                                             LOOKUP_NONOTIFY | LOOKUP_NOSTATS | LOOKUP_NOTOUCH);
        return snapshotMaterializeHash(s, keyobj, live);
    }
    /* Not preserved => unchanged since the snapshot => read the live value from
     * the snapshot's own DB (not the caller's currently-selected DB). */
    return lookupKeyReadWithFlags(db, keyobj,
                                  LOOKUP_NONOTIFY | LOOKUP_NOSTATS | LOOKUP_NOTOUCH);
}

static int objTypeFromName(const char *n) {
    if (!strcasecmp(n, "string")) return OBJ_STRING;
    if (!strcasecmp(n, "list"))   return OBJ_LIST;
    if (!strcasecmp(n, "set"))    return OBJ_SET;
    if (!strcasecmp(n, "zset"))   return OBJ_ZSET;
    if (!strcasecmp(n, "hash"))   return OBJ_HASH;
    if (!strcasecmp(n, "module")) return OBJ_MODULE;
    if (!strcasecmp(n, "stream")) return OBJ_STREAM;
    return -1;
}

/* Subcommands:
 *   DEBUG KVSNAPSHOT CREATE [PREFIX <p>]   -> integer snapshot id
 *   DEBUG KVSNAPSHOT ADDTYPE <id> <type>   -> restrict to a value type (+OK)
 *   DEBUG KVSNAPSHOT GET   <id> <key>      -> string value as seen by the snapshot
 *   DEBUG KVSNAPSHOT HGET  <id> <key> <fld>-> hash field value seen by the snapshot
 *   DEBUG KVSNAPSHOT LEN   <id> <key>      -> object length (elements / strlen)
 *   DEBUG KVSNAPSHOT FREE  <id>            -> +OK
 *   DEBUG KVSNAPSHOT STATS                 -> map of counters
 */
void debugKvSnapshotCommand(client *c) {
    if (!strcasecmp(c->argv[2]->ptr, "create") &&
        (c->argc == 3 || (c->argc == 5 && !strcasecmp(c->argv[3]->ptr, "prefix"))))
    {
        sds prefix = (c->argc == 5) ? c->argv[4]->ptr : NULL;
        keyspaceSnapshot *s = kvSnapshotCreate(c->db->id, prefix);
        addReplyLongLong(c, (long long)s->id);
        return;
    }

    if (c->argc == 4 && !strcasecmp(c->argv[2]->ptr, "free")) {
        keyspaceSnapshot *s = snapshotFind(strtoull(c->argv[3]->ptr, NULL, 10));
        if (!s) { addReplyError(c, "no such snapshot id"); return; }
        kvSnapshotFree(s);
        addReply(c, shared.ok);
        return;
    }

    if (c->argc == 5 && !strcasecmp(c->argv[2]->ptr, "addtype")) {
        keyspaceSnapshot *s = snapshotFind(strtoull(c->argv[3]->ptr, NULL, 10));
        if (!s) { addReplyError(c, "no such snapshot id"); return; }
        int t = objTypeFromName(c->argv[4]->ptr);
        if (t < 0) { addReplyError(c, "unknown type"); return; }
        kvSnapshotAddType(s, t);
        addReply(c, shared.ok);
        return;
    }


    if (c->argc == 5 && !strcasecmp(c->argv[2]->ptr, "get")) {
        keyspaceSnapshot *s = snapshotFind(strtoull(c->argv[3]->ptr, NULL, 10));
        if (!s) { addReplyError(c, "no such snapshot id"); return; }
        kvobj *o = kvSnapshotView(s, c->argv[4]);
        if (!o) { addReplyNull(c); return; }
        if (o->type != OBJ_STRING) { addReplyError(c, "value is not a string"); return; }
        addReplyBulk(c, o);
        return;
    }

    if (c->argc == 6 && !strcasecmp(c->argv[2]->ptr, "hget")) {
        keyspaceSnapshot *s = snapshotFind(strtoull(c->argv[3]->ptr, NULL, 10));
        if (!s) { addReplyError(c, "no such snapshot id"); return; }
        robj *val = kvSnapshotHashField(s, c->argv[4], c->argv[5]->ptr);
        if (val) { addReplyBulk(c, val); decrRefCount(val); }
        else addReplyNull(c);
        return;
    }

    if (c->argc == 4 && !strcasecmp(c->argv[2]->ptr, "deltahash")) {
        keyspaceSnapshot *s = snapshotFind(strtoull(c->argv[3]->ptr, NULL, 10));
        if (!s) { addReplyError(c, "no such snapshot id"); return; }
        kvSnapshotSetDeltaHash(s, 1);
        addReply(c, shared.ok);
        return;
    }

    if (c->argc == 5 && !strcasecmp(c->argv[2]->ptr, "deltacap")) {
        keyspaceSnapshot *s = snapshotFind(strtoull(c->argv[3]->ptr, NULL, 10));
        if (!s) { addReplyError(c, "no such snapshot id"); return; }
        s->delta_max = (size_t)strtoull(c->argv[4]->ptr, NULL, 10);
        addReply(c, shared.ok);
        return;
    }

    if (c->argc == 5 && !strcasecmp(c->argv[2]->ptr, "len")) {
        keyspaceSnapshot *s = snapshotFind(strtoull(c->argv[3]->ptr, NULL, 10));
        if (!s) { addReplyError(c, "no such snapshot id"); return; }
        kvobj *o = kvSnapshotView(s, c->argv[4]);
        if (!o) { addReplyLongLong(c, -1); return; }
        addReplyLongLong(c, (long long)getObjectLength(o));
        return;
    }

    if (c->argc == 3 && !strcasecmp(c->argv[2]->ptr, "stats")) {
        addReplyMapLen(c, 6);
        addReplyBulkCString(c, "keyspace_version");
        addReplyLongLong(c, (long long)server.keyspace_version);
        addReplyBulkCString(c, "snapshots_open");
        addReplyLongLong(c, server.snapshots_open);
        addReplyBulkCString(c, "cow_copies");
        addReplyLongLong(c, server.stat_snapshot_cow_copies);
        addReplyBulkCString(c, "module_copies");
        addReplyLongLong(c, server.stat_snapshot_module_copies);
        addReplyBulkCString(c, "hash_deltas");
        addReplyLongLong(c, server.stat_snapshot_hash_deltas);
        addReplyBulkCString(c, "next_id");
        addReplyLongLong(c, (long long)nextSnapshotId);
        return;
    }

    addReplySubcommandSyntaxError(c);
}

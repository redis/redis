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
};

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
    listAddNodeTail(server.keyspace_snapshots, s);
    server.snapshots_open++;
    return s;
}

/* Restrict the snapshot to a set of value types (OBJ_*). Additive: each call
 * adds one type; with none added, all types are preserved. */
void kvSnapshotAddType(keyspaceSnapshot *s, int objtype) {
    s->type_mask |= (1u << objtype);
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
 * scope filters (the caller has already checked db match and not-already-preserved). */
static int snapshotWantsKey(keyspaceSnapshot *s, robj *key, kvobj *kv) {
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

/* --- DEBUG KVSNAPSHOT ... ------------------------------------------------- */

/* Resolve a snapshot's view of a key: the frozen value if preserved, else the
 * live value (unchanged since the snapshot). Read-only, side-effect-free. */
kvobj *kvSnapshotView(keyspaceSnapshot *s, robj *keyobj) {
    kvobj *o = dictFetchValue(s->preserved, keyobj->ptr);
    if (o) return o;
    /* Not preserved => unchanged since the snapshot => read the live value from
     * the snapshot's own DB (not the caller's currently-selected DB). */
    return lookupKeyReadWithFlags(&server.db[s->dbid], keyobj,
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
        kvobj *o = kvSnapshotView(s, c->argv[4]);
        if (!o) { addReplyNull(c); return; }
        if (o->type != OBJ_HASH) { addReplyError(c, "value is not a hash"); return; }
        robj *val = NULL;
        hashTypeGetValueObject(c->db, o, c->argv[5]->ptr,
                               HFE_LAZY_AVOID_FIELD_DEL | HFE_LAZY_NO_NOTIFICATION,
                               &val, NULL, NULL);
        if (val) { addReplyBulk(c, val); decrRefCount(val); }
        else addReplyNull(c);
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
        addReplyMapLen(c, 5);
        addReplyBulkCString(c, "keyspace_version");
        addReplyLongLong(c, (long long)server.keyspace_version);
        addReplyBulkCString(c, "snapshots_open");
        addReplyLongLong(c, server.snapshots_open);
        addReplyBulkCString(c, "cow_copies");
        addReplyLongLong(c, server.stat_snapshot_cow_copies);
        addReplyBulkCString(c, "module_copies");
        addReplyLongLong(c, server.stat_snapshot_module_copies);
        addReplyBulkCString(c, "next_id");
        addReplyLongLong(c, (long long)nextSnapshotId);
        return;
    }

    addReplySubcommandSyntaxError(c);
}

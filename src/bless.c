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
 * Bless is one owner of the generic per-key attribute bitmask (see keyattr.c):
 * it claims the NO-EVICT bit. The bit lives in the key's ATTR keymeta value, so
 * it persists to RDB and rides DUMP/RESTORE, slot migration and AOF rewrite.
 *
 * Each redisDb also keeps an in-RAM index of its NO-EVICT keys (db->blessed_keys)
 * for BLESS LIST and INFO's blessed_keys count. It is per-DB (like db->expires)
 * so it stays correct across SWAPDB. The eviction path never consults it -
 * blessNoEvict() reads the bit inline from the key's keymeta.
 *
 * Eviction (see evict.c): blessed keys are never chosen as victims. If eviction
 * can't free enough because blessed keys hold the memory, used memory is allowed
 * to overshoot up to 1.25x maxmemory before writes are rejected with OOM - that
 * overshoot factor is the effective ceiling for a heavily-blessed keyspace.
 */

#include "server.h"

/* Bless is a single LEVEL per key, stored in the shared ATTR mask (see reserved
 * bits in server.h). Setting a level replaces any previous one.
 *   NONE     = 0
 *   NO-EVICT = bit 0   (never evicted under maxmemory) */
#define BLESS_NONE     0
#define BLESS_NOEVICT  (1ULL << 0)

/* Per-DB index: NO-EVICT key name (sds) -> attribute mask (in the value ptr). */
static dictType blessedDictType = {
    dictSdsHash,            /* hash function */
    NULL,                   /* key dup */
    NULL,                   /* val dup */
    dictSdsKeyCompare,      /* key compare */
    dictSdsDestructor,      /* key destructor */
    NULL,                   /* val destructor (mask lives in the pointer) */
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

static void blessedSetPut(redisDb *db, sds keyname, uint64_t mask) {
    int slot = getKeySlot(keyname);
    dictEntry *de = kvstoreDictFind(db->blessed_keys, slot, keyname);
    if (de) {
        kvstoreDictSetVal(db->blessed_keys, slot, de, (void *)(uintptr_t)mask);
        return;
    }
    sds dup = sdsdup(keyname);
    de = kvstoreDictAddRaw(db->blessed_keys, slot, dup, NULL);
    kvstoreDictSetVal(db->blessed_keys, slot, de, (void *)(uintptr_t)mask);
    *blessedBytesRef(db->blessed_keys) += sdsAllocSize(dup);
}

static void blessedSetDel(redisDb *db, sds keyname) {
    int slot = getKeySlot(keyname);
    dictEntry *de = kvstoreDictFind(db->blessed_keys, slot, keyname);
    if (!de) return;
    *blessedBytesRef(db->blessed_keys) -= sdsAllocSize(dictGetKey(de));
    kvstoreDictDelete(db->blessed_keys, slot, keyname);
}

/* True if the key must not be evicted. Reads the NO-EVICT bit inline from the
 * key's keymeta - no index, no lookup. Safe for unblessed keys (mask 0). */
int blessNoEvict(kvobj *kv) {
    return (keyAttrGet(kv) & BLESS_NOEVICT) != 0;
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
 * kvstoreMoveDict, which bypasses blessedSetDel; reconcile the byte counter by
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

/* ---- attribute-owner callbacks (registered with keyattr) ---- */

static void blessTrack(redisDb *db, sds key, uint64_t mask) {
    if (mask & BLESS_NOEVICT) blessedSetPut(db, key, mask);
}

static void blessUntrack(redisDb *db, sds key) {
    blessedSetDel(db, key);
}

/* Re-emit "BLESS SET <key> <level>" onto the command-format AOF stream,
 * mirroring how TTL re-emits PEXPIREAT: that stream carries no keymeta. */
static void blessAof(RedisModuleIO *io, uint64_t mask) {
    if (!(mask & BLESS_NOEVICT)) return;
    rio *r = io->rio;
    if (rioWriteBulkCount(r, '*', 4) == 0 ||
        rioWriteBulkString(r, "BLESS", 5) == 0 ||
        rioWriteBulkString(r, "SET", 3) == 0 ||
        rioWriteBulkObject(r, io->key) == 0 ||
        rioWriteBulkString(r, "NO-EVICT", 8) == 0)
        io->error = 1;
}

/* Register bless as the owner of the NO-EVICT bit. Called at startup, after
 * keyAttrInit() created the ATTR class. The wire mapping ties our bit to its own
 * RDB opcode, so the on-disk format is decoupled from the in-RAM bit. */
void blessInit(void) {
    static const keyAttrWire blessWire[] = {
        { BLESS_NOEVICT, RDB_OPCODE_KEY_NOEVICT },
    };
    keyAttrRegister(BLESS_NOEVICT, blessWire, 1, blessTrack, blessUntrack, blessAof);
}

/* ---- commands ---- */

/* Parse flag tokens (argv[first..argc-1]) into a mask. Only NO-EVICT exists
 * today; an unknown token, or the same flag given more than once, is a syntax
 * error. Arity (-4) guarantees at least one token. */
static int blessParseFlags(client *c, int first, uint64_t *out) {
    uint64_t mask = 0;
    for (int i = first; i < c->argc; i++) {
        if (!strcasecmp(c->argv[i]->ptr, "no-evict") && !(mask & BLESS_NOEVICT))
            mask |= BLESS_NOEVICT;
        else return C_ERR;
    }
    *out = mask;
    return C_OK;
}

/* Present the key in the per-DB index iff it carries any bless flag. */
static void blessedIndexUpdate(redisDb *db, sds keyname, uint64_t mask) {
    if (mask) blessedSetPut(db, keyname, mask);
    else      blessedSetDel(db, keyname);
}

/* Shared body of BLESS SET (add=1, OR the flags in) and BLESS CLEAR (add=0,
 * AND-NOT them out). Replies 1 if the key's flag set changed, else 0. */
static void blessGenericCommand(client *c, int add) {
    uint64_t flags;
    if (blessParseFlags(c, 3, &flags) != C_OK) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    robj *key = c->argv[2];
    robj *o = lookupKeyWrite(c->db, key);
    if (o == NULL) {
        addReplyErrorObject(c, shared.nokeyerr);
        return;
    }

    uint64_t cur = keyAttrGet(o);
    uint64_t next = add ? (cur | flags) : (cur & ~flags);
    if (next == cur) { /* nothing changed */
        addReply(c, shared.czero);
        return;
    }

    if (keyMetaSetMetadata(c->db, o, server.key_attr_class_id, next) == NULL) {
        addReplyError(c, "failed to update key attribute metadata");
        return;
    }
    blessedIndexUpdate(c->db, key->ptr, next);

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
    uint64_t mask = keyAttrGet(o);
    addReplyArrayLen(c, (mask & BLESS_NOEVICT) ? 1 : 0);
    if (mask & BLESS_NOEVICT)
        addReplyBulkCString(c, "NO-EVICT");
}

/* BLESS is a container. All subcommands share this dispatcher (OBJECT-style);
 * per-subcommand arity and key specs are enforced by the command table.
 * LIST reports the current DB only, like KEYS. (The instance-wide blessed-key
 * count is exposed via INFO's blessed_keys field, not a command.) */
void blessCommand(client *c) {
    const char *sub = c->argv[1]->ptr;
    if (!strcasecmp(sub, "set")) {
        blessGenericCommand(c, 1);          /* BLESS SET <key> NO-EVICT [flag ...] - turn flags ON */
    } else if (!strcasecmp(sub, "clear")) {
        blessGenericCommand(c, 0);          /* BLESS CLEAR <key> NO-EVICT [flag ...] - turn flags OFF */
    } else if (!strcasecmp(sub, "get")) {
        blessGetCommand(c);
    } else if (!strcasecmp(sub, "list")) {
        /* BLESS LIST NO-EVICT - array of keys in the current DB carrying the given
         * flag. The flag is required (no default); arity guarantees one token. */
        uint64_t flag;
        if (blessParseFlags(c, 2, &flag) != C_OK) {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
        void *replylen = addReplyDeferredLen(c);
        unsigned long n = 0;
        kvstoreIterator kvs_it;
        kvstoreIteratorInit(&kvs_it, c->db->blessed_keys);
        dictEntry *de;
        while ((de = kvstoreIteratorNext(&kvs_it)) != NULL) {
            uint64_t mask = (uint64_t)(uintptr_t)dictGetVal(de);
            if ((mask & flag) == flag) {
                sds name = dictGetKey(de);
                addReplyBulkCBuffer(c, name, sdslen(name));
                n++;
            }
        }
        kvstoreIteratorReset(&kvs_it);
        setDeferredArrayLen(c, replylen, n);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

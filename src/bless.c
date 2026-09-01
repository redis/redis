/*
 * BLESS - protect keys from eviction ("blessed" keys).
 *
 * Bless is one owner of the generic per-key attribute bitmask (see keyattr.c):
 * it claims the NO-EVICT bit. The bit lives in the key's ATTR keymeta value, so
 * it persists to RDB and rides DUMP/RESTORE, slot migration and AOF rewrite.
 *
 * Each redisDb also keeps an in-RAM index of its NO-EVICT keys (db->blessed_keys)
 * for BLESS COUNT / LIST and as the eviction guard's fallback when a key's
 * keymeta is not resident in RAM (fully disk-only under RoF). It is per-DB (like
 * db->expires) so it stays correct across SWAPDB. On the core eviction path the
 * key is in RAM, so blessNoEvict() reads the bit inline and never touches it.
 */

#include "server.h"

/* Bless is a single LEVEL per key, stored in the shared ATTR mask (see reserved
 * bits in server.h). Setting a level replaces any previous one.
 *   NONE     = 0
 *   NO-EVICT = bit 0   (never evicted under maxmemory) */
#define BLESS_NONE     0
#define BLESS_NOEVICT  (1ULL << 0)

/* Level name for a stored mask. */
static const char *blessLevelName(uint64_t mask) {
    return (mask & BLESS_NOEVICT) ? "NO-EVICT" : "NONE";
}

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

/* Create a per-DB blessed-keys index. Slot-partitioned like db->expires so a
 * slot migration (ASM) can drop a whole slot's entries in O(1). */
kvstore *blessedKvstoreCreate(int slot_count_bits, int flags) {
    return kvstoreCreate(&kvstoreBaseType, &blessedDictType, slot_count_bits, flags);
}

/* ---- per-DB index helpers (main thread only) ---- */

static void blessedSetPut(redisDb *db, sds keyname, uint64_t mask) {
    int slot = getKeySlot(keyname);
    dictEntry *de = kvstoreDictFind(db->blessed_keys, slot, keyname);
    if (de) {
        kvstoreDictSetVal(db->blessed_keys, slot, de, (void *)(uintptr_t)mask);
        return;
    }
    de = kvstoreDictAddRaw(db->blessed_keys, slot, sdsdup(keyname), NULL);
    kvstoreDictSetVal(db->blessed_keys, slot, de, (void *)(uintptr_t)mask);
}

static void blessedSetDel(redisDb *db, sds keyname) {
    kvstoreDictDelete(db->blessed_keys, getKeySlot(keyname), keyname);
}

/* Total NO-EVICT keys across all DBs, summed on demand (like dbTotalServerKeyCount).
 * BLESS SET is cold, so no maintained counter - just sum the per-DB indexes. */
static unsigned long long blessedCount(void) {
    unsigned long long n = 0;
    for (int i = 0; i < server.dbnum; i++)
        n += kvstoreSize(server.db[i].blessed_keys);
    return n;
}

/* True if the key must not be evicted. Reads the NO-EVICT bit inline from the
 * key's keymeta - no index, no lookup. Safe for unblessed keys (mask 0). */
int blessNoEvict(kvobj *kv) {
    return (keyAttrGet(kv) & BLESS_NOEVICT) != 0;
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
    const char *level = blessLevelName(mask);
    rio *r = io->rio;
    if (rioWriteBulkCount(r, '*', 4) == 0 ||
        rioWriteBulkString(r, "BLESS", 5) == 0 ||
        rioWriteBulkString(r, "SET", 3) == 0 ||
        rioWriteBulkObject(r, io->key) == 0 ||
        rioWriteBulkString(r, level, strlen(level)) == 0)
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

/* BLESS SET <key> [NONE|NO-EVICT]
 * Sets a key's blessing LEVEL against memory pressure. The level is optional and
 * defaults to NO-EVICT. NONE removes protection; NO-EVICT protects from eviction.
 * Setting a level replaces any previous one. Replies 1 if the stored level
 * changed, 0 otherwise. */
static void blessSetCommand(client *c) {
    robj *keyobj = c->argv[2];

    uint64_t mask;
    if (c->argc == 3) {
        mask = BLESS_NOEVICT;                       /* default level */
    } else if (c->argc == 4) {
        const char *lvl = c->argv[3]->ptr;
        if (!strcasecmp(lvl, "none"))
            mask = BLESS_NONE;
        else if (!strcasecmp(lvl, "no-evict"))
            mask = BLESS_NOEVICT;
        else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    } else {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    robj *o = lookupKeyWrite(c->db, keyobj);
    if (o == NULL) { addReplyErrorObject(c, shared.nokeyerr); return; }   /* missing key -> error */
    sds keyname = keyobj->ptr;

    uint64_t cur = keyAttrGet(o);
    if (mask == cur) { /* no change */
        addReply(c, shared.czero);
        return;
    }

    /* Enforce bless-max-keys only for direct client writes, and only on a
     * NONE->NO-EVICT transition (a new blessed key). Commands from the master
     * link, AOF replay, or ASM import (all mustObeyClient) must never be
     * rejected, so replication/AOF/migration may push a node over the cap - a
     * soft, best-effort guard, same as proto-max-bulk-len and maxmemory. */
    if (!mustObeyClient(c) &&
        (mask & BLESS_NOEVICT) && !(cur & BLESS_NOEVICT) &&
        server.bless_max_keys &&
        blessedCount() >= (unsigned long long)server.bless_max_keys)
    {
        addReplyError(c, "BLESS: bless-max-keys limit reached");
        return;
    }

    if (keyMetaSetMetadata(c->db, o, server.key_attr_class_id, mask) == NULL) {
        addReplyError(c, "failed to update key attribute metadata");
        return;
    }
    if (mask & BLESS_NOEVICT)
        blessedSetPut(c->db, keyname, mask);
    else
        blessedSetDel(c->db, keyname);

    keyModified(c, c->db, keyobj, NULL, 1);
    notifyKeyspaceEvent(NOTIFY_GENERIC, "bless", keyobj, c->db->id);
    server.dirty++;
    addReply(c, shared.cone);
}

/* BLESS GET <key> -> the key's blessing level (NONE | NO-EVICT). A key that
 * exists but isn't blessed reports NONE. Errors if the key does not exist. */
static void blessGetCommand(client *c) {
    robj *keyobj = c->argv[2];
    robj *o = lookupKeyReadWithFlags(c->db, keyobj, LOOKUP_NOTOUCH);
    if (o == NULL) {
        addReplyErrorObject(c, shared.nokeyerr);
        return;
    }
    /* RESP simple string (+NO-EVICT\r\n), per the API contract - not a bulk string. */
    addReplyStatus(c, blessLevelName(keyAttrGet(o)));
}

/* BLESS is a container. All subcommands share this dispatcher (OBJECT-style);
 * per-subcommand arity and key specs are enforced by the command table.
 * COUNT/LIST report the current DB only, like DBSIZE/KEYS. */
void blessCommand(client *c) {
    const char *sub = c->argv[1]->ptr;
    if (!strcasecmp(sub, "set")) {
        blessSetCommand(c);
    } else if (!strcasecmp(sub, "get")) {
        blessGetCommand(c);
    } else if (!strcasecmp(sub, "count")) {
        /* BLESS COUNT [NO-EVICT] - number of keys blessed at the given level
         * (default, and currently only, NO-EVICT), parsed like BLESS LIST. Every
         * tracked key carries the NO-EVICT bit, so the index size is the count. */
        if (c->argc == 3) {
            if (strcasecmp(c->argv[2]->ptr, "no-evict")) {
                addReplyErrorObject(c, shared.syntaxerr);
                return;
            }
        } else if (c->argc > 3) {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
        addReplyLongLong(c, kvstoreSize(c->db->blessed_keys));
    } else if (!strcasecmp(sub, "list")) {
        /* BLESS LIST [NO-EVICT] - array of keys in the current DB blessed at the
         * given level (default, and currently only, NO-EVICT). */
        uint64_t flag = BLESS_NOEVICT;
        if (c->argc == 3) {
            if (strcasecmp(c->argv[2]->ptr, "no-evict")) {
                addReplyErrorObject(c, shared.syntaxerr);
                return;
            }
        } else if (c->argc > 3) {
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

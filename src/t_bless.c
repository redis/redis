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

/* Bless owns bit 0 of the shared ATTR mask (see reserved bits in server.h). */
#define BLESS_NONE     0
#define BLESS_NOEVICT  (1ULL << 0)   /* never evicted under maxmemory */

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

static uint64_t blessGetMask(redisDb *db, sds keyname) {
    dictEntry *de = kvstoreDictFind(db->blessed_keys, getKeySlot(keyname), keyname);
    return de ? (uint64_t)(uintptr_t)dictGetVal(de) : BLESS_NONE;
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

/* Re-emit "BLESS SET <key> NO-EVICT ON" onto the command-format AOF stream,
 * mirroring how TTL re-emits PEXPIREAT: that stream carries no keymeta. */
static void blessAof(RedisModuleIO *io, uint64_t mask) {
    if (!(mask & BLESS_NOEVICT)) return;
    rio *r = io->rio;
    if (rioWriteBulkCount(r, '*', 5) == 0 ||
        rioWriteBulkString(r, "BLESS", 5) == 0 ||
        rioWriteBulkString(r, "SET", 3) == 0 ||
        rioWriteBulkObject(r, io->key) == 0 ||
        rioWriteBulkString(r, "NO-EVICT", 8) == 0 ||
        rioWriteBulkString(r, "ON", 2) == 0)
        io->error = 1;
}

/* Register bless as the owner of the NO-EVICT bit. Called at startup, after
 * keyAttrInit() created the ATTR class. */
void blessInit(void) {
    keyAttrRegister(BLESS_NOEVICT, blessTrack, blessUntrack, blessAof);
}

/* ---- commands ---- */

/* BLESS SET <key> NO-EVICT ON|OFF        (future: [IN-RAM ON|OFF])
 * Sets a key's protections against memory pressure, in the idiom of
 * CLIENT NO-EVICT ON|OFF. NO-EVICT ON protects the key from eviction; OFF
 * removes the protection. At least one protection must be given - a bare
 * `BLESS SET <key>` is an error (no implicit default), so intent is always
 * explicit and IN-RAM can be added later without changing what a bare call means.
 * Only the named bit is modified; other attribute bits are left untouched.
 * Replies 1 if the key's stored state changed, 0 otherwise (no change). */
static void blessSetCommand(client *c) {
    robj *keyobj = c->argv[2];

    int noevict = -1;   /* -1 = not specified */
    for (int j = 3; j < c->argc; ) {
        const char *opt = c->argv[j]->ptr;
        if (!strcasecmp(opt, "no-evict") && j + 1 < c->argc) {
            const char *v = c->argv[j + 1]->ptr;
            if (!strcasecmp(v, "on"))        noevict = 1;
            else if (!strcasecmp(v, "off"))  noevict = 0;
            else { addReplyErrorObject(c, shared.syntaxerr); return; }
            j += 2;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }
    if (noevict == -1) {
        addReplyError(c, "at least one protection is required (NO-EVICT ON|OFF)");
        return;
    }

    robj *o = lookupKeyWrite(c->db, keyobj);
    if (o == NULL) { addReply(c, shared.nokeyerr); return; }   /* missing key -> error */
    sds keyname = keyobj->ptr;

    /* Flags are independent: flip only the NO-EVICT bit, leaving the rest of the
     * mask (e.g. a future IN-RAM) untouched. */
    uint64_t cur = keyAttrGet(o), mask = cur;
    if (noevict == 1) mask |= BLESS_NOEVICT;
    else              mask &= ~(uint64_t)BLESS_NOEVICT;

    if (mask == cur) { addReply(c, shared.czero); return; }    /* no change */
    if (keyMetaSetMetadata(c->db, o, server.key_attr_class_id, mask) == NULL) {
        addReplyError(c, "failed to update key attribute metadata");
        return;
    }
    if (mask & BLESS_NOEVICT) blessedSetPut(c->db, keyname, mask);
    else                      blessedSetDel(c->db, keyname);

    keyModified(c, c->db, keyobj, NULL, 1);
    notifyKeyspaceEvent(NOTIFY_GENERIC, "bless", keyobj, c->db->id);
    server.dirty++;
    addReply(c, shared.cone);
}

/* BLESS GET <key> -> a map of each protection to its ON|OFF state. Currently just
 * NO-EVICT; IN-RAM joins as a second entry when implemented. A key that exists
 * but isn't protected reports OFF. Errors if the key does not exist. */
static void blessGetCommand(client *c) {
    robj *keyobj = c->argv[2];
    if (lookupKeyReadWithFlags(c->db, keyobj, LOOKUP_NOTOUCH) == NULL) {
        addReply(c, shared.nokeyerr);
        return;
    }
    uint64_t mask = blessGetMask(c->db, keyobj->ptr);
    addReplyMapLen(c, 1);
    addReplyBulkCString(c, "NO-EVICT");
    addReplyBulkCString(c, (mask & BLESS_NOEVICT) ? "ON" : "OFF");
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
        addReplyLongLong(c, kvstoreSize(c->db->blessed_keys));
    } else if (!strcasecmp(sub, "list")) {
        /* BLESS LIST [NO-EVICT] - array of keys in the current DB that have the
         * given flag set (default NO-EVICT). Flags are independent, so this is a
         * bit test (IN-RAM joins as another selectable flag). */
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
            if (mask & flag) {
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

/*
 * BLESS - protect keys from eviction ("blessed" keys).
 *
 * The bless level is stored per-key as a keymeta class value (see keymeta.h),
 * so it is persisted to RDB and carried inline through DUMP/RESTORE and cluster
 * slot migration for free - exactly like TTL (keymeta class 0).
 *
 * In addition each redisDb keeps an in-RAM index of its blessed keys
 * (db->blessed_keys). It answers BLESS COUNT / LIST for the current DB, and it
 * is the eviction guard's fallback source when a key's keymeta is not resident
 * in RAM (the fully disk-only case under RoF). It is per-DB (like db->expires)
 * so it stays correct across SWAPDB and multiple databases.
 *
 * On the core eviction path the key is always in RAM, so blessNoEvict() reads
 * the level inline from the kvobj and never touches this index; the index is
 * consulted only when there is no in-RAM keymeta to read (RoF flash-eviction).
 *
 * NOTE: this implements NOEVICT only (storage + command surface + the eviction
 * guard). The level is stored as a number, so additional stronger levels can be
 * added later without changing storage, index, persistence, or migration.
 */

#include "server.h"

/* Bless level - stored as the keymeta value. A NUMBER (not a boolean) so more
 * levels can be added later without touching storage. 0 means "not blessed"
 * (the reset sentinel, never persisted/migrated). */
#define BLESS_NONE     0  /* may be evicted. */
#define BLESS_NOEVICT  1  /* never evicted. */

/* Per-DB index: blessed key name (sds) -> level (stored in the value pointer). */
static dictType blessedDictType = {
    dictSdsHash,            /* hash function */
    NULL,                   /* key dup */
    NULL,                   /* val dup */
    dictSdsKeyCompare,      /* key compare */
    dictSdsDestructor,      /* key destructor */
    NULL,                   /* val destructor (level lives in the pointer) */
    NULL                    /* allow to resize */
};

/* Create a per-DB blessed-keys index. Slot-partitioned like db->expires so a
 * slot migration (ASM) can drop a whole slot's entries in O(1). Called once per
 * redisDb at startup. */
kvstore *blessedKvstoreCreate(int slot_count_bits, int flags) {
    return kvstoreCreate(&kvstoreBaseType, &blessedDictType, slot_count_bits, flags);
}

/* ---- per-DB index helpers (main thread only) ---- */

static void blessedSetPut(redisDb *db, sds keyname, uint64_t level) {
    int slot = getKeySlot(keyname);
    dictEntry *de = kvstoreDictFind(db->blessed_keys, slot, keyname);
    if (de) {
        kvstoreDictSetVal(db->blessed_keys, slot, de, (void *)(uintptr_t)level);
        return;
    }
    de = kvstoreDictAddRaw(db->blessed_keys, slot, sdsdup(keyname), NULL);
    kvstoreDictSetVal(db->blessed_keys, slot, de, (void *)(uintptr_t)level);
}

static void blessedSetDel(redisDb *db, sds keyname) {
    kvstoreDictDelete(db->blessed_keys, getKeySlot(keyname), keyname);
}

/* Public: add/update a blessed key in its DB's index. Called from dbAddInternal
 * and dbAddRDBLoad when a key arrives with the bless metadata attached (RDB load,
 * RESTORE, cluster slot migration, COPY, MOVE, RENAME) - the keymeta rdb_load
 * callback has no key name, so these are the points that see key + metadata
 * together. */
void blessTrackKey(redisDb *db, sds keyname, uint64_t level) {
    blessedSetPut(db, keyname, level);
}

/* Guarding queries. Safe for unblessed keys (-> NONE). */
static uint64_t blessGetLevel(redisDb *db, sds keyname) {
    dictEntry *de = kvstoreDictFind(db->blessed_keys, getKeySlot(keyname), keyname);
    return de ? (uint64_t)(uintptr_t)dictGetVal(de) : BLESS_NONE;
}

/* True if the key must not be evicted (NOEVICT and up). Called from
 * performEvictions() for each sampled key. Reads the level straight from the
 * key's inline keymeta, so the core eviction path needs no side index and no
 * extra lookup. Written as a ">= level" test so higher levels can be added the
 * same way. Safe for unblessed keys: keyMetaGetMetadata leaves level=NONE. */
int blessNoEvict(kvobj *kv) {
    if (server.bless_class_id <= 0) return 0;
    uint64_t level = BLESS_NONE;
    keyMetaGetMetadata(server.bless_class_id, kv, &level);
    return level >= BLESS_NOEVICT;
}

/* ---- keymeta class callbacks ---- */

/* Persist the level. The framework only calls this when the value is not the
 * reset sentinel, so unblessed keys are never written. */
static void blessRdbSave(RedisModuleIO *io, void *reserved, uint64_t *meta) {
    UNUSED(reserved);
    if (rdbSaveLen(io->rio, *meta) == -1) io->error = 1;
}

static int blessRdbLoad(RedisModuleIO *io, uint64_t *meta, int encver) {
    UNUSED(encver);
    uint64_t v = rdbLoadLen(io->rio, NULL);
    if (v == RDB_LENERR) { io->error = 1; return -1; }
    *meta = v;
    return 1; /* attach; the DB index is populated later in dbAdd* (io has no key). */
}

/* Logical removal on the main thread (DEL, expire, overwrite). We use unlink
 * rather than free() because free() may run on a background (lazyfree) thread
 * and must not touch the keyspace/globals. */
static void blessUnlink(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    UNUSED(meta);
    if (ctx->from_key && ctx->from_dbid >= 0)
        blessedSetDel(&server.db[ctx->from_dbid], ctx->from_key->ptr);
}

/* RENAME: drop the old name here; the new name is (re)added via dbAddInternal.
 * Returning non-zero keeps the metadata attached to the renamed key. */
static int blessRename(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    UNUSED(meta);
    if (ctx->from_key && ctx->from_dbid >= 0)
        blessedSetDel(&server.db[ctx->from_dbid], ctx->from_key->ptr);
    return 1;
}

/* COPY/MOVE: keep the metadata; the destination key is added to its DB's index
 * via dbAddInternal once it becomes live. */
static int blessKeep(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    UNUSED(ctx);
    UNUSED(meta);
    return 1;
}

/* Re-emit "BLESS SET <key> NO-EVICT ON" onto the command-format AOF stream,
 * mirroring how TTL re-emits PEXPIREAT: that stream reconstructs keys from data
 * commands and carries no keymeta, so a blessed key would otherwise lose its
 * level across an AOF rewrite or an AOF-format slot (ASM) migration. Invoked via
 * keyMetaOnUnlink's AOF sibling keyMetaOnAof() (aof.c); the framework only calls
 * it for a non-reset (blessed) value. */
static void blessAofRewrite(RedisModuleIO *io, void *reserved, uint64_t meta) {
    UNUSED(reserved);
    UNUSED(meta);
    rio *r = io->rio;
    if (rioWriteBulkCount(r, '*', 5) == 0 ||
        rioWriteBulkString(r, "BLESS", 5) == 0 ||
        rioWriteBulkString(r, "SET", 3) == 0 ||
        rioWriteBulkObject(r, io->key) == 0 ||
        rioWriteBulkString(r, "NO-EVICT", 8) == 0 ||
        rioWriteBulkString(r, "ON", 2) == 0)
        io->error = 1;
}

/* Register the BLESS keymeta class. Called once at startup, right after
 * keyMetaInit() and before any RDB load. The per-DB indexes are created with
 * their redisDb (see initServer). */
void blessInit(void) {
    KeyMetaClassConf conf;
    memset(&conf, 0, sizeof(conf));
    conf.flags = (1u << KEY_META_FLAG_ALLOW_IGNORE); /* older servers skip gracefully */
    conf.reset_value = BLESS_NONE;
    conf.rdb_save = blessRdbSave;
    conf.rdb_load = blessRdbLoad;
    conf.unlink   = blessUnlink;
    conf.rename   = blessRename;
    conf.copy       = blessKeep;
    conf.move       = blessKeep;
    conf.aof_rewrite = blessAofRewrite;
    /* aof_rewrite (blessAofRewrite) re-emits BLESS on the command-format AOF
     * stream, so bless survives an AOF rewrite and AOF-format slot migration too,
     * not just the RDB / DUMP / RESTORE paths. */

    server.bless_class_id = keyMetaClassCreate(NULL, "BLES", 0, &conf);
    serverAssert(server.bless_class_id >= KEY_META_ID_MODULE_FIRST);
}

/* ---- commands ---- */

/* BLESS SET <key> NO-EVICT ON|OFF        (future: [IN-RAM ON|OFF])
 * Sets a key's protections against memory pressure, in the idiom of
 * CLIENT NO-EVICT ON|OFF. NO-EVICT ON protects the key from eviction; OFF
 * removes the protection. At least one protection must be given - a bare
 * `BLESS SET <key>` is an error (no implicit default), so intent is always
 * explicit and IN-RAM can be added later without changing what a bare call means.
 *   IN-RAM ON|OFF is reserved for a future Redis-on-Flash "keep value in RAM"
 *   toggle and is intentionally not parsed yet; the level is stored as a number
 *   so an independent IN-RAM bit can be added later without touching storage.
 * Replies 1 if the key's stored state changed, 0 otherwise (no change).
 * No cap: bless is meant for a small number of keys (see README). */
static void blessSetCommand(client *c) {
    /* BLESS SET <key> [NO-EVICT ON|OFF] */
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
    /* At least one protection must be specified; there is no implicit default.
     * IN-RAM will join this check when implemented. */
    if (noevict == -1) {
        addReplyError(c, "at least one protection is required (NO-EVICT ON|OFF)");
        return;
    }
    uint64_t level = (noevict == 1) ? BLESS_NOEVICT : BLESS_NONE;

    robj *o = lookupKeyWrite(c->db, keyobj);
    if (o == NULL) { addReply(c, shared.nokeyerr); return; }   /* missing key -> error */
    sds keyname = keyobj->ptr;
    if (blessGetLevel(c->db, keyname) == level) { addReply(c, shared.czero); return; }
    if (keyMetaSetMetadata(c->db, o, server.bless_class_id, level) == NULL) {
        addReply(c, shared.czero);
        return;
    }
    if (level == BLESS_NONE) blessedSetDel(c->db, keyname);
    else                     blessedSetPut(c->db, keyname, level);

    keyModified(c, c->db, keyobj, NULL, 1);
    notifyKeyspaceEvent(NOTIFY_GENERIC, "bless", keyobj, c->db->id);
    server.dirty++;
    addReply(c, shared.cone);
}

/* BLESS GET <key> -> a map of each protection to its ON|OFF state, in the same
 * vocabulary BLESS SET uses. Currently just NO-EVICT; INRAM joins as a second
 * entry when implemented. A key that exists but isn't blessed reports OFF.
 * Errors if the key does not exist. */
static void blessGetCommand(client *c) {
    robj *keyobj = c->argv[2];
    if (lookupKeyReadWithFlags(c->db, keyobj, LOOKUP_NOTOUCH) == NULL) {
        addReply(c, shared.nokeyerr);
        return;
    }
    uint64_t level = blessGetLevel(c->db, keyobj->ptr);
    addReplyMapLen(c, 1);
    addReplyBulkCString(c, "NO-EVICT");
    addReplyBulkCString(c, (level >= BLESS_NOEVICT) ? "ON" : "OFF");
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
        /* BLESS LIST [NO-EVICT] - array of keys in the current DB whose
         * protection includes the given type (default NO-EVICT). Higher levels
         * imply lower ones, so this is a ">= threshold" filter (IN-RAM joins as
         * a stronger threshold later). */
        uint64_t threshold = BLESS_NOEVICT;
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
            uint64_t level = (uint64_t)(uintptr_t)dictGetVal(de);
            if (level >= threshold) {
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

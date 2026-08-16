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

/* Create a per-DB blessed-keys index. Called once per redisDb at startup. */
dict *blessedDictCreate(void) {
    return dictCreate(&blessedDictType);
}

/* ---- per-DB index helpers (main thread only) ---- */

static void blessedSetPut(redisDb *db, sds keyname, uint64_t level) {
    dictEntry *de = dictFind(db->blessed_keys, keyname);
    if (de) {
        dictSetVal(db->blessed_keys, de, (void *)(uintptr_t)level);
        return;
    }
    dictAdd(db->blessed_keys, sdsdup(keyname), (void *)(uintptr_t)level);
}

static void blessedSetDel(redisDb *db, sds keyname) {
    dictDelete(db->blessed_keys, keyname);
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
    dictEntry *de = dictFind(db->blessed_keys, keyname);
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
    conf.copy     = blessKeep;
    conf.move     = blessKeep;
    /* No aof_rewrite callback: with the RDB preamble (aof-use-rdb-preamble yes,
     * the default) bless survives an AOF rewrite because the preamble carries
     * keymeta. Only a command-format rewrite (preamble off) would drop it; add
     * an aof_rewrite that re-emits BLESS if that must be covered too. */

    server.bless_class_id = keyMetaClassCreate(NULL, "BLES", 0, &conf);
    serverAssert(server.bless_class_id >= KEY_META_ID_MODULE_FIRST);
}

/* ---- commands ---- */

/* BLESS SET <key> [NO-EVICT ON|OFF]
 * Sets a key's protections against memory pressure, in the idiom of
 * CLIENT NO-EVICT ON|OFF. NO-EVICT ON protects the key from eviction; OFF
 * removes the protection. When NO-EVICT is omitted it defaults to ON, so
 * `BLESS SET <key>` protects the key.
 *   INRAM ON|OFF is reserved for a future Redis-on-Flash "keep value in RAM"
 *   toggle and is intentionally not parsed yet; the level is stored as a number
 *   so an independent INRAM bit can be added later without touching storage.
 * Replies 1 if the key's stored state changed, 0 otherwise (no change).
 * No cap: bless is meant for a small number of keys (see README). A hard cap
 * would also be unenforceable on the migration / RDB-load paths anyway. */
static void blessSetCommand(client *c) {
    /* BLESS SET <key> [NO-EVICT ON|OFF] */
    robj *keyobj = c->argv[2];

    /* Default when NO-EVICT is omitted: ON (protect the key). */
    int noevict = 1;
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
    uint64_t level = noevict ? BLESS_NOEVICT : BLESS_NONE;

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
        addReplyLongLong(c, dictSize(c->db->blessed_keys));
    } else if (!strcasecmp(sub, "list")) {
        /* Reply is a map for the current DB: key name -> { NO-EVICT: ON|OFF },
         * the same per-toggle shape BLESS GET uses (INRAM joins later). Every
         * key in the index is protected, so NO-EVICT is always ON here. */
        addReplyMapLen(c, dictSize(c->db->blessed_keys));
        dictIterator *di = dictGetIterator(c->db->blessed_keys);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL) {
            sds name = dictGetKey(de);
            uint64_t level = (uint64_t)(uintptr_t)dictGetVal(de);
            addReplyBulkCBuffer(c, name, sdslen(name));
            addReplyMapLen(c, 1);
            addReplyBulkCString(c, "NO-EVICT");
            addReplyBulkCString(c, (level >= BLESS_NOEVICT) ? "ON" : "OFF");
        }
        dictReleaseIterator(di);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

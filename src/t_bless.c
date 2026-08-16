/*
 * BLESS - protect keys from eviction ("blessed" keys).
 *
 * The bless level is stored per-key as a keymeta class value (see keymeta.h),
 * so it is persisted to RDB and carried inline through DUMP/RESTORE and cluster
 * slot migration for free - exactly like TTL (keymeta class 0).
 *
 * In addition each redisDb keeps an in-RAM index of its blessed keys
 * (db->blessed_keys). It answers BLESSED COUNT / LIST for the current DB,
 * enforces the bless-max-keys cap, and is the structure the eviction decision
 * consults. It is per-DB (like db->expires) so it stays correct across SWAPDB
 * and multiple databases.
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

/* Human-readable name of a bless level. */
static const char *blessLevelName(uint64_t level) {
    switch (level) {
    case BLESS_NOEVICT: return "NOEVICT";
    default:            return "NONE";
    }
}

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

/* True if the key must not be evicted (NOEVICT and up). performEvictions().
 * Written as a ">= level" test so higher levels can be added the same way. */
int blessNoEvict(redisDb *db, sds keyname) {
    return blessGetLevel(db, keyname) >= BLESS_NOEVICT;
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
    /* ponytail: no aof_rewrite callback yet -> bless is not preserved across an
     * AOF rewrite. RDB, replication and slot migration are covered. Add an
     * aof_rewrite that re-emits BLESS if AOF persistence of bless is required. */

    server.bless_class_id = keyMetaClassCreate(NULL, "BLES", 0, &conf);
    serverAssert(server.bless_class_id >= KEY_META_ID_MODULE_FIRST);
}

/* ---- commands ---- */

/* BLESS SET key LEVEL
 * LEVEL is NONE (clear) or NOEVICT. The level is numeric, leaving room for
 * stronger levels later. */
static void blessSetCommand(client *c) {
    uint64_t level;
    const char *t = c->argv[3]->ptr;
    if (!strcasecmp(t, "none"))         level = BLESS_NONE;
    else if (!strcasecmp(t, "noevict")) level = BLESS_NOEVICT;
    else {
        addReplyError(c, "unknown bless level (use NONE or NOEVICT)");
        return;
    }

    robj *o = lookupKeyWrite(c->db, c->argv[2]);
    if (o == NULL) { addReplyErrorObject(c, shared.nokeyerr); return; }

    sds keyname = c->argv[2]->ptr;
    int already = (dictFind(c->db->blessed_keys, keyname) != NULL);

    if (level == BLESS_NONE) {
        if (!already) { addReply(c, shared.ok); return; } /* nothing to unbless */
        if (keyMetaSetMetadata(c->db, o, server.bless_class_id, BLESS_NONE) == NULL) {
            addReplyError(c, "failed to update key metadata");
            return;
        }
        blessedSetDel(c->db, keyname);
    } else {
        /* Cap applies only to newly blessed keys; re-leveling is free. Per-DB:
         * dictSize is the count for this DB (see the design doc on the per-node
         * vs per-DB cap decision). */
        if (!already && (int)dictSize(c->db->blessed_keys) >= server.bless_max_keys) {
            addReplyError(c, "reached the maximum number of blessed keys (bless-max-keys)");
            return;
        }
        if (keyMetaSetMetadata(c->db, o, server.bless_class_id, level) == NULL) {
            addReplyError(c, "failed to update key metadata");
            return;
        }
        blessedSetPut(c->db, keyname, level);
    }

    keyModified(c, c->db, c->argv[2], NULL, 1);
    notifyKeyspaceEvent(NOTIFY_GENERIC, "bless", c->argv[2], c->db->id);
    server.dirty++;
    addReply(c, shared.ok);
}

/* BLESS GET key -> the key's level name (nil if not blessed). */
static void blessGetCommand(client *c) {
    uint64_t level = blessGetLevel(c->db, c->argv[2]->ptr);
    if (level == BLESS_NONE) { addReplyNull(c); return; }
    addReplyBulkCString(c, blessLevelName(level));
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
        /* Reply is a map: key name -> level name, for the current DB. */
        addReplyMapLen(c, dictSize(c->db->blessed_keys));
        dictIterator *di = dictGetIterator(c->db->blessed_keys);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL) {
            sds name = dictGetKey(de);
            uint64_t level = (uint64_t)(uintptr_t)dictGetVal(de);
            addReplyBulkCBuffer(c, name, sdslen(name));
            addReplyBulkCString(c, blessLevelName(level));
        }
        dictReleaseIterator(di);
    } else if (!strcasecmp(sub, "help")) {
        const char *help[] = {
            "SET <key> <NONE | NOEVICT>",
            "    Bless a key so it is never evicted (NONE clears).",
            "GET <key>",
            "    Show a key's bless level.",
            "COUNT",
            "    Number of blessed keys in the current DB.",
            "LIST",
            "    Map of blessed key -> level in the current DB.",
            NULL
        };
        addReplyHelp(c, help);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

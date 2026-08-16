/*
 * BLESS - protect keys from eviction / swapping ("blessed" keys).
 *
 * The bless level is stored per-key as a keymeta class value (see keymeta.h),
 * so it is persisted to RDB and carried inline through DUMP/RESTORE and cluster
 * slot migration for free - exactly like TTL (keymeta class 0).
 *
 * In addition we keep a per-node in-RAM set of blessed keys. That set answers
 * BLESSED COUNT / BLESSED LIST, enforces the bless-max-keys cap, and is the
 * structure the eviction/swap decision would consult without touching flash.
 *
 * NOTE: this implements storage + command surface only. The actual enforcement
 * ("guarding" eviction/swap, and the ROF RAM-residency wiring) is intentionally
 * out of scope here.
 */

#include "server.h"

/* Bless levels - stored as the keymeta value. 0 (the reset sentinel) means
 * "not blessed" (AUFERRE) and is never persisted/migrated. */
/* Bless flags - a bitfield stored as the keymeta value. Each bit is one
 * independent protection; a value of 0 means "not blessed" (AUFERRE) and is
 * the reset sentinel that is never persisted/migrated. Flags can be combined. */
#define BLESS_NONE     0
#define BLESS_NOEVICT  (1u << 0)  /* Never evicted. */
#define BLESS_NOSWAP   (1u << 1)  /* Kept in RAM, never swapped to flash. */
#define BLESS_ALL      (BLESS_NOEVICT | BLESS_NOSWAP)

/* Append the flag names of 'f' (e.g. "NOEVICT|NOSWAP") to the reply. */
static void addReplyBlessFlags(client *c, uint64_t f) {
    sds s = sdsempty();
    if (f & BLESS_NOEVICT) s = sdscat(s, "NOEVICT");
    if (f & BLESS_NOSWAP) {
        if (sdslen(s)) s = sdscat(s, "|");
        s = sdscat(s, "NOSWAP");
    }
    addReplyBulkCBuffer(c, s, sdslen(s));
    sdsfree(s);
}

/* Global set: blessed key name (sds) -> level (stored in the value pointer).
 * ponytail: keyed by name only, one set for all DBs. The feature targets
 * single-DB use; add a (dbid,name) composite key if multi-DB support is needed. */
static dictType blessedDictType = {
    dictSdsHash,            /* hash function */
    NULL,                   /* key dup */
    NULL,                   /* val dup */
    dictSdsKeyCompare,      /* key compare */
    dictSdsDestructor,      /* key destructor */
    NULL,                   /* val destructor (level lives in the pointer) */
    NULL                    /* allow to resize */
};

/* ---- RAM-side blessed-set helpers (main thread only) ---- */

static void blessedSetPut(sds keyname, uint64_t level) {
    dictEntry *de = dictFind(server.blessed_keys, keyname);
    if (de) {
        dictSetVal(server.blessed_keys, de, (void *)(uintptr_t)level);
        return;
    }
    dictAdd(server.blessed_keys, sdsdup(keyname), (void *)(uintptr_t)level);
}

static void blessedSetDel(sds keyname) {
    dictDelete(server.blessed_keys, keyname);
}

/* Public: add/update a blessed key in the RAM set. Called from dbAddInternal()
 * when a key arrives with the bless metadata attached (RDB load, RESTORE,
 * cluster slot migration, COPY, MOVE, RENAME) - the keymeta rdb_load callback
 * has no key name, so this is the single point that sees key + metadata together. */
void blessTrackKey(sds keyname, uint64_t level) {
    blessedSetPut(keyname, level);
}

/* Public: drop the whole set (FLUSHDB/FLUSHALL). */
void blessedFlushAll(void) {
    if (server.blessed_keys) dictEmpty(server.blessed_keys, NULL);
}

/* Guarding queries. Safe before blessInit() and for unblessed keys (→ 0). */
static uint64_t blessGetFlags(sds keyname) {
    if (!server.blessed_keys) return 0;
    dictEntry *de = dictFind(server.blessed_keys, keyname);
    return de ? (uint64_t)(uintptr_t)dictGetVal(de) : 0;
}

/* True if the key must not be evicted. Consulted by performEvictions(). */
int blessNoEvict(sds keyname) { return (blessGetFlags(keyname) & BLESS_NOEVICT) != 0; }

/* True if the key must stay in RAM (never swapped to flash). Consulted by the
 * ROF swap-out selector. */
int blessNoSwap(sds keyname) { return (blessGetFlags(keyname) & BLESS_NOSWAP) != 0; }

/* ---- keymeta class callbacks ---- */

/* Persist the 1-byte level. The framework only calls this when the value is
 * not the reset sentinel, so AUFERRE keys are never written. */
static void blessRdbSave(RedisModuleIO *io, void *reserved, uint64_t *meta) {
    UNUSED(reserved);
    if (rdbSaveLen(io->rio, *meta) == -1) io->error = 1;
}

static int blessRdbLoad(RedisModuleIO *io, uint64_t *meta, int encver) {
    UNUSED(encver);
    uint64_t v = rdbLoadLen(io->rio, NULL);
    if (v == RDB_LENERR) { io->error = 1; return -1; }
    *meta = v;
    return 1; /* attach; RAM set is populated later in dbAddInternal (io has no key). */
}

/* Logical removal on the main thread (DEL, expire, overwrite). We use unlink
 * rather than free() because free() may run on a background (lazyfree) thread
 * and must not touch the keyspace/globals. */
static void blessUnlink(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    UNUSED(meta);
    if (ctx->from_key) blessedSetDel(ctx->from_key->ptr);
}

/* RENAME: drop the old name here; the new name is (re)added via dbAddInternal.
 * Returning non-zero keeps the metadata attached to the renamed key. */
static int blessRename(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    UNUSED(meta);
    if (ctx->from_key) blessedSetDel(ctx->from_key->ptr);
    return 1;
}

/* COPY/MOVE: keep the metadata; the destination key is added to the set via
 * dbAddInternal once it becomes live. */
static int blessKeep(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    UNUSED(ctx);
    UNUSED(meta);
    return 1;
}

/* Register the BLESS keymeta class and create the RAM set. Called once at
 * startup, right after keyMetaInit() and before any RDB load. */
void blessInit(void) {
    server.blessed_keys = dictCreate(&blessedDictType);

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

/* BLESS SET key FLAG [FLAG ...]
 * FLAG is one of: NOEVICT, NOSWAP, AETERNUS (=NOEVICT), VELOX (=NOEVICT|NOSWAP),
 * AUFERRE/NONE (clear). Flags may be given as separate arguments and/or joined
 * in a single '|'-separated token. */
static void blessSetCommand(client *c) {
    uint64_t flags = 0;
    int sawAuferre = 0, sawFlag = 0;

    for (int j = 3; j < c->argc; j++) {
        int parts;
        sds *tok = sdssplitlen(c->argv[j]->ptr, sdslen(c->argv[j]->ptr), "|", 1, &parts);
        if (tok == NULL) { addReplyError(c, "syntax error"); return; }
        for (int i = 0; i < parts; i++) {
            sds t = tok[i];
            if (sdslen(t) == 0) continue; /* from a bare '|' token or trailing '|' */
            if (!strcasecmp(t, "noevict"))       { flags |= BLESS_NOEVICT; sawFlag = 1; }
            else if (!strcasecmp(t, "noswap"))   { flags |= BLESS_NOSWAP;  sawFlag = 1; }
            else if (!strcasecmp(t, "aeternus")) { flags |= BLESS_NOEVICT; sawFlag = 1; }
            else if (!strcasecmp(t, "velox"))    { flags |= BLESS_ALL;     sawFlag = 1; }
            else if (!strcasecmp(t, "auferre") || !strcasecmp(t, "none")) { sawAuferre = 1; }
            else {
                addReplyErrorFormat(c, "unknown bless flag '%s'", t);
                sdsfreesplitres(tok, parts);
                return;
            }
        }
        sdsfreesplitres(tok, parts);
    }

    if (sawAuferre && sawFlag) {
        addReplyError(c, "AUFERRE cannot be combined with other flags");
        return;
    }
    if (!sawAuferre && !sawFlag) {
        addReplyError(c, "syntax error: no bless flags given");
        return;
    }
    if (sawAuferre) flags = BLESS_NONE; /* clear all protections */

    robj *o = lookupKeyWrite(c->db, c->argv[2]);
    if (o == NULL) { addReplyErrorObject(c, shared.nokeyerr); return; }

    sds keyname = c->argv[2]->ptr;
    int already = (dictFind(server.blessed_keys, keyname) != NULL);

    if (flags == BLESS_NONE) {
        if (!already) { addReply(c, shared.ok); return; } /* nothing to unbless */
        if (keyMetaSetMetadata(c->db, o, server.bless_class_id, BLESS_NONE) == NULL) {
            addReplyError(c, "failed to update key metadata");
            return;
        }
        blessedSetDel(keyname);
    } else {
        /* Per-node cap applies only to newly blessed keys; re-flagging is free. */
        if (!already && (int)dictSize(server.blessed_keys) >= server.bless_max_keys) {
            addReplyError(c, "reached the maximum number of blessed keys (bless-max-keys)");
            return;
        }
        if (keyMetaSetMetadata(c->db, o, server.bless_class_id, flags) == NULL) {
            addReplyError(c, "failed to update key metadata");
            return;
        }
        blessedSetPut(keyname, flags);
    }

    keyModified(c, c->db, c->argv[2], NULL, 1);
    notifyKeyspaceEvent(NOTIFY_GENERIC, "bless", c->argv[2], c->db->id);
    server.dirty++;
    addReply(c, shared.ok);
}

/* BLESS GET key -> the key's flags (nil if not blessed). */
static void blessGetCommand(client *c) {
    uint64_t flags = blessGetFlags(c->argv[2]->ptr);
    if (flags == BLESS_NONE) { addReplyNull(c); return; }
    addReplyBlessFlags(c, flags);
}

/* BLESS is a container. All subcommands share this dispatcher (OBJECT-style);
 * per-subcommand arity and key specs are enforced by the command table. */
void blessCommand(client *c) {
    const char *sub = c->argv[1]->ptr;
    if (!strcasecmp(sub, "set")) {
        blessSetCommand(c);
    } else if (!strcasecmp(sub, "get")) {
        blessGetCommand(c);
    } else if (!strcasecmp(sub, "count")) {
        addReplyLongLong(c, dictSize(server.blessed_keys));
    } else if (!strcasecmp(sub, "list")) {
        /* Reply is a map: key name -> flags. */
        addReplyMapLen(c, dictSize(server.blessed_keys));
        dictIterator *di = dictGetIterator(server.blessed_keys);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL) {
            sds name = dictGetKey(de);
            uint64_t flags = (uint64_t)(uintptr_t)dictGetVal(de);
            addReplyBulkCBuffer(c, name, sdslen(name));
            addReplyBlessFlags(c, flags);
        }
        dictReleaseIterator(di);
    } else if (!strcasecmp(sub, "help")) {
        const char *help[] = {
            "SET <key> <NOEVICT|NOSWAP|AUFERRE ...>",
            "    Bless a key (AUFERRE clears all flags).",
            "GET <key>",
            "    Show a key's bless flags.",
            "COUNT",
            "    Number of blessed keys.",
            "LIST",
            "    Map of blessed key -> flags.",
            NULL
        };
        addReplyHelp(c, help);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

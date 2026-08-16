/*
 * keyattr - generic per-key attributes.
 *
 * One keymeta class ("ATTR") stores a uint64_t bitmask per key. Each owner
 * (e.g. bless) claims some bits and registers callbacks; this layer only ever
 * tests (mask & owner_flags) and never interprets a bit. Owners define their
 * own masks in their own files (see the reserved-bits table in server.h) and
 * register once with the combined mask of the bits they manage.
 */

#include "server.h"

typedef struct keyAttrOwner {
    uint64_t flags;                                     /* bits this owner manages */
    void (*track)(redisDb *db, sds key, uint64_t mask); /* key gained a bit -> its index */
    void (*untrack)(redisDb *db, sds key);              /* key removed -> drop from index */
    void (*aof)(RedisModuleIO *io, uint64_t mask);      /* re-emit its command(s) */
} keyAttrOwner;

/* ponytail: fixed small table; one slot per owner (module), not per bit. Bump
 * KEY_ATTR_MAX_OWNERS if more attribute-owning modules than this are added. */
#define KEY_ATTR_MAX_OWNERS 8
static keyAttrOwner owners[KEY_ATTR_MAX_OWNERS];
static int ownerCount = 0;

void keyAttrRegister(uint64_t flags,
                     void (*track)(redisDb *, sds, uint64_t),
                     void (*untrack)(redisDb *, sds),
                     void (*aof)(RedisModuleIO *, uint64_t)) {
    serverAssert(ownerCount < KEY_ATTR_MAX_OWNERS);
    owners[ownerCount++] = (keyAttrOwner){flags, track, untrack, aof};
}

/* Read a key's attribute mask inline from its keymeta (0 if none / uninit). */
uint64_t keyAttrGet(kvobj *kv) {
    uint64_t mask = 0;
    if (server.key_attr_class_id > 0)
        keyMetaGetMetadata(server.key_attr_class_id, kv, &mask);
    return mask;
}

/* Route a live key's mask to each owner claiming a set bit. Called from dbAdd*
 * where key + metadata first meet (RDB load, RESTORE, migration, COPY/MOVE/RENAME);
 * the keymeta rdb_load callback has no key name, so this is the point that does. */
void keyAttrTrackKey(redisDb *db, sds key, uint64_t mask) {
    for (int i = 0; i < ownerCount; i++)
        if (mask & owners[i].flags) owners[i].track(db, key, mask);
}

static void keyAttrUntrackKey(redisDb *db, sds key, uint64_t mask) {
    for (int i = 0; i < ownerCount; i++)
        if (mask & owners[i].flags) owners[i].untrack(db, key);
}

/* ---- ATTR keymeta class callbacks (generic; operate on the raw mask) ---- */

static void attrRdbSave(RedisModuleIO *io, void *reserved, uint64_t *meta) {
    UNUSED(reserved);
    if (rdbSaveLen(io->rio, *meta) == -1) io->error = 1;
}

static int attrRdbLoad(RedisModuleIO *io, uint64_t *meta, int encver) {
    UNUSED(encver);
    uint64_t v = rdbLoadLen(io->rio, NULL);
    if (v == RDB_LENERR) { io->error = 1; return -1; }
    *meta = v;
    return 1; /* attach; the DB indexes are populated later in dbAdd* (io has no key). */
}

/* Main-thread removal (DEL, expire, overwrite): drop the key from every owner's
 * index. unlink (not free) because free() may run on a lazyfree thread and must
 * not touch the keyspace/globals. */
static void attrUnlink(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    if (ctx->from_key && ctx->from_dbid >= 0)
        keyAttrUntrackKey(&server.db[ctx->from_dbid], ctx->from_key->ptr, *meta);
}

/* RENAME: drop the old name; the new name is re-added via dbAdd*. */
static int attrRename(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    if (ctx->from_key && ctx->from_dbid >= 0)
        keyAttrUntrackKey(&server.db[ctx->from_dbid], ctx->from_key->ptr, *meta);
    return 1;
}

/* COPY/MOVE: keep the mask on the destination (added to its index via dbAdd*). */
static int attrKeep(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    UNUSED(ctx);
    UNUSED(meta);
    return 1;
}

/* AOF rewrite carries no keymeta, so each owner re-emits its own command(s). */
static void attrAofRewrite(RedisModuleIO *io, void *reserved, uint64_t meta) {
    UNUSED(reserved);
    for (int i = 0; i < ownerCount; i++)
        if ((meta & owners[i].flags) && owners[i].aof) owners[i].aof(io, meta);
}

/* Register the ATTR keymeta class once at startup, before owners register and
 * before any RDB load. */
void keyAttrInit(void) {
    KeyMetaClassConf conf;
    memset(&conf, 0, sizeof(conf));
    conf.flags = (1u << KEY_META_FLAG_ALLOW_IGNORE); /* older servers skip gracefully */
    conf.reset_value = 0;                            /* empty mask = not present */
    conf.rdb_save = attrRdbSave;
    conf.rdb_load = attrRdbLoad;
    conf.unlink = attrUnlink;
    conf.rename = attrRename;
    conf.copy = attrKeep;
    conf.move = attrKeep;
    conf.aof_rewrite = attrAofRewrite;
    server.key_attr_class_id = keyMetaClassCreate(NULL, "ATTR", 0, &conf);
    serverAssert(server.key_attr_class_id >= KEY_META_ID_MODULE_FIRST);
}

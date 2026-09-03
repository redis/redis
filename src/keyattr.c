/*
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * keyattr - generic per-key attributes.
 *
 * One keymeta class ("ATTR") stores a uint64_t bitmask per key. Each owner
 * (e.g. bless) claims some bits and registers callbacks; this layer only ever
 * tests (mask & owner_flags) and never interprets a bit. Owners define their
 * own masks in their own files (see the reserved-bits table in server.h) and
 * register once with the combined mask of the bits they manage.
 */

#include "server.h"

/* at most 8 serialized bits per owner; bump if an owner ever needs more. */
#define KEY_ATTR_MAX_WIRE 8

typedef struct keyAttrOwner {
    uint64_t flags;                                     /* bits this owner manages */
    void (*track)(redisDb *db, sds key, uint64_t mask); /* key gained a bit -> its index */
    void (*untrack)(redisDb *db, sds key);              /* key removed -> drop from index */
    void (*aof)(RedisModuleIO *io, uint64_t mask);      /* re-emit its command(s) */
    keyAttrWire wire[KEY_ATTR_MAX_WIRE];                /* bit -> RDB opcode (this owner's) */
    int wireLen;
} keyAttrOwner;

/* ponytail: fixed small table; one slot per owner (module), not per bit. Bump
 * KEY_ATTR_MAX_OWNERS if more attribute-owning modules than this are added. */
#define KEY_ATTR_MAX_OWNERS 8
/* Registered owners, packed from slot 0. A registered owner always has flags!=0,
 * so a zero-flags slot marks the end of the table (no separate count needed). */
static keyAttrOwner owners[KEY_ATTR_MAX_OWNERS];

void keyAttrRegister(uint64_t flags,
                     const keyAttrWire *wire, int wireLen,
                     void (*track)(redisDb *, sds, uint64_t),
                     void (*untrack)(redisDb *, sds),
                     void (*aof)(RedisModuleIO *, uint64_t))
{
    serverAssert(flags != 0);                    /* 0 flags marks an empty slot */
    serverAssert(wireLen <= KEY_ATTR_MAX_WIRE);
    int i = 0;
    while (i < KEY_ATTR_MAX_OWNERS && owners[i].flags) i++;
    serverAssert(i < KEY_ATTR_MAX_OWNERS);
    keyAttrOwner *o = &owners[i];
    o->flags = flags; o->track = track; o->untrack = untrack; o->aof = aof;
    o->wireLen = wireLen;
    for (int j = 0; j < wireLen; j++) o->wire[j] = wire[j];
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
    for (int i = 0; i < KEY_ATTR_MAX_OWNERS && owners[i].flags; i++)
        if (mask & owners[i].flags) owners[i].track(db, key, mask);
}

static void keyAttrUntrackKey(redisDb *db, sds key, uint64_t mask) {
    for (int i = 0; i < KEY_ATTR_MAX_OWNERS && owners[i].flags; i++)
        if (mask & owners[i].flags) owners[i].untrack(db, key);
}

/* Value overwrite (SET k v2): the ATTR keymeta value is preserved on the new
 * object (kept by db.c and carried by keyMetaTransition), but attrUnlink already
 * dropped the key from the owners' indexes. Re-add it so the whole attribute set
 * survives the overwrite - only BLESS SET NONE / key removal clears it. */
void keyAttrOnOverwrite(redisDb *db, robj *key, kvobj *kv) {
    uint64_t mask = keyAttrGet(kv);
    if (mask) keyAttrTrackKey(db, key->ptr, mask);
}

/* ---- RDB / DUMP serialization ----
 * Each attribute bit serializes as its OWN payload-less opcode (supplied by the
 * owner via keyAttrRegister), so the on-disk format is fully independent of the
 * RAM bit layout - change how a bit is stored in RAM and only the owner's wire
 * entry moves; the opcode on disk stays fixed. Written to the RDB file per-key
 * before TYPE by rdb.c, read back in rdbResolveKeyType. */

/* Write one opcode for each attribute bit the key carries. 0 ok, -1 on I/O error. */
int keyAttrRdbSave(rio *rdb, kvobj *kv) {
    /* Fast path: the metabit says the key has no ATTR value, so skip the keymeta
     * lookup and the owner scan entirely (the case for almost every key). */
    if (!(kv->metabits & KEY_ATTR_METABIT)) return 0;
    uint64_t mask = keyAttrGet(kv);
    for (int i = 0; i < KEY_ATTR_MAX_OWNERS && owners[i].flags; i++) {
        if (!(mask & owners[i].flags)) continue;   /* key has none of this owner's bits */
        for (int j = 0; j < owners[i].wireLen; j++)
            if (mask & owners[i].wire[j].bit)
                if (rdbSaveType(rdb, owners[i].wire[j].rdbOpcode) == -1) return -1;
    }
    return 0;
}

/* If `opcode` is an attribute opcode, return the RAM bit it maps to; else 0. */
uint64_t keyAttrBitForOpcode(int opcode) {
    for (int i = 0; i < KEY_ATTR_MAX_OWNERS && owners[i].flags; i++)
        for (int j = 0; j < owners[i].wireLen; j++)
            if (owners[i].wire[j].rdbOpcode == opcode) return owners[i].wire[j].bit;
    return 0;
}

/* ---- ATTR keymeta class lifecycle callbacks (generic; operate on the raw mask) ---- */

/* Main-thread removal (DEL, expire, overwrite): drop the key from every owner's
 * index. unlink (not free) because free() may run on a lazyfree thread and must
 * not touch the keyspace/globals. */
static void attrUnlink(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    if (ctx->from_key && ctx->from_dbid >= 0)
        keyAttrUntrackKey(&server.db[ctx->from_dbid], ctx->from_key->ptr, *meta);
}

/* RENAME/MOVE: drop the source's index entry; the destination is re-added via
 * dbAdd*. (MOVE zeroes the source slot before dbDelete, so attrUnlink won't fire
 * there - the untrack must happen here.) */
static int attrRename(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    if (ctx->from_key && ctx->from_dbid >= 0)
        keyAttrUntrackKey(&server.db[ctx->from_dbid], ctx->from_key->ptr, *meta);
    return 1;
}

/* COPY: keep the source's entry; the destination is added to its index via dbAdd*. */
static int attrKeep(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    UNUSED(ctx);
    UNUSED(meta);
    return 1;
}

/* AOF rewrite carries no keymeta, so each owner re-emits its own command(s). */
static void attrAofRewrite(RedisModuleIO *io, void *reserved, uint64_t meta) {
    UNUSED(reserved);
    for (int i = 0; i < KEY_ATTR_MAX_OWNERS && owners[i].flags; i++)
        if ((meta & owners[i].flags) && owners[i].aof) owners[i].aof(io, meta);
}

/* Register the ATTR keymeta class once at startup, before owners register and
 * before any RDB load. */
void keyAttrInit(void) {
    KeyMetaClassConf conf;
    memset(&conf, 0, sizeof(conf));
    conf.reset_value = 0;                            /* empty mask = not present */
    /* No rdb_save/rdb_load: attributes serialize via their own RDB opcodes
     * (keyAttrRdbSave / keyAttrBitForOpcode), not the generic keymeta format. A
     * new opcode isn't skippable by old servers, so the RDB version is bumped -
     * old servers reject a newer RDB up front (upgrade-only, like any format). */
    conf.unlink = attrUnlink;
    conf.rename = attrRename;
    conf.copy = attrKeep;
    conf.move = attrRename;   /* MOVE must drop the source index, like RENAME */
    conf.aof_rewrite = attrAofRewrite;
    server.key_attr_class_id = keyMetaClassCreate(NULL, "ATTR", 0, &conf);
    serverAssert(server.key_attr_class_id >= KEY_META_ID_MODULE_FIRST);
}

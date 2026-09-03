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
 * A thin framework over keymeta: one keymeta class ("ATTR") stores a uint64_t
 * bitmask per key. Each owner (e.g. bless) claims some bits and registers
 * callbacks; this layer only tests (mask & owner_flags) and never interprets a
 * bit. Owners define their own masks in their own files and register once with
 * the combined mask of the bits they manage.
 *
 * Reserved ATTR bits (each owner defines its masks in its own file):
 *   bit 0    : bless NO-EVICT   (bless.c)
 *   bits 1.. : free
 */

#ifndef __KEYATTR_H
#define __KEYATTR_H

#include <stdint.h>
#include "sds.h"
#include "object.h"

/* fwd decls */
struct redisDb;
struct RedisModuleIO;

/* Metabit of the ATTR keymeta class within a kvobj's `metabits` field. The class
 * id is assigned at startup (keyAttrInit), so this is a runtime bit, not a
 * compile-time constant like KEY_META_MASK_EXPIRE. */
#define KEY_ATTR_METABIT (1u << server.key_attr_class_id)

/* Create the ATTR keymeta class (once at startup, before owners register). */
void keyAttrInit(void);

/* RDB wire mapping the owner supplies: one payload-less opcode per attribute
 * bit. The opcode's presence on disk sets the bit; nothing about the RAM layout
 * is serialized. Keeps the opcode's identity with its owner, not in this layer. */
typedef struct keyAttrWire {
    uint64_t bit;    /* attribute bit within the ATTR mask */
    int rdbOpcode;   /* RDB opcode that represents it on disk */
} keyAttrWire;

/* Register an attribute owner: the bits it manages, its RDB wire mapping, and
 * its callbacks.
 *   wire/wireLen - bit -> RDB opcode entries (may be NULL/0 if not persisted)
 *   track   - key gained a managed bit -> add to the owner's index
 *   untrack - key removed              -> drop from the owner's index
 *   aof     - re-emit the owner's command(s) on AOF rewrite (may be NULL) */
void keyAttrRegister(uint64_t flags,
                     const keyAttrWire *wire, int wireLen,
                     void (*track)(struct redisDb *db, sds key, uint64_t mask),
                     void (*untrack)(struct redisDb *db, sds key),
                     void (*aof)(struct RedisModuleIO *io, uint64_t mask));

/* Route a live key's mask to each owner claiming a set bit (called from dbAdd*). */
void keyAttrTrackKey(struct redisDb *db, sds key, uint64_t mask);

/* Read a key's attribute mask inline from its keymeta (0 if none). */
uint64_t keyAttrGet(kvobj *kv);

/* Re-attach a key's attributes to their indexes after a value overwrite, whose
 * generic unlink handling detached them (called from dbSetValue). */
void keyAttrOnOverwrite(struct redisDb *db, robj *key, kvobj *kv);

/* RDB serialization: each attribute bit is written as its own payload-less
 * opcode. keyAttrRdbSave writes them for a key; keyAttrBitForOpcode maps an
 * opcode back to its RAM bit (0 if the opcode isn't an attribute opcode). */
int keyAttrRdbSave(rio *rdb, kvobj *kv);
uint64_t keyAttrBitForOpcode(int opcode);

/* True if `op` is an attribute RDB opcode (payload-less; presence flags the
 * key). Lets callers ask "is this ours?" without caring which bit it maps to. */
#define KEY_ATTR_IS_RDB_OPCODE(op) (keyAttrBitForOpcode(op) != 0)

#endif /* __KEYATTR_H */

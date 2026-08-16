/*
 * keyattr - generic per-key attributes.
 *
 * A thin framework over keymeta: one keymeta class ("ATTR") stores a uint64_t
 * bitmask per key. Each owner (e.g. bless) claims some bits and registers
 * callbacks; this layer only tests (mask & owner_flags) and never interprets a
 * bit. Owners define their own masks in their own files and register once with
 * the combined mask of the bits they manage.
 *
 * Reserved ATTR bits (each owner defines its masks in its own file):
 *   bit 0    : bless NO-EVICT   (t_bless.c)
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

/* Register an attribute owner: the bits it manages plus its callbacks.
 *   track   - key gained a managed bit -> add to the owner's index
 *   untrack - key removed              -> drop from the owner's index
 *   aof     - re-emit the owner's command(s) on AOF rewrite (may be NULL) */
void keyAttrRegister(uint64_t flags,
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

#endif /* __KEYATTR_H */

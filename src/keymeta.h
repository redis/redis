/*
 * Key Metadata (keymeta)
 *
 * High-level idea
 * ----------------
 * keymeta is a framework for attaching & maintaining metadata to keys. 
 * 
 * - Up to 8 different metadata classes can be registered globally. First one
 *   is reserved for EXPIRE.
 * - Each class has a unique ID and name, like modules datat-types names, yet 
 *   it has its own namespace.
 * - Each key metadata class provides a set of callbacks for key lifecycle operations, 
 *   ensuring consistent handling across copy, rename, logical removal (unlink), 
 *   actual deallocation (free), persistence (RDB/AOF), and defragmentation. 
 * - Each key can carry up to 8 independent metadata values. Each value is related 
 *   to a specific metadata class. 
 * - The 8-byte slot can either hold inline data or a pointer/handle to a larger, 
 *   externally managed structure.
 *
 * Relation to other components
 * ----------------------------
 * - kvobj: 8 bits metabits field in kvobj is used to indicate active metadata.
 *   bit number corresponds to class ID. 
 * - Expiration: class ID 0 is reserved for TTL/expire; 
 * - Registration: redisServer.keyMetaClass[] stores registered classes. Modules
 *   register via keyMetaClassCreate (see redismodule.h) and may provide callbacks
 *   for persistence, copy/rename behavior, and lifecycle hooks (unlink/free).
 * - modules: modules can register metadata classes and provide callbacks.  
 */

#ifndef __KEYMETA_H
#define __KEYMETA_H

#include <stdint.h>
#include <stddef.h>
#include "sds.h"
#include "object.h"

/* fwd decls */
struct redisDb;
struct redisObject;
struct RedisModuleIO;
struct RedisModuleKeyOptCtx;
struct RedisModuleDefragCtx;
struct RedisModule;
typedef int KeyMetaClassId; /* Index into redisServer.keyMetaClass[] */

/* kvmeta - Metadata to be attached to kvobj */
#define KEY_META_ID_EXPIRE        0 /* Must be first */
/* IDs 1..7 are available for modules */
#define KEY_META_ID_MODULE_FIRST  1
#define KEY_META_ID_MODULE_LAST   7
#define KEY_META_ID_MAX           8

#define KEY_META_MAX_NUM_MODULES  (KEY_META_ID_MODULE_LAST - KEY_META_ID_MODULE_FIRST + 1)

#define KEY_META_MASK_NONE        0
#define KEY_META_MASK_MODULES     (((1U << KEY_META_MAX_NUM_MODULES) - 1) << KEY_META_ID_MODULE_FIRST)
#define KEY_META_MASK_EXPIRE      (1U << KEY_META_ID_EXPIRE)

typedef int (*KeyMetaLoadFunc)(RedisModuleIO *rdb, uint64_t *meta, int encver);
typedef void (*KeyMetaSaveFunc)(RedisModuleIO *rdb, void *value, uint64_t *meta);
typedef void (*KeyMetaAOFRewriteFunc)(RedisModuleIO *aof, void *value, uint64_t meta);
typedef void (*KeyMetaFreeFunc)(const char *keyname, uint64_t meta);
typedef int (*KeyMetaCopyFunc)(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta);
typedef int (*KeyMetaRenameFunc)(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta);
typedef int (*KeyMetaDefragFunc)(RedisModuleDefragCtx *ctx, RedisModuleString *keyname, uint64_t meta);
typedef size_t (*KeyMetaMemUsageFunc)(struct RedisModuleKeyOptCtx *ctx, size_t sample_size, uint64_t meta);
typedef size_t (*KeyMetaFreeEffortFunc)(struct RedisModuleKeyOptCtx *ctx, uint64_t meta);
typedef void (*KeyMetaUnlinkFunc)(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta);
typedef int (*KeyMetaMoveFunc)(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta);

/* For explanation, see struct RedisModuleKeyMetaClassConfig */
typedef struct KeyMetaClassConf {
#define KEY_META_FLAG_ALLOW_IGNORE 0   /* Ignore silently on RDB load, if module not avail */
    uint64_t flags;
    uint64_t reset_value;
    
    int (*copy)(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta);
    int (*rename)(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta);
    int (*move)(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta);
    void (*unlink)(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta);
    void (*free)(const char *keyname, uint64_t meta);

    /****************************** TBD: ******************************/
    int (*rdb_load)(struct RedisModuleIO *rdb, uint64_t *meta, int metaver);
    void (*rdb_save)(struct RedisModuleIO *rdb, void *value, uint64_t *meta);
    void (*aof_rewrite)(struct RedisModuleIO *aof, void *value, uint64_t meta);
    int (*defrag) (struct RedisModuleDefragCtx *ctx, struct redisObject *key, uint64_t meta);
    size_t (*mem_usage)(struct RedisModuleKeyOptCtx *ctx, size_t sample_size, uint64_t meta);
    size_t (*free_effort)(struct RedisModuleKeyOptCtx *ctx, uint64_t meta);
} KeyMetaClassConf;

/* KeyMetaSpec - Used by dbAddInternal() to describe metadata of a new key */
typedef struct KeyMetaSpec {
    uint16_t numMeta; /* Num active metadata entries. Aligned with metabits */
    uint16_t metabits;

    /* Array of metadata values. Entries are populated in reverse order
     * (from the end of the array backward) to make bulk copying with
     * memcpy more efficient. During insertion, the next slot is:
     *            meta[KEY_META_ID_MAX - (++numMeta)]
     *            
     * For example if numMeta=2, and metabits=0b101, then the last entry holds 
     * value for class 0, and the previous entry holds value for class 2.  
     */
    uint64_t meta[KEY_META_ID_MAX];
} KeyMetaSpec;

/* init Keys metadata on server startup */
void keyMetaInit(void);

/* Key metadata event callbacks */
void keyMetaOnUnlink(struct redisDb *db, robj *key,kvobj *kv);
void keyMetaOnFree(kvobj *kv);
void keyMetaOnRename(struct redisDb *db,  kvobj *kv, robj *oldKey, robj *newKey, KeyMetaSpec *kms);
void keyMetaOnMove(kvobj *kv, robj *key, int srcDbId, int dstDbId, KeyMetaSpec *kms);
void keyMetaOnCopy(kvobj *kv, robj *srcKey, robj *dstKey, int srcDbId, int dstDbId, KeyMetaSpec *kms);
int keyMetaOnAof(rio *r, robj *key, kvobj *kv, int dbid);

void keyMetaResetModuleValues(kvobj *kv);
void keyMetaTransition(kvobj *kvOld, kvobj *kvNew);

/* return 0 if failed to create. Otherwise return handle (between 1 and 7) */
KeyMetaClassId keyMetaClassCreate(struct RedisModule *ctx, const char *metaname, int metaver, KeyMetaClassConf *conf);
/* Destroy (release) a previously created class. Return 1 on success, 0 on failure. */
int keyMetaClassRelease(KeyMetaClassId class_id);

kvobj *keyMetaSetMetadata(struct redisDb *db, kvobj *kv, KeyMetaClassId kmcId, uint64_t metadata);
int keyMetaGetMetadata(KeyMetaClassId kmcId, kvobj *kv, uint64_t *metadata);
int keyMetaRemoveMetadata(KeyMetaClassId kmcId, RedisModuleKey *key);

/* KeyMetaSpec helpers */
static inline void keyMetaSpecInit(KeyMetaSpec *keymeta);
static inline void keyMetaSpecAdd(KeyMetaSpec *keymeta, int metaid, uint64_t metaval);

/* bit operations on metabits */
static inline uint32_t getNumMeta(uint16_t metabits);
static inline uint32_t getModuleMetaBits(uint16_t metabits);

/********** Inline functions **********/

static inline void keyMetaResetValues(kvobj *kv) {
    if (unlikely(kv->metabits & KEY_META_MASK_MODULES))
        keyMetaResetModuleValues(kv);
    /* Must be first meta (optimized) */
    if (kv->metabits & KEY_META_MASK_EXPIRE)
        ((uint64_t *)kv)[-1] = -1;
}

static inline void keyMetaSpecInit(KeyMetaSpec *keymeta) {
    /* Enough to init metabits and numMeta. meta[] is not used. */
    keymeta->metabits = 0;
    keymeta->numMeta = 0;
}

/* Add metadata to keymeta spec. metaid must be in range 0..7. */
static inline void keyMetaSpecAdd(KeyMetaSpec *keymeta, int metaid, uint64_t metaval) {
    keymeta->metabits |= 1 << metaid ;
    keymeta->numMeta++;
    /* populated in reverse order */
    keymeta->meta[KEY_META_ID_MAX - keymeta->numMeta] = metaval;
}

static inline uint32_t getNumMeta(uint16_t metabits) {
    /* Assumed expire is always first meta */
    return __builtin_popcount(metabits);
}

static inline uint32_t getModuleMetaBits(uint16_t metabits) {
    return metabits & KEY_META_MASK_MODULES;
}

#endif // __KEYMETA_H

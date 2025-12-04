/* Keys Metadata subsystem initialization and callbacks scaffolding. */

#include "server.h"
#include <string.h>

typedef enum KeyMetaClassState {
    CLASS_STATE_FREE = 0, /* Free must be 0. */
    CLASS_STATE_INUSE = 1,
    CLASS_STATE_RELEASED = 2,
} KeyMetaClassState;

/* Key metadata class */
typedef struct KeyMetaClass {
    char name[5];                 /* 4-char name of the class */
    ModuleEntityId mEntity;       /* module key metadata name and ID. */
    KeyMetaClassConf conf;        /* copy of configuration callbacks and options */
    KeyMetaClassState state;
    uint32_t classSpecSerialized; /* 32-bits serialized form of [name][version][flags] */
} KeyMetaClass;

static KeyMetaClass keyMetaClass[KEY_META_ID_MAX];

/* Encode 64-bit module-style ID and 32-bit serialized class ID 
 *
 * Takes a 4-character name (e.g., "KMT1"), version (0-31), and flags, and:
 * 1. Validates the 4-char name uses valid character set
 * 2. Validates version is 5 bits (0-31) for metadata classes
 * 3. Generates 9-char entity name with "META-" prefix (e.g., "META-KMT1")
 * -> Encodes the 9-char name into 64-bit ID (compatible with moduleTypeEncodeId).
 * -> Encodes compact 32-bit RDB/DUMP serialized class ID:
 *            31                           8 7     3 2   0
 *            ┌─────────────────────────────┬───────┬─────┐
 *            │   4-char name (24 bits)     │ ver   │flags│
 *            │   (6 bits per char)         │(5 bit)│(3b) │
 *            └─────────────────────────────┴───────┴─────┘
 *
 * Returns 64-bit entityID and keyMetaClassSer, 0 on error 
 */
static uint64_t keyMetaEncodeId(const char *name, int metaver, uint64_t flags,
                                char *fullname, uint32_t *keyMetaClassSer) {
/* Character set for metadata class names (same as module types). */
    static const char *cset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                              "abcdefghijklmnopqrstuvwxyz"
                              "0123456789_-";
    /* Validate name is exactly 4 characters */
    if (strlen(name) != 4) return 0;

    /* Validate version range (5 bits = 0-31 for metadata classes) */
    if (metaver < 0 || metaver > 31) return 0;
    
    /* Generate 9-char name with "META-" prefix */
    memcpy(fullname, "META-", 5);
    memcpy(fullname + 5, name, 4);
    fullname[9] = '\0';

    /* Encode 9-char name into 64-bit entityId (module-style ID, 54 bits name
     * plus 10 bits version) */
    uint64_t entityId = 0;
    /* Encode last 4-char into 32-bit serialized class ID (24b name + 5b version + 3b flags) */
    uint32_t classSer = 0;
    for (int j = 0; j < 9; j++) {
        char *p = strchr(cset, fullname[j]);
        if (!p) return 0; /* Invalid character in name */
        unsigned long pos = p - cset;
        entityId = (entityId << 6) | pos;
        if (j >= 5) classSer = (classSer << 6) | pos;
    }
    /* Add version (10 bits) to entityId */
    entityId = (entityId << 10) | metaver;

    /* Add version (5 bits) and flags (3 bits) to serialized class ID if requested */
    if (keyMetaClassSer) {
        classSer = (classSer << 5) | (metaver & 0x1F);
        classSer = (classSer << 3) | (flags & 0x07);
        *keyMetaClassSer = classSer;
    }

    return entityId; /* Return 64-bit module ID (0 on error) */
}

/* Return -1 if not found, 1..7 for slot if INUSE, alreadyReleased if found but released */
static int keyMetaClassLookupByName(const char *name, int *alreayReleased) {
    *alreayReleased = 0;
    if (!name) return -1;
    
    for (int i = KEY_META_ID_MODULE_FIRST; i <= KEY_META_ID_MODULE_LAST; i++) {
        if (keyMetaClass[i].state == CLASS_STATE_FREE) 
            continue;
        if (memcmp(keyMetaClass[i].name, name, 4) != 0) 
            continue;
        if (keyMetaClass[i].state == CLASS_STATE_INUSE) 
            return i;
        if (keyMetaClass[i].state == CLASS_STATE_RELEASED) {
            *alreayReleased = 1;
            return i;
        }
    }
    return -1;
}

/* Initialize server.keyMeta with defaults and reserve built-in classes. */
void keyMetaInit(void) {
    memset(keyMetaClass, 0, sizeof(KeyMetaClass) * KEY_META_ID_MAX);

    /* Slot 0 is EXPIRE, built-in and always active. */
    keyMetaClass[KEY_META_ID_EXPIRE].state = CLASS_STATE_INUSE;
    keyMetaClass[KEY_META_ID_EXPIRE].conf.flags = 0; /* No special flags for EXPIRE. */
    keyMetaClass[KEY_META_ID_EXPIRE].conf.reset_value = (uint64_t)-1; /* -1 means no expire */
}

/* Prepare key metadata spec for copy of `srcKv` */
void keyMetaOnCopy(kvobj *kv, robj *srcKey, robj *dstKey, int srcDbId, int dstDbId,
                   KeyMetaSpec *keymeta)
{
    uint64_t *pMeta = ((uint64_t *)kv) - 1;
    if (kv->metabits & KEY_META_MASK_EXPIRE) {
        if (*pMeta != (uint64_t)-1)
            keyMetaSpecAdd(keymeta, KEY_META_ID_EXPIRE, *pMeta);
        pMeta--;
    }

    uint32_t mbits = kv->metabits >> KEY_META_ID_MODULE_FIRST;
    if (likely(mbits == 0)) return;

    int keyMetaId = KEY_META_ID_MODULE_FIRST;
    struct RedisModuleKeyOptCtx ctx = {srcKey, dstKey, srcDbId, dstDbId };
    do {
        if (mbits & 1) {
            serverAssert(keyMetaClass[keyMetaId].state == CLASS_STATE_INUSE);
            /* Copy metadata from kv to temporary storage keymeta */
            uint64_t tmpMeta = *pMeta--;
            /* if callback provided, then it is to decide whether to keep
             * or discard the metadata. Otherwise, it is discarded. */
            if ((keyMetaClass[keyMetaId].conf.copy) &&
                (keyMetaClass[keyMetaId].conf.copy(&ctx, &tmpMeta)))
                keyMetaSpecAdd(keymeta, keyMetaId, tmpMeta);
        }
        mbits >>= 1;
        keyMetaId++;
    } while (mbits != 0);
}

/* Prepare metadata spec for rename of `kv` */
void keyMetaOnRename(struct redisDb *db,  kvobj *kv, robj *oldKey, robj *newKey, KeyMetaSpec *kms) {
    uint64_t *pMeta = ((uint64_t *)kv) - 1;

    /* Handle builtin expire: add only if set and value != -1, but always advance
     * the pointer when the expire bit is set since the slot exists either way. */
    if (kv->metabits & KEY_META_MASK_EXPIRE) {
        if (*pMeta != ((uint64_t)-1))
            keyMetaSpecAdd(kms, KEY_META_ID_EXPIRE, *pMeta);
        pMeta--; /* skip expire slot */
    }

    /* Process module metadata. Default on rename: keep if no callback. */
    uint32_t mbits = kv->metabits >> KEY_META_ID_MODULE_FIRST;
    if (likely(mbits == 0)) return;

    int keyMetaId = KEY_META_ID_MODULE_FIRST;
    struct RedisModuleKeyOptCtx ctx = { oldKey, newKey, db ? db->id : -1, db ? db->id : -1 };
    do {
        if (mbits & 1) {
            serverAssert(keyMetaClass[keyMetaId].state == CLASS_STATE_INUSE);
            uint64_t tmpMeta = *pMeta; /* read current module slot */
            int keep = 1; /* default: keep if no callback */
            if (keyMetaClass[keyMetaId].conf.rename)
                keep = keyMetaClass[keyMetaId].conf.rename(&ctx, &tmpMeta);
            if (keep) {
                keyMetaSpecAdd(kms, keyMetaId, tmpMeta);
                /* Set old metadata slot to reset_value to prevent free callback */
                *pMeta = keyMetaClass[keyMetaId].conf.reset_value;
            }
            pMeta--; /* advance to next module slot */
        }
        mbits >>= 1;
        keyMetaId++;
    } while (mbits != 0);
}

/* Prepare metadata spec for move of `kv` from srcDbId to dstDbId */
void keyMetaOnMove(kvobj *kv, robj *key, int srcDbId, int dstDbId, KeyMetaSpec *kms) {
    uint64_t *pMeta = ((uint64_t *)kv) - 1;

    /* Handle builtin expire: add only if set and value != -1, but always advance
     * the pointer when the expire bit is set since the slot exists either way. */
    if (kv->metabits & KEY_META_MASK_EXPIRE) {
        if (*pMeta != ((uint64_t)-1))
            keyMetaSpecAdd(kms, KEY_META_ID_EXPIRE, *pMeta);
        pMeta--; /* skip expire slot */
    }

    /* Process module metadata. Default on move: keep if no callback. */
    uint32_t mbits = kv->metabits >> KEY_META_ID_MODULE_FIRST;
    if (likely(mbits == 0)) return;

    int keyMetaId = KEY_META_ID_MODULE_FIRST;
    struct RedisModuleKeyOptCtx ctx = { key, NULL, srcDbId, dstDbId};
    do {
        if (mbits & 1) {
            serverAssert(keyMetaClass[keyMetaId].state == CLASS_STATE_INUSE);
            uint64_t tmpMeta = *pMeta; /* read current module slot */
            int keep = 1; /* default: keep if no callback */
            if (keyMetaClass[keyMetaId].conf.move)
                keep = keyMetaClass[keyMetaId].conf.move(&ctx, &tmpMeta);
            if (keep) {
                keyMetaSpecAdd(kms, keyMetaId, tmpMeta);
                /* If keep, set old metadata to reset_value to prevent free callback */
                *pMeta = keyMetaClass[keyMetaId].conf.reset_value;
            }
            pMeta--; /* advance to next module slot */
        }
        mbits >>= 1;
        keyMetaId++;
    } while (mbits != 0);
}

/*
 * keyMetaOnUnlink() - when a key is logically overwritten/removed from the DB
 *
 * - Runs before the value object is actually freed (see keyMetaOnFree()).
 * - Runs on the main thread (same timing as moduleNotifyKeyUnlink()).
 * - Allows modules to detach per-key metadata from external structures, update
 *   auxiliary indexes, stats, etc.
 * - Skips the built-in EXPIRE slot (handled by caller).
 * - Iterates over module metadata bits and, for every set bit, invokes the
 *   class-specific unlink callback if provided.
 */
void keyMetaOnUnlink(redisDb *db, robj *key, kvobj *kv) {
    /* Skip builtin expire slot if present; no action for expire itself here. */
    uint64_t *pMeta = ((uint64_t *)kv) - 1;
    if (kv->metabits & KEY_META_MASK_EXPIRE)
        pMeta--;

    /* Iterate module metadata and invoke per-class unlink if provided. */
    uint32_t mbits = kv->metabits >> KEY_META_ID_MODULE_FIRST;
    if (likely(mbits == 0)) return;

    /* Build operation context for modules: from_key = key name, to_key = NULL. */
    struct RedisModuleKeyOptCtx ctx = { key, NULL, db ? db->id : -1, -1 };

    int keyMetaId = KEY_META_ID_MODULE_FIRST;
    do {
        if (mbits & 1) {
            serverAssert(keyMetaClass[keyMetaId].state == CLASS_STATE_INUSE);
            if (keyMetaClass[keyMetaId].conf.unlink) {
                keyMetaClass[keyMetaId].conf.unlink(&ctx, pMeta);
            }
            pMeta--;
        }
        mbits >>= 1;
        keyMetaId++;
    } while (mbits != 0);
}

/*
 * keyMetaOnFree() - when kvobj's metadata is actually being freed 
 *
 * - Called after the key has been logically unlinked (see keyMetaOnUnlink())
 * - This is the place to reclaim resources associated with per-key metadata (e.g.,
 *   free external allocations referenced by the 8-byte metadata value).
 * - May run in a background thread; therefore module code invoked here must NOT
 *   access Redis keyspace or perform operations that require the main thread.
 *   Only perform thread-safe memory cleanup pertinent to the metadata.
 * - For each attached metadata invokes class-specific 'free' callback if given, 
 */
void keyMetaOnFree(kvobj *kv) {
    /* Skip builtin expire slot if present; no action needed for expire itself. */
    uint64_t *pMeta = ((uint64_t *)kv) - 1;
    if (kv->metabits & KEY_META_MASK_EXPIRE)
        pMeta--;

    /* Iterate module metadata and invoke per-class free if provided. */
    uint32_t mbits = kv->metabits >> KEY_META_ID_MODULE_FIRST;
    if (likely(mbits == 0)) return;

    int keyMetaId = KEY_META_ID_MODULE_FIRST;
    const char *keyname = kvobjGetKey(kv);
    do {
        if (mbits & 1) {
            serverAssert(keyMetaClass[keyMetaId].state == CLASS_STATE_INUSE);
            uint64_t meta = *pMeta--; /* consume this module's metadata slot */
            if (keyMetaClass[keyMetaId].conf.free)
                keyMetaClass[keyMetaId].conf.free(keyname, meta);
        }
        mbits >>= 1;
        keyMetaId++;
    } while (mbits != 0);
}

/* returns 0 on error, 1 on success. */
int keyMetaOnAof(rio *r, robj *key, kvobj *kv, int dbid) {
    /* Skip builtin expire slot if present; no action needed for expire itself. */
    uint64_t *pMeta = ((uint64_t *)kv) - 1;
    if (kv->metabits & KEY_META_MASK_EXPIRE)
        pMeta--;

    /* Iterate module metadata and invoke per-class aof_rewrite if provided */
    uint32_t mbits = kv->metabits >> KEY_META_ID_MODULE_FIRST;
    if (likely(mbits == 0)) return 1;

    int keyMetaId = KEY_META_ID_MODULE_FIRST;
    do {
        if (mbits & 1) {
            serverAssert(keyMetaClass[keyMetaId].state == CLASS_STATE_INUSE);

            /* If module provided aof_rewrite callback, invoke it */
            if (keyMetaClass[keyMetaId].conf.aof_rewrite) {
                uint64_t meta = *pMeta;
                RedisModuleIO io;
                moduleInitIOContext(&io, &keyMetaClass[keyMetaId].mEntity, r, key, dbid);
                keyMetaClass[keyMetaId].conf.aof_rewrite(&io, kv, meta);
                if (io.ctx) {
                    moduleFreeContext(io.ctx);
                    zfree(io.ctx);
                }
                if (io.error) return 0;
            }
            pMeta--;
        }
        mbits >>= 1;
        keyMetaId++;
    } while (mbits != 0);

    return 1;
}

/* Move entire metadata from old to new kvobj as is */
void keyMetaTransition(kvobj *kvOld, kvobj *kvNew) {
    /* Precondition: */
    debugServerAssert(kvOld->metabits>>KEY_META_ID_MODULE_FIRST);
    
    /* Skip builtin expire slot if present; no action needed for expire itself. */
    uint64_t *pMetaOld = ((uint64_t *)kvOld) - 1;
    if (kvOld->metabits & KEY_META_MASK_EXPIRE) pMetaOld--;
    uint64_t *pMetaNew = ((uint64_t *)kvNew) - 1;
    if (kvNew->metabits & KEY_META_MASK_EXPIRE) pMetaNew--;
    
    uint32_t mbitsOld = kvOld->metabits >> KEY_META_ID_MODULE_FIRST;
    uint32_t mbitsNew = kvNew->metabits >> KEY_META_ID_MODULE_FIRST;
    if (likely(mbitsOld == 0)) return;
    int keyMetaId = KEY_META_ID_MODULE_FIRST;
    do {
        if (mbitsOld & 1) {
            if (mbitsNew & 1) {
                /* Transition metadata from old to new */
                *pMetaNew-- = *pMetaOld;
                /* Reset old metadata value to prevent double-free */
                *pMetaOld-- = keyMetaClass[keyMetaId].conf.reset_value;
            } else {
                /* Leave metadata in old key as is */
                pMetaOld--;
            }
        } else {
            /* Update pMetaNew if needed (No need to reset value in new key, 
             * assuming it was initialized earlier). */
            pMetaNew -= mbitsNew & 1;  
        }
        
        mbitsOld >>= 1;
        mbitsNew >>= 1;
        keyMetaId++;
    } while (mbitsOld);
}

/* Create a new metadata class. Returns class ID (1-7) on success, 0 on failure.
 * 
 * context - In case of a module, pass the module pointer. Otherwise NULL.
 */
KeyMetaClassId keyMetaClassCreate(RedisModule *context, const char *name,
                                  int metaver, KeyMetaClassConf *conf) {
    if (!conf) return 0;

    /* Validate and encode ID. This also validates 4-char name and generates "META-" prefix. */
    char fullname[10];
    uint32_t classSpecSerialized;
    /* Resovle: entityId, fullname, keyMetaClassSer */
    uint64_t entityId = keyMetaEncodeId(name,
                                        metaver,
                                        conf->flags & KEY_META_FLAGS_RDB_MASK,
                                        fullname,
                                        &classSpecSerialized);
    if (entityId == 0) return 0;

    /* Check for name conflicts using 4-char name. Allow reuse of RELEASED; forbid if INUSE. */
    int alreayReleased;
    int slot = keyMetaClassLookupByName(name, &alreayReleased);

    if (alreayReleased) {
        /* If already released, then reuse the slot. */
    } else {
        /* Assert class is registered for first time */
        serverAssert(slot == -1);

        /* Find free slot */
        for (int i = KEY_META_ID_MODULE_FIRST; i <= KEY_META_ID_MODULE_LAST; i++) {
            if (keyMetaClass[i].state == CLASS_STATE_FREE) {
                slot = i;
                break;
            }
        }
        if (slot == -1) return 0; /* no free slots */
    }

    KeyMetaClass *pKeyMetaClass = &keyMetaClass[slot];

    /* Store 4-char short name */
    memcpy(pKeyMetaClass->name, name, 4);
    pKeyMetaClass->name[4] = '\0';

    /* Store 9-char full name with "META-" prefix */
    memcpy(pKeyMetaClass->mEntity.name, fullname,sizeof(fullname));
    pKeyMetaClass->mEntity.id = entityId;
    pKeyMetaClass->mEntity.module = context;
    pKeyMetaClass->state = CLASS_STATE_INUSE;
    pKeyMetaClass->classSpecSerialized = classSpecSerialized;
    pKeyMetaClass->conf = *conf; /* Copy config as is. */
    return slot; /* Return handle (1..7). */
}

/* Destroy (release) a class by its ID. Returns 1 on success, 0 on failure. */
int keyMetaClassRelease(KeyMetaClassId id) {
    if (!(id >= KEY_META_ID_MODULE_FIRST && id <= KEY_META_ID_MODULE_LAST))
        return 0;

    if (keyMetaClass[id].state != CLASS_STATE_INUSE)
        return 0;

    keyMetaClass[id].state = CLASS_STATE_RELEASED;
    return 1;
}

/* Set a module metadata value on an opened key. Returns the new kvobj pointer (may be reallocated).
 * Returns NULL on failure. The caller must update any references to the old kv pointer. */
kvobj *keyMetaSetMetadata(redisDb *db, kvobj *kv, KeyMetaClassId id, uint64_t metadata) {
    serverAssert(id >= KEY_META_ID_MODULE_FIRST && id <= KEY_META_ID_MODULE_LAST);

    /* Class must be active */
    if (keyMetaClass[id].state != CLASS_STATE_INUSE)
        return NULL;

    /* If metadata already attached, just update it in place. */
    if (kv->metabits & (1u << id)) {
        *kvobjMetaRef(kv, id) = metadata;
        return kv;
    }

    /* We need to grow kv to add a new 8-byte metadata slot. This may reallocate
     * the object, so we must carefully preserve and restore:
     * - The key's expires dictionary entry (if TTL is set)
     * - The global Hash Field Expires (HFE) registration for hash objects
     * - All existing metadata values (including expire value)
     */

    sds key = kvobjGetKey(kv);
    int slot = getKeySlot(key);

    /* Preserve HFE registration for hash objects (embedded in object memory). */
    uint64_t subexpiry = EB_EXPIRE_TIME_INVALID;
    if (kv->type == OBJ_HASH)
        subexpiry = estoreRemove(db->subexpires, slot, kv);

    /* Preserve existing expire value (and whether an expires entry exists). */
    long long old_expire_val = kvobjGetExpire(kv);
    
    /* We'll need the key's link in the main dictionary to update pointer if reallocated. */
    dictEntryLink keyLink = kvstoreDictFindLink(db->keys, slot, key, NULL);
    serverAssert(keyLink != NULL);

    /* If the key has an actual TTL (expire != -1), also preserve the expires dict link. */
    dictEntryLink exLink = NULL;
    if (old_expire_val != -1) {
        exLink = kvstoreDictFindLink(db->expires, slot, key, NULL);
        serverAssert(exLink != NULL);
    }

    /* Reallocate kv with the new metadata bit enabled. kvobjSet may return a new 
     * ptr. Takes care to transition existing metadata as needed. */
    kv = kvobjSet(key, kv, kv->metabits | (1u << id));
    kvstoreDictSetAtLink(db->keys, slot, kv, &keyLink, 0);

    /* Set new metadata */
    *kvobjMetaRef(kv, id) = metadata;
    
    /* If there was an expires entry (expire != -1), update its kv pointer. */
    if (exLink) {
        ((uint64_t *)kv)[-1] = old_expire_val; /* expiry must be first meta */
        kvstoreDictSetAtLink(db->expires, slot, kv, &exLink, 0);
    }

    /* Re-register in HFE if needed. */
    if (subexpiry != EB_EXPIRE_TIME_INVALID)
        estoreAdd(db->subexpires, slot, kv, subexpiry);

    return kv;
}

/* Retrieve a module metadata value from an opened key. Returns 1 on success, 0 otherwise. */
int keyMetaGetMetadata(KeyMetaClassId kmcId, kvobj *kv, uint64_t *metadata) {
    serverAssert(kmcId >= KEY_META_ID_MODULE_FIRST && kmcId <= KEY_META_ID_MODULE_LAST);
    
    if (keyMetaClass[kmcId].state != CLASS_STATE_INUSE) 
        return 0;
    
    if (!(kv->metabits & (1u << kmcId))) 
        return 0; /* metadata not attached */

    *metadata = *kvobjMetaRef(kv, kmcId);
    return 1;
}

/* Blindly reset modules metadata values to reset_value */
void keyMetaResetModuleValues(kvobj *kv) {
    /* Precondition: */
    debugServerAssert(kv->metabits & KEY_META_MASK_MODULES);

    uint64_t *pMeta = ((uint64_t *)kv) - 1;
    uint32_t mbits = kv->metabits;
    int keyMetaId = 0;
    do {
        if (mbits & 1)
            *pMeta-- = keyMetaClass[keyMetaId].conf.reset_value;

        mbits >>= 1;
        keyMetaId++;
    } while (mbits != 0);
}

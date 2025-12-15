/* Read keymeta.h for high-level overview. */

#include "server.h"
#include <string.h>

/* Encoding constants for metadata class names and serialization */
#define KM_NAME_LEN           4    /* Short name length (e.g., "KMT1") */
#define KM_PREFIX             "META-"
#define KM_PREFIX_LEN         5    /* Length of "META-" prefix */
#define KM_FULLNAME_LEN       9    /* Full name length: "META-xxxx" */
#define KM_ENC_CHAR_BITS      6    /* Bits per character in encoding */
#define KM_CHARSET_SIZE       64   /* Size of character set (2^6) */
#define KM_VER_BITS           5    /* Bits for version in 32-bit class spec */
#define KM_VER_MAX            31   /* Max version value (2^5 - 1) */
#define KM_FLAGS_BITS         3    /* Bits for flags in 32-bit class spec */
#define KM_FLAGS_MASK         0x7  /* Mask for 3-bit flags */
#define KM_VER_MASK           0x1F /* Mask for 5-bit version */
#define KM_CHAR_MASK          0x3F /* Mask for 6-bit character */
#define KM_ENTITY_VER_BITS    10  /* Bits for version in 64-bit entity ID */
#define KM_CLASS_SPEC_SIZE    4    /* Size of 32-bit class spec in bytes */
#define KM_EXPIRE_RESET_VALUE ((uint64_t)-1) /* Sentinel: no expiration */

/* Cast const away only for initialization */
#define KM_SET_CONST_CONF(conf)  (*((KeyMetaClassConf *) (&conf)))

/* Character set for metadata class names (same as module types). */
static const char *keyMetaCharSet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                    "abcdefghijklmnopqrstuvwxyz"
                                    "0123456789_-";

typedef enum KeyMetaClassState {
    CLASS_STATE_FREE = 0, /* Free must be 0. */
    CLASS_STATE_INUSE = 1,
    CLASS_STATE_RELEASED = 2,
} KeyMetaClassState;

/* Key metadata class */
typedef struct KeyMetaClass {
    char name[5];                 /* 4-char name of the class */
    ModuleEntityId mEntity;       /* module key metadata name and ID. */
    const KeyMetaClassConf conf;  /* copy of config */
    KeyMetaClassState state;      /* FREE/INUSE/RELEASED */
    uint32_t classSpecEncoded;    /* See keyMetaClassEncode() */
} KeyMetaClass;
static KeyMetaClass keyMetaClass[KEY_META_ID_MAX];

/* Encode 64b For module entity encode. Encode 32b class spec for RDB. 
 *
 * Takes a 4-character name (e.g., "KMT1"), version (0-31), and flags, validates 
 * 4-char name uses valid character set. Version is 5 bits (0-31).
 * 
 * >> ENCODING 32-BIT CLASS SPEC
 * Encodes compact 32-bit class Spec for RDB/DUMP serialization:
 *            31                           8 7     3 2   0
 *            ┌───────────────────────────────┬───────┬─────┐
 *            │   4-char name "xxxx"(24 bits) │ ver   │flags│
 *            │   (6 bits per char)           │(5 bit)│(3b) │
 *            └───────────────────────────────┴───────┴─────┘
 *
 * >> ENCODING MODULE-STYLE ID
 * Generates 9-char entity name with "META-" prefix (e.g., "META-KMT1"), 54 bits
 * in total, plus 10 bits version (values 0-31). Compatible with moduleTypeEncodeId:
 *            63                                     10 9           0
 *            ┌───────────────────────────────────────┬─────────────┐
 *            │   9-char name (56 bits) "META-xxxx"   │  ver (0-31) │
 *            │   (6 bits per char)                   │   (10 bit)  │
 *            └───────────────────────────────────────┴─────────────┘
 * 
 */
static uint64_t keyMetaClassEncode(const char *name, int metaver, uint64_t flags,
                                char *fullname, uint32_t *rdbEncodedValue) {
    /* Validate name is exactly 4 characters */
    if (strlen(name) != KM_NAME_LEN) return 0;

    /* Validate version range (5 bits = 0-31 for metadata classes) */
    if (metaver < 0 || metaver > KM_VER_MAX) return 0;

    /* Generate 9-char name with "META-" prefix */
    memcpy(fullname, KM_PREFIX, KM_PREFIX_LEN);
    memcpy(fullname + KM_PREFIX_LEN, name, KM_NAME_LEN);
    fullname[KM_FULLNAME_LEN] = '\0';

    /* Encode 9-char name into 64-bit entityId (module-style ID, 54 bits name
     * plus 10 bits version) */
    uint64_t encName9Chars = 0;
    /* Encode last 4-char into 32-bit serialized class ID (24b name + 5b version + 3b flags) */
    uint32_t encName4chars = 0;
    for (int j = 0; j < KM_FULLNAME_LEN; j++) {
        char *p = strchr(keyMetaCharSet, fullname[j]);
        if (!p) return 0; /* Invalid character in name */
        unsigned long pos = p - keyMetaCharSet;
        encName9Chars = (encName9Chars << KM_ENC_CHAR_BITS) | pos;
        if (j >= KM_PREFIX_LEN) encName4chars = (encName4chars << KM_ENC_CHAR_BITS) | pos;
    }

    /* Encodes compact 32-bit RDB/DUMP serialized class Spec */
    *rdbEncodedValue = ((encName4chars << KM_VER_BITS) | metaver) << KM_FLAGS_BITS | (flags & KM_FLAGS_MASK);

    /* Encodes the 9-char name into 64-bit ID (compatible with moduleTypeEncodeId) */
    uint64_t entityId = (encName9Chars << KM_ENTITY_VER_BITS) | metaver;
    return entityId;
}

/* Decode 32-bit class spec from RDB/DUMP format
 *
 * Takes a 32-bit keyMetaClassSer and extracts:
 * - 4-character name (24 bits, 6 bits per char)
 * - version (5 bits, 0-31)
 * - flags (3 bits)
 *
 * This is the reverse of the encoding done in keyMetaClassEncode().
 *
 * Returns 1 on success, 0 on error (invalid encoding).
 */
int keyMetaClassDecode(uint32_t value, char *name, int *metaver, uint8_t *flags) {
    debugServerAssert(name && metaver && flags);

    /* Extract flags (lowest 3 bits) */
    *flags = value & KM_FLAGS_MASK;
    value >>= KM_FLAGS_BITS;

    /* Extract version (next 5 bits) */
    *metaver = value & KM_VER_MASK;
    value >>= KM_VER_BITS;

    /* Extract 4-char name (24 bits, 6 bits per char, big-endian) */
    for (int i = KM_NAME_LEN - 1; i >= 0; i--) {
        unsigned int pos = value & KM_CHAR_MASK;
        if (pos >= KM_CHARSET_SIZE) return 0; /* Invalid character position */
        name[i] = keyMetaCharSet[pos];
        value >>= KM_ENC_CHAR_BITS;
    }
    name[KM_NAME_LEN] = '\0';

    /* Verify no extra bits set */
    if (value != 0) return 0;

    return 1;
}

/* Return -1 if not found, 1..7 for slot if INUSE, alreadyReleased if found but released */
static int keyMetaClassLookupByName(const char *name, int *alreayReleased) {
    *alreayReleased = 0;
    if (!name) return -1;

    for (int i = KEY_META_ID_MODULE_FIRST; i <= KEY_META_ID_MODULE_LAST; i++) {
        if (keyMetaClass[i].state == CLASS_STATE_FREE)
            continue;
        if (memcmp(keyMetaClass[i].name, name, KM_NAME_LEN) != 0)
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
    debugServerAssert(CLASS_STATE_FREE == 0);

    /* Slot 0 is EXPIRE, built-in and always active. */
    keyMetaClass[KEY_META_ID_EXPIRE].state = CLASS_STATE_INUSE;
    KM_SET_CONST_CONF(keyMetaClass[KEY_META_ID_EXPIRE].conf).flags = 0;
    KM_SET_CONST_CONF(keyMetaClass[KEY_META_ID_EXPIRE].conf).reset_value = KM_EXPIRE_RESET_VALUE;
}

/* Prepare key metadata spec for copy of `srcKv` */
void keyMetaOnCopy(kvobj *kv, robj *srcKey, robj *dstKey, int srcDbId, int dstDbId,
                   KeyMetaSpec *keymeta)
{
    uint64_t *pMeta = ((uint64_t *)kv) - 1;
    if (kv->metabits & KEY_META_MASK_EXPIRE) {
        if (*pMeta != KM_EXPIRE_RESET_VALUE)
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
            if (tmpMeta != keyMetaClass[keyMetaId].conf.reset_value &&
                keyMetaClass[keyMetaId].conf.copy &&
                keyMetaClass[keyMetaId].conf.copy(&ctx, &tmpMeta))
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
        if (*pMeta != KM_EXPIRE_RESET_VALUE)
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
            if (tmpMeta != keyMetaClass[keyMetaId].conf.reset_value &&
                (!keyMetaClass[keyMetaId].conf.rename || 
                 keyMetaClass[keyMetaId].conf.rename(&ctx, &tmpMeta))) 
            {
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
        if (*pMeta != KM_EXPIRE_RESET_VALUE)
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
            if (tmpMeta != keyMetaClass[keyMetaId].conf.reset_value &&
                (!keyMetaClass[keyMetaId].conf.move || 
                 keyMetaClass[keyMetaId].conf.move(&ctx, &tmpMeta))) 
            {
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

            if (*pMeta != keyMetaClass[keyMetaId].conf.reset_value &&
                keyMetaClass[keyMetaId].conf.unlink) 
            {
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
            if (meta != keyMetaClass[keyMetaId].conf.reset_value &&
                keyMetaClass[keyMetaId].conf.free)
                keyMetaClass[keyMetaId].conf.free(keyname, meta);
        }
        mbits >>= 1;
        keyMetaId++;
    } while (mbits != 0);
}

int rdbLoadSkipMetaIfAllowed(rio *rdb, char *cname, int flags) {
    static int countDownNotice = 0;
    static rio *lastRdb = NULL;
    if (lastRdb != rdb) {
        countDownNotice = 10;
        lastRdb = rdb;
    }

    /* Check ALLOW_IGNORE flag */
    if (flags & (1 << KEY_META_FLAG_ALLOW_IGNORE)) {
        if (countDownNotice-- > 0) {
            /* Skip this metadata gracefully */
            serverLog(LL_NOTICE, "Skipping metadata for class '%s' (not registered or missing rdb_load)", cname);
        }

        /* Skip the metadata value by loading and discarding it.
         * The metadata format is: VALUE (variable length) + EOF marker.
         *
         * The VALUE is saved using RedisModule_Save* functions which use module opcodes
         * (RDB_MODULE_OPCODE_SINT, etc.), so we use rdbLoadCheckModuleValue() to skip it.
         *
         * Note: rdbLoadCheckModuleValue() reads opcodes until it finds RDB_MODULE_OPCODE_EOF,
         * so it consumes the EOF marker as well. We don't need to read it separately. */
        robj *dummy = rdbLoadCheckModuleValue(rdb, cname);
        if (dummy == NULL) {
            serverLog(LL_WARNING, "Corrupted metadata value for class '%s'", cname);
            return -1;
        }

        decrRefCount(dummy);
        return 0;
    } else {
        serverLog(LL_WARNING, "RDB load key metadata failed: Class '%s' not registered or missing rdb_load().", cname);
        return -1;
    }
}

/* Load module metadata from RDB.
 * Returns 0 on success, -1 on error.
 * Stores loaded metadata in the provided KeyMetaSpec structure.
 *
 * Format (same as save):
 *   1B: NUM_CLASSES (already read by caller)
 *   For each class:
 *     4B: CLASS_SPEC (32-bit classSpecEncoded)
 *     ?B: VALUE (from rdb_load callback)
 *     1B: RDB_MODULE_OPCODE_EOF
 */
int rdbLoadKeyMetadata(rio *rdb, int dbid, int numClasses, KeyMetaSpec *kms) {
    if (numClasses > KEY_META_MAX_NUM_MODULES) {
        serverLog(LL_WARNING, "Too many metadata classes: %d (max %d)",
                  numClasses, KEY_META_MAX_NUM_MODULES);
        return -1;
    }

    for (int i = 0; i < numClasses; i++) {
        /* Read 32-bit encoded class spec */
        uint32_t encClassSpec;
        if (rioRead(rdb, &encClassSpec, KM_CLASS_SPEC_SIZE) == 0) return -1;

        /* Deserialize to get name, version, flags */
        char name[5];
        int metaver;
        uint8_t flags;
        if (keyMetaClassDecode(encClassSpec, name, &metaver, &flags) == 0) {
            serverLog(LL_WARNING, "Corrupted metadata class spec: 0x%08x", encClassSpec);
            return -1;
        }

        /* Lookup class by name */
        int alreadyReleased = 0;
        KeyMetaClassId classId = keyMetaClassLookupByName(name, &alreadyReleased);

        /* If class not found or released, check ALLOW_IGNORE flag */
        if (classId == -1 || alreadyReleased) {
            int rc = rdbLoadSkipMetaIfAllowed(rdb, name, flags);
            if (rc == -1) return -1;
            continue;
        } 
        
        /* Verify version matches */
        KeyMetaClass *pClass = &keyMetaClass[classId];
        debugServerAssert(pClass->state == CLASS_STATE_INUSE);

        /* If no rdb_load callback, check ALLOW_IGNORE flag */
        if (pClass->conf.rdb_load == NULL) {
            /* No rdb_load callback - check ALLOW_IGNORE flag */
            int rc = rdbLoadSkipMetaIfAllowed(rdb, name, flags);
            if (rc == -1) return -1;
            continue;            
        }

        RedisModuleIO io;
        /* We don't have the key yet, so pass NULL for now */
        moduleInitIOContext(&io, &pClass->mEntity, rdb, NULL, dbid);

        uint64_t meta = 0;
        int rc = pClass->conf.rdb_load(&io, &meta, metaver);

        /* Read EOF marker */
        uint64_t eof = rdbLoadLen(rdb, NULL);
        if (eof != RDB_MODULE_OPCODE_EOF) {
            serverLog(LL_WARNING, "Missing EOF after key metadata '%s' (got 0x%llx)",
                      name, (unsigned long long)eof);
            io.error = 1;
        }

        if (io.ctx) {
            moduleFreeContext(io.ctx);
            zfree(io.ctx);
        }

        if (io.error) return -1;

        /* Handle rdb_load return value:
         *   1: Attach metadata to key (success)
         *   0: Ignore/skip metadata (not an error)
         *  -1: Error - abort RDB load */
        if (rc == 1) {
            /* Attach metadata. Cannot overflow since loop is bounded by numClasses <= KEY_META_MAX_NUM_MODULES */
            keyMetaSpecAdd(kms, classId, meta);
        } else if (rc == 0) {
            /* Ignore/skip - don't attach metadata, continue loading */
        } else if (rc == -1) {
            /* Error - abort RDB load */
            serverLog(LL_WARNING,
                "RDB load failed: rdb_load callback for metadata class '%s' returned error", name);
            return -1;
        } else {
            /* Invalid return value */
            serverLog(LL_WARNING,
                "RDB load failed: rdb_load callback for metadata class '%s' "
                "returned invalid value %d (expected -1, 0, or 1)",
                name, rc);
            return -1;
        }
    }
    return 0; /* Success */
}

/* Save all key metadata to RDB using lazy header writing.
 * We accumulate class data (CLASS_SPEC + VALUE + EOF) in a temporary buffer,
 * counting classes that actually write data. Only if count > 0, we write the
 * opcode and NUM_CLASSES to RDB, followed by the accumulated payload.
 * This avoids writing RDB_OPCODE_KEY_META when no module writes any data.
 *
 * Format:
 *   1B: RDB_OPCODE_KEY_META
 *   ?B: NUM_CLASSES (count of classes that wrote data)
 *   For each class:
 *     4B: CLASS_SPEC (32-bit classSpecEncoded)
 *     ?B: VALUE (from rdb_save callback)
 *     1B: RDB_MODULE_OPCODE_EOF
 *     
  * Returns -1 on error, 0 on success.
 */
int rdbSaveKeyMetadata(rio *rdb, robj *key, kvobj *kv, int dbid) {

    /* Check if there are any module metadata bits set */
    uint32_t mbits = kv->metabits >> KEY_META_ID_MODULE_FIRST;
    if (likely(mbits == 0)) return 0; /* No module metadata */

    /* Skip builtin expire slot if present */
    uint64_t *pMeta = ((uint64_t *)kv) - 1;
    if (kv->metabits & KEY_META_MASK_EXPIRE)
        pMeta--;

    /* Create temporary buffer for payload (class data only, no headers) */
    rio payload_rio;
    rioInitWithBuffer(&payload_rio, sdsempty());

    /* Iterate through classes and accumulate payload */
    int numClasses = 0;
    int keyMetaId = KEY_META_ID_MODULE_FIRST;
    uint32_t mbits_copy = mbits;

    do {
        /* Check if metadata is attached for this class */
        if (mbits_copy & 1) {
            KeyMetaClass *pClass = &keyMetaClass[keyMetaId];
            serverAssert(pClass->state == CLASS_STATE_INUSE);

            if (*pMeta != pClass->conf.reset_value && pClass->conf.rdb_save) {
                /* Write 32-bit class spec to payload buffer */
                uint32_t classSpec = pClass->classSpecEncoded;
                if (rdbWriteRaw(&payload_rio, &classSpec, KM_CLASS_SPEC_SIZE) == -1) goto error;

                size_t bytes_before = sdslen(payload_rio.io.buffer.ptr);

                /* Call module's rdb_save callback */
                RedisModuleIO io;
                moduleInitIOContext(&io, &pClass->mEntity, &payload_rio, key, dbid);
                pClass->conf.rdb_save(&io, kv, pMeta);

                if (io.ctx) {
                    moduleFreeContext(io.ctx);
                    zfree(io.ctx);
                }

                if (io.error) goto error;

                size_t bytes_after = sdslen(payload_rio.io.buffer.ptr);

                /* Check if module actually wrote any data */
                if (bytes_after > bytes_before) {
                    /* Module wrote data - add EOF marker and count it */
                    if (rdbSaveLen(&payload_rio, RDB_MODULE_OPCODE_EOF) == -1) goto error;
                    numClasses++;
                } else {
                    /* Module didn't write data - remove the class spec we wrote.
                     * bytes_before is the length after writing the class spec, so we want
                     * to keep bytes_before - KM_CLASS_SPEC_SIZE bytes. We also need to update the RIO's pos to match. */
                    sdssubstr(payload_rio.io.buffer.ptr, 0, bytes_before - KM_CLASS_SPEC_SIZE);
                    payload_rio.io.buffer.pos = bytes_before - KM_CLASS_SPEC_SIZE;
                }
            }

            pMeta--; /* Move to next metadata slot */
        }
        keyMetaId++;
        mbits_copy >>= 1;
    } while (mbits_copy);

    /* If no classes wrote data, discard everything */
    if (numClasses == 0) {
        sdsfree(payload_rio.io.buffer.ptr);
        return 0;
    }

    /* Now write headers to RDB, followed by payload */
    if (rdbSaveType(rdb, RDB_OPCODE_KEY_META) == -1) goto error;
    if (rdbSaveLen(rdb, numClasses) == -1) goto error;

    /* Write accumulated payload to RDB */
    sds payload = payload_rio.io.buffer.ptr;
    if (rdbWriteRaw(rdb, payload, sdslen(payload)) == -1) {
        sdsfree(payload);
        return -1;
    }

    sdsfree(payload);
    return 0;

error:
    sdsfree(payload_rio.io.buffer.ptr);
    return -1;
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

            uint64_t meta = *pMeta;
            if (meta != keyMetaClass[keyMetaId].conf.reset_value &&
                keyMetaClass[keyMetaId].conf.aof_rewrite) 
            {
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
    char fullname[KM_FULLNAME_LEN+1];
    uint32_t classSpecEncoded;
    /* Resolve: entityId, fullname, keyMetaClassSer */
    uint64_t entityId = keyMetaClassEncode(name,
                                        metaver,
                                        conf->flags & KEY_META_FLAGS_RDB_MASK,
                                        fullname,
                                        &classSpecEncoded);
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
    memcpy(pKeyMetaClass->name, name, KM_NAME_LEN);
    pKeyMetaClass->name[KM_NAME_LEN] = '\0';

    /* Store 9-char full name with "META-" prefix */
    memcpy(pKeyMetaClass->mEntity.name, fullname, KM_FULLNAME_LEN+1);
    pKeyMetaClass->mEntity.id = entityId;
    pKeyMetaClass->mEntity.module = context;
    pKeyMetaClass->state = CLASS_STATE_INUSE;
    pKeyMetaClass->classSpecEncoded = classSpecEncoded;
    KM_SET_CONST_CONF(pKeyMetaClass->conf) = *conf; /* Copy config as is. */
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

/* Add metadata to keymeta spec. Must be in range 0..7 and in order! */
void keyMetaSpecAdd(KeyMetaSpec *keymeta, int metaid, uint64_t metaval) {
    /* Verify added in order and for the first time */
    debugServerAssert(keymeta->metabits == 0 || (1<<metaid) > keymeta->metabits);
    keymeta->metabits |= 1 << metaid ;
    keymeta->numMeta++;
    /* populated in reverse order */
    keymeta->meta[KEY_META_ID_MAX - keymeta->numMeta] = metaval;
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

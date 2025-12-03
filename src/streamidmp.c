/* IDMP (Idempotent Message Producer) utilities for Redis Streams
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "streamidmp.h"
#include <string.h>
#include "xxhash.h"

/* -----------------------------------------------------------------------
 * idmpEntry Management Functions
 * ----------------------------------------------------------------------- */

/* Comparison function for idmpEntry structures in AVL tree.
 * Compares entries by IID (XXH128_hash_t) only.
 * Returns: negative if a < b, 0 if a == b, positive if a > b */
int idmpEntryCompare(const void *a, const void *b) {
    const idmpEntry *ea = (const idmpEntry *)a;
    const idmpEntry *eb = (const idmpEntry *)b;
    
    /* Compare high64 first, then low64 */
    if (ea->iid.high64 != eb->iid.high64) {
        return (ea->iid.high64 > eb->iid.high64) ? 1 : -1;
    }
    if (ea->iid.low64 != eb->iid.low64) {
        return (ea->iid.low64 > eb->iid.low64) ? 1 : -1;
    }
    return 0;
}

/* Create a new idmpEntry with the given IID hash.
 * Returns NULL on allocation failure. */
idmpEntry *idmpEntryCreate(XXH128_hash_t iid, size_t *alloc_size) {
    size_t usable;
    idmpEntry *entry = zmalloc_usable(sizeof(idmpEntry), &usable);
    if (entry == NULL) return NULL;
    
    entry->iid = iid;
    
    if (alloc_size) {
        *alloc_size += usable;
    }
    
    return entry;
}

/* Free an idmpEntry. */
void idmpEntryFree(idmpEntry *entry, size_t *alloc_size) {
    if (entry == NULL) return;
    
    if (alloc_size) {
        *alloc_size -= zmalloc_size(entry);
    }
    
    zfree(entry);
}

/* -----------------------------------------------------------------------
 * IDMP Tracking and Cleanup Functions
 * ----------------------------------------------------------------------- */

/* Register a stream key for IDMP entry tracking.
 * This registers a stream key in the database's stream_idmp_keys dictionary,
 * allowing the cron job handleExpiredIdmpEntries() to periodically check
 * and clean up expired idempotency entries from the stream's idmp_tring.
 *
 * 'c' is the client that is performing the XADD operation with IDMP.
 * 'key' is the stream key object to track.
 *
 * If the key is not already tracked, it is added to stream_idmp_keys and its
 * reference count is incremented. If the key is already being tracked (added
 * by a previous XADD operation), this function does nothing, as the stream
 * is already registered for periodic cleanup. */
void trackStreamIdmpEntries(client *c, robj *key) {
    dictEntry *db_track_entry;
    db_track_entry = dictAddRaw(c->db->stream_idmp_keys, key, NULL);
    if (db_track_entry != NULL) {
        incrRefCount(key);
    }
}

/* Clean up expired idempotency entries from tracked streams. This function
 * is invoked regularly from blockedBeforeSleep() to remove expired entries
 * from the idmp_tring of streams that have idempotency tracking enabled,
 * keeping memory usage under control.
 *
 * The function processes up to CRON_DBS_PER_CALL databases per call in a
 * round-robin fashion, cycling through all databases over multiple invocations.
 * For each database, it iterates through the stream_idmp_keys dictionary.
 * For each tracked stream, it compares the timestamp of entries in the stream's
 * idmp_tring against the expiration threshold (current time - idmp_duration).
 * Entries with timestamps older than the threshold are popped from the front
 * of the tring. When all entries have been removed and the tring becomes empty,
 * the stream key is removed from stream_idmp_keys to stop tracking it. */
void handleExpiredIdmpEntries(void) {
    static unsigned int current_db = 0;
    int dbs_per_call = CRON_DBS_PER_CALL;
    int j;

    if (dbs_per_call > server.dbnum) dbs_per_call = server.dbnum;

    for (j = 0; j < dbs_per_call; j++) {
        redisDb *db = &server.db[current_db % server.dbnum];
        current_db++;

        if (dictIsEmpty(db->stream_idmp_keys))
            continue;

        dictEntry *de;
        dictIterator di;
        dictInitSafeIterator(&di, db->stream_idmp_keys);
        while ((de = dictNext(&di)) != NULL) {
            robj *key = dictGetKey(de);
            kvobj *kv = dbFind(db, key->ptr);

            if (!kv || kv->type != OBJ_STREAM) {
                dictDelete(db->stream_idmp_keys, key);
                continue;
            }

            stream *s = kv->ptr;
            uint64_t expire_time = server.mstime - (s->idmp_duration * 1000);
            while (!tringEmpty(s->idmp_tring)) {
                idmpEntry *entry = (idmpEntry*)tringFront(s->idmp_tring);
                if (entry->id.ms <= expire_time) {
                    tringPopFront(s->idmp_tring);
                } else {
                    break;
                }
            }

            if (tringEmpty(s->idmp_tring)) {
                dictDelete(db->stream_idmp_keys, key);
                continue;
            }
        }
        dictResetIterator(&di);
    }
}

/* -----------------------------------------------------------------------
 * Field-Value Pair Hashing for AUTOIDMP
 * ----------------------------------------------------------------------- */

/* Hash field-value pairs using XXH3_128bits for AUTOIDMP.
 * 
 * This function takes an array of robj pointers representing field-value pairs
 * and the number of pairs. It hashes each field-value pair together using
 * XXH3_128bits and XORs all the pair hashes to produce a final 128-bit hash.
 * 
 * The raw 128-bit hash (16 bytes) is written directly to the provided result buffer.
 * 
 * Algorithm:
 * 1. For each field-value pair:
 *    a. Create a streaming hash state with XXH3_128bits_reset()
 *    b. Update hash with field data using XXH3_128bits_update()
 *    c. Update hash with value data using XXH3_128bits_update()
 *    d. Finalize pair hash with XXH3_128bits_digest()
 *    e. XOR the pair hash with the accumulated result
 * 2. Write final 128-bit hash to result buffer
 * 
 * Parameters:
 *   argv      - Array of robj pointers containing field-value pairs
 *               (argv[0] = field1, argv[1] = value1, argv[2] = field2, ...)
 *   numfields - Number of field-value pairs (not the array length)
 *   result    - Buffer to store the raw 16-byte hash (must be at least 16 bytes)
 * 
 * Returns:
 *   XXH128_hash_t containing the 128-bit hash result */
XXH128_hash_t createIdempotencyHash(robj **argv, int64_t numfields) {
    XXH128_hash_t hash_result = {0, 0};
    XXH3_state_t* state = XXH3_createState();
    if (state == NULL) return hash_result;
    
    char llbuf[LONG_STR_SIZE];
    XXH_errorcode err;
    
    /* Process each field-value pair */
    for (int64_t i = 0; i < numfields; i++) {
        robj *field = argv[i * 2];
        robj *value = argv[i * 2 + 1];
        
        /* Initialize hash state for this pair */
        err = XXH3_128bits_reset(state);
        if (err != XXH_OK) {
            XXH3_freeState(state);
            return (XXH128_hash_t){0, 0};
        }
        
        /* Hash the field */
        long field_len;
        unsigned char *field_data = getObjectReadOnlyString(field, &field_len, llbuf);
        err = XXH3_128bits_update(state, field_data, field_len);
        if (err != XXH_OK) {
            XXH3_freeState(state);
            return (XXH128_hash_t){0, 0};
        }
        
        /* Hash the value */
        long value_len;
        unsigned char *value_data = getObjectReadOnlyString(value, &value_len, llbuf);
        err = XXH3_128bits_update(state, value_data, value_len);
        if (err != XXH_OK) {
            XXH3_freeState(state);
            return (XXH128_hash_t){0, 0};
        }
        
        /* Get the hash for this pair */
        XXH128_hash_t pair_hash = XXH3_128bits_digest(state);
        
        /* XOR with accumulated result */
        hash_result.low64 ^= pair_hash.low64;
        hash_result.high64 ^= pair_hash.high64;
    }
    
    XXH3_freeState(state);
    
    return hash_result;
}

/* Hash a raw buffer using XXH3_128bits for AUTOIDMP.
 * 
 * This function takes a raw character buffer and its length, and produces
 * a 128-bit hash using XXH3_128bits.
 * 
 * Parameters:
 *   data   - Pointer to the data buffer to hash
 *   len    - Length of the data buffer in bytes
 * 
 * Returns:
 *   XXH128_hash_t containing the 128-bit hash result */
XXH128_hash_t createIdempotencyHashFromBuffer(const char *data, size_t len) {
    return XXH3_128bits(data, len);
}


/* IDMP (Idempotent Message Producer) utilities for Redis Streams
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "server.h"
#include "streamidmp.h"
#include <string.h>
#include "../deps/xxhash/xxhash.h"

#ifdef HAVE_AVX2
/* Define __MM_MALLOC_H to prevent importing the memory aligned
 * allocation functions, which we don't use. */
#define __MM_MALLOC_H
#include <immintrin.h>
#endif

#ifdef HAVE_AVX512
/* Define __MM_MALLOC_H to prevent importing the memory aligned
 * allocation functions, which we don't use. */
#define __MM_MALLOC_H
#include <immintrin.h>
#endif

#ifdef HAVE_AVX2
#define BITOP_USE_AVX2 (__builtin_cpu_supports("avx2"))
#else
#define BITOP_USE_AVX2 0
#endif

#ifdef HAVE_AVX512
#define BITOP_USE_AVX512 (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw"))
#else
#define BITOP_USE_AVX512 0
#endif

/* -----------------------------------------------------------------------
 * SIMD-Optimized String Comparison Functions
 * ----------------------------------------------------------------------- */

/* AVX-512 optimized string comparison
 * Compares up to 64 bytes at a time using AVX-512 instructions
 * Returns position where comparison stopped (either at first difference or after processing) */
#ifdef HAVE_AVX512
ATTRIBUTE_TARGET_AVX512
static inline size_t compareStringsAVX512(const char *str_a, const char *str_b, size_t len) {
    size_t i = 0;
    
    /* Process 128 bytes at a time (unrolled 2x) for better throughput */
    while (i + 128 <= len) {
        __m512i va0 = _mm512_loadu_si512((const __m512i *)(str_a + i));
        __m512i vb0 = _mm512_loadu_si512((const __m512i *)(str_b + i));
        __m512i va1 = _mm512_loadu_si512((const __m512i *)(str_a + i + 64));
        __m512i vb1 = _mm512_loadu_si512((const __m512i *)(str_b + i + 64));
        
        __mmask64 cmp_mask0 = _mm512_cmpeq_epi8_mask(va0, vb0);
        __mmask64 cmp_mask1 = _mm512_cmpeq_epi8_mask(va1, vb1);
        
        /* Check both masks - if either has differences, find the first one */
        if (cmp_mask0 != 0xFFFFFFFFFFFFFFFFULL) {
            return i + __builtin_ctzll(~cmp_mask0);
        }
        if (cmp_mask1 != 0xFFFFFFFFFFFFFFFFULL) {
            return i + 64 + __builtin_ctzll(~cmp_mask1);
        }
        i += 128;
    }
    
    /* Compare 64 bytes at a time */
    if (i + 64 <= len) {
        __m512i va = _mm512_loadu_si512((const __m512i *)(str_a + i));
        __m512i vb = _mm512_loadu_si512((const __m512i *)(str_b + i));
        
        __mmask64 cmp_mask = _mm512_cmpeq_epi8_mask(va, vb);
        
        if (cmp_mask != 0xFFFFFFFFFFFFFFFFULL) {
            return i + __builtin_ctzll(~cmp_mask);
        }
        i += 64;
    }
    
    /* Handle remaining 32-63 bytes with AVX2 */
    if (i + 32 <= len) {
        __m256i va = _mm256_loadu_si256((const __m256i *)(str_a + i));
        __m256i vb = _mm256_loadu_si256((const __m256i *)(str_b + i));
        
        __m256i cmp = _mm256_cmpeq_epi8(va, vb);
        int mask = _mm256_movemask_epi8(cmp);
        
        if (mask != -1) {
            return i + __builtin_ctz(~mask);
        }
        i += 32;
    }
    
    /* Handle remaining 16-31 bytes with SSE */
    if (i + 16 <= len) {
        __m128i va = _mm_loadu_si128((const __m128i *)(str_a + i));
        __m128i vb = _mm_loadu_si128((const __m128i *)(str_b + i));
        
        __m128i cmp = _mm_cmpeq_epi8(va, vb);
        int mask = _mm_movemask_epi8(cmp);
        
        if (mask != 0xFFFF) {
            return i + __builtin_ctz(~mask & 0xFFFF);
        }
        i += 16;
    }
    
    /* Handle remaining 8-15 bytes */
    if (i + 8 <= len) {
        uint64_t va, vb;
        memcpy(&va, str_a + i, 8);
        memcpy(&vb, str_b + i, 8);
        if (va != vb) {
            /* Find first differing byte */
            uint64_t xor_val = va ^ vb;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            return i + (__builtin_ctzll(xor_val) >> 3);
#else
            return i + (__builtin_clzll(xor_val) >> 3);
#endif
        }
        i += 8;
    }
    
    return i;
}
#endif

/* AVX2 optimized string comparison
 * Compares up to 32 bytes at a time using AVX2 instructions
 * Returns position where comparison stopped (either at first difference or after processing) */
#ifdef HAVE_AVX2
ATTRIBUTE_TARGET_AVX2
static inline size_t compareStringsAVX2(const char *str_a, const char *str_b, size_t len) {
    size_t i = 0;
    
    /* Process 128 bytes at a time (unrolled 4x) for better throughput */
    while (i + 128 <= len) {
        __m256i va0 = _mm256_loadu_si256((const __m256i *)(str_a + i));
        __m256i vb0 = _mm256_loadu_si256((const __m256i *)(str_b + i));
        __m256i va1 = _mm256_loadu_si256((const __m256i *)(str_a + i + 32));
        __m256i vb1 = _mm256_loadu_si256((const __m256i *)(str_b + i + 32));
        __m256i va2 = _mm256_loadu_si256((const __m256i *)(str_a + i + 64));
        __m256i vb2 = _mm256_loadu_si256((const __m256i *)(str_b + i + 64));
        __m256i va3 = _mm256_loadu_si256((const __m256i *)(str_a + i + 96));
        __m256i vb3 = _mm256_loadu_si256((const __m256i *)(str_b + i + 96));
        
        __m256i cmp0 = _mm256_cmpeq_epi8(va0, vb0);
        __m256i cmp1 = _mm256_cmpeq_epi8(va1, vb1);
        __m256i cmp2 = _mm256_cmpeq_epi8(va2, vb2);
        __m256i cmp3 = _mm256_cmpeq_epi8(va3, vb3);
        
        int mask0 = _mm256_movemask_epi8(cmp0);
        int mask1 = _mm256_movemask_epi8(cmp1);
        int mask2 = _mm256_movemask_epi8(cmp2);
        int mask3 = _mm256_movemask_epi8(cmp3);
        
        /* Fast path: all equal */
        if ((mask0 & mask1 & mask2 & mask3) == -1) {
            i += 128;
            continue;
        }
        
        /* Find which block has the difference */
        if (mask0 != -1) return i + __builtin_ctz(~mask0);
        if (mask1 != -1) return i + 32 + __builtin_ctz(~mask1);
        if (mask2 != -1) return i + 64 + __builtin_ctz(~mask2);
        return i + 96 + __builtin_ctz(~mask3);
    }
    
    /* AVX2 path: compare 32 bytes at a time */
    while (i + 32 <= len) {
        __m256i va = _mm256_loadu_si256((const __m256i *)(str_a + i));
        __m256i vb = _mm256_loadu_si256((const __m256i *)(str_b + i));
        
        __m256i cmp = _mm256_cmpeq_epi8(va, vb);
        int mask = _mm256_movemask_epi8(cmp);
        
        if (mask != -1) {
            return i + __builtin_ctz(~mask);
        }
        i += 32;
    }
    
    /* Handle remaining 16-31 bytes with SSE */
    if (i + 16 <= len) {
        __m128i va = _mm_loadu_si128((const __m128i *)(str_a + i));
        __m128i vb = _mm_loadu_si128((const __m128i *)(str_b + i));
        
        __m128i cmp = _mm_cmpeq_epi8(va, vb);
        int mask = _mm_movemask_epi8(cmp);
        
        if (mask != 0xFFFF) {
            return i + __builtin_ctz(~mask & 0xFFFF);
        }
        i += 16;
    }
    
    /* Handle remaining 8-15 bytes */
    if (i + 8 <= len) {
        uint64_t va, vb;
        memcpy(&va, str_a + i, 8);
        memcpy(&vb, str_b + i, 8);
        if (va != vb) {
            /* Find first differing byte */
            uint64_t xor_val = va ^ vb;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            return i + (__builtin_ctzll(xor_val) >> 3);
#else
            return i + (__builtin_clzll(xor_val) >> 3);
#endif
        }
        i += 8;
    }
    
    return i;
}
#endif

/* Fast comparison for very short strings (1-16 bytes)
 * Uses optimized paths based on length */
static inline int compareShortStrings(const char *str_a, const char *str_b, size_t len) {
    /* Handle 8-16 bytes with overlapping 8-byte loads */
    if (len > 8) {
        /* Load first 8 bytes */
        uint64_t head_a, head_b;
        memcpy(&head_a, str_a, 8);
        memcpy(&head_b, str_b, 8);
        
        if (head_a != head_b) {
            uint64_t xor_val = head_a ^ head_b;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            int byte_pos = __builtin_ctzll(xor_val) >> 3;
#else
            int byte_pos = __builtin_clzll(xor_val) >> 3;
#endif
            return (unsigned char)str_a[byte_pos] - (unsigned char)str_b[byte_pos];
        }
        
        /* Load last 8 bytes (overlapping) */
        uint64_t tail_a, tail_b;
        memcpy(&tail_a, str_a + len - 8, 8);
        memcpy(&tail_b, str_b + len - 8, 8);
        
        if (tail_a != tail_b) {
            uint64_t xor_val = tail_a ^ tail_b;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            int byte_pos = __builtin_ctzll(xor_val) >> 3;
#else
            int byte_pos = __builtin_clzll(xor_val) >> 3;
#endif
            return (unsigned char)str_a[len - 8 + byte_pos] - (unsigned char)str_b[len - 8 + byte_pos];
        }
        return 0;
    }
    
    /* Handle 5-8 bytes */
    if (len >= 5) {
        /* Load as 4 bytes + overlapping 4 bytes */
        uint32_t head_a, head_b;
        memcpy(&head_a, str_a, 4);
        memcpy(&head_b, str_b, 4);
        
        if (head_a != head_b) {
            uint32_t xor_val = head_a ^ head_b;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            int byte_pos = __builtin_ctz(xor_val) >> 3;
#else
            int byte_pos = __builtin_clz(xor_val) >> 3;
#endif
            return (unsigned char)str_a[byte_pos] - (unsigned char)str_b[byte_pos];
        }
        
        uint32_t tail_a, tail_b;
        memcpy(&tail_a, str_a + len - 4, 4);
        memcpy(&tail_b, str_b + len - 4, 4);
        
        if (tail_a != tail_b) {
            uint32_t xor_val = tail_a ^ tail_b;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            int byte_pos = __builtin_ctz(xor_val) >> 3;
#else
            int byte_pos = __builtin_clz(xor_val) >> 3;
#endif
            return (unsigned char)str_a[len - 4 + byte_pos] - (unsigned char)str_b[len - 4 + byte_pos];
        }
        return 0;
    }
    
    /* Handle 1-4 bytes - simple byte-by-byte is fastest */
    for (size_t i = 0; i < len; i++) {
        if (str_a[i] != str_b[i]) {
            return (unsigned char)str_a[i] - (unsigned char)str_b[i];
        }
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * idmpEntry Management Functions
 * ----------------------------------------------------------------------- */

/* SIMD-optimized comparison function for idmpEntry structures in AVL tree.
 * Compares entries by IID only (lexicographically).
 * Returns: negative if a < b, 0 if a == b, positive if a > b */
int idmpEntryCompare(const void *a, const void *b) {
    const idmpEntry *ea = (const idmpEntry *)a;
    const idmpEntry *eb = (const idmpEntry *)b;
    
    size_t len_a = ea->iid_len;
    size_t len_b = eb->iid_len;
    size_t min_len = len_a < len_b ? len_a : len_b;
    
    const char *str_a = ea->iid;
    const char *str_b = eb->iid;
    
    /* Fast path for very short strings (<= 16 bytes) */
    if (min_len <= 16) {
        int cmp = compareShortStrings(str_a, str_b, min_len);
        if (cmp != 0) return cmp;
        /* All common bytes equal, return length comparison */
        return (len_a > len_b) - (len_a < len_b);
    }
    
    size_t i = 0;
    
    /* Use SIMD for longer strings */
#ifdef HAVE_AVX512
    if (BITOP_USE_AVX512) {
        i = compareStringsAVX512(str_a, str_b, min_len);
    } else
#endif
#ifdef HAVE_AVX2
    if (BITOP_USE_AVX2) {
        i = compareStringsAVX2(str_a, str_b, min_len);
    }
#endif
    
    /* Handle any remaining bytes with scalar comparison */
    while (i < min_len) {
        if (str_a[i] != str_b[i]) {
            return (unsigned char)str_a[i] - (unsigned char)str_b[i];
        }
        i++;
    }
    
    /* All common bytes equal, compare lengths */
    return (len_a > len_b) - (len_a < len_b);
}

/* Create a new idmpEntry with the given IID and stream ID.
 * The IID string is copied into the entry.
 * Returns NULL on allocation failure. */
idmpEntry *idmpEntryCreate(const char *iid, size_t iid_len, size_t *alloc_size) {
    size_t usable;
    idmpEntry *entry = zmalloc_usable(sizeof(idmpEntry), &usable);
    if (entry == NULL) return NULL;
    
    size_t iid_usable;
    entry->iid = zmalloc_usable(iid_len, &iid_usable);
    if (entry->iid == NULL) {
        zfree(entry);
        return NULL;
    }
    
    memcpy(entry->iid, iid, iid_len);
    entry->iid_len = iid_len;
    
    if (alloc_size) {
        *alloc_size += usable + iid_usable;
    }
    
    return entry;
}

/* Free an idmpEntry and its IID string. */
void idmpEntryFree(idmpEntry *entry, size_t *alloc_size) {
    if (entry == NULL) return;
    
    if (alloc_size) {
        if (entry->iid != NULL) {
            *alloc_size -= zmalloc_size(entry->iid);
        }
        *alloc_size -= zmalloc_size(entry);
    }
    
    if (entry->iid != NULL) zfree(entry->iid);
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
            uint64_t expire_time = server.mstime - s->idmp_duration;
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
 *   1 on success
 *   0 on error (memory allocation failure or hash function error) */
int createIdempotencyHash(robj **argv, int64_t numfields, char *result) {
    XXH128_hash_t hash_result = {0, 0};
    XXH3_state_t* state = XXH3_createState();
    if (state == NULL) return 0;
    
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
            return 0;
        }
        
        /* Hash the field */
        long field_len;
        unsigned char *field_data = getObjectReadOnlyString(field, &field_len, llbuf);
        err = XXH3_128bits_update(state, field_data, field_len);
        if (err != XXH_OK) {
            XXH3_freeState(state);
            return 0;
        }
        
        /* Hash the value */
        long value_len;
        unsigned char *value_data = getObjectReadOnlyString(value, &value_len, llbuf);
        err = XXH3_128bits_update(state, value_data, value_len);
        if (err != XXH_OK) {
            XXH3_freeState(state);
            return 0;
        }
        
        /* Get the hash for this pair */
        XXH128_hash_t pair_hash = XXH3_128bits_digest(state);
        
        /* XOR with accumulated result */
        hash_result.low64 ^= pair_hash.low64;
        hash_result.high64 ^= pair_hash.high64;
    }
    
    XXH3_freeState(state);
    
    /* Write raw 128-bit hash to result buffer (16 bytes) */
    memcpy(result, &hash_result, 16);
    
    return 1;
}


/* IDMP (Idempotent Message Producer) utilities for Redis Streams
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef __STREAMIDMP_H
#define __STREAMIDMP_H

#include "server.h"
#include "stream.h"
#include "xxhash.h"
#include <stddef.h>

/* Comparison function for idmpEntry structures.
 * Used as the comparison callback for tring (AVL tree with ring buffer).
 * Compares entries by IID (XXH128_hash_t) only.
 * Returns: negative if a < b, 0 if a == b, positive if a > b */
int idmpEntryCompare(const void *a, const void *b);

/* Create a new idmpEntry with the given IID hash.
 * Returns NULL on allocation failure. */
idmpEntry *idmpEntryCreate(XXH128_hash_t iid, size_t *alloc_size);

/* Free an idmpEntry. */
void idmpEntryFree(idmpEntry *entry, size_t *alloc_size);

/* Register a stream key for IDMP entry tracking.
 * This registers a stream key in the database's stream_idmp_keys dictionary,
 * allowing the cron job handleExpiredIdmpEntries() to periodically check
 * and clean up expired idempotency entries from the stream's idmp_tring. */
void trackStreamIdmpEntries(client *c, robj *key);

/* Clean up expired idempotency entries from tracked streams.
 * This function is invoked regularly from blockedBeforeSleep() to remove 
 * expired entries from the idmp_tring of streams that have idempotency 
 * tracking enabled, keeping memory usage under control. */
void handleExpiredIdmpEntries(void);

/* Hash field-value pairs using XXH3_128bits for AUTOIDMP.
 * Takes an array of robj pointers (field-value pairs) and the number of pairs.
 * Each field-value pair is hashed together, and all pair hashes are XORed.
 * 
 * Parameters:
 *   argv      - Array of robj pointers containing field-value pairs
 *   numfields - Number of field-value pairs (not the array length)
 * 
 * Returns: XXH128_hash_t containing the 128-bit hash result */
XXH128_hash_t createIdempotencyHash(robj **argv, int64_t numfields);

/* Hash a raw buffer using XXH3_128bits for AUTOIDMP.
 * Takes a raw character buffer and its length, and produces a 128-bit hash.
 * 
 * Parameters:
 *   data   - Pointer to the data buffer to hash
 *   len    - Length of the data buffer in bytes
 * 
 * Returns: XXH128_hash_t containing the 128-bit hash result */
XXH128_hash_t createIdempotencyHashFromBuffer(const char *data, size_t len);

#endif /* __STREAMIDMP_H */


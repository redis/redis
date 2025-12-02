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
#include <stddef.h>

/* SIMD-optimized comparison function for idmpEntry structures.
 * Used as the comparison callback for tring (AVL tree with ring buffer).
 * Compares entries by IID only (lexicographically).
 * Returns: negative if a < b, 0 if a == b, positive if a > b */
int idmpEntryCompare(const void *a, const void *b);

/* Create a new idmpEntry with the given IID and stream ID.
 * The IID string is copied into the entry.
 * Returns NULL on allocation failure. */
idmpEntry *idmpEntryCreate(const char *iid, size_t iid_len, size_t *alloc_size);

/* Free an idmpEntry and its IID string. */
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
 * The raw 128-bit hash (16 bytes) is written to the provided result buffer.
 * 
 * Parameters:
 *   argv      - Array of robj pointers containing field-value pairs
 *   numfields - Number of field-value pairs (not the array length)
 *   result    - Buffer to store the raw 16-byte hash (must be at least 16 bytes)
 * 
 * Returns: 1 on success, 0 on error */
int createIdempotencyHash(robj **argv, int64_t numfields, char *result);

#endif /* __STREAMIDMP_H */


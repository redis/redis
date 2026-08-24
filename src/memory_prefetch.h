/*
 * Copyright (c) 2025-Present, Redis Ltd.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
 */

#ifndef MEMORY_PREFETCH_H
#define MEMORY_PREFETCH_H

#include <stddef.h>

struct client;
struct dict;

/* Cross-command batch prefetching */
void prefetchCommandsBatchInit(void);
int determinePrefetchCount(int len);
int addCommandToBatch(struct client *c);
void resetCommandsBatch(void);
void prefetchCommands(void);

/* Intra-command prefetch: prefetch dict lookup data for an array of keys.
 * Reuses the same state machine as the cross-command path. The dict's
 * dictType drives any key/value payload prefetching via the
 * prefetchEntryKey / prefetchEntryValue callbacks.
 *
 * nkeys must be <= DICT_PREFETCH_MAX_SIZE (the function asserts this).
 * Callers should batch larger inputs into chunks of this size or smaller. */
#define DICT_PREFETCH_MAX_SIZE 64
void dictPrefetchKeys(struct dict **dicts, void **keys, size_t nkeys);

/* Batch size for intra-command key prefetching: the number of keys a
 * multi-key command warms per dictPrefetchKeys() call. This is a tuning
 * constant, deliberately distinct from DICT_PREFETCH_MAX_SIZE above, which
 * is the hard array-size ceiling dictPrefetchKeys() asserts against. Callers
 * pick a batch size (this), the primitive enforces the ceiling (that). */
#define PREFETCH_BATCH_SIZE 16

/* Adaptive prefetch batch sizing. Take a full batch of 'batch_size' items
 * only if at least two full batches remain in 'remaining_items'; otherwise
 * take everything remaining in one call.
 *
 * This keeps the trailing partial batch of a large request from degenerating
 * to a size too small for dictPrefetchKeys() to help: the primitive
 * early-returns at nkeys <= 1, so a naive fixed-size chunk loop gives zero
 * prefetch benefit to the last item of any request whose item count is
 * 1 (mod batch_size). Folding the remainder into the previous batch instead
 * means the returned size is either exactly 'batch_size', or everything that
 * is left (which is < batch_size*2).
 *
 * Callers sizing stack arrays from the result must therefore allow for
 * batch_size*2 - 1 items; the established idiom is to declare them as
 * [PREFETCH_BATCH_SIZE*2].
 *
 * This arithmetic lives here, in one place, so both the string (MGET/MSET)
 * and sorted-set (ZMSCORE) call sites — and any future one — cannot drift
 * apart. */
static inline int prefetchNextBatchSize(int remaining_items, int batch_size) {
    int batch = remaining_items;
    if (batch >= batch_size * 2) batch = batch_size;
    return batch;
}

#endif /* MEMORY_PREFETCH_H */

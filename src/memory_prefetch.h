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

#endif /* MEMORY_PREFETCH_H */

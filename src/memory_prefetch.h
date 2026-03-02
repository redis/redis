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

/* Callback returning a pointer to prefetch for a kv object's value data,
 * or NULL if nothing needs prefetching. */
typedef void *(*PrefetchGetValueDataFunc)(const void *val);

/* Cross-command batch prefetching (I/O-thread path) */
void prefetchCommandsBatchInit(void);
int determinePrefetchCount(int len);
int addCommandToBatch(struct client *c);
void resetCommandsBatch(void);
void prefetchCommands(void);

/* Intra-command prefetch: prefetch dict lookup data for an array of keys.
 * Reuses the same state machine as the cross-command path.
 * Callers should keep nkeys bounded (e.g. <= 16-32) per call. */
void dictPrefetchKeys(struct dict **dicts, void **keys, size_t nkeys,
                      PrefetchGetValueDataFunc get_val_data);

/* Default value-data callback for string kv objects (OBJ_STRING / OBJ_ENCODING_RAW). */
void *prefetchGetObjectValuePtr(const void *val);

#endif /* MEMORY_PREFETCH_H */

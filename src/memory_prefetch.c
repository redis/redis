/*
 * This file utilizes prefetching keys and data for multiple commands in a batch,
 * to improve performance by amortizing memory access costs across multiple operations.
 *
 * Copyright (c) 2025-Present, Redis Ltd. and contributors.
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

#include "memory_prefetch.h"
#include "server.h"
#include "dict.h"

typedef enum { HT_IDX_FIRST = 0, HT_IDX_SECOND = 1, HT_IDX_INVALID = -1 } HashTableIndex;

typedef enum {
    PREFETCH_BUCKET,     /* Initial state, determines which hash table to use and prefetch the table's bucket */
    PREFETCH_ENTRY,      /* prefetch entries associated with the given key's hash */
    PREFETCH_KVOBJ,      /* prefetch the kv object of the entry found in the previous step */
    PREFETCH_VALDATA,    /* prefetch the value data of the kv object found in the previous step */
    PREFETCH_DONE        /* Indicates that prefetching for this key is complete */
} PrefetchState;


/************************************ State machine diagram for the prefetch operation. ********************************
                                                           │
                                                         start
                                                           │
                                                  ┌────────▼─────────┐
                                       ┌─────────►│  PREFETCH_BUCKET ├────►────────┐
                                       │          └────────┬─────────┘            no more tables -> done
                                       |             bucket|found                  |
                                       │                   |                       │
        entry not found - goto next table         ┌────────▼────────┐              │
                                       └────◄─────┤ PREFETCH_ENTRY  |              ▼
                                    ┌────────────►└────────┬────────┘              │
                                    |                 Entry│found                  │
                                    │                      |                       │
                                    |              ┌───────▼────────┐              │
                                    │              | PREFETCH_KVOBJ |              ▼
                                    │              └───────┬────────┘              │
        kvobj not found - goto next entry                  |                       |
                                    │          ┌───────────▼────────────┐          │
                                    └──────◄───│    PREFETCH_VALDATA    │          ▼
                                               └───────────┬────────────┘          │
                                                           |                       │
                                                 ┌───────-─▼─────────────┐         │
                                                 │     PREFETCH_DONE     │◄────────┘
                                                 └───────────────────────┘
**********************************************************************************************************************/

typedef struct KeyPrefetchInfo {
    PrefetchState state;      /* Current state of the prefetch operation */
    HashTableIndex ht_idx;    /* Index of the current hash table (0 or 1 for rehashing) */
    uint64_t bucket_idx;      /* Index of the bucket in the current hash table */
    uint64_t key_hash;        /* Hash value of the key being prefetched */
    dictEntry *current_entry; /* Pointer to the current entry being processed */
    kvobj *current_kv;        /* Pointer to the kv object being prefetched */
} KeyPrefetchInfo;

/* DictPrefetchCtx holds the minimal state needed to run the dict prefetch
 * state machine.  It is used by both the cross-command batch path and the
 * new intra-command dictPrefetchKeys() API. */
typedef struct DictPrefetchCtx {
    size_t cur_idx;                        /* Round-robin index into key arrays */
    size_t key_count;                      /* Number of keys being prefetched */
    size_t remaining;                      /* Number of keys not yet PREFETCH_DONE */
    void **keys;                           /* Array of key pointers (sds) */
    dict **dicts;                          /* Per-key dictionary pointers */
    KeyPrefetchInfo *info;                 /* Per-key prefetch state */
    PrefetchGetValueDataFunc get_val_data; /* Optional value-data callback */
} DictPrefetchCtx;

/* PrefetchCommandsBatch structure holds the state of the current batch of client commands being processed. */
typedef struct PrefetchCommandsBatch {
    size_t cur_idx;                 /* Index of the current key being processed */
    size_t key_count;               /* Number of keys in the current batch */
    size_t client_count;            /* Number of clients in the current batch */
    size_t pcmd_count;              /* Number of pending commands in the current batch */
    size_t max_prefetch_size;       /* Maximum number of keys to prefetch in a batch */
    void **keys;                    /* Array of keys to prefetch in the current batch */
    client **clients;               /* Array of clients in the current batch */
    pendingCommand **pending_cmds;  /* Array of pending commands in the current batch */
    dict **keys_dicts;              /* Main dict for each key */
    KeyPrefetchInfo *prefetch_info; /* Prefetch info for each key */
} PrefetchCommandsBatch;

static PrefetchCommandsBatch *batch = NULL;

void freePrefetchCommandsBatch(void) {
    if (batch == NULL) {
        return;
    }

    zfree(batch->clients);
    zfree(batch->pending_cmds);
    zfree(batch->keys);
    zfree(batch->keys_dicts);
    zfree(batch->prefetch_info);
    zfree(batch);
    batch = NULL;
}

void prefetchCommandsBatchInit(void) {
    serverAssert(!batch);

    /* To avoid prefetching small batches, we set the max size to twice
     * the configured size, so if not exceeding twice the limit, we can
     * prefetch all of it. See also `determinePrefetchCount` */
    size_t max_prefetch_size = server.prefetch_batch_max_size * 2;

    if (max_prefetch_size == 0) {
        return;
    }

    batch = zcalloc(sizeof(PrefetchCommandsBatch));
    batch->max_prefetch_size = max_prefetch_size;
    batch->clients = zcalloc(max_prefetch_size * sizeof(client *));
    batch->pending_cmds = zcalloc(max_prefetch_size * sizeof(pendingCommand *));
    batch->keys = zcalloc(max_prefetch_size * sizeof(void *));
    batch->keys_dicts = zcalloc(max_prefetch_size * sizeof(dict *));
    batch->prefetch_info = zcalloc(max_prefetch_size * sizeof(KeyPrefetchInfo));
}

void onMaxBatchSizeChange(void) {
    if (batch && batch->client_count > 0) {
        /* We need to process the current batch before updating the size */
        return;
    }

    freePrefetchCommandsBatch();
    prefetchCommandsBatchInit();
}

/* ---- State machine helpers operating on DictPrefetchCtx ---- */

/* Prefetch the given pointer and advance to the next key (round-robin). */
static inline void ctxPrefetchAndAdvance(DictPrefetchCtx *ctx, void *addr) {
    redis_prefetch_read(addr);
    if (++ctx->cur_idx >= ctx->key_count) ctx->cur_idx = 0;
}

static inline void ctxMarkDone(DictPrefetchCtx *ctx, KeyPrefetchInfo *info) {
    info->state = PREFETCH_DONE;
    ctx->remaining--;
    server.stat_total_prefetch_entries++;
}

/* Return the next KeyPrefetchInfo that still needs work, or NULL if all done. */
static inline KeyPrefetchInfo *ctxNextInfo(DictPrefetchCtx *ctx) {
    if (ctx->remaining == 0) return NULL;
    while (ctx->info[ctx->cur_idx].state == PREFETCH_DONE) {
        if (++ctx->cur_idx >= ctx->key_count) ctx->cur_idx = 0;
    }
    return &ctx->info[ctx->cur_idx];
}

static void ctxInit(DictPrefetchCtx *ctx) {
    size_t remaining = 0;
    for (size_t i = 0; i < ctx->key_count; i++) {
        KeyPrefetchInfo *info = &ctx->info[i];
        if (!ctx->dicts[i] || dictSize(ctx->dicts[i]) == 0) {
            info->state = PREFETCH_DONE;
            continue;
        }

        /* We skip prefetch during loading, so ht_table[0] should never be NULL
         * when dictSize() > 0 (which only happens mid-dictEmpty via _dictReset). */
        serverAssert(ctx->dicts[i]->ht_table[0]);

        info->ht_idx = HT_IDX_INVALID;
        info->current_entry = NULL;
        info->current_kv = NULL;
        info->state = PREFETCH_BUCKET;
        info->key_hash = dictGetHash(ctx->dicts[i], ctx->keys[i]);
        remaining++;
    }
    ctx->remaining = remaining;
}

/* Prefetch the bucket of the next hash table index.
 * If no tables are left, move to the PREFETCH_DONE state. */
static void ctxPrefetchBucket(DictPrefetchCtx *ctx, KeyPrefetchInfo *info) {
    size_t i = ctx->cur_idx;

    /* Determine which hash table to use */
    if (info->ht_idx == HT_IDX_INVALID) {
        info->ht_idx = HT_IDX_FIRST;
    } else if (info->ht_idx == HT_IDX_FIRST && dictIsRehashing(ctx->dicts[i])) {
        info->ht_idx = HT_IDX_SECOND;
    } else {
        /* No more tables left - mark as done. */
        ctxMarkDone(ctx, info);
        return;
    }

    /* Prefetch the bucket */
    info->bucket_idx = info->key_hash & DICTHT_SIZE_MASK(ctx->dicts[i]->ht_size_exp[info->ht_idx]);
    ctxPrefetchAndAdvance(ctx, &ctx->dicts[i]->ht_table[info->ht_idx][info->bucket_idx]);
    info->current_entry = NULL;
    info->state = PREFETCH_ENTRY;
}

/* Prefetch the entry in the bucket and move to the PREFETCH_KVOBJ state.
 * If no more entries in the bucket, move to the PREFETCH_BUCKET state to look at the next table. */
static void ctxPrefetchEntry(DictPrefetchCtx *ctx, KeyPrefetchInfo *info) {
    size_t i = ctx->cur_idx;

    if (info->current_entry) {
        /* We already found an entry in the bucket - move to the next entry */
        info->current_entry = dictGetNext(info->current_entry);
    } else {
        /* Go to the first entry in the bucket */
        info->current_entry = ctx->dicts[i]->ht_table[info->ht_idx][info->bucket_idx];
    }

    if (info->current_entry) {
        ctxPrefetchAndAdvance(ctx, info->current_entry);
        info->current_kv = NULL;
        info->state = PREFETCH_KVOBJ;
    } else {
        /* No entry found in the bucket - try the bucket in the next table */
        info->state = PREFETCH_BUCKET;
    }
}

/* Prefetch the kv object in the dict entry, and move to the PREFETCH_VALDATA state. */
static inline void ctxPrefetchKvobj(DictPrefetchCtx *ctx, KeyPrefetchInfo *info) {
    kvobj *kv = dictGetKey(info->current_entry);
    int is_kv = dictEntryIsKey(info->current_entry);

    info->current_kv = kv;
    info->state = PREFETCH_VALDATA;
    /* If the entry is a pointer of kv object, we don't need to prefetch it */
    if (!is_kv) ctxPrefetchAndAdvance(ctx, kv);
}

/* Check if the current kv object matches the key we're looking for.
 * If a value-data callback is set, prefetch the value data.
 * Otherwise just mark done. */
static inline void ctxPrefetchValdata(DictPrefetchCtx *ctx, KeyPrefetchInfo *info) {
    size_t i = ctx->cur_idx;
    kvobj *kv = info->current_kv;
    sds key = kvobjGetKey(kv);

    /* 1. If this is the last element, we assume a hit and don't compare the keys
     * 2. This kv object is the target of the lookup. */
    if ((!dictGetNext(info->current_entry) && !dictIsRehashing(ctx->dicts[i])) ||
        dictCompareKeys(ctx->dicts[i], ctx->keys[i], key))
    {
        if (ctx->get_val_data) {
            void *value_data = ctx->get_val_data(kv);
            if (value_data) ctxPrefetchAndAdvance(ctx, value_data);
        }
        ctxMarkDone(ctx, info);
    } else {
        /* Not found in the current entry, move to the next entry */
        info->state = PREFETCH_ENTRY;
    }
}

/* Prefetch dictionary data for an array of keys.
 *
 * This function takes an array of dictionaries and keys, attempting to bring
 * data closer to the L1 cache that might be needed for dictionary operations
 * on those keys.
 *
 * The dictFind algorithm:
 * 1. Evaluate the hash of the key
 * 2. Access the index in the first table
 * 3. Walk the entries linked list until the key is found
 *    If the key hasn't been found and the dictionary is in the middle of rehashing,
 *    access the index on the second table and repeat step 3
 *
 * dictPrefetch executes the same algorithm as dictFind, but one step at a time
 * for each key. Instead of waiting for data to be read from memory, it prefetches
 * the data and then moves on to execute the next prefetch for another key.
 *
 * dicts - An array of dictionaries to prefetch data from.
 * get_val_data_func - A callback function that dictPrefetch can invoke
 * to bring the key's value data closer to the L1 cache as well.
 */
/* Run the prefetch state machine over all keys in the context until every
 * key reaches PREFETCH_DONE. */
static void dictPrefetchRun(DictPrefetchCtx *ctx) {
    KeyPrefetchInfo *info;
    while ((info = ctxNextInfo(ctx))) {
        switch (info->state) {
        case PREFETCH_BUCKET:  ctxPrefetchBucket(ctx, info);  break;
        case PREFETCH_ENTRY:   ctxPrefetchEntry(ctx, info);   break;
        case PREFETCH_KVOBJ:   ctxPrefetchKvobj(ctx, info);   break;
        case PREFETCH_VALDATA: ctxPrefetchValdata(ctx, info); break;
        default: serverPanic("Unknown prefetch state %d", info->state);
        }
    }
}

/* Wrapper used by the cross-command batch path (prefetchCommands). */
static void dictPrefetch(dict **dicts, PrefetchGetValueDataFunc get_val_data_func) {
    DictPrefetchCtx ctx = {
        .cur_idx      = batch->cur_idx,
        .key_count    = batch->key_count,
        .keys         = batch->keys,
        .dicts        = dicts,
        .info         = batch->prefetch_info,
        .get_val_data = get_val_data_func,
    };
    ctxInit(&ctx);
    dictPrefetchRun(&ctx);
    /* Write back cur_idx so the global batch stays in sync. */
    batch->cur_idx = ctx.cur_idx;
}

/* Helper function to get the value pointer of a kv object. */
void *prefetchGetObjectValuePtr(const void *value) {
    kvobj *kv = (kvobj *)value;
    return (kv->type == OBJ_STRING && kv->encoding == OBJ_ENCODING_RAW) ? kv->ptr : NULL;
}

void resetCommandsBatch(void) {
    if (batch == NULL) {
        /* Handle the case where prefetching becomes enabled from disabled. */
        if (server.prefetch_batch_max_size) prefetchCommandsBatchInit();
        return;
    }

    batch->cur_idx = 0;
    batch->key_count = 0;
    batch->client_count = 0;
    batch->pcmd_count = 0;

    /* Handle the case where the max prefetch size has been changed. */
    if (batch->max_prefetch_size != (size_t)server.prefetch_batch_max_size * 2) {
        onMaxBatchSizeChange();
    }
}

/* Prefetching in very small batches tends to be ineffective because the technique
 * relies on a small gap—typically a few CPU cycles—between issuing the prefetch
 * and performing the actual memory access. If the batch is too small, this delay
 * cannot be effectively inserted, and the prefetching yields little to no benefit.
 *
 * To avoid wasting effort, when the remaining data is small (less than twice the
 * maximum batch size), we simply prefetch all of it at once. Otherwise, we only
 * prefetch a limited portion, capped at the configured maximum. */
int determinePrefetchCount(int len) {
    if (!batch) return 0;

    /* The batch max size is double of the configured size. */
    int config_size = batch->max_prefetch_size / 2;
    return len < (int)batch->max_prefetch_size ? len : config_size;
}

/* Prefetch command-related data:
 * 1. Prefetch the command arguments allocated by the I/O thread to bring them
 *    closer to the L1 cache.
 * 2. Prefetch the io_deferred_objects for all clients.
 * 3. Prefetch the keys and values for all commands in the current batch from
 *    the main dictionaries. */
void prefetchCommands(void) {
    if (!batch || server.loading) return;

    /* Prefetch argv's for all pending commands */
    for (size_t i = 0; i < batch->pcmd_count; i++) {
        pendingCommand *pcmd = batch->pending_cmds[i];
        if (unlikely(pcmd->argc <= 0)) continue;
        for (int j = 0; j < pcmd->argc; j++) {
            redis_prefetch_read(pcmd->argv[j]);
        }
    }

    /* Prefetch the argv->ptr if required */
    for (size_t i = 0; i < batch->pcmd_count; i++) {
        pendingCommand *pcmd = batch->pending_cmds[i];
        if (unlikely(pcmd->argc <= 1)) continue;
        /* Skip the first argument (command name), as it's typically short */
        for (int j = 1; j < pcmd->argc; j++) {
            if (pcmd->argv[j]->encoding == OBJ_ENCODING_RAW) {
                redis_prefetch_read(pcmd->argv[j]->ptr);
            }
        }
    }

    /* Prefetch io_deferred_objects for all clients */
    for (size_t i = 0; i < batch->client_count; i++) {
        client *c = batch->clients[i];
        if (!c->io_deferred_objects || c->io_deferred_objects_num == 0) continue;
        for (int j = 0; j < c->io_deferred_objects_num; j++)
            redis_prefetch_read(c->io_deferred_objects[j]);
    }

    /* Get the keys ptrs - we do it here after the key obj was prefetched. */
    for (size_t i = 0; i < batch->key_count; i++) {
        batch->keys[i] = ((robj *)batch->keys[i])->ptr;
    }

    /* Prefetch dict keys for all commands.
     * Prefetching is beneficial only if there are more than one key. */
    if (batch->key_count > 1) {
        server.stat_total_prefetch_batches++;
        /* Prefetch keys from the main dict */
        dictPrefetch(batch->keys_dicts, prefetchGetObjectValuePtr);
    }
}

/* --------------------------------------------------------------------------
 * Intra-command prefetch API
 * --------------------------------------------------------------------------
 * dictPrefetchKeys() allows a single multi-key command (e.g. MGET) to
 * prefetch dict data for a batch of its own keys, reusing the same state
 * machine that the cross-command path uses.
 *
 * Typical usage from a command implementation:
 *
 *   #define BATCH 16
 *   void myMultiKeyCommand(client *c) {
 *       dict *d = kvstoreGetDict(c->db->keys, slot);
 *       for (int j = 0; j < numkeys; j += BATCH) {
 *           int n = MIN(BATCH, numkeys - j);
 *           void *keys[BATCH]; dict *dicts[BATCH];
 *           for (int k = 0; k < n; k++) {
 *               keys[k] = c->argv[j+k+1]->ptr;
 *               dicts[k] = d;
 *           }
 *           dictPrefetchKeys(dicts, keys, n, NULL);
 *           // Now process these n keys — dict bucket / entry / kvobj are in cache.
 *       }
 *   }
 *
 * The value-data callback (last argument) is typically NULL. For commands
 * like MGET the extra state the callback pushes through the prefetch pipeline
 * costs more than warming the value payload saves, because lookupKeyRead +
 * addReplyBulk reach the payload immediately afterwards and the hardware
 * prefetcher is already on it. Pass prefetchGetObjectValuePtr only when the
 * caller has measured a benefit from warming the value as well.
 * ----------------------------------------------------------------------- */
void dictPrefetchKeys(dict **dicts, void **keys, size_t nkeys,
                      PrefetchGetValueDataFunc get_val_data)
{
    if (nkeys <= 1) return;  /* Single-key prefetch has no benefit — nothing
                              * to interleave with.  Tail batches of 1 key in
                              * callers like mgetCommand simply skip prefetch
                              * and proceed with a direct lookup. */

    /* Stack-allocate per-key state — callers should keep nkeys bounded
     * (typically ≤ 16–32) to avoid excessive stack usage.
     * Zero-initialize so that keys marked PREFETCH_DONE by ctxInit
     * (null/empty dict) don't carry indeterminate field values. */
    KeyPrefetchInfo pf_info[nkeys];
    memset(pf_info, 0, sizeof(pf_info));

    DictPrefetchCtx ctx = {
        .cur_idx      = 0,
        .key_count    = nkeys,
        .keys         = keys,
        .dicts        = dicts,
        .info         = pf_info,
        .get_val_data = get_val_data,
    };

    server.stat_total_prefetch_batches++;
    ctxInit(&ctx);
    dictPrefetchRun(&ctx);
}

/* Adds the client's command to the current batch.
 *
 * Returns C_OK if the command was added successfully, C_ERR otherwise. */
int addCommandToBatch(client *c) {
    if (unlikely(!batch)) return C_ERR;

    /* If the batch is full, process it.
     * We also check the client count to handle cases where
     * no keys exist for the clients' commands. */
    if (batch->client_count == batch->max_prefetch_size ||
        batch->key_count == batch->max_prefetch_size ||
        batch->pcmd_count == batch->max_prefetch_size)
    {
        return C_ERR;
    }

    /* Avoid partial prefetching: if the batch already has commands and adding this
     * client's ready commands would likely exceed the batch size limit, reject
     * the entire client. This is a conservative estimate using command count as
     * a proxy for key count to ensure all keys from a client are either fully
     * prefetched together or not prefetched at all. */
    if (batch->pcmd_count > 0 &&
        (c->pending_cmds.ready_len + batch->key_count > batch->max_prefetch_size ||
         c->pending_cmds.ready_len + batch->pcmd_count > batch->max_prefetch_size))
    {
        return C_ERR;
    }

    batch->clients[batch->client_count++] = c;

    pendingCommand *pcmd = c->pending_cmds.head;
    while (pcmd != NULL && batch->pcmd_count < batch->max_prefetch_size) {
        if (pcmd->next) redis_prefetch_read(pcmd->next);

        /* Skip commands that have not been preprocessed, or have errors. */
        if ((pcmd->flags & PENDING_CMD_FLAG_INCOMPLETE) || !pcmd->cmd || pcmd->read_error) break;

        batch->pending_cmds[batch->pcmd_count++] = pcmd;

        serverAssert(pcmd->flags & PENDING_CMD_KEYS_RESULT_VALID);
        size_t keys_before = batch->key_count;
        for (int i = 0; i < pcmd->keys_result.numkeys && batch->key_count < batch->max_prefetch_size; i++) {
            batch->keys[batch->key_count] = pcmd->argv[pcmd->keys_result.keys[i].pos];
            batch->keys_dicts[batch->key_count] =
                kvstoreGetDict(c->db->keys, pcmd->slot > 0 ? pcmd->slot : 0);
            batch->key_count++;
        }
        /* Mark the command as prefetched only if ALL of its keys were
         * added to the batch.  If the batch ran out of space mid-command,
         * the remaining keys were not prefetched and the intra-command
         * path (e.g. dictPrefetchKeys in mgetCommand) must handle them. */
        if (batch->key_count - keys_before == (size_t)pcmd->keys_result.numkeys) {
            pcmd->flags |= PENDING_CMD_KEYS_PREFETCHED;
        }
        pcmd = pcmd->next;
    }

    return C_OK;
}

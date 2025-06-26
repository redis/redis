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

typedef void *(*GetValueDataFunc)(const void *val);

typedef struct KeyPrefetchInfo {
    PrefetchState state;      /* Current state of the prefetch operation */
    HashTableIndex ht_idx;    /* Index of the current hash table (0 or 1 for rehashing) */
    uint64_t bucket_idx;      /* Index of the bucket in the current hash table */
    uint64_t key_hash;        /* Hash value of the key being prefetched */
    dictEntry *current_entry; /* Pointer to the current entry being processed */
    kvobj *current_kv;        /* Pointer to the kv object being prefetched */
} KeyPrefetchInfo;

/* PrefetchCommandsBatch structure holds the state of the current batch of client commands being processed. */
typedef struct PrefetchCommandsBatch {
    size_t cur_idx;                 /* Index of the current key being processed */
    size_t key_count;               /* Number of keys in the current batch */
    size_t client_count;            /* Number of clients in the current batch */
    size_t max_prefetch_size;       /* Maximum number of keys to prefetch in a batch */
    void **keys;                    /* Array of keys to prefetch in the current batch */
    client **clients;               /* Array of clients in the current batch */
    dict **keys_dicts;              /* Main dict for each key */
    dict **current_dicts;           /* Points to dict to prefetch from */
    KeyPrefetchInfo *prefetch_info; /* Prefetch info for each key */
    GetValueDataFunc get_value_data_func; /* Function to get the value data */
} PrefetchCommandsBatch;

static PrefetchCommandsBatch *batch = NULL;

void freePrefetchCommandsBatch(void) {
    if (batch == NULL) {
        return;
    }

    zfree(batch->clients);
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

/* Prefetch the given pointer and move to the next key in the batch. */
static inline void prefetchAndMoveToNextKey(void *addr) {
    redis_prefetch_read(addr);
    /* While the prefetch is in progress, we can continue to the next key */
    batch->cur_idx = (batch->cur_idx + 1) % batch->key_count;
}

static inline void markKeyAsdone(KeyPrefetchInfo *info) {
    info->state = PREFETCH_DONE;
    server.stat_total_prefetch_entries++;
}

/* Returns the next KeyPrefetchInfo structure that needs to be processed. */
static KeyPrefetchInfo *getNextPrefetchInfo(void) {
    size_t start_idx = batch->cur_idx;
    do {
        KeyPrefetchInfo *info = &batch->prefetch_info[batch->cur_idx];
        if (info->state != PREFETCH_DONE) return info;
        batch->cur_idx = (batch->cur_idx + 1) % batch->key_count;
    } while (batch->cur_idx != start_idx);
    return NULL;
}

static void initBatchInfo(dict **dicts, GetValueDataFunc func) {
    batch->current_dicts = dicts;
    batch->get_value_data_func = func;

    /* Initialize the prefetch info */
    for (size_t i = 0; i < batch->key_count; i++) {
        KeyPrefetchInfo *info = &batch->prefetch_info[i];
        if (!batch->current_dicts[i] || dictSize(batch->current_dicts[i]) == 0) {
            info->state = PREFETCH_DONE;
            continue;
        }
        info->ht_idx = HT_IDX_INVALID;
        info->current_entry = NULL;
        info->current_kv = NULL;
        info->state = PREFETCH_BUCKET;
        info->key_hash = dictGetHash(batch->current_dicts[i], batch->keys[i]);
    }
}

/* Prefetch the bucket of the next hash table index.
 * If no tables are left, move to the PREFETCH_DONE state. */
static void prefetchBucket(KeyPrefetchInfo *info) {
    size_t i = batch->cur_idx;

    /* Determine which hash table to use */
    if (info->ht_idx == HT_IDX_INVALID) {
        info->ht_idx = HT_IDX_FIRST;
    } else if (info->ht_idx == HT_IDX_FIRST && dictIsRehashing(batch->current_dicts[i])) {
        info->ht_idx = HT_IDX_SECOND;
    } else {
        /* No more tables left - mark as done. */
        markKeyAsdone(info);
        return;
    }

    /* Prefetch the bucket */
    info->bucket_idx = info->key_hash & DICTHT_SIZE_MASK(batch->current_dicts[i]->ht_size_exp[info->ht_idx]);
    prefetchAndMoveToNextKey(&batch->current_dicts[i]->ht_table[info->ht_idx][info->bucket_idx]);
    info->current_entry = NULL;
    info->state = PREFETCH_ENTRY;
}

/* Prefetch the entry in the bucket and move to the PREFETCH_KVOBJ state.
 * If no more entries in the bucket, move to the PREFETCH_BUCKET state to look at the next table. */
static void prefetchEntry(KeyPrefetchInfo *info) {
    size_t i = batch->cur_idx;

    if (info->current_entry) {
        /* We already found an entry in the bucket - move to the next entry */
        info->current_entry = dictGetNext(info->current_entry);
    } else {
        /* Go to the first entry in the bucket */
        info->current_entry = batch->current_dicts[i]->ht_table[info->ht_idx][info->bucket_idx];
    }

    if (info->current_entry) {
        prefetchAndMoveToNextKey(info->current_entry);
        info->current_kv = NULL;
        info->state = PREFETCH_KVOBJ;
    } else {
        /* No entry found in the bucket - try the bucket in the next table */
        info->state = PREFETCH_BUCKET;
    }
}

/* Prefetch the kv object in the dict entry, and to the PREFETCH_VALDATA state. */
static inline void prefetchKVOject(KeyPrefetchInfo *info) {
    kvobj *kv = dictGetKey(info->current_entry);
    int is_kv = dictEntryIsKey(info->current_entry);

    info->current_kv = kv;
    info->state = PREFETCH_VALDATA;
    /* If the entry is a pointer of kv object, we don't need to prefetch it */
    if (!is_kv) prefetchAndMoveToNextKey(kv);
}

/* Prefetch the value data of the kv object found in dict entry. */
static void prefetchValueData(KeyPrefetchInfo *info) {
    size_t i = batch->cur_idx;
    kvobj *kv = info->current_kv;

    /* 1. If this is the last element, we assume a hit and don't compare the keys
     * 2. This kv object is the target of the lookup. */
    if ((!dictGetNext(info->current_entry) && !dictIsRehashing(batch->current_dicts[i])) ||
        dictCompareKeys(batch->current_dicts[i], batch->keys[i], kv))
    {
        if (batch->get_value_data_func) {
            void *value_data = batch->get_value_data_func(kv);
            if (value_data) prefetchAndMoveToNextKey(value_data);
        }
        markKeyAsdone(info);
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
static void dictPrefetch(dict **dicts, GetValueDataFunc get_val_data_func) {
    initBatchInfo(dicts, get_val_data_func);
    KeyPrefetchInfo *info;
    while ((info = getNextPrefetchInfo())) {
        switch (info->state) {
        case PREFETCH_BUCKET: prefetchBucket(info); break;
        case PREFETCH_ENTRY: prefetchEntry(info); break;
        case PREFETCH_KVOBJ: prefetchKVOject(info); break;
        case PREFETCH_VALDATA: prefetchValueData(info); break;
        default: serverPanic("Unknown prefetch state %d", info->state);
        }
    }
}

/* Helper function to get the value pointer of a kv object. */
static void *getObjectValuePtr(const void *value) {
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
    return len < server.prefetch_batch_max_size ? len : config_size;
}

/* Prefetch command-related data:
 * 1. Prefetch the command arguments allocated by the I/O thread to bring them
 *    closer to the L1 cache.
 * 2. Prefetch the keys and values for all commands in the current batch from
 *    the main dictionaries. */
void prefetchCommands(void) {
    if (!batch) return;

    /* Prefetch argv's for all clients */
    for (size_t i = 0; i < batch->client_count; i++) {
        client *c = batch->clients[i];
        if (!c || c->argc <= 1) continue;
        /* Skip prefetching first argv (cmd name) it was already looked up by
         * the I/O thread, and the main thread will not touch argv[0]. */
        for (int j = 1; j < c->argc; j++) {
            redis_prefetch_read(c->argv[j]);
        }
    }

    /* Prefetch the argv->ptr if required */
    for (size_t i = 0; i < batch->client_count; i++) {
        client *c = batch->clients[i];
        if (!c || c->argc <= 1) continue;
        for (int j = 1; j < c->argc; j++) {
            if (c->argv[j]->encoding == OBJ_ENCODING_RAW) {
                redis_prefetch_read(c->argv[j]->ptr);
            }
        }
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
        dictPrefetch(batch->keys_dicts, getObjectValuePtr);
    }
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
        batch->key_count == batch->max_prefetch_size)
    {
        return C_ERR;
    }

    batch->clients[batch->client_count++] = c;

    if (likely(c->iolookedcmd)) {
        /* Get command's keys positions */
        getKeysResult result = GETKEYS_RESULT_INIT;
        int num_keys = getKeysFromCommand(c->iolookedcmd, c->argv, c->argc, &result);
        for (int i = 0; i < num_keys && batch->key_count < batch->max_prefetch_size; i++) {
            batch->keys[batch->key_count] = c->argv[result.keys[i].pos];
            batch->keys_dicts[batch->key_count] =
                kvstoreGetDict(c->db->keys, c->slot > 0 ? c->slot : 0);
            batch->key_count++;
        }
        getKeysFreeResult(&result);
    }

    return C_OK;
}

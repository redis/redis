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
    PREFETCH_VALUE,      /* prefetch the value object of the entry found in the previous step */
    PREFETCH_VALUE_DATA, /* prefetch the value object's data (if applicable) */
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
       value not found - goto next entry           ┌───────▼────────┐              |
                                    └───────◄──────┤ PREFETCH_VALUE |              ▼
                                                   └───────┬────────┘              │
                                                      Value│found                  │
                                                           |                       |
                                               ┌───────────▼──────────────┐        │
                                               │    PREFETCH_VALUE_DATA   │        ▼
                                               └───────────┬──────────────┘        │
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
} KeyPrefetchInfo;

/* PrefetchCommandsBatch structure holds the state of the current batch of client commands being processed. */
typedef struct PrefetchCommandsBatch {
    size_t cur_idx;                 /* Index of the current key being processed */
    size_t keys_done;               /* Number of keys that have been prefetched */
    size_t key_count;               /* Number of keys in the current batch */
    size_t client_count;            /* Number of clients in the current batch */
    size_t max_prefetch_size;       /* Maximum number of keys to prefetch in a batch */
    size_t executed_commands;       /* Number of commands executed in the current batch */
    int *slots;                     /* Array of slots for each key */
    void **keys;                    /* Array of keys to prefetch in the current batch */
    client **clients;               /* Array of clients in the current batch */
    dict **keys_dicts;              /* Main dict for each key */
    dict **expire_dicts;            /* Expire dict for each key */
    dict **current_dicts;           /* Points to either keys_dicts or expire_dicts */
    KeyPrefetchInfo *prefetch_info; /* Prefetch info for each key */
} PrefetchCommandsBatch;

static PrefetchCommandsBatch *batch = NULL;

void freePrefetchCommandsBatch(void) {
    if (batch == NULL) {
        return;
    }

    zfree(batch->clients);
    zfree(batch->keys);
    zfree(batch->keys_dicts);
    zfree(batch->expire_dicts);
    zfree(batch->slots);
    zfree(batch->prefetch_info);
    zfree(batch);
    batch = NULL;
}

void prefetchCommandsBatchInit(void) {
    serverAssert(!batch);

    /* To avoid prefetching small batches, we set the max size to twice
     * the configured size, so if not exceeding twice the limit, we can
     * prefetch all of it. */
    size_t max_prefetch_size = server.prefetch_batch_max_size * 2;

    if (max_prefetch_size == 0) {
        return;
    }

    batch = zcalloc(sizeof(PrefetchCommandsBatch));
    batch->max_prefetch_size = max_prefetch_size;
    batch->clients = zcalloc(max_prefetch_size * sizeof(client *));
    batch->keys = zcalloc(max_prefetch_size * sizeof(void *));
    batch->keys_dicts = zcalloc(max_prefetch_size * sizeof(dict *));
    batch->expire_dicts = zcalloc(max_prefetch_size * sizeof(dict *));
    batch->slots = zcalloc(max_prefetch_size * sizeof(int));
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
static void prefetchAndMoveToNextKey(void *addr) {
    redis_prefetch(addr);
    /* While the prefetch is in progress, we can continue to the next key */
    batch->cur_idx = (batch->cur_idx + 1) % batch->key_count;
}

static void markKeyAsdone(KeyPrefetchInfo *info) {
    info->state = PREFETCH_DONE;
    server.stat_total_prefetch_entries++;
    batch->keys_done++;
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

static void initBatchInfo(dict **dicts) {
    batch->current_dicts = dicts;

    /* Initialize the prefetch info */
    for (size_t i = 0; i < batch->key_count; i++) {
        KeyPrefetchInfo *info = &batch->prefetch_info[i];
        if (!batch->current_dicts[i] || dictSize(batch->current_dicts[i]) == 0) {
            info->state = PREFETCH_DONE;
            batch->keys_done++;
            continue;
        }
        info->ht_idx = HT_IDX_INVALID;
        info->current_entry = NULL;
        info->state = PREFETCH_BUCKET;
        info->key_hash = dictBucketHashKey(batch->current_dicts[i], batch->keys[i]);
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

/* Prefetch the next entry in the bucket and move to the PREFETCH_VALUE state.
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
        info->state = PREFETCH_VALUE;
    } else {
        /* No entry found in the bucket - try the bucket in the next table */
        info->state = PREFETCH_BUCKET;
    }
}

/* Prefetch the entry's value. If the value is found, move to the PREFETCH_VALUE_DATA state.
 * If the value is not found, move to the PREFETCH_ENTRY state to look at the next entry in the bucket. */
static void prefetchValue(KeyPrefetchInfo *info) {
    size_t i = batch->cur_idx;
    void *value = dictGetVal(info->current_entry);

    if (dictGetNext(info->current_entry) == NULL && !dictIsRehashing(batch->current_dicts[i])) {
        /* If this is the last element, we assume a hit and don't compare the keys */
        prefetchAndMoveToNextKey(value);
        info->state = PREFETCH_VALUE_DATA;
        return;
    }

    void *current_entry_key = dictGetKey(info->current_entry);
    if (batch->keys[i] == current_entry_key ||
        dictCompareKeys(batch->current_dicts[i], batch->keys[i], current_entry_key)) {
        /* If the key is found, prefetch the value */
        prefetchAndMoveToNextKey(value);
        info->state = PREFETCH_VALUE_DATA;
    } else {
        /* Move to the next entry */
        info->state = PREFETCH_ENTRY;
    }
}

/* Prefetch the value data if available. */
static void prefetchValueData(KeyPrefetchInfo *info, GetValueDataFunc get_val_data_func) {
    if (get_val_data_func) {
        void *value_data = get_val_data_func(dictGetVal(info->current_entry));
        if (value_data) prefetchAndMoveToNextKey(value_data);
    }
    markKeyAsdone(info);
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
    initBatchInfo(dicts);
    KeyPrefetchInfo *info;
    while ((info = getNextPrefetchInfo())) {
        switch (info->state) {
        case PREFETCH_BUCKET: prefetchBucket(info); break;
        case PREFETCH_ENTRY: prefetchEntry(info); break;
        case PREFETCH_VALUE: prefetchValue(info); break;
        case PREFETCH_VALUE_DATA: prefetchValueData(info, get_val_data_func); break;
        default: serverPanic("Unknown prefetch state %d", info->state);
        }
    }
}

/* Helper function to get the value pointer of an object. */
static void *getObjectValuePtr(const void *val) {
    robj *o = (robj *)val;
    return (o->type == OBJ_STRING && o->encoding == OBJ_ENCODING_RAW) ? o->ptr : NULL;
}

void resetCommandsBatch(void) {
    if (!batch) {
        /* Handle the case where prefetching becomes enabled from disabled. */
        if (server.prefetch_batch_max_size > 0)
            prefetchCommandsBatchInit();
        return;
    }

    batch->cur_idx = 0;
    batch->keys_done = 0;
    batch->key_count = 0;
    batch->client_count = 0;
    batch->executed_commands = 0;

    /* Handle the case where the max prefetch size has been changed. */
    if (batch->max_prefetch_size != (size_t)server.prefetch_batch_max_size * 2) {
        onMaxBatchSizeChange();
    }
}

int getConfigPrefetchBatchSize(void) {
    if (!batch) return 0;
    /* We double the size when initializing the batch, so divide it by 2. */
    return batch->max_prefetch_size / 2;
}

/* Prefetch command-related data:
 * 1. Prefetch the command arguments allocated by the I/O thread to bring them closer to the L1 cache.
 * 2. Prefetch the keys and values for all commands in the current batch from the main and expires dictionaries. */
void prefetchCommands(void) {
    if (!batch) return;

    /* Prefetch argv's for all clients */
    for (size_t i = 0; i < batch->client_count; i++) {
        client *c = batch->clients[i];
        if (!c || c->argc <= 1) continue;
        /* Skip prefetching first argv (cmd name) it was already looked up by the I/O thread. */
        for (int j = 1; j < c->argc; j++) {
            redis_prefetch(c->argv[j]);
        }
    }

    /* Prefetch the argv->ptr if required */
    for (size_t i = 0; i < batch->client_count; i++) {
        client *c = batch->clients[i];
        if (!c || c->argc <= 1) continue;
        for (int j = 1; j < c->argc; j++) {
            if (c->argv[j]->encoding == OBJ_ENCODING_RAW) {
                redis_prefetch(c->argv[j]->ptr);
            }
        }
    }

    /* Get the keys ptrs - we do it here after the key obj was prefetched. */
    for (size_t i = 0; i < batch->key_count; i++) {
        batch->keys[i] = ((robj *)batch->keys[i])->ptr;
    }

    /* Prefetch dict keys for all commands. Prefetching is beneficial only if there are more than one key. */
    if (batch->key_count > 1) {
        server.stat_total_prefetch_batches++;
        /* Prefetch keys from the main dict */
        dictPrefetch(batch->keys_dicts, getObjectValuePtr);
        /* Prefetch keys from the expires dict - no value data to prefetch */
        dictPrefetch(batch->expire_dicts, NULL);
    }
}

/* Adds the client's command to the current batch.
 *
 * Returns C_OK if the command was added successfully, C_ERR otherwise. */
int addCommandToBatch(client *c) {
    if (!batch) return C_ERR;

    /* If the batch is full, process it.
     * We also check the client count to handle cases where
     * no keys exist for the clients' commands. */
    if (batch->client_count == batch->max_prefetch_size || batch->key_count == batch->max_prefetch_size) {
        return C_ERR;
    }

    batch->clients[batch->client_count++] = c;

    /* Get command's keys positions */
    if (c->iolookedcmd) {
        getKeysResult result = GETKEYS_RESULT_INIT;
        int num_keys = getKeysFromCommand(c->iolookedcmd, c->argv, c->argc, &result);
        for (int i = 0; i < num_keys && batch->key_count < batch->max_prefetch_size; i++) {
            batch->keys[batch->key_count] = c->argv[result.keys[i].pos];
            batch->slots[batch->key_count] = c->slot > 0 ? c->slot : 0;
            batch->keys_dicts[batch->key_count] = kvstoreGetDict(c->db->keys, batch->slots[batch->key_count]);
            batch->expire_dicts[batch->key_count] = kvstoreGetDict(c->db->expires, batch->slots[batch->key_count]);
            batch->key_count++;
        }
        getKeysFreeResult(&result);
    }

    return C_OK;
}

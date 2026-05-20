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

/* --------------------------------------------------------------------------
 * Dict prefetching state machine
 * -------------------------------------------------------------------------- */

typedef enum { HT_IDX_FIRST = 0, HT_IDX_SECOND = 1, HT_IDX_INVALID = -1 } dictHtIdx;

typedef enum {
    PREFETCH_BUCKET,        /* Initial state, determines which hash table to use and prefetch the table's bucket */
    PREFETCH_ENTRY,         /* prefetch entries associated with the given key's hash */
    PREFETCH_ENTRY_KEY,     /* dictType-driven prefetch of the entry's key payload (for keyCompare) */
    PREFETCH_ENTRY_VALUE,   /* compare keys; on match, dictType-driven prefetch of the value payload */
    PREFETCH_DONE           /* Indicates that prefetching for this key is complete */
} dictPrefetchState;

/* Per-key state of an in-flight, software-pipelined dictFind, advanced one
 * stage at a time by dictPrefetcher (see below). The non-state fields mirror
 * the locals that a synchronous dictFind would otherwise carry across one
 * bucket walk. */
typedef struct dictPrefetchLookup {
    dictPrefetchState state;  /* Current FSM stage of this lookup */
    dictHtIdx ht_idx;         /* Index of the current hash table (0 or 1 for rehashing) */
    uint64_t bucket_idx;      /* Index of the bucket in the current hash table */
    uint64_t key_hash;        /* Hash value of the key being looked up */
    dictEntry *current_entry; /* Pointer to the current entry being processed */
} dictPrefetchLookup;

/* dictPrefetcher drives a batch of dictPrefetchLookup objects through the
 * prefetch FSM, yielding to the next in-flight lookup each time a prefetch
 * is issued — so one lookup's memory stall overlaps another's work. The
 * state machine itself is fully dict-pure: any key/value payload prefetching
 * is delegated to the dictType->prefetchEntryKey / prefetchEntryValue
 * callbacks of each key's dict. The same prefetcher is used by both the
 * cross-command batch path and the intra-command dictPrefetchKeys() API. */
typedef struct dictPrefetcher {
    size_t cur_idx;              /* Cursor; advances on each prefetch issue */
    size_t nkeys;                /* Total key lookups in this batch */
    size_t remaining;            /* Number of in-flight key lookups (not yet PREFETCH_DONE) */
    void **keys;                 /* Array of key pointers (sds) */
    dict **dicts;                /* Per-key dictionary pointers */
    dictPrefetchLookup *lookups; /* Per-key lookup state, capacity == max_keys */
    size_t max_keys;             /* Capacity of lookups[] */
} dictPrefetcher;

/******************************** State machine diagram for the dict prefetch operation. ******************************
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
                                    |          ┌───────────▼─────────────┐         │
                                    │          |   PREFETCH_ENTRY_KEY    |         ▼
                                    │          └───────────┬─────────────┘         │
        key mismatch - goto next entry                     |                       |
                                    │          ┌───────────▼─────────────┐         │
                                    └──────◄───│   PREFETCH_ENTRY_VALUE  │         ▼
                                               └───────────┬─────────────┘         │
                                                           |                       │
                                                 ┌───────-─▼─────────────┐         │
                                                 │     PREFETCH_DONE     │◄────────┘
                                                 └───────────────────────┘

**********************************************************************************************************************/

/* Issue a software prefetch for `addr`, then yield to the next lookup by
 * advancing the cursor. */
static inline void dictPrefetchAdvance(dictPrefetcher *p, void *addr) {
    redis_prefetch_read(addr);
    if (++p->cur_idx >= p->nkeys) p->cur_idx = 0;
}

static inline void dictPrefetchMarkDone(dictPrefetcher *p, dictPrefetchLookup *lk) {
    lk->state = PREFETCH_DONE;
    p->remaining--;
    server.stat_total_prefetch_entries++;
}

/* Return the next in-flight lookup that still needs work, or NULL if all done. */
static inline dictPrefetchLookup *dictPrefetchNextInFlight(dictPrefetcher *p) {
    if (p->remaining == 0) return NULL;
    while (p->lookups[p->cur_idx].state == PREFETCH_DONE) {
        if (++p->cur_idx >= p->nkeys) p->cur_idx = 0;
    }
    return &p->lookups[p->cur_idx];
}

/* Prefetch the bucket of the next hash table index.
 * If no tables are left, move to the PREFETCH_DONE state. */
static void dictPrefetchBucket(dictPrefetcher *p, dictPrefetchLookup *lk) {
    size_t i = p->cur_idx;
    dict *d = p->dicts[i];

    /* Determine which hash table to use */
    if (lk->ht_idx == HT_IDX_INVALID) {
        lk->ht_idx = HT_IDX_FIRST;
    } else if (lk->ht_idx == HT_IDX_FIRST && dictIsRehashing(d)) {
        lk->ht_idx = HT_IDX_SECOND;
    } else {
        /* No more tables left - mark as done. */
        dictPrefetchMarkDone(p, lk);
        return;
    }

    /* Prefetch the bucket */
    lk->bucket_idx = lk->key_hash & DICTHT_SIZE_MASK(d->ht_size_exp[lk->ht_idx]);
    dictPrefetchAdvance(p, &d->ht_table[lk->ht_idx][lk->bucket_idx]);
    lk->current_entry = NULL;
    lk->state = PREFETCH_ENTRY;
}

/* Prefetch the entry in the bucket and move to the PREFETCH_ENTRY_KEY state.
 * If no more entries in the bucket, move to the PREFETCH_BUCKET state to look at the next table. */
static void dictPrefetchEntry(dictPrefetcher *p, dictPrefetchLookup *lk) {
    size_t i = p->cur_idx;

    if (lk->current_entry) {
        /* We already found an entry in the bucket - move to the next entry */
        lk->current_entry = dictGetNext(lk->current_entry);
    } else {
        /* Go to the first entry in the bucket */
        lk->current_entry = p->dicts[i]->ht_table[lk->ht_idx][lk->bucket_idx];
    }

    if (lk->current_entry) {
        dictPrefetchAdvance(p, lk->current_entry);
        lk->state = PREFETCH_ENTRY_KEY;
    } else {
        /* No entry found in the bucket - try the bucket in the next table */
        lk->state = PREFETCH_BUCKET;
    }
}

/* Bring the entry's key payload into cache via the dictType callback,
 * then move to PREFETCH_ENTRY_VALUE where the keyCompare runs. If the
 * dict provides no callback, the entry alone already carries everything
 * keyCompare needs. */
static void dictPrefetchEntryKey(dictPrefetcher *p, dictPrefetchLookup *lk) {
    dictType *type = p->dicts[p->cur_idx]->type;
    lk->state = PREFETCH_ENTRY_VALUE;
    if (type->prefetchEntryKey) {
        void *addr = type->prefetchEntryKey(lk->current_entry);
        if (addr) dictPrefetchAdvance(p, addr);
    }
}

/* Compare the entry's stored key against the lookup key. On match, ask
 * the dictType to prefetch the value-side payload (if any) and mark the
 * lookup done. On mismatch, walk to the next entry in the chain.
 *
 * The entry's stored key may be in a different shape than the lookup key
 * (e.g. dbDictType stores a kvobj but keyCompare wants the sds). When that
 * is the case the dict provides keyFromStoredKey to convert; otherwise the
 * stored key is already in comparable form. This mirrors what
 * dictFindLinkInternal does. */
static void dictPrefetchEntryValue(dictPrefetcher *p, dictPrefetchLookup *lk) {
    size_t i = p->cur_idx;
    dict *d = p->dicts[i];
    dictType *type = d->type;
    const void *stored_key = dictGetKey(lk->current_entry);
    const void *cmp_key = type->keyFromStoredKey ? type->keyFromStoredKey(stored_key) : stored_key;

    /* 1. If this is the last element, we assume a hit and don't compare the keys
     * 2. The stored entry matches the lookup key. */
    if ((!dictGetNext(lk->current_entry) && !dictIsRehashing(d)) ||
        dictCompareKeys(d, p->keys[i], cmp_key))
    {
        if (type->prefetchEntryValue) {
            void *addr = type->prefetchEntryValue(lk->current_entry);
            if (addr) dictPrefetchAdvance(p, addr);
        }
        dictPrefetchMarkDone(p, lk);
    } else {
        /* Not found in the current entry, move to the next entry */
        lk->state = PREFETCH_ENTRY;
    }
}

/* Allocate the per-key lookup array. The prefetcher can then be reused across
 * many batches by repeated dictPrefetcherReset / dictPrefetcherRun calls. */
static void dictPrefetcherInit(dictPrefetcher *p, size_t max_keys) {
    p->lookups = zcalloc(max_keys * sizeof(dictPrefetchLookup));
    p->max_keys = max_keys;
}

static void dictPrefetcherFree(dictPrefetcher *p) {
    zfree(p->lookups);
    p->lookups = NULL;
    p->max_keys = 0;
}

/* Configure the prefetcher for a single batch and seed every lookup's
 * starting state. dicts/keys must remain valid until dictPrefetcherRun
 * returns; only the pointers are stored. */
static void dictPrefetcherReset(dictPrefetcher *p, dict **dicts, void **keys, size_t nkeys) {
    serverAssert(nkeys <= p->max_keys);
    p->dicts = dicts;
    p->keys = keys;
    p->nkeys = nkeys;
    p->cur_idx = 0;

    size_t remaining = 0;
    for (size_t i = 0; i < nkeys; i++) {
        dictPrefetchLookup *lk = &p->lookups[i];
        if (!dicts[i] || dictSize(dicts[i]) == 0) {
            lk->state = PREFETCH_DONE;
            continue;
        }

        /* We skip prefetch during loading, so ht_table[0] should never be NULL
         * when dictSize() > 0 (which only happens mid-dictEmpty via _dictReset). */
        serverAssert(dicts[i]->ht_table[0]);

        lk->ht_idx = HT_IDX_INVALID;
        lk->current_entry = NULL;
        lk->state = PREFETCH_BUCKET;
        lk->key_hash = dictGetHash(dicts[i], keys[i]);
        remaining++;
    }
    p->remaining = remaining;
}

/* Drive the prefetch state machine across all dict lookups until every lookup
 * reaches PREFETCH_DONE.
 *
 * Conceptually each dict lookup is a dictFind broken into four stages:
 *   bucket → entry → entry key payload → entry value payload
 * If the key is not found in ht[0] and the dict is mid-rehash, the lookup
 * loops back to the bucket stage to retry against ht[1].
 *
 * Instead of waiting for each stage's memory access to complete, the FSM
 * issues a prefetch and yields to another in-flight lookup, hiding the
 * memory access latency.
 *
 * Any prefetching of the entry's key payload (e.g. an out-of-line kvobj head)
 * and the entry's value payload (e.g. kv->ptr for a RAW string) is delegated
 * to dictType->prefetchEntryKey and prefetchEntryValue respectively. */
static void dictPrefetcherRun(dictPrefetcher *p) {
    dictPrefetchLookup *lk;
    while ((lk = dictPrefetchNextInFlight(p))) {
        switch (lk->state) {
            case PREFETCH_BUCKET:      dictPrefetchBucket(p, lk); break;
            case PREFETCH_ENTRY:       dictPrefetchEntry(p, lk); break;
            case PREFETCH_ENTRY_KEY:   dictPrefetchEntryKey(p, lk); break;
            case PREFETCH_ENTRY_VALUE: dictPrefetchEntryValue(p, lk); break;
            default: serverPanic("Unknown prefetch state %d", lk->state);
        }
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
 *           dictPrefetchKeys(dicts, keys, n);
 *           // Now process these n keys — dict bucket / entry / key payload
 *           // (and value payload, if dictType->prefetchEntryValue is set)
 *           // are warm in cache.
 *       }
 *   }
 * ----------------------------------------------------------------------- */
void dictPrefetchKeys(dict **dicts, void **keys, size_t nkeys) {
    /* Single-key prefetch has no benefit — nothing to interleave with.
     * Callers passing nkeys==1 (e.g. tail of a multi-key batch) should
     * fall through to a direct lookup. */
    if (nkeys <= 1) return;

    /* Guard the fixed-size stack array below; callers must batch larger
     * inputs into chunks of DICT_PREFETCH_MAX_SIZE or smaller. */
    serverAssert(nkeys <= DICT_PREFETCH_MAX_SIZE);
    server.stat_total_prefetch_batches++;

    dictPrefetchLookup lookups[DICT_PREFETCH_MAX_SIZE];
    dictPrefetcher p = { .lookups = lookups, .max_keys = nkeys };
    dictPrefetcherReset(&p, dicts, keys, nkeys);
    dictPrefetcherRun(&p);
}

/* --------------------------------------------------------------------------
 * Cross-command batch prefetching
 * -------------------------------------------------------------------------- */

/* PrefetchCommandsBatch structure holds the state of the current batch of client commands being processed. */
typedef struct PrefetchCommandsBatch {
    size_t key_count;               /* Number of keys in the current batch */
    size_t client_count;            /* Number of clients in the current batch */
    size_t pcmd_count;              /* Number of pending commands in the current batch */
    size_t max_prefetch_size;       /* Maximum number of keys to prefetch in a batch */
    void **keys;                    /* Array of keys to prefetch in the current batch */
    client **clients;               /* Array of clients in the current batch */
    pendingCommand **pending_cmds;  /* Array of pending commands in the current batch */
    dict **keys_dicts;              /* Main dict for each key */
    dictPrefetcher prefetcher;      /* Initialized once; reset and reused per batch. */
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
    dictPrefetcherFree(&batch->prefetcher);
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
    dictPrefetcherInit(&batch->prefetcher, max_prefetch_size);
}

void onMaxBatchSizeChange(void) {
    if (batch && batch->client_count > 0) {
        /* We need to process the current batch before updating the size */
        return;
    }

    freePrefetchCommandsBatch();
    prefetchCommandsBatchInit();
}

void resetCommandsBatch(void) {
    if (batch == NULL) {
        /* Handle the case where prefetching becomes enabled from disabled. */
        if (server.prefetch_batch_max_size) prefetchCommandsBatchInit();
        return;
    }

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
        /* Prefetch keys from the main dict — value-side prefetch (if any)
         * is driven by dbDictType->prefetchEntryValue. */
        dictPrefetcherReset(&batch->prefetcher, batch->keys_dicts, batch->keys, batch->key_count);
        dictPrefetcherRun(&batch->prefetcher);
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
        dict *cmd_dict = kvstoreGetDict(c->db->keys, pcmd->slot > 0 ? pcmd->slot : 0);
        for (int i = 0; i < pcmd->keys_result.numkeys && batch->key_count < batch->max_prefetch_size; i++) {
            batch->keys[batch->key_count] = pcmd->argv[pcmd->keys_result.keys[i].pos];
            batch->keys_dicts[batch->key_count] = cmd_dict;
            batch->key_count++;
            /* Mark the command as prefetched so the intra-command prefetch
             * path skips it. Even on a partial batch, running both paths
             * would just contend for cache bandwidth. */
            pcmd->flags |= PENDING_CMD_KEYS_PREFETCHED;
        }
        pcmd = pcmd->next;
    }

    return C_OK;
}

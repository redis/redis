/* 
 * Active memory defragmentation
 * Try to find key / value allocations that need to be re-allocated in order 
 * to reduce external fragmentation.
 * We do that by scanning the keyspace and for each pointer we have, we can try to
 * ask the allocator if moving it to a new address will help reduce fragmentation.
 *
 * Copyright (c) 2020-Present, Redis Ltd.
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

#include "server.h"
#include <stddef.h>
#include <math.h>

#ifdef HAVE_DEFRAG

#define DEFRAG_CYCLE_US 500 /* Standard duration of defrag cycle (in microseconds) */

typedef enum { DEFRAG_NOT_DONE = 0,
               DEFRAG_DONE = 1 } doneStatus;

/*
 * Defragmentation is performed in stages. Each stage is serviced by a stage function
 * (defragStageFn). The stage function is passed a context (void*) to defrag. The contents of that
 * context are unique to the particular stage - and may even be NULL for some stage functions. The
 * same stage function can be used multiple times (for different stages) each having a different
 * context.
 *
 * Parameters:
 *  endtime     - This is the monotonic time that the function should end and return. This ensures
 *                a bounded latency due to defrag.
 *  ctx         - A pointer to context which is unique to the stage function.
 *
 * Returns:
 *  - DEFRAG_DONE if the stage is complete
 *  - DEFRAG_NOT_DONE if there is more work to do
 */
typedef doneStatus (*defragStageFn)(void *ctx, monotime endtime);

/* Function pointer type for freeing context in defragmentation stages. */
typedef void (*defragStageContextFreeFn)(void *ctx);
typedef struct {
    defragStageFn stage_fn; /* The function to be invoked for the stage */
    defragStageContextFreeFn ctx_free_fn; /* Function to free the context */
    void *ctx; /* Context, unique to the stage function */
} StageDescriptor;

/* Globals needed for the main defrag processing logic.
 * Doesn't include variables specific to a stage or type of data. */
struct DefragContext {
    monotime start_cycle;           /* Time of beginning of defrag cycle */
    long long start_defrag_hits;    /* server.stat_active_defrag_hits captured at beginning of cycle */
    long long start_defrag_misses;  /* server.stat_active_defrag_misses captured at beginning of cycle */
    float start_frag_pct;           /* Fragmention percent of beginning of defrag cycle */
    float decay_rate;               /* Defrag speed decay rate */

    list *remaining_stages;         /* List of stages which remain to be processed */
    listNode *current_stage;        /* The list node of stage that's currently being processed */

    long long timeproc_id;      /* Eventloop ID of the timerproc (or AE_DELETED_EVENT_ID) */
    monotime timeproc_end_time; /* Ending time of previous timerproc execution */
    long timeproc_overage_us;   /* A correction value if over target CPU percent */
};
static struct DefragContext defrag = {0, 0, 0, 0, 1.0f};

/* There are a number of stages which process a kvstore. To simplify this, a stage helper function
 * `defragStageKvstoreHelper()` is defined. This function aids in iterating over the kvstore. It
 * uses these definitions.
 */
/* State of the kvstore helper. The context passed to the kvstore helper MUST BEGIN
 * with a kvstoreIterState (or be passed as NULL). */
#define KVS_SLOT_DEFRAG_LUT -2
#define KVS_SLOT_UNASSIGNED -1
typedef struct {
    kvstore *kvs;
    int slot;
    unsigned long cursor;
} kvstoreIterState;
#define INIT_KVSTORE_STATE(kvs) ((kvstoreIterState){(kvs), KVS_SLOT_DEFRAG_LUT, 0})

/* The kvstore helper uses this function to perform tasks before continuing the iteration. For the
 * main dictionary, large items are set aside and processed by this function before continuing with
 * iteration over the kvstore.
 *  endtime     - This is the monotonic time that the function should end and return.
 *  ctx         - Context for functions invoked by the helper. If provided in the call to
 *                `defragStageKvstoreHelper()`, the `kvstoreIterState` portion (at the beginning)
 *                will be updated with the current kvstore iteration status.
 *
 * Returns:
 *  - DEFRAG_DONE if the pre-continue work is complete
 *  - DEFRAG_NOT_DONE if there is more work to do
 */
typedef doneStatus (*kvstoreHelperPreContinueFn)(void *ctx, monotime endtime);

typedef struct {
    kvstoreIterState kvstate;
    int dbid;

    /* When scanning a main kvstore, large elements are queued for later handling rather than
     * causing a large latency spike while processing a hash table bucket. This list is only used
     * for stage: "defragStageDbKeys". It will only contain values for the current kvstore being
     * defragged.
     * Note that this is a list of key names. It's possible that the key may be deleted or modified
     * before "later" and we will search by key name to find the entry when we defrag the item later. */
    list *defrag_later;
    unsigned long defrag_later_cursor;
} defragKeysCtx;
static_assert(offsetof(defragKeysCtx, kvstate) == 0, "defragStageKvstoreHelper requires this");

/* Context for hexpires */
typedef struct {
    int dbid;
    ebuckets hexpires;
    unsigned long cursor;
} defragHExpiresCtx;

/* Context for pubsub kvstores */
typedef dict *(*getClientChannelsFn)(client *);
typedef struct {
    kvstoreIterState kvstate;
    getClientChannelsFn getPubSubChannels;
} defragPubSubCtx;
static_assert(offsetof(defragPubSubCtx, kvstate) == 0, "defragStageKvstoreHelper requires this");

typedef struct {
    sds module_name;
    unsigned long cursor;
} defragModuleCtx;

/* this method was added to jemalloc in order to help us understand which
 * pointers are worthwhile moving and which aren't */
int je_get_defrag_hint(void* ptr);

/* Defrag helper for generic allocations without freeing old pointer.
 *
 * Note: The caller is responsible for freeing the old pointer if this function
 * returns a non-NULL value. */
void* activeDefragAllocWithoutFree(void *ptr) {
    size_t size;
    void *newptr;
    if(!je_get_defrag_hint(ptr)) {
        server.stat_active_defrag_misses++;
        return NULL;
    }
    /* move this allocation to a new allocation.
     * make sure not to use the thread cache. so that we don't get back the same
     * pointers we try to free */
    size = zmalloc_usable_size(ptr);
    newptr = zmalloc_no_tcache(size);
    memcpy(newptr, ptr, size);
    server.stat_active_defrag_hits++;
    return newptr;
}

void activeDefragFree(void *ptr) {
    zfree_no_tcache(ptr);
}

/* Defrag helper for generic allocations.
 *
 * returns NULL in case the allocation wasn't moved.
 * when it returns a non-null value, the old pointer was already released
 * and should NOT be accessed. */
void* activeDefragAlloc(void *ptr) {
    void *newptr = activeDefragAllocWithoutFree(ptr);
    if (newptr)
        activeDefragFree(ptr);
    return newptr;
}

/* Raw memory allocation for defrag, avoid using tcache. */
void *activeDefragAllocRaw(size_t size) {
    return zmalloc_no_tcache(size);
}

/* Raw memory free for defrag, avoid using tcache. */
void activeDefragFreeRaw(void *ptr) {
    activeDefragFree(ptr);
    server.stat_active_defrag_hits++;
}

/*Defrag helper for sds strings
 *
 * returns NULL in case the allocation wasn't moved.
 * when it returns a non-null value, the old pointer was already released
 * and should NOT be accessed. */
sds activeDefragSds(sds sdsptr) {
    void* ptr = sdsAllocPtr(sdsptr);
    void* newptr = activeDefragAlloc(ptr);
    if (newptr) {
        size_t offset = sdsptr - (char*)ptr;
        sdsptr = (char*)newptr + offset;
        return sdsptr;
    }
    return NULL;
}

/* Defrag helper for hfield strings
 *
 * returns NULL in case the allocation wasn't moved.
 * when it returns a non-null value, the old pointer was already released
 * and should NOT be accessed. */
hfield activeDefragHfield(hfield hf) {
    void *ptr = hfieldGetAllocPtr(hf);
    void *newptr = activeDefragAlloc(ptr);
    if (newptr) {
        size_t offset = hf - (char*)ptr;
        hf = (char*)newptr + offset;
        return hf;
    }
    return NULL;
}

/* Defrag helper for hfield strings and update the reference in the dict.
 *
 * returns NULL in case the allocation wasn't moved.
 * when it returns a non-null value, the old pointer was already released
 * and should NOT be accessed. */
void *activeDefragHfieldAndUpdateRef(void *ptr, void *privdata) {
    dict *d = privdata;
    dictEntryLink link;

    /* Before the key is released, obtain the link to
     * ensure we can safely access and update the key. */
    dictUseStoredKeyApi(d, 1);
    link = dictFindLink(d, ptr, NULL);
    serverAssert(link);
    dictUseStoredKeyApi(d, 0);

    hfield newhf = activeDefragHfield(ptr);
    if (newhf)
        dictSetKeyAtLink(d, newhf, &link, 0);
    return newhf;
}

/* Defrag helper for robj and/or string objects with expected refcount.
 *
 * Like activeDefragStringOb, but it requires the caller to pass in the expected
 * reference count. In some cases, the caller needs to update a robj whose
 * reference count is not 1, in these cases, the caller must explicitly pass
 * in the reference count, otherwise defragmentation will not be performed.
 * Note that the caller is responsible for updating any other references to the robj. */
robj *activeDefragStringObEx(robj* ob, int expected_refcount) {
    robj *ret = NULL;
    if (ob->refcount!=expected_refcount)
        return NULL;

    /* try to defrag robj (only if not an EMBSTR type (handled below). */
    if (ob->type!=OBJ_STRING || ob->encoding!=OBJ_ENCODING_EMBSTR) {
        if ((ret = activeDefragAlloc(ob))) {
            ob = ret;
        }
    }

    /* try to defrag string object */
    if (ob->type == OBJ_STRING) {
        if(ob->encoding==OBJ_ENCODING_RAW) {
            sds newsds = activeDefragSds((sds)ob->ptr);
            if (newsds) {
                ob->ptr = newsds;
            }
        } else if (ob->encoding==OBJ_ENCODING_EMBSTR) {
            /* The sds is embedded in the object allocation, calculate the
             * offset and update the pointer in the new allocation. */
            long ofs = (intptr_t)ob->ptr - (intptr_t)ob;
            if ((ret = activeDefragAlloc(ob))) {
                ret->ptr = (void*)((intptr_t)ret + ofs);
            }
        } else if (ob->encoding!=OBJ_ENCODING_INT) {
            serverPanic("Unknown string encoding");
        }
    }
    return ret;
}

/* Defrag helper for robj and/or string objects
 *
 * returns NULL in case the allocation wasn't moved.
 * when it returns a non-null value, the old pointer was already released
 * and should NOT be accessed. */
robj *activeDefragStringOb(robj* ob) {
    return activeDefragStringObEx(ob, 1);
}

/* Defrag helper for lua scripts
 *
 * returns NULL in case the allocation wasn't moved.
 * when it returns a non-null value, the old pointer was already released
 * and should NOT be accessed. */
luaScript *activeDefragLuaScript(luaScript *script) {
    luaScript *ret = NULL;

    /* try to defrag script struct */
    if ((ret = activeDefragAlloc(script))) {
        script = ret;
    }

    /* try to defrag actual script object */
    robj *ob = activeDefragStringOb(script->body);
    if (ob) script->body = ob;

    return ret;
}

/* Defrag helper for dict main allocations (dict struct, and hash tables).
 * Receives a pointer to the dict* and return a new dict* when the dict
 * struct itself was moved.
 * 
 * Returns NULL in case the allocation wasn't moved.
 * When it returns a non-null value, the old pointer was already released
 * and should NOT be accessed. */
dict *dictDefragTables(dict *d) {
    dict *ret = NULL;
    dictEntry **newtable;
    /* handle the dict struct */
    if ((ret = activeDefragAlloc(d)))
        d = ret;
    /* handle the first hash table */
    if (!d->ht_table[0]) return ret; /* created but unused */
    newtable = activeDefragAlloc(d->ht_table[0]);
    if (newtable)
        d->ht_table[0] = newtable;
    /* handle the second hash table */
    if (d->ht_table[1]) {
        newtable = activeDefragAlloc(d->ht_table[1]);
        if (newtable)
            d->ht_table[1] = newtable;
    }
    return ret;
}

/* Internal function used by zslDefrag */
void zslUpdateNode(zskiplist *zsl, zskiplistNode *oldnode, zskiplistNode *newnode, zskiplistNode **update) {
    int i;
    for (i = 0; i < zsl->level; i++) {
        if (update[i]->level[i].forward == oldnode)
            update[i]->level[i].forward = newnode;
    }
    serverAssert(zsl->header!=oldnode);
    if (newnode->level[0].forward) {
        serverAssert(newnode->level[0].forward->backward==oldnode);
        newnode->level[0].forward->backward = newnode;
    } else {
        serverAssert(zsl->tail==oldnode);
        zsl->tail = newnode;
    }
}

/* Defrag helper for sorted set.
 * Update the robj pointer, defrag the skiplist struct and return the new score
 * reference. We may not access oldele pointer (not even the pointer stored in
 * the skiplist), as it was already freed. Newele may be null, in which case we
 * only need to defrag the skiplist, but not update the obj pointer.
 * When return value is non-NULL, it is the score reference that must be updated
 * in the dict record. */
double *zslDefrag(zskiplist *zsl, double score, sds oldele, sds newele) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x, *newx;
    int i;
    sds ele = newele? newele: oldele;

    /* find the skiplist node referring to the object that was moved,
     * and all pointers that need to be updated if we'll end up moving the skiplist node. */
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward &&
            x->level[i].forward->ele != oldele && /* make sure not to access the
                                                     ->obj pointer if it matches
                                                     oldele */
            (x->level[i].forward->score < score ||
                (x->level[i].forward->score == score &&
                sdscmp(x->level[i].forward->ele,ele) < 0)))
            x = x->level[i].forward;
        update[i] = x;
    }

    /* update the robj pointer inside the skip list record. */
    x = x->level[0].forward;
    serverAssert(x && score == x->score && x->ele==oldele);
    if (newele)
        x->ele = newele;

    /* try to defrag the skiplist record itself */
    newx = activeDefragAlloc(x);
    if (newx) {
        zslUpdateNode(zsl, x, newx, update);
        return &newx->score;
    }
    return NULL;
}

/* Defrag helper for sorted set.
 * Defrag a single dict entry key name, and corresponding skiplist struct */
void activeDefragZsetEntry(zset *zs, dictEntry *de) {
    sds newsds;
    double* newscore;
    sds sdsele = dictGetKey(de);
    if ((newsds = activeDefragSds(sdsele)))
        dictSetKey(zs->dict, de, newsds);
    newscore = zslDefrag(zs->zsl, *(double*)dictGetVal(de), sdsele, newsds);
    if (newscore) {
        dictSetVal(zs->dict, de, newscore);
    }
}

#define DEFRAG_SDS_DICT_NO_VAL 0
#define DEFRAG_SDS_DICT_VAL_IS_SDS 1
#define DEFRAG_SDS_DICT_VAL_IS_STROB 2
#define DEFRAG_SDS_DICT_VAL_VOID_PTR 3
#define DEFRAG_SDS_DICT_VAL_LUA_SCRIPT 4

void activeDefragSdsDictCallback(void *privdata, const dictEntry *de, dictEntryLink plink) {
    UNUSED(plink);
    UNUSED(privdata);
    UNUSED(de);
}

void activeDefragLuaScriptDictCallback(void *privdata, const dictEntry *de, dictEntryLink plink) {
    UNUSED(plink);
    UNUSED(privdata);

    /* If this luaScript is in the LRU list, unconditionally update the node's
     * value pointer to the current dict key (regardless of reallocation). */
    luaScript *script = dictGetVal(de);
    if (script->node)
        script->node->value = dictGetKey(de);
}

void activeDefragHfieldDictCallback(void *privdata, const dictEntry *de, dictEntryLink plink) {
    UNUSED(plink);
    dict *d = privdata;
    hfield newhf = NULL, hf = dictGetKey(de);

    /* If the hfield does not have TTL, we directly defrag it.
     * Fields with TTL are skipped here and will be defragmented later
     * during the hash expiry ebuckets defragmentation phase. */
    if (hfieldGetExpireTime(hf) == EB_EXPIRE_TIME_INVALID) {
        if ((newhf = activeDefragHfield(hf)))
            dictSetKey(d, (dictEntry *)de, newhf);
    }
}

/* Defrag a dict with sds key and optional value (either ptr, sds or robj string) */
void activeDefragSdsDict(dict* d, int val_type) {
    unsigned long cursor = 0;
    dictDefragFunctions defragfns = {
        .defragAlloc = activeDefragAlloc,
        .defragKey = (dictDefragAllocFunction *)activeDefragSds,
        .defragVal = (val_type == DEFRAG_SDS_DICT_VAL_IS_SDS ? (dictDefragAllocFunction *)activeDefragSds :
                      val_type == DEFRAG_SDS_DICT_VAL_IS_STROB ? (dictDefragAllocFunction *)activeDefragStringOb :
                      val_type == DEFRAG_SDS_DICT_VAL_VOID_PTR ? (dictDefragAllocFunction *)activeDefragAlloc :
                      val_type == DEFRAG_SDS_DICT_VAL_LUA_SCRIPT ? (dictDefragAllocFunction *)activeDefragLuaScript :
                      NULL)
    };
    dictScanFunction *fn = (val_type == DEFRAG_SDS_DICT_VAL_LUA_SCRIPT ?
        activeDefragLuaScriptDictCallback : activeDefragSdsDictCallback);
    do {
        cursor = dictScanDefrag(d, cursor, fn,
                                &defragfns, NULL);
    } while (cursor != 0);
}

/* Defrag a dict with hfield key and sds value. */
void activeDefragHfieldDict(dict *d) {
    unsigned long cursor = 0;
    dictDefragFunctions defragfns = {
        .defragAlloc = activeDefragAlloc,
        .defragKey = NULL, /* Will be defragmented in activeDefragHfieldDictCallback. */
        .defragVal = (dictDefragAllocFunction *)activeDefragSds
    };
    do {
        cursor = dictScanDefrag(d, cursor, activeDefragHfieldDictCallback,
                                &defragfns, d);
    } while (cursor != 0);

    /* Continue with defragmentation of hash fields that have with TTL.
     * During the dictionary defragmentaion above, we skipped fields with TTL,
     * Now we continue to defrag those fields by using the expiry buckets. */
    if (d->type == &mstrHashDictTypeWithHFE) {
        cursor = 0;
        ebDefragFunctions eb_defragfns = {
            .defragAlloc = activeDefragAlloc,
            .defragItem = activeDefragHfieldAndUpdateRef
        };
        ebuckets *eb = hashTypeGetDictMetaHFE(d);
        while (ebScanDefrag(eb, &hashFieldExpireBucketsType, &cursor, &eb_defragfns, d)) {}
    }
}

/* Defrag a list of ptr, sds or robj string values */
void activeDefragQuickListNode(quicklist *ql, quicklistNode **node_ref) {
    quicklistNode *newnode, *node = *node_ref;
    unsigned char *newzl;
    if ((newnode = activeDefragAlloc(node))) {
        if (newnode->prev)
            newnode->prev->next = newnode;
        else
            ql->head = newnode;
        if (newnode->next)
            newnode->next->prev = newnode;
        else
            ql->tail = newnode;
        *node_ref = node = newnode;
    }
    if ((newzl = activeDefragAlloc(node->entry)))
        node->entry = newzl;
}

void activeDefragQuickListNodes(quicklist *ql) {
    quicklistNode *node = ql->head;
    while (node) {
        activeDefragQuickListNode(ql, &node);
        node = node->next;
    }
}

/* when the value has lots of elements, we want to handle it later and not as
 * part of the main dictionary scan. this is needed in order to prevent latency
 * spikes when handling large items */
void defragLater(defragKeysCtx *ctx, kvobj *kv) {
    if (!ctx->defrag_later) {
        ctx->defrag_later = listCreate();
        listSetFreeMethod(ctx->defrag_later, sdsfreegeneric);
        ctx->defrag_later_cursor = 0;
    }
    sds key = sdsdup(kvobjGetKey(kv));
    listAddNodeTail(ctx->defrag_later, key);
}

/* returns 0 if no more work needs to be been done, and 1 if time is up and more work is needed. */
long scanLaterList(robj *ob, unsigned long *cursor, monotime endtime) {
    quicklist *ql = ob->ptr;
    quicklistNode *node;
    long iterations = 0;
    int bookmark_failed = 0;
    serverAssert(ob->type == OBJ_LIST && ob->encoding == OBJ_ENCODING_QUICKLIST);

    if (*cursor == 0) {
        /* if cursor is 0, we start new iteration */
        node = ql->head;
    } else {
        node = quicklistBookmarkFind(ql, "_AD");
        if (!node) {
            /* if the bookmark was deleted, it means we reached the end. */
            *cursor = 0;
            return 0;
        }
        node = node->next;
    }

    (*cursor)++;
    while (node) {
        activeDefragQuickListNode(ql, &node);
        server.stat_active_defrag_scanned++;
        if (++iterations > 128 && !bookmark_failed) {
            if (getMonotonicUs() > endtime) {
                if (!quicklistBookmarkCreate(&ql, "_AD", node)) {
                    bookmark_failed = 1;
                } else {
                    ob->ptr = ql; /* bookmark creation may have re-allocated the quicklist */
                    return 1;
                }
            }
            iterations = 0;
        }
        node = node->next;
    }
    quicklistBookmarkDelete(ql, "_AD");
    *cursor = 0;
    return bookmark_failed? 1: 0;
}

typedef struct {
    zset *zs;
} scanLaterZsetData;

void scanLaterZsetCallback(void *privdata, const dictEntry *_de, dictEntryLink plink) {
    UNUSED(plink);
    dictEntry *de = (dictEntry*)_de;
    scanLaterZsetData *data = privdata;
    activeDefragZsetEntry(data->zs, de);
    server.stat_active_defrag_scanned++;
}

void scanLaterZset(robj *ob, unsigned long *cursor) {
    serverAssert(ob->type == OBJ_ZSET && ob->encoding == OBJ_ENCODING_SKIPLIST);
    zset *zs = (zset*)ob->ptr;
    dict *d = zs->dict;
    scanLaterZsetData data = {zs};
    dictDefragFunctions defragfns = {.defragAlloc = activeDefragAlloc};
    *cursor = dictScanDefrag(d, *cursor, scanLaterZsetCallback, &defragfns, &data);
}

/* Used as scan callback when all the work is done in the dictDefragFunctions. */
void scanCallbackCountScanned(void *privdata, const dictEntry *de, dictEntryLink plink) {
    UNUSED(plink);
    UNUSED(privdata);
    UNUSED(de);
    server.stat_active_defrag_scanned++;
}

void scanLaterSet(robj *ob, unsigned long *cursor) {
    serverAssert(ob->type == OBJ_SET && ob->encoding == OBJ_ENCODING_HT);
    dict *d = ob->ptr;
    dictDefragFunctions defragfns = {
        .defragAlloc = activeDefragAlloc,
        .defragKey = (dictDefragAllocFunction *)activeDefragSds
    };
    *cursor = dictScanDefrag(d, *cursor, scanCallbackCountScanned, &defragfns, NULL);
}

void scanLaterHash(robj *ob, unsigned long *cursor) {
    serverAssert(ob->type == OBJ_HASH && ob->encoding == OBJ_ENCODING_HT);
    dict *d = ob->ptr;

    typedef enum {
        HASH_DEFRAG_NONE = 0,
        HASH_DEFRAG_DICT = 1,
        HASH_DEFRAG_EBUCKETS = 2
    } hashDefragPhase;
    static hashDefragPhase defrag_phase = HASH_DEFRAG_NONE;

    /* Start a new hash defrag. */
    if (!*cursor || defrag_phase == HASH_DEFRAG_NONE)
        defrag_phase = HASH_DEFRAG_DICT;

    /* Defrag hash dictionary but skip TTL fields. */
    if (defrag_phase == HASH_DEFRAG_DICT) {
        dictDefragFunctions defragfns = {
            .defragAlloc = activeDefragAlloc,
            .defragKey = NULL, /* Will be defragmented in activeDefragHfieldDictCallback. */
            .defragVal = (dictDefragAllocFunction *)activeDefragSds
        };
        *cursor = dictScanDefrag(d, *cursor, activeDefragHfieldDictCallback, &defragfns, d);

        /* Move to next phase. */
        if (!*cursor) defrag_phase = HASH_DEFRAG_EBUCKETS;
    }

    /* Defrag ebuckets and TTL fields. */
    if (defrag_phase == HASH_DEFRAG_EBUCKETS) {
        if (d->type == &mstrHashDictTypeWithHFE) {
            ebDefragFunctions eb_defragfns = {
                .defragAlloc = activeDefragAlloc,
                .defragItem = activeDefragHfieldAndUpdateRef
            };
            ebuckets *eb = hashTypeGetDictMetaHFE(d);
            ebScanDefrag(eb, &hashFieldExpireBucketsType, cursor, &eb_defragfns, d);
        } else {
            /* Finish defragmentation if this dict doesn't have expired fields. */
            *cursor = 0;
        }
        if (!*cursor) defrag_phase = HASH_DEFRAG_NONE;
    }
}

void defragQuicklist(defragKeysCtx *ctx, kvobj *kv) {
    quicklist *ql = kv->ptr, *newql;
    serverAssert(kv->type == OBJ_LIST && kv->encoding == OBJ_ENCODING_QUICKLIST);
    if ((newql = activeDefragAlloc(ql)))
        kv->ptr = ql = newql;
    if (ql->len > server.active_defrag_max_scan_fields)
        defragLater(ctx, kv);
    else
        activeDefragQuickListNodes(ql);
}

void defragZsetSkiplist(defragKeysCtx *ctx, kvobj *ob) {
    zset *zs = (zset*)ob->ptr;
    zset *newzs;
    zskiplist *newzsl;
    dict *newdict;
    dictEntry *de;
    struct zskiplistNode *newheader;
    serverAssert(ob->type == OBJ_ZSET && ob->encoding == OBJ_ENCODING_SKIPLIST);
    if ((newzs = activeDefragAlloc(zs)))
        ob->ptr = zs = newzs;
    if ((newzsl = activeDefragAlloc(zs->zsl)))
        zs->zsl = newzsl;
    if ((newheader = activeDefragAlloc(zs->zsl->header)))
        zs->zsl->header = newheader;
    if (dictSize(zs->dict) > server.active_defrag_max_scan_fields)
        defragLater(ctx, ob);
    else {
        dictIterator di;
        dictInitIterator(&di, zs->dict);
        while((de = dictNext(&di)) != NULL) {
            activeDefragZsetEntry(zs, de);
        }
        dictResetIterator(&di);
    }
    /* defrag the dict struct and tables */
    if ((newdict = dictDefragTables(zs->dict)))
        zs->dict = newdict;
}

void defragHash(defragKeysCtx *ctx, kvobj *ob) {
    dict *d, *newd;
    serverAssert(ob->type == OBJ_HASH && ob->encoding == OBJ_ENCODING_HT);
    d = ob->ptr;
    if (dictSize(d) > server.active_defrag_max_scan_fields)
        defragLater(ctx, ob);
    else
        activeDefragHfieldDict(d);
    /* defrag the dict struct and tables */
    if ((newd = dictDefragTables(ob->ptr)))
        ob->ptr = newd;
}

void defragSet(defragKeysCtx *ctx, kvobj *ob) {
    dict *d, *newd;
    serverAssert(ob->type == OBJ_SET && ob->encoding == OBJ_ENCODING_HT);
    d = ob->ptr;
    if (dictSize(d) > server.active_defrag_max_scan_fields)
        defragLater(ctx, ob);
    else
        activeDefragSdsDict(d, DEFRAG_SDS_DICT_NO_VAL);
    /* defrag the dict struct and tables */
    if ((newd = dictDefragTables(ob->ptr)))
        ob->ptr = newd;
}

/* Defrag callback for radix tree iterator, called for each node,
 * used in order to defrag the nodes allocations. */
int defragRaxNode(raxNode **noderef, void *privdata) {
    UNUSED(privdata);
    raxNode *newnode = activeDefragAlloc(*noderef);
    if (newnode) {
        *noderef = newnode;
        return 1;
    }
    return 0;
}

/* returns 0 if no more work needs to be been done, and 1 if time is up and more work is needed. */
int scanLaterStreamListpacks(robj *ob, unsigned long *cursor, monotime endtime) {
    static unsigned char next[sizeof(streamID)];
    raxIterator ri;
    long iterations = 0;
    serverAssert(ob->type == OBJ_STREAM && ob->encoding == OBJ_ENCODING_STREAM);

    stream *s = ob->ptr;
    raxStart(&ri,s->rax);
    if (*cursor == 0) {
        /* if cursor is 0, we start new iteration */
        defragRaxNode(&s->rax->head, NULL);
        /* assign the iterator node callback before the seek, so that the
         * initial nodes that are processed till the first item are covered */
        ri.node_cb = defragRaxNode;
        raxSeek(&ri,"^",NULL,0);
    } else {
        /* if cursor is non-zero, we seek to the static 'next'.
         * Since node_cb is set after seek operation, any node traversed during seek wouldn't
         * be defragmented. To prevent this, we advance to next node before exiting previous
         * run, ensuring it gets defragmented instead of being skipped during current seek. */
        if (!raxSeek(&ri,">=", next, sizeof(next))) {
            *cursor = 0;
            raxStop(&ri);
            return 0;
        }
        /* assign the iterator node callback after the seek, so that the
         * initial nodes that are processed till now aren't covered */
        ri.node_cb = defragRaxNode;
    }

    (*cursor)++;
    while (raxNext(&ri)) {
        void *newdata = activeDefragAlloc(ri.data);
        if (newdata)
            raxSetData(ri.node, ri.data=newdata);
        server.stat_active_defrag_scanned++;
        if (++iterations > 128) {
            if (getMonotonicUs() > endtime) {
                /* Move to next node. */
                if (!raxNext(&ri)) {
                    /* If we reached the end, we can stop */
                    *cursor = 0;
                    raxStop(&ri);
                    return 0;
                }
                serverAssert(ri.key_len==sizeof(next));
                memcpy(next,ri.key,ri.key_len);
                raxStop(&ri);
                return 1;
            }
            iterations = 0;
        }
    }
    raxStop(&ri);
    *cursor = 0;
    return 0;
}

/* optional callback used defrag each rax element (not including the element pointer itself) */
typedef void *(raxDefragFunction)(raxIterator *ri, void *privdata);

/* defrag radix tree including:
 * 1) rax struct
 * 2) rax nodes
 * 3) rax entry data (only if defrag_data is specified)
 * 4) call a callback per element, and allow the callback to return a new pointer for the element */
void defragRadixTree(rax **raxref, int defrag_data, raxDefragFunction *element_cb, void *element_cb_data) {
    raxIterator ri;
    rax* rax;
    if ((rax = activeDefragAlloc(*raxref)))
        *raxref = rax;
    rax = *raxref;
    raxStart(&ri,rax);
    ri.node_cb = defragRaxNode;
    defragRaxNode(&rax->head, NULL);
    raxSeek(&ri,"^",NULL,0);
    while (raxNext(&ri)) {
        void *newdata = NULL;
        if (element_cb)
            newdata = element_cb(&ri, element_cb_data);
        if (defrag_data && !newdata)
            newdata = activeDefragAlloc(ri.data);
        if (newdata)
            raxSetData(ri.node, ri.data=newdata);
    }
    raxStop(&ri);
}

typedef struct {
    streamCG *cg;
    streamConsumer *c;
} PendingEntryContext;

void* defragStreamConsumerPendingEntry(raxIterator *ri, void *privdata) {
    PendingEntryContext *ctx = privdata;
    streamNACK *nack = ri->data, *newnack;
    nack->consumer = ctx->c; /* update nack pointer to consumer */
    nack->cgroup_ref_node->value = ctx->cg; /* Update the value of cgroups_ref node to the consumer group. */
    newnack = activeDefragAlloc(nack);
    if (newnack) {
        /* update consumer group pointer to the nack */
        void *prev;
        raxInsert(ctx->cg->pel, ri->key, ri->key_len, newnack, &prev);
        serverAssert(prev==nack);
    }
    return newnack;
}

void* defragStreamConsumer(raxIterator *ri, void *privdata) {
    streamConsumer *c = ri->data;
    streamCG *cg = privdata;
    void *newc = activeDefragAlloc(c);
    if (newc) {
        c = newc;
    }
    sds newsds = activeDefragSds(c->name);
    if (newsds)
        c->name = newsds;
    if (c->pel) {
        PendingEntryContext pel_ctx = {cg, c};
        defragRadixTree(&c->pel, 0, defragStreamConsumerPendingEntry, &pel_ctx);
    }
    return newc; /* returns NULL if c was not defragged */
}

void* defragStreamConsumerGroup(raxIterator *ri, void *privdata) {
    streamCG *newcg, *cg = ri->data;
    UNUSED(privdata);
    if ((newcg = activeDefragAlloc(cg)))
        cg = newcg;
    if (cg->consumers)
        defragRadixTree(&cg->consumers, 0, defragStreamConsumer, cg);
    if (cg->pel)
        defragRadixTree(&cg->pel, 0, NULL, NULL);
    return cg;
}

void defragStream(defragKeysCtx *ctx, kvobj *ob) {
    serverAssert(ob->type == OBJ_STREAM && ob->encoding == OBJ_ENCODING_STREAM);
    stream *s = ob->ptr, *news;

    /* handle the main struct */
    if ((news = activeDefragAlloc(s)))
        ob->ptr = s = news;

    if (raxSize(s->rax) > server.active_defrag_max_scan_fields) {
        rax *newrax = activeDefragAlloc(s->rax);
        if (newrax)
            s->rax = newrax;
        defragLater(ctx, ob);
    } else
        defragRadixTree(&s->rax, 1, NULL, NULL);

    if (s->cgroups)
        defragRadixTree(&s->cgroups, 0, defragStreamConsumerGroup, NULL);
}

/* Defrag a module key. This is either done immediately or scheduled
 * for later. Returns then number of pointers defragged.
 */
void defragModule(defragKeysCtx *ctx, redisDb *db, kvobj *kv) {
    serverAssert(kv->type == OBJ_MODULE);
    robj keyobj;
    initStaticStringObject(keyobj, kvobjGetKey(kv));
    if (!moduleDefragValue(&keyobj, kv, db->id))
        defragLater(ctx, kv);
}

/* for each key we scan in the main dict, this function will attempt to defrag
 * all the various pointers it has. */
void defragKey(defragKeysCtx *ctx, dictEntry *de, dictEntryLink link) {
    UNUSED(link);
    dictEntryLink exlink = NULL;
    kvobj *kvnew = NULL, *ob = dictGetKV(de);
    redisDb *db = &server.db[ctx->dbid];
    int slot = ctx->kvstate.slot;
    unsigned char *newzl;
    
    long long expire = kvobjGetExpire(ob);
    /* We can't search in db->expires for that KV after we've released
     * the pointer it holds, since it won't be able to do the string
     * compare. Search it before, if needed. */ 
     if (expire != -1) {
         exlink = kvstoreDictFindLink(db->expires, slot, kvobjGetKey(ob), NULL);
         serverAssert(exlink != NULL);
     }

    /* Try to defrag robj and/or string value. For hash objects with HFEs,
     * defer defragmentation until processing db's hexpires. */
    if (!(ob->type == OBJ_HASH && hashTypeGetMinExpire(ob, 0) != EB_EXPIRE_TIME_INVALID)) {
        /* If the dict doesn't have metadata, we directly defrag it. */
        kvnew = activeDefragStringOb(ob);
    }
    if (kvnew) {
        kvstoreDictSetAtLink(db->keys, slot, kvnew, &link, 0);
        if (expire != -1)
            kvstoreDictSetAtLink(db->expires, slot, kvnew, &exlink, 0);
        ob = kvnew;
    }

    if (ob->type == OBJ_STRING) {
        /* Already handled in activeDefragStringOb. */
    } else if (ob->type == OBJ_LIST) {
        if (ob->encoding == OBJ_ENCODING_QUICKLIST) {
            defragQuicklist(ctx, ob);
        } else if (ob->encoding == OBJ_ENCODING_LISTPACK) {
            if ((newzl = activeDefragAlloc(ob->ptr)))
                ob->ptr = newzl;
        } else {
            serverPanic("Unknown list encoding");
        }
    } else if (ob->type == OBJ_SET) {
        if (ob->encoding == OBJ_ENCODING_HT) {
            defragSet(ctx, ob);
        } else if (ob->encoding == OBJ_ENCODING_INTSET ||
                   ob->encoding == OBJ_ENCODING_LISTPACK)
        {
            void *newptr, *ptr = ob->ptr;
            if ((newptr = activeDefragAlloc(ptr)))
                ob->ptr = newptr;
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (ob->type == OBJ_ZSET) {
        if (ob->encoding == OBJ_ENCODING_LISTPACK) {
            if ((newzl = activeDefragAlloc(ob->ptr)))
                ob->ptr = newzl;
        } else if (ob->encoding == OBJ_ENCODING_SKIPLIST) {
            defragZsetSkiplist(ctx, ob);
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else if (ob->type == OBJ_HASH) {
        if (ob->encoding == OBJ_ENCODING_LISTPACK) {
            if ((newzl = activeDefragAlloc(ob->ptr)))
                ob->ptr = newzl;
        } else if (ob->encoding == OBJ_ENCODING_LISTPACK_EX) {
            listpackEx *newlpt, *lpt = (listpackEx*)ob->ptr;
            if ((newlpt = activeDefragAlloc(lpt)))
                ob->ptr = lpt = newlpt;
            if ((newzl = activeDefragAlloc(lpt->lp)))
                lpt->lp = newzl;
        } else if (ob->encoding == OBJ_ENCODING_HT) {
            defragHash(ctx, ob);
        } else {
            serverPanic("Unknown hash encoding");
        }
    } else if (ob->type == OBJ_STREAM) {
        defragStream(ctx, ob);
    } else if (ob->type == OBJ_MODULE) {
        defragModule(ctx,db, ob);
    } else {
        serverPanic("Unknown object type");
    }
}

/* Defrag scan callback for the main db dictionary. */
static void dbKeysScanCallback(void *privdata, const dictEntry *de, dictEntryLink plink) {
    long long hits_before = server.stat_active_defrag_hits;
    defragKey((defragKeysCtx *)privdata, (dictEntry *)de, plink);
    if (server.stat_active_defrag_hits != hits_before)
        server.stat_active_defrag_key_hits++;
    else
        server.stat_active_defrag_key_misses++;
    server.stat_active_defrag_scanned++;
}

/* Utility function to get the fragmentation ratio from jemalloc.
 * It is critical to do that by comparing only heap maps that belong to
 * jemalloc, and skip ones the jemalloc keeps as spare. Since we use this
 * fragmentation ratio in order to decide if a defrag action should be taken
 * or not, a false detection can cause the defragmenter to waste a lot of CPU
 * without the possibility of getting any results. */
float getAllocatorFragmentation(size_t *out_frag_bytes) {
    size_t resident, active, allocated, frag_smallbins_bytes;
    zmalloc_get_allocator_info(1, &allocated, &active, &resident, NULL, NULL, &frag_smallbins_bytes);

    if (server.lua_arena != UINT_MAX) {
        size_t lua_resident, lua_active, lua_allocated, lua_frag_smallbins_bytes;
        zmalloc_get_allocator_info_by_arena(server.lua_arena, 0, &lua_allocated, &lua_active, &lua_resident, &lua_frag_smallbins_bytes);
        resident -= lua_resident;
        active -= lua_active;
        allocated -= lua_allocated;
        frag_smallbins_bytes -= lua_frag_smallbins_bytes;
    }

    /* Calculate the fragmentation ratio as the proportion of wasted memory in small
     * bins (which are defraggable) relative to the total allocated memory (including large bins).
     * This is because otherwise, if most of the memory usage is large bins, we may show high percentage,
     * despite the fact it's not a lot of memory for the user. */
    float frag_pct = (float)frag_smallbins_bytes / allocated * 100;
    float rss_pct = ((float)resident / allocated)*100 - 100;
    size_t rss_bytes = resident - allocated;
    if(out_frag_bytes)
        *out_frag_bytes = frag_smallbins_bytes;
    serverLog(LL_DEBUG,
        "allocated=%zu, active=%zu, resident=%zu, frag=%.2f%% (%.2f%% rss), frag_bytes=%zu (%zu rss)",
        allocated, active, resident, frag_pct, rss_pct, frag_smallbins_bytes, rss_bytes);
    return frag_pct;
}

/* Defrag scan callback for the pubsub dictionary. */
void defragPubsubScanCallback(void *privdata, const dictEntry *de, dictEntryLink plink) {
    UNUSED(plink);
    defragPubSubCtx *ctx = privdata;
    kvstore *pubsub_channels = ctx->kvstate.kvs;
    robj *newchannel, *channel = dictGetKey(de);
    dict *newclients, *clients = dictGetVal(de);

    /* Try to defrag the channel name. */
    serverAssert(channel->refcount == (int)dictSize(clients) + 1);
    newchannel = activeDefragStringObEx(channel, dictSize(clients) + 1);
    if (newchannel) {
        kvstoreDictSetKey(pubsub_channels, ctx->kvstate.slot, (dictEntry*)de, newchannel);

        /* The channel name is shared by the client's pubsub(shard) and server's
         * pubsub(shard), after defraging the channel name, we need to update
         * the reference in the clients' dictionary. */
        dictIterator di;
        dictEntry *clientde;
        dictInitIterator(&di, clients);
        while((clientde = dictNext(&di)) != NULL) {
            client *c = dictGetKey(clientde);
            dict *client_channels = ctx->getPubSubChannels(c);
            uint64_t hash = dictGetHash(client_channels, newchannel);
            dictEntry *pubsub_channel = dictFindByHashAndPtr(client_channels, channel, hash);
            serverAssert(pubsub_channel);
            dictSetKey(ctx->getPubSubChannels(c), pubsub_channel, newchannel);
        }
        dictResetIterator(&di);
    }

    /* Try to defrag the dictionary of clients that is stored as the value part. */
    if ((newclients = dictDefragTables(clients)))
        kvstoreDictSetVal(pubsub_channels, ctx->kvstate.slot, (dictEntry *)de, newclients);

    server.stat_active_defrag_scanned++;
}

/* returns 0 more work may or may not be needed (see non-zero cursor),
 * and 1 if time is up and more work is needed. */
int defragLaterItem(kvobj *ob, unsigned long *cursor, monotime endtime, int dbid) {
    if (ob) {
        if (ob->type == OBJ_LIST && ob->encoding == OBJ_ENCODING_QUICKLIST) {
            return scanLaterList(ob, cursor, endtime);
        } else if (ob->type == OBJ_SET && ob->encoding == OBJ_ENCODING_HT) {
            scanLaterSet(ob, cursor);
        } else if (ob->type == OBJ_ZSET && ob->encoding == OBJ_ENCODING_SKIPLIST) {
            scanLaterZset(ob, cursor);
        } else if (ob->type == OBJ_HASH && ob->encoding == OBJ_ENCODING_HT) {
            scanLaterHash(ob, cursor);
        } else if (ob->type == OBJ_STREAM && ob->encoding == OBJ_ENCODING_STREAM) {
            return scanLaterStreamListpacks(ob, cursor, endtime);
        } else if (ob->type == OBJ_MODULE) {
            robj keyobj;
            initStaticStringObject(keyobj, kvobjGetKey(ob));
            return moduleLateDefrag(&keyobj, ob, cursor, endtime, dbid);
        } else {
            *cursor = 0; /* object type/encoding may have changed since we schedule it for later */
        }
    } else {
        *cursor = 0; /* object may have been deleted already */
    }
    return 0;
}

static int defragIsRunning(void) {
    return (defrag.timeproc_id > 0);
}

/* A kvstoreHelperPreContinueFn */
static doneStatus defragLaterStep(void *ctx, monotime endtime) {
    defragKeysCtx *defrag_keys_ctx = ctx;

    unsigned int iterations = 0;
    unsigned long long prev_defragged = server.stat_active_defrag_hits;
    unsigned long long prev_scanned = server.stat_active_defrag_scanned;

    while (defrag_keys_ctx->defrag_later && listLength(defrag_keys_ctx->defrag_later) > 0) {
        listNode *head = listFirst(defrag_keys_ctx->defrag_later);
        sds key = head->value;
        dictEntry *de = kvstoreDictFind(defrag_keys_ctx->kvstate.kvs, defrag_keys_ctx->kvstate.slot, key);
        kvobj *kv = de ? dictGetKV(de) : NULL;

        long long key_defragged = server.stat_active_defrag_hits;
        int timeout = (defragLaterItem(kv, &defrag_keys_ctx->defrag_later_cursor, endtime, defrag_keys_ctx->dbid) == 1);
        if (key_defragged != server.stat_active_defrag_hits) {
            server.stat_active_defrag_key_hits++;
        } else {
            server.stat_active_defrag_key_misses++;
        }

        if (timeout) break;

        if (defrag_keys_ctx->defrag_later_cursor == 0) {
            /* the item is finished, move on */
            listDelNode(defrag_keys_ctx->defrag_later, head);
        }

        if (++iterations > 16 || server.stat_active_defrag_hits - prev_defragged > 512 ||
            server.stat_active_defrag_scanned - prev_scanned > 64) {
            if (getMonotonicUs() > endtime) break;
            iterations = 0;
            prev_defragged = server.stat_active_defrag_hits;
            prev_scanned = server.stat_active_defrag_scanned;
        }
    }

    return (!defrag_keys_ctx->defrag_later || listLength(defrag_keys_ctx->defrag_later) == 0) ? DEFRAG_DONE : DEFRAG_NOT_DONE;
}

#define INTERPOLATE(x, x1, x2, y1, y2) ( (y1) + ((x)-(x1)) * ((y2)-(y1)) / ((x2)-(x1)) )
#define LIMIT(y, min, max) ((y)<(min)? min: ((y)>(max)? max: (y)))

/* decide if defrag is needed, and at what CPU effort to invest in it */
void computeDefragCycles(void) {
    size_t frag_bytes;
    float frag_pct = getAllocatorFragmentation(&frag_bytes);
    /* If we're not already running, and below the threshold, exit. */
    if (!server.active_defrag_running) {
        if(frag_pct < server.active_defrag_threshold_lower || frag_bytes < server.active_defrag_ignore_bytes)
            return;
    }

    /* Calculate the adaptive aggressiveness of the defrag based on the current
     * fragmentation and configurations. */
    int cpu_pct = INTERPOLATE(frag_pct,
            server.active_defrag_threshold_lower,
            server.active_defrag_threshold_upper,
            server.active_defrag_cycle_min,
            server.active_defrag_cycle_max);
    cpu_pct *= defrag.decay_rate;
    cpu_pct = LIMIT(cpu_pct,
            server.active_defrag_cycle_min,
            server.active_defrag_cycle_max);

    /* Normally we allow increasing the aggressiveness during a scan, but don't
     * reduce it, since we should not lower the aggressiveness when fragmentation
     * drops. But when a configuration is made, we should reconsider it. */
    if (cpu_pct > server.active_defrag_running ||
        server.active_defrag_configuration_changed)
    {
        server.active_defrag_configuration_changed = 0;
        if (defragIsRunning()) {
            serverLog(LL_VERBOSE, "Changing active defrag CPU, frag=%.0f%%, frag_bytes=%zu, cpu=%d%%",
                      frag_pct, frag_bytes, cpu_pct);
        } else {
            serverLog(LL_VERBOSE,
                "Starting active defrag, frag=%.0f%%, frag_bytes=%zu, cpu=%d%%",
                frag_pct, frag_bytes, cpu_pct);
        }
        server.active_defrag_running = cpu_pct;
    }
}

/* This helper function handles most of the work for iterating over a kvstore. 'privdata', if
 * provided, MUST begin with 'kvstoreIterState' and this part is automatically updated by this
 * function during the iteration. */
static doneStatus defragStageKvstoreHelper(monotime endtime,
                                           void *ctx,
                                           dictScanFunction scan_fn,
                                           kvstoreHelperPreContinueFn precontinue_fn,
                                           dictDefragFunctions *defragfns)
{
    unsigned int iterations = 0;
    unsigned long long prev_defragged = server.stat_active_defrag_hits;
    unsigned long long prev_scanned = server.stat_active_defrag_scanned;
    kvstoreIterState *state = (kvstoreIterState*)ctx;

    if (state->slot == KVS_SLOT_DEFRAG_LUT) {
        /* Before we start scanning the kvstore, handle the main structures */
        do {
            state->cursor = kvstoreDictLUTDefrag(state->kvs, state->cursor, dictDefragTables);
            if (getMonotonicUs() >= endtime) return DEFRAG_NOT_DONE;
        } while (state->cursor != 0);
        state->slot = KVS_SLOT_UNASSIGNED;
    }

    while (1) {
        if (++iterations > 16 || server.stat_active_defrag_hits - prev_defragged > 512 || server.stat_active_defrag_scanned - prev_scanned > 64) {
            if (getMonotonicUs() >= endtime) break;
            iterations = 0;
            prev_defragged = server.stat_active_defrag_hits;
            prev_scanned = server.stat_active_defrag_scanned;
        }

        if (precontinue_fn) {
            if (precontinue_fn(ctx, endtime) == DEFRAG_NOT_DONE) return DEFRAG_NOT_DONE;
        }

        if (!state->cursor) {
            /* If there's no cursor, we're ready to begin a new kvstore slot. */
            if (state->slot == KVS_SLOT_UNASSIGNED) {
                state->slot = kvstoreGetFirstNonEmptyDictIndex(state->kvs);
            } else {
                state->slot = kvstoreGetNextNonEmptyDictIndex(state->kvs, state->slot);
            }

            if (state->slot == KVS_SLOT_UNASSIGNED) return DEFRAG_DONE;
        }

        /* Whatever privdata's actual type, this function requires that it begins with kvstoreIterState. */
        state->cursor = kvstoreDictScanDefrag(state->kvs, state->slot, state->cursor,
                                             scan_fn, defragfns, ctx);
    }

    return DEFRAG_NOT_DONE;
}

static doneStatus defragStageDbKeys(void *ctx, monotime endtime) {
    defragKeysCtx *defrag_keys_ctx = ctx;
    redisDb *db = &server.db[defrag_keys_ctx->dbid];
    if (db->keys != defrag_keys_ctx->kvstate.kvs) {
        /* There has been a change of the kvs (flushdb, swapdb, etc.). Just complete the stage. */
        return DEFRAG_DONE;
    }

    /* Note: for DB keys, we use the start/finish callback to fix an expires table entry if
     * the main DB entry has been moved. */
    static dictDefragFunctions defragfns = {
        .defragAlloc = activeDefragAlloc,
        .defragKey = NULL, /* Handled by dbKeysScanCallback */
        .defragVal = NULL, /* Handled by dbKeysScanCallback */
    };

    return defragStageKvstoreHelper(endtime, ctx,
        dbKeysScanCallback, defragLaterStep, &defragfns);
}

static doneStatus defragStageExpiresKvstore(void *ctx, monotime endtime) {
    defragKeysCtx *defrag_keys_ctx = ctx;
    redisDb *db = &server.db[defrag_keys_ctx->dbid];
    if (db->expires != defrag_keys_ctx->kvstate.kvs) {
        /* There has been a change of the kvs (flushdb, swapdb, etc.). Just complete the stage. */
        return DEFRAG_DONE;
    }

    static dictDefragFunctions defragfns = {
        .defragAlloc = activeDefragAlloc,
        .defragKey = NULL, /* Not needed for expires (just a ref) */
        .defragVal = NULL, /* Not needed for expires (no value) */
    };
    return defragStageKvstoreHelper(endtime, ctx,
        scanCallbackCountScanned, NULL, &defragfns);
}

/* Defragment hash object with HFE and update its reference in the DB keys. */
void *activeDefragHExpiresOB(void *ptr, void *privdata) {
    redisDb *db = privdata;
    dictEntryLink link, exlink = NULL;
    kvobj *newkv, *kvobj = ptr;
    sds keystr = kvobjGetKey(kvobj);
    unsigned int slot = calculateKeySlot(keystr);
    serverAssert(kvobj->type == OBJ_HASH);

    long long expire = kvobjGetExpire(kvobj);
    /* We can't search in db->expires for that KV after we've released
     * the pointer it holds, since it won't be able to do the string
     * compare. Search it before, if needed. */
    if (expire != -1) {
        exlink = kvstoreDictFindLink(db->expires, slot, kvobjGetKey(kvobj), NULL);
        serverAssert(exlink != NULL);
    }

    if ((newkv = activeDefragAllocWithoutFree(kvobj))) {
        /* Update its reference in the DB keys. */
        link = kvstoreDictFindLink(db->keys, slot, keystr, NULL);
        serverAssert(link != NULL);
        kvstoreDictSetAtLink(db->keys, slot, newkv, &link, 0);
        if (expire != -1)
            kvstoreDictSetAtLink(db->expires, slot, newkv, &exlink, 0);
        activeDefragFree(kvobj);
    }
    return newkv;
}

static doneStatus defragStageHExpires(void *ctx, monotime endtime) {
    unsigned int iterations = 0;
    defragHExpiresCtx *defrag_hexpires_ctx = ctx;
    redisDb *db = &server.db[defrag_hexpires_ctx->dbid];
    if (db->hexpires != defrag_hexpires_ctx->hexpires) {
        /* There has been a change of the kvs (flushdb, swapdb, etc.). Just complete the stage. */
        return DEFRAG_DONE;
    }

    ebDefragFunctions eb_defragfns = {
        .defragAlloc = activeDefragAlloc,
        .defragItem = activeDefragHExpiresOB
    };
    while (1) {
        if (!ebScanDefrag(&db->hexpires, &hashExpireBucketsType, &defrag_hexpires_ctx->cursor, &eb_defragfns, db))
            return DEFRAG_DONE;

        if (++iterations > 16) {
            if (getMonotonicUs() >= endtime) break;
            iterations = 0;
        }
    }

    return DEFRAG_NOT_DONE;
}

static doneStatus defragStagePubsubKvstore(void *ctx, monotime endtime) {
    static dictDefragFunctions defragfns = {
        .defragAlloc = activeDefragAlloc,
        .defragKey = NULL, /* Handled by defragPubsubScanCallback */
        .defragVal = NULL, /* Not needed for expires (no value) */
    };

    return defragStageKvstoreHelper(endtime, ctx,
        defragPubsubScanCallback, NULL, &defragfns);
}

static doneStatus defragLuaScripts(void *ctx, monotime endtime) {
    UNUSED(endtime);
    UNUSED(ctx);
    activeDefragSdsDict(evalScriptsDict(), DEFRAG_SDS_DICT_VAL_LUA_SCRIPT);
    return DEFRAG_DONE;
}

/* Handles defragmentation of module global data. This is a stage function
 * that gets called periodically during the active defragmentation process. */
static doneStatus defragModuleGlobals(void *ctx, monotime endtime) {
    defragModuleCtx *defrag_module_ctx = ctx;

    RedisModule *module = moduleGetHandleByName(defrag_module_ctx->module_name);
    if (!module) {
        /* Module has been unloaded, nothing to defrag. */
        return DEFRAG_DONE;
    }
    /* Interval shouldn't exceed 1 hour  */
    serverAssert(!endtime || llabs((long long)endtime - (long long)getMonotonicUs()) < 60*60*1000*1000LL);

    /* Call appropriate version of module's defrag callback:
     * 1. Version 2 (defrag_cb_2): Supports incremental defrag and returns whether more work is needed
     * 2. Version 1 (defrag_cb): Legacy version, performs all work in one call.
     *    Note: V1 doesn't support incremental defragmentation, may block for longer periods. */
    RedisModuleDefragCtx defrag_ctx = { endtime, &defrag_module_ctx->cursor, NULL, -1, -1, -1 };
    if (module->defrag_cb_2) {
        return module->defrag_cb_2(&defrag_ctx) ? DEFRAG_NOT_DONE : DEFRAG_DONE;
    } else if (module->defrag_cb) {
        module->defrag_cb(&defrag_ctx);
        return DEFRAG_DONE;
    } else {
        redis_unreachable();
    }
}

static void freeDefragKeysContext(void *ctx) {
    defragKeysCtx *defrag_keys_ctx = ctx;
    if (defrag_keys_ctx->defrag_later) {
        listRelease(defrag_keys_ctx->defrag_later);
    }
    zfree(defrag_keys_ctx);
}

static void freeDefragModelContext(void *ctx) {
    defragModuleCtx *defrag_model_ctx = ctx;
    sdsfree(defrag_model_ctx->module_name);
    zfree(defrag_model_ctx);
}

static void freeDefragContext(void *ptr) {
    StageDescriptor *stage = ptr;
    if (stage->ctx_free_fn)
        stage->ctx_free_fn(stage->ctx);
    zfree(stage);
}

static void addDefragStage(defragStageFn stage_fn, defragStageContextFreeFn ctx_free_fn, void *ctx) {
    StageDescriptor *stage = zmalloc(sizeof(StageDescriptor));
    stage->stage_fn = stage_fn;
    stage->ctx_free_fn = ctx_free_fn;
    stage->ctx = ctx;
    listAddNodeTail(defrag.remaining_stages, stage);
}

/* Updates the defrag decay rate based on the observed effectiveness of the defrag process.
 * The decay rate is used to gradually slow down defrag when it's not being effective. */
static void updateDefragDecayRate(float frag_pct) {
    long long last_hits = server.stat_active_defrag_hits - defrag.start_defrag_hits;
    long long last_misses = server.stat_active_defrag_misses - defrag.start_defrag_misses;
    float last_frag_pct_change = defrag.start_frag_pct - frag_pct;
    /* When defragmentation efficiency is low, we gradually reduce the
     * speed for the next cycle to avoid CPU waste. However, in the
     * following two cases, we keep the normal speed:
     * 1) If the fragmentation percentage has increased or decreased by more than 2%.
     * 2) If the fragmentation percentage decrease is small, but hits are above 1%,
     *    we still keep the normal speed. */
    if (fabs(last_frag_pct_change) > 2 ||
        (last_frag_pct_change < 0 && last_hits >= (last_hits + last_misses) * 0.01))
    {
        defrag.decay_rate = 1.0f;
    } else {
        defrag.decay_rate *= 0.9;
    }
}

/* Called at the end of a complete defrag cycle, or when defrag is terminated */
static void endDefragCycle(int normal_termination) {
    if (normal_termination) {
        /* For normal termination, we expect... */
        serverAssert(!defrag.current_stage);
        serverAssert(listLength(defrag.remaining_stages) == 0);
    } else {
        /* Defrag is being terminated abnormally */
        aeDeleteTimeEvent(server.el, defrag.timeproc_id);

        if (defrag.current_stage) {
            listDelNode(defrag.remaining_stages, defrag.current_stage);
            defrag.current_stage = NULL;
        }
    }
    defrag.timeproc_id = AE_DELETED_EVENT_ID;

    listRelease(defrag.remaining_stages);
    defrag.remaining_stages = NULL;

    size_t frag_bytes;
    float frag_pct = getAllocatorFragmentation(&frag_bytes);
    serverLog(LL_VERBOSE, "Active defrag done in %dms, reallocated=%d, frag=%.0f%%, frag_bytes=%zu",
              (int)elapsedMs(defrag.start_cycle), (int)(server.stat_active_defrag_hits - defrag.start_defrag_hits),
              frag_pct, frag_bytes);

    server.stat_total_active_defrag_time += elapsedUs(server.stat_last_active_defrag_time);
    server.stat_last_active_defrag_time = 0;
    server.active_defrag_running = 0;

    updateDefragDecayRate(frag_pct);
    moduleDefragEnd();

    /* Immediately check to see if we should start another defrag cycle. */
    activeDefragCycle();
}

/* Must be called at the start of the timeProc as it measures the delay from the end of the previous
 * timeProc invocation when performing the computation. */
static int computeDefragCycleUs(void) {
    long dutyCycleUs;

    int targetCpuPercent = server.active_defrag_running;
    serverAssert(targetCpuPercent > 0 && targetCpuPercent < 100);

    static int prevCpuPercent = 0; /* STATIC - this persists */
    if (targetCpuPercent != prevCpuPercent) {
        /* If the targetCpuPercent changes, the value might be different from when the last wait
         * time was computed. In this case, don't consider wait time. (This is really only an
         * issue in crazy tests that dramatically increase CPU while defrag is running.) */
        defrag.timeproc_end_time = 0;
        prevCpuPercent = targetCpuPercent;
    }

    /* Given when the last duty cycle ended, compute time needed to achieve the desired percentage. */
    if (defrag.timeproc_end_time == 0) {
        /* Either the first call to the timeProc, or we were paused for some reason. */
        defrag.timeproc_overage_us = 0;
        dutyCycleUs = DEFRAG_CYCLE_US;
    } else {
        long waitedUs = getMonotonicUs() - defrag.timeproc_end_time;
        /* Given the elapsed wait time between calls, compute the necessary duty time needed to
         * achieve the desired CPU percentage.
         * With:  D = duty time, W = wait time, P = percent
         * Solve:    D          P
         *         -----   =  -----
         *         D + W       100
         * Solving for D:
         *     D = P * W / (100 - P)
         *
         * Note that dutyCycleUs addresses starvation. If the wait time was long, we will compensate
         * with a proportionately long duty-cycle. This won't significantly affect perceived
         * latency, because clients are already being impacted by the long cycle time which caused
         * the starvation of the timer. */
        dutyCycleUs = targetCpuPercent * waitedUs / (100 - targetCpuPercent);

        /* Also adjust for any accumulated overage. */
        dutyCycleUs -= defrag.timeproc_overage_us;
        defrag.timeproc_overage_us = 0;

        if (dutyCycleUs < DEFRAG_CYCLE_US) {
            /* We never reduce our cycle time, that would increase overhead. Instead, we track this
             * as part of the overage, and increase wait time between cycles. */
            defrag.timeproc_overage_us = DEFRAG_CYCLE_US - dutyCycleUs;
            dutyCycleUs = DEFRAG_CYCLE_US;
        } else if (dutyCycleUs > DEFRAG_CYCLE_US * 10) {
            /* Add a time limit for the defrag duty cycle to prevent excessive latency.
             * When latency is already high (indicated by a long time between calls),
             * we don't want to make it worse by running defrag for too long. */
            dutyCycleUs = DEFRAG_CYCLE_US * 10;
        }
    }
    return dutyCycleUs;
}

/* Must be called at the end of the timeProc as it records the timeproc_end_time for use in the next
 * computeDefragCycleUs computation. */
static int computeDelayMs(monotime intendedEndtime) {
    defrag.timeproc_end_time = getMonotonicUs();
    long overage = defrag.timeproc_end_time - intendedEndtime;
    defrag.timeproc_overage_us += overage; /* track over/under desired CPU */
    /* Allow negative overage (underage) to count against existing overage, but don't allow
     * underage (from short stages) to be accumulated. */
    if (defrag.timeproc_overage_us < 0) defrag.timeproc_overage_us = 0;

    int targetCpuPercent = server.active_defrag_running;
    serverAssert(targetCpuPercent > 0 && targetCpuPercent < 100);

    /* Given the desired duty cycle, what inter-cycle delay do we need to achieve that? */
    /* We want to achieve a specific CPU percent. To do that, we can't use a skewed computation. */
    /* Example, if we run for 1ms and delay 10ms, that's NOT 10%, because the total cycle time is 11ms. */
    /* Instead, if we rum for 1ms, our total time should be 10ms. So the delay is only 9ms. */
    long totalCycleTimeUs = DEFRAG_CYCLE_US * 100 / targetCpuPercent;
    long delayUs = totalCycleTimeUs - DEFRAG_CYCLE_US;
    /* Only increase delay by the fraction of the overage that would be non-duty-cycle */
    delayUs += defrag.timeproc_overage_us * (100 - targetCpuPercent) / 100;
    if (delayUs < 0) delayUs = 0;
    long delayMs = delayUs / 1000; /* round down */
    return delayMs;
}

/* An independent time proc for defrag. While defrag is running, this is called much more often
 * than the server cron. Frequent short calls provides low latency impact. */
static int activeDefragTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    /* This timer shouldn't be registered unless there's work to do. */
    serverAssert(defrag.current_stage || listLength(defrag.remaining_stages) > 0);

    if (!server.active_defrag_enabled) {
        /* Defrag has been disabled while running */
        endDefragCycle(0);
        return AE_NOMORE;
    }

    if (hasActiveChildProcess()) {
        /* If there's a child process, pause the defrag, polling until the child completes. */
        defrag.timeproc_end_time = 0; /* prevent starvation recovery */
        return 100;
    }

    monotime starttime = getMonotonicUs();
    int dutyCycleUs = computeDefragCycleUs();
    monotime endtime = starttime + dutyCycleUs;
    int haveMoreWork = 1;

    mstime_t latency;
    latencyStartMonitor(latency);

    do {
        if (!defrag.current_stage) {
            defrag.current_stage = listFirst(defrag.remaining_stages);
        }

        StageDescriptor *stage = listNodeValue(defrag.current_stage);
        doneStatus status = stage->stage_fn(stage->ctx, endtime);
        if (status == DEFRAG_DONE) {
            listDelNode(defrag.remaining_stages, defrag.current_stage);
            defrag.current_stage = NULL;
        }

        haveMoreWork = (defrag.current_stage || listLength(defrag.remaining_stages) > 0);
        /* If we've completed a stage early, and still have a standard time allotment remaining,
         * we'll start another stage. This can happen when defrag is running infrequently, and
         * starvation protection has increased the duty-cycle. */
    } while (haveMoreWork && getMonotonicUs() <= endtime - DEFRAG_CYCLE_US);

    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("active-defrag-cycle", latency);

    if (haveMoreWork) {
        return computeDelayMs(endtime);
    } else {
        endDefragCycle(1);
        return AE_NOMORE; /* Ends the timer proc */
    }
}

/* During long running scripts, or while loading, there is a periodic function for handling other
 * actions. This interface allows defrag to continue running, avoiding a single long defrag step
 * after the long operation completes. */
void defragWhileBlocked(void) {
    /* This is called infrequently, while timers are not active. We might need to start defrag. */
    if (!defragIsRunning()) activeDefragCycle();

    if (!defragIsRunning()) return;

    /* Save off the timeproc_id. If we have a normal termination, it will be cleared. */
    long long timeproc_id = defrag.timeproc_id;

    /* Simulate a single call of the timer proc */
    long long reschedule_delay = activeDefragTimeProc(NULL, 0, NULL);
    if (reschedule_delay == AE_NOMORE) {
        /* If it's done, deregister the timer */
        aeDeleteTimeEvent(server.el, timeproc_id);
    }
    /* Otherwise, just ignore the reschedule_delay, the timer will pop the next time that the
     * event loop can process timers again. */
}

static void beginDefragCycle(void) {
    serverAssert(!defragIsRunning());

    moduleDefragStart();

    serverAssert(defrag.remaining_stages == NULL);
    defrag.remaining_stages = listCreate();
    listSetFreeMethod(defrag.remaining_stages, freeDefragContext);

    for (int dbid = 0; dbid < server.dbnum; dbid++) {
        redisDb *db = &server.db[dbid];

        /* Add stage for keys. */
        defragKeysCtx *defrag_keys_ctx = zcalloc(sizeof(defragKeysCtx));
        defrag_keys_ctx->kvstate = INIT_KVSTORE_STATE(db->keys);
        defrag_keys_ctx->dbid = dbid;
        addDefragStage(defragStageDbKeys, freeDefragKeysContext, defrag_keys_ctx);

        /* Add stage for expires. */
        defragKeysCtx *defrag_expires_ctx = zcalloc(sizeof(defragKeysCtx));
        defrag_expires_ctx->kvstate = INIT_KVSTORE_STATE(db->expires);
        defrag_expires_ctx->dbid = dbid;
        addDefragStage(defragStageExpiresKvstore, freeDefragKeysContext, defrag_expires_ctx);

        /* Add stage for hexpires. */
        defragHExpiresCtx *defrag_hexpires_ctx = zcalloc(sizeof(defragHExpiresCtx));
        defrag_hexpires_ctx->hexpires = db->hexpires;
        defrag_hexpires_ctx->dbid = dbid;
        addDefragStage(defragStageHExpires, zfree, defrag_hexpires_ctx);
    }

    /* Add stage for pubsub channels. */
    defragPubSubCtx *defrag_pubsub_ctx = zmalloc(sizeof(defragPubSubCtx));
    defrag_pubsub_ctx->kvstate = INIT_KVSTORE_STATE(server.pubsub_channels);
    defrag_pubsub_ctx->getPubSubChannels = getClientPubSubChannels;
    addDefragStage(defragStagePubsubKvstore, zfree, defrag_pubsub_ctx);

    /* Add stage for pubsubshard channels. */
    defragPubSubCtx *defrag_pubsubshard_ctx = zmalloc(sizeof(defragPubSubCtx));
    defrag_pubsubshard_ctx->kvstate = INIT_KVSTORE_STATE(server.pubsubshard_channels);
    defrag_pubsubshard_ctx->getPubSubChannels = getClientPubSubShardChannels;
    addDefragStage(defragStagePubsubKvstore, zfree, defrag_pubsubshard_ctx);

    addDefragStage(defragLuaScripts, NULL, NULL);

    /* Add stages for modules. */
    dictIterator di;
    dictEntry *de;
    dictInitIterator(&di, modules);
    while ((de = dictNext(&di)) != NULL) {
        struct RedisModule *module = dictGetVal(de);
        if (module->defrag_cb || module->defrag_cb_2) {
            defragModuleCtx *ctx = zmalloc(sizeof(defragModuleCtx));
            ctx->cursor = 0;
            ctx->module_name = sdsnew(module->name);
            addDefragStage(defragModuleGlobals, freeDefragModelContext, ctx);
        }
    }
    dictResetIterator(&di);

    defrag.current_stage = NULL;
    defrag.start_cycle = getMonotonicUs();
    defrag.start_defrag_hits = server.stat_active_defrag_hits;
    defrag.start_defrag_misses = server.stat_active_defrag_misses;
    defrag.start_frag_pct = getAllocatorFragmentation(NULL);
    defrag.timeproc_end_time = 0;
    defrag.timeproc_overage_us = 0;
    defrag.timeproc_id = aeCreateTimeEvent(server.el, 0, activeDefragTimeProc, NULL, NULL);

    elapsedStart(&server.stat_last_active_defrag_time);
}

void activeDefragCycle(void) {
    if (!server.active_defrag_enabled) return;

    /* Defrag gets paused while a child process is active. So there's no point in starting a new
     * cycle or adjusting the CPU percentage for an existing cycle. */
    if (hasActiveChildProcess()) return;

    computeDefragCycles();

    if (server.active_defrag_running > 0 && !defragIsRunning()) beginDefragCycle();
}

#else /* HAVE_DEFRAG */

void activeDefragCycle(void) {
    /* Not implemented yet. */
}

void *activeDefragAlloc(void *ptr) {
    UNUSED(ptr);
    return NULL;
}

void *activeDefragAllocRaw(size_t size) {
    /* fallback to regular allocation */
    return zmalloc(size);
}

void activeDefragFreeRaw(void *ptr) {
    /* fallback to regular free */
    zfree(ptr);
}

robj *activeDefragStringOb(robj *ob) {
    UNUSED(ob);
    return NULL;
}

void defragWhileBlocked(void) {
}

#endif

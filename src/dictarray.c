#include "fmacros.h"

#include <string.h>
#include <stddef.h>

#include "zmalloc.h"
#include "dictarray.h"
#include "redisassert.h"
#include "monotonic.h"

/**********************************/
/*** Helpers **********************/
/**********************************/

/* Returns total (cumulative) number of keys up until given slot (inclusive).
 * Time complexity is O(log(da->num_slots)). */
static unsigned long long cumulativeKeyCountRead(dictarray *da, int slot) {
    if (da->num_slots == 1) {
        assert(slot == 0);
        return daSize(da);
    }
    int idx = slot + 1;
    unsigned long long sum = 0;
    while (idx > 0) {
        sum += da->state.slot_size_index[idx];
        idx -= (idx & -idx);
    }
    return sum;
}

static void addSlotIdToCursor(dictarray *da, int slot, unsigned long long *cursor) {
    if (da->num_slots == 1)
        return;
    /* Slot id can be -1 when iteration is over and there are no more slots to visit. */
    if (slot < 0)
        return;
    *cursor = (*cursor << da->num_slots_bits) | slot;
}

static int getAndClearSlotIdFromCursor(dictarray *da, unsigned long long *cursor) {
    if (da->num_slots == 1)
        return 0;
    int slot = (int) (*cursor & (da->num_slots-1));
    *cursor = *cursor >> da->num_slots_bits;
    return slot;
}

/**********************************/
/*** API **************************/
/**********************************/

/* Create an array of dictionaries */
dictarray *daCreate(dictType *type, int num_slots_bits) {
    dictarray *da = zcalloc(sizeof(*da));
    da->num_slots_bits = num_slots_bits;
    da->num_slots = 1 << da->num_slots_bits;
    da->dicts = zmalloc(sizeof(dict*) * da->num_slots);
    for (int i = 0; i < da->num_slots; i++)
        da->dicts[i] = dictCreate(type);

    da->state.rehashing = listCreate();
    da->state.key_count = 0;
    da->state.non_empty_slots = 0;
    da->state.resize_cursor = -1;
    da->state.slot_size_index = da->num_slots > 1 ? zcalloc(sizeof(unsigned long long) * (da->num_slots + 1)) : NULL;
    da->state.bucket_count = 0;

    return da;
}

void daEmpty(dictarray *da, void(callback)(dict*)) {
    for (int slot = 0; slot < da->num_slots; slot++)
        dictEmpty(daGetDict(da, slot), callback);

    if (da->state.rehashing)
        listEmpty(da->state.rehashing);
    da->state.key_count = 0;
    da->state.non_empty_slots = 0;
    da->state.resize_cursor = -1;
    da->state.bucket_count = -1;
    if (da->state.slot_size_index)
        memset(da->state.slot_size_index, 0, sizeof(unsigned long long) * (da->num_slots + 1));
}

void daRelease(dictarray *da) {
    for (int slot = 0; slot < da->num_slots; slot++)
        dictRelease(daGetDict(da, slot));
    zfree(da->dicts);

    listRelease(da->state.rehashing);
    if (da->state.slot_size_index)
        zfree(da->state.slot_size_index);

    zfree(da);
}

unsigned long long int daSize(dictarray *da) {
    if (da->num_slots != 1) {
        return da->state.key_count;
    } else {
        return dictSize(da->dicts[0]);
    }
}

/* This method provides the cumulative sum of all the dictionary buckets
 * across dictionaries in a database. */
unsigned long daBuckets(dictarray *da) {
    if (da->num_slots != 1) {
        return da->state.bucket_count;
    } else {
        return dictBuckets(da->dicts[0]);
    }
}

size_t daMemUsage(dictarray *da) {
    size_t mem = 0;
    unsigned long long keys_count = daSize(da);
    mem += keys_count * dictEntryMemUsage() +
           daBuckets(da) * sizeof(dictEntry*) +
           da->num_slots * sizeof(dict);
    return mem;
}

dictEntry *daFind(dictarray *da, void *key, int slot) {
    dict *d = daGetDict(da, slot);
    return dictFind(d, key);
}

/*
 * This method is used to iterate over the elements of the entire database specifically across slots.
 * It's a three pronged approach.
 *
 * 1. It uses the provided cursor `v` to retrieve the slot from it.
 * 2. If the dictionary is in a valid state checked through the provided callback `dictScanValidFunction`,
 *    it performs a dictScan over the appropriate `keyType` dictionary of `db`.
 * 3. If the slot is entirely scanned i.e. the cursor has reached 0, the next non empty slot is discovered.
 *    The slot information is embedded into the cursor and returned.
 *
 * To restrict the scan to a single cluster slot, pass a valid slot as
 * 'onlyslot', otherwise pass -1.
 */
unsigned long long daScan(dictarray *da, unsigned long long cursor,
                          int onlyslot, dictScanFunction *scan_cb,
                          dictarrayScanShouldSkipDict *skip_cb,
                          void *privdata)
{
    unsigned long long _cursor = 0;
    /* During main dictionary traversal in cluster mode, 48 lower bits in the cursor are used for positioning in the HT.
     * Following 14 bits are used for the slot number, ranging from 0 to 2^14-1.
     * Slot is always 0 at the start of iteration and can be incremented only in cluster mode. */
    int slot = getAndClearSlotIdFromCursor(da, &cursor);
    if (onlyslot >= 0) {
        if (slot < onlyslot) {
            /* Fast-forward to onlyslot. */
            assert(onlyslot < da->num_slots);
            slot = onlyslot;
            cursor = 0;
        } else if (slot > onlyslot) {
            /* The cursor is already past onlyslot. */
            return 0;
        }
    }

    dict *d = daGetDict(da, slot);

    int skip = skip_cb && skip_cb(d);
    if (!skip) {
        _cursor = dictScan(d, cursor, scan_cb, privdata);
    }
    /* scanning done for the current dictionary or if the scanning wasn't possible, move to the next slot. */
    if (_cursor == 0 || skip) {
        if (onlyslot >= 0)
            return 0;
        slot = daGetNextNonEmptySlot(da, slot);
    }
    if (slot == -1) {
        return 0;
    }
    addSlotIdToCursor(da, slot, &_cursor);
    return _cursor;
}

/*
 * This functions increases size of the main/expires db to match desired number.
 * In cluster mode resizes all individual dictionaries for slots that this node owns.
 *
 * Based on the parameter `try_expand`, appropriate dict expand API is invoked.
 * if try_expand is set to 1, `dictTryExpand` is used else `dictExpand`.
 * The return code is either `DICT_OK`/`DICT_ERR` for both the API(s).
 * `DICT_OK` response is for successful expansion. However ,`DICT_ERR` response signifies failure in allocation in
 * `dictTryExpand` call and in case of `dictExpand` call it signifies no expansion was performed.
 */
int daExpand(dictarray *da, uint64_t newsize, int try_expand, dictarrayExpandShouldSkipSlot *skip_cb) {
    dict *d;
    if (da->num_slots > 1) {
        for (int i = 0; i < da->num_slots; i++) {
            if (!(skip_cb && skip_cb(i))) {
                d = daGetDict(da, i);
                int result = try_expand ? dictTryExpand(d, newsize) : dictExpand(d, newsize);
                if (try_expand && result == DICT_ERR)
                    return 0;
            }
        }
    } else {
        d = daGetDict(da, 0);
        int result = try_expand ? dictTryExpand(d, newsize) : dictExpand(d, newsize);
        if (try_expand && result == DICT_ERR)
            return 0;
    }
    return 1;
}

/* Returns fair random slot, probability of each slot being returned is proportional to the number of elements that slot dictionary holds.
 * This function guarantees that it returns a slot whose dict is non-empty, unless the entire db is empty.
 * Time complexity of this function is O(log(da->num_slots)). */
int daGetFairRandomSlot(dictarray *da) {
    unsigned long target = daSize(da) ? (randomULong() % daSize(da)) + 1 : 0;
    int slot = daFindSlotByKeyIndex(da, target);
    return slot;
}

void daGetStats(dictarray *da, char *buf, size_t bufsize, int full) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;
    dictStats *mainHtStats = NULL;
    dictStats *rehashHtStats = NULL;
    dict *d;
    daIterator *dait = daIteratorInit(da);
    while ((d = daIteratorNextDict(dait))) {
        dictStats *stats = dictGetStatsHt(d, 0, full);
        if (!mainHtStats) {
            mainHtStats = stats;
        } else {
            dictCombineStats(stats, mainHtStats);
            dictFreeStats(stats);
        }
        if (dictIsRehashing(d)) {
            stats = dictGetStatsHt(d, 1, full);
            if (!rehashHtStats) {
                rehashHtStats = stats;
            } else {
                dictCombineStats(stats, rehashHtStats);
                dictFreeStats(stats);
            }
        }
    }
    daReleaseIterator(dait);
    l = dictGetStatsMsg(buf, bufsize, mainHtStats, full);
    dictFreeStats(mainHtStats);
    buf += l;
    bufsize -= l;
    if (rehashHtStats && bufsize > 0) {
        dictGetStatsMsg(buf, bufsize, rehashHtStats, full);
        dictFreeStats(rehashHtStats);
    }
    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize) orig_buf[orig_bufsize - 1] = '\0';
}

dict *daGetDict(dictarray *da, int slot) {
    return da->dicts[slot];
}

/* Finds a slot containing target element in a key space ordered by slot id.
 * Consider this example. Slots are represented by brackets and keys by dots:
 *  #0   #1   #2     #3    #4
 * [..][....][...][.......][.]
 *                    ^
 *                 target
 *
 * In this case slot #3 contains key that we are trying to find.
 *
 * The return value is 0 based slot, and the range of the target is [1..dbSize], dbSize inclusive.
 *
 * To find the slot, we start with the root node of the binary index tree and search through its children
 * from the highest index (2^14 in our case) to the lowest index. At each node, we check if the target
 * value is greater than the node's value. If it is, we remove the node's value from the target and recursively
 * search for the new target using the current node as the parent.
 * Time complexity of this function is O(log(da->num_slots))
 */
int daFindSlotByKeyIndex(dictarray *da, unsigned long target) {
    if (da->num_slots == 1 || daSize(da) == 0)
        return 0;
    assert(target <= daSize(da));

    int result = 0, bit_mask = 1 << da->num_slots_bits;
    for (int i = bit_mask; i != 0; i >>= 1) {
        int current = result + i;
        /* When the target index is greater than 'current' node value the we will update
         * the target and search in the 'current' node tree. */
        if (target > da->state.slot_size_index[current]) {
            target -= da->state.slot_size_index[current];
            result = current;
        }
    }
    /* Adjust the result to get the correct slot:
     * 1. result += 1;
     *    After the calculations, the index of target in slot_size_index should be the next one,
     *    so we should add 1.
     * 2. result -= 1;
     *    Unlike BIT(slot_size_index is 1-based), slots are 0-based, so we need to subtract 1.
     * As the addition and subtraction cancel each other out, we can simply return the result. */
    return result;
}

/* Returns next non-empty slot strictly after given one, or -1 if provided slot is the last one. */
int daGetNextNonEmptySlot(dictarray *da, int slot) {
    unsigned long long next_key = cumulativeKeyCountRead(da, slot) + 1;
    return next_key <= daSize(da) ? daFindSlotByKeyIndex(da, next_key) : -1;
}

/* Updates binary index tree (also known as Fenwick tree), increasing key count for a given slot.
 * You can read more about this data structure here https://en.wikipedia.org/wiki/Fenwick_tree
 * Time complexity is O(log(da->num_slots)). */
void daCumulativeKeyCountAdd(dictarray *da, int slot, long delta) {
    da->state.key_count += delta;

    dict *d = daGetDict(da, slot);
    size_t dsize = dictSize(d);
    int non_empty_slots_delta = dsize == 1? 1 : dsize == 0? -1 : 0;
    da->state.non_empty_slots += non_empty_slots_delta;

    /* BIT does not need to be calculated when the cluster is turned off. */
    if (da->num_slots == 1)
        return;

    /* Update the BIT */
    int idx = slot + 1; /* Unlike slots, BIT is 1-based, so we need to add 1. */
    while (idx <= da->num_slots) {
        if (delta < 0) {
            assert(da->state.slot_size_index[idx] >= (unsigned long long)labs(delta));
        }
        da->state.slot_size_index[idx] += delta;
        idx += (idx & -idx);
    }
}

int daNonEmptySlots(dictarray *da) {
    return da->state.non_empty_slots;
}

/* Returns DB iterator that can be used to iterate through sub-dictionaries.
 * Primary database contains only one dictionary when node runs without cluster mode,
 * or 16k dictionaries (one per slot) when node runs with cluster mode enabled.
 *
 * The caller should free the resulting dbit with dbReleaseIterator. */
daIterator *daIteratorInit(dictarray *da) {
    daIterator *dait = zmalloc(sizeof(*dait));
    dait->da = da;
    dait->slot = -1;
    dait->next_slot = daFindSlotByKeyIndex(dait->da, 1); /* Finds first non-empty slot. */
    dictInitSafeIterator(&dait->di, NULL);
    return dait;
}

/* Free the dbit returned by dbIteratorInit. */
void daReleaseIterator(daIterator *dait) {
    dictIterator *iter = &dait->di;
    dictResetIterator(iter);

    zfree(dait);
}

/* Returns next dictionary from the iterator, or NULL if iteration is complete. */
dict *daIteratorNextDict(daIterator *dait) {
    if (dait->next_slot == -1)
        return NULL;
    dait->slot = dait->next_slot;
    dait->next_slot = daGetNextNonEmptySlot(dait->da, dait->slot);
    return dait->da->dicts[dait->slot];
}

int daIteratorGetCurrentSlot(daIterator *dait) {
    assert(dait->slot >= 0 && dait->slot < dait->da->num_slots);
    return dait->slot;
}

/* Returns next entry from the multi slot db. */
dictEntry *daIteratorNext(daIterator *dait) {
    dictEntry *de = dait->di.d ? dictNext(&dait->di) : NULL;
    if (!de) { /* No current dict or reached the end of the dictionary. */
        dict *d = daIteratorNextDict(dait);
        if (!d)
            return NULL;
        if (dait->di.d) {
            /* Before we move to the next dict, reset the iter of the previous dict. */
            dictIterator *iter = &dait->di;
            dictResetIterator(iter);
        }
        dictInitSafeIterator(&dait->di, d);
        de = dictNext(&dait->di);
    }
    return de;
}

dict *daGetDictFromIterator(daIterator *dait) {
    return daGetDict(dait->da, dait->slot);
}

/* In cluster-enabled setup, this method traverses through all main/expires dictionaries (CLUSTER_SLOTS)
 * and triggers a resize if the percentage of used buckets in the HT reaches HASHTABLE_MIN_FILL
 * we resize the hash table to save memory.
 *
 * In non cluster-enabled setup, it resize main/expires dictionary based on the same condition described above. */
void daTryResizeHashTables(dictarray *da, int attempts) {
    if (daSize(da) == 0)
        return;

    if (da->state.resize_cursor == -1)
        da->state.resize_cursor = daFindSlotByKeyIndex(da, 1);

    for (int i = 0; i < attempts && da->state.resize_cursor != -1; i++) {
        int slot = da->state.resize_cursor;
        dict *d = daGetDict(da, slot);
        dictResize(d);
        da->state.resize_cursor = daGetNextNonEmptySlot(da, slot);
    }
}

/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use 1 millisecond
 * of CPU time at every call of this function to perform some rehashing.
 *
 * The function returns 1 if some rehashing was performed, otherwise 0
 * is returned. */
int daIncrementallyRehash(dictarray *da, uint64_t threshold_ms) {
    /* Rehash main and expire dictionary . */
    if (da->num_slots > 1) {
        listNode *node, *nextNode;
        monotime timer;
        elapsedStart(&timer);
        /* Our goal is to rehash as many slot specific dictionaries as we can before reaching predefined threshold,
         * while removing those that already finished rehashing from the queue. */
        while ((node = listFirst(da->state.rehashing))) {
            dict *d = listNodeValue(node);
            if (dictIsRehashing(d)) {
                dictRehashMilliseconds(d, threshold_ms);
                if (elapsedMs(timer) >= threshold_ms) {
                    return 1;  /* Reached the time limit. */
                }
            } else { /* It is possible that rehashing has already completed for this dictionary, simply remove it from the queue. */
                nextNode = listNextNode(node);
                listDelNode(da->state.rehashing, node);
                node = nextNode;
            }
        }

        /* When cluster mode is disabled, only one dict is used for the entire DB and rehashing list isn't populated. */
    } else {
        dict *d = daGetDict(da, 0);
        if (dictIsRehashing(d)) {
            dictRehashMilliseconds(d, threshold_ms);
            return 1; /* already used our millisecond for this loop... */
        }
    }
    return 0;
}

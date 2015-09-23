#include "server.h"
#include "bio.h"

static int lazyfree_threaded = 1; /* Use a thread to reclaim objects. */

/* Initialization of the lazy free engine. Must be called only once at server
 * startup. */
void initLazyfreeEngine(void) {
    server.lazyfree_dbs = listCreate();
    server.lazyfree_obj = listCreate();
    server.lazyfree_elements = 0;
}

/* Return the amount of work needed in order to free an object.
 * The return value is not always the actual number of allocations the
 * object is compoesd of, but a number proportional to it.
 *
 * For strings the function always returns 1.
 *
 * For aggregated objects represented by hash tables or other data structures
 * the function just returns the number of elements the object is composed of.
 *
 * Objects composed of single allocations are always reported as having a
 * single item even if they are actaully logical composed of multiple
 * elements.
 *
 * For lists the funciton returns the number of elements in the quicklist
 * representing the list. */
size_t lazyfreeGetFreeEffort(robj *obj) {
    if (obj->type == OBJ_LIST) {
        quicklist *ql = obj->ptr;
        return ql->len;
    } else if (obj->type == OBJ_SET && obj->encoding == OBJ_ENCODING_HT) {
        dict *ht = obj->ptr;
        return dictSize(ht);
    } else if (obj->type == OBJ_ZSET && obj->encoding == OBJ_ENCODING_SKIPLIST){
        zset *zs = obj->ptr;
        return zs->zsl->length;
    } else if (obj->type == OBJ_HASH && obj->encoding == OBJ_ENCODING_HT) {
        dict *ht = obj->ptr;
        return dictSize(ht);
    } else {
        return 1; /* Everything else is a single allocation. */
    }
}

/* This callback is used together with dictScan() in order to free a dict.c
 * hash table incrementally. */
void lazyfreeScanCallback(void *privdata, const dictEntry *de) {
    dict *ht = privdata;
    long saved_iterators = ht->iterators;
    ht->iterators = 1; /* Make sure no rehashing happens. */
    dictDelete(ht,dictGetKey(de));
    ht->iterators = saved_iterators;
}

/* Free some object from the lazy free list. */
#define LAZYFREE_ITER_PER_STEP 100
size_t lazyfreeFastStep(void) {
    size_t maxiter = LAZYFREE_ITER_PER_STEP;
    size_t workdone = 0;
    robj *current = NULL;

    while(maxiter--) {
        if (current == NULL) {
            listNode *ln = listFirst(server.lazyfree_obj);
            if (ln == NULL) break; /* Nothing more to free. */
            current = ln->value;
        }
        if ((current->type == OBJ_SET ||
            current->type == OBJ_HASH) &&
            current->encoding == OBJ_ENCODING_HT)
        {
            dict *ht = current->ptr;
            size_t origsize = dictSize(ht);
            ht->iterators = dictScan(ht,ht->iterators,lazyfreeScanCallback,ht);
            workdone++; /* We are not sure how many elements we freed, even if
                           zero, the free list is non empty so we don't return
                           0 to the caller. */
            server.lazyfree_elements -= (origsize - dictSize(ht));
            if (dictSize(ht) == 0) {
                decrRefCount(current);
                listNode *ln = listFirst(server.lazyfree_obj);
                listDelNode(server.lazyfree_obj,ln);
                current = NULL;
            }
        } else {
            /* Not handled type or encoding. Do a blocking free. */
            size_t effort = lazyfreeGetFreeEffort(current);
            server.lazyfree_elements -= effort;
            workdone += effort;
            decrRefCount(current);
            listNode *ln = listFirst(server.lazyfree_obj);
            listDelNode(server.lazyfree_obj,ln);
            current = NULL;
        }
    }
    return workdone;
}

/* Handles slow or fast collection steps. */
size_t lazyfreeStep(int type) {
    /* Threaded implementaiton: only block for STEP_OOM. */
    if (lazyfree_threaded) {
        if (type == LAZYFREE_STEP_OOM)
            return bioWaitStepOfType(BIO_LAZY_FREE);
        return 0;
    }

    /* Non threaded implementation: free things incrementally avoiding
     * to block. */
    if (type == LAZYFREE_STEP_FAST ||
        type == LAZYFREE_STEP_OOM) return lazyfreeFastStep();

    size_t totalwork = 0;
    mstime_t end = mstime()+2;
    do {
        size_t workdone = lazyfreeFastStep();
        if (workdone == 0) break;
        totalwork += workdone;
    } while(mstime() < end);
    return totalwork;
}

/* Delete a key, value, and associated expiration entry if any, from the DB.
 * If there are enough allocations to free the value object may be put into
 * a lazy free list instead of being freed synchronously. The lazy free list
 * will be reclaimed incrementally in a non blocking way. */
#define LAZYFREE_THRESHOLD 64
int dbAsyncDelete(redisDb *db, robj *key) {
    /* Deleting an entry from the expires dict will not free the sds of
     * the key, because it is shared with the main dictionary. */
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);

    /* If the value is composed of a few allocations, to free in a lazy way
     * is actually just slower... So under a certain limit we just free
     * the object synchronously. */
    dictEntry *de = dictFind(db->dict,key->ptr);
    if (de) {
        robj *val = dictGetVal(de);
        size_t free_effort = lazyfreeGetFreeEffort(val);

        /* If releasing the object is too much work, let's put it into the
         * lazy free list. */
        if (free_effort > LAZYFREE_THRESHOLD) {
            if (lazyfree_threaded) {
                bioCreateBackgroundJob(BIO_LAZY_FREE,val,NULL,NULL);
            } else {
                listAddNodeTail(server.lazyfree_obj,val);
                server.lazyfree_elements += free_effort;
            }
            dictSetVal(db->dict,de,NULL);
        }
    }

    /* Release the key-val pair, or just the key if we set the val
     * field to NULL in order to lazy free it later. */
    if (dictDelete(db->dict,key->ptr) == DICT_OK) {
        if (server.cluster_enabled) slotToKeyDel(key);
        return 1;
    } else {
        return 0;
    }
}

/* This is the timer handler we use to incrementally perform collection
 * into the lazy free lists. We can't use serverCron since we need a
 * very high timer frequency when there are many objects to collect, while
 * we lower the frequency to just 1HZ when there is nothing to do.
 *
 * Since a slow lazy free step will take 1.5 milliseconds and we modulate
 * the timer frequency from 1 to 333 HZ in an adaptive way, the CPU
 * used is between 0% (nothing in the lazy free list) to 50%.
 *
 * The frequency is obtained as follows: if the lazy free list is empty
 * it is set to 1HZ. If the lazy free has elements the call period starts
 * at 20 (50HZ) and is decremented (up to 3 ms = 333HZ) each time the server
 * used memory raises between calls of this function. */
int lazyfreeCron(struct aeEventLoop *eventLoop, long long id, void *clientData)
{
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    /* Threaded lazy free does not need a timer, unregister the timer event. */
    if (lazyfree_threaded) return AE_NOMORE;

    static size_t prev_mem;
    static int timer_period = 1000; /* Defauls to 1HZ */
    static double mem_trend = 0;
    size_t mem = zmalloc_used_memory();

    /* Compute the memory trend, biased towards thinking memory is raising
     * for a few calls every time previous and current memory raise. */
    if (prev_mem < mem) mem_trend = 1;
    mem_trend *= 0.9; /* Make it slowly forget. */
    int mem_is_raising = mem_trend > .1;

    /* Free a few items. */
    size_t workdone = lazyfreeStep(LAZYFREE_STEP_SLOW);

    /* Adjust this timer call frequency according to the current state. */
    if (workdone) {
        if (timer_period == 1000) timer_period = 20;
        if (mem_is_raising && timer_period > 3)
            timer_period--; /* Raise call frequency. */
        else if (!mem_is_raising && timer_period < 20)
            timer_period++; /* Lower call frequency. */
    } else {
        timer_period = 1000;    /* 1 HZ */
    }
    prev_mem = mem;
#if 0
    printf("%llu (%d hz) %s (%f)\n",
        (unsigned long long)server.lazyfree_elements,
        1000/timer_period,
        mem_is_raising ? "RAISING" : "lowering",
        mem_trend);
#endif
    return timer_period;
}

#include "server.h"
#include "lazyfree.h"
#include "dict.h"
#include "adlist.h"
#include "rax.h"
#include "adlist.h"
#include "atomicvar.h"

static redisAtomic size_t lazyfree_objects = 0;
static redisAtomic size_t lazyfreed_objects = 0;

static bjmJobFuncHandle lazyfreeObjectId;
static bjmJobFuncHandle lazyfreeDictId;
static bjmJobFuncHandle lazyfreeListId;
static bjmJobFuncHandle lazyfreeRaxId;
static bjmJobFuncHandle lazyfreeRaxWithCallbackId;


/* Release objects from a BJM thread. It's just decrRefCount()
 * updating the count of objects to release. */
static void lazyfreeObjectFunc(void *privdata) {
    robj *o = (robj*)privdata;
    decrRefCount(o);
    atomicDecr(lazyfree_objects, 1);
    atomicIncr(lazyfreed_objects, 1);
}


/* Release a dict (calling free function if defined) from a BJM thread. */
static void lazyfreeDictFunc(void *privdata) {
    dict *d = (dict*)privdata;
    size_t numkeys = dictSize(d);
    dictRelease(d);
    atomicDecr(lazyfree_objects, numkeys);
    atomicIncr(lazyfreed_objects, numkeys);
}


/* Release a list (calling free function if defined) from a BJM thread. */
static void lazyfreeListFunc(void *privdata) {
    list *l = (list*)privdata;
    size_t numkeys = listLength(l);
    listRelease(l);
    atomicDecr(lazyfree_objects, numkeys);
    atomicIncr(lazyfreed_objects, numkeys);
}


/* Release a RAX from a BJM thread. */
static void lazyfreeRaxFunc(void *privdata) {
    rax *r = (rax*)privdata;
    size_t numkeys = raxSize(r);
    raxFree(r);
    atomicDecr(lazyfree_objects, numkeys);
    atomicIncr(lazyfreed_objects, numkeys);
}

typedef struct {
    rax *r;
    void (*free)(void *ptr);
} raxWithCallback;

/* Release a RAX (calling provided free function) from a BJM thread. */
static void lazyfreeRaxWithCallbackFunc(void *privdata) {
    raxWithCallback *rwc = (raxWithCallback*)privdata;
    size_t numkeys = raxSize(rwc->r);
    raxFreeWithCallback(rwc->r, rwc->free);
    zfree(rwc);
    atomicDecr(lazyfree_objects, numkeys);
    atomicIncr(lazyfreed_objects, numkeys);
}


void lazyfreeInit() {
    lazyfreeObjectId = bjmRegisterJobFunc(lazyfreeObjectFunc);
    lazyfreeDictId = bjmRegisterJobFunc(lazyfreeDictFunc);
    lazyfreeListId = bjmRegisterJobFunc(lazyfreeListFunc);
    lazyfreeRaxId = bjmRegisterJobFunc(lazyfreeRaxFunc);
    lazyfreeRaxWithCallbackId = bjmRegisterJobFunc(lazyfreeRaxWithCallbackFunc);
}


/* Return the number of currently pending objects to free. */
size_t lazyfreeGetPendingObjectsCount(void) {
    size_t aux;
    atomicGet(lazyfree_objects, aux);
    return aux;
}

/* Return the number of objects that have been freed. */
size_t lazyfreeGetFreedObjectsCount(void) {
    size_t aux;
    atomicGet(lazyfreed_objects, aux);
    return aux;
}

void lazyfreeResetStats() {
    atomicSet(lazyfreed_objects, 0);
}

/* Return the amount of work needed in order to free an object.
 * The return value is not always the actual number of allocations the
 * object is composed of, but a number proportional to it.
 *
 * For strings the function always returns 1.
 *
 * For aggregated objects represented by hash tables or other data structures
 * the function just returns the number of elements the object is composed of.
 *
 * Objects composed of single allocations are always reported as having a
 * single item even if they are actually logical composed of multiple
 * elements.
 *
 * For lists the function returns the number of elements in the quicklist
 * representing the list. */
static size_t lazyfreeGetFreeEffort(robj *obj) {
    if (obj->type == OBJ_LIST && obj->encoding == OBJ_ENCODING_QUICKLIST) {
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
    } else if (obj->type == OBJ_STREAM) {
        size_t effort = 0;
        stream *s = obj->ptr;

        /* Make a best effort estimate to maintain constant runtime. Every macro
         * node in the Stream is one allocation. */
        effort += s->rax->numnodes;

        /* Every consumer group is an allocation and so are the entries in its
         * PEL. We use size of the first group's PEL as an estimate for all
         * others. */
        if (s->cgroups && raxSize(s->cgroups)) {
            raxIterator ri;
            streamCG *cg;
            raxStart(&ri,s->cgroups);
            raxSeek(&ri,"^",NULL,0);
            /* There must be at least one group so the following should always
             * work. */
            serverAssert(raxNext(&ri));
            cg = ri.data;
            effort += raxSize(s->cgroups)*(1+raxSize(cg->pel));
            raxStop(&ri);
        }
        return effort;
    } else if (obj->type == OBJ_MODULE) {
        // Modules require a DBID and KEY, this function is just for an
        //  arbitrary robj which might not be related to a DB entry. 
        serverAssert(0);
    } else {
        return 1; /* Everything else is a single allocation. */
    }
}


void lazyfreeObject(robj *o) {
    if (o->refcount == 1 && lazyfreeGetFreeEffort(o) > LAZYFREE_THRESHOLD) {
        atomicIncr(lazyfree_objects, 1);
        bjmSubmitJob(lazyfreeObjectId, o);
    } else {
        decrRefCount(o);
    }
}


void lazyfreeDict(dict *d) {
    if (dictSize(d) > LAZYFREE_THRESHOLD) {
        atomicIncr(lazyfree_objects, dictSize(d));
        bjmSubmitJob(lazyfreeDictId, d);
    } else {
        dictRelease(d);
    }
}


void lazyfreeList(list *l) {
    if (listLength(l) > LAZYFREE_THRESHOLD) {
        atomicIncr(lazyfree_objects, listLength(l));
        bjmSubmitJob(lazyfreeListId, l);
    } else {
        listRelease(l);
    }
}


void lazyfreeRax(rax *r) {
    if (raxSize(r) > LAZYFREE_THRESHOLD) {
        atomicIncr(lazyfree_objects, raxSize(r));
        bjmSubmitJob(lazyfreeRaxId, r);
    } else {
        raxFree(r);
    }
}


void lazyfreeRaxWithCallback(rax *r, void (*free_callback)(void*)) {
    if (raxSize(r) > LAZYFREE_THRESHOLD) {
        atomicIncr(lazyfree_objects, raxSize(r));
        raxWithCallback *rwc = zmalloc(sizeof(raxWithCallback));
        rwc->r = r;
        rwc->free = free_callback;
        bjmSubmitJob(lazyfreeRaxWithCallbackId, rwc);
    } else {
        raxFreeWithCallback(r, free_callback);
    }
}


void lazyfreeGeneric(long cardinality, bjmJobFuncHandle func, void *item) {
    atomicIncr(lazyfree_objects, cardinality);
    bjmSubmitJob(func, item);
}


void lazyfreeGenericComplete(long cardinality) {
    atomicDecr(lazyfree_objects, cardinality);
    atomicIncr(lazyfreed_objects, cardinality);
}

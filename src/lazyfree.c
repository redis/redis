#include "server.h"
#include "bio.h"
#include "atomicvar.h"
#include "functions.h"
#include "cluster.h"

static redisAtomic size_t lazyfree_objects = 0;
static redisAtomic size_t lazyfreed_objects = 0;

/* Release objects from the lazyfree thread. It's just decrRefCount()
 * updating the count of objects to release. */
void lazyfreeFreeObject(void *args[]) {
    robj *o = (robj *) args[0];
    decrRefCount(o);
    atomicDecr(lazyfree_objects,1);
    atomicIncr(lazyfreed_objects,1);
}

/* Release a database from the lazyfree thread. The 'db' pointer is the
 * database which was substituted with a fresh one in the main thread
 * when the database was logically deleted. */
void lazyfreeFreeDatabase(void *args[]) {
    kvstore *da1 = args[0];
    kvstore *da2 = args[1];

    size_t numkeys = kvstoreSize(da1);
    kvstoreRelease(da1);
    kvstoreRelease(da2);
    atomicDecr(lazyfree_objects,numkeys);
    atomicIncr(lazyfreed_objects,numkeys);

#if defined(USE_JEMALLOC)
    /* Only clear the current thread cache.
     * Ignore the return call since this will fail if the tcache is disabled. */
    je_mallctl("thread.tcache.flush", NULL, NULL, NULL, 0);

    jemalloc_purge();
#endif
}

/* Release the key tracking table. */
void lazyFreeTrackingTable(void *args[]) {
    rax *rt = args[0];
    size_t len = rt->numele;
    freeTrackingRadixTree(rt);
    atomicDecr(lazyfree_objects,len);
    atomicIncr(lazyfreed_objects,len);
}

/* Release the error stats rax tree. */
void lazyFreeErrors(void *args[]) {
    rax *errors = args[0];
    size_t len = errors->numele;
    raxFreeWithCallback(errors, zfree);
    atomicDecr(lazyfree_objects,len);
    atomicIncr(lazyfreed_objects,len);
}

/* Release the lua_scripts dict. */
void lazyFreeLuaScripts(void *args[]) {
    dict *lua_scripts = args[0];
    list *lua_scripts_lru_list = args[1];
    lua_State *lua = args[2];
    long long len = dictSize(lua_scripts);
    freeLuaScriptsSync(lua_scripts, lua_scripts_lru_list, lua);
    atomicDecr(lazyfree_objects,len);
    atomicIncr(lazyfreed_objects,len);
}

/* Release the functions ctx. */
void lazyFreeFunctionsCtx(void *args[]) {
    functionsLibCtx *functions_lib_ctx = args[0];
    size_t len = functionsLibCtxFunctionsLen(functions_lib_ctx);
    functionsLibCtxFree(functions_lib_ctx);
    atomicDecr(lazyfree_objects,len);
    atomicIncr(lazyfreed_objects,len);
}

/* Release replication backlog referencing memory. */
void lazyFreeReplicationBacklogRefMem(void *args[]) {
    list *blocks = args[0];
    rax *index = args[1];
    long long len = listLength(blocks);
    len += raxSize(index);
    listRelease(blocks);
    raxFree(index);
    atomicDecr(lazyfree_objects,len);
    atomicIncr(lazyfreed_objects,len);
}

/* Return the number of currently pending objects to free. */
size_t lazyfreeGetPendingObjectsCount(void) {
    size_t aux;
    atomicGet(lazyfree_objects,aux);
    return aux;
}

/* Return the number of objects that have been freed. */
size_t lazyfreeGetFreedObjectsCount(void) {
    size_t aux;
    atomicGet(lazyfreed_objects,aux);
    return aux;
}

void lazyfreeResetStats(void) {
    atomicSet(lazyfreed_objects,0);
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
size_t lazyfreeGetFreeEffort(robj *key, robj *obj, int dbid) {
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
        size_t effort = moduleGetFreeEffort(key, obj, dbid);
        /* If the module's free_effort returns 0, we will use asynchronous free
         * memory by default. */
        return effort == 0 ? ULONG_MAX : effort;
    } else {
        return 1; /* Everything else is a single allocation. */
    }
}

/* If there are enough allocations to free the value object asynchronously, it
 * may be put into a lazy free list instead of being freed synchronously. The
 * lazy free list will be reclaimed in a different bio.c thread. If the value is
 * composed of a few allocations, to free in a lazy way is actually just
 * slower... So under a certain limit we just free the object synchronously. */
#define LAZYFREE_THRESHOLD 64

/* Free an object, if the object is huge enough, free it in async way. */
void freeObjAsync(robj *key, robj *obj, int dbid) {
    size_t free_effort = lazyfreeGetFreeEffort(key,obj,dbid);
    /* Note that if the object is shared, to reclaim it now it is not
     * possible. This rarely happens, however sometimes the implementation
     * of parts of the Redis core may call incrRefCount() to protect
     * objects, and then call dbDelete(). */
    if (free_effort > LAZYFREE_THRESHOLD && obj->refcount == 1) {
        atomicIncr(lazyfree_objects,1);
        bioCreateLazyFreeJob(lazyfreeFreeObject,1,obj);
    } else {
        decrRefCount(obj);
    }
}

/* Empty a Redis DB asynchronously. What the function does actually is to
 * create a new empty set of hash tables and scheduling the old ones for
 * lazy freeing. */
void emptyDbAsync(redisDb *db) {
    int slot_count_bits = 0;
    int flags = KVSTORE_ALLOCATE_DICTS_ON_DEMAND;
    if (server.cluster_enabled) {
        slot_count_bits = CLUSTER_SLOT_MASK_BITS;
        flags |= KVSTORE_FREE_EMPTY_DICTS;
    }
    kvstore *oldkeys = db->keys, *oldexpires = db->expires;
    db->keys = kvstoreCreate(&dbDictType, slot_count_bits, flags);
    db->expires = kvstoreCreate(&dbExpiresDictType, slot_count_bits, flags);
    atomicIncr(lazyfree_objects, kvstoreSize(oldkeys));
    bioCreateLazyFreeJob(lazyfreeFreeDatabase, 2, oldkeys, oldexpires);
}

/* Free the key tracking table.
 * If the table is huge enough, free it in async way. */
void freeTrackingRadixTreeAsync(rax *tracking) {
    /* Because this rax has only keys and no values so we use numnodes. */
    if (tracking->numnodes > LAZYFREE_THRESHOLD) {
        atomicIncr(lazyfree_objects,tracking->numele);
        bioCreateLazyFreeJob(lazyFreeTrackingTable,1,tracking);
    } else {
        freeTrackingRadixTree(tracking);
    }
}

/* Free the error stats rax tree.
 * If the rax tree is huge enough, free it in async way. */
void freeErrorsRadixTreeAsync(rax *errors) {
    /* Because this rax has only keys and no values so we use numnodes. */
    if (errors->numnodes > LAZYFREE_THRESHOLD) {
        atomicIncr(lazyfree_objects,errors->numele);
        bioCreateLazyFreeJob(lazyFreeErrors,1,errors);
    } else {
        raxFreeWithCallback(errors, zfree);
    }
}

/* Free lua_scripts dict and lru list, if the dict is huge enough, free them in async way.
 * Close lua interpreter, if there are a lot of lua scripts, close it in async way. */
void freeLuaScriptsAsync(dict *lua_scripts, list *lua_scripts_lru_list, lua_State *lua) {
    if (dictSize(lua_scripts) > LAZYFREE_THRESHOLD) {
        atomicIncr(lazyfree_objects,dictSize(lua_scripts));
        bioCreateLazyFreeJob(lazyFreeLuaScripts,3,lua_scripts,lua_scripts_lru_list,lua);
    } else {
        freeLuaScriptsSync(lua_scripts, lua_scripts_lru_list, lua);
    }
}

/* Free functions ctx, if the functions ctx contains enough functions, free it in async way. */
void freeFunctionsAsync(functionsLibCtx *functions_lib_ctx) {
    if (functionsLibCtxFunctionsLen(functions_lib_ctx) > LAZYFREE_THRESHOLD) {
        atomicIncr(lazyfree_objects,functionsLibCtxFunctionsLen(functions_lib_ctx));
        bioCreateLazyFreeJob(lazyFreeFunctionsCtx,1,functions_lib_ctx);
    } else {
        functionsLibCtxFree(functions_lib_ctx);
    }
}

/* Free replication backlog referencing buffer blocks and rax index. */
void freeReplicationBacklogRefMemAsync(list *blocks, rax *index) {
    if (listLength(blocks) > LAZYFREE_THRESHOLD ||
        raxSize(index) > LAZYFREE_THRESHOLD)
    {
        atomicIncr(lazyfree_objects,listLength(blocks)+raxSize(index));
        bioCreateLazyFreeJob(lazyFreeReplicationBacklogRefMem,2,blocks,index);
    } else {
        listRelease(blocks);
        raxFree(index);
    }
}

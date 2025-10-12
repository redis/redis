/* SOBJ -- autonomous shared-object reference handling.
 *
 * Copyright (c) 2017-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "server.h"
#include "sobj.h"

void sobj_type_destructor(dict *d, void *key)
{
    UNUSED(d);
    sobj_free_raw(key);
}

dictType sobj_dict_type = {
    dictObjHash,
    NULL,
    NULL,
    dictObjKeyCompare,
    sobj_type_destructor,
    NULL,
    .no_value = 1, /* Implement a set (no values). Allows optimizing out the dictEntry struct */
    .keys_are_odd = 0
};

dict *sobj_init(void)
{
    return dictCreate(&sobj_dict_type);
}

void sobj_release(dict *pool)
{
    dictRelease(pool);
}

sobj *sobj_new(const char *init, size_t initlen, dict *pool)
{
    if (initlen == 0)
        return NULL;
    serverAssert(init);

    sobj *new_obj = createStringObject(init, initlen);
    serverAssert(new_obj->ptr);
    dictEntry *existing_de;
    if (dictAddRaw(pool, new_obj, &existing_de)) { /* No applicable shared-string found */
        serverLog(LL_NOTICE, "Added new shared-string %p, %d, (%p) '%s'", (void*)new_obj, new_obj->refcount, new_obj->ptr, (char*)new_obj->ptr);
        return new_obj;
    } else { /* Same value shared-string found in the pool */
        sobj_free_raw(new_obj);

        sobj *existing_obj = dictGetKey(existing_de);
        incrRefCount(existing_obj);
        serverLog(LL_NOTICE, "Added existing shared-string %p, %d, (%p) '%s'", (void*)existing_obj, existing_obj->refcount, existing_obj->ptr, (char*)existing_obj->ptr);
        return existing_obj;
    }
}

void sobj_free_raw(sobj *o)
{
    serverAssert(o->refcount == 1);
    serverLog(LL_NOTICE, "Freeing shared-string %p, %d, (%p) '%s'", (void*)o, o->refcount, o->ptr, (char*)o->ptr);
    decrRefCount(o);
}

void sobj_free(sobj *o, dict *pool)
{
    if (o == NULL)
        return;

    if (o->refcount == 1) {
        if (sobj_find(o, pool) == o) /* Usual case. If no active-defrag enabled, this clause is *always* true */
            dictDelete(pool, o);
        else {/* In case we active-defraged, the respective `de` was already removed (pool points only to newest shared-string) */
            sobj_free_raw(o);
        }
    } else {
        --o->refcount;
    }
}

sobj *sobj_defrag(sobj *o, dict *pool)
{
#ifdef HAVE_DEFRAG
    if (o == NULL)
        return NULL;

    serverLog(LL_NOTICE, "Before defragging shared-string %p, %d, (%p) '%s'", (void*)o, o->refcount, o->ptr, (char*)o->ptr);
    sobj *other = sobj_find(o, pool);
    if (other == o ||  /* Active shared-string pointer is our pointer, defragging by creating a new one */
        other == NULL) /* Edge case where active shared-string deleted before defrag process finished */
    {
        if (other) {
            dictEntry *de = dictUnlink(pool, o);
            serverAssert(de);
            dictFreeUnlinkedEntryOnly(de);
        }

        server.disable_defrag_misses = 1;
        other = activeDefragStringObEx(o, o->refcount, 1);
        server.disable_defrag_misses = 0;

        serverAssert(other && other != o && other->ptr != o->ptr);
        other->refcount = 0;
        dictAdd(pool, other, NULL);
    }

    if (o->refcount == 1) {
        sobj_free_raw(o);
    } else {
        --o->refcount;
    }
    ++other->refcount;

    serverLog(LL_NOTICE, "After defragging shared-string (original) %p, %d, (%p) '%s'", (void*)o, o->refcount, o->ptr, (char*)o->ptr);
    serverLog(LL_NOTICE, "After defragging shared-string (returned) %p, %d, (%p) '%s'", (void*)other, other->refcount, other->ptr, (char*)other->ptr);
    return other;
#else
    UNUSED(o);
    UNUSED(pool);
    return NULL;
#endif
}

sobj *sobj_find(sobj *o, dict *pool)
{
    dictEntry *de = dictFind(pool, o);
    return de ? dictGetKey(de) : NULL;
}

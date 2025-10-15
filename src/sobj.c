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
    decrRefCount(key);
    server.stat_shared_objects_cnt--;
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
        server.stat_shared_objects_cnt++;
        return new_obj;
    } else { /* Same value shared-string found in the pool */
        decrRefCount(new_obj);

        sobj *existing_obj = dictGetKey(existing_de);
        incrRefCount(existing_obj);
        return existing_obj;
    }
}

void sobj_free(sobj *o, dict *pool)
{
    if (o == NULL)
        return;

    /* sobj_find(o, pool) == o is usually true, unless we active-defraged.
       in that case, the respective `de` was already removed - pool points only to newest shared-string */
    if (o->refcount == 1 && sobj_find(o, pool) == o)
        dictDelete(pool, o);
    else
        decrRefCount(o);
}

sobj *sobj_defrag(sobj *o, dict *pool)
{
#ifdef HAVE_DEFRAG
    if (o == NULL)
        return NULL;

    sobj *other = sobj_find(o, pool);
    if (other == o ||  /* Active shared-string pointer is our pointer, defragging by creating a new one */
        other == NULL) /* Edge case where active shared-string deleted before defrag process finished */
    {
        if (other) {
            dictEntry *de = dictUnlink(pool, o);
            serverAssert(de);
            dictFreeUnlinkedEntryOnly(de);
        }

        other = activeDefragStringObEx(o, o->refcount, 1);

        serverAssert(other && other != o && other->ptr != o->ptr);
        other->refcount = 0;
        dictAdd(pool, other, NULL);
    }

    decrRefCount(o);
    incrRefCount(other);
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

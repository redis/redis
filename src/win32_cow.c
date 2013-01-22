/*
 * Copyright (c), Microsoft Open Technologies, Inc.
 * All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/************************************************************************
 * This module implements copy on write to support
 * saving on a background thread in Windows.
 *
 * Collection objects (dictionaries, lists, sets, zsets)
 *  are copied to a read-only form if a command to modify the
 *  collection is started. This is triggered via lookupKeyWrite().
 *
 * Objects which are modified in place - ziplist, zipset, etc.
 *  are copied before being modified.
 * Strings are normally copied before being modified.
 *
 * In addition deletion of objects is deferred until the save is completed.
 *  This is done by modifying the dictionary delete function, and also
 *  by modifying the decrRefCount function.
 *
 * To allow conversion of collections while the save is iterating on them
 *  special iterators are used. These iterators can be migrated
 *  from their normal mode to iterating over a read-only collection.
 *  Locking is used so that iterator can be used from 2 threads.
 *  For migration to work properly, only one save at a time may run.
 *   (this restriction was already imposed in the Redis code)
 *
 ************************************************************************/

#include "redis.h"

#ifdef _WIN32

/* list of objects to be deleted */
list *deferSdsDelete = NULL;
list *deferObjDelete = NULL;

/* COW locking */
void cowLock() {
    EnterCriticalSection(&server.cowCurIters.csMigrate);
}

void cowUnlock() {
    LeaveCriticalSection(&server.cowCurIters.csMigrate);
}

/* read only iterator migration
 * switches from actual collection to readonly array */
void roDBMigrateIterator(dict *d, cowDictArray *ar) {
    cowLock();
    if (server.cowCurIters.curDbDictIter != NULL &&
        server.cowCurIters.curDbDictIter->hdict == d) {
        server.cowCurIters.curDbDictIter->ar = ar;
    }
    cowUnlock();
}

void roDictMigrateIterator(dict *d, cowDictArray *ar) {
    cowLock();
    if (server.cowCurIters.curObjDictIter != NULL &&
        server.cowCurIters.curObjDictIter->hdict == d) {
        server.cowCurIters.curObjDictIter->ar = ar;
    }
    cowUnlock();
}

void roZDictMigrateIterator(dict *d, cowDictZArray *ar) {
    cowLock();
    if (server.cowCurIters.curObjZDictIter != NULL &&
        server.cowCurIters.curObjZDictIter->hdict == d) {
        server.cowCurIters.curObjZDictIter->ar = ar;
    }
    cowUnlock();
}

void roListMigrateIterator(list *l, cowListArray *ar) {
    cowLock();
    if (server.cowCurIters.curObjListIter != NULL &&
        server.cowCurIters.curObjListIter->olist == l) {
        server.cowCurIters.curObjListIter->ar = ar;
    }
    cowUnlock();
}


/* put object in list if deletes are deferred.
 * return 1 if deferred, 0 otherwise */
int deferFreeObject(void *obj) {
    if (deferObjDelete != NULL && server.isBackgroundSaving == 1) {
        listAddNodeHead(deferObjDelete, obj);
        return 1;
    }
    return 0;
}

/* dictionary destructors for defering delete */
void dictDbKeyDeferDestructor(void *privdata, void *val) {
    DICT_NOTUSED(privdata);

    listAddNodeHead(deferSdsDelete, val);
}

static void dictDbValDeferDestructor(void *privdata, void *val) {
    DICT_NOTUSED(privdata);

    listAddNodeHead(deferObjDelete, val);
}


/* dictionary types */
static void dictSdsDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    sdsfree((sds)val);
}

static unsigned int dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, (int)sdslen((char*)key));
}

static int dictSdsKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    size_t l1,l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}
static void dictRedisObjectDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    decrRefCount(val);
}

/* dictionary types used to support copy on write */
dictType ptrDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    dictSdsDestructor,         /* key destructor */
    NULL                       /* val destructor */
};

dictType dbDeferDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictDbKeyDeferDestructor,   /* key destructor */
    dictRedisObjectDestructor   /* val destructor */
};

dictType dbReadOnlyDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    NULL,                      /* key destructor */
    NULL                       /* val destructor */
};

dictType copiedCollectionDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    NULL,                       /* key destructor */
    dictRedisObjectDestructor   /* val destructor */
};


/* convert a linked list encoding to a list array encoding */
cowListArray *cowConvertListToArray(list *olist) {
    listIter li;
    listNode *ln;
    cowListArray *lar;
    listNode *lnNew;
    listNode *lnPrev;
    size_t ix = 0;

    lar = (cowListArray *)zmalloc(sizeof(cowListArray) + (sizeof(listNode) * olist->len));

    /* add copy of each item from old list */
    listRewind(olist,&li);
    lnNew = &lar->le[0];
    lnPrev = NULL;
    while((ln = listNext(&li)) && ix < olist->len) {
        /* copy object value to array list
            Do not incr ref count.  */
        lnNew->value = listNodeValue(ln);
        lnNew->prev = lnPrev;
        if (lnPrev != NULL) {
            lnPrev->next = lnNew;
        }
        lnPrev = lnNew;
        lnNew++;
        ix++;
    }
    if (lnPrev != NULL) {
        lnPrev->next = NULL;
    }
    lar->numele = ix;
    return lar;
}

void cowReleaseListArray(cowListArray *ar) {
    zfree(ar);
}

/* convert a hash dictionary encoding to a dictionary array encoding */
cowDictArray *cowConvertDictToArray(dict *hdict) {
    dictIterator * di;
    dictEntry *de;
    size_t dsize;
    cowDictArray *dar;
    size_t dcount = 0;
    dictEntry *deNew;
    dictEntry *dePrev;

    /* create copy */
    dsize = dictSize(hdict) > dictSlots(hdict) ? dictSize(hdict) : dictSlots(hdict);
    dar = (cowDictArray *)zmalloc(sizeof(cowDictArray) + (dsize * sizeof(dictEntry)));

    /* copy all entries without refcounting or copying values */
    /* can't just memcpy the whole dictionary because entries are allocated */
    di = dictGetSafeIterator(hdict);
    deNew = &dar->de[0];
    dePrev = NULL;
    while((de = dictNext(di)) != NULL && dcount < dsize) {
        /* copy object value to dict array
            Do not incr ref count.  */
        deNew->v = de->v;
        deNew->key = de->key;
        /* fix next ptr of prev entry */
        if (dePrev != NULL) {
            dePrev->next = deNew;
        }
        dePrev = deNew;
        deNew++;
        dcount++;
    }
    if (dePrev != NULL) {
        dePrev->next = NULL;
    }
    dar->numele = dcount;
    dictReleaseIterator(di);
    return dar;
}

void cowReleaseDictArray(cowDictArray *ar) {
    zfree(ar);
}

/* convert a hash dictionary encoding to a dictionary array encoding */
cowDictZArray *cowConvertDictToZArray(dict *hdict) {
    dictIterator * di;
    dictEntry *de;
    size_t dsize;
    cowDictZArray *dar;
    size_t dcount = 0;
    dictZEntry *dezNew;
    dictZEntry *dezPrev;

    /* create copy */
    dsize = dictSize(hdict) > dictSlots(hdict) ? dictSize(hdict) : dictSlots(hdict);
    dar = (cowDictZArray *)zmalloc(sizeof(cowDictZArray) +
                                (dsize * sizeof(dictZEntry)) );

    /* copy all entries without refcounting or copying values */
    /* can't just memcpy the whole dictionary because entries are allocated */
    di = dictGetSafeIterator(hdict);
    dezNew = &dar->zde[0];
    dezPrev = NULL;
    while((de = dictNext(di)) != NULL && dcount < dsize) {
        double *score = (double *)dictGetVal(de);
        /* copy score value into array
            and point val to score.  */
        dezNew->de.key = de->key;
        dezNew->score = *score;
        dezNew->de.v.val = &dezNew->score;
        /* fix next ptr of prev entry */
        if (dezPrev != NULL) {
            dezPrev->de.next = &dezNew->de;
        }
        dezPrev = dezNew;
        dezNew++;
        dcount++;
    }
    if (dezPrev != NULL) {
        dezPrev->de.next = NULL;
    }
    dar->numele = dcount;
    dictReleaseIterator(di);
    return dar;
}

void cowReleaseDictZArray(cowDictZArray *ar) {
    zfree(ar);
}

/* convert a linked list encoding to a list array encoding */
robj *cowListCopy(robj *val) {
    long long sttime;
    robj *newval;
    sttime = ustime();
    if (val->encoding == REDIS_ENCODING_ZIPLIST) {
        size_t bytes;
        newval = createZiplistObject();
        /* do raw memory copy */
        bytes = ziplistBlobLen(val->ptr);
        newval->ptr = zrealloc(newval->ptr, bytes);
        memcpy(newval->ptr, val->ptr, bytes);

        return newval;
    } else if (val->encoding == REDIS_ENCODING_LINKEDLIST) {
        list *list = val->ptr;
        cowListArray *lar;

        lar = cowConvertListToArray(list);
        newval = createObject(REDIS_LIST, lar);
        newval->encoding = REDIS_ENCODING_LINKEDLISTARRAY;

        return newval;
    } else {
        /* error. unexpected encoding */
        return NULL;
    }
}

/* convert a hash dictionary encoding to a dictionary array encoding */
robj *cowSetCopy(robj *val) {
    robj *newval;
    if (val->encoding == REDIS_ENCODING_INTSET) {
        size_t bytes;
        newval = createIntsetObject();
        /* do raw memory copy */
        bytes = intsetBlobLen(val->ptr);
        newval->ptr = zrealloc(newval->ptr, bytes);
        memcpy(newval->ptr, val->ptr, bytes);
        return newval;
    } else if (val->encoding == REDIS_ENCODING_HT) {
        dict *olddict = (dict *)val->ptr;
        cowDictArray *dar;

        dar = cowConvertDictToArray(olddict);
        newval = createObject(REDIS_SET, dar);
        newval->encoding = REDIS_ENCODING_HTARRAY;

        return newval;
    } else {
        /* error. unexpected encoding */
        return NULL;
    }
    return NULL;
}

/* convert a hash dictionary encoding to a dictionary array encoding */
robj *cowZSetCopy(robj *val) {
    robj *newval;
    if (val->encoding == REDIS_ENCODING_ZIPLIST) {
        size_t bytes;
        newval = createZsetZiplistObject();
        /* do raw memory copy */
        bytes = ziplistBlobLen(val->ptr);
        newval->ptr = zrealloc(newval->ptr, bytes);
        memcpy(newval->ptr, val->ptr, bytes);
        return newval;
    } else if (val->encoding == REDIS_ENCODING_SKIPLIST) {
        zset *oldzs = (zset *)val->ptr;
        cowDictZArray *dar;

        dar = cowConvertDictToZArray(oldzs->dict);
        newval = createObject(REDIS_ZSET, dar);
        newval->encoding = REDIS_ENCODING_HTZARRAY;

        return newval;
    } else {
        /* error. unexpected encoding */
        return NULL;
    }
    return NULL;
}

/* convert a hash dictionary encoding to a dictionary array encoding */
robj *cowHashCopy(robj *val) {
    robj *newval = createHashObject();
    if (val->encoding == REDIS_ENCODING_ZIPLIST) {
        size_t bytes;
        /* do raw memory copy */
        bytes = ziplistBlobLen(val->ptr);
        newval->ptr = zrealloc(newval->ptr, bytes);
        memcpy(newval->ptr, val->ptr, bytes);
        return newval;
    } else if (val->encoding == REDIS_ENCODING_HT) {
        dict *olddict = (dict *)val->ptr;
        cowDictArray *dar;

        dar = cowConvertDictToArray(olddict);
        newval = createObject(REDIS_HASH, dar);
        newval->encoding = REDIS_ENCODING_HTARRAY;

        return newval;
    } else {
        /* error. unexpected encoding */
        return NULL;
    }
    return NULL;
}

/* Make a readonly version of a dictionary of redis objects
   and make the existing dictionary not delete objects */
cowDictArray *copyReadonly_dictobj(dict *curdict, bkgdDbExt *extDict) {
    cowDictArray *dar;

    /* checks if copy needed. else return curdict */
    if (server.isBackgroundSaving == 0 || server.cowDictCopied == NULL) {
        return NULL;
    }

    /* create copy */
    dar = cowConvertDictToArray(curdict);

    if (extDict != NULL) {
        /* fix types to not delete while saving */
        extDict->savedType = curdict->type;
        curdict->type = extDict->cowType;
    }

    return dar;
}

/* if copy on write active, then ensure there is a
   copy of the value that is safe to modify or delete,
   and update DB dict entry to refer to this value*/
robj *cowEnsureWriteCopy(redisDb *db, robj *key, robj *val) {
    long long sttime;

    if (server.isBackgroundSaving == 0 ||
        server.cowDictCopied == NULL) {
        /* no copy needed */
        return val;
    } else {
        sds keyname;
        robj *newval = NULL;

        sttime = ustime();
        /* first ensure DB dict readonly copy exists */
        cowLock();
        if (server.cowSaveDbExt[db->id].dictArray == NULL) {
            /* make clone with modified cow destructors for db dict */
            server.cowSaveDbExt[db->id].dictArray = copyReadonly_dictobj(server.db[db->id].dict,
                                                        &server.cowSaveDbExt[db->id]);

            /* migrate iterator */
            roDBMigrateIterator(server.db[db->id].dict, server.cowSaveDbExt[db->id].dictArray);
        }
        cowUnlock();

        if (val == NULL || key == NULL) {
            return NULL;
        }
        if (dictFind(server.cowDictCopied, (sds)key->ptr) != NULL) {
            /* already copied */
            return val;
        }

        /* need to duplicate object, add key to cowDictCopied,
           add original to deferred delete list, and update db entry */
        cowLock();
        switch (val->type) {
        case REDIS_STRING:
            /* updates always duplicate, original uses defered delete destructor */
            break;
        case REDIS_LIST:
            newval = cowListCopy(val);
            break;
        case REDIS_SET:
            newval = cowSetCopy(val);
            break;
        case REDIS_ZSET:
            newval = cowZSetCopy(val);
            break;
        case REDIS_HASH:
            newval = cowHashCopy(val);
            break;
        default:
            break;
        }
        cowUnlock();

        if (newval == NULL) {
            /* no duplicate needed. return original */
            return val;
        }

        /* add key to copied dictionary to avoid extra copies */
        keyname = sdsdup((sds)key->ptr);
        dictAdd(server.cowDictCopied, keyname, NULL);

        /* rewitten iterators are added to converted for lookup
         * during save. For other objects, replace DB entry */
        if (newval->encoding == REDIS_ENCODING_HTARRAY ||
            newval->encoding == REDIS_ENCODING_LINKEDLISTARRAY ||
            newval->encoding == REDIS_ENCODING_HTZARRAY) {
            cowLock();
            /* add value to converted dictionary for iterator lookup */
            dictAdd(server.cowDictConverted, keyname, newval);

            /* migrate current iterator */
            if (newval->encoding == REDIS_ENCODING_HTARRAY) {
                roDictMigrateIterator((dict *)val->ptr, (cowDictArray *)newval->ptr);
            } else if (newval->encoding == REDIS_ENCODING_LINKEDLISTARRAY) {
                roListMigrateIterator((list *)val->ptr, (cowListArray *)newval->ptr);
            } else if (newval->encoding == REDIS_ENCODING_HTZARRAY) {
                roZDictMigrateIterator((dict *)val->ptr, (cowDictZArray *)newval->ptr);
            }
            cowUnlock();
        } else {
            /* replace active DB entry */
            dbOverwrite(db, key, newval);
            val = newval;
        }

        return val;
    }
}


/* copy a dictionary of redis objects
   Assumes copied directory uses COW destructors */
dict *copyonwrite_dictobj(dict *curdict, bkgdDbExt *extDict) {
    dict *newdict;
    dictIterator * di;
    dictEntry *de;

    /* checks if copy needed. else return curdict */
    if (server.isBackgroundSaving == 0 || server.cowDictCopied == NULL) {
        return curdict;
    }

    /* create copy */
    newdict = dictCreate(curdict->type, curdict->privdata);
    if (newdict != NULL) {
        /* copy all entries without refcounting or copying values */
        /* can't just memcpy the whole dictionary because entries are allocated */
        di = dictGetSafeIterator(curdict);
        while((de = dictNext(di)) != NULL) {
            dictAdd(newdict, de->key, de->v.val);
        }
        dictReleaseIterator(di);

        if (extDict != NULL) {
            /* fix types to not delete while saving */
            extDict->savedType = newdict->type;
            newdict->type = extDict->cowType;
            curdict->type = extDict->readonlyType;
        }
    }

    return newdict;
}


void restore_dictobj(dict *curdict, bkgdDbExt *extDict) {
    if (extDict != NULL && extDict->savedType != NULL) {
        curdict->type = extDict->savedType;
        extDict->savedType = NULL;
    }
}


/* if copy on write active, then ensure there is a
   copy of the value that is safe to modify or delete,
   and update DB dict entry to refer to this value*/
void cowEnsureExpiresCopy(redisDb *db) {
    long long sttime;

    if (server.isBackgroundSaving == 0 ||
        server.cowDictCopied == NULL ||
        server.cowSaveDb[db->id].expires == NULL) {
        /* no copy needed */
        return;
    } else {
        /* ensure DB expires is copied */
        if (server.cowSaveDb[db->id].expires == server.db[db->id].expires) {
            sttime = ustime();
            server.db[db->id].expires = copyonwrite_dictobj(server.cowSaveDb[db->id].expires, NULL);
            redisLog(REDIS_NOTICE, "elapsed COW DB expires time %d", (unsigned int)(ustime() - sttime));
        }
    }
}

/* global init function */
void cowInit(void) {
    int j;
    server.isBackgroundSaving = 0;
    server.cowDictCopied = NULL;
    server.cowDictConverted = NULL;
    server.cowSaveDbExt = (bkgdDbExt *)zmalloc(sizeof(bkgdDbExt)*server.dbnum);
    server.cowSaveDb = (redisDb *)zmalloc(sizeof(redisDb)*server.dbnum);

    deferSdsDelete = listCreate();
    deferObjDelete = listCreate();

    for (j = 0; j < server.dbnum; j++) {
        server.cowSaveDb[j].dict = NULL;
        server.cowSaveDb[j].expires = NULL;
        server.cowSaveDb[j].blocking_keys = NULL;
        server.cowSaveDb[j].watched_keys = NULL;
        server.cowSaveDb[j].id = j;
        server.cowSaveDbExt[j].savedType = NULL;
        server.cowSaveDbExt[j].cowType = &dbDeferDictType;
        server.cowSaveDbExt[j].readonlyType = &dbDeferDictType;
        server.cowSaveDbExt[j].dictArray = NULL;
        server.cowSaveDbExt[j].id = j;
    }

    server.cowCurIters.curDbDictIter = NULL;
    server.cowCurIters.curObjDictIter = NULL;
    server.cowCurIters.curObjListIter = NULL;
    server.cowCurIters.curObjZDictIter = NULL;
    server.cowCurIters.curObjHashIter = NULL;
    InitializeCriticalSectionAndSpinCount(&server.cowCurIters.csMigrate, 500);

}

/* release memory allocated for copy on write during background save */
void cowBkgdSaveReset() {
    int j;
    listNode *ln;

    if (server.cowDictCopied != NULL) {
        for (j = 0; j < server.dbnum; j++) {
            if (server.cowSaveDb[j].dict != NULL) {
                /* restore normal dictionary destructors */
                restore_dictobj(server.db[j].dict, &server.cowSaveDbExt[j]);
                server.cowSaveDb[j].dict = NULL;
            }

            if (server.cowSaveDbExt[j].dictArray != NULL) {
                cowReleaseDictArray(server.cowSaveDbExt[j].dictArray);
                server.cowSaveDbExt[j].dictArray = NULL;
            }

            if (server.cowSaveDb[j].expires != NULL &&
                server.cowSaveDb[j].expires != server.db[j].expires) {
                dictRelease(server.cowSaveDb[j].expires);
                server.cowSaveDb[j].expires = NULL;
            }
        }
    }

    server.cowCurIters.curDbDictIter = NULL;
    server.cowCurIters.curObjDictIter = NULL;
    server.cowCurIters.curObjZDictIter = NULL;
    server.cowCurIters.curObjListIter = NULL;
    server.cowCurIters.curObjHashIter = NULL;

    /* cleanup table of copied items */
    if (server.cowDictCopied != NULL) {
        dictRelease(server.cowDictCopied);
        server.cowDictCopied = NULL;
    }

    if (server.cowDictConverted != NULL) {
        dictRelease(server.cowDictConverted);
        server.cowDictConverted = NULL;
    }

    /* delete all deferred items */
    redisLog(REDIS_NOTICE, "cowBkgdSaveReset deleting %d SDS and %d obj items",
                listLength(deferSdsDelete), listLength(deferObjDelete));
    while ( (ln = listFirst(deferSdsDelete)) != NULL) {
        sdsfree((sds)(ln->value));
        listDelNode(deferSdsDelete, ln);
    }
    while ( (ln = listFirst(deferObjDelete)) != NULL) {
        if (ln->value != NULL) {
            decrRefCount(ln->value);
        }
        listDelNode(deferObjDelete, ln);
    }
}

/* requires sync with main thread */
void cowBkgdSaveStart() {
    int j;

    cowBkgdSaveReset();
    server.cowDictCopied = dictCreate(&ptrDictType, NULL);
    server.cowDictConverted = dictCreate(&copiedCollectionDictType, NULL);
    server.isBackgroundSaving = 1;
    for (j = 0; j < server.dbnum; j++) {
        /* copy dictionary references for saving */
        server.cowSaveDb[j].dict = server.db[j].dict;
        server.cowSaveDb[j].expires = server.db[j].expires;
        server.cowSaveDb[j].blocking_keys = server.db[j].blocking_keys;
        server.cowSaveDb[j].watched_keys = server.db[j].watched_keys;
    }
}

/* requires sync with main thread */
void cowBkgdSaveStop() {
    server.isBackgroundSaving = 0;
    cowBkgdSaveReset();
}



/* get converted object for saving */
void *getRoConvertedObj(void *key, void *o) {
    cowLock();
    if (server.cowDictConverted != NULL) {
        dictEntry *de;
        de = dictFind(server.cowDictConverted, key);
        if (de != NULL) {
            o = de->v.val;
        }
    }
    cowUnlock();
    return o;
}


/* Iterators for saving */

size_t roDBDictSize(int id) {
    if (server.isBackgroundSaving != 0) {
        if (server.cowSaveDbExt[id].dictArray != NULL) {
            return server.cowSaveDbExt[id].dictArray->numele;
        }
    }
    return dictSize(server.db[id].dict);
}

/* iterator for DB dictionary */
roDictIter *roDBGetIterator(int id) {
    roDictIter *iter;
    iter = (roDictIter *)zmalloc(sizeof(roDictIter));

    cowLock();
    iter->di = dictGetSafeIterator(server.db[id].dict);
    iter->hdict = server.db[id].dict;
    iter->ar = NULL;
    iter->pos = 0;

    if (server.isBackgroundSaving != 0) {
        if (server.cowSaveDbExt[id].dictArray != NULL) {
            iter->ar = server.cowSaveDbExt[id].dictArray;
        }
        server.cowCurIters.curDbDictIter = iter;
    }
    cowUnlock();
    return iter;
}

/* iterator for set (not DB) */
roDictIter *roDictGetIterator(dict *d, cowDictArray *ro) {
    roDictIter *iter;
    iter = (roDictIter *)zmalloc(sizeof(roDictIter));

    cowLock();
    if (d != NULL) {
        iter->di = dictGetIterator(d);
    } else {
        iter->di = NULL;
    }
    iter->hdict = d;
    iter->ar = ro;
    iter->pos = 0;
    if (server.isBackgroundSaving != 0) {
        server.cowCurIters.curObjDictIter = iter;
    }
    cowUnlock();

    return iter;
}

dictEntry *roDictNext(roDictIter *iter) {
    dictEntry *de = NULL;

    cowLock();
    if (iter->ar != NULL) {
        if (iter->pos >= 0 && iter->pos < iter->ar->numele) {
            de = &iter->ar->de[iter->pos];
            iter->pos++;
        }
    } else if (iter->di != NULL) {
        de = dictNext(iter->di);
        iter->pos++;
    }
    if (de == NULL) {
        iter->pos = -1;
    }
    cowUnlock();

    return de;
}

void roDictReleaseIterator(roDictIter *iter) {
    server.cowCurIters.curObjDictIter = NULL;
    if (iter->di != NULL) {
        dictReleaseIterator(iter->di);
    }
    zfree(iter);
}


/* iterator for zset */
roZDictIter *roZDictGetIterator(dict *d, cowDictZArray *ro) {
    roZDictIter *iter;
    iter = (roZDictIter *)zmalloc(sizeof(roZDictIter));

    cowLock();
    if (d != NULL) {
        iter->di = dictGetIterator(d);
    } else {
        iter->di = NULL;
    }
    iter->hdict = d;
    iter->ar = ro;
    iter->pos = 0;
    if (server.isBackgroundSaving != 0) {
        server.cowCurIters.curObjZDictIter = iter;
    }
    cowUnlock();

    return iter;
}

dictEntry *roZDictNext(roZDictIter *iter) {
    dictEntry *de = NULL;

    cowLock();
    if (iter->ar != NULL) {
        if (iter->pos >= 0 && iter->pos < iter->ar->numele) {
            de = &iter->ar->zde[iter->pos].de;
            iter->pos++;
        }
    } else if (iter->di != NULL) {
        de = dictNext(iter->di);
        iter->pos++;
    }
    if (de == NULL) {
        iter->pos = -1;
    }
    cowUnlock();

    return de;
}

void roZDictReleaseIterator(roZDictIter *iter) {
    server.cowCurIters.curObjZDictIter = NULL;
    if (iter->di != NULL) {
        dictReleaseIterator(iter->di);
    }
    zfree(iter);
}


/* iterator for list */
roListIter *roListGetIterator(list *l, cowListArray *ro) {
    roListIter *iter;
    iter = (roListIter *)zmalloc(sizeof(roListIter));

    roListRewind(l, ro, iter);

    return iter;
}

void roListRewind(list *l, cowListArray *ro, roListIter *iter) {
    cowLock();
    if (l != NULL) {
        listRewind(l, &iter->li);
    }
    iter->olist = l;
    iter->ar = ro;
    iter->pos = 0;
    if (server.isBackgroundSaving != 0) {
        server.cowCurIters.curObjListIter = iter;
    }
    cowUnlock();
}

listNode *roListNext(roListIter *iter) {
    listNode *ln = NULL;

    cowLock();
    if (iter->ar != NULL) {
        if (iter->pos >= 0 && iter->pos < iter->ar->numele) {
            ln = &iter->ar->le[iter->pos];
            iter->pos++;
        }
    } else {
        ln = listNext(&iter->li);
        iter->pos++;
    }
    if (ln == NULL) {
        iter->pos = -1;
    }
    cowUnlock();

    return ln;
}

void roListReleaseIterator(roListIter *iter) {
    server.cowCurIters.curObjListIter = NULL;
    zfree(iter);
}

/* iterator for hash */
/* Because the hashTypeIterator is defined later in redis.h
   member di is defined as a void*. Need to do type casting */
roHashIter *roHashGetIterator(void *subject, cowDictArray *ro) {
    roHashIter *iter;
    iter = (roHashIter *)zmalloc(sizeof(roHashIter));

    cowLock();
    if (subject != NULL) {
        iter->di = hashTypeInitIterator((robj *)subject);
    } else {
        iter->di = NULL;
    }
    iter->ar = ro;
    iter->pos = 0;
    if (server.isBackgroundSaving != 0) {
        server.cowCurIters.curObjHashIter = iter;
    }
    cowUnlock();

    return iter;
}

int roHashNext(roHashIter *iter) {
    int rc;

    cowLock();
    if (iter->ar != NULL) {
        if (iter->pos >= 0 && (size_t)iter->pos < iter->ar->numele) {
            iter->pos++;
            rc = REDIS_OK;
        } else {
            rc = REDIS_ERR;
        }
    } else {
        rc = hashTypeNext((hashTypeIterator *)(iter->di));
        iter->pos++;
    }
    cowUnlock();

    return rc;
}

int roHashGetEncoding(roHashIter *iter) {
    int rc;

    cowLock();
    if (iter->ar != NULL) {
        rc = REDIS_ENCODING_HTARRAY;
    } else {
        rc = ((hashTypeIterator *)(iter->di))->encoding;
    }
    cowUnlock();

    return rc;
}

void roHashGetCurrentFromArray(roHashIter *iter, int what, void **dst) {
    redisAssert(iter->ar != NULL);

    if (what & REDIS_HASH_KEY) {
        *dst = dictGetKey(&iter->ar->de[iter->pos]);
    } else {
        *dst = dictGetVal(&iter->ar->de[iter->pos]);
    }
}

void *roHashGetHashIter(roHashIter *iter) {
    return (hashTypeIterator *)iter->di;
}

void roHashReleaseIterator(roHashIter *iter) {
    server.cowCurIters.curObjHashIter = NULL;
    zfree(iter);
}


#endif

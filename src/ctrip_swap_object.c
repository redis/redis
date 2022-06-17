/* Copyright (c) 2021, ctrip.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "ctrip_swap.h"

/*-----------------------------------------------------------------------------
 * db.evict related API
 *----------------------------------------------------------------------------*/
robj *lookupEvictKey(redisDb *db, robj *key) {
    return dictFetchValue(db->evict,key->ptr);
}

void dbAddEvict(redisDb *db, robj *key, robj *evict) {
    sds copy = sdsdup(key->ptr);
    int retval = dictAdd(db->evict, copy, evict);
    serverAssertWithInfo(NULL,key,retval == DICT_OK);
}

/* Note: 
 * - only evict object shell will be deleted.
 * - corresponding expire will be deleted (expire must not survive alone) */
int dbDeleteEvict(redisDb *db, robj *key) {
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);
    dictEntry *de = dictUnlink(db->evict,key->ptr);
    if (de) {
        robj *evict = dictGetVal(de);
        /* Tells the module that the key has been unlinked from the database. */
        moduleNotifyKeyUnlink(key,evict);
        /* Only free evict object shell */ 
        if (evict->type == OBJ_MODULE) {
            moduleValue *mv = evict->ptr;
            if (mv) mv->value = NULL;
        } else {
            evict->ptr = NULL;
        }
        dictFreeUnlinkedEntry(db->evict,de);
        return 1;
    } else {
        return 0;
    }
}

void dbSetDirty(redisDb *db, robj *key) {
    robj *o = lookupKey(db,key,LOOKUP_NOTOUCH);
    if (o) setObjectDirty(o);
}

int objectIsDirty(robj *o) {
    return o->dirty;
}

/* Note that db.meta is a satellite dict just like db.expire. */ 
/* Db->meta */
int dictExpandAllowed(size_t moreMem, double usedRatio);

void dictObjectMetaFree(void *privdata, void *val) {
    DICT_NOTUSED(privdata);
    freeObjectMeta(val);
}

dictType dbMetaDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    NULL,                       /* key destructor */
    dictObjectMetaFree,         /* val destructor */
    dictExpandAllowed           /* allow to expand */
};

objectMeta *createObjectMeta(size_t len) {
    objectMeta *m = zmalloc(sizeof(objectMeta));
    m->len = len;
    m->version = server.meta_version++;
    return m;
}

objectMeta *dupObjectMeta(objectMeta *ms) {
    objectMeta *m = zmalloc(sizeof(objectMeta));
    m->len = ms->len;
    m->version = ms->version;
    return m;
}

void freeObjectMeta(objectMeta *m) {
    zfree(m);
}

objectMeta *lookupMeta(redisDb *db, robj *key) {
    return dictFetchValue(db->meta,key->ptr);
}

void dbAddMeta(redisDb *db, robj *key, objectMeta *m) {
    dictEntry *kde;
    kde = dictFind(db->dict,key->ptr);
	if (!kde) kde = dictFind(db->evict,key->ptr);
    serverAssertWithInfo(NULL,key,kde != NULL);
    serverAssert(dictAdd(db->meta,dictGetKey(kde),m) == DICT_OK);
}

int dbDeleteMeta(redisDb *db, robj *key) {
    if (dictSize(db->meta) == 0) return 0;
    return dictDelete(db->meta,key->ptr) == DICT_OK ? 1 : 0;
}

void dbSetBig(redisDb *db, robj *key) {
    robj *o = lookupKey(db,key,LOOKUP_NOTOUCH);
    if (o) setObjectBig(o);
}

int objectIsBig(robj *o) {
    return o->big;
}

#ifdef REDIS_TEST
int swapObjectTest(int argc, char *argv[], int accurate) {
    initTestRedisServer();
    redisDb* db = server.db + 0;
    int error = 0;
    UNUSED(argc), UNUSED(argv), UNUSED(accurate);

    TEST("object: meta can be deleted specificly or by effect") {
        char *key1raw = "key1", *val1raw = "val1";
        robj *key1 = createStringObject(key1raw, strlen(key1raw)); 
        robj *val1 = createStringObject(val1raw, strlen(val1raw)); 

        dbAdd(db,key1,val1);
        dbAddMeta(db,key1,createObjectMeta(1));
        test_assert(lookupMeta(db,key1) != NULL);
        dbDeleteMeta(db,key1);
        test_assert(lookupMeta(db,key1) == NULL);
        dbAddMeta(db,key1,createObjectMeta(1));
        test_assert(lookupMeta(db,key1) != NULL);
        dbDelete(db,key1);
        test_assert(lookupMeta(db,key1) == NULL);
    }
    return error;
}
#endif

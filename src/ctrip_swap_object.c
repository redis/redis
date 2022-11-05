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

void dbSetDirty(redisDb *db, robj *key) {
    robj *o = lookupKey(db,key,LOOKUP_NOTOUCH);
    if (o) setObjectDirty(o);
}

int objectIsDirty(robj *o) {
    return o->dirty;
}

/* objectMeta */
static inline objectMetaType *getObjectMetaType(int object_type) {
    objectMetaType *omtype = NULL;
    switch (object_type) {
    case OBJ_STRING:
        omtype = NULL;
        break;
    case OBJ_HASH:
    case OBJ_SET:
    case OBJ_ZSET:
        omtype = &lenObjectMetaType;
        break;
    case OBJ_LIST:
        omtype = &listObjectMetaType;
        break;
    default:
        break;
    }
    return omtype;
}

objectMeta *createObjectMeta(int object_type, uint64_t version) {
	objectMeta *m = zmalloc(sizeof(objectMeta));
    m->version = version;
    m->object_type = object_type;
    m->len = 0;
    return m;
}

void freeObjectMeta(objectMeta *object_meta) {
    objectMetaType *omtype;
    if (object_meta == NULL) return;
    omtype = getObjectMetaType(object_meta->object_type);
    if (omtype != NULL && omtype->free) omtype->free(object_meta);
    zfree(object_meta);
}

objectMeta *dupObjectMeta(objectMeta *object_meta) {
    objectMeta *dup_meta;
    objectMetaType *omtype;
    if (object_meta == NULL) return NULL;
    omtype = getObjectMetaType(object_meta->object_type);
    dup_meta = zmalloc(sizeof(objectMeta));
    memcpy(dup_meta,object_meta,sizeof(objectMeta));
    if (omtype != NULL && omtype->duplicate) {
        dup_meta->ptr = 0;
        omtype->duplicate(dup_meta,object_meta);
    }
    return dup_meta;
}

int buildObjectMeta(int object_type, uint64_t version, const char *extend,
        size_t extlen, OUT objectMeta **pobject_meta) {
    objectMeta *object_meta;
    objectMetaType *omtype = getObjectMetaType(object_type);

    if (omtype == NULL || omtype->decodeObjectMeta == NULL || extend == NULL) {
        if (pobject_meta) *pobject_meta = NULL;
        return 0;
    }

    if (pobject_meta == NULL) return 0;

    object_meta = createObjectMeta(object_type, version);
    if (omtype->decodeObjectMeta(object_meta,extend,extlen)) {
        zfree(object_meta);
        *pobject_meta = NULL;
        return -1;
    }

    *pobject_meta = object_meta;
    return 0;
}

sds objectMetaEncode(struct objectMeta *object_meta) {
    serverAssert(object_meta);
    objectMetaType *omtype = getObjectMetaType(object_meta->object_type);
    if (omtype->encodeObjectMeta) {
        return omtype->encodeObjectMeta(object_meta);
    } else {
        return NULL;
    }
}

int objectMetaDecode(struct objectMeta *object_meta, const char *extend, size_t extlen) {
    serverAssert(object_meta);
    objectMetaType *omtype = getObjectMetaType(object_meta->object_type);
    if (omtype->decodeObjectMeta) {
        return omtype->decodeObjectMeta(object_meta,extend,extlen);
    } else {
        return -1;
    }
}

int keyIsHot(objectMeta *object_meta, robj *value) {
    swapObjectMeta som;
    objectMetaType *type;
    if (value == NULL) return 0;
    if (object_meta == NULL) return 1;
    type = getObjectMetaType(object_meta->object_type);
    initStaticSwapObjectMeta(som,type,object_meta,value);
    return swapObjectMetaIsHot(&som);
}

struct listMeta;
sds listMetaDump(sds result, struct listMeta *lm);

sds dumpObjectMeta(objectMeta *object_meta) {
    sds result = sdsempty();
    if (object_meta == NULL) {
        result = sdscat(result,"<nil>");
        return result;
    }

    objectMetaType *omtype = getObjectMetaType(object_meta->object_type);
    result = sdscatprintf(result,"version=%lu",object_meta->version);
    if (omtype == &lenObjectMetaType){
        result = sdscatprintf(result,"len=%ld",(long)object_meta->len);
    } else if (omtype == &listObjectMetaType) {
        result = sdscat(result,"list_meta=");
        struct listMeta *meta = objectMetaGetPtr(object_meta);;
        result = listMetaDump(result,meta);
    } else {
        result = sdscat(result,"list_meta=<unknown>");
    }
    return result;
}

/* lenObjectMeta, used by hash/set/zset */

objectMeta *createLenObjectMeta(int object_type, uint64_t version, size_t len) {
    objectMeta *m = createObjectMeta(object_type,version);
	m->len = len;
	return m;
}

sds encodeLenObjectMeta(struct objectMeta *object_meta) {
    return object_meta ? rocksEncodeObjectMetaLen(object_meta->len) : NULL;
}

int decodeLenObjectMeta(struct objectMeta *object_meta, const char *extend, size_t extlen) {
    long len = rocksDecodeObjectMetaLen(extend,extlen);
    if (len < 0) return -1;
    object_meta->len = len;
    return 0;
}

int lenObjectMetaIsHot(objectMeta *object_meta, robj *value) {
    serverAssert(value && object_meta && object_meta->len >= 0);
    return object_meta->len == 0;
}

objectMetaType lenObjectMetaType = {
    .encodeObjectMeta = encodeLenObjectMeta,
    .decodeObjectMeta = decodeLenObjectMeta,
    .objectIsHot = lenObjectMetaIsHot,
    .free = NULL,
    .duplicate = NULL,
};

/* Note that db.meta is a satellite dict just like db.expire. */ 
/* Db->meta */
int dictExpandAllowed(size_t moreMem, double usedRatio);

void dictObjectMetaFree(void *privdata, void *val) {
    DICT_NOTUSED(privdata);
    freeObjectMeta(val);
}

dictType objectMetaDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    NULL,                       /* key destructor */
    dictObjectMetaFree,         /* val destructor */
    dictExpandAllowed           /* allow to expand */
};

objectMeta *lookupMeta(redisDb *db, robj *key) {
    return dictFetchValue(db->meta,key->ptr);
}

void dbAddMeta(redisDb *db, robj *key, objectMeta *m) {
    dictEntry *kde;
    kde = dictFind(db->dict,key->ptr);
    serverAssertWithInfo(NULL,key,kde != NULL);
    serverAssert(dictAdd(db->meta,dictGetKey(kde),m) == DICT_OK);
}

int dbDeleteMeta(redisDb *db, robj *key) {
    if (dictSize(db->meta) == 0) return 0;
    return dictDelete(db->meta,key->ptr) == DICT_OK ? 1 : 0;
}

sds objectDump(robj *o) {
    sds repr = sdsempty();

    repr = sdscatprintf(repr,"type:%s, ", getObjectTypeName(o));
    switch (o->encoding) {
    case OBJ_ENCODING_INT:
        repr = sdscatprintf(repr, "encoding:int, value:%ld", (long)o->ptr);
        break;
    case OBJ_ENCODING_EMBSTR:
        repr = sdscatprintf(repr, "encoding:emedstr, value:%.*s", (int)sdslen(o->ptr), (sds)o->ptr);
        break;
    case OBJ_ENCODING_RAW:
        repr = sdscatprintf(repr, "encoding:raw, value:%.*s", (int)sdslen(o->ptr), (sds)o->ptr);
        break;
    default:
        repr = sdscatprintf(repr, "encoding:%d, value:nan", o->encoding);
        break;
    }
    return repr;
}

/* For big Hash/Set/Zset object, object might changed by swap thread in
 * createOrMergeObject, so iterating those big objects in main thread without
 * requestGetIOAndLock is not safe. intead we just estimate those object size. */
#define OBJECT_ESTIMATE_SIZE_SAMPLE 5
size_t objectComputeSize(robj *o, size_t sample_size);
size_t objectEstimateSize(robj *o) {
    size_t asize = 0;

    switch (o->type) {
    case OBJ_STRING:
        asize = objectComputeSize(o,OBJECT_ESTIMATE_SIZE_SAMPLE);
        break;
    case OBJ_HASH:
        /* Hash may convert encoding in swap thread, so we can't safely
         * estimate hash size by encoding. */
        asize = DEFAULT_HASH_FIELD_COUNT*DEFAULT_HASH_FIELD_SIZE;
        break;
    case OBJ_SET:
        /* similar to Hash */
        asize = DEFAULT_SET_MEMBER_COUNT*DEFAULT_SET_MEMBER_SIZE;
        break;
    case OBJ_LIST:
        serverAssert(o->encoding == OBJ_ENCODING_QUICKLIST);
        asize = listTypeLength(o)*DEFAULT_LIST_ELE_SIZE;
        break;
    case OBJ_ZSET:
        asize = DEFAULT_ZSET_MEMBER_COUNT*DEFAULT_ZSET_MEMBER_SIZE;
        break;
    case OBJ_STREAM:
        asize = objectComputeSize(o,OBJECT_ESTIMATE_SIZE_SAMPLE);
        break;
    case OBJ_MODULE:
        /*TODO support module*/
        asize = objectComputeSize(o,OBJECT_ESTIMATE_SIZE_SAMPLE);
        break;
    }

    return asize;
}
size_t keyEstimateSize(redisDb *db, robj *key) {
    robj *val = lookupKey(db, key, LOOKUP_NOTOUCH);
    return val ? objectEstimateSize(val): 0;
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
        dbAddMeta(db,key1,createHashObjectMeta(0,1));
        test_assert(lookupMeta(db,key1) != NULL);
        dbDeleteMeta(db,key1);
        test_assert(lookupMeta(db,key1) == NULL);
        dbAddMeta(db,key1,createHashObjectMeta(0,1));
        test_assert(lookupMeta(db,key1) != NULL);
        dbDelete(db,key1);
        test_assert(lookupMeta(db,key1) == NULL);
    }
    return error;
}
#endif

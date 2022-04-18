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

#include "server.h"

robj *dupObjectShell(robj *o) {
    robj *e = createObject(o->type, NULL);
    e->encoding = o->encoding;
    e->lru = o->lru;
    return e;
}

robj *dupObjectWk(robj *o) {
    switch(o->type) {
    case OBJ_STRING:
        return dupStringObject(o);
    case OBJ_HASH:
        serverLog(LL_WARNING, "FATAL: hash dupObjectWk not implemented.");
        incrRefCount(o);
        return o;
    case OBJ_LIST:
    case OBJ_SET:
    case OBJ_ZSET:
    default:
        return NULL;
    }
}

int getObjectRdbType(robj *o) {
    switch (o->type) {
    case OBJ_STRING:
        return RDB_TYPE_STRING;
    case OBJ_LIST:
        if (o->encoding == OBJ_ENCODING_QUICKLIST)
            return RDB_TYPE_LIST_QUICKLIST;
        else
            serverPanic("Unknown list encoding");
    case OBJ_SET:
        if (o->encoding == OBJ_ENCODING_INTSET)
            return RDB_TYPE_SET_INTSET;
        else if (o->encoding == OBJ_ENCODING_HT)
            return RDB_TYPE_SET;
        else
            serverPanic("Unknown set encoding");
    case OBJ_ZSET:
        if (o->encoding == OBJ_ENCODING_ZIPLIST)
            return RDB_TYPE_ZSET_ZIPLIST;
        else if (o->encoding == OBJ_ENCODING_SKIPLIST)
            return RDB_TYPE_ZSET_2;
        else
            serverPanic("Unknown sorted set encoding");
    case OBJ_HASH:
        if (o->encoding == OBJ_ENCODING_ZIPLIST)
            return RDB_TYPE_HASH_ZIPLIST;
        else if (o->encoding == OBJ_ENCODING_HT)
            return RDB_TYPE_HASH;
        else
            serverPanic("Unknown hash encoding");
    case OBJ_STREAM:
        return RDB_TYPE_STREAM_LISTPACKS;
    case OBJ_MODULE:
        return RDB_TYPE_MODULE_2;
    default:
        serverPanic("Unknown object type");
    }
    return -1; /* avoid warning */
}

/* NOTE: newval ownership moved */
robj *createSwapInObject(robj *newval, robj *evict) {
    robj *swapin = newval;

    serverAssert(evict);
    serverAssert(evict->type == newval->type);

    /* Copy swapin object before modifing If newval is shared object. */
    if (newval->refcount > 1) swapin = dupObjectWk(newval);

    swapin->lru = evict->lru;
    swapin->dirty = 0;
    swapin->scs = 0;
    swapin->evicted = 0;

    return swapin;
}

robj *createSwapOutObject(robj *value, robj *evict) {
    robj *swapout;

    serverAssert(value);
    serverAssert(evict == NULL || evict->evicted == 0);

    if (evict == NULL) {
        swapout = createObject(value->type, NULL);
    } else {
        incrRefCount(evict);
        swapout = evict;
    }

    swapout->lru = value->lru;
    swapout->type = value->type;
    swapout->encoding = value->encoding;
    swapout->evicted = 1;

    return swapout;
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

/* Move key from db.evict to db.dict. (newval ownership moved) */
void dbSwapInWk(redisDb *db, robj *key, robj *newval) {
    robj *evict, *swapin;

    evict = lookupEvict(db,key);
    swapin = createSwapInObject(newval, evict);
    dbAdd(db,key,swapin);

    /* reserve scs */
    if (evict->scs) {
        evict->evicted = 0;
    } else {
        dbDeleteEvict(db,key);
    }
}

void *lookupSwappingClientsWk(redisDb *db, robj *key) {
    robj *evict = lookupEvictSCS(db, key);
    return evictObjectGetSCS(evict);
}

void setupSwappingClientsWk(redisDb *db, robj *key, void *scs) {
    robj *evict, *value;

    value = lookupKey(db, key, LOOKUP_NOTOUCH);
    evict = lookupEvict(db, key);
    serverAssert(value || evict);

    if (evict != NULL) {
        if (scs != NULL) {
            /* overwrite with new scs */
            evictObjectSetSCS(evict, scs);
        } else {
            if (!objectIsEvicted(evict)) {
                /* delete key.evict if key not evicted and new scs is NULL */
                dbDeleteEvict(db, key);
            } else {
                /* clear scs and scs flag if key evicted and new scs is NULL */ 
                evictObjectSetSCS(evict, NULL);
            }
        }
    } else {
        if (scs != NULL) {
            /* setup new evict object. */
            evict = dupObjectShell(value);
            evictObjectSetSCS(evict, scs);
            dbAddEvict(db,key,evict);
        }
    }
}

void getDataSwapsWk(robj *key, int mode, getSwapsResult *result) {
    UNUSED(mode);
    incrRefCount(key);
    getSwapsAppendResult(result, key, NULL, NULL);
}

sds encodeKeyWk(robj *key, robj *value, robj* evict) {
    char *typename;
    sds rawkey;

    serverAssert(value || evict);

    if (value) typename = getObjectTypeName(value);
    else typename = getObjectTypeName(evict);

    rawkey = sdscat(sdsempty(), typename);
    rawkey = sdscatsds(rawkey, key->ptr);
    return rawkey;
}

sds encodeValRdbWk(robj *value) {
    rio sdsrdb;

    rioInitWithBuffer(&sdsrdb,sdsempty());
    rdbSaveObject(&sdsrdb,value,NULL);

    sds repr = objectDump(value);
    serverLog(LL_WARNING, "[xxx] encode %s => %s", repr, sdsrdb.io.buffer.ptr);
    serverLogHexDump(LL_WARNING, "[xxx] hex", sdsrdb.io.buffer.ptr, sdslen(sdsrdb.io.buffer.ptr));
    sdsfree(repr);
    
    /* rawvalue ownership transfered to caller */
    return sdsrdb.io.buffer.ptr;
}

robj *decodeValRdbWk(int rdbtype, sds raw) {
    robj *value;
    rio sdsrdb;

    rioInitWithBuffer(&sdsrdb,raw);
    value = rdbLoadObject(rdbtype,&sdsrdb,NULL,NULL);

    serverLogHexDump(LL_WARNING, "[xxx] hex", raw, sdslen(raw));
    sds repr = objectDump(value);
    serverLog(LL_WARNING, "[xxx] decode %s => %s", raw, repr);
    sdsfree(repr);

    return value;
}

typedef struct swapInWkPd {
    int rdbtype;
    void *key;
} swapInWkPd;

void swapInWk(void *ctx, int action, char* _rawkey, char *_rawval, void *_pd) {
    redisDb *db = ctx;
    sds rawval = _rawval;
    robj *val;
    swapInWkPd *pd = _pd;

    UNUSED(action);
    UNUSED(_rawkey);

    val = decodeValRdbWk(pd->rdbtype, rawval);

    sds repr = objectDump(val);
    serverLog(LL_WARNING, "[xxx] swapin: %s", repr);
    sdsfree(repr);

    if (val == NULL) {
        serverLogHexDump(LL_WARNING, "swapInWk decode key failed",
                rawval, sdslen(rawval));
    } else {
        dbSwapInWk(db, pd->key, val);
    }

    decrRefCount(pd->key);
    zfree(pd);
}

void dbSwapOutWk(redisDb *db, robj *key) {
    robj *value, *evict, *swapout;

    value = lookupKey(db,key,LOOKUP_NOTOUCH);
    evict = lookupEvict(db,key);

    swapout = createSwapOutObject(value, evict);
    if (evict) dbDeleteEvict(db, key);
    dictDelete(db->dict, key->ptr);
    dbAddEvict(db, key, swapout);
}

void swapOutWk(void *ctx, int action, char* _rawkey, char *_rawval, void *pd) {
    redisDb *db = ctx;
    robj *key = pd;
    UNUSED(action);
    UNUSED(_rawkey);
    UNUSED(_rawval);
    dbSwapOutWk(db, key);
    decrRefCount(key);
}

int swapAnaWk(struct redisCommand *cmd, redisDb *db, robj *key, int *action,
        char **rawkey, char **rawval, dataSwapFinishedCallback *cb, void **pd)
{
    robj *value, *evict;
    int swapaction = cmd->swap_action;
    value = lookupKey(db,key,LOOKUP_NOTOUCH);
    evict = lookupEvict(db,key);
    serverAssert(value || evict);

    /* NOTE that rawkey/rawval ownership are moved to rocks, which will be
     * freed when rocksSwapFinished. Also NOTE that if CRDT key expired, we
     * should swapin before delete because crdtPropagateExpire needs value. */ 
    if (keyIsExpired(db,key) && lookupEvictKey(db,key)) {
        swapInWkPd *sipd = zmalloc(sizeof(swapInWkPd));
        *action = SWAP_GET;
        *rawkey = encodeKeyWk(key,value,evict);
        *rawval = NULL;
        *cb = swapInWk;
        incrRefCount(key);
		sipd->key = key;
		sipd->rdbtype = value ? getObjectRdbType(value) : getObjectRdbType(evict);
        *pd = sipd;
    } else if (swapaction == SWAP_GET && lookupEvictKey(db,key)) {
        swapInWkPd *sipd = zmalloc(sizeof(swapInWkPd));
        *action = SWAP_GET;
        *rawkey = encodeKeyWk(key,value,evict);
        *rawval = NULL;
        *cb = swapInWk;
        incrRefCount(key);
		sipd->key = key;
		sipd->rdbtype = value ? getObjectRdbType(value) : getObjectRdbType(evict);
        *pd = sipd;
    } else if (swapaction == SWAP_PUT && value != NULL) {
        /* Only dirty key needs to swap out to rocksdb, non-dirty key could be
         * freed right away (no rocksdb IO needed). */
        if (!objectIsDirty(value)) {
            dbSwapOutWk(db,key);

            *action = SWAP_NOP;
            *rawkey = NULL;
            *rawval = NULL;
            *cb = NULL;
            *pd = NULL;
        } else {
            *action = SWAP_PUT;
            *rawkey = encodeKeyWk(key,value,evict);
            *rawval = encodeValRdbWk(value);
            *cb = swapOutWk;
            incrRefCount(key);
            *pd = key;
        }
    } else if (swapaction == SWAP_DEL) {
        *action = SWAP_DEL;
        *rawkey = encodeKeyWk(key,value,evict);
        *rawval = NULL;
        *cb = NULL;
        *pd = NULL;
    } else {
        *action = SWAP_NOP;
        *rawkey = NULL;
        *rawval = NULL;
        *cb = NULL;
        *pd = NULL;
    }

    return 0;
}

int complementWkRaw(void **pdupptr, char* _rawkey, char *_rawval, void *pd) {
    sds rawval = _rawval;
    UNUSED(_rawkey);
    UNUSED(pd);
    serverAssert(pdupptr && *pdupptr == NULL);
    *pdupptr = rawval;
    return 0;
}

void *getComplementSwapsWk(redisDb *db, robj *key, int mode, int *type,
        getSwapsResult *result, complementObjectFunc *comp, void **pd) {
    robj *value, *evict;
    UNUSED(mode);
    value = lookupKey(db,key,LOOKUP_NOTOUCH);
    evict = lookupEvict(db, key);
    serverAssert(value || evict);

    sds rawkey = encodeKeyWk(key,value,evict);
    /* NOTE that rawkey is sds if this is a complement swap. */
    getSwapsAppendResult(result, (robj*)rawkey, NULL, NULL);

    *type = COMP_TYPE_RAW;
    *comp = complementWkRaw;
    *pd = NULL;
    return NULL;
}

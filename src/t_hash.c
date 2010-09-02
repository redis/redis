#include "redis.h"

#include <math.h>

/*-----------------------------------------------------------------------------
 * Hash type API
 *----------------------------------------------------------------------------*/

/* Check the length of a number of objects to see if we need to convert a
 * zipmap to a real hash. Note that we only check string encoded objects
 * as their string length can be queried in constant time. */
void hashTypeTryConversion(robj *subject, robj **argv, int start, int end) {
    int i;
    if (subject->encoding != REDIS_ENCODING_ZIPMAP) return;

    for (i = start; i <= end; i++) {
        if (argv[i]->encoding == REDIS_ENCODING_RAW &&
            sdslen(argv[i]->ptr) > server.hash_max_zipmap_value)
        {
            convertToRealHash(subject);
            return;
        }
    }
}

/* Encode given objects in-place when the hash uses a dict. */
void hashTypeTryObjectEncoding(robj *subject, robj **o1, robj **o2) {
    if (subject->encoding == REDIS_ENCODING_HT) {
        if (o1) *o1 = tryObjectEncoding(*o1);
        if (o2) *o2 = tryObjectEncoding(*o2);
    }
}

/* Get the value from a hash identified by key. Returns either a string
 * object or NULL if the value cannot be found. The refcount of the object
 * is always increased by 1 when the value was found. */
robj *hashTypeGet(robj *o, robj *key) {
    robj *value = NULL;
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        unsigned char *v;
        unsigned int vlen;
        key = getDecodedObject(key);
        if (zipmapGet(o->ptr,key->ptr,sdslen(key->ptr),&v,&vlen)) {
            value = createStringObject((char*)v,vlen);
        }
        decrRefCount(key);
    } else {
        dictEntry *de = dictFind(o->ptr,key);
        if (de != NULL) {
            value = dictGetEntryVal(de);
            incrRefCount(value);
        }
    }
    return value;
}

/* Test if the key exists in the given hash. Returns 1 if the key
 * exists and 0 when it doesn't. */
int hashTypeExists(robj *o, robj *key) {
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        key = getDecodedObject(key);
        if (zipmapExists(o->ptr,key->ptr,sdslen(key->ptr))) {
            decrRefCount(key);
            return 1;
        }
        decrRefCount(key);
    } else {
        if (dictFind(o->ptr,key) != NULL) {
            return 1;
        }
    }
    return 0;
}

/* Add an element, discard the old if the key already exists.
 * Return 0 on insert and 1 on update. */
int hashTypeSet(robj *o, robj *key, robj *value) {
    int update = 0;
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        key = getDecodedObject(key);
        value = getDecodedObject(value);
        o->ptr = zipmapSet(o->ptr,
            key->ptr,sdslen(key->ptr),
            value->ptr,sdslen(value->ptr), &update);
        decrRefCount(key);
        decrRefCount(value);

        /* Check if the zipmap needs to be upgraded to a real hash table */
        if (zipmapLen(o->ptr) > server.hash_max_zipmap_entries)
            convertToRealHash(o);
    } else {
        if (dictReplace(o->ptr,key,value)) {
            /* Insert */
            incrRefCount(key);
        } else {
            /* Update */
            update = 1;
        }
        incrRefCount(value);
    }
    return update;
}

/* Delete an element from a hash.
 * Return 1 on deleted and 0 on not found. */
int hashTypeDelete(robj *o, robj *key) {
    int deleted = 0;
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        key = getDecodedObject(key);
        o->ptr = zipmapDel(o->ptr,key->ptr,sdslen(key->ptr), &deleted);
        decrRefCount(key);
    } else {
        deleted = dictDelete((dict*)o->ptr,key) == DICT_OK;
        /* Always check if the dictionary needs a resize after a delete. */
        if (deleted && htNeedsResize(o->ptr)) dictResize(o->ptr);
    }
    return deleted;
}

/* Return the number of elements in a hash. */
unsigned long hashTypeLength(robj *o) {
    return (o->encoding == REDIS_ENCODING_ZIPMAP) ?
        zipmapLen((unsigned char*)o->ptr) : dictSize((dict*)o->ptr);
}

hashTypeIterator *hashTypeInitIterator(robj *subject) {
    hashTypeIterator *hi = zmalloc(sizeof(hashTypeIterator));
    hi->encoding = subject->encoding;
    if (hi->encoding == REDIS_ENCODING_ZIPMAP) {
        hi->zi = zipmapRewind(subject->ptr);
    } else if (hi->encoding == REDIS_ENCODING_HT) {
        hi->di = dictGetIterator(subject->ptr);
    } else {
        redisAssert(NULL);
    }
    return hi;
}

void hashTypeReleaseIterator(hashTypeIterator *hi) {
    if (hi->encoding == REDIS_ENCODING_HT) {
        dictReleaseIterator(hi->di);
    }
    zfree(hi);
}

/* Move to the next entry in the hash. Return REDIS_OK when the next entry
 * could be found and REDIS_ERR when the iterator reaches the end. */
int hashTypeNext(hashTypeIterator *hi) {
    if (hi->encoding == REDIS_ENCODING_ZIPMAP) {
        if ((hi->zi = zipmapNext(hi->zi, &hi->zk, &hi->zklen,
            &hi->zv, &hi->zvlen)) == NULL) return REDIS_ERR;
    } else {
        if ((hi->de = dictNext(hi->di)) == NULL) return REDIS_ERR;
    }
    return REDIS_OK;
}

/* Get key or value object at current iteration position.
 * This increases the refcount of the field object by 1. */
robj *hashTypeCurrent(hashTypeIterator *hi, int what) {
    robj *o;
    if (hi->encoding == REDIS_ENCODING_ZIPMAP) {
        if (what & REDIS_HASH_KEY) {
            o = createStringObject((char*)hi->zk,hi->zklen);
        } else {
            o = createStringObject((char*)hi->zv,hi->zvlen);
        }
    } else {
        if (what & REDIS_HASH_KEY) {
            o = dictGetEntryKey(hi->de);
        } else {
            o = dictGetEntryVal(hi->de);
        }
        incrRefCount(o);
    }
    return o;
}

robj *hashTypeLookupWriteOrCreate(redisClient *c, robj *key) {
    robj *o = lookupKeyWrite(c->db,key);
    if (o == NULL) {
        o = createHashObject();
        dbAdd(c->db,key,o);
    } else {
        if (o->type != REDIS_HASH) {
            addReply(c,shared.wrongtypeerr);
            return NULL;
        }
    }
    return o;
}

void convertToRealHash(robj *o) {
    unsigned char *key, *val, *p, *zm = o->ptr;
    unsigned int klen, vlen;
    dict *dict = dictCreate(&hashDictType,NULL);

    redisAssert(o->type == REDIS_HASH && o->encoding != REDIS_ENCODING_HT);
    p = zipmapRewind(zm);
    while((p = zipmapNext(p,&key,&klen,&val,&vlen)) != NULL) {
        robj *keyobj, *valobj;

        keyobj = createStringObject((char*)key,klen);
        valobj = createStringObject((char*)val,vlen);
        keyobj = tryObjectEncoding(keyobj);
        valobj = tryObjectEncoding(valobj);
        dictAdd(dict,keyobj,valobj);
    }
    o->encoding = REDIS_ENCODING_HT;
    o->ptr = dict;
    zfree(zm);
}

/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/

void hsetCommand(redisClient *c) {
    int update;
    robj *o;

    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    hashTypeTryConversion(o,c->argv,2,3);
    hashTypeTryObjectEncoding(o,&c->argv[2], &c->argv[3]);
    update = hashTypeSet(o,c->argv[2],c->argv[3]);
    addReply(c, update ? shared.czero : shared.cone);
    touchWatchedKey(c->db,c->argv[1]);
    server.dirty++;
}

void hsetnxCommand(redisClient *c) {
    robj *o;
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    hashTypeTryConversion(o,c->argv,2,3);

    if (hashTypeExists(o, c->argv[2])) {
        addReply(c, shared.czero);
    } else {
        hashTypeTryObjectEncoding(o,&c->argv[2], &c->argv[3]);
        hashTypeSet(o,c->argv[2],c->argv[3]);
        addReply(c, shared.cone);
        touchWatchedKey(c->db,c->argv[1]);
        server.dirty++;
    }
}

void hmsetCommand(redisClient *c) {
    int i;
    robj *o;

    if ((c->argc % 2) == 1) {
        addReplySds(c,sdsnew("-ERR wrong number of arguments for HMSET\r\n"));
        return;
    }

    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    hashTypeTryConversion(o,c->argv,2,c->argc-1);
    for (i = 2; i < c->argc; i += 2) {
        hashTypeTryObjectEncoding(o,&c->argv[i], &c->argv[i+1]);
        hashTypeSet(o,c->argv[i],c->argv[i+1]);
    }
    addReply(c, shared.ok);
    touchWatchedKey(c->db,c->argv[1]);
    server.dirty++;
}

void hincrbyCommand(redisClient *c) {
    long long value, incr;
    robj *o, *current, *new;

    if (getLongLongFromObjectOrReply(c,c->argv[3],&incr,NULL) != REDIS_OK) return;
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    if ((current = hashTypeGet(o,c->argv[2])) != NULL) {
        if (getLongLongFromObjectOrReply(c,current,&value,
            "hash value is not an integer") != REDIS_OK) {
            decrRefCount(current);
            return;
        }
        decrRefCount(current);
    } else {
        value = 0;
    }

    value += incr;
    new = createStringObjectFromLongLong(value);
    hashTypeTryObjectEncoding(o,&c->argv[2],NULL);
    hashTypeSet(o,c->argv[2],new);
    decrRefCount(new);
    addReplyLongLong(c,value);
    touchWatchedKey(c->db,c->argv[1]);
    server.dirty++;
}

void hgetCommand(redisClient *c) {
    robj *o, *value;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;

    if ((value = hashTypeGet(o,c->argv[2])) != NULL) {
        addReplyBulk(c,value);
        decrRefCount(value);
    } else {
        addReply(c,shared.nullbulk);
    }
}

void hmgetCommand(redisClient *c) {
    int i;
    robj *o, *value;
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o != NULL && o->type != REDIS_HASH) {
        addReply(c,shared.wrongtypeerr);
    }

    /* Note the check for o != NULL happens inside the loop. This is
     * done because objects that cannot be found are considered to be
     * an empty hash. The reply should then be a series of NULLs. */
    addReplyMultiBulkLen(c,c->argc-2);
    for (i = 2; i < c->argc; i++) {
        if (o != NULL && (value = hashTypeGet(o,c->argv[i])) != NULL) {
            addReplyBulk(c,value);
            decrRefCount(value);
        } else {
            addReply(c,shared.nullbulk);
        }
    }
}

void hdelCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;

    if (hashTypeDelete(o,c->argv[2])) {
        if (hashTypeLength(o) == 0) dbDelete(c->db,c->argv[1]);
        addReply(c,shared.cone);
        touchWatchedKey(c->db,c->argv[1]);
        server.dirty++;
    } else {
        addReply(c,shared.czero);
    }
}

void hlenCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;

    addReplyUlong(c,hashTypeLength(o));
}

void genericHgetallCommand(redisClient *c, int flags) {
    robj *o, *obj;
    unsigned long count = 0;
    hashTypeIterator *hi;
    void *replylen = NULL;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
        || checkType(c,o,REDIS_HASH)) return;

    replylen = addDeferredMultiBulkLength(c);
    hi = hashTypeInitIterator(o);
    while (hashTypeNext(hi) != REDIS_ERR) {
        if (flags & REDIS_HASH_KEY) {
            obj = hashTypeCurrent(hi,REDIS_HASH_KEY);
            addReplyBulk(c,obj);
            decrRefCount(obj);
            count++;
        }
        if (flags & REDIS_HASH_VALUE) {
            obj = hashTypeCurrent(hi,REDIS_HASH_VALUE);
            addReplyBulk(c,obj);
            decrRefCount(obj);
            count++;
        }
    }
    hashTypeReleaseIterator(hi);
    setDeferredMultiBulkLength(c,replylen,count);
}

void hkeysCommand(redisClient *c) {
    genericHgetallCommand(c,REDIS_HASH_KEY);
}

void hvalsCommand(redisClient *c) {
    genericHgetallCommand(c,REDIS_HASH_VALUE);
}

void hgetallCommand(redisClient *c) {
    genericHgetallCommand(c,REDIS_HASH_KEY|REDIS_HASH_VALUE);
}

void hexistsCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;

    addReply(c, hashTypeExists(o,c->argv[2]) ? shared.cone : shared.czero);
}

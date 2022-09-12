#include "ctrip_swap.h"

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

typedef unsigned int keylen_t;

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
size_t objectComputeSize(robj *o, size_t sample_size);
size_t objectEstimateSize(robj *o) {
    size_t asize = 0;
    if (o->type == OBJ_HASH && o->encoding == OBJ_ENCODING_HT) {
        dict *d = o->ptr;
        asize = sizeof(*o)+sizeof(dict)+(sizeof(struct dictEntry*)*dictSlots(d));
        asize += DEFAULT_HASH_FIELD_SIZE*dictSize(d);
    } else {
        asize = objectComputeSize(o,5);
    }
    return asize;
}
size_t keyEstimateSize(redisDb *db, robj *key) {
    robj *val = lookupKey(db, key, LOOKUP_NOTOUCH);
    return val ? objectEstimateSize(val): 0;
}

/* Create an unshared object from src, note that o.refcount decreased. */
robj *unshareStringValue(robj *o) {
    serverAssert(o->type == OBJ_STRING);
    if (o->refcount != 1 || o->encoding != OBJ_ENCODING_RAW) {
        robj *decoded = getDecodedObject(o);
        decrRefCount(o);
        o = createRawStringObject(decoded->ptr, sdslen(decoded->ptr));
        decrRefCount(decoded);
        return o;
    } else {
        return o;
    }
}

const char *strObjectType(int type) {
    switch (type) {
    case OBJ_STRING: return "string";
    case OBJ_HASH: return "hash";
    case OBJ_LIST: return "list";
    case OBJ_SET: return "set";
    case OBJ_ZSET: return "zset";
    case OBJ_STREAM: return "stream";
    default: return "unknown";
    }
}

static inline char objectType2Abbrev(int object_type) {
    char abbrevs[] = {'K','L','S','Z','H','M','X'};
    if (object_type >= 0 && object_type < (int)sizeof(abbrevs)) {
        return abbrevs[object_type];
    } else {
        return '?';
    }
}

static inline char abbrev2ObjectType(char abbrev) {
    char abbrevs[] = {'K','L','S','Z','H','M','X'};
    for (size_t i = 0; i < sizeof(abbrevs); i++) {
        if (abbrevs[i] == abbrev) return i;
    }
    return -1;
}

sds rocksEncodeMetaVal(int object_type, long long expire, sds extend) {
    size_t len = 1 + sizeof(expire) + (extend ? sdslen(extend) : 0);
    sds raw = sdsnewlen(SDS_NOINIT,len), ptr = raw;
    ptr[0] = objectType2Abbrev(object_type), ptr++;
    memcpy(raw+1,&expire,sizeof(expire)), ptr+=sizeof(expire);
    if (extend) memcpy(ptr,extend,sdslen(extend));
    return raw;
}

/* extend: pointer to rawkey, not allocated. */
int rocksDecodeMetaVal(const char *raw, size_t rawlen, int *pobject_type,
        long long *pexpire, const char **pextend, size_t *pextend_len) {
    const char *ptr = raw;
    size_t len = rawlen;
    long long expire;
    int object_type;

    if (rawlen < 1 + sizeof(expire)) return -1;

    if ((object_type = abbrev2ObjectType(ptr[0])) < 0) return -1;
    ptr++, len--;
    if (pobject_type) *pobject_type = object_type;

    expire = *(long long*)ptr;
    ptr += sizeof(long long), len -= sizeof(long long);
    if (pexpire) *pexpire = expire;

    if (pextend_len) *pextend_len = len;
    if (pextend) {
        *pextend = len > 0 ? ptr : NULL;
    }

    return 0;
}

sds rocksEncodeDataKey(redisDb *db, sds key, sds subkey) {
    int dbid = db->id;
    keylen_t keylen = key ? sdslen(key) : 0;
    keylen_t subkeylen = subkey ? sdslen(subkey) : 0;
    size_t rawkeylen = sizeof(dbid)+sizeof(keylen)+keylen+subkeylen;
    sds rawkey = sdsnewlen(SDS_NOINIT,rawkeylen), ptr = rawkey;
    memcpy(ptr, &dbid, sizeof(dbid)), ptr += sizeof(dbid);
    memcpy(ptr, &keylen, sizeof(keylen_t)), ptr += sizeof(keylen_t);
    memcpy(ptr, key, keylen), ptr += keylen;
    if (subkey) memcpy(ptr, subkey, subkeylen), ptr += subkeylen;
    return rawkey;
}

int rocksDecodeDataKey(const char *raw, size_t rawlen, int *dbid,
        const char **key, size_t *keylen,
        const char **subkey, size_t *subkeylen) {
    keylen_t keylen_;
    if (raw == NULL || rawlen < sizeof(int)+sizeof(keylen_t)) return -1;
    if (dbid) *dbid = *(int*)raw;
    raw += sizeof(int), rawlen -= sizeof(int);
    keylen_ = *(keylen_t*)raw;
    if (keylen) *keylen = keylen_;
    raw += sizeof(keylen_t), rawlen -= sizeof(keylen_t);
    if (key) *key = raw;
    if (rawlen < keylen_) return -1;
    raw += keylen_, rawlen -= keylen_;
    if (subkeylen) *subkeylen = rawlen;
    if (subkey) {
        *subkey = rawlen > 0 ? raw : NULL;
    }
    return 0;
}

sds rocksEncodeValRdb(robj *value) {
    rio sdsrdb;
    rioInitWithBuffer(&sdsrdb,sdsempty());
    rdbSaveObjectType(&sdsrdb,value) ;
    rdbSaveObject(&sdsrdb,value,NULL);
    return sdsrdb.io.buffer.ptr;
}

robj *rocksDecodeValRdb(sds raw) {
    robj *value;
    rio sdsrdb;
    int rdbtype;
    rioInitWithBuffer(&sdsrdb,raw);
    rdbtype = rdbLoadObjectType(&sdsrdb);
    value = rdbLoadObject(rdbtype,&sdsrdb,NULL,NULL);
    return value;
}

sds rocksEncodeObjectMetaLen(unsigned long len) {
    return sdsnewlen(&len,sizeof(len));
}

long rocksDecodeObjectMetaLen(const char *raw, size_t rawlen) {
    if (rawlen != sizeof(unsigned long)) return -1;
    return *(long*)raw;
}

sds rocksCalculateNextKey(sds current) {
	sds next = NULL;
	size_t nextlen = sdslen(current);

	do {
		if (current[nextlen - 1] != (char)0xff) break;
		nextlen--;
	} while(nextlen > 0); 

	if (0 == nextlen) return NULL;

	next = sdsnewlen(current, nextlen);
	next[nextlen - 1]++;

	return next;
}

#ifdef REDIS_TEST

int swapUtilTest(int argc, char **argv, int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);
    int error = 0;

    TEST("util - encode & decode object meta len") {
        sds raw;
        raw = rocksEncodeObjectMetaLen(666);
        test_assert(666 == rocksDecodeObjectMetaLen(raw,sdslen(raw)));
        sdsfree(raw);
        raw = rocksEncodeObjectMetaLen(0);
        test_assert(0 == rocksDecodeObjectMetaLen(raw,sdslen(raw)));
        sdsfree(raw);
        raw = rocksEncodeObjectMetaLen(-1);
        test_assert(-1 == rocksDecodeObjectMetaLen(raw,sdslen(raw)));
        sdsfree(raw);
        raw = rocksEncodeObjectMetaLen(-666);
        test_assert(-666 == rocksDecodeObjectMetaLen(raw,sdslen(raw)));
        sdsfree(raw);
    }

    return error;
}

#endif

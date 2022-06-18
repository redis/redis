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

int rocksGetObjectType(unsigned char enc_type) {
    switch (enc_type) {
    case ENC_TYPE_STRING: return OBJ_STRING;
    case ENC_TYPE_LIST: return OBJ_LIST;
    case ENC_TYPE_SET: return OBJ_SET;
    case ENC_TYPE_ZSET: return OBJ_ZSET;
    case ENC_TYPE_HASH: return OBJ_HASH;
    case ENC_TYPE_HASH_SUB: return OBJ_HASH;
    case ENC_TYPE_MODULE: return OBJ_MODULE;
    case ENC_TYPE_STREAM: return OBJ_STREAM;
    default: return -1;
    }
}

static inline char rocksGetKeyEncType(int obj_type) {
    switch (obj_type) {
    case OBJ_STRING : return ENC_TYPE_STRING  ;
    case OBJ_LIST   : return ENC_TYPE_LIST    ;
    case OBJ_SET    : return ENC_TYPE_SET     ;
    case OBJ_ZSET   : return ENC_TYPE_ZSET    ;
    case OBJ_HASH   : return ENC_TYPE_HASH    ;
    case OBJ_MODULE : return ENC_TYPE_MODULE  ;
    case OBJ_STREAM : return ENC_TYPE_STREAM  ;
    default         : return ENC_TYPE_UNKNOWN ;
    }
}

static inline char rocksGetSubkeyEncType(int obj_type) {
    switch (obj_type) {
    case OBJ_LIST   : return ENC_TYPE_LIST_SUB  ; 
    case OBJ_SET    : return ENC_TYPE_SET_SUB   ; 
    case OBJ_ZSET   : return ENC_TYPE_ZSET_SUB  ; 
    case OBJ_HASH   : return ENC_TYPE_HASH_SUB  ; 
    case OBJ_MODULE : return ENC_TYPE_MODULE_SUB; 
    case OBJ_STREAM : return ENC_TYPE_STREAM_SUB; 
    default         : return ENC_TYPE_UNKNOWN_SUB;
    }
}

unsigned char rocksGetEncType(int obj_type, int big) {
    return big ? rocksGetSubkeyEncType(obj_type) : rocksGetKeyEncType(obj_type);
}

unsigned char rocksGetObjectEncType(robj *o) {
    return rocksGetEncType(o->type,o->big);
}

sds rocksEncodeKey(unsigned char enc_type, sds key) {
    sds rawkey = sdsnewlen(SDS_NOINIT,1+sdslen(key));
    rawkey[0] = enc_type;
    memcpy(rawkey+1, key, sdslen(key));
    return rawkey;
}

sds rocksEncodeSubkey(unsigned char enc_type, uint64_t version, sds key, sds subkey) {
    size_t subkeylen = subkey ? sdslen(subkey) : 0;
    size_t rawkeylen = 1+sizeof(version)+sizeof(keylen_t)+sdslen(key)+subkeylen;
    sds rawkey = sdsnewlen(SDS_NOINIT,rawkeylen);
            
    char *ptr = rawkey;
    keylen_t keylen = (keylen_t)sdslen(key);
    ptr[0] = enc_type, ptr++;
    memcpy(ptr, &version, sizeof(version)), ptr += sizeof(version);
    memcpy(ptr, &keylen, sizeof(keylen_t)), ptr += sizeof(keylen_t);
    memcpy(ptr, key, sdslen(key)), ptr += sdslen(key);
    if (subkey) {
        memcpy(ptr, subkey, sdslen(subkey)), ptr += sdslen(subkey);
    }
    return rawkey;
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

int rocksDecodeKey(const char *raw, size_t rawlen, const char **key,
        size_t *klen) {
    int obj_type;
    if (rawlen < 2) return -1;
    if ((obj_type = rocksGetObjectType(raw[0])) < 0) return -1;
    raw++, rawlen--;
    if (key) *key = raw;
    if (klen) *klen = rawlen;
    return obj_type;
}

int rocksDecodeSubkey(const char *raw, size_t rawlen, uint64_t *version,
        const char **key, size_t *klen, const char **sub, size_t *slen) {
    int obj_type;
    keylen_t _klen;
    uint64_t _version;
    if (rawlen <= 1+sizeof(uint64_t)+sizeof(keylen_t)) return -1;

    if ((obj_type = rocksGetObjectType(raw[0])) < 0) return -1;
    raw++, rawlen--;

    _version = *(uint64_t*)raw;
    if (version) *version = _version;
    raw += sizeof(uint64_t);
    rawlen -= sizeof(uint64_t);

    _klen = *(keylen_t*)raw;
    if (klen) *klen = (size_t)_klen;
    raw += sizeof(keylen_t);
    rawlen -= sizeof(keylen_t);

    if (key) *key = raw;
    raw += _klen;
    rawlen-= _klen;

    if (rawlen <= 0) return -1;
    if (sub) *sub = raw;
    if (slen) *slen = rawlen;
    return obj_type;
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

size_t keyComputeSize(redisDb *db, robj *key) {
    robj *val = lookupKey(db, key, LOOKUP_NOTOUCH);
    return val ? objectComputeSize(val, 5): 0;
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


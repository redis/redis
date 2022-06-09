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

int typeR2O(char rocks_type) {
    switch (rocks_type) {
    case 'K': return OBJ_STRING;
    case 'L': return OBJ_LIST;
    case 'S': return OBJ_SET;
    case 'Z': return OBJ_ZSET;
    case 'H': return OBJ_HASH;
    case 'h': return OBJ_HASH;
    case 'M': return OBJ_MODULE;
    case 'X': return OBJ_STREAM;
    default: return -1;
    }
}

char typeO2R(int obj_type) {
    switch (obj_type) {
    case OBJ_STRING : return 'K';
    case OBJ_LIST   : return 'L'; 
    case OBJ_SET    : return 'S'; 
    case OBJ_ZSET   : return 'Z'; 
    case OBJ_HASH   : return 'H'; 
    case OBJ_MODULE : return 'M'; 
    case OBJ_STREAM : return 'X'; 
    default         : return '?';
    }
}

char typeO2S(int obj_type) {
    switch (obj_type) {
    case OBJ_STRING : return 'k';
    case OBJ_LIST   : return 'l'; 
    case OBJ_SET    : return 's'; 
    case OBJ_ZSET   : return 'z'; 
    case OBJ_HASH   : return 'h'; 
    case OBJ_MODULE : return 'm'; 
    case OBJ_STREAM : return 'x'; 
    default         : return '?';
    }
}

sds rocksEncodeKey(int obj_type, sds key) {
    sds rawkey = sdsnewlen(SDS_NOINIT,1+sdslen(key));
    rawkey[0] = typeO2R(obj_type);
    memcpy(rawkey+1, key, sdslen(key));
    return rawkey;
}

sds rocksEncodeSubkey(int obj_type, sds key, sds subkey) {
    size_t subkeylen = subkey ? sdslen(subkey) : 0;
    size_t rawkeylen = 1+sizeof(keylen_t)+sdslen(key)+subkeylen;
    sds rawkey = sdsnewlen(SDS_NOINIT,rawkeylen);
            
    char *ptr = rawkey;
    keylen_t keylen = (keylen_t)sdslen(key);
    ptr[0] = typeO2S(obj_type), ptr++;
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
    if ((obj_type = typeR2O(raw[0])) < 0) return -1;
    raw++, rawlen--;
    if (key) *key = raw;
    if (klen) *klen = rawlen;
    return obj_type;
}

int rocksDecodeSubkey(const char *raw, size_t rawlen, const char **key,
        size_t *klen, const char **sub, size_t *slen) {
    int obj_type;
    keylen_t _klen;
    if (rawlen <= 1+sizeof(keylen_t)) return -1;
    if ((obj_type = typeR2O(raw[0])) < 0) return -1;
    raw++, rawlen--;
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

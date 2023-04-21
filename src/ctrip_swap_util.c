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

#include "endianconv.h"
#include <dirent.h>
#include <sys/stat.h>

/* See keyIsExpired for more details */
size_t ctripDbSize(redisDb *db) {
    return dictSize(db->dict) + db->cold_keys;
}

/* See keyIsExpired for more details */
int timestampIsExpired(mstime_t when) {
    mstime_t now;

    if (when < 0) return 0;
    if (server.loading) return 0;
    if (server.lua_caller) {
        now = server.lua_time_snapshot;
    } else if (server.fixed_time_expire > 0) {
        now = server.mstime;
    } else {
        now = mstime();
    }
    return now > when;
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

/* Encode version in BE order, so that numeric order matches alphabatic. */
#define rocksEncodeVersion(version) htonu64(version)
#define rocksDecodeVersion(version) ntohu64(version)

sds rocksEncodeMetaVal(int object_type, long long expire, uint64_t version,
        sds extend) {
    uint64_t encoded_version = rocksEncodeVersion(version);
    size_t len = 1 + sizeof(expire) + sizeof(encoded_version) + 
        (extend ? sdslen(extend) : 0);
    sds raw = sdsnewlen(SDS_NOINIT,len), ptr = raw;
    ptr[0] = objectType2Abbrev(object_type), ptr++;
    memcpy(ptr,&expire,sizeof(expire)), ptr+=sizeof(expire);
    memcpy(ptr,&encoded_version,sizeof(encoded_version));
    ptr += sizeof(encoded_version);
    if (extend) memcpy(ptr,extend,sdslen(extend));
    return raw;
}

/* extend: pointer to rawkey, not allocated. */
int rocksDecodeMetaVal(const char *raw, size_t rawlen, int *pobject_type,
        long long *pexpire, uint64_t *pversion, const char **pextend,
        size_t *pextend_len) {
    const char *ptr = raw;
    size_t len = rawlen;
    long long expire;
    int object_type;
    uint64_t encoded_version;

    if (rawlen < 1 + sizeof(expire) + sizeof(encoded_version)) return -1;

    if ((object_type = abbrev2ObjectType(ptr[0])) < 0) return -1;
    ptr++, len--;
    if (pobject_type) *pobject_type = object_type;

    expire = *(long long*)ptr;
    ptr += sizeof(long long), len -= sizeof(long long);
    if (pexpire) *pexpire = expire;

    encoded_version = *(uint64_t*)ptr;
    if (pversion) *pversion = rocksDecodeVersion(encoded_version);
    ptr += sizeof(encoded_version), len -= sizeof(encoded_version);

    if (pextend_len) *pextend_len = len;
    if (pextend) {
        *pextend = len > 0 ? ptr : NULL;
    }

    return 0;
}

typedef unsigned int keylen_t;

static sds _rocksEncodeDataKey(int dbid, sds key, uint64_t version,
        uint8_t subkeyflag, sds subkey) {
    keylen_t keylen = key ? sdslen(key) : 0;
    keylen_t subkeylen = subkey ? sdslen(subkey) : 0;
    uint64_t encoded_version = rocksEncodeVersion(version);
    size_t rawkeylen = sizeof(dbid)+sizeof(keylen)+keylen+
        sizeof(encoded_version)+1+subkeylen;
    sds rawkey = sdsnewlen(SDS_NOINIT,rawkeylen), ptr = rawkey;
    memcpy(ptr, &dbid, sizeof(dbid)), ptr += sizeof(dbid);
    memcpy(ptr, &keylen, sizeof(keylen_t)), ptr += sizeof(keylen_t);
    memcpy(ptr, key, keylen), ptr += keylen;
    memcpy(ptr, &encoded_version, sizeof(encoded_version));
    ptr += sizeof(encoded_version);
    ptr[0] = subkeyflag, ptr++;
    if (subkeyflag == ROCKS_KEY_FLAG_SUBKEY) {
        memcpy(ptr, subkey, subkeylen), ptr += subkeylen;
    }
    return rawkey;
}

sds rocksEncodeDataKey(redisDb *db, sds key, uint64_t version, sds subkey) {
    if (subkey) {
        return _rocksEncodeDataKey(db->id,key,version,ROCKS_KEY_FLAG_SUBKEY,subkey);
    } else {
        return _rocksEncodeDataKey(db->id,key,version,ROCKS_KEY_FLAG_NONE,NULL);
    }
}

sds rocksEncodeDataRangeStartKey(redisDb *db, sds key, uint64_t version) {
    return _rocksEncodeDataKey(db->id,key,version,ROCKS_KEY_FLAG_SUBKEY,shared.emptystring->ptr);
}

sds rocksEncodeDataRangeEndKey(redisDb *db, sds key, uint64_t version) {
    return _rocksEncodeDataKey(db->id,key,version,ROCKS_KEY_FLAG_DELETE,NULL);
}

sds rocksEncodeDbRangeStartKey(int dbid) {
    sds rawkey = sdsnewlen(SDS_NOINIT,sizeof(dbid));
    memcpy(rawkey, &dbid, sizeof(dbid));
    return rawkey;
}

sds rocksEncodeDbRangeEndKey(int dbid) {
    return rocksEncodeDbRangeStartKey(dbid+1);
}

int rocksDecodeDataKey(const char *raw, size_t rawlen, int *dbid,
        const char **key, size_t *keylen, uint64_t *version,
        const char **subkey, size_t *subkeylen) {
    keylen_t keylen_;
    uint64_t encoded_version;
    if (raw == NULL || rawlen < sizeof(int)+sizeof(keylen_t)+sizeof(encoded_version)+1) return -1;
    if (dbid) *dbid = *(int*)raw;
    raw += sizeof(int), rawlen -= sizeof(int);
    keylen_ = *(keylen_t*)raw;
    if (keylen) *keylen = keylen_;
    raw += sizeof(keylen_t), rawlen -= sizeof(keylen_t);
    if (key) *key = raw;
    if (rawlen < keylen_) return -1;
    raw += keylen_, rawlen -= keylen_;
    if (rawlen < sizeof(encoded_version)) return -1;
    if (version) {
        encoded_version = *(uint64_t*)raw;
        *version = rocksDecodeVersion(encoded_version);
    }
    raw += sizeof(encoded_version), rawlen -= sizeof(encoded_version);
    if (subkeylen) *subkeylen = rawlen - 1;
    if (subkey) {
        *subkey = raw[0] == ROCKS_KEY_FLAG_SUBKEY ? raw + 1 : NULL;
    }
    return 0;
}

/* Note that metakey MUST be prefix of datakeys, rdb save key switch detection
 * relay on that assumption. */
sds encodeMetaKey(int dbid, const char* key, size_t keylen_) {
    keylen_t keylen = keylen_;
    size_t rawkeylen = sizeof(dbid)+sizeof(keylen)+keylen;
    sds rawkey = sdsnewlen(SDS_NOINIT,rawkeylen), ptr = rawkey;
    memcpy(ptr, &dbid, sizeof(dbid)), ptr += sizeof(dbid);
    memcpy(ptr, &keylen, sizeof(keylen_t)), ptr += sizeof(keylen_t);
    memcpy(ptr, key, keylen), ptr += keylen;
    return rawkey;
}

sds rocksEncodeMetaKey(redisDb *db, sds key) {    
    return encodeMetaKey(db->id, key, key ? sdslen(key) : 0);
}

int rocksDecodeMetaKey(const char *raw, size_t rawlen, int *dbid,
        const char **key, size_t *keylen) {
    keylen_t keylen_;
    if (raw == NULL || rawlen < sizeof(int)+sizeof(keylen_t)) return -1;
    if (dbid) *dbid = *(int*)raw;
    raw += sizeof(int), rawlen -= sizeof(int);
    keylen_ = *(keylen_t*)raw;
    if (keylen) *keylen = keylen_;
    raw += sizeof(keylen_t), rawlen -= sizeof(keylen_t);
    if (key) *key = raw;
    if (rawlen < keylen_) return -1;
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

sds encodeMetaScanKey(unsigned long cursor, int limit, sds seek) {
    size_t len = sizeof(cursor) + sizeof(limit) + (seek ? 0 : sdslen(seek));
    sds result = sdsnewlen(SDS_NOINIT,len);
    char *ptr = result;

    memcpy(ptr,&cursor,sizeof(cursor)), ptr+=sizeof(cursor);
    memcpy(ptr,&limit,sizeof(limit)), ptr+=sizeof(limit);
    if (seek) memcpy(ptr,seek,sdslen(seek));
    return result;
}

int decodeMetaScanKey(sds meta_scan_key, unsigned long *cursor, int *limit,
        const char **seek, size_t *seeklen) {
    size_t len = sizeof(unsigned long) + sizeof(int);
    const char *ptr = meta_scan_key;
    if (sdslen(meta_scan_key) < len) return -1;
    if (cursor) *cursor = *(unsigned long*)ptr;
    ptr += sizeof(unsigned long);
    if (limit) *limit = *(int*)ptr;
    ptr += sizeof(int);
    if (seek) *seek = ptr;
    if (seeklen) *seeklen = sdslen(meta_scan_key) - len;
    return 0;
}

int encodeFixed64(char* buf, uint64_t value) {
    if (BYTE_ORDER == BIG_ENDIAN) {
        memcpy(buf, &value, sizeof(value));
        return sizeof(value);
    } else {
        buf[0] = (uint8_t)((value >> 56) & 0xff);
        buf[1] = (uint8_t)((value >> 48) & 0xff);
        buf[2] = (uint8_t)((value >> 40) & 0xff);
        buf[3] = (uint8_t)((value >> 32) & 0xff);
        buf[4] = (uint8_t)((value >> 24) & 0xff);
        buf[5] = (uint8_t)((value >> 16) & 0xff);
        buf[6] = (uint8_t)((value >> 8) & 0xff);
        buf[7] = (uint8_t)(value & 0xff);
        return 8;
    }
}

int encodeDouble(char* buf, double value) {
    uint64_t u64;
    memcpy(&u64, &value, sizeof(value));
    uint64_t* ptr = &u64;
    if ((*ptr >> 63) == 1) {
        // signed bit would be zero
        *ptr ^= 0xffffffffffffffff;
    } else {
        // signed bit would be one
        *ptr |= 0x8000000000000000;
    }
    return encodeFixed64(buf, *ptr);
}

uint32_t decodeFixed32(const char *ptr) {
  if (BYTE_ORDER == BIG_ENDIAN) {
    uint32_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
  } else {
    return (((uint32_t)((uint8_t)(ptr[3])))
        | ((uint32_t)((uint8_t)(ptr[2])) << 8)
        | ((uint32_t)((uint8_t)(ptr[1])) << 16)
        | ((uint32_t)((uint8_t)(ptr[0])) << 24));
  }
}


uint64_t decodeFixed64(const char *ptr) {
  if (BYTE_ORDER == BIG_ENDIAN) {
    uint64_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
  } else {
    uint64_t hi = decodeFixed32(ptr);
    uint64_t lo = decodeFixed32(ptr+4);
    return (hi << 32) | lo;
  }
}

int decodeDouble(const char* val, double* score) {
    uint64_t decoded = decodeFixed64(val);
    if ((decoded >> 63) == 0) {
        decoded ^= 0xffffffffffffffff;
    } else {
        decoded &= 0x7fffffffffffffff;
    }
    double value;
    memcpy(&value, &decoded, sizeof(value));
    *score = value;
    return sizeof(value);
}

sds _encodeScoreKey(int dbid, sds key, uint64_t version, uint8_t subkeyflag,
        double score, sds subkey) {
    uint64_t encoded_version = rocksEncodeVersion(version);
    keylen_t keylen = key ? sdslen(key) : 0;
    keylen_t scoresubkeylen, rawkeylen;
    sds rawkey, ptr;

    if (subkeyflag == ROCKS_KEY_FLAG_SUBKEY) {
        serverAssert(subkey);
        scoresubkeylen = sizeOfDouble;
        scoresubkeylen += sdslen(subkey);
    } else {
        scoresubkeylen = 0;
    } 

    rawkeylen = sizeof(dbid)+sizeof(keylen)+keylen+sizeof(version)+1+scoresubkeylen;
    rawkey = sdsnewlen(SDS_NOINIT,rawkeylen), ptr = rawkey;

    memcpy(ptr, &dbid, sizeof(dbid)), ptr += sizeof(dbid);
    memcpy(ptr, &keylen, sizeof(keylen_t)), ptr += sizeof(keylen_t);
    memcpy(ptr, key, keylen), ptr += keylen;
    memcpy(ptr, &encoded_version, sizeof(encoded_version));
    ptr += sizeof(encoded_version);
    ptr[0] = subkeyflag, ptr++;

    if (subkeyflag == ROCKS_KEY_FLAG_SUBKEY) {
        ptr += encodeDouble(ptr,score);
        memcpy(ptr,subkey,sdslen(subkey)), ptr += scoresubkeylen;
    }

    return rawkey;
}

sds encodeScoreRangeStart(redisDb* db, sds key, uint64_t version) {
    return _encodeScoreKey(db->id,key,version,ROCKS_KEY_FLAG_NONE,0,NULL);
}

sds encodeScoreRangeEnd(redisDb* db, sds key, uint64_t version) {
    return _encodeScoreKey(db->id,key,version,ROCKS_KEY_FLAG_DELETE,0,NULL);
}

sds encodeScoreKey(redisDb* db, sds key, uint64_t version, double score, sds subkey) {
    if (subkey) {
        return _encodeScoreKey(db->id,key,version,ROCKS_KEY_FLAG_SUBKEY,score,subkey);
    } else {
        return _encodeScoreKey(db->id,key,version,ROCKS_KEY_FLAG_NONE,0,NULL);
    }
}

int decodeScoreKey(const char* raw, int rawlen, int* dbid, const char** key,
        size_t* keylen, uint64_t *version, double* score, const char** subkey,
        size_t* subkeylen) {
    keylen_t keylen_;
    if (raw == NULL || rawlen < (int)(sizeof(int)+sizeof(keylen_t)+sizeof(uint64_t)+1+sizeOfDouble)) return -1;
    if (dbid) *dbid = *(int*)raw;
    raw += sizeof(int), rawlen -= sizeof(int);
    keylen_ = *(keylen_t*)raw;
    if (keylen) *keylen = keylen_;
    raw += sizeof(keylen_t), rawlen -= sizeof(keylen_t);
    if (key) *key = raw;
    if (rawlen < (int)keylen_) return -1;
    raw += keylen_, rawlen -= keylen_;
    if (version) {
        uint64_t encoded_version = *(uint64_t*)raw;
        *version = rocksDecodeVersion(encoded_version);
    }
    raw += sizeof(uint64_t), rawlen -= sizeof(uint64_t);
    uint8_t subkeyflag = raw[0];
    raw++, rawlen--;
    if (subkeyflag == ROCKS_KEY_FLAG_SUBKEY) {
        int double_offset = decodeDouble(raw, score);
        raw += double_offset;
        rawlen -= double_offset;
        if (subkeylen) *subkeylen = rawlen;
        if (subkey) *subkey = raw;
    } else {
        if (score) *score = 0;
        if (subkeylen) *subkeylen = 0;
        if (subkey) *subkey = NULL;
    }

    return 0;
}


sds encodeIntervalSds(int ex, MOVE IN sds data) {
    sds result;
    if (ex) {
        result = sdscatsds(sdsnewlen("(", 1), data);
    } else {
        result = sdscatsds(sdsnewlen("[", 1), data);
    }
    sdsfree(data);
    return result;
}

int decodeIntervalSds(sds data, int* ex, char** raw, size_t* rawlen) {
    if (sdslen(data) == 0) {
        return C_ERR;
    }
    switch (data[0])
    {
        case '(':
            *ex = 1;
            break;
        case '[':
            *ex = 0;
            break;
        default:
            return C_ERR;
            break;
    }
    *raw = data + 1;
    *rawlen = sdslen(data) - 1;
    return C_OK;
}

/*
    calculate the size of all files in a folder
*/
long get_dir_size(char *dirname)
{
    DIR *dir;
    struct dirent *ptr;
    long total_size = 0;
    char path[PATH_MAX] = {0};

    dir = opendir(dirname);
    if(dir == NULL)
    {
        serverLog(LL_WARNING,"open dir(%s) failed.", dirname);
        return -1;
    }

    while((ptr=readdir(dir)) != NULL)
    {
        snprintf(path, (size_t)PATH_MAX, "%s/%s", dirname,ptr->d_name);
        struct stat buf;
        if(lstat(path, &buf) < 0) {
            serverLog(LL_WARNING, "path(%s) lstat error", path);
        }
        if(strcmp(ptr->d_name,".") == 0) {
            total_size += buf.st_size;
            continue;
        }
        if(strcmp(ptr->d_name,"..") == 0) {
            continue;
        }
        if (S_ISDIR(buf.st_mode))
        {
            total_size += get_dir_size(path);
            memset(path, 0, sizeof(path));
        } else {
            total_size += buf.st_size;
        }
    }
    closedir(dir);
    return total_size;
}

#ifdef REDIS_TEST

int swapUtilTest(int argc, char **argv, int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);
    int error = 0;
    redisDb* db = server.db + 0;

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

    TEST("util - decode & encode version") {
        uint64_t V = 0x12345678;
        test_assert(V == rocksDecodeVersion(rocksEncodeVersion(V)));
    }

    TEST("util - encode & decode data key") {
        sds key = sdsnew("key1");
        sds f1 = sdsnew("f1");
        int dbId = 123456789;
        const char *keystr = NULL, *subkeystr = NULL;
        size_t klen = 123456789, slen = 123456789;
        sds empty = sdsempty();
        uint64_t version, V = 0x12345678;

        /* util - encode & decode no subkey */
        sds rocksKey = rocksEncodeDataKey(db,key,V,NULL);
        rocksDecodeDataKey(rocksKey,sdslen(rocksKey),&dbId,&keystr,&klen,&version,&subkeystr,&slen);
        test_assert(dbId == db->id);
        test_assert(memcmp(key,keystr,klen) == 0);
        test_assert(subkeystr == NULL);
        test_assert(version == V);
        sdsfree(rocksKey);
        /* util - encode & decode with subkey */
        rocksKey = rocksEncodeDataKey(db,key,V,f1);
        rocksDecodeDataKey(rocksKey,sdslen(rocksKey),&dbId,&keystr,&klen,&version,&subkeystr,&slen);
        test_assert(dbId == db->id);
        test_assert(memcmp(key,keystr,klen) == 0);
        test_assert(memcmp(f1,subkeystr,slen) == 0);
        test_assert(sdslen(f1) == slen);
        test_assert(version == V);
        sdsfree(rocksKey);
        /* util - encode & decode with empty subkey */
        rocksKey = rocksEncodeDataKey(db,key,V,empty);
        rocksDecodeDataKey(rocksKey,sdslen(rocksKey),&dbId,&keystr,&klen,&version,&subkeystr,&slen);
        test_assert(dbId == db->id);
        test_assert(memcmp(key,keystr,klen) == 0);
        test_assert(memcmp("",subkeystr,slen) == 0);
        test_assert(slen == 0);
        test_assert(version == V);
        sdsfree(rocksKey);
        /* util - encode end key */
        sds start_key = rocksEncodeDataRangeStartKey(db,key,V);
        sds end_key = rocksEncodeDataRangeEndKey(db,key,V);
        test_assert(sdscmp(start_key, end_key) < 0);
        rocksKey = rocksEncodeDataKey(db,key,V,empty);
        test_assert(sdscmp(rocksKey, start_key) >= 0 && sdscmp(rocksKey, end_key) < 0);
        sdsfree(rocksKey);

        rocksKey = rocksEncodeDataKey(db,key,V,f1);
        test_assert(sdscmp(rocksKey, start_key) > 0 && sdscmp(rocksKey, end_key) < 0);
        sdsfree(rocksKey);

        sdsfree(empty);
    }

    TEST("util - encode & decode meta") {
        sds empty = sdsempty(), rocksKey, rocksVal;
        sds key = sdsnew("key1");
        sds EXT = sdsfromlonglong(666);
        int dbId = 123456789, object_type;
        const char *keystr = NULL, *extend;
        size_t klen = 12345, extlen = 12345;
        uint64_t version, V = 0x12345678;
        long long expire, EXP = 10086;

        /* util - encode & decode empty meta key */
        rocksKey = rocksEncodeMetaKey(db,empty);
        rocksDecodeMetaKey(rocksKey,sdslen(rocksKey),&dbId,&keystr,&klen);
        test_assert(dbId == db->id);
        test_assert(klen == 0);
        sdsfree(rocksKey);

        /* util - encode & decode meta key */
        rocksKey = rocksEncodeMetaKey(db,key);
        rocksDecodeMetaKey(rocksKey,sdslen(rocksKey),&dbId,&keystr,&klen);
        test_assert(dbId == db->id);
        test_assert(memcmp(key,keystr,klen) == 0 && klen == sdslen(key));
        sdsfree(rocksKey);

        /* util - encode & decode meta val */
        rocksVal = rocksEncodeMetaVal(OBJ_HASH,EXP,V,EXT);
        rocksDecodeMetaVal(rocksVal,sdslen(rocksVal),&object_type,&expire,&version,&extend,&extlen);
        test_assert(object_type = OBJ_HASH);
        test_assert(expire == EXP);
        test_assert(version == V);
        test_assert(extlen == sdslen(EXT) && memcmp(extend,EXT,extlen) == 0);
        sdsfree(rocksVal);

        sdsfree(empty), sdsfree(key), sdsfree(EXT);
    }

    TEST("util - encode & decode score key") {
        sds empty = sdsempty(), subkey = sdsnew("subkey"), rocksKey;
        sds key = sdsnew("key1");
        int dbId = 123456789;
        const char *keystr = NULL, *subkeystr;
        size_t klen, slen;
        uint64_t version, V = 0x12345678;
        double score, SCORE = 0.25;
        sds start_key, end_key;

        rocksKey = encodeScoreKey(db,key,V,SCORE,empty);
        decodeScoreKey(rocksKey,sdslen(rocksKey),&dbId,&keystr,&klen,&version,&score,&subkeystr,&slen);
        test_assert(dbId == db->id);
        test_assert(klen == sdslen(key) && memcmp(keystr,key,klen) == 0);
        test_assert(version == V);
        test_assert(score == SCORE);
        test_assert(slen == 0);
        sdsfree(rocksKey);

        rocksKey = encodeScoreKey(db,key,V,SCORE,subkey);
        decodeScoreKey(rocksKey,sdslen(rocksKey),&dbId,&keystr,&klen,&version,&score,&subkeystr,&slen);
        test_assert(dbId == db->id);
        test_assert(klen == sdslen(key) && memcmp(keystr,key,klen) == 0);
        test_assert(version == V);
        test_assert(score == SCORE);
        test_assert(slen == sdslen(subkey) && memcmp(subkeystr,subkey,slen) == 0);

        start_key = encodeScoreRangeStart(db,key,V);
        end_key = encodeScoreRangeEnd(db,key,V);
        test_assert(memcmp(rocksKey,start_key,sdslen(start_key)) > 0);
        test_assert(memcmp(rocksKey,end_key,sdslen(end_key)) < 0);

        sdsfree(rocksKey), sdsfree(start_key), sdsfree(end_key);
        sdsfree(empty), sdsfree(subkey), sdsfree(key);
    }

    TEST("util - data & score constains") {
        sds key = sdsnew("key"), empty = sdsempty(), subkey = sdsnew("subkey");
        sds dataKey, metaKey;

        /* rdb iter constains: meta key must be prefix of data key. */
        metaKey = rocksEncodeMetaKey(db,key);

        dataKey = rocksEncodeDataKey(db,key,0,empty);
        test_assert(memcmp(metaKey,dataKey,sdslen(metaKey)) == 0);
        sdsfree(dataKey);

        dataKey = rocksEncodeDataKey(db,key,0,subkey);
        test_assert(memcmp(metaKey,dataKey,sdslen(metaKey)) == 0);
        sdsfree(dataKey);

        dataKey = rocksEncodeDataKey(db,key,12345678,empty);
        test_assert(memcmp(metaKey,dataKey,sdslen(metaKey)) == 0);
        sdsfree(dataKey);

        dataKey = rocksEncodeDataKey(db,key,12345678,subkey);
        test_assert(memcmp(metaKey,dataKey,sdslen(metaKey)) == 0);
        sdsfree(dataKey);
        sdsfree(key), sdsfree(empty), sdsfree(subkey);
    }

    return error;
}

#endif

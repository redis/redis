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
#include "ctrip_swap_rdb.h"

/* ------------------------------ rdb save -------------------------------- */
#define RDBSAVE_ROCKS_CACHED_MAX_KEY 1024
#define RDBSAVE_ROCKS_CACHED_MAX_VAL 4096

void rdbSaveProgress(rio *rdb, int rdbflags);
ssize_t rdbWriteRaw(rio *rdb, void *p, size_t len);

/* Whole key encoding in rocksdb is the same as in rdb, so we skip encoding
 * and decoding to reduce cpu usage. */ 
int rdbSaveKeyRawPair(rio *rdb, robj *key, robj *evict, sds raw, 
                        long long expiretime) {
    int savelru = server.maxmemory_policy & MAXMEMORY_FLAG_LRU;
    int savelfu = server.maxmemory_policy & MAXMEMORY_FLAG_LFU;

    /* save expire/type/key */
    if (expiretime != -1) {
        if (rdbSaveType(rdb,RDB_OPCODE_EXPIRETIME_MS) == -1) return -1;
        if (rdbSaveMillisecondTime(rdb,expiretime) == -1) return -1;
    }    

    /* Save the LRU info. */
    if (savelru) {
        uint64_t idletime = estimateObjectIdleTime(evict);
        idletime /= 1000; /* Using seconds is enough and requires less space.*/
        if (rdbSaveType(rdb,RDB_OPCODE_IDLE) == -1) return -1;
        if (rdbSaveLen(rdb,idletime) == -1) return -1;
    }

    /* Save the LFU info. */
    if (savelfu) {
        uint8_t buf[1];
        buf[0] = LFUDecrAndReturn(evict);
        /* We can encode this in exactly two bytes: the opcode and an 8
         * bit counter, since the frequency is logarithmic with a 0-255 range.
         * Note that we do not store the halving time because to reset it
         * a single time when loading does not affect the frequency much. */
        if (rdbSaveType(rdb,RDB_OPCODE_FREQ) == -1) return -1;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
    }

    /* Save type, key, value */
    if (rdbSaveObjectType(rdb,evict) == -1) return -1;
    if (rdbSaveStringObject(rdb,key) == -1) return -1;
    if (rdbWriteRaw(rdb,raw,sdslen(raw)) == -1) return -1;

    /* Delay return if required (for testing) */
    if (server.rdb_key_save_delay)
        debugDelay(server.rdb_key_save_delay);

    return 1;
}

int rdbSaveRocks(rio *rdb, redisDb *db, int rdbflags) {
    rocksIter *it;
    sds cached_key;
    sds rawkey, rawval;

    if (db->id > 0) return C_OK; /*TODO support multi-db */

    if (!(it = rocksCreateIter(server.rocks, db))) {
        serverLog(LL_WARNING, "Create rocks iterator failed.");
        return C_ERR;
    }

    cached_key = sdsnewlen(NULL,CACHED_MAX_KEY_LEN);

    if (!rocksIterSeekToFirst(it)) goto end;

    do {
        robj keyobj, *evict;
        long long expire;
        int obj_type;
        const char *keyptr;
        size_t klen;
        sds key;
        int retval;

        rocksIterKeyValue(it, &rawkey, &rawval);

        obj_type = rocksDecodeKey(rawkey, sdslen(rawkey), &keyptr, &klen);
        if (klen > CACHED_MAX_KEY_LEN) {
            key = sdsnewlen(keyptr, klen);
        } else {
            memcpy(cached_key, keyptr, klen);
            cached_key[klen] = '\0';
            sdssetlen(cached_key, klen);
            key = cached_key;
        }

        initStaticStringObject(keyobj, key);
        evict = lookupEvictKey(db, &keyobj);
        if (evict == NULL || evict->type != obj_type) {
            if (evict != NULL) {
                serverLog(LL_WARNING,
                        "Save object rocks type(%d) not match "
                        "evict type(%d) for key: %s",
                        obj_type, evict->type, key);
            }
            if (key != cached_key) sdsfree(key);
            continue;
        }

        expire = getExpire(db, &keyobj);
        retval = rdbSaveKeyRawPair(rdb,&keyobj,evict,rawval,expire);
        if (key != cached_key) sdsfree(key);

        if (retval == -1) {
            serverLog(LL_WARNING, "Save Raw value failed for key: %s.",key);
            goto err;
        }
        rdbSaveProgress(rdb,rdbflags);
    } while(rocksIterNext(it));

end:
    sdsfree(cached_key);
    rocksReleaseIter(it);
    return C_OK;

err:
    sdsfree(cached_key);
    rocksReleaseIter(it);
    return C_ERR;
}

void evictStopLoading(int success) {
    UNUSED(success);
    rocksIODrain(server.rocks, -1);
    parallelSwapFree(server.rdb_load_ps);
    server.rdb_load_ps = NULL;
}

#define LOAD_ERR 1 
#define WRITE_ERR 2


int rioPipe(rio* src, rio* target,void* enc, size_t len) {
    if(enc == NULL) {
        char buf[len];
        if(rioRead(src, buf, len) == 0) return 0;
        if(rioWrite(target, buf, len) == 0) return 0;
    } else {
        if(rioRead(src, enc, len) == 0) return 0;
        if(rioWrite(target, enc, len) == 0) return 0;
    }    
    return 1; 
}

int rdbPipeLenByRef(rio* src, rio* target, int *isencoded, uint64_t *lenptr) {
    unsigned char buf[2];
    int type;
    if (rioPipe(src, target, buf, 1) == 0) return -1;
    type = (buf[0]&0xC0) >> 6;
    if (type == RDB_ENCVAL) {
        /* Read a 6 bit len. */
        *lenptr = buf[0]&0x3F;
        if (isencoded) *isencoded = 1;
        *lenptr = buf[0]&0x3F;
    } else if(type == RDB_6BITLEN) {
        /* Read a 6 bit len. */
        *lenptr = buf[0]&0x3F;
    } else if(type == RDB_14BITLEN) {
        /* Read a 14 bit len. */
        if (rioPipe(src,target,buf + 1, 1) == 0) return -1;
        *lenptr = ((buf[0]&0x3F)<<8)|buf[1];
    } else if (buf[0] == RDB_32BITLEN) {
        /* Read a 32 bit len. */
        uint32_t len;
        if (rioPipe(src,target,&len, 4) == 0) return -1;
        *lenptr = ntohl(len);
    } else if (buf[0] == RDB_64BITLEN) {
        /* Read a 64 bit len. */
        uint64_t len;
        if (rioRead(src,target,8) == 0) return -1;
        *lenptr = ntohu64(len);
    } else {
        serverLog(LL_WARNING,
            "[rdbPipeLenByRef]Unknown length encoding %d in rdbLoadLen()",type);
        return -1; /* Never reached. */
    }
    return 0;
}

int rdbPipeLen(rio* src, rio* target, int *isencoded) {
    uint64_t len;
    if (rdbPipeLenByRef(src,target,isencoded, &len) == -1) return RDB_LENERR;
    return len;
}

int rdbPipeWriteIntegerObject(rio* src, rio* target, int enctype) {
    unsigned char enc[4];
    long long val;

    if (enctype == RDB_ENC_INT8) {
        if (rioPipe(src,target,enc, 1) == 0) return 0;
        val = (signed char)enc[0];
    } else if (enctype == RDB_ENC_INT16) {
        uint16_t v;
        if (rioPipe(src,target,enc,2) == 0) return 0;
        v = enc[0]|(enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == RDB_ENC_INT32) {
        if (rioPipe(src,target,enc, 4) == 0) return 0;
    } else {
        serverLog(LL_WARNING, "[rdbPipeWriteIntegerObject]Unknown RDB integer encoding type %d",enctype);
        return -1; /* Never reached. */
    }
    return 1;
}

int rdbPipeWirteLzfStringObject(rio* src, rio* target) {
    uint64_t len, clen;
    if((clen = rdbPipeLen(src, target, NULL)) == RDB_LENERR) return 0;
    if((len = rdbPipeLen(src, target, NULL)) == RDB_LENERR) return 0;
    if(rioPipe(src, target, NULL, clen) == 0) return 0;
    return 1;
}

int rdbPipeWriteStringObject(rio* src, rio* target, int* error) {
    int isencoded;
    unsigned long long len = rdbLoadLen(src, &isencoded);
    if (len == RDB_LENERR) return 0;
    rdbSaveLen(target, len);
    if (isencoded) {
        switch(len) {
            case RDB_ENC_INT8:
            case RDB_ENC_INT16:
            case RDB_ENC_INT32:
                return rdbPipeWriteIntegerObject(src, target, len);
            case RDB_ENC_LZF:
                return rdbPipeWirteLzfStringObject(src, target);
            default:
                return 0;
        }
    } else {
        return rioPipe(src, target, NULL, len);
    }
}

int rdbPipeWriteHashObject(rio* src, rio* target, int* error) {
    uint64_t len;
    int ret;
    sds field, value;
    dict *dupSearchDict = NULL;
    len = rdbPipeLen(src, target, NULL);
    if (len == RDB_LENERR) {
        return 0;
    }
    if (len == 0) {
        *error = RDB_LOAD_ERR_EMPTY_KEY; 
        return 0;
    }
    while (len > 0) {
        len--;
        //key
        if (rdbPipeWriteStringObject(src, target, error) == 0) {
            return 0;
        }
        //value
        if (rdbPipeWriteStringObject(src, target, error) == 0) {
            return 0;
        }
    }
    return 1;
}
//rdb load
int rdbLoadObjectString(int rdbtype, rio* rdb, sds key, struct ctripRdbLoadResult* result) {
    //TODO 
    rio obj_rio;
    rioInitWithBuffer(&obj_rio, sdsempty());
    int error;
    robj* evict;
    if(rdbtype == RDB_TYPE_STRING) {
        evict = createObject(OBJ_STRING, NULL);
        if(rdbPipeWriteStringObject(rdb, &obj_rio, &error) == 0) {
            result->error = error;
            return 0;
        }
    } else if(rdbtype == RDB_TYPE_HASH) {
        evict = createHashObject();
        if(rdbPipeWriteHashObject(rdb, &obj_rio, &error) == 0) {
            return 0;
        }
    } else if(rdbtype == RDB_TYPE_HASH_ZIPLIST) {
        evict = createObject(OBJ_STRING, NULL);
        evict->type = OBJ_HASH;
        evict->encoding = OBJ_ENCODING_ZIPLIST;
        if(rdbPipeWriteStringObject(rdb, &obj_rio, &error) == 0) {
            result->error = error;
            return 0;
        }
    } else {
        //not support data type
        return -1;
    }
    result->cold_data = obj_rio.io.buffer.ptr;
    result->val = evict;
    return 1;
}

void ctripRdbLoadObject(int rdbtype, rio *rdb, sds key, struct ctripRdbLoadResult* result) {
    int error = result->error;
    if (rdbLoadObjectString(rdbtype, rdb, key, result) != -1) {
        result-> type = COLD_DATA; 
        return;
    } else {
        //hot data save to memory
        robj* val = rdbLoadObject(rdbtype, rdb, key, &error);
        result->type = HOT_DATA;
        result->error = error;
        result->val = val;
    }
    
}


int ctripDbAddColdData(redisDb* db, int datatype, sds key, robj* evict, sds cold_data, long long expire_time) {
    robj keyobj;
    initStaticStringObject(keyobj,key);
    //submit write to rocksdb task 
    if(parallelSwapPut(rocksEncodeKey(datatype, key), cold_data, NULL, NULL)) {
        return 0;
    }
    evict->evicted = 1;
    //add evict
    dbAddEvict(db, &keyobj, evict);
    /* Set the expire time if needed */
    if (expire_time != -1) {
        setExpire(NULL,db,&keyobj,expire_time);
    }
    return 1;
}


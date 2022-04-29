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


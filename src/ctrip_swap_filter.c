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

/* cf compaction filter */
static void dataFilterDestroy(void* arg) { 
    (void)arg; 
}

static const char* dataFilterName(void* arg) {
  (void)arg;
  return "data_cf_filter";
}

static sds rocksdbGet(rocksdb_readoptions_t* ropts, int cf, sds rawkey, char** err) {
    serverAssert(cf < CF_COUNT);
    size_t vallen;
    *err = NULL;
    char* val = rocksdb_get_cf(server.rocks->db, ropts,
            server.rocks->cf_handles[cf],
            rawkey, sdslen(rawkey), &vallen, err);
    if (*err != NULL || val == NULL)  return NULL;       
    sds result = sdsnewlen(val, vallen);
    zlibc_free(val);
    return result;
}


static redisAtomic filterState filter_state = FILTER_STATE_CLOSE; 
int setFilterState(filterState state) {
    atomicSet(filter_state, state);
    return C_OK;
}


static unsigned char metaVersionFilter(void* arg, int level, const char* rawkey,
                                   size_t rawkey_length,
                                   int (*decodekey)(const char*, size_t , int* , const char**, size_t* ,uint64_t*)) {
    UNUSED(arg);
    UNUSED(level);
    unsigned char result = 0;
    int dbid;
    uint64_t key_version;
    const char* key;
    size_t key_len;
    filterState state;
    atomicGet(filter_state, state);
    if (state == FILTER_STATE_CLOSE) return 0;
    int retval = decodekey(rawkey, rawkey_length, &dbid, &key, &key_len, &key_version);
    if (retval != 0) return 0;
    /*  key_version == 0 when data type is string*/
    if (key_version == 0) return 0;
    sds meta_key = encodeMetaKey(dbid, key, key_len);

    char* err = NULL;
    sds meta_val = rocksdbGet(server.rocks->ropts, META_CF, meta_key, &err);
    if (err != NULL) {
        serverLog(LL_NOTICE, "[metaVersionFilter] rockget (%s) meta val fail: %s ", meta_key, err);
        goto end;
    }

    if (meta_val == NULL) {
        result = 1;
        goto end;
    }
    int object_type;
    uint64_t meta_version;
    long long expire;
    const char* extend;
    size_t extend_len;

    retval = rocksDecodeMetaVal(meta_val,sdslen(meta_val),&object_type,&expire,
            &meta_version, &extend,&extend_len);
    if (retval) {
        serverLog(LL_NOTICE, "[metaVersionFilter] decode meta val fail: %s", meta_val);
        goto end;
    }
    if (meta_version > key_version) {
        result = 1;
    }   
end: 
    sdsfree(meta_key);
    if (meta_val != NULL) sdsfree(meta_val);
    if (err != NULL) zlibc_free(err);
    return result;
}

static int decodeDataVersion(const char* rawkey, size_t rawkey_len, int* dbid, const char** key, size_t* key_len, uint64_t* version) {
    const char* subkey;
    size_t subkey_len;
    return rocksDecodeDataKey(rawkey, rawkey_len, dbid, key, key_len, version, &subkey, &subkey_len);
}

static unsigned char dataFilterFilter(void* arg, int level, const char* rawkey,
                                   size_t rawkey_length,
                                   const char* existing_value,
                                   size_t value_length, char** new_value,
                                   size_t* new_value_length,
                                   unsigned char* value_changed) {
    UNUSED(existing_value);
    UNUSED(value_length);
    UNUSED(new_value);
    UNUSED(new_value_length);
    UNUSED(value_changed);
    return metaVersionFilter(arg, level, rawkey, rawkey_length, decodeDataVersion);
}

rocksdb_compactionfilter_t* createDataCfCompactionFilter() {
    return rocksdb_compactionfilter_create(NULL, dataFilterDestroy,
                                              dataFilterFilter, dataFilterName);
}
rocksdb_compactionfilter_t* createMetaCfCompactionFilter() {
    return NULL;
}

static void scoreFilterDestroy(void* arg) { 
    (void)arg; 
}

static const char* scoreFilterName(void* arg) {
  (void)arg;
  return "score_cf_filter";
}

static int decodeScoreVersion(const char* rawkey, size_t rawkey_len, int* dbid, const char** key, size_t* key_len,uint64_t* version) {
    const char* subkey;
    size_t subkey_len;
    double score;
    return decodeScoreKey(rawkey, rawkey_len, dbid, key, key_len, version, &score, &subkey, &subkey_len);
}

static unsigned char scoreFilterFilter(void* arg, int level, const char* rawkey,
                                   size_t rawkey_length,
                                   const char* existing_value,
                                   size_t value_length, char** new_value,
                                   size_t* new_value_length,
                                   unsigned char* value_changed) {
    UNUSED(existing_value);
    UNUSED(value_length);
    UNUSED(new_value);
    UNUSED(new_value_length);
    UNUSED(value_changed);
    return metaVersionFilter(arg, level, rawkey, rawkey_length, decodeScoreVersion);
}

rocksdb_compactionfilter_t* createScoreCfCompactionFilter() {
    return  rocksdb_compactionfilter_create(NULL, scoreFilterDestroy,
                                              scoreFilterFilter, scoreFilterName);
}


#ifdef REDIS_TEST
static void rocksdbPut(int cf, sds rawkey, sds rawval, char** err) {
    serverAssert(cf < CF_COUNT);
    *err = NULL;
    rocksdb_put_cf(server.rocks->db, server.rocks->wopts,
            server.rocks->cf_handles[cf],
            rawkey, sdslen(rawkey), rawval, sdslen(rawval), err);
}

static void rocksdbDelete(int cf, sds rawkey, char** err) {
    rocksdb_delete_cf(server.rocks->db, server.rocks->wopts,
            server.rocks->cf_handles[cf],
            rawkey, sdslen(rawkey), err);
    if(rocksdbGet(server.rocks->ropts, cf, rawkey, err) != NULL) {
        *err = "delete fail";
    }
}

void initServer(void);
void initServerConfig(void);
void InitServerLast();
int swapFilterTest(int argc, char **argv, int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;
    server.hz = 10;
    server.verbosity = LL_WARNING;
    robj *key1 = createStringObject("key1",4);
    robj *val1 = createStringObject("val1",4);
    initTestRedisDb();
    redisDb *db = server.db;

    sds subkey = sdsnew("subkey");
    char* err = NULL;
    TEST("exec: data compaction filter func") {

        
        /* test1 no-meta filter */
        {
            /* mock hash */
            sds rawkey = rocksEncodeDataKey(db, key1->ptr, 1, subkey );
            rocksdbPut(DATA_CF,rawkey,val1->ptr, &err);
            test_assert(err == NULL);
            /* compact filter will del data when no-meta */
            rocksdb_compact_range_cf(server.rocks->db, server.rocks->cf_handles[DATA_CF], NULL, 0, NULL, 0);
            sds val = rocksdbGet(server.rocks->ropts, DATA_CF, rawkey, &err);
            test_assert(err == NULL);
            test_assert(val == NULL);
            sdsfree(rawkey);
        }

        /* test2 metaversion > dataversion */
        {
            /* mock hash data */
            sds rawkey = rocksEncodeDataKey(db, key1->ptr, 1, subkey);
            rocksdbPut(DATA_CF,rawkey,val1->ptr, &err);
            test_assert(err == NULL);
            /* mock meta version */
            sds rawmetakey = rocksEncodeMetaKey(db, key1->ptr);
            sds extend = rocksEncodeObjectMetaLen(1);
            sds rawmetaval = rocksEncodeMetaVal(OBJ_HASH, -1, 2, extend);
            rocksdbPut(META_CF, rawmetakey, rawmetaval, &err);
            test_assert(err == NULL);

            //compact filter will del data when metaversion > dataversion 
            rocksdb_compact_range_cf(server.rocks->db, server.rocks->cf_handles[DATA_CF], NULL, 0, NULL, 0);
            sds val = rocksdbGet(server.rocks->ropts, DATA_CF, rawkey, &err);
            test_assert(err == NULL);
            test_assert(val == NULL);

            //clean
            rocksdbDelete(META_CF, rawmetakey, &err);
            test_assert(err == NULL);

            sdsfree(rawkey);
            sdsfree(rawmetakey);
            sdsfree(rawmetaval);

        }


        /* test3 metaversion <= dataversion */
        {
            /* mock string data */
            sds rawkey = rocksEncodeDataKey(db, key1->ptr, 1, subkey);
            rocksdbPut(DATA_CF,rawkey,val1->ptr, &err);
            test_assert(err == NULL);
            /* mock meta version &&  dataversion == metaversion */
            sds rawmetakey = rocksEncodeMetaKey(db, key1->ptr);
            sds extend = rocksEncodeObjectMetaLen(1);
            sds rawmetaval = rocksEncodeMetaVal(OBJ_HASH, -1, 1, extend);
            rocksdbPut(META_CF,rawmetakey,rawmetaval, &err);
            test_assert(err == NULL);

            //compact filter will del data when metaversion > dataversion 
            rocksdb_compact_range_cf(server.rocks->db, server.rocks->cf_handles[DATA_CF], NULL, 0, NULL, 0);
            sds val = rocksdbGet(server.rocks->ropts, DATA_CF, rawkey, &err);
            test_assert(err == NULL);
            test_assert(val != NULL);
            sdsfree(val);

            /* mock string data && dataversion > metaversion */
            rawkey = rocksEncodeDataKey(db, key1->ptr, 2, subkey);
            rocksdbPut(DATA_CF,rawkey,val1->ptr, &err);
            test_assert(err == NULL);
            /* compact filter will del data when metaversion > dataversion */
            rocksdb_compact_range_cf(server.rocks->db, server.rocks->cf_handles[DATA_CF], NULL, 0, NULL, 0);
            val = rocksdbGet(server.rocks->ropts, DATA_CF, rawkey, &err);
            test_assert(err == NULL);
            test_assert(val != NULL);
            sdsfree(val);

            /* clean && free */
            rocksdbDelete(META_CF, rawmetakey, &err);
            test_assert(err == NULL);
            rocksdbDelete(DATA_CF, rawkey, &err);
            test_assert(err == NULL);

            sdsfree(rawkey);
            sdsfree(rawmetakey);
            sdsfree(rawmetaval);
        }

        /* unknow data (unfilte) */
        {
            sds unknow = sdsnew("foo");
            rocksdbPut(DATA_CF,unknow,val1->ptr, &err);
            test_assert(err == NULL);

            rocksdb_compact_range_cf(server.rocks->db, server.rocks->cf_handles[DATA_CF], NULL, 0, NULL, 0);
            sds val = rocksdbGet(server.rocks->ropts, DATA_CF, unknow, &err);
            test_assert(err == NULL);
            test_assert(val != NULL);
            sdsfree(val);
            /* clean */
            rocksdbDelete(DATA_CF, unknow, &err);
            test_assert(err == NULL);
            sdsfree(unknow);
        }

        /* meta version unknow */
        {
            sds rawkey = rocksEncodeDataKey(db, key1->ptr, 1, subkey);
            rocksdbPut(DATA_CF,rawkey,val1->ptr, &err);
            test_assert(err == NULL);
            sds rawmetakey = rocksEncodeMetaKey(db, key1->ptr);
            sds rawmetaval = sdsnew("foo");
            rocksdbPut(META_CF,rawmetakey,rawmetaval, &err);
            test_assert(err == NULL);
            rocksdb_compact_range_cf(server.rocks->db, server.rocks->cf_handles[DATA_CF], NULL, 0, NULL, 0);
            sds val = rocksdbGet(server.rocks->ropts, DATA_CF, rawkey, &err);
            test_assert(err == NULL);
            test_assert(val != NULL);
            sdsfree(val);
            /* clean */
            rocksdbDelete(META_CF, rawmetakey, &err);
            test_assert(err == NULL);
            rocksdbDelete(DATA_CF, rawkey, &err);
            test_assert(err == NULL);

            sdsfree(rawkey);
            sdsfree(rawmetaval);
            sdsfree(rawmetakey);
        }

        /* version == 0 => (type is string)   (unfilter) */
        {
            sds rawkey = rocksEncodeDataKey(db, key1->ptr, 0, NULL);
            rocksdbPut(DATA_CF,rawkey,val1->ptr, &err);
            test_assert(err == NULL);
            rocksdb_compact_range_cf(server.rocks->db, server.rocks->cf_handles[DATA_CF], NULL, 0, NULL, 0);
            sds val = rocksdbGet(server.rocks->ropts, DATA_CF, rawkey, &err);
            test_assert(err == NULL);
            test_assert(val != NULL);
            sdsfree(val);
            /* clean */
            rocksdbDelete(DATA_CF, rawkey, &err);
            test_assert(err == NULL);
            sdsfree(rawkey);
        }
   }

   TEST("exec: score compaction filter -data") {
        /* test1 no-meta filter */
        {
            /* mock score data */
            sds rawscorekey = encodeScoreKey(db, key1->ptr,  1, 10, subkey);
            rocksdbPut(SCORE_CF,rawscorekey,val1->ptr, &err);
            test_assert(err == NULL);
            /* compact filter will del data when no-meta */
            rocksdb_compact_range_cf(server.rocks->db, server.rocks->cf_handles[SCORE_CF], NULL, 0, NULL, 0);
            sds val = rocksdbGet(server.rocks->ropts, SCORE_CF, rawscorekey, &err);
            test_assert(err == NULL);
            test_assert(val == NULL);
            sdsfree(rawscorekey);
        }

        /* test2 metaversion > dataversion */
        {
            sds rawscorekey = encodeScoreKey(db, key1->ptr,  1, 10, subkey);
            rocksdbPut(SCORE_CF,rawscorekey,val1->ptr, &err);
            test_assert(err == NULL);
            /* mock meta version */
            sds rawmetakey = rocksEncodeMetaKey(db, key1->ptr);
            sds extend = rocksEncodeObjectMetaLen(1);
            sds rawmetaval = rocksEncodeMetaVal(OBJ_ZSET, -1, 2, extend);
            rocksdbPut(META_CF,rawmetakey,rawmetaval, &err);
            test_assert(err == NULL);

            /* compact filter will del data when metaversion > dataversion */ 
            rocksdb_compact_range_cf(server.rocks->db, server.rocks->cf_handles[SCORE_CF], NULL, 0, NULL, 0);
            sds val = rocksdbGet(server.rocks->ropts, SCORE_CF, rawscorekey, &err);
            test_assert(err == NULL);
            test_assert(val == NULL);

            /* clean */
            rocksdbDelete(META_CF, rawmetakey, &err);
            test_assert(err == NULL);
            sdsfree(rawscorekey);
            sdsfree(rawmetakey);
            sdsfree(rawmetaval);
            sdsfree(extend);
        }


        /* test3 metaversion <= dataversion */
        {
            sds rawscorekey = encodeScoreKey(db, key1->ptr, 1, 10, subkey);
            rocksdbPut(SCORE_CF,rawscorekey,val1->ptr, &err);
            test_assert(err == NULL);
            /* mock meta version &&  dataversion == metaversion */
            sds rawmetakey = rocksEncodeMetaKey(db, key1->ptr);
            sds extend = rocksEncodeObjectMetaLen(1);
            sds rawmetaval = rocksEncodeMetaVal(OBJ_HASH, -1,  1, extend);
            rocksdbPut(META_CF,rawmetakey,rawmetaval, &err);
            test_assert(err == NULL);

            /* compact filter will del data when metaversion > dataversion */
            rocksdb_compact_range_cf(server.rocks->db, server.rocks->cf_handles[SCORE_CF], NULL, 0, NULL, 0);
            sds val = rocksdbGet(server.rocks->ropts, SCORE_CF, rawscorekey, &err);
            test_assert(err == NULL);
            test_assert(val != NULL);
            sdsfree(val);

            /* mock string data && dataversion > metaversion */
            rawscorekey = encodeScoreKey(db, key1->ptr, 2, 10, subkey);
            rocksdbPut(SCORE_CF,rawscorekey,val1->ptr, &err);
            test_assert(err == NULL);
            /* compact filter will del data when metaversion > dataversion */
            rocksdb_compact_range_cf(server.rocks->db, server.rocks->cf_handles[SCORE_CF], NULL, 0, NULL, 0);
            val = rocksdbGet(server.rocks->ropts, SCORE_CF, rawscorekey, &err);
            test_assert(err == NULL);
            test_assert(val != NULL);
            sdsfree(val);

            /* clean */
            rocksdbDelete(META_CF, rawmetakey, &err);
            test_assert(err == NULL);
            rocksdbDelete(SCORE_CF, rawscorekey, &err);
            test_assert(err == NULL);
            sdsfree(rawscorekey);
            sdsfree(rawmetakey);
            sdsfree(rawmetaval);
        }

        /* unknow data (unfilte) */
        {
            sds unknow = sdsnew("foo");
            rocksdbPut(SCORE_CF,unknow,val1->ptr, &err);
            test_assert(err == NULL);

            rocksdb_compact_range_cf(server.rocks->db, server.rocks->cf_handles[SCORE_CF], NULL, 0, NULL, 0);
            sds val = rocksdbGet(server.rocks->ropts, SCORE_CF, unknow, &err);
            test_assert(err == NULL);
            test_assert(val != NULL);
            sdsfree(val);
            /* clean */
            rocksdbDelete(SCORE_CF, unknow, &err);
            test_assert(err == NULL);
            sdsfree(unknow);

        }
        /* meta version unknow */
        {
            sds rawscorekey = encodeScoreKey(db, key1->ptr, 1, 10, subkey);\
            rocksdbPut(SCORE_CF,rawscorekey,val1->ptr, &err);
            test_assert(err == NULL);
            /* mock meta version &&  dataversion == metaversion */
            sds rawmetakey = rocksEncodeMetaKey(db, key1->ptr);
            sds rawmetaval = sdsnew("foo");
            rocksdbPut(META_CF,rawmetakey,rawmetaval, &err);
            test_assert(err == NULL);
            rocksdb_compact_range_cf(server.rocks->db, server.rocks->cf_handles[SCORE_CF], NULL, 0, NULL, 0);
            sds val = rocksdbGet(server.rocks->ropts, SCORE_CF, rawscorekey, &err);
            test_assert(err == NULL);
            test_assert(val != NULL);
            sdsfree(val);
            /* clean */
            rocksdbDelete(META_CF, rawmetakey, &err);
            test_assert(err == NULL);
            rocksdbDelete(SCORE_CF, rawscorekey, &err);
            test_assert(err == NULL);
            sdsfree(rawmetakey);
            sdsfree(rawmetaval);
            sdsfree(rawscorekey);
        }
    }
    return error;

}

#endif
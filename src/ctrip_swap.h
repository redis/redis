#ifndef  __CTRIP_SWAP_H__
#define  __CTRIP_SWAP_H__

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
#include <rocksdb/c.h>
#include "atomicvar.h"

#define IN        /* Input parameter */
#define OUT       /* Output parameter */
#define INOUT     /* Input/Output parameter */
#define MOVE      /* Moved ownership */

#define DATA_CF 0
#define META_CF 1
#define SCORE_CF 2
#define CF_COUNT 3

#define data_cf_name "default"
#define meta_cf_name "meta"
#define score_cf_name "score"
extern const char *swap_cf_names[CF_COUNT];

/* Delete key in rocksdb after right after swap in. */
#define SWAP_IN_DEL (1U<<0)
/* No need to swap if this is a big object */
#define SWAP_IN_META (1U<<1)
/* Delete key in rocksdb and mock value needed to be swapped in. */
#define SWAP_IN_DEL_MOCK_VALUE (1U<<2)
/* This is a metascan request for scan command. */
#define SWAP_METASCAN_SCAN (1U<<3)
/* This is a metascan request for randomkey command. */
#define SWAP_METASCAN_RANDOMKEY (1U<<4)
/* This is a metascan request for active-expire. */
#define SWAP_METASCAN_EXPIRE (1U<<5)
/* Data swap in will be overwritten by fun dbOverwrite
 * same as SWAP_IN_DEL for collection type(SET, ZSET, LISH, HASH...), same as SWAP_IN for STRING */
#define SWAP_IN_OVERWRITE (1U<<6)
/* whether to expire Keys with generated in writtable slave is decided
 * before submitExpireClientRequest and should not skip expire even
 * if current role is slave. */
#define SWAP_EXPIRE_FORCE (1U<<7)

/* Delete rocksdb data key */
#define SWAP_EXEC_IN_DEL (1U<<0)
/* Put rocksdb meta key */
#define SWAP_EXEC_OUT_META (1U<<1)

/* Don't delete key in keyspace when swap (Delete key in rocksdb) finish. */
#define SWAP_FIN_DEL_SKIP (1U<<8)
/* Propagate expired when swap finish */
#define SWAP_FIN_PROP_EXPIRED (1U<<9)
/* Set object dirty when swap finish */
#define SWAP_FIN_SET_DIRTY (1U<<10)

#define SWAP_NOP    0
#define SWAP_IN     1
#define SWAP_OUT    2
#define SWAP_DEL    3
#define ROCKSDB_UTILS 4
#define SWAP_TYPES  5

static inline const char *swapIntentionName(int intention) {
  const char *name = "?";
  const char *intentions[] = {"NOP", "IN", "OUT", "DEL", "UTILS"};
  if (intention >= 0 && intention < SWAP_TYPES)
    name = intentions[intention];
  return name;
}

static inline int isMetaScanRequest(uint32_t intention_flag) {
    return (intention_flag & SWAP_METASCAN_SCAN) ||
           (intention_flag & SWAP_METASCAN_RANDOMKEY) ||
           (intention_flag & SWAP_METASCAN_EXPIRE);
}

/* Cmd */
#define REQUEST_LEVEL_SVR  0
#define REQUEST_LEVEL_DB   1
#define REQUEST_LEVEL_KEY  2

#define MAX_KEYREQUESTS_BUFFER 8

typedef void (*freefunc)(void *);

static inline const char *requestLevelName(int level) {
  const char *name = "?";
  const char *levels[] = {"svr","db","key"};
  if (level >= 0 && (size_t)level < sizeof(levels)/sizeof(char*))
    name = levels[level];
  return name;
}

#define SEGMENT_TYPE_HOT 0
#define SEGMENT_TYPE_COLD 1
#define SEGMENT_TYPE_BOTH 2

/* Both start and end are inclusive, see addListRangeReply for details. */
typedef struct range {
  long start;
  long end;
} range;

#define KEYREQUEST_TYPE_KEY    0
#define KEYREQUEST_TYPE_SUBKEY 1
#define KEYREQUEST_TYPE_RANGE  2
#define KEYREQUEST_TYPE_SCORE  3
#define KEYREQUEST_TYPE_LEX 4

typedef struct argRewriteRequest {
  int mstate_idx; /* >=0 if current command is a exec, means index in mstate; -1 means req not in multi/exec */
  int arg_idx; /* index of argument to rewrite */
} argRewriteRequest;

static inline void argRewriteRequestInit(argRewriteRequest *arg_req) {
  arg_req->mstate_idx = -1;
  arg_req->arg_idx = -1;
} 

typedef struct keyRequest{
  int dbid;
  int level;
  int cmd_intention;
  int cmd_intention_flags;
  int type;
  robj *key;
  union {
    struct {
      int num_subkeys;
      robj **subkeys;
    } b; /* subkey: hash, set */
    struct {
      int num_ranges;
      range *ranges;
    } l; /* range: list */
    struct {
      zrangespec* rangespec;
      int reverse;
      int limit;
    } zs; /* zset score*/
    struct {
      zlexrangespec* rangespec;
      int reverse;
      int limit;
    } zl; /* zset lex */
  };
  argRewriteRequest list_arg_rewrite[2];
} keyRequest;

void copyKeyRequest(keyRequest *dst, keyRequest *src);
void moveKeyRequest(keyRequest *dst, keyRequest *src);
void keyRequestDeinit(keyRequest *key_request);
void getKeyRequests(client *c, struct getKeyRequestsResult *result);
void releaseKeyRequests(struct getKeyRequestsResult *result);
int getKeyRequestsNone(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsGlobal(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsMetaScan(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);

int getKeyRequestsBitop(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsSort(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);

#define getKeyRequestsHsetnx getKeyRequestsHset
#define getKeyRequestsHget getKeyRequestsHmget
#define getKeyRequestsHdel getKeyRequestsHmget
#define getKeyRequestsHstrlen getKeyRequestsHmget
#define getKeyRequestsHincrby getKeyRequestsHget
#define getKeyRequestsHincrbyfloat getKeyRequestsHmget
#define getKeyRequestsHexists getKeyRequestsHmget
int getKeyRequestsHset(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsHmget(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsHlen(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);

#define getKeyRequestsSadd getKeyRequestSmembers
#define getKeyRequestsSrem getKeyRequestSmembers
#define getKeyRequestsSdiffstore getKeyRequestsSinterstore
#define getKeyRequestsSunionstore getKeyRequestsSinterstore
int getKeyRequestSmembers(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestSmove(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsSinterstore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);

int getKeyRequestsZunionstore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsZinterstore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsZdiffstore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);

int getKeyRequestsRpop(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsBrpop(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsLpop(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsBlpop(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsRpoplpush(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsLmove(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsLindex(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
#define getKeyRequestsLset getKeyRequestsLindex
int getKeyRequestsLrange(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsLtrim(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);

int getKeyRequestsZAdd(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsZScore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsZincrby(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsZrange(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsZrangestore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsSinterstore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsZpopMin(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsZpopMax(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsZrangeByScore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsZrevrangeByScore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsZremRangeByScore1(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
#define getKeyRequestsZremRangeByScore getKeyRequestsZrangeByScore
int getKeyRequestsZrevrangeByLex(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsZrangeByLex(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsZremRangeByLex(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsZlexCount(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);

#define getKeyRequestsSdiffstore getKeyRequestsSinterstore
#define getKeyRequestsSunionstore getKeyRequestsSinterstore
#define getKeyRequestsZrem getKeyRequestsZScore

int getKeyRequestsGeoAdd(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsGeoRadius(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsGeoHash(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsGeoDist(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsGeoSearch(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsGeoSearchStore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
#define getKeyRequestsGeoRadiusByMember getKeyRequestsGeoRadius
#define getKeyRequestsGeoPos getKeyRequestsGeoHash

int getKeyRequestsGtid(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsGtidAuto(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);


#define GET_KEYREQUESTS_RESULT_INIT { {{0}}, NULL, 0, MAX_KEYREQUESTS_BUFFER}

typedef struct getKeyRequestsResult {
	keyRequest buffer[MAX_KEYREQUESTS_BUFFER];
	keyRequest *key_requests;
	int num;
	int size;
} getKeyRequestsResult;

void getKeyRequestsPrepareResult(getKeyRequestsResult *result, int numswaps);
void getKeyRequestsAppendSubkeyResult(getKeyRequestsResult *result, int level, MOVE robj *key, int num_subkeys, MOVE robj **subkeys, int cmd_intention, int cmd_intention_flags, int dbid);
void getKeyRequestsFreeResult(getKeyRequestsResult *result);

void getKeyRequestsAppendRangeResult(getKeyRequestsResult *result, int level, MOVE robj *key, int arg_rewrite0, int arg_rewrite1, int num_ranges, MOVE range *ranges, int cmd_intention, int cmd_intention_flags, int dbid);

#define setObjectDirty(o) do { \
    if (o) o->dirty = 1; \
} while(0)

void dbSetDirty(redisDb *db, robj *key);
int objectIsDirty(robj *o);

/* Object meta */
extern dictType objectMetaDictType;

struct objectMeta;
typedef struct objectMetaType {
  sds (*encodeObjectMeta) (struct objectMeta *object_meta);
  int (*decodeObjectMeta) (struct objectMeta *object_meta, const char* extend, size_t extlen);
  int (*objectIsHot)(struct objectMeta *object_meta, robj *value);
  void (*free)(struct objectMeta *object_meta);
  void (*duplicate)(struct objectMeta *dup_meta, struct objectMeta *object_meta);
} objectMetaType;

typedef struct objectMeta {
  unsigned object_type:4;
  union {
    long long len:60;
    unsigned long long ptr:60;
  };
} objectMeta;

extern objectMetaType lenObjectMetaType;
extern objectMetaType listObjectMetaType;

int buildObjectMeta(int object_type, const char *extend, size_t extlen, OUT objectMeta **pobject_meta);
objectMeta *dupObjectMeta(objectMeta *object_meta);
void freeObjectMeta(objectMeta *object_meta);
sds objectMetaEncode(struct objectMeta *object_meta);
int objectMetaDecode(struct objectMeta *object_meta, const char *extend, size_t extlen);
int keyIsHot(objectMeta *object_meta, robj *value);
sds dumpObjectMeta(objectMeta *object_meta);

static inline void *objectMetaGetPtr(objectMeta *object_meta) {
  return (void*)(long)object_meta->ptr;
}
static inline void objectMetaSetPtr(objectMeta *object_meta, void *ptr) {
  object_meta->ptr = (unsigned long long)ptr;
}

objectMeta *createObjectMeta(int object_type);

objectMeta *createLenObjectMeta(int object_type, size_t len);
sds encodeLenObjectMeta(struct objectMeta *object_meta);
int decodeLenObjectMeta(struct objectMeta *object_meta, const char *extend, size_t extlen);
int lenObjectMetaIsHot(struct objectMeta *object_meta, robj *value);

objectMeta *lookupMeta(redisDb *db, robj *key);
void dbAddMeta(redisDb *db, robj *key, objectMeta *m);
int dbDeleteMeta(redisDb *db, robj *key);

typedef struct swapObjectMeta {
  objectMetaType *omtype;
  objectMeta *object_meta;
  objectMeta *cold_meta;
  robj *value;
} swapObjectMeta;

#define initStaticSwapObjectMeta(_som,_omtype,_object_meta,_value) do { \
    _som.omtype = _omtype; \
    _som.object_meta = _object_meta; \
    _som.value = _value; \
} while(0)

static inline int swapObjectMetaIsHot(swapObjectMeta *som) {
    if (som->value == NULL) return 0;
    if (som->object_meta == NULL) return 1;
    serverAssert(som->object_meta->object_type == som->value->type);
    if (som->omtype->objectIsHot) {
      return som->omtype->objectIsHot(som->object_meta,som->value);
    } else {
      return 0;
    }
}


/* Data */

/* SwapData represents key state when swap start. It is stable during
 * key swapping, misc dynamic data are save in dataCtx. */
typedef struct swapData {
  struct swapDataType *type;
  struct objectMetaType *omtype;
  redisDb *db;
  robj *key; /*own*/
  robj *value; /*own*/
  long long expire;
  objectMeta *object_meta; /* ref */
  objectMeta *cold_meta; /* own, moved from exec */
  objectMeta *new_meta; /* own */
  int object_type;
  unsigned propagate_expire:1;
  unsigned set_dirty:1;
  unsigned del_meta:1;
  unsigned reserved:29;
  void *extends[3];
} swapData;

/* keyRequest: client request parse from command.
 * swapData: key state when swap start.
 * dataCtx: dynamic data when swapping.  */
typedef struct swapDataType {
  char *name;
  int (*swapAna)(struct swapData *data, struct keyRequest *key_request, OUT int *intention, OUT uint32_t *intention_flags, void *datactx);
  int (*encodeKeys)(struct swapData *data, int intention, void *datactx, OUT int *action, OUT int *num, OUT int **cfs, OUT sds **rawkeys);
  int (*encodeData)(struct swapData *data, int intention, void *datactx, OUT int *action, OUT int *num, OUT int **cfs, OUT sds **rawkeys, OUT sds **rawvals);
  int (*decodeData)(struct swapData *data, int num, int *cfs, sds *rawkeys, sds *rawvals, OUT void **decoded);
  int (*swapIn)(struct swapData *data, MOVE void *result, void *datactx);
  int (*swapOut)(struct swapData *data, void *datactx);
  int (*swapDel)(struct swapData *data, void *datactx, int async);
  void *(*createOrMergeObject)(struct swapData *data, MOVE void *decoded, void *datactx);
  int (*cleanObject)(struct swapData *data, void *datactx);
  int (*beforeCall)(struct swapData *data, client *c, void *datactx);
  void (*free)(struct swapData *data, void *datactx);
  int (*rocksDel)(struct swapData *data_,  void *datactx_, int inaction, int num, int* cfs, sds *rawkeys, sds *rawvals, OUT int *outaction, OUT int *outnum, OUT int** outcfs,OUT sds **outrawkeys);
} swapDataType;

swapData *createSwapData(redisDb *db, robj *key, robj *value);
int swapDataSetupMeta(swapData *d, int object_type, long long expire, OUT void **datactx);
int swapDataAlreadySetup(swapData *d);
void swapDataMarkPropagateExpire(swapData *data);
int swapDataAna(swapData *d, struct keyRequest *key_request, int *intention, uint32_t *intention_flag, void *datactx);
sds swapDataEncodeMetaKey(swapData *d);
sds swapDataEncodeMetaVal(swapData *d);
int swapDataEncodeKeys(swapData *d, int intention, void *datactx, int *action, int *num, int **cfs, sds **rawkeys);
int swapDataEncodeData(swapData *d, int intention, void *datactx, int *action, int *num, int **cfs, sds **rawkeys, sds **rawvals);
int swapDataDecodeAndSetupMeta(swapData *d, sds rawval, OUT void **datactx);
int swapDataDecodeData(swapData *d, int num, int *cfs, sds *rawkeys, sds *rawvals, void **decoded);
int swapDataSwapIn(swapData *d, void *result, void *datactx);
int swapDataSwapOut(swapData *d, void *datactx);
int swapDataSwapDel(swapData *d, void *datactx, int async);
void *swapDataCreateOrMergeObject(swapData *d, MOVE void *decoded, void *datactx);
int swapDataCleanObject(swapData *d, void *datactx);
int swapDataBeforeCall(swapData *d, client *c, void *datactx);
int swapDataKeyRequestFinished(swapData *data);
char swapDataGetObjectAbbrev(robj *value);
void swapDataFree(swapData *data, void *datactx);
static inline void swapDataSetObjectMeta(swapData *d, objectMeta *object_meta) {
    d->object_meta = object_meta;
}
static inline void swapDataSetColdObjectMeta(swapData *d, MOVE objectMeta *cold_meta) {
    d->cold_meta = cold_meta;
}
static inline void swapDataSetNewObjectMeta(swapData *d, MOVE objectMeta *new_meta) {
    d->new_meta = new_meta;
}
static inline int swapDataIsCold(swapData *data) {
  return data->value == NULL;
}
static inline int swapDataIsHot(swapData *data) {
  swapObjectMeta som;
  initStaticSwapObjectMeta(som,data->omtype,data->object_meta,data->value);
  return swapObjectMetaIsHot(&som);
}
static inline objectMeta *swapDataObjectMeta(swapData *d) {
    serverAssert(
        !(d->object_meta && d->new_meta) || 
        !(d->object_meta && d->cold_meta) ||
        !(d->new_meta && d->cold_meta));

    if (d->object_meta) return d->object_meta;
    if (d->cold_meta) return d->cold_meta;
    return d->new_meta;
}
static inline int swapDataPersisted(swapData *d) {
    return d->object_meta || d->cold_meta;
}
static inline void swapDataObjectMetaModifyLen(swapData *d, int delta) {
    objectMeta *object_meta = swapDataObjectMeta(d);
    object_meta->len += delta;
}
static inline void swapDataObjectMetaSetPtr(swapData *d, void *ptr) {
    objectMeta *object_meta = swapDataObjectMeta(d);
    object_meta->ptr = (unsigned long long)(long)ptr;
}
void swapDataTurnWarmOrHot(swapData *data);
void swapDataTurnCold(swapData *data);
void swapDataTurnDeleted(swapData *data,int del_skip);


/* Debug msgs */
#ifdef SWAP_DEBUG
#define MAX_MSG    64
#define MAX_STEPS  16

typedef struct swapDebugMsgs {
  char identity[MAX_MSG];
  struct swapCtxStep {
    char name[MAX_MSG];
    char info[MAX_MSG];
  } steps[MAX_STEPS];
  int index;
} swapDebugMsgs;

void swapDebugMsgsInit(swapDebugMsgs *msgs, char *identity);
#ifdef __GNUC__
void swapDebugMsgsAppend(swapDebugMsgs *msgs, char *step, char *fmt, ...)
  __attribute__((format(printf, 3, 4)));
#else
void swapDebugMsgsAppend(swapDebugMsgs *msgs, char *step, char *fmt, ...);
#endif
void swapDebugMsgsDump(swapDebugMsgs *msgs);

#define DEBUG_MSGS_INIT(msgs, identity) do { if (msgs) swapDebugMsgsInit(msgs, identity); } while (0)
#define DEBUG_MSGS_APPEND(msgs, step, ...) do { if (msgs) swapDebugMsgsAppend(msgs, step, __VA_ARGS__); } while (0)
#else
#define DEBUG_MSGS_INIT(msgs, identity)
#define DEBUG_MSGS_APPEND(msgs, step, ...)
#endif

/* Swap */
#define SWAP_ERR_SETUP_FAIL -100
#define SWAP_ERR_SETUP_UNSUPPORTED -101
#define SWAP_ERR_DATA_FAIL -200
#define SWAP_ERR_DATA_ANA_FAIL -201
#define SWAP_ERR_DATA_DECODE_FAIL -202
#define SWAP_ERR_DATA_FIN_FAIL -203
#define SWAP_ERR_DATA_UNEXPECTED_INTENTION -204
#define SWAP_ERR_DATA_DECODE_META_FAILED -205
#define SWAP_ERR_EXEC_FAIL -300
#define SWAP_ERR_EXEC_RIO_FAIL -301
#define SWAP_ERR_EXEC_UNEXPECTED_ACTION -302
#define SWAP_ERR_EXEC_UNEXPECTED_UTIL -303
#define SWAP_ERR_METASCAN_CURSOR_INVALID -401
#define SWAP_ERR_METASCAN_UNSUPPORTED_IN_MULTI -402

struct swapCtx;

typedef void (*clientKeyRequestFinished)(client *c, struct swapCtx *ctx);

typedef struct swapCtx {
  client *c;
  keyRequest key_request[1];
  unsigned int expired:1;
  unsigned int set_dirty:1;
  unsigned int reserved:32;
  swapData *data;
  void *datactx;
  clientKeyRequestFinished finished;
  int errcode;
  void *swap_lock;
#ifdef SWAP_DEBUG
  swapDebugMsgs msgs;
#endif
} swapCtx;

swapCtx *swapCtxCreate(client *c, keyRequest *key_request, clientKeyRequestFinished finished);
void swapCtxSetSwapData(swapCtx *ctx, MOVE swapData *data, MOVE void *datactx);
void swapCtxFree(swapCtx *ctx);

void pauseClientSwap(int pause_type);
void resumeClientSwap();
void processResumedClientKeyRequests(void);

/* see server.req_submitted */
#define REQ_SUBMITTED_NONE 0
#define REQ_SUBMITTED_BGSAVE (1ULL<<0)
#define REQ_SUBMITTED_REPL_START (1ULL<<1)

/* String */
typedef struct wholeKeySwapData {
  swapData d;
} wholeKeySwapData;

int swapDataSetupWholeKey(swapData *d, OUT void **datactx);

/* Hash */
typedef struct hashSwapData {
  swapData d;
} hashSwapData;

#define BIG_DATA_CTX_FLAG_NONE 0
#define BIG_DATA_CTX_FLAG_MOCK_VALUE (1U<<0)
typedef struct baseBigDataCtx {
    int num;
    robj **subkeys;
    int ctx_flag;
} baseBigDataCtx;

typedef struct hashDataCtx {
    baseBigDataCtx ctx;
} hashDataCtx;

int swapDataSetupHash(swapData *d, OUT void **datactx);

#define hashObjectMetaType lenObjectMetaType
#define createHashObjectMeta(len) createLenObjectMeta(OBJ_HASH, len)

/* Set */
typedef struct setSwapData {
    swapData d;
} setSwapData;

typedef struct setDataCtx {
    baseBigDataCtx ctx;
} setDataCtx;

int swapDataSetupSet(swapData *d, OUT void **datactx);

#define setObjectMetaType lenObjectMetaType
#define createSetObjectMeta(len) createLenObjectMeta(OBJ_SET, len)

/* List */
typedef struct listSwapData {
  swapData d;
} listSwapData;

typedef struct listDataCtx {
  struct listMeta *swap_meta;
  argRewriteRequest arg_reqs[2];
  int ctx_flag;
} listDataCtx;

objectMeta *createListObjectMeta(MOVE struct listMeta *list_meta);
int swapDataSetupList(swapData *d, void **pdatactx);

typedef struct argRewrite {
  argRewriteRequest arg_req;
  robj *orig_arg; /* own */
} argRewrite;

#define ARG_REWRITES_MAX 2
typedef struct argRewrites {
  int num;
  argRewrite rewrites[ARG_REWRITES_MAX];
} argRewrites;

argRewrites *argRewritesCreate();
void argRewritesAdd(argRewrites *arg_rewrites, argRewriteRequest arg_req, MOVE robj *orig_arg);
void argRewritesReset(argRewrites *arg_rewrites);
void argRewritesFree(argRewrites *arg_rewrites);

void clientArgRewritesRestore(client *c);

long ctripListTypeLength(robj *list, objectMeta *object_meta);
void ctripListTypePush(robj *subject, robj *value, int where, redisDb *db, robj *key);
robj *ctripListTypePop(robj *subject, int where, redisDb *db, robj *key);
void ctripListMetaDelRange(redisDb *db, robj *key, long ltrim, long rtrim);
/* zset */
typedef struct zsetSwapData {
  swapData sd;
} zsetSwapData;
#define TYPE_NONE 0
#define TYPE_ZS 1
#define TYPE_ZL 2

typedef struct zsetDataCtx {
	baseBigDataCtx bdc;
  int type;
  union {
    struct {
      zrangespec* rangespec;
      int reverse;
      int limit;
    } zs;
    struct {
      zlexrangespec* rangespec;
      int reverse;
      int limit;
    } zl;
  };

} zsetDataCtx;
int swapDataSetupZSet(swapData *d, OUT void **datactx);
#define createZsetObjectMeta(len) createLenObjectMeta(OBJ_ZSET, len)
#define zsetObjectMetaType lenObjectMetaType


/* MetaScan */
#define DEFAULT_SCANMETA_BUFFER 16

typedef struct scanMeta {
  sds key;
  long long expire;
  int object_type;
} scanMeta;

int scanMetaExpireIfNeeded(redisDb *db, scanMeta *meta);

typedef struct metaScanResult {
  scanMeta buffer[DEFAULT_SCANMETA_BUFFER];
  scanMeta *metas;
  int num;
  int size;
  sds nextseek;
} metaScanResult;

metaScanResult *metaScanResultCreate();
void metaScanResultAppend(metaScanResult *result, int object_type, MOVE sds key, long long expire);
void metaScanResultSetNextSeek(metaScanResult *result, MOVE sds nextseek);
void freeScanMetaResult(metaScanResult *result);

typedef struct metaScanSwapData {
  swapData d;
} metaScanSwapData;

/* There are three kinds of meta scan ctx: scan/randomkey/activeexpire */
struct metaScanDataCtx;

typedef struct metaScanDataCtxType {
    void (*swapAna)(struct metaScanDataCtx *datactx, int *intention, uint32_t *intention_flags);
    void (*swapIn)(struct metaScanDataCtx *datactx, metaScanResult *result);
    void (*freeExtend)(struct metaScanDataCtx *datactx);
} metaScanDataCtxType;

typedef struct metaScanDataCtx {
    metaScanDataCtxType *type;
    client *c;
    int limit;
    sds seek;
    void *extend;
} metaScanDataCtx;

int swapDataSetupMetaScan(swapData *d, uint32_t intention_flags, client *c, OUT void **datactx);

void rewindClientSwapScanCursor(client *c);
robj *metaScanResultRandomKey(redisDb *db, metaScanResult *result);

#define EXPIRESCAN_DEFAULT_LIMIT 32
#define EXPIRESCAN_DEFAULT_CANDIDATES (16*1024)

extern dict *slaveKeysWithExpire;

typedef struct expireCandidates {
    robj *zobj;
    size_t capacity;
} expireCandidates;

expireCandidates *expireCandidatesCreate(size_t capacity);
void freeExpireCandidates(expireCandidates *candidates);

typedef struct scanExpire {
    expireCandidates *candidates;
    int inprogress;
    sds nextseek;
    int limit;
    double stale_percent;
    long long stat_estimated_cycle_seconds;
    size_t stat_scan_per_sec;
    size_t stat_expired_per_sec;
    long long stat_scan_time_used;
    long long stat_expire_time_used;
} scanExpire;

scanExpire *scanExpireCreate();
void scanExpireFree(scanExpire *scan_expire);
void scanExpireEmpty(scanExpire *scan_expire);

void scanexpireCommand(client *c);
int scanExpireDbCycle(redisDb *db, int type, long long timelimit);
sds genScanExpireInfoString(sds info);

void expireSlaveKeysSwapMode(void);

/* Exec */
struct swapRequest;

typedef void (*swapRequestNotifyCallback)(struct swapRequest *req, void *pd);
typedef void (*swapRequestFinishedCallback)(swapData *data, void *pd, int errcode);

#define SWAP_REQUEST_TYPE_DATA 0
#define SWAP_REQUEST_TYPE_META 0

typedef struct swapRequest {
  keyRequest *key_request; /* key_request for meta swap request */
  int intention; /* intention for data swap request */
  uint32_t intention_flags;
  swapCtx *swapCtx;
  swapData *data;
  void *datactx;
  void *result; /* ref (create in decodeData, moved to swapIn) */
  swapRequestNotifyCallback notify_cb;
  void *notify_pd;
  swapRequestFinishedCallback finish_cb;
  void *finish_pd;
  redisAtomic size_t swap_memory;
#ifdef SWAP_DEBUG
  swapDebugMsgs *msgs;
#endif
  int errcode;
} swapRequest;

swapRequest *swapRequestNew(keyRequest *key_request, int intention, uint32_t intention_flags, swapCtx *swapCtx, swapData *data, void *datactx, swapRequestFinishedCallback cb, void *pd, void *msgs);
void swapRequestFree(swapRequest *req);
void processSwapRequest(swapRequest *req);
void finishSwapRequest(swapRequest *req);
void submitSwapDataRequest(int mode, int intention, uint32_t intention_flags, swapCtx *ctx, swapData* data, void *datactx, swapRequestFinishedCallback cb, void *pd, void *msgs, int thread_idx);
void submitSwapMetaRequest(int mode, keyRequest *key_request, swapCtx *ctx, swapData* data, void *datactx, swapRequestFinishedCallback cb, void *pd, void *msgs, int thread_idx);

/* Threads (encode/rio/decode/finish) */
#define SWAP_THREADS_DEFAULT     4
#define SWAP_THREADS_MAX         64

typedef struct swapThread {
    int id;
    pthread_t thread_id;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    list *pending_reqs;
    redisAtomic unsigned long is_running_rio;
} swapThread;

int swapThreadsInit();
void swapThreadsDeinit();
void swapThreadsDispatch(swapRequest *req, int idx);
int swapThreadsDrained();

/* RIO */
#define ROCKS_GET             	1
#define ROCKS_PUT            	  2
#define ROCKS_DEL              	3
#define ROCKS_WRITE             4
#define ROCKS_MULTIGET          5
#define ROCKS_SCAN              6
#define ROCKS_MULTI_DELETERANGE       7
#define ROCKS_ITERATE           8
#define ROCKS_RANGE             9
#define ROCKS_TYPES             10

static inline const char *rocksActionName(int action) {
  const char *name = "?";
  const char *actions[] = {"NOP", "GET", "PUT", "DEL", "WRITE", "MULTIGET", "SCAN", "DELETERANGE", "ITERATE"};
  if (action >= 0 && (size_t)action < sizeof(actions)/sizeof(char*))
    name = actions[action];
  return name;
}

typedef struct RIO {
	int action;
	union {
		struct {
      int cf;
			sds rawkey;
			sds rawval;
		} get;
		struct {
      int cf;
			sds rawkey;
			sds rawval;
		} put;
		struct {
      int cf;
			sds rawkey;
		} del;
		struct {
			rocksdb_writebatch_t *wb;
		} write;
		struct {
			int numkeys;
      int *cfs;
			sds *rawkeys;
			sds *rawvals;
		} multiget;
		struct {
      int cf;
			sds prefix;
			int numkeys;
			sds *rawkeys;
			sds *rawvals;
		} scan;
		struct {
      int cf;
			sds start_key;
      sds end_key;
		} delete_range;
    struct {
      int cf;
      sds seek;
      int limit;
      int numkeys;
      sds *rawkeys;
      sds *rawvals;
    } iterate;
    struct {
      rocksdb_writebatch_t *wb;
    } multidel;
    struct {
      int num;
      int* cf;
      sds* rawkeys;
    } multidel_range;

    struct {
      sds start;
      sds end;
      int reverse;
      size_t limit;
      int cf;
      int numkeys;
      sds *rawkeys;
      sds *rawvals;
    } range;
	};
  sds err;
} RIO;

void RIOInitGet(RIO *rio, int cf, sds rawkey);
void RIOInitPut(RIO *rio, int cf, sds rawkey, sds rawval);
void RIOInitDel(RIO *rio, int cf, sds rawkey);
void RIOInitWrite(RIO *rio, rocksdb_writebatch_t *wb);
void RIOInitMultiGet(RIO *rio, int numkeys, int *cfs, sds *rawkeys);
void RIOInitScan(RIO *rio, int cf, sds prefix);
void RIOInitMultiDeleteRange(RIO* rio, int num, int* cf , sds* rawkeys);
void RIOInitIterate(RIO *rio, int cf, sds seek, int limit);
void RIODeinit(RIO *rio);

#define SWAP_MODE_ASYNC 0
#define SWAP_MODE_PARALLEL_SYNC 1

/* Async */
#define ASYNC_COMPLETE_QUEUE_NOTIFY_READ_MAX  512

typedef struct asyncCompleteQueue {
    int notify_recv_fd;
    int notify_send_fd;
    pthread_mutex_t lock;
    list *complete_queue;
} asyncCompleteQueue;

int asyncCompleteQueueInit();
void asyncCompleteQueueDeinit(asyncCompleteQueue *cq);
void asyncCompleteQueueAppend(asyncCompleteQueue *cq, swapRequest *req);
int asyncCompleteQueueDrain(mstime_t time_limit);

void asyncSwapRequestSubmit(swapRequest *req, int idx);

/* Parallel sync */
typedef struct {
    int inprogress;         /* swap entry in progress? */
    int pipe_read_fd;       /* read end to wait rio swap finish. */
    int pipe_write_fd;      /* write end to notify swap finish by rio. */
    swapRequest *req;
} swapEntry;

typedef struct parallelSync {
    list *entries;
    int parallel;
    int mode;
} parallelSync;

int parallelSyncInit(int parallel);
void parallelSyncDeinit();
int parallelSyncDrain();

int parallelSyncSwapRequestSubmit(swapRequest *req, int idx);

/* Lock */
#define DEFAULT_REQUEST_LISTENER_REENTRANT_SIZE 1
#define REQUEST_NOTIFY_ACK  (1<<0)
#define REQUEST_NOTIFY_RLS  (1<<1)

typedef void (*requestProceed)(void *listeners, redisDb *db, robj *key, client *c, void *pd);

typedef struct requestListenerEntry {
  redisDb *db;    /* key level request listener might bind on svr/db level */
  robj *key;      /* so we need to save db&key for each requst listener. */
  client *c;
  requestProceed proceed;
  void *pd;
  freefunc pdfree;
#ifdef SWAP_DEBUG
  swapDebugMsgs *msgs;
#endif
} requestListenerEntry;

typedef struct requestListener {
  int64_t txid;
  struct requestListenerEntry buf[DEFAULT_REQUEST_LISTENER_REENTRANT_SIZE];
  struct requestListenerEntry *entries; /* hold entries that wait for same listener reentrantly. */
  int capacity;
  int count;
  int proceeded;
  int acked;
  int notified;
  int ntxlistener; /* # of txlistener of current and childs. */
  int ntxrequest;
  int ntxacked;
} requestListener;

typedef struct requestListeners {
  list *listeners;                  /* list of listenters */
  struct requestListeners *parent;  /* parent lisenter */
  int nlistener;                   /* # of listeners of current and childs */
  int level;                        /* key/db/svr */
  union {
      struct {
          int dbnum;
          struct requestListeners **dbs;
      } svr;
      struct {
          redisDb *db;
          dict *keys;
      } db;
      struct {
          robj *key;
      } key;
  };
  int64_t cur_txid;
  int cur_ntxlistener;
  int cur_ntxrequest; 
  /* # of acked requests for successive GetIOAndLock of cur_txid. note that
   * cur_ntxacked is not correct when requestReleaseLock. */
  int cur_ntxacked; 
} requestListeners;

requestListeners *serverRequestListenersCreate(void);
void serverRequestListenersRelease(requestListeners *s);
int requestLockWouldBlock(int64_t txid, redisDb *db, robj *key);
void requestGetIOAndLock(int64_t txid, redisDb *db, robj *key, requestProceed cb, client *c, void *pd, freefunc pdfree, void *msgs);
int requestReleaseIO(void *listeners);
int requestReleaseLock(void *listeners);

list *clientRenewRequestLocks(client *c);
void clientGotRequestIOAndLock(client *c, swapCtx *ctx, void *lock);
void clientReleaseRequestLocks(client *c, swapCtx *ctx);

/* Evict */
#define EVICT_SUCC_SWAPPED      1
#define EVICT_SUCC_FREED        2
#define EVICT_FAIL_ABSENT       -1
#define EVICT_FAIL_EVICTED      -2
#define EVICT_FAIL_SWAPPING     -3
#define EVICT_FAIL_HOLDED       -4
#define EVICT_FAIL_UNSUPPORTED  -5

int tryEvictKey(redisDb *db, robj *key, int *evict_result);
void tryEvictKeyAsapLater(redisDb *db, robj *key);
void evictCommand(client *c);
int evictAsap();
void debugEvictKeys();
void holdKey(redisDb *db, robj *key, int64_t swap);
void unholdKey(redisDb *db, robj *key);
void clientHoldKey(client *c, robj *key, int64_t swap);
void clientUnholdKeys(client *c);
void clientUnholdKey(client *c, robj *key);

/* Expire */
int submitExpireClientRequest(client *c, robj *key, int force);

/* Rocks */
#define ROCKS_DIR_MAX_LEN 512
#define ROCKS_DATA "data.rocks"
#define ROCKS_DISK_HEALTH_DETECT_FILE "disk_health_detect"

/* Rocksdb engine */
typedef struct rocks {
    rocksdb_t *db;
    rocksdb_options_t *cf_opts[CF_COUNT];
    rocksdb_block_based_table_options_t *block_opts[CF_COUNT];
    rocksdb_column_family_handle_t *cf_handles[CF_COUNT];
    rocksdb_options_t *db_opts;
    rocksdb_readoptions_t *ropts;
    rocksdb_writeoptions_t *wopts;
    const rocksdb_snapshot_t *snapshot;
    rocksdb_checkpoint_t* checkpoint;
    sds checkpoint_dir;
    char* rocksdb_stats_cache;
} rocks;

typedef struct rocksdbMemOverhead {
  size_t total;
  size_t block_cache;
  size_t index_and_filter;
  size_t memtable;
  size_t pinned_blocks;
} rocksdbMemOverhead;

int rocksInit(void);
void rocksRelease(void);
void rocksReinit(void);
int rocksFlushAll(void);
void rocksCron(void);
void rocksCreateSnapshot(void);
int rocksCreateCheckpoint(sds checkpoint_dir);
void rocksReleaseCheckpoint(void);
void rocksUseSnapshot(void);
void rocksReleaseSnapshot(void);
struct rocksdbMemOverhead *rocksGetMemoryOverhead();
void rocksFreeMemoryOverhead(struct rocksdbMemOverhead *mh);
rocksdb_t *rocksGetDb(void);
sds genRocksInfoString(sds info);

/* Repl */
int submitReplClientRequests(client *c);

/* Swap */
void swapInit(void);
int dbSwap(client *c);
int clientSwap(client *c);
void continueProcessCommand(client *c);
int replClientSwap(client *c);
int replClientDiscardDispatchedCommands(client *c);
void replClientDiscardSwappingState(client *c);
void submitClientKeyRequests(client *c, getKeyRequestsResult *result, clientKeyRequestFinished cb);
int submitNormalClientRequests(client *c);
void keyRequestBeforeCall(client *c, swapCtx *ctx);
void mutexopCommand(client *c);
int lockGlobalAndExec(clientKeyRequestFinished locked_op, uint64_t exclude_mark);


/* Swap rate limit */
#define SWAP_RL_NO      0
#define SWAP_RL_SLOW    1
#define SWAP_RL_STOP    2

#define SWAP_STAT_METRIC_COUNT 0
#define SWAP_STAT_METRIC_MEMORY 1
#define SWAP_STAT_METRIC_SIZE 2

#define SWAP_SWAP_STATS_METRIC_COUNT (SWAP_STAT_METRIC_SIZE*SWAP_TYPES)
#define SWAP_RIO_STATS_METRIC_COUNT (SWAP_STAT_METRIC_SIZE*ROCKS_TYPES)
/* stats metrics are ordered mem>swap>rio */ 
#define SWAP_SWAP_STATS_METRIC_OFFSET STATS_METRIC_COUNT_MEM
#define SWAP_RIO_STATS_METRIC_OFFSET (SWAP_SWAP_STATS_METRIC_OFFSET+SWAP_SWAP_STATS_METRIC_COUNT)

#define SWAP_STATS_METRIC_COUNT (SWAP_SWAP_STATS_METRIC_COUNT+SWAP_RIO_STATS_METRIC_COUNT)

typedef struct swapStat {
    const char *name;
    redisAtomic size_t count;
    redisAtomic size_t memory;
    int stats_metric_idx_count;
    int stats_metric_idx_memory;
} swapStat;

void initStatsSwap(void);
void resetStatsSwap(void);
void updateStatsSwapStart(swapRequest *req);
void updateStatsSwapRIO(swapRequest *req, RIO *rio);
void updateStatsSwapFinish(swapRequest *req);

void trackSwapInstantaneousMetrics();
sds genSwapInfoString(sds info);

int swapRateLimitState(void);
int swapRateLimit(client *c);
int swapRateLimited(client *c);

/* Rocks iter thread */
#define ITER_BUFFER_CAPACITY_DEFAULT 4096
#define ITER_NOTIFY_BATCH 32

typedef struct iterResult {
    int cf;
    sds rawkey;
    unsigned char rdbtype;
    sds rawval;
} iterResult;

typedef struct bufferedIterCompleteQueue {
    int buffer_capacity;
    iterResult *buffered;
    int iter_finished;
    int64_t buffered_count;
    int64_t processed_count;
    pthread_mutex_t buffer_lock;
    pthread_cond_t ready_cond;
    pthread_cond_t vacant_cond;
} bufferedIterCompleteQueue;

typedef struct rocksIter{
    redisDb *db;
    struct rocks *rocks;
    pthread_t io_thread;
    int io_thread_exited;
    pthread_mutex_t io_thread_exit_mutex;
    bufferedIterCompleteQueue *buffered_cq;
    rocksdb_column_family_handle_t *cf_handles[CF_COUNT];
    rocksdb_iterator_t *data_iter;
    rocksdb_iterator_t *meta_iter;
    rocksdb_t* checkpoint_db;
} rocksIter;

rocksIter *rocksCreateIter(struct rocks *rocks, redisDb *db);
int rocksIterSeekToFirst(rocksIter *it);
int rocksIterNext(rocksIter *it);
void rocksIterCfKeyTypeValue(rocksIter *it, int *cf, sds *rawkey, unsigned char *type, sds *rawval);
void rocksReleaseIter(rocksIter *it);
void rocksIterGetError(rocksIter *it, char **error);

/* Rdb save */
#define DEFAULT_STRING_SIZE 512 
#define DEFAULT_HASH_FIELD_COUNT 8
#define DEFAULT_HASH_FIELD_SIZE 256
#define DEFAULT_SET_MEMBER_COUNT 8
#define DEFAULT_SET_MEMBER_SIZE 128
#define DEFAULT_LIST_ELE_COUNT 32
#define DEFAULT_LIST_ELE_SIZE 128
#define DEFAULT_ZSET_MEMBER_COUNT 16
#define DEFAULT_ZSET_MEMBER_SIZE 128

/* result that decoded from current rocksIter value */
typedef struct decodedResult {
  int cf;
  int dbid;
  sds key;
  void *reserved[6];
} decodedResult;

typedef struct decodedMeta {
  int cf;
  int dbid;
  sds key;
  int object_type;
  long long expire;
  sds extend;
} decodedMeta;

typedef struct decodedData {
  int cf;
  int dbid;
  sds key;
  sds subkey;
  int rdbtype;
  sds rdbraw;
} decodedData;

void decodedResultInit(decodedResult *decoded);
void decodedResultDeinit(decodedResult *decoded);

struct rdbKeySaveData;
typedef struct rdbKeySaveType {
  int (*save_setup)(struct rdbKeySaveData *keydata, rio *rdb);
  int (*save_start)(struct rdbKeySaveData *keydata, rio *rdb);
  int (*save)(struct rdbKeySaveData *keydata, rio *rdb, decodedData *decoded);
  int (*save_end)(struct rdbKeySaveData *keydata, rio *rdb, int save_result);
  void (*save_deinit)(struct rdbKeySaveData *keydata);
} rdbKeySaveType;

typedef struct rdbKeySaveData {
  rdbKeySaveType *type;
  objectMetaType *omtype;
  robj *key; /* own */
  robj *value; /* ref: incrRefcount will cause cow */
  objectMeta *object_meta; /* own */
  long long expire;
  int saved;
  void *iter; /* used by list (metaListIterator) */
} rdbKeySaveData;

typedef struct rdbSaveRocksStats {
    long long iter_decode_ok;
    long long iter_decode_err;
    long long init_save_ok;
    long long init_save_skip;
    long long init_save_err;
    long long save_ok;
} rdbSaveRocksStats;

/* rdb save */
int rdbSaveRocks(rio *rdb, int *error, redisDb *db, int rdbflags);
int rdbSaveKeyHeader(rio *rdb, robj *key, robj *evict, unsigned char rdbtype, long long expiretime);
int rdbKeySaveDataInit(rdbKeySaveData *keydata, redisDb *db, decodedResult *dr);
void rdbKeySaveDataDeinit(rdbKeySaveData *keydata);
int rdbKeySaveStart(struct rdbKeySaveData *keydata, rio *rdb);
int rdbKeySave(struct rdbKeySaveData *keydata, rio *rdb, decodedData *d);
int rdbKeySaveEnd(struct rdbKeySaveData *keydata, rio *rdb, int save_result);
void wholeKeySaveInit(rdbKeySaveData *keydata);
int hashSaveInit(rdbKeySaveData *save, const char *extend, size_t extlen);
int setSaveInit(rdbKeySaveData *save, const char *extend, size_t extlen);
int listSaveInit(rdbKeySaveData *save, const char *extend, size_t extlen);
int zsetSaveInit(rdbKeySaveData *save, const char *extend, size_t extlen);

/* Rdb load */
/* RDB_LOAD_ERR_*: [1 +inf), SWAP_ERR_RDB_LOAD_*: (-inf -500] */
#define SWAP_ERR_RDB_LOAD_UNSUPPORTED -500

#define RDB_LOAD_BATCH_COUNT 50
#define RDB_LOAD_BATCH_CAPACITY  (4*1024*1024)

typedef struct ctripRdbLoadCtx {
  size_t errors;
  size_t idx;
  struct {
    size_t capacity;
    size_t memory;
    size_t count;
    size_t index;
    int *cfs;
    sds *rawkeys;
    sds *rawvals;
  } batch;
} ctripRdbLoadCtx;

void evictStartLoading(void);
void evictStopLoading(int success);

struct rdbKeyLoadData;

typedef struct rdbKeyLoadType {
  void (*load_start)(struct rdbKeyLoadData *keydata, rio *rdb, OUT int *cf, OUT sds *rawkey, OUT sds *rawval, OUT int *error);
  int (*load)(struct rdbKeyLoadData *keydata, rio *rdb, OUT int *cf, OUT sds *rawkey, OUT sds *rawval, OUT int *error);
  int (*load_end)(struct rdbKeyLoadData *keydata,rio *rdb);
  void (*load_deinit)(struct rdbKeyLoadData *keydata);
} rdbKeyLoadType;

typedef struct rdbKeyLoadData {
    rdbKeyLoadType *type;
    objectMetaType *omtype;
    redisDb *db;
    sds key; /* ref */
    long long expire;
    long long now;
    int rdbtype;
    int object_type;
    int nfeeds;
    int total_fields;
    int loaded_fields;
    robj *value;
    void *iter;
} rdbKeyLoadData;

static inline sds rdbVerbatimNew(unsigned char rdbtype) {
    return sdsnewlen(&rdbtype,1);
}

int rdbLoadStringVerbatim(rio *rdb, sds *verbatim);
int rdbLoadHashFieldsVerbatim(rio *rdb, unsigned long long len, sds *verbatim);
int ctripRdbLoadObject(int rdbtype, rio *rdb, redisDb *db, sds key, long long expire, long long now, rdbKeyLoadData *keydata);
robj *rdbKeyLoadGetObject(rdbKeyLoadData *keydata);
int rdbKeyLoadDataInit(rdbKeyLoadData *keydata, int rdbtype, redisDb *db, sds key, long long expire, long long now);
void rdbKeyLoadDataDeinit(rdbKeyLoadData *keydata);
void rdbKeyLoadDataSetup(rdbKeyLoadData *keydata, int rdbtype, redisDb *db, sds key, long long expire, long long now);
void rdbKeyLoadStart(struct rdbKeyLoadData *keydata, rio *rdb, int *cf, sds *rawkey, sds *rawval, int *error);
int rdbKeyLoad(struct rdbKeyLoadData *keydata, rio *rdb, int *cf, sds *rawkey, sds *rawval, int *error);
int rdbKeyLoadEnd(struct rdbKeyLoadData *keydata, rio *rdb);
int rdbKeyLoadDbAdd(struct rdbKeyLoadData *keydata, redisDb *db);
void rdbKeyLoadExpired(struct rdbKeyLoadData *keydata);

#define rdbLoadStartHT rdbLoadStartLenMeta
#define rdbLoadStartSet rdbLoadStartLenMeta
void rdbLoadStartLenMeta(struct rdbKeyLoadData *load, rio *rdb, int *cf, sds *rawkey, sds *rawval, int *error);

void wholeKeyLoadInit(rdbKeyLoadData *keydata);
void hashLoadInit(rdbKeyLoadData *load);
void setLoadInit(rdbKeyLoadData *load);
void listLoadInit(rdbKeyLoadData *load);

void zsetLoadInit(rdbKeyLoadData *load);
int rdbLoadLenVerbatim(rio *rdb, sds *verbatim, int *isencoded, unsigned long long *lenptr);

/* Util */
#define ROCKS_KEY_FLAG_NONE 0x0
#define ROCKS_KEY_FLAG_SUBKEY 0x1
#define ROCKS_KEY_FLAG_DELETE 0xff
#define rocksEncodeMetaKey(db,key)  rocksEncodeDataKey(db,key,NULL)
#define rocksDecodeMetaKey(raw,rawlen,dbid,key,keylen)  rocksDecodeDataKey(raw,rawlen,dbid,key,keylen,NULL,NULL)
#define rocksEncodeDataScanPrefix(db,key) rocksEncodeDataKey(db,key,shared.emptystring->ptr)
sds rocksEncodeDataKey(redisDb *db, sds key, sds subkey);
int rocksDecodeDataKey(const char *raw, size_t rawlen, int *dbid, const char **key, size_t *keylen, const char **subkey, size_t *subkeylen);
sds rocksEncodeMetaVal(int object_type, long long expire, sds extend);
int rocksDecodeMetaVal(const char* raw, size_t rawlen, int *object_type, long long *expire, const char **extend, size_t *extend_len);
sds rocksEncodeValRdb(robj *value);
robj *rocksDecodeValRdb(sds raw);
sds rocksEncodeObjectMetaLen(unsigned long len);
long rocksDecodeObjectMetaLen(const char *raw, size_t rawlen);
sds rocksGenerateEndKey(sds start_key);
sds encodeMetaScanKey(unsigned long cursor, int limit, sds seek);
int decodeMetaScanKey(sds meta_scan_key, unsigned long *cursor, int *limit, const char **seek, size_t *seeklen);

#define sizeOfDouble (BYTE_ORDER == BIG_ENDIAN? sizeof(double):8)
int encodeDouble(char* buf, double value);
int decodeDouble(char* val, double* score);
int decodeScoreKey(char* raw, int rawlen, int* dbid, char** key, size_t* keylen, char** subkey, size_t* subkeylen, double* score);
sds encodeScoreKey(redisDb* db ,sds key, sds subkey, double score);
sds encodeIntervalSds(int ex, MOVE IN sds data);
int decodeIntervalSds(sds data, int* ex, char** raw, size_t* rawlen);
sds encodeScorePrefix(redisDb* db, sds key);

robj *unshareStringValue(robj *value);
size_t objectEstimateSize(robj *o);
size_t keyEstimateSize(redisDb *db, robj *key);
void swapCommand(client *c);
void expiredCommand(client *c);
const char *strObjectType(int type);
int timestampIsExpired(mstime_t expire);
size_t ctripDbSize(redisDb *db);

#define cursorIsHot(outer_cursor) ((outer_cursor & 0x1UL) == 0)
#define cursorOuterToInternal(cursor) (cursor >> 1)
#define cursorInternalToOuter(outer_cursor, cursor) (cursor << 1 | (outer_cursor & 0x1UL))

static inline void clientSwapError(client *c, int swap_errcode) {
  if (c && swap_errcode) {
    atomicIncr(server.swap_error,1);
    c->swap_errcode = swap_errcode;
  }
}

#define COMPACT_RANGE_TASK 0
#define GET_ROCKSDB_STATS_TASK 1
#define TASK_COUNT 2
typedef struct rocksdbUtilTaskManager{
    struct {
        int stat;
    } stats[TASK_COUNT];
} rocksdbUtilTaskManager;
rocksdbUtilTaskManager* createRocksdbUtilTaskManager();
int submitUtilTask(int type, void* ctx, sds* error);

#ifdef REDIS_TEST

#define TEST(name) printf("test — %s\n", name);
#define TEST_DESC(name, ...) printf("test — " name "\n", __VA_ARGS__);
#define test_assert(e) do {							\
	if (!(e)) {				\
		printf(						\
		    "%s:%d: Failed assertion: \"%s\"\n",	\
		    __FILE__, __LINE__, #e);				\
		error++;						\
	}								\
} while (0)

int initTestRedisDb(void);
int initTestRedisServer(void);
int clearTestRedisDb(void);
int clearTestRedisServer(void);
swapData *createWholeKeySwapDataWithExpire(redisDb *db, robj *key, robj *value, long long expire, void **datactx);
swapData *createWholeKeySwapData(redisDb *db, robj *key, robj *value, void **datactx);

int swapDataWholeKeyTest(int argc, char **argv, int accurate);
int swapDataHashTest(int argc, char **argv, int accurate);
robj **mockSubKeys(int num,...);
int swapDataSetTest(int argc, char **argv, int accurate);
int swapDataZsetTest(int argc, char **argv, int accurate);
int swapDataTest(int argc, char *argv[], int accurate);
int swapWaitTest(int argc, char **argv, int accurate);
int swapWaitReentrantTest(int argc, char **argv, int accurate);
int swapWaitAckTest(int argc, char **argv, int accurate);
int swapCmdTest(int argc, char **argv, int accurate);
int swapExecTest(int argc, char **argv, int accurate);
int swapRdbTest(int argc, char **argv, int accurate);
int swapObjectTest(int argc, char *argv[], int accurate);
int swapIterTest(int argc, char *argv[], int accurate);
int metaScanTest(int argc, char *argv[], int accurate);
int swapExpireTest(int argc, char *argv[], int accurate);
int swapUtilTest(int argc, char **argv, int accurate);
int swapListMetaTest(int argc, char *argv[], int accurate);
int swapListDataTest(int argc, char *argv[], int accurate);
int swapListUtilsTest(int argc, char *argv[], int accurate);

int swapTest(int argc, char **argv, int accurate);

#endif

#endif
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

/* --- Cmd --- */
#define REQUEST_LEVEL_SVR  0
#define REQUEST_LEVEL_DB   1
#define REQUEST_LEVEL_KEY  2

/* Delete key in rocksdb after right after swap in. */
#define INTENTION_IN_DEL (1<<0)
/* No need to swap if this is a big object */
#define INTENTION_IN_META (1<<1)
/* Delete key in rocksdb without touching keyspace. */
#define INTENTION_DEL_ASYNC (1<<2)


static inline const char *requestLevelName(int level) {
  const char *name = "?";
  const char *levels[] = {"svr","db","key"};
  if (level >= 0 && (size_t)level < sizeof(levels)/sizeof(char*))
    name = levels[level];
  return name;
}

typedef struct keyRequest{
  int level;
	int num_subkeys;
	robj *key;
	robj **subkeys;
  int cmd_intention;
  int cmd_intention_flags;
  int dbid;
} keyRequest;

int swapCmdInit();
void swapCmdDeinit();
void copyKeyRequest(keyRequest *dst, keyRequest *src);
void moveKeyRequest(keyRequest *dst, keyRequest *src);
void keyRequestDeinit(keyRequest *key_request);
void getKeyRequests(client *c, struct getKeyRequestsResult *result);
void releaseKeyRequests(struct getKeyRequestsResult *result);
int getKeyRequestsNone(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsGlobal(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);

#define getKeyRequestsHsetnx getKeyRequestsHset
#define getKeyRequestsHget getKeyRequestsHmget
#define getKeyRequestsHdel getKeyRequestsHmget
#define getKeyRequestsHstrlen getKeyRequestsHmget
#define getKeyRequestsHincrby getKeyRequestsHget
#define getKeyRequestsHincrbyfloat getKeyRequestsHmget
#define getKeyRequestsHexists getKeyRequestsHmget
int getKeyRequestsHset(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsHmget(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsHlen(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);

#define MAX_KEYREQUESTS_BUFFER 128
#define GET_KEYREQUESTS_RESULT_INIT { {{0}}, NULL, 0, MAX_KEYREQUESTS_BUFFER, 0}

#define KEYREQUESTS_DBID 0
#define KEYREQUESTS_RESULT_SEQUENTIAL (1<<0)

typedef struct getKeyRequestsResult {
	keyRequest buffer[MAX_KEYREQUESTS_BUFFER];
	keyRequest *key_requests;
	int num;
	int size;
  int flags;
  dict *dupkey; /* ref to server.swap_duplicate_key. */
} getKeyRequestsResult;

void getKeyRequestsPrepareResult(getKeyRequestsResult *result, int numswaps);
void getKeyRequestsAppendResult(getKeyRequestsResult *result, int level, robj *key, int num_subkeys, robj **subkeys, int cmd_intention, int cmd_intention_flags, int dbid);
void getKeyRequestsFreeResult(getKeyRequestsResult *result);

/* --- Data --- */
#define IN        /* Input parameter */
#define OUT       /* Output parameter */
#define INOUT     /* Input/Output parameter */

#define SWAP_NOP    0
#define SWAP_IN     1
#define SWAP_OUT    2
#define SWAP_DEL    3
#define SWAP_TYPES  4

/* --- RDB_KEY_SAVE --- */
#define RDB_KEY_SAVE_NEXT 1
#define RDB_KEY_SAVE_END 0
#define RDB_KEY_SAVE_SKIP -1

static inline const char *swapIntentionName(int intention) {
  const char *name = "?";
  const char *intentions[] = {"NOP", "IN", "OUT", "DEL"};
  if (intention >= 0 && intention < SWAP_TYPES)
    name = intentions[intention];
  return name;
}

/* SwapData represents key state when swap start. It is stable during
 * key swapping, misc dynamic data are save in dataCtx. */
typedef struct swapData {
  struct swapDataType *type;
} swapData;

/* keyRequest: client request parse from command.
 * swapData: key state when swap start.
 * dataCtx: dynamic data when swapping.  */
typedef struct swapDataType {
  char *name;
  int (*swapAna)(struct swapData *data, int cmd_intention, uint32_t cmd_intention_flags, OUT struct keyRequest *key_request, OUT int *intention, OUT uint32_t *intention_flags, OUT void *datactx);
  int (*encodeKeys)(struct swapData *data, int intention, void *datactx, OUT int *action, OUT int *num, OUT sds **rawkeys);
  int (*encodeData)(struct swapData *data, int intention, void *datactx, OUT int *action, OUT int *num, OUT sds **rawkeys, OUT sds **rawvals);
  int (*decodeData)(struct swapData *data, int num, sds *rawkeys, sds *rawvals, OUT robj **decoded);
  int (*swapIn)(struct swapData *data, robj *result, void *datactx);
  int (*swapOut)(struct swapData *data, void *datactx);
  int (*swapDel)(struct swapData *data, void *datactx, int async);
  robj *(*createOrMergeObject)(struct swapData *data, robj *decoded, void *datactx);
  int (*cleanObject)(struct swapData *data, void *datactx);
  void (*free)(struct swapData *data, void *datactx);
} swapDataType;

int swapDataAna(swapData *d, int cmd_intention, uint32_t cmd_intention_flags, struct keyRequest *key_request, int *intention, uint32_t *intention_flag, void *datactx);
int swapDataEncodeKeys(swapData *d, int intention, void *datactx, int *action, int *num, sds **rawkeys);
int swapDataEncodeData(swapData *d, int intention, void *datactx, int *action, int *num, sds **rawkeys, sds **rawvals);
int swapDataDecodeData(swapData *d, int num, sds *rawkeys, sds *rawvals, robj **decoded);
int swapDataSwapIn(swapData *d, robj *result, void *datactx);
int swapDataSwapOut(swapData *d, void *datactx);
int swapDataSwapDel(swapData *d, void *datactx, int async);
robj *swapDataCreateOrMergeObject(swapData *d, robj *decoded, void *datactx);
int swapDataCleanObject(swapData *d, void *datactx);
void swapDataFree(swapData *data, void *datactx);
int dbAddEvictRDBLoad(redisDb* db, sds key, robj* evict);
int rdbLoadStringVerbatim(rio *rdb, sds *verbatim);
int rdbLoadHashFieldsVerbatim(rio *rdb, unsigned long long len, sds *verbatim);

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

/* Swap context */
#define SWAP_ERR_ANA_FAIL -100

struct swapCtx;

typedef void (*clientKeyRequestFinished)(client *c, struct swapCtx *ctx);

typedef struct swapCtx {
  client *c;
  keyRequest key_request[1];
  int key_requests_flags;
  unsigned int expired:1;
  unsigned int set_dirty:1;
  unsigned int reserved:32;
  void *listeners;
  int swap_intention;
  uint32_t swap_intention_flags;
  swapData *data;
  void *datactx;
  clientKeyRequestFinished finished;
  int errcode;
#ifdef SWAP_DEBUG
  swapDebugMsgs msgs;
#endif
} swapCtx;

swapCtx *swapCtxCreate(client *c, keyRequest *key_request, int key_requests_flags, clientKeyRequestFinished finished);
void swapCtxFree(swapCtx *ctx);

struct objectMeta;
swapData *createSwapData(redisDb *db, robj *key, robj *value, robj *evict, struct objectMeta *meta, void **datactx);

/* Whole key */
typedef struct wholeKeySwapData {
  swapData d;
  redisDb *db;
  robj *key;
  robj *value;
  robj *evict;
} wholeKeySwapData;

swapData *createWholeKeySwapData(redisDb *db, robj *key, robj *value, robj *evict, void **datactx);

/* Object */
extern dictType dbMetaDictType;

#define setObjectDirty(o) do { \
    if (o) o->dirty = 1; \
} while(0)

#define setObjectBig(o) do { \
    if (o) o->big = 1; \
} while(0)

typedef struct objectMeta {
  ssize_t len;
  uint64_t version;
} objectMeta;

objectMeta *createObjectMeta(size_t len);
objectMeta *dupObjectMeta(objectMeta *m);
void freeObjectMeta(objectMeta *m);

objectMeta *lookupMeta(redisDb *db, robj *key);
void dbAddMeta(redisDb *db, robj *key, objectMeta *m);
int dbDeleteMeta(redisDb *db, robj *key);

robj *lookupEvictKey(redisDb *db, robj *key);
void dbAddEvict(redisDb *db, robj *key, robj *evict);
int dbDeleteEvict(redisDb *db, robj *key);
void dbSetDirty(redisDb *db, robj *key);
int objectIsDirty(robj *o);
void dbSetBig(redisDb *db, robj *key);
int objectIsBig(robj *o);

/* Big hash */
typedef struct bigHashSwapData {
  swapData d;
  redisDb *db;
  robj *key;
  robj *value;
  robj *evict;
  objectMeta *meta;
} bigHashSwapData;

typedef struct bigHashDataCtx {
  int num;
  robj **subkeys;
  ssize_t meta_len_delta;
  objectMeta *new_meta; /* ref, will be moved to db.meta */
} bigHashDataCtx;

void hashTransformBig(robj *o, objectMeta *m);
swapData *createBigHashSwapData(redisDb *db, robj *key, robj *value, robj *evict, objectMeta *meta, void **pdatactx);

/* --- Exec --- */
#define SWAP_DISPATCH_SEQUENTIAL 0
#define SWAP_DISPATCH_ROUNDROBIN -1

struct swapRequest;

typedef int (*swapRequestNotifyCallback)(struct swapRequest *req, void *pd);
typedef int (*swapRequestFinishedCallback)(swapData *data, void *pd);

typedef struct swapRequest {
  int intention;
  uint32_t intention_flags;
  swapData *data;
  void *datactx;
  robj *result;
  swapRequestNotifyCallback notify_cb;
  void *notify_pd;
  swapRequestFinishedCallback finish_cb;
  void *finish_pd;
  redisAtomic size_t swap_memory;
#ifdef SWAP_DEBUG
  swapDebugMsgs *msgs;
#endif
} swapRequest;

swapRequest *swapRequestNew(int intention, uint32_t intention_flags, swapData *data, void *datactx, swapRequestFinishedCallback cb, void *pd, void *msgs);
void swapRequestFree(swapRequest *req);
int executeSwapRequest(swapRequest *req);
int finishSwapRequest(swapRequest *req);
void submitSwapRequest(int mode, int dispatch_mode, int intention, uint32_t intention_flags, swapData* data, void *datactx, swapRequestFinishedCallback cb, void *pd, void *msgs);

/* --- Threads (encode/rio/decode/finish) --- */
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
void swapThreadsDispatch(int dispatch_mode,swapRequest *req);
int swapThreadsDrained();

/* RIO */
#define ROCKS_GET             	1
#define ROCKS_PUT            	  2
#define ROCKS_DEL              	3
#define ROCKS_WRITE             4
#define ROCKS_MULTIGET          5
#define ROCKS_SCAN              6
#define ROCKS_DELETERANGE       7
#define ROCKS_TYPES             8

static inline const char *rocksActionName(int action) {
  const char *name = "?";
  const char *actions[] = {"NOP", "GET", "PUT", "DEL", "WRITE", "MULTIGET", "SCAN", "DELETERANGE"};
  if (action >= 0 && (size_t)action < sizeof(actions)/sizeof(char*))
    name = actions[action];
  return name;
}

typedef struct RIO {
	int action;
	union {
		struct {
			sds rawkey;
			sds rawval;
		} get;
		struct {
			sds rawkey;
			sds rawval;
		} put;
		struct {
			sds rawkey;
		} del;
		struct {
			rocksdb_writebatch_t *wb;
		} write;
		struct {
			int numkeys;
			sds *rawkeys;
			sds *rawvals;
		} multiget;
		struct {
			sds prefix;
			int numkeys;
			sds *rawkeys;
			sds *rawvals;
		} scan;
		struct {
			sds start_key;
      sds end_key;
		} delete_range;
	};
  sds err;
} RIO;

void RIOInitGet(RIO *rio, sds rawkey);
void RIOInitPut(RIO *rio, sds rawkey, sds rawval);
void RIOInitDel(RIO *rio, sds rawkey);
void RIOInitWrite(RIO *rio, rocksdb_writebatch_t *wb);
void RIOInitMultiGet(RIO *rio, int numkeys, sds *rawkeys);
void RIOInitScan(RIO *rio, sds prefix);
void RIODeinit(RIO *rio);

#define SWAP_MODE_ASYNC 0
#define SWAP_MODE_PARALLEL_SYNC 1

/* --- Async --- */
#define ASYNC_COMPLETE_QUEUE_NOTIFY_READ_MAX  512

typedef struct asyncCompleteQueue {
    int notify_recv_fd;
    int notify_send_fd;
    pthread_mutex_t lock;
    list *complete_queue;
} asyncCompleteQueue;

int asyncCompleteQueueInit();
void asyncCompleteQueueDeinit(asyncCompleteQueue *cq);
int asyncCompleteQueueAppend(asyncCompleteQueue *cq, swapRequest *req);
int asyncCompleteQueueDrain(mstime_t time_limit);

void asyncSwapRequestSubmit(int dispatch_mode, swapRequest *req);

/* --- Parallel sync --- */
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

int parallelSyncSwapRequestSubmit(int dispatch_mode, swapRequest *req);

/* --- Wait --- */
#define DEFAULT_REQUEST_LISTENER_REENTRANT_SIZE 8
#define REQUEST_NOTIFY_ACK  (1<<0)
#define REQUEST_NOTIFY_RLS  (1<<1)

typedef void (*freefunc)(void *);
typedef int (*requestProceed)(void *listeners, redisDb *db, robj *key, client *c, void *pd);

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
  /* # of acked requests for successive requestWait of cur_txid. note that
   * cur_ntxacked is not correct when requestNotify. */
  int cur_ntxacked; 
} requestListeners;

requestListeners *serverRequestListenersCreate(void);
void serverRequestListenersRelease(requestListeners *s);
int requestWaitWouldBlock(int64_t txid, redisDb *db, robj *key);
int requestWait(int64_t txid, redisDb *db, robj *key, requestProceed cb, client *c, void *pd, freefunc pdfree, void *msgs);
int requestAck(void *listeners);
int requestNotify(void *listeners);

/* --- Evict --- */
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
void clientHoldKey(client *c, robj *key, int64_t swap);
void clientUnholdKeys(client *c);
void clientUnholdKey(client *c, robj *key);

/* --- Expire --- */
int submitExpireClientRequest(client *c, robj *key);

/* --- Rocks --- */
#define ROCKS_DIR_MAX_LEN 512
#define ROCKS_DATA "data.rocks"
#define ROCKS_DISK_HEALTH_DETECT_FILE "disk_health_detect"

/* Rocksdb engine */
typedef struct rocks {
    rocksdb_t *db;
    rocksdb_column_family_handle_t *default_cf;
    rocksdb_cache_t *block_cache;
    rocksdb_block_based_table_options_t *block_opts;
    rocksdb_options_t *db_opts;
    rocksdb_readoptions_t *ropts;
    rocksdb_writeoptions_t *wopts;
    const rocksdb_snapshot_t *snapshot;
    rocksdb_checkpoint_t* checkpoint;
    sds checkpoint_dir;
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

/* --- Repl --- */
int submitReplClientRequests(client *c);

/* --- Swap --- */
void swapInit(void);
int dbSwap(client *c);
int clientSwap(client *c);
void continueProcessCommand(client *c);
int replClientSwap(client *c);
int replClientDiscardDispatchedCommands(client *c);
void replClientDiscardSwappingState(client *c);
void submitClientKeyRequests(client *c, getKeyRequestsResult *result, clientKeyRequestFinished cb);
int submitNormalClientRequests(client *c);


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

/* --- Rdb --- */
/* rocks iter thread */
#define ITER_BUFFER_CAPACITY_DEFAULT 4096
#define ITER_NOTIFY_BATCH 32

typedef struct iterResult {
    sds rawkey;
    unsigned char type;
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
    rocksdb_iterator_t *rocksdb_iter;
    rocksdb_t* checkpoint_db;
} rocksIter;

rocksIter *rocksCreateIter(struct rocks *rocks, redisDb *db);
int rocksIterSeekToFirst(rocksIter *it);
int rocksIterNext(rocksIter *it);
void rocksIterKeyTypeValue(rocksIter *it, sds *rawkey, unsigned char *type, sds *rawval);
void rocksReleaseIter(rocksIter *it);
void rocksIterGetError(rocksIter *it, char **error);

/* ctrip rdb load swap data */
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
    sds *rawkeys;
    sds *rawvals;
  } batch;
} ctripRdbLoadCtx;

void evictStartLoading(void);
void evictStopLoading(int success);

/* rdb key type. */ 
#define DEFAULT_HASH_FIELD_SIZE 256

#define RDB_KEY_TYPE_WHOLEKEY 0
#define RDB_KEY_TYPE_BIGHASH 1
#define RDB_KEY_TYPE_MEMKEY 2

typedef struct decodeResult {
    unsigned char enc_type;
    size_t version;
    sds key;
    sds subkey;
    unsigned char rdbtype;
    sds rdbraw;
} decodeResult;

void decodeResultDeinit(decodeResult *decoded);

struct rdbKeyData;
typedef struct rdbKeyType {
    int (*save_start)(struct rdbKeyData *keydata, rio *rdb);
    int (*save)(struct rdbKeyData *keydata, rio *rdb, decodeResult *decoded);
    int (*save_end)(struct rdbKeyData *keydata, int save_result);
    void (*save_deinit)(struct rdbKeyData *keydata);
    int (*load)(struct rdbKeyData *keydata, rio *rdb, OUT sds *rawkey, OUT sds *rawval, OUT int *error);
    int (*load_end)(struct rdbKeyData *keydata,rio *rdb);
    int (*load_dbadd)(struct rdbKeyData *keydata, redisDb *db);
    void (*load_expired)(struct rdbKeyData *keydata);
    int (*load_deinit)(struct rdbKeyData *keydata);
} rdbKeyType;

typedef struct rdbKeyData {
    rdbKeyType *type;
    struct {
        int type;
        robj *value; /* ref: incrRefcount will cause cow */
        robj *evict; /* ref: incrRefcount will cause cow */
        long long expire;
        union {
          struct {
            int nop;
          } wholekey;
          struct {
            objectMeta *meta; /* ref */
            robj *key; /* own */
            int saved;
          } bighash;
        };
    } savectx;
    struct {
        int type;
        int rdbtype;
        sds key; /* ref */
        int nfeeds;
        union {
          struct {
            robj *value; /* moved to db.dict */
          } memkey;
          struct {
            robj *evict; /* moved (to db.evict) */
            int hash_nfields; /* parsed hash nfields from header */
            sds hash_header; /* moved (to rdbLoadSwapData) */
          } wholekey;
          struct {
            int hash_nfields;
            int scanned_fields;
            robj *evict; /* moved (to db.evict) */
            objectMeta *meta; /* moved (to db.meta) */
          } bighash;
        };
    } loadctx;
} rdbKeyData;

/* rdb save */
int rdbSaveRocks(rio *rdb, int *error, redisDb *db, int rdbflags);
void initSwapWholeKey();

int rdbSaveKeyHeader(rio *rdb, robj *key, robj *evict, unsigned char rdbtype, long long expiretime);
void rdbKeyDataInitSaveKey(rdbKeyData *keydata, robj *value, robj *evict, long long expire);
int rdbKeyDataInitSave(rdbKeyData *keydata, redisDb *db, decodeResult *decoded);
void rdbKeyDataDeinitSave(rdbKeyData *keydata);

int rdbKeySaveStart(struct rdbKeyData *keydata, rio *rdb);
int rdbKeySave(struct rdbKeyData *keydata, rio *rdb, decodeResult *d);
int rdbKeySaveEnd(struct rdbKeyData *keydata, int save_result);
/* whole key */
void rdbKeyDataInitSaveWholeKey(rdbKeyData *keydata, robj *value, robj *evict, long long expire);
int wholekeySaveStart(rdbKeyData *keydata, rio *rdb);
int wholekeySave(rdbKeyData *keydata,  rio *rdb, decodeResult *d);
int wholekeySaveEnd(rdbKeyData *keydata, int save_result); 
/* big hash */
void rdbKeyDataInitSaveBigHash(rdbKeyData *keydata, robj *value, robj *evict, objectMeta *meta, long long expire, sds keystr);
int bighashSaveStart(rdbKeyData *keydata, rio *rdb);
int bighashSave(rdbKeyData *keydata,  rio *rdb, decodeResult *d);
int bighashSaveEnd(rdbKeyData *keydata, int save_result); 
void bighashSaveDeinit(rdbKeyData *keydata);

static inline sds rdbVerbatimNew(unsigned char rdbtype) {
    return sdsnewlen(&rdbtype,1);
}
int ctripRdbLoadObject(int rdbtype, rio *rdb, sds key, rdbKeyData *keydata);
robj *rdbKeyLoadGetObject(rdbKeyData *keydata);
int rdbKeyDataInitLoad(rdbKeyData *keydata, rio *rdb, int rdbtype, sds key);
void rdbKeyDataDeinitLoad(rdbKeyData *keydata);
void rdbKeyDataInitLoadKey(rdbKeyData *keydata, int rdbtype, sds key);
int rdbKeyLoadStart(struct rdbKeyData *keydata, rio *rdb, int rdbtype, sds key);
int rdbKeyLoad(struct rdbKeyData *keydata, rio *rdb, sds *rawkey, sds *rawval, int *error);
int rdbKeyLoadEnd(struct rdbKeyData *keydata, rio *rdb);
int rdbKeyLoadDbAdd(struct rdbKeyData *keydata, redisDb *db);
void rdbKeyLoadExpired(struct rdbKeyData *keydata);
/* mem key */
void rdbKeyDataInitLoadMemkey(rdbKeyData *keydata, int rdbtype, sds key);
int memkeyRdbLoad(struct rdbKeyData *keydata, rio *rdb, sds *rawkey, sds *rawval, int *error);
int memkeyRdbLoadDbAdd(struct rdbKeyData *keydata, redisDb *db);
void memkeyRdbLoadExpired(struct rdbKeyData *keydata);
/* whole key */
void rdbKeyDataInitLoadWholeKey(rdbKeyData *keydata, int rdbtype, sds key);
int wholekeyRdbLoad(struct rdbKeyData *keydata, rio *rdb, sds *rawkey, sds *rawval, int *error);
int wholekeyRdbLoadDbAdd(struct rdbKeyData *keydata, redisDb *db);
void wholekeyRdbLoadExpired(struct rdbKeyData *keydata);
/* big hash */
void rdbKeyDataInitLoadBigHash(rdbKeyData *keydata, int rdbtype, sds key);
int bighashRdbLoad(struct rdbKeyData *keydata, rio *rdb, sds *rawkey, sds *rawval, int *error);
int bighashRdbLoadDbAdd(struct rdbKeyData *keydata, redisDb *db);
void bighashRdbLoadExpired(struct rdbKeyData *keydata);


/* --- Util --- */
#define ROCKS_VAL_TYPE_LEN 1

#define ENC_TYPE_STRING       'K'
#define ENC_TYPE_LIST         'L'
#define ENC_TYPE_SET          'S'
#define ENC_TYPE_ZSET         'Z'
#define ENC_TYPE_HASH         'H'
#define ENC_TYPE_MODULE       'M'
#define ENC_TYPE_STREAM       'X'
#define ENC_TYPE_UNKNOWN      '?'

#define ENC_TYPE_STRING_SUB       'k'
#define ENC_TYPE_LIST_SUB         'l'
#define ENC_TYPE_SET_SUB          's'
#define ENC_TYPE_ZSET_SUB         'z'
#define ENC_TYPE_HASH_SUB         'h'
#define ENC_TYPE_MODULE_SUB       'm'
#define ENC_TYPE_STREAM_SUB       'x'
#define ENC_TYPE_UNKNOWN_SUB      '?'

#define isSubkeyEncType(enc_type) ((enc_type) <= 'z' && (enc_type) >= 'a')
unsigned char rocksGetEncType(int obj_type, int big);
int rocksGetObjectType(unsigned char enc_type);
unsigned char rocksGetObjectEncType(robj *o);
sds rocksEncodeKey(unsigned char enc_type, sds key);
sds rocksEncodeSubkey(unsigned char enc_type, uint64_t version, sds key, sds subkey);
sds rocksEncodeValRdb(robj *value);
int rocksDecodeKey(const char *rawkey, size_t rawlen, const char **key, size_t *klen);
int rocksDecodeSubkey(const char *raw, size_t rawlen, uint64_t *version, const char **key, size_t *klen, const char **sub, size_t *slen);
robj *rocksDecodeValRdb(sds raw);
robj *unshareStringValue(robj *value);
size_t objectEstimateSize(robj *o);
size_t keyEstimateSize(redisDb *db, robj *key);
void swapCommand(client *c);
void expiredCommand(client *c);
const char *strObjectType(int type);


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

int swapDataWholeKeyTest(int argc, char **argv, int accurate);
int swapDataBigHashTest(int argc, char **argv, int accurate);
int swapWaitTest(int argc, char **argv, int accurate);
int swapWaitReentrantTest(int argc, char **argv, int accurate);
int swapWaitAckTest(int argc, char **argv, int accurate);
int swapCmdTest(int argc, char **argv, int accurate);
int swapExecTest(int argc, char **argv, int accurate);
int swapRdbTest(int argc, char **argv, int accurate);
int swapObjectTest(int argc, char *argv[], int accurate);

int swapTest(int argc, char **argv, int accurate);

#endif

#endif

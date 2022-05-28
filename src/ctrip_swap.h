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
#include <stdatomic.h>
#include <threads.h>

/* --- Cmd --- */
#define REQUEST_LEVEL_SVR  0
#define REQUEST_LEVEL_DB   1
#define REQUEST_LEVEL_KEY  2

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
} keyRequest;

void getKeyRequests(client *c, struct getKeyRequestsResult *result);
void releaseKeyRequests(struct getKeyRequestsResult *result);
int getKeyRequestsNone(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);
int getKeyRequestsGlobal(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result);

#define MAX_KEYREQUESTS_BUFFER 128
#define GET_KEYREQUESTS_RESULT_INIT { {{0}}, NULL, 0, MAX_KEYREQUESTS_BUFFER }

typedef struct getKeyRequestsResult {
	keyRequest buffer[MAX_KEYREQUESTS_BUFFER];
	keyRequest *key_requests;
	int num;
	int size;
} getKeyRequestsResult;

void getKeyRequestsPrepareResult(getKeyRequestsResult *result, int numswaps);
void getKeyRequestsAppendResult(getKeyRequestsResult *result, int level, robj *key, int num_subkeys, robj **subkeys);
void getKeyRequestsFreeResult(getKeyRequestsResult *result);

/* --- Data --- */
#define SWAP_NOP    0
#define SWAP_IN     1
#define SWAP_OUT    2
#define SWAP_DEL    3
#define SWAP_TYPES  4

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
  int (*swapAna)(struct swapData *data, int cmd_intention, struct keyRequest *key_request, int *intention);
  int (*encodeKeys)(struct swapData *data, int intention, int *action, int *num, sds **rawkeys);
  int (*encodeData)(struct swapData *data, int intention, int *action, int *num, sds **rawkeys, sds **rawvals, void *datactx);
  int (*decodeData)(struct swapData *data, int num, sds *rawkeys, sds *rawvals, robj **decoded);
  int (*swapIn)(struct swapData *data, robj *result, void *datactx);
  int (*swapOut)(struct swapData *data, void *datactx);
  int (*swapDel)(struct swapData *data, void *datactx);
  robj *(*createOrMergeObject)(struct swapData *data, robj *decoded, void *datactx);
  int (*cleanObject)(struct swapData *data, void *datactx);
  void (*free)(struct swapData *data, void *datactx);
} swapDataType;

int swapDataAna(swapData *d, int cmd_intention, struct keyRequest *key_request, int *intention);
int swapDataEncodeKeys(swapData *d, int intention, int *action, int *num, sds **rawkeys);
int swapDataEncodeData(swapData *d, int intention, int *action, int *num, sds **rawkeys, sds **rawvals, void *datactx);
int swapDataDecodeData(swapData *d, int num, sds *rawkeys, sds *rawvals, robj **decoded);
int swapDataSwapIn(swapData *d, robj *result, void *datactx);
int swapDataSwapOut(swapData *d, void *datactx);
int swapDataSwapDel(swapData *d, void *datactx);
robj *swapDataCreateOrMergeObject(swapData *d, robj *decoded, void *datactx);
int swapDataCleanObject(swapData *d, void *datactx);
void swapDataFree(swapData *data, void *datactx);

#define SWAP_ERR_ANA_FAIL -100

struct swapCtx;

typedef void (*clientKeyRequestFinished)(client *c, struct swapCtx *ctx);

typedef struct swapCtx {
  client *c;
  int cmd_intention;
  keyRequest key_request[1];
  void *listeners;
  int swap_intention;
  swapData *data;
  void *datactx;
  clientKeyRequestFinished finished;
  int errcode;

#ifdef SWAP_DEBUG
#define MAX_MSG    64
#define MAX_STEPS  16
  struct {
    char identity[MAX_MSG];
    struct swapCtxStep {
      char name[MAX_MSG];
      char info[MAX_MSG];
    } steps[MAX_STEPS];
    int index;
  } msgs;
#endif

} swapCtx;

swapCtx *swapCtxCreate(client *c, keyRequest *key_request, clientKeyRequestFinished finished);
void swapCtxFree(swapCtx *ctx);

#ifdef SWAP_DEBUG
#ifdef __GNUC__
void swapCtxMsgAppend(swapCtx *ctx, char *step, char *fmt, ...)
  __attribute__((format(printf, 3, 4)));
#else
void swapCtxMsgAppend(swapCtx *ctx, char *step, char *fmt, ...);
#endif
void swapCtxMsgDump(swapCtx *ctx);
#define DEBUG_APPEND(ctx, step, ...) swapCtxMsgAppend(ctx, step, __VA_ARGS__)
#else
#define DEBUG_APPEND(ctx, step, ...)
#endif

swapData *createSwapData(redisDb *db, robj *key, robj *value, robj *evict, void **datactx);

/* Whole key */
typedef struct wholeKeySwapData {
  swapData d;
  redisDb *db;
  robj *key;
  robj *value;
  robj *evict;
} wholeKeySwapData;

swapData *createWholeKeySwapData(redisDb *db, robj *key, robj *value, robj *evict, void **datactx);

/* Big hash */
typedef struct hashSwapData {
  swapData d;
  redisDb *db;
  robj *key;
  robj *value;
  robj *evict;
} hashSwapData;

typedef struct hashDataCtx {
  int num;
  robj **subkeys;
} hashDataCtx;

swapData *createHashSwapData(redisDb *db, sds key);

/* --- Exec --- */
struct swapRequest;

typedef int (*swapRequestNotifyCallback)(struct swapRequest *req, void *pd);
typedef int (*swapRequestFinishedCallback)(swapData *data, void *pd);

typedef struct swapRequest {
  int intention;
  swapData *data;
  void *datactx;
  robj *result;
  swapRequestNotifyCallback notify_cb;
  void *notify_pd;
  swapRequestFinishedCallback finish_cb;
  void *finish_pd;
} swapRequest;

swapRequest *swapRequestNew(int intention, swapData *data, void *datactx, swapRequestFinishedCallback cb, void *pd);
void swapRequestFree(swapRequest *req);
int executeSwapRequest(swapRequest *req);
int finishSwapRequest(swapRequest *req);
void submitSwapRequest(int mode, int intention, swapData* data, void *datactx, swapRequestFinishedCallback cb, void *pd);

/* --- Threads (encode/rio/decode/finish) --- */
#define SWAP_THREADS_DEFAULT     6
#define SWAP_THREADS_MAX         64

typedef struct swapThread {
    int id;
    pthread_t thread_id;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    list *pending_reqs;
    atomic_int is_running_rio;
} swapThread;

int swapThreadsInit();
void swapThreadsDeinit();
void swapThreadsDispatch(swapRequest *req);
int swapThreadsDrained();

/* RIO */
#define ROCKS_GET             	1
#define ROCKS_PUT            	  2
#define ROCKS_DEL              	3
#define ROCKS_WRITE             4
#define ROCKS_MULTIGET          5
#define ROCKS_SCAN              6

static inline const char *rocksActionName(int action) {
  const char *name = "?";
  const char *actions[] = {"NOP", "GET", "PUT", "DEL", "WRITE", "MULTIGET", "SCAN"};
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
			char **rawvals;
			size_t *valens;
		} multiget;
		struct {
			sds prefix;
			int numkeys;
			sds *rawkeys;
			sds *rawvals;
		} scan;
	};
  char *err;
} RIO;

void RIOInitGet(RIO *rio, sds rawkey);
void RIOInitPut(RIO *rio, sds rawkey, sds rawval);
void RIOInitDel(RIO *rio, sds rawkey);
void RIOInitWrite(RIO *rio, rocksdb_writebatch_t *wb);
void RIOInitMultiGet(RIO *rio, int numkeys, sds *rawkeys);
void RIOInitScan(RIO *rio, sds prefix);

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

void asyncSwapRequestSubmit(swapRequest *req);

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

int parallelSyncSwapRequestSubmit(swapRequest *req);

/* --- Wait --- */
typedef void (*freefunc)(void *);
typedef int (*requestProceed)(void *listeners, redisDb *db, robj *key, client *c, void *pd);

typedef struct requestListener {
  redisDb *db;    /* key level request listener might bind on svr/db level */
  robj *key;      /* so we need to save db&key for each requst listener. */
  client *c;
  requestProceed proceed;
  void *pd;
  freefunc pdfree;
} requestListener;

typedef struct requestListeners {
  list *listeners;                  /* list of listenters */
  struct requestListeners *parent;  /* parent lisenter */
  int nlisteners;                   /* # of listeners of current and childs */
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
} requestListeners;

requestListeners *serverRequestListenersCreate(void);
void serverRequestListenersRelease(requestListeners *s);
int requestWouldBlock(redisDb *db, robj *key);
int requestWait(redisDb *db, robj *key, requestProceed cb, client *c, void *pd, freefunc pdfree);
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
void clientUnholdKeys(client *c);
void clientUnholdKey(client *c, robj *key);

/* --- Expire --- */
int dbExpire(redisDb *db, robj *key);

/* --- Rocks --- */
#define ROCKS_DIR_MAX_LEN 512
#define ROCKS_DATA "data.rocks"

/* Rocksdb engine */
typedef struct rocks {
    rocksdb_t *db;
    rocksdb_cache_t *block_cache;
    rocksdb_block_based_table_options_t *block_opts;
    rocksdb_options_t *db_opts;
    rocksdb_readoptions_t *ropts;
    rocksdb_writeoptions_t *wopts;
    const rocksdb_snapshot_t *snapshot;
} rocks;

int rocksInit(void);
void rocksRelease(void);
int rocksFlushAll(void);
void rocksCron(void);
void rocksCreateSnapshot(void);
void rocksUseSnapshot(void);
void rocksReleaseSnapshot(void);
rocksdb_t *rocksGetDb(void);

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

typedef struct swapStat {
    char *name;
    long long started;
    long long finished;
    mstime_t last_start_time;
    mstime_t last_finish_time;
    size_t started_rawkey_bytes;
    size_t started_rawval_bytes;
    size_t finished_rawkey_bytes;
    size_t finished_rawval_bytes;
} swapStat;

void updateStatsSwapStart(int type, sds rawkey, sds rawval);
void updateStatsSwapFinish(int type, sds rawkey, sds rawval);

int swapRateLimitState(void);
int swapRateLimit(client *c);
int swapRateLimited(client *c);

/* --- Rdb --- */
/* save */
#define ITER_BUFFER_CAPACITY_DEFAULT 4096
#define ITER_CACHED_MAX_KEY_LEN 1000
#define ITER_CACHED_MAX_VAL_LEN 4000
#define ITER_NOTIFY_BATCH 32

typedef struct iterResult {
    sds cached_key;
    sds cached_val;
    sds rawkey;
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
    bufferedIterCompleteQueue *buffered_cq;
    rocksdb_iterator_t *rocksdb_iter;
} rocksIter;

rocksIter *rocksCreateIter(struct rocks *rocks, redisDb *db);
int rocksIterSeekToFirst(rocksIter *it);
int rocksIterNext(rocksIter *it);
void rocksIterKeyValue(rocksIter *it, sds *rawkey, sds *rawval);
void rocksReleaseIter(rocksIter *it);
void rocksIterGetError(rocksIter *it, char **error);

int rdbSaveRocks(rio *rdb, redisDb *db, int rdbflags);

/* load */
#define RDB_LOAD_VTYPE_VERBATIM 0
#define RDB_LOAD_VTYPE_OBJECT 1
#define RDB_LOAD_BATCH_COUNT 50
#define RDB_LOAD_BATCH_CAPACITY  (4*1024*1024)

typedef struct ctripRdbLoadCtx {
  size_t errors;
  struct {
    size_t capacity;
    size_t memory;
    size_t count;
    size_t index;
    sds *rawkeys;
    sds *rawvals;
  } batch;
} ctripRdbLoadCtx;

int ctripRdbLoadObject(int rdbtype, rio *rdb, sds key, int *vtype, robj **val, sds *rawval);
int ctripDbAddRDBLoad(int vtype, redisDb* db, sds key, robj* val, sds rawval);

void evictStartLoading(void);
void evictStopLoading(int success);

/* --- Util --- */
sds rocksEncodeKey(int type, sds key);
sds rocksEncodeValRdb(robj *value);
int rocksDecodeKey(const char *rawkey, size_t rawlen, const char **key, size_t *klen);
int getObjectRdbType(robj *o);
robj *rocksDecodeValRdb(int rdbtype, sds raw);
size_t objectComputeSize(robj *o, size_t sample_size);
size_t keyComputeSize(redisDb *db, robj *key);


#ifdef REDIS_TEST
#define yell(str, ...) printf("ERROR! " str "\n\n", __VA_ARGS__)

#define ERROR                                                                  \
    do {                                                                       \
        printf("\tERROR!\n");                                                  \
        err++;                                                                 \
    } while (0)

#define ERR(x, ...)                                                            \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __func__, __LINE__);                   \
        printf("ERROR! " x "\n", __VA_ARGS__);                                 \
        err++;                                                                 \
    } while (0)

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
/* --- initTest ---*/
int initTestRedisServer();
int clearTestRedisDb();
int clearTestRedisServer();
/* --- Data --- */
int swapDataWholeKeyTest(int argc, char **argv, int accurate);
int swapDataTest(int argc, char **argv, int accurate);
int swapWaitTest(int argc, char **argv, int accurate);
int swapCmdTest(int argc, char **argv, int accurate);

int swapTest(int argc, char **argv, int accurate);
#endif

#endif

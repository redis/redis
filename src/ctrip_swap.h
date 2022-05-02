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
#include "ctrip_swap.h"
#include <rocksdb/c.h>

/* --- Rocks rio threads(one-req-one-rsp) --- */
#define RIO_THREADS_DEFAULT     6
#define RIO_THREADS_MAX         64

#define ROCKS_GET             	1
#define ROCKS_PUT            	  2
#define ROCKS_DEL              	3
#define ROCKS_WRITE             4

struct RIO;

typedef void (*RIOFinished)(struct RIO *rio, void *pd);

typedef struct RIO {
    int action; /* GET/PUT/DEL/WRITE */
    union {
      struct {
        sds rawkey;
        sds rawval;
      }kv;
      rocksdb_writebatch_t *wb;
    } req;
    struct {
      struct {
        sds rawkey;
        sds rawval;
      } kv;
      char *err;
    } rsp;
    /* RIO defines pointer to store svr cb&pd here for simplicty, although it
     * should be define in struct point by pd (rio.pd), but hey... */
    struct {
      struct {
        RIOFinished cb;
        void *pd;
      } rio; /* rio thread cb&pd */
      struct {
        voidfuncptr cb;
        void *pd;
      } svr; /* server thread cb&pd */
      void *extra; /* save extra info here */
    };
} RIO;

typedef struct {
    int id;
    pthread_t thread_id;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    list *pending_rios;
} RIOThread;

int rocksInitThreads(struct rocks *rocks);
void rocksDeinitThreads(struct rocks *rocks);
void rocksIOSubmit(uint32_t dist, RIO *req, RIOFinished cb, void *pd);

/* --- Async rocks io --- */
#define ASYNC_COMPLETE_QUEUE_NOTIFY_READ_MAX  512

typedef void (*rocksAsyncCallback)(int action, sds key, sds val, const char *err, void *pd);

typedef struct {
    int notify_recv_fd;
    int notify_send_fd;
    pthread_mutex_t lock;
    list *complete_queue;
} asyncCompleteQueue;

int asyncCompleteQueueInit(struct rocks *rocks);
void asyncCompleteQueueDeinit(asyncCompleteQueue *cq);
int asyncCompleteQueueAppend(asyncCompleteQueue *cq, RIO *rio);
void asyncRIOFinished(RIO *rio, void *pd);
int asyncCompleteQueueDrain(struct rocks *rocks, mstime_t time_limit);
/* Low level rocks async API */
void rocksAsyncSubmit(uint32_t dist, int action, sds key, sds val, rocksAsyncCallback cb, void *pd);
/* Rocks asyc API */
void rocksAsyncPut(uint32_t dist, sds rawkey, sds rawval, rocksAsyncCallback cb, void *pd);
void rocksAsyncGet(uint32_t dist, sds rawkey, rocksAsyncCallback cb, void *pd);
void rocksAsyncDel(uint32_t dist, sds rawkey, rocksAsyncCallback cb, void *pd);

/* --- Parallel swap --- */
#define PARALLEL_SWAP_MODE_CONST 0
#define PARALLEL_SWAP_MODE_ROUND_ROBIN 1
#define PARALLEL_SWAP_MODE_BATCH 2

#define PARALLEL_SWAP_MODE_BATCH_SIZE 32

typedef void (*parallelSwapCallback)(int action, sds rawkey, sds rawval, const char *err, void *pd);

typedef struct {
    int inprogress;         /* swap entry in progress? */
    int pipe_read_fd;       /* read end to wait rio swap finish. */
    int pipe_write_fd;      /* write end to notify swap finish by rio. */
    struct {
      int action;           /* result action */
      sds rawkey;           /* result rawkey */
      sds rawval;           /* result rawval */
      const char *err;      /* result err */
    } rsp;
    parallelSwapCallback cb; /* swap finished callback. */
    void *pd;               /* swap finished private data. */
} swapEntry;

typedef struct parallelSwap {
    list *entries;
    int parallel;
    int mode;
} parallelSwap;

void parallelSwapRIOFinished(RIO *rio, void *pd);
parallelSwap *parallelSwapNew(int parallel, int mode);
void parallelSwapFree(parallelSwap *ps);
int parallelSwapDrain(parallelSwap *ps);
int parallelSwapGet(parallelSwap *ps, sds rawkey, parallelSwapCallback cb, void *pd);
int parallelSwapPut(parallelSwap *ps, sds rawkey, sds rawval, parallelSwapCallback cb, void *pd);
int parallelSwapDel(parallelSwap *ps, sds rawkey, parallelSwapCallback cb, void *pd);
int parallelSwapWrite(parallelSwap *ps, rocksdb_writebatch_t *wb, parallelSwapCallback cb, void *pd);

/* --- Rocks db engine --- */
#define ROCKS_DIR_MAX_LEN 512
#define ROCKS_DATA "data.rocks"

typedef struct rocks {
    /* Engine */
    int rocksdb_epoch;
    rocksdb_t *rocksdb;
    rocksdb_cache_t *block_cache;
    rocksdb_block_based_table_options_t *block_opts;
    rocksdb_options_t *rocksdb_opts;
    rocksdb_readoptions_t *rocksdb_ropts;
    rocksdb_writeoptions_t *rocksdb_wopts;
    const rocksdb_snapshot_t *rocksdb_snapshot;
    /* Rio threads */
    int threads_num;
    RIOThread threads[RIO_THREADS_MAX];
    asyncCompleteQueue CQ;
} rocks;

struct rocks *rocksCreate(void);
void rocksDestroy(struct rocks *rocks);
int rocksDelete(redisDb *db, robj *key);
int rocksFlushAll();
void rocksCron();
void rocksCreateSnapshot(struct rocks *rocks);
void rocksUseSnapshot(struct rocks *rocks);
void rocksReleaseSnapshot(struct rocks *rocks);
rocksdb_t *rocksGetDb(struct rocks *rocks);

/* --- Rocks swap --- */
#define SWAP_NOP    0
#define SWAP_GET    1
#define SWAP_PUT    2
#define SWAP_DEL    3
#define SWAP_TYPES  4

typedef struct swapClient {
    swap s;
    client *c;
} swapClient;

typedef struct swappingClients {
    redisDb *db;    /* db used to index scs */
    robj *key;      /* key used to index scs */
    robj *subkey;   /* subkey used to index scs */
    struct swappingClients *parent; /* parent scs */
    int nchild;     /* # of child scs list */
    list *swapclients; /* list of swapClients */
} swappingClients;

sds swappingClientsDump(swappingClients *scs);

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

void swapInit();
int dbSwap(client *c);
int clientSwap(client *c);
int replClientSwap(client *c);
void continueProcessCommand(client *c);
void evictCommand(client *c);
int getSwapsNone(struct redisCommand *cmd, robj **argv, int argc, getSwapsResult *result);
int getSwapsGlobal(struct redisCommand *cmd, robj **argv, int argc, getSwapsResult *result);
void updateStatsSwapStart(int type, sds rawkey, sds rawval);
void updateStatsSwapFinish(int type, sds rawkey, sds rawval);
int swapsPendingOfType(int type);
size_t objectComputeSize(robj *o, size_t sample_size);
size_t keyComputeSize(redisDb *db, robj *key);
int replClientDiscardDispatchedCommands(client *c);
void replClientDiscardSwappingState(client *c);

#define EVICT_SUCC_SWAPPED      1
#define EVICT_SUCC_FREED        2
#define EVICT_FAIL_ABSENT       -1
#define EVICT_FAIL_EVICTED      -2
#define EVICT_FAIL_SWAPPING     -3
#define EVICT_FAIL_HOLDED       -4
#define EVICT_FAIL_UNSUPPORTED  -5
int dbEvict(redisDb *db, robj *key, int *evict_result);
int dbExpire(redisDb *db, robj *key);
int serverForked();
void dbEvictAsapLater(redisDb *db, robj *key);
int evictAsap();
void debugEvictKeys();

/* Swap rate limit */
#define SWAP_RL_NO      0
#define SWAP_RL_SLOW    1
#define SWAP_RL_STOP    2

int swapRateLimitState();
int swapRateLimit(client *c);
int swapRateLimited(client *c);

/* swap rocks related */
struct swappingClients *lookupSwappingClients(client *c, robj *key, robj *subkey);
void setupSwappingClients(client *c, robj *key, robj *subkey, swappingClients *scs);
void getEvictionSwaps(client *c, robj *key, getSwapsResult *result);
void getExpireSwaps(client *c, robj *key, getSwapsResult *result);
int swapAna(client *c, robj *key, robj *subkey, int *action, char **rawkey, char **rawval, int *cb_type, dataSwapFinishedCallback *cb, void **pd);
void dataSwapFinished(client *c, int action, char *rawkey, char *rawval, int cb_type, dataSwapFinishedCallback cb, void *pd);
void getSwaps(client *c, getSwapsResult *result);
void releaseSwaps(getSwapsResult *result);

/* Whole key swap (string, hash) */
int swapAnaWk(struct redisCommand *cmd, redisDb *db, robj *key, int *action, char **rawkey, char **rawval, dataSwapFinishedCallback *cb, void **pd);
void getDataSwapsWk(robj *key, int mode, getSwapsResult *result);
void setupSwappingClientsWk(redisDb *db, robj *key, void *scs);
void *lookupSwappingClientsWk(redisDb *db, robj *key);
void *getComplementSwapsWk(redisDb *db, robj *key, int mode, int *type, getSwapsResult *result, complementObjectFunc *comp, void **pd);

/* --- rocks iter --- */
#define ITER_BUFFER_CAPACITY_DEFAULT 256
#define ITER_CACHED_MAX_KEY_LEN 1000
#define ITER_CACHED_MAX_VAL_LEN 4000

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

sds rocksEncodeKey(int type, sds key);
int rocksDecodeKey(const char *rawkey, size_t rawlen, const char **key, size_t *klen);

int rdbSaveRocks(rio *rdb, redisDb *db, int rdbflags);

/* --- Rdb swap --- */
/* Raw buffer that could be directly write to rocksdb. */
#define RDB_LOAD_VTYPE_VERBATIM 0
/* Robj that should be encoded before write to rocksdb. */
#define RDB_LOAD_VTYPE_OBJECT 1

/* Max batch keys before write to rocksdb */
#define RDB_LOAD_BATCH_COUNT 50
/* Max batch size before write to rocksb */
#define RDB_LOAD_BATCH_CAPACITY  (4*1024*1024)

typedef struct ctripRdbLoadCtx {
  parallelSwap *ps;
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

void evictStartLoading();
void evictStopLoading(int success);

/* complement swap */
#define COMP_MODE_RDB           0

#define COMP_TYPE_OBJ           0
#define COMP_TYPE_RAW           1

typedef struct {
    int type;
    void *value;
    robj *evict; /* note that evict do not own evict (refcount not incremented
                    to reduce cow), we have to make sure evict exists. */
}compVal;

compVal *compValNew(int type, void *value, robj *evict);
void compValFree(compVal *cv);
compVal *getComplementSwaps(redisDb *db, robj *key, int mode, getSwapsResult *result, complementObjectFunc *comp, void **pd);
int complementObj(robj *dup, sds rawkey, sds rawval, complementObjectFunc comp, void *pd);
int complementCompVal(compVal *cv, sds rawkey, sds rawval, complementObjectFunc comp, void *pd);

#endif

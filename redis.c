/*
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#define REDIS_VERSION "2.1.1"

#include "fmacros.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#include <ucontext.h>
#endif /* HAVE_BACKTRACE */

#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <pthread.h>

#if defined(__sun)
#include "solarisfixes.h"
#endif

#include "redis.h"
#include "ae.h"     /* Event driven programming library */
#include "sds.h"    /* Dynamic safe strings */
#include "anet.h"   /* Networking the easy way */
#include "dict.h"   /* Hash tables */
#include "adlist.h" /* Linked lists */
#include "zmalloc.h" /* total memory usage aware version of malloc/free */
#include "lzf.h"    /* LZF compression library */
#include "pqsort.h" /* Partial qsort for SORT+LIMIT */
#include "zipmap.h" /* Compact dictionary-alike data structure */
#include "sha1.h"   /* SHA1 is used for DEBUG DIGEST */
#include "release.h" /* Release and/or git repository information */

/* Error codes */
#define REDIS_OK                0
#define REDIS_ERR               -1

/* Static server configuration */
#define REDIS_SERVERPORT        6379    /* TCP port */
#define REDIS_MAXIDLETIME       (60*5)  /* default client timeout */
#define REDIS_IOBUF_LEN         1024
#define REDIS_LOADBUF_LEN       1024
#define REDIS_STATIC_ARGS       8
#define REDIS_DEFAULT_DBNUM     16
#define REDIS_CONFIGLINE_MAX    1024
#define REDIS_OBJFREELIST_MAX   1000000 /* Max number of objects to cache */
#define REDIS_MAX_SYNC_TIME     60      /* Slave can't take more to sync */
#define REDIS_EXPIRELOOKUPS_PER_CRON    10 /* lookup 10 expires per loop */
#define REDIS_MAX_WRITE_PER_EVENT (1024*64)
#define REDIS_REQUEST_MAX_SIZE (1024*1024*256) /* max bytes in inline command */

/* If more then REDIS_WRITEV_THRESHOLD write packets are pending use writev */
#define REDIS_WRITEV_THRESHOLD      3
/* Max number of iovecs used for each writev call */
#define REDIS_WRITEV_IOVEC_COUNT    256

/* Hash table parameters */
#define REDIS_HT_MINFILL        10      /* Minimal hash table fill 10% */

/* Command flags */
#define REDIS_CMD_BULK          1       /* Bulk write command */
#define REDIS_CMD_INLINE        2       /* Inline command */
/* REDIS_CMD_DENYOOM reserves a longer comment: all the commands marked with
   this flags will return an error when the 'maxmemory' option is set in the
   config file and the server is using more than maxmemory bytes of memory.
   In short this commands are denied on low memory conditions. */
#define REDIS_CMD_DENYOOM       4
#define REDIS_CMD_FORCE_REPLICATION 8 /* Force replication even if dirty is 0 */

/* Object types */
#define REDIS_STRING 0
#define REDIS_LIST 1
#define REDIS_SET 2
#define REDIS_ZSET 3
#define REDIS_HASH 4

/* Objects encoding. Some kind of objects like Strings and Hashes can be
 * internally represented in multiple ways. The 'encoding' field of the object
 * is set to one of this fields for this object. */
#define REDIS_ENCODING_RAW 0    /* Raw representation */
#define REDIS_ENCODING_INT 1    /* Encoded as integer */
#define REDIS_ENCODING_ZIPMAP 2 /* Encoded as zipmap */
#define REDIS_ENCODING_HT 3     /* Encoded as an hash table */

static char* strencoding[] = {
    "raw", "int", "zipmap", "hashtable"
};

/* Object types only used for dumping to disk */
#define REDIS_EXPIRETIME 253
#define REDIS_SELECTDB 254
#define REDIS_EOF 255

/* Defines related to the dump file format. To store 32 bits lengths for short
 * keys requires a lot of space, so we check the most significant 2 bits of
 * the first byte to interpreter the length:
 *
 * 00|000000 => if the two MSB are 00 the len is the 6 bits of this byte
 * 01|000000 00000000 =>  01, the len is 14 byes, 6 bits + 8 bits of next byte
 * 10|000000 [32 bit integer] => if it's 01, a full 32 bit len will follow
 * 11|000000 this means: specially encoded object will follow. The six bits
 *           number specify the kind of object that follows.
 *           See the REDIS_RDB_ENC_* defines.
 *
 * Lenghts up to 63 are stored using a single byte, most DB keys, and may
 * values, will fit inside. */
#define REDIS_RDB_6BITLEN 0
#define REDIS_RDB_14BITLEN 1
#define REDIS_RDB_32BITLEN 2
#define REDIS_RDB_ENCVAL 3
#define REDIS_RDB_LENERR UINT_MAX

/* When a length of a string object stored on disk has the first two bits
 * set, the remaining two bits specify a special encoding for the object
 * accordingly to the following defines: */
#define REDIS_RDB_ENC_INT8 0        /* 8 bit signed integer */
#define REDIS_RDB_ENC_INT16 1       /* 16 bit signed integer */
#define REDIS_RDB_ENC_INT32 2       /* 32 bit signed integer */
#define REDIS_RDB_ENC_LZF 3         /* string compressed with FASTLZ */

/* Virtual memory object->where field. */
#define REDIS_VM_MEMORY 0       /* The object is on memory */
#define REDIS_VM_SWAPPED 1      /* The object is on disk */
#define REDIS_VM_SWAPPING 2     /* Redis is swapping this object on disk */
#define REDIS_VM_LOADING 3      /* Redis is loading this object from disk */

/* Virtual memory static configuration stuff.
 * Check vmFindContiguousPages() to know more about this magic numbers. */
#define REDIS_VM_MAX_NEAR_PAGES 65536
#define REDIS_VM_MAX_RANDOM_JUMP 4096
#define REDIS_VM_MAX_THREADS 32
#define REDIS_THREAD_STACK_SIZE (1024*1024*4)
/* The following is the *percentage* of completed I/O jobs to process when the
 * handelr is called. While Virtual Memory I/O operations are performed by
 * threads, this operations must be processed by the main thread when completed
 * in order to take effect. */
#define REDIS_MAX_COMPLETED_JOBS_PROCESSED 1

/* Client flags */
#define REDIS_SLAVE 1       /* This client is a slave server */
#define REDIS_MASTER 2      /* This client is a master server */
#define REDIS_MONITOR 4     /* This client is a slave monitor, see MONITOR */
#define REDIS_MULTI 8       /* This client is in a MULTI context */
#define REDIS_BLOCKED 16    /* The client is waiting in a blocking operation */
#define REDIS_IO_WAIT 32    /* The client is waiting for Virtual Memory I/O */
#define REDIS_DIRTY_CAS 64  /* Watched keys modified. EXEC will fail. */

/* Slave replication state - slave side */
#define REDIS_REPL_NONE 0   /* No active replication */
#define REDIS_REPL_CONNECT 1    /* Must connect to master */
#define REDIS_REPL_CONNECTED 2  /* Connected to master */

/* Slave replication state - from the point of view of master
 * Note that in SEND_BULK and ONLINE state the slave receives new updates
 * in its output queue. In the WAIT_BGSAVE state instead the server is waiting
 * to start the next background saving in order to send updates to it. */
#define REDIS_REPL_WAIT_BGSAVE_START 3 /* master waits bgsave to start feeding it */
#define REDIS_REPL_WAIT_BGSAVE_END 4 /* master waits bgsave to start bulk DB transmission */
#define REDIS_REPL_SEND_BULK 5 /* master is sending the bulk DB */
#define REDIS_REPL_ONLINE 6 /* bulk DB already transmitted, receive updates */

/* List related stuff */
#define REDIS_HEAD 0
#define REDIS_TAIL 1

/* Sort operations */
#define REDIS_SORT_GET 0
#define REDIS_SORT_ASC 1
#define REDIS_SORT_DESC 2
#define REDIS_SORTKEY_MAX 1024

/* Log levels */
#define REDIS_DEBUG 0
#define REDIS_VERBOSE 1
#define REDIS_NOTICE 2
#define REDIS_WARNING 3

/* Anti-warning macro... */
#define REDIS_NOTUSED(V) ((void) V)

#define ZSKIPLIST_MAXLEVEL 32 /* Should be enough for 2^32 elements */
#define ZSKIPLIST_P 0.25      /* Skiplist P = 1/4 */

/* Append only defines */
#define APPENDFSYNC_NO 0
#define APPENDFSYNC_ALWAYS 1
#define APPENDFSYNC_EVERYSEC 2

/* Hashes related defaults */
#define REDIS_HASH_MAX_ZIPMAP_ENTRIES 64
#define REDIS_HASH_MAX_ZIPMAP_VALUE 512

/* We can print the stacktrace, so our assert is defined this way: */
#define redisAssert(_e) ((_e)?(void)0 : (_redisAssert(#_e,__FILE__,__LINE__),_exit(1)))
#define redisPanic(_e) _redisPanic(#_e,__FILE__,__LINE__),_exit(1)
static void _redisAssert(char *estr, char *file, int line);
static void _redisPanic(char *msg, char *file, int line);

/*================================= Data types ============================== */

/* A redis object, that is a type able to hold a string / list / set */

/* The VM object structure */
struct redisObjectVM {
    off_t page;         /* the page at witch the object is stored on disk */
    off_t usedpages;    /* number of pages used on disk */
    time_t atime;       /* Last access time */
} vm;

/* The actual Redis Object */
typedef struct redisObject {
    void *ptr;
    unsigned char type;
    unsigned char encoding;
    unsigned char storage;  /* If this object is a key, where is the value?
                             * REDIS_VM_MEMORY, REDIS_VM_SWAPPED, ... */
    unsigned char vtype; /* If this object is a key, and value is swapped out,
                          * this is the type of the swapped out object. */
    int refcount;
    /* VM fields, this are only allocated if VM is active, otherwise the
     * object allocation function will just allocate
     * sizeof(redisObjct) minus sizeof(redisObjectVM), so using
     * Redis without VM active will not have any overhead. */
    struct redisObjectVM vm;
} robj;

/* Macro used to initalize a Redis object allocated on the stack.
 * Note that this macro is taken near the structure definition to make sure
 * we'll update it when the structure is changed, to avoid bugs like
 * bug #85 introduced exactly in this way. */
#define initStaticStringObject(_var,_ptr) do { \
    _var.refcount = 1; \
    _var.type = REDIS_STRING; \
    _var.encoding = REDIS_ENCODING_RAW; \
    _var.ptr = _ptr; \
    if (server.vm_enabled) _var.storage = REDIS_VM_MEMORY; \
} while(0);

typedef struct redisDb {
    dict *dict;                 /* The keyspace for this DB */
    dict *expires;              /* Timeout of keys with a timeout set */
    dict *blocking_keys;        /* Keys with clients waiting for data (BLPOP) */
    dict *io_keys;              /* Keys with clients waiting for VM I/O */
    dict *watched_keys;         /* WATCHED keys for MULTI/EXEC CAS */
    int id;
} redisDb;

/* Client MULTI/EXEC state */
typedef struct multiCmd {
    robj **argv;
    int argc;
    struct redisCommand *cmd;
} multiCmd;

typedef struct multiState {
    multiCmd *commands;     /* Array of MULTI commands */
    int count;              /* Total number of MULTI commands */
} multiState;

/* With multiplexing we need to take per-clinet state.
 * Clients are taken in a liked list. */
typedef struct redisClient {
    int fd;
    redisDb *db;
    int dictid;
    sds querybuf;
    robj **argv, **mbargv;
    int argc, mbargc;
    int bulklen;            /* bulk read len. -1 if not in bulk read mode */
    int multibulk;          /* multi bulk command format active */
    list *reply;
    int sentlen;
    time_t lastinteraction; /* time of the last interaction, used for timeout */
    int flags;              /* REDIS_SLAVE | REDIS_MONITOR | REDIS_MULTI ... */
    int slaveseldb;         /* slave selected db, if this client is a slave */
    int authenticated;      /* when requirepass is non-NULL */
    int replstate;          /* replication state if this is a slave */
    int repldbfd;           /* replication DB file descriptor */
    long repldboff;         /* replication DB file offset */
    off_t repldbsize;       /* replication DB file size */
    multiState mstate;      /* MULTI/EXEC state */
    robj **blocking_keys;   /* The key we are waiting to terminate a blocking
                             * operation such as BLPOP. Otherwise NULL. */
    int blocking_keys_num;  /* Number of blocking keys */
    time_t blockingto;      /* Blocking operation timeout. If UNIX current time
                             * is >= blockingto then the operation timed out. */
    list *io_keys;          /* Keys this client is waiting to be loaded from the
                             * swap file in order to continue. */
    list *watched_keys;     /* Keys WATCHED for MULTI/EXEC CAS */
    dict *pubsub_channels;  /* channels a client is interested in (SUBSCRIBE) */
    list *pubsub_patterns;  /* patterns a client is interested in (SUBSCRIBE) */
} redisClient;

struct saveparam {
    time_t seconds;
    int changes;
};

/* Global server state structure */
struct redisServer {
    int port;
    int fd;
    redisDb *db;
    long long dirty;            /* changes to DB from the last save */
    list *clients;
    list *slaves, *monitors;
    char neterr[ANET_ERR_LEN];
    aeEventLoop *el;
    int cronloops;              /* number of times the cron function run */
    list *objfreelist;          /* A list of freed objects to avoid malloc() */
    time_t lastsave;            /* Unix time of last save succeeede */
    /* Fields used only for stats */
    time_t stat_starttime;         /* server start time */
    long long stat_numcommands;    /* number of processed commands */
    long long stat_numconnections; /* number of connections received */
    long long stat_expiredkeys;   /* number of expired keys */
    /* Configuration */
    int verbosity;
    int glueoutputbuf;
    int maxidletime;
    int dbnum;
    int daemonize;
    int appendonly;
    int appendfsync;
    int shutdown_asap;
    time_t lastfsync;
    int appendfd;
    int appendseldb;
    char *pidfile;
    pid_t bgsavechildpid;
    pid_t bgrewritechildpid;
    sds bgrewritebuf; /* buffer taken by parent during oppend only rewrite */
    sds aofbuf;       /* AOF buffer, written before entering the event loop */
    struct saveparam *saveparams;
    int saveparamslen;
    char *logfile;
    char *bindaddr;
    char *dbfilename;
    char *appendfilename;
    char *requirepass;
    int rdbcompression;
    int activerehashing;
    /* Replication related */
    int isslave;
    char *masterauth;
    char *masterhost;
    int masterport;
    redisClient *master;    /* client that is master for this slave */
    int replstate;
    unsigned int maxclients;
    unsigned long long maxmemory;
    unsigned int blpop_blocked_clients;
    unsigned int vm_blocked_clients;
    /* Sort parameters - qsort_r() is only available under BSD so we
     * have to take this state global, in order to pass it to sortCompare() */
    int sort_desc;
    int sort_alpha;
    int sort_bypattern;
    /* Virtual memory configuration */
    int vm_enabled;
    char *vm_swap_file;
    off_t vm_page_size;
    off_t vm_pages;
    unsigned long long vm_max_memory;
    /* Hashes config */
    size_t hash_max_zipmap_entries;
    size_t hash_max_zipmap_value;
    /* Virtual memory state */
    FILE *vm_fp;
    int vm_fd;
    off_t vm_next_page; /* Next probably empty page */
    off_t vm_near_pages; /* Number of pages allocated sequentially */
    unsigned char *vm_bitmap; /* Bitmap of free/used pages */
    time_t unixtime;    /* Unix time sampled every second. */
    /* Virtual memory I/O threads stuff */
    /* An I/O thread process an element taken from the io_jobs queue and
     * put the result of the operation in the io_done list. While the
     * job is being processed, it's put on io_processing queue. */
    list *io_newjobs; /* List of VM I/O jobs yet to be processed */
    list *io_processing; /* List of VM I/O jobs being processed */
    list *io_processed; /* List of VM I/O jobs already processed */
    list *io_ready_clients; /* Clients ready to be unblocked. All keys loaded */
    pthread_mutex_t io_mutex; /* lock to access io_jobs/io_done/io_thread_job */
    pthread_mutex_t obj_freelist_mutex; /* safe redis objects creation/free */
    pthread_mutex_t io_swapfile_mutex; /* So we can lseek + write */
    pthread_attr_t io_threads_attr; /* attributes for threads creation */
    int io_active_threads; /* Number of running I/O threads */
    int vm_max_threads; /* Max number of I/O threads running at the same time */
    /* Our main thread is blocked on the event loop, locking for sockets ready
     * to be read or written, so when a threaded I/O operation is ready to be
     * processed by the main thread, the I/O thread will use a unix pipe to
     * awake the main thread. The followings are the two pipe FDs. */
    int io_ready_pipe_read;
    int io_ready_pipe_write;
    /* Virtual memory stats */
    unsigned long long vm_stats_used_pages;
    unsigned long long vm_stats_swapped_objects;
    unsigned long long vm_stats_swapouts;
    unsigned long long vm_stats_swapins;
    /* Pubsub */
    dict *pubsub_channels; /* Map channels to list of subscribed clients */
    list *pubsub_patterns; /* A list of pubsub_patterns */
    /* Misc */
    FILE *devnull;
};

typedef struct pubsubPattern {
    redisClient *client;
    robj *pattern;
} pubsubPattern;

typedef void redisCommandProc(redisClient *c);
typedef void redisVmPreloadProc(redisClient *c, struct redisCommand *cmd, int argc, robj **argv);
struct redisCommand {
    char *name;
    redisCommandProc *proc;
    int arity;
    int flags;
    /* Use a function to determine which keys need to be loaded
     * in the background prior to executing this command. Takes precedence
     * over vm_firstkey and others, ignored when NULL */
    redisVmPreloadProc *vm_preload_proc;
    /* What keys should be loaded in background when calling this command? */
    int vm_firstkey; /* The first argument that's a key (0 = no keys) */
    int vm_lastkey;  /* THe last argument that's a key */
    int vm_keystep;  /* The step between first and last key */
};

struct redisFunctionSym {
    char *name;
    unsigned long pointer;
};

typedef struct _redisSortObject {
    robj *obj;
    union {
        double score;
        robj *cmpobj;
    } u;
} redisSortObject;

typedef struct _redisSortOperation {
    int type;
    robj *pattern;
} redisSortOperation;

/* ZSETs use a specialized version of Skiplists */

typedef struct zskiplistNode {
    struct zskiplistNode **forward;
    struct zskiplistNode *backward;
    unsigned int *span;
    double score;
    robj *obj;
} zskiplistNode;

typedef struct zskiplist {
    struct zskiplistNode *header, *tail;
    unsigned long length;
    int level;
} zskiplist;

typedef struct zset {
    dict *dict;
    zskiplist *zsl;
} zset;

/* Our shared "common" objects */

#define REDIS_SHARED_INTEGERS 10000
struct sharedObjectsStruct {
    robj *crlf, *ok, *err, *emptybulk, *czero, *cone, *pong, *space,
    *colon, *nullbulk, *nullmultibulk, *queued,
    *emptymultibulk, *wrongtypeerr, *nokeyerr, *syntaxerr, *sameobjecterr,
    *outofrangeerr, *plus,
    *select0, *select1, *select2, *select3, *select4,
    *select5, *select6, *select7, *select8, *select9,
    *messagebulk, *pmessagebulk, *subscribebulk, *unsubscribebulk, *mbulk3,
    *mbulk4, *psubscribebulk, *punsubscribebulk,
    *integers[REDIS_SHARED_INTEGERS];
} shared;

/* Global vars that are actally used as constants. The following double
 * values are used for double on-disk serialization, and are initialized
 * at runtime to avoid strange compiler optimizations. */

static double R_Zero, R_PosInf, R_NegInf, R_Nan;

/* VM threaded I/O request message */
#define REDIS_IOJOB_LOAD 0          /* Load from disk to memory */
#define REDIS_IOJOB_PREPARE_SWAP 1  /* Compute needed pages */
#define REDIS_IOJOB_DO_SWAP 2       /* Swap from memory to disk */
typedef struct iojob {
    int type;   /* Request type, REDIS_IOJOB_* */
    redisDb *db;/* Redis database */
    robj *key;  /* This I/O request is about swapping this key */
    robj *val;  /* the value to swap for REDIS_IOREQ_*_SWAP, otherwise this
                 * field is populated by the I/O thread for REDIS_IOREQ_LOAD. */
    off_t page; /* Swap page where to read/write the object */
    off_t pages; /* Swap pages needed to save object. PREPARE_SWAP return val */
    int canceled; /* True if this command was canceled by blocking side of VM */
    pthread_t thread; /* ID of the thread processing this entry */
} iojob;

/*================================ Prototypes =============================== */

static void freeStringObject(robj *o);
static void freeListObject(robj *o);
static void freeSetObject(robj *o);
static void decrRefCount(void *o);
static robj *createObject(int type, void *ptr);
static void freeClient(redisClient *c);
static int rdbLoad(char *filename);
static void addReply(redisClient *c, robj *obj);
static void addReplySds(redisClient *c, sds s);
static void incrRefCount(robj *o);
static int rdbSaveBackground(char *filename);
static robj *createStringObject(char *ptr, size_t len);
static robj *dupStringObject(robj *o);
static void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc);
static void replicationFeedMonitors(list *monitors, int dictid, robj **argv, int argc);
static void flushAppendOnlyFile(void);
static void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc);
static int syncWithMaster(void);
static robj *tryObjectEncoding(robj *o);
static robj *getDecodedObject(robj *o);
static int removeExpire(redisDb *db, robj *key);
static int expireIfNeeded(redisDb *db, robj *key);
static int deleteIfVolatile(redisDb *db, robj *key);
static int deleteIfSwapped(redisDb *db, robj *key);
static int deleteKey(redisDb *db, robj *key);
static time_t getExpire(redisDb *db, robj *key);
static int setExpire(redisDb *db, robj *key, time_t when);
static void updateSlavesWaitingBgsave(int bgsaveerr);
static void freeMemoryIfNeeded(void);
static int processCommand(redisClient *c);
static void setupSigSegvAction(void);
static void rdbRemoveTempFile(pid_t childpid);
static void aofRemoveTempFile(pid_t childpid);
static size_t stringObjectLen(robj *o);
static void processInputBuffer(redisClient *c);
static zskiplist *zslCreate(void);
static void zslFree(zskiplist *zsl);
static void zslInsert(zskiplist *zsl, double score, robj *obj);
static void sendReplyToClientWritev(aeEventLoop *el, int fd, void *privdata, int mask);
static void initClientMultiState(redisClient *c);
static void freeClientMultiState(redisClient *c);
static void queueMultiCommand(redisClient *c, struct redisCommand *cmd);
static void unblockClientWaitingData(redisClient *c);
static int handleClientsWaitingListPush(redisClient *c, robj *key, robj *ele);
static void vmInit(void);
static void vmMarkPagesFree(off_t page, off_t count);
static robj *vmLoadObject(robj *key);
static robj *vmPreviewObject(robj *key);
static int vmSwapOneObjectBlocking(void);
static int vmSwapOneObjectThreaded(void);
static int vmCanSwapOut(void);
static int tryFreeOneObjectFromFreelist(void);
static void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
static void vmThreadedIOCompletedJob(aeEventLoop *el, int fd, void *privdata, int mask);
static void vmCancelThreadedIOJob(robj *o);
static void lockThreadedIO(void);
static void unlockThreadedIO(void);
static int vmSwapObjectThreaded(robj *key, robj *val, redisDb *db);
static void freeIOJob(iojob *j);
static void queueIOJob(iojob *j);
static int vmWriteObjectOnSwap(robj *o, off_t page);
static robj *vmReadObjectFromSwap(off_t page, int type);
static void waitEmptyIOJobsQueue(void);
static void vmReopenSwapFile(void);
static int vmFreePage(off_t page);
static void zunionInterBlockClientOnSwappedKeys(redisClient *c, struct redisCommand *cmd, int argc, robj **argv);
static void execBlockClientOnSwappedKeys(redisClient *c, struct redisCommand *cmd, int argc, robj **argv);
static int blockClientOnSwappedKeys(redisClient *c, struct redisCommand *cmd);
static int dontWaitForSwappedKey(redisClient *c, robj *key);
static void handleClientsBlockedOnSwappedKey(redisDb *db, robj *key);
static void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask);
static struct redisCommand *lookupCommand(char *name);
static void call(redisClient *c, struct redisCommand *cmd);
static void resetClient(redisClient *c);
static void convertToRealHash(robj *o);
static int pubsubUnsubscribeAllChannels(redisClient *c, int notify);
static int pubsubUnsubscribeAllPatterns(redisClient *c, int notify);
static void freePubsubPattern(void *p);
static int listMatchPubsubPattern(void *a, void *b);
static int compareStringObjects(robj *a, robj *b);
static int equalStringObjects(robj *a, robj *b);
static void usage();
static int rewriteAppendOnlyFileBackground(void);
static int vmSwapObjectBlocking(robj *key, robj *val);
static int prepareForShutdown();
static void touchWatchedKey(redisDb *db, robj *key);
static void touchWatchedKeysOnFlush(int dbid);
static void unwatchAllKeys(redisClient *c);

static void authCommand(redisClient *c);
static void pingCommand(redisClient *c);
static void echoCommand(redisClient *c);
static void setCommand(redisClient *c);
static void setnxCommand(redisClient *c);
static void setexCommand(redisClient *c);
static void getCommand(redisClient *c);
static void delCommand(redisClient *c);
static void existsCommand(redisClient *c);
static void incrCommand(redisClient *c);
static void decrCommand(redisClient *c);
static void incrbyCommand(redisClient *c);
static void decrbyCommand(redisClient *c);
static void selectCommand(redisClient *c);
static void randomkeyCommand(redisClient *c);
static void keysCommand(redisClient *c);
static void dbsizeCommand(redisClient *c);
static void lastsaveCommand(redisClient *c);
static void saveCommand(redisClient *c);
static void bgsaveCommand(redisClient *c);
static void bgrewriteaofCommand(redisClient *c);
static void shutdownCommand(redisClient *c);
static void moveCommand(redisClient *c);
static void renameCommand(redisClient *c);
static void renamenxCommand(redisClient *c);
static void lpushCommand(redisClient *c);
static void rpushCommand(redisClient *c);
static void lpopCommand(redisClient *c);
static void rpopCommand(redisClient *c);
static void llenCommand(redisClient *c);
static void lindexCommand(redisClient *c);
static void lrangeCommand(redisClient *c);
static void ltrimCommand(redisClient *c);
static void typeCommand(redisClient *c);
static void lsetCommand(redisClient *c);
static void saddCommand(redisClient *c);
static void sremCommand(redisClient *c);
static void smoveCommand(redisClient *c);
static void sismemberCommand(redisClient *c);
static void scardCommand(redisClient *c);
static void spopCommand(redisClient *c);
static void srandmemberCommand(redisClient *c);
static void sinterCommand(redisClient *c);
static void sinterstoreCommand(redisClient *c);
static void sunionCommand(redisClient *c);
static void sunionstoreCommand(redisClient *c);
static void sdiffCommand(redisClient *c);
static void sdiffstoreCommand(redisClient *c);
static void syncCommand(redisClient *c);
static void flushdbCommand(redisClient *c);
static void flushallCommand(redisClient *c);
static void sortCommand(redisClient *c);
static void lremCommand(redisClient *c);
static void rpoplpushcommand(redisClient *c);
static void infoCommand(redisClient *c);
static void mgetCommand(redisClient *c);
static void monitorCommand(redisClient *c);
static void expireCommand(redisClient *c);
static void expireatCommand(redisClient *c);
static void getsetCommand(redisClient *c);
static void ttlCommand(redisClient *c);
static void slaveofCommand(redisClient *c);
static void debugCommand(redisClient *c);
static void msetCommand(redisClient *c);
static void msetnxCommand(redisClient *c);
static void zaddCommand(redisClient *c);
static void zincrbyCommand(redisClient *c);
static void zrangeCommand(redisClient *c);
static void zrangebyscoreCommand(redisClient *c);
static void zcountCommand(redisClient *c);
static void zrevrangeCommand(redisClient *c);
static void zcardCommand(redisClient *c);
static void zremCommand(redisClient *c);
static void zscoreCommand(redisClient *c);
static void zremrangebyscoreCommand(redisClient *c);
static void multiCommand(redisClient *c);
static void execCommand(redisClient *c);
static void discardCommand(redisClient *c);
static void blpopCommand(redisClient *c);
static void brpopCommand(redisClient *c);
static void appendCommand(redisClient *c);
static void substrCommand(redisClient *c);
static void zrankCommand(redisClient *c);
static void zrevrankCommand(redisClient *c);
static void hsetCommand(redisClient *c);
static void hsetnxCommand(redisClient *c);
static void hgetCommand(redisClient *c);
static void hmsetCommand(redisClient *c);
static void hmgetCommand(redisClient *c);
static void hdelCommand(redisClient *c);
static void hlenCommand(redisClient *c);
static void zremrangebyrankCommand(redisClient *c);
static void zunionstoreCommand(redisClient *c);
static void zinterstoreCommand(redisClient *c);
static void hkeysCommand(redisClient *c);
static void hvalsCommand(redisClient *c);
static void hgetallCommand(redisClient *c);
static void hexistsCommand(redisClient *c);
static void configCommand(redisClient *c);
static void hincrbyCommand(redisClient *c);
static void subscribeCommand(redisClient *c);
static void unsubscribeCommand(redisClient *c);
static void psubscribeCommand(redisClient *c);
static void punsubscribeCommand(redisClient *c);
static void publishCommand(redisClient *c);
static void watchCommand(redisClient *c);
static void unwatchCommand(redisClient *c);

/*================================= Globals ================================= */

/* Global vars */
static struct redisServer server; /* server global state */
static struct redisCommand cmdTable[] = {
    {"get",getCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"set",setCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,0,0,0},
    {"setnx",setnxCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,0,0,0},
    {"setex",setexCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,0,0,0},
    {"append",appendCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"substr",substrCommand,4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"del",delCommand,-2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"exists",existsCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"incr",incrCommand,2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"decr",decrCommand,2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"mget",mgetCommand,-2,REDIS_CMD_INLINE,NULL,1,-1,1},
    {"rpush",rpushCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"lpush",lpushCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"rpop",rpopCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"lpop",lpopCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"brpop",brpopCommand,-3,REDIS_CMD_INLINE,NULL,1,1,1},
    {"blpop",blpopCommand,-3,REDIS_CMD_INLINE,NULL,1,1,1},
    {"llen",llenCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"lindex",lindexCommand,3,REDIS_CMD_INLINE,NULL,1,1,1},
    {"lset",lsetCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"lrange",lrangeCommand,4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"ltrim",ltrimCommand,4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"lrem",lremCommand,4,REDIS_CMD_BULK,NULL,1,1,1},
    {"rpoplpush",rpoplpushcommand,3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,2,1},
    {"sadd",saddCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"srem",sremCommand,3,REDIS_CMD_BULK,NULL,1,1,1},
    {"smove",smoveCommand,4,REDIS_CMD_BULK,NULL,1,2,1},
    {"sismember",sismemberCommand,3,REDIS_CMD_BULK,NULL,1,1,1},
    {"scard",scardCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"spop",spopCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"srandmember",srandmemberCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"sinter",sinterCommand,-2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,-1,1},
    {"sinterstore",sinterstoreCommand,-3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,2,-1,1},
    {"sunion",sunionCommand,-2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,-1,1},
    {"sunionstore",sunionstoreCommand,-3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,2,-1,1},
    {"sdiff",sdiffCommand,-2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,-1,1},
    {"sdiffstore",sdiffstoreCommand,-3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,2,-1,1},
    {"smembers",sinterCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"zadd",zaddCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"zincrby",zincrbyCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"zrem",zremCommand,3,REDIS_CMD_BULK,NULL,1,1,1},
    {"zremrangebyscore",zremrangebyscoreCommand,4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"zremrangebyrank",zremrangebyrankCommand,4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"zunionstore",zunionstoreCommand,-4,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,zunionInterBlockClientOnSwappedKeys,0,0,0},
    {"zinterstore",zinterstoreCommand,-4,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,zunionInterBlockClientOnSwappedKeys,0,0,0},
    {"zrange",zrangeCommand,-4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"zrangebyscore",zrangebyscoreCommand,-4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"zcount",zcountCommand,4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"zrevrange",zrevrangeCommand,-4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"zcard",zcardCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"zscore",zscoreCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"zrank",zrankCommand,3,REDIS_CMD_BULK,NULL,1,1,1},
    {"zrevrank",zrevrankCommand,3,REDIS_CMD_BULK,NULL,1,1,1},
    {"hset",hsetCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"hsetnx",hsetnxCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"hget",hgetCommand,3,REDIS_CMD_BULK,NULL,1,1,1},
    {"hmset",hmsetCommand,-4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"hmget",hmgetCommand,-3,REDIS_CMD_BULK,NULL,1,1,1},
    {"hincrby",hincrbyCommand,4,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"hdel",hdelCommand,3,REDIS_CMD_BULK,NULL,1,1,1},
    {"hlen",hlenCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"hkeys",hkeysCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"hvals",hvalsCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"hgetall",hgetallCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"hexists",hexistsCommand,3,REDIS_CMD_BULK,NULL,1,1,1},
    {"incrby",incrbyCommand,3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"decrby",decrbyCommand,3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"getset",getsetCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"mset",msetCommand,-3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,-1,2},
    {"msetnx",msetnxCommand,-3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,-1,2},
    {"randomkey",randomkeyCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"select",selectCommand,2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"move",moveCommand,3,REDIS_CMD_INLINE,NULL,1,1,1},
    {"rename",renameCommand,3,REDIS_CMD_INLINE,NULL,1,1,1},
    {"renamenx",renamenxCommand,3,REDIS_CMD_INLINE,NULL,1,1,1},
    {"expire",expireCommand,3,REDIS_CMD_INLINE,NULL,0,0,0},
    {"expireat",expireatCommand,3,REDIS_CMD_INLINE,NULL,0,0,0},
    {"keys",keysCommand,2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"dbsize",dbsizeCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"auth",authCommand,2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"ping",pingCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"echo",echoCommand,2,REDIS_CMD_BULK,NULL,0,0,0},
    {"save",saveCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"bgsave",bgsaveCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"bgrewriteaof",bgrewriteaofCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"shutdown",shutdownCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"lastsave",lastsaveCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"type",typeCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"multi",multiCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"exec",execCommand,1,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,execBlockClientOnSwappedKeys,0,0,0},
    {"discard",discardCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"sync",syncCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"flushdb",flushdbCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"flushall",flushallCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"sort",sortCommand,-2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"info",infoCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"monitor",monitorCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"ttl",ttlCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"slaveof",slaveofCommand,3,REDIS_CMD_INLINE,NULL,0,0,0},
    {"debug",debugCommand,-2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"config",configCommand,-2,REDIS_CMD_BULK,NULL,0,0,0},
    {"subscribe",subscribeCommand,-2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"unsubscribe",unsubscribeCommand,-1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"psubscribe",psubscribeCommand,-2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"punsubscribe",punsubscribeCommand,-1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"publish",publishCommand,3,REDIS_CMD_BULK|REDIS_CMD_FORCE_REPLICATION,NULL,0,0,0},
    {"watch",watchCommand,-2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"unwatch",unwatchCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {NULL,NULL,0,0,NULL,0,0,0}
};

/*============================ Utility functions ============================ */

/* Glob-style pattern matching. */
static int stringmatchlen(const char *pattern, int patternLen,
        const char *string, int stringLen, int nocase)
{
    while(patternLen) {
        switch(pattern[0]) {
        case '*':
            while (pattern[1] == '*') {
                pattern++;
                patternLen--;
            }
            if (patternLen == 1)
                return 1; /* match */
            while(stringLen) {
                if (stringmatchlen(pattern+1, patternLen-1,
                            string, stringLen, nocase))
                    return 1; /* match */
                string++;
                stringLen--;
            }
            return 0; /* no match */
            break;
        case '?':
            if (stringLen == 0)
                return 0; /* no match */
            string++;
            stringLen--;
            break;
        case '[':
        {
            int not, match;

            pattern++;
            patternLen--;
            not = pattern[0] == '^';
            if (not) {
                pattern++;
                patternLen--;
            }
            match = 0;
            while(1) {
                if (pattern[0] == '\\') {
                    pattern++;
                    patternLen--;
                    if (pattern[0] == string[0])
                        match = 1;
                } else if (pattern[0] == ']') {
                    break;
                } else if (patternLen == 0) {
                    pattern--;
                    patternLen++;
                    break;
                } else if (pattern[1] == '-' && patternLen >= 3) {
                    int start = pattern[0];
                    int end = pattern[2];
                    int c = string[0];
                    if (start > end) {
                        int t = start;
                        start = end;
                        end = t;
                    }
                    if (nocase) {
                        start = tolower(start);
                        end = tolower(end);
                        c = tolower(c);
                    }
                    pattern += 2;
                    patternLen -= 2;
                    if (c >= start && c <= end)
                        match = 1;
                } else {
                    if (!nocase) {
                        if (pattern[0] == string[0])
                            match = 1;
                    } else {
                        if (tolower((int)pattern[0]) == tolower((int)string[0]))
                            match = 1;
                    }
                }
                pattern++;
                patternLen--;
            }
            if (not)
                match = !match;
            if (!match)
                return 0; /* no match */
            string++;
            stringLen--;
            break;
        }
        case '\\':
            if (patternLen >= 2) {
                pattern++;
                patternLen--;
            }
            /* fall through */
        default:
            if (!nocase) {
                if (pattern[0] != string[0])
                    return 0; /* no match */
            } else {
                if (tolower((int)pattern[0]) != tolower((int)string[0]))
                    return 0; /* no match */
            }
            string++;
            stringLen--;
            break;
        }
        pattern++;
        patternLen--;
        if (stringLen == 0) {
            while(*pattern == '*') {
                pattern++;
                patternLen--;
            }
            break;
        }
    }
    if (patternLen == 0 && stringLen == 0)
        return 1;
    return 0;
}

static int stringmatch(const char *pattern, const char *string, int nocase) {
    return stringmatchlen(pattern,strlen(pattern),string,strlen(string),nocase);
}

/* Convert a string representing an amount of memory into the number of
 * bytes, so for instance memtoll("1Gi") will return 1073741824 that is
 * (1024*1024*1024).
 *
 * On parsing error, if *err is not NULL, it's set to 1, otherwise it's
 * set to 0 */
static long long memtoll(const char *p, int *err) {
    const char *u;
    char buf[128];
    long mul; /* unit multiplier */
    long long val;
    unsigned int digits;

    if (err) *err = 0;
    /* Search the first non digit character. */
    u = p;
    if (*u == '-') u++;
    while(*u && isdigit(*u)) u++;
    if (*u == '\0' || !strcasecmp(u,"b")) {
        mul = 1;
    } else if (!strcasecmp(u,"k")) {
        mul = 1000;
    } else if (!strcasecmp(u,"kb")) {
        mul = 1024;
    } else if (!strcasecmp(u,"m")) {
        mul = 1000*1000;
    } else if (!strcasecmp(u,"mb")) {
        mul = 1024*1024;
    } else if (!strcasecmp(u,"g")) {
        mul = 1000L*1000*1000;
    } else if (!strcasecmp(u,"gb")) {
        mul = 1024L*1024*1024;
    } else {
        if (err) *err = 1;
        mul = 1;
    }
    digits = u-p;
    if (digits >= sizeof(buf)) {
        if (err) *err = 1;
        return LLONG_MAX;
    }
    memcpy(buf,p,digits);
    buf[digits] = '\0';
    val = strtoll(buf,NULL,10);
    return val*mul;
}

/* Convert a long long into a string. Returns the number of
 * characters needed to represent the number, that can be shorter if passed
 * buffer length is not enough to store the whole number. */
static int ll2string(char *s, size_t len, long long value) {
    char buf[32], *p;
    unsigned long long v;
    size_t l;

    if (len == 0) return 0;
    v = (value < 0) ? -value : value;
    p = buf+31; /* point to the last character */
    do {
        *p-- = '0'+(v%10);
        v /= 10;
    } while(v);
    if (value < 0) *p-- = '-';
    p++;
    l = 32-(p-buf);
    if (l+1 > len) l = len-1; /* Make sure it fits, including the nul term */
    memcpy(s,p,l);
    s[l] = '\0';
    return l;
}

static void redisLog(int level, const char *fmt, ...) {
    va_list ap;
    FILE *fp;

    fp = (server.logfile == NULL) ? stdout : fopen(server.logfile,"a");
    if (!fp) return;

    va_start(ap, fmt);
    if (level >= server.verbosity) {
        char *c = ".-*#";
        char buf[64];
        time_t now;

        now = time(NULL);
        strftime(buf,64,"%d %b %H:%M:%S",localtime(&now));
        fprintf(fp,"[%d] %s %c ",(int)getpid(),buf,c[level]);
        vfprintf(fp, fmt, ap);
        fprintf(fp,"\n");
        fflush(fp);
    }
    va_end(ap);

    if (server.logfile) fclose(fp);
}

/*====================== Hash table type implementation  ==================== */

/* This is an hash table type that uses the SDS dynamic strings libary as
 * keys and radis objects as values (objects can hold SDS strings,
 * lists, sets). */

static void dictVanillaFree(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    zfree(val);
}

static void dictListDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    listRelease((list*)val);
}

static int sdsDictKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    int l1,l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

static void dictRedisObjectDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    if (val == NULL) return; /* Values of swapped out keys as set to NULL */
    decrRefCount(val);
}

static int dictObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    const robj *o1 = key1, *o2 = key2;
    return sdsDictKeyCompare(privdata,o1->ptr,o2->ptr);
}

static unsigned int dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}

static int dictEncObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    robj *o1 = (robj*) key1, *o2 = (robj*) key2;
    int cmp;

    if (o1->encoding == REDIS_ENCODING_INT &&
        o2->encoding == REDIS_ENCODING_INT)
            return o1->ptr == o2->ptr;

    o1 = getDecodedObject(o1);
    o2 = getDecodedObject(o2);
    cmp = sdsDictKeyCompare(privdata,o1->ptr,o2->ptr);
    decrRefCount(o1);
    decrRefCount(o2);
    return cmp;
}

static unsigned int dictEncObjHash(const void *key) {
    robj *o = (robj*) key;

    if (o->encoding == REDIS_ENCODING_RAW) {
        return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
    } else {
        if (o->encoding == REDIS_ENCODING_INT) {
            char buf[32];
            int len;

            len = ll2string(buf,32,(long)o->ptr);
            return dictGenHashFunction((unsigned char*)buf, len);
        } else {
            unsigned int hash;

            o = getDecodedObject(o);
            hash = dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
            decrRefCount(o);
            return hash;
        }
    }
}

/* Sets type and expires */
static dictType setDictType = {
    dictEncObjHash,            /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictEncObjKeyCompare,      /* key compare */
    dictRedisObjectDestructor, /* key destructor */
    NULL                       /* val destructor */
};

/* Sorted sets hash (note: a skiplist is used in addition to the hash table) */
static dictType zsetDictType = {
    dictEncObjHash,            /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictEncObjKeyCompare,      /* key compare */
    dictRedisObjectDestructor, /* key destructor */
    dictVanillaFree            /* val destructor of malloc(sizeof(double)) */
};

/* Db->dict */
static dictType dbDictType = {
    dictObjHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictObjKeyCompare,          /* key compare */
    dictRedisObjectDestructor,  /* key destructor */
    dictRedisObjectDestructor   /* val destructor */
};

/* Db->expires */
static dictType keyptrDictType = {
    dictObjHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictObjKeyCompare,         /* key compare */
    dictRedisObjectDestructor, /* key destructor */
    NULL                       /* val destructor */
};

/* Hash type hash table (note that small hashes are represented with zimpaps) */
static dictType hashDictType = {
    dictEncObjHash,             /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictEncObjKeyCompare,       /* key compare */
    dictRedisObjectDestructor,  /* key destructor */
    dictRedisObjectDestructor   /* val destructor */
};

/* Keylist hash table type has unencoded redis objects as keys and
 * lists as values. It's used for blocking operations (BLPOP) and to
 * map swapped keys to a list of clients waiting for this keys to be loaded. */
static dictType keylistDictType = {
    dictObjHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictObjKeyCompare,          /* key compare */
    dictRedisObjectDestructor,  /* key destructor */
    dictListDestructor          /* val destructor */
};

static void version();

/* ========================= Random utility functions ======================= */

/* Redis generally does not try to recover from out of memory conditions
 * when allocating objects or strings, it is not clear if it will be possible
 * to report this condition to the client since the networking layer itself
 * is based on heap allocation for send buffers, so we simply abort.
 * At least the code will be simpler to read... */
static void oom(const char *msg) {
    redisLog(REDIS_WARNING, "%s: Out of memory\n",msg);
    sleep(1);
    abort();
}

/* ====================== Redis server networking stuff ===================== */
static void closeTimedoutClients(void) {
    redisClient *c;
    listNode *ln;
    time_t now = time(NULL);
    listIter li;

    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        c = listNodeValue(ln);
        if (server.maxidletime &&
            !(c->flags & REDIS_SLAVE) &&    /* no timeout for slaves */
            !(c->flags & REDIS_MASTER) &&   /* no timeout for masters */
            dictSize(c->pubsub_channels) == 0 && /* no timeout for pubsub */
            listLength(c->pubsub_patterns) == 0 &&
            (now - c->lastinteraction > server.maxidletime))
        {
            redisLog(REDIS_VERBOSE,"Closing idle client");
            freeClient(c);
        } else if (c->flags & REDIS_BLOCKED) {
            if (c->blockingto != 0 && c->blockingto < now) {
                addReply(c,shared.nullmultibulk);
                unblockClientWaitingData(c);
            }
        }
    }
}

static int htNeedsResize(dict *dict) {
    long long size, used;

    size = dictSlots(dict);
    used = dictSize(dict);
    return (size && used && size > DICT_HT_INITIAL_SIZE &&
            (used*100/size < REDIS_HT_MINFILL));
}

/* If the percentage of used slots in the HT reaches REDIS_HT_MINFILL
 * we resize the hash table to save memory */
static void tryResizeHashTables(void) {
    int j;

    for (j = 0; j < server.dbnum; j++) {
        if (htNeedsResize(server.db[j].dict))
            dictResize(server.db[j].dict);
        if (htNeedsResize(server.db[j].expires))
            dictResize(server.db[j].expires);
    }
}

/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use 1 millisecond
 * of CPU time at every serverCron() loop in order to rehash some key. */
static void incrementallyRehash(void) {
    int j;

    for (j = 0; j < server.dbnum; j++) {
        if (dictIsRehashing(server.db[j].dict)) {
            dictRehashMilliseconds(server.db[j].dict,1);
            break; /* already used our millisecond for this loop... */
        }
    }
}

/* A background saving child (BGSAVE) terminated its work. Handle this. */
void backgroundSaveDoneHandler(int statloc) {
    int exitcode = WEXITSTATUS(statloc);
    int bysignal = WIFSIGNALED(statloc);

    if (!bysignal && exitcode == 0) {
        redisLog(REDIS_NOTICE,
            "Background saving terminated with success");
        server.dirty = 0;
        server.lastsave = time(NULL);
    } else if (!bysignal && exitcode != 0) {
        redisLog(REDIS_WARNING, "Background saving error");
    } else {
        redisLog(REDIS_WARNING,
            "Background saving terminated by signal %d", WTERMSIG(statloc));
        rdbRemoveTempFile(server.bgsavechildpid);
    }
    server.bgsavechildpid = -1;
    /* Possibly there are slaves waiting for a BGSAVE in order to be served
     * (the first stage of SYNC is a bulk transfer of dump.rdb) */
    updateSlavesWaitingBgsave(exitcode == 0 ? REDIS_OK : REDIS_ERR);
}

/* A background append only file rewriting (BGREWRITEAOF) terminated its work.
 * Handle this. */
void backgroundRewriteDoneHandler(int statloc) {
    int exitcode = WEXITSTATUS(statloc);
    int bysignal = WIFSIGNALED(statloc);

    if (!bysignal && exitcode == 0) {
        int fd;
        char tmpfile[256];

        redisLog(REDIS_NOTICE,
            "Background append only file rewriting terminated with success");
        /* Now it's time to flush the differences accumulated by the parent */
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) server.bgrewritechildpid);
        fd = open(tmpfile,O_WRONLY|O_APPEND);
        if (fd == -1) {
            redisLog(REDIS_WARNING, "Not able to open the temp append only file produced by the child: %s", strerror(errno));
            goto cleanup;
        }
        /* Flush our data... */
        if (write(fd,server.bgrewritebuf,sdslen(server.bgrewritebuf)) !=
                (signed) sdslen(server.bgrewritebuf)) {
            redisLog(REDIS_WARNING, "Error or short write trying to flush the parent diff of the append log file in the child temp file: %s", strerror(errno));
            close(fd);
            goto cleanup;
        }
        redisLog(REDIS_NOTICE,"Parent diff flushed into the new append log file with success (%lu bytes)",sdslen(server.bgrewritebuf));
        /* Now our work is to rename the temp file into the stable file. And
         * switch the file descriptor used by the server for append only. */
        if (rename(tmpfile,server.appendfilename) == -1) {
            redisLog(REDIS_WARNING,"Can't rename the temp append only file into the stable one: %s", strerror(errno));
            close(fd);
            goto cleanup;
        }
        /* Mission completed... almost */
        redisLog(REDIS_NOTICE,"Append only file successfully rewritten.");
        if (server.appendfd != -1) {
            /* If append only is actually enabled... */
            close(server.appendfd);
            server.appendfd = fd;
            fsync(fd);
            server.appendseldb = -1; /* Make sure it will issue SELECT */
            redisLog(REDIS_NOTICE,"The new append only file was selected for future appends.");
        } else {
            /* If append only is disabled we just generate a dump in this
             * format. Why not? */
            close(fd);
        }
    } else if (!bysignal && exitcode != 0) {
        redisLog(REDIS_WARNING, "Background append only file rewriting error");
    } else {
        redisLog(REDIS_WARNING,
            "Background append only file rewriting terminated by signal %d",
            WTERMSIG(statloc));
    }
cleanup:
    sdsfree(server.bgrewritebuf);
    server.bgrewritebuf = sdsempty();
    aofRemoveTempFile(server.bgrewritechildpid);
    server.bgrewritechildpid = -1;
}

/* This function is called once a background process of some kind terminates,
 * as we want to avoid resizing the hash tables when there is a child in order
 * to play well with copy-on-write (otherwise when a resize happens lots of
 * memory pages are copied). The goal of this function is to update the ability
 * for dict.c to resize the hash tables accordingly to the fact we have o not
 * running childs. */
static void updateDictResizePolicy(void) {
    if (server.bgsavechildpid == -1 && server.bgrewritechildpid == -1)
        dictEnableResize();
    else
        dictDisableResize();
}

static int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    int j, loops = server.cronloops++;
    REDIS_NOTUSED(eventLoop);
    REDIS_NOTUSED(id);
    REDIS_NOTUSED(clientData);

    /* We take a cached value of the unix time in the global state because
     * with virtual memory and aging there is to store the current time
     * in objects at every object access, and accuracy is not needed.
     * To access a global var is faster than calling time(NULL) */
    server.unixtime = time(NULL);

    /* We received a SIGTERM, shutting down here in a safe way, as it is
     * not ok doing so inside the signal handler. */
    if (server.shutdown_asap) {
        if (prepareForShutdown() == REDIS_OK) exit(0);
        redisLog(REDIS_WARNING,"SIGTERM received but errors trying to shut down the server, check the logs for more information");
    }

    /* Show some info about non-empty databases */
    for (j = 0; j < server.dbnum; j++) {
        long long size, used, vkeys;

        size = dictSlots(server.db[j].dict);
        used = dictSize(server.db[j].dict);
        vkeys = dictSize(server.db[j].expires);
        if (!(loops % 50) && (used || vkeys)) {
            redisLog(REDIS_VERBOSE,"DB %d: %lld keys (%lld volatile) in %lld slots HT.",j,used,vkeys,size);
            /* dictPrintStats(server.dict); */
        }
    }

    /* We don't want to resize the hash tables while a bacground saving
     * is in progress: the saving child is created using fork() that is
     * implemented with a copy-on-write semantic in most modern systems, so
     * if we resize the HT while there is the saving child at work actually
     * a lot of memory movements in the parent will cause a lot of pages
     * copied. */
    if (server.bgsavechildpid == -1 && server.bgrewritechildpid == -1) {
        if (!(loops % 10)) tryResizeHashTables();
        if (server.activerehashing) incrementallyRehash();
    }

    /* Show information about connected clients */
    if (!(loops % 50)) {
        redisLog(REDIS_VERBOSE,"%d clients connected (%d slaves), %zu bytes in use",
            listLength(server.clients)-listLength(server.slaves),
            listLength(server.slaves),
            zmalloc_used_memory());
    }

    /* Close connections of timedout clients */
    if ((server.maxidletime && !(loops % 100)) || server.blpop_blocked_clients)
        closeTimedoutClients();

    /* Check if a background saving or AOF rewrite in progress terminated */
    if (server.bgsavechildpid != -1 || server.bgrewritechildpid != -1) {
        int statloc;
        pid_t pid;

        if ((pid = wait3(&statloc,WNOHANG,NULL)) != 0) {
            if (pid == server.bgsavechildpid) {
                backgroundSaveDoneHandler(statloc);
            } else {
                backgroundRewriteDoneHandler(statloc);
            }
            updateDictResizePolicy();
        }
    } else {
        /* If there is not a background saving in progress check if
         * we have to save now */
         time_t now = time(NULL);
         for (j = 0; j < server.saveparamslen; j++) {
            struct saveparam *sp = server.saveparams+j;

            if (server.dirty >= sp->changes &&
                now-server.lastsave > sp->seconds) {
                redisLog(REDIS_NOTICE,"%d changes in %d seconds. Saving...",
                    sp->changes, sp->seconds);
                rdbSaveBackground(server.dbfilename);
                break;
            }
         }
    }

    /* Try to expire a few timed out keys. The algorithm used is adaptive and
     * will use few CPU cycles if there are few expiring keys, otherwise
     * it will get more aggressive to avoid that too much memory is used by
     * keys that can be removed from the keyspace. */
    for (j = 0; j < server.dbnum; j++) {
        int expired;
        redisDb *db = server.db+j;

        /* Continue to expire if at the end of the cycle more than 25%
         * of the keys were expired. */
        do {
            long num = dictSize(db->expires);
            time_t now = time(NULL);

            expired = 0;
            if (num > REDIS_EXPIRELOOKUPS_PER_CRON)
                num = REDIS_EXPIRELOOKUPS_PER_CRON;
            while (num--) {
                dictEntry *de;
                time_t t;

                if ((de = dictGetRandomKey(db->expires)) == NULL) break;
                t = (time_t) dictGetEntryVal(de);
                if (now > t) {
                    deleteKey(db,dictGetEntryKey(de));
                    expired++;
                    server.stat_expiredkeys++;
                }
            }
        } while (expired > REDIS_EXPIRELOOKUPS_PER_CRON/4);
    }

    /* Swap a few keys on disk if we are over the memory limit and VM
     * is enbled. Try to free objects from the free list first. */
    if (vmCanSwapOut()) {
        while (server.vm_enabled && zmalloc_used_memory() >
                server.vm_max_memory)
        {
            int retval;

            if (tryFreeOneObjectFromFreelist() == REDIS_OK) continue;
            retval = (server.vm_max_threads == 0) ?
                        vmSwapOneObjectBlocking() :
                        vmSwapOneObjectThreaded();
            if (retval == REDIS_ERR && !(loops % 300) &&
                zmalloc_used_memory() >
                (server.vm_max_memory+server.vm_max_memory/10))
            {
                redisLog(REDIS_WARNING,"WARNING: vm-max-memory limit exceeded by more than 10%% but unable to swap more objects out!");
            }
            /* Note that when using threade I/O we free just one object,
             * because anyway when the I/O thread in charge to swap this
             * object out will finish, the handler of completed jobs
             * will try to swap more objects if we are still out of memory. */
            if (retval == REDIS_ERR || server.vm_max_threads > 0) break;
        }
    }

    /* Check if we should connect to a MASTER */
    if (server.replstate == REDIS_REPL_CONNECT && !(loops % 10)) {
        redisLog(REDIS_NOTICE,"Connecting to MASTER...");
        if (syncWithMaster() == REDIS_OK) {
            redisLog(REDIS_NOTICE,"MASTER <-> SLAVE sync succeeded");
            if (server.appendonly) rewriteAppendOnlyFileBackground();
        }
    }
    return 100;
}

/* This function gets called every time Redis is entering the
 * main loop of the event driven library, that is, before to sleep
 * for ready file descriptors. */
static void beforeSleep(struct aeEventLoop *eventLoop) {
    REDIS_NOTUSED(eventLoop);

    /* Awake clients that got all the swapped keys they requested */
    if (server.vm_enabled && listLength(server.io_ready_clients)) {
        listIter li;
        listNode *ln;

        listRewind(server.io_ready_clients,&li);
        while((ln = listNext(&li))) {
            redisClient *c = ln->value;
            struct redisCommand *cmd;

            /* Resume the client. */
            listDelNode(server.io_ready_clients,ln);
            c->flags &= (~REDIS_IO_WAIT);
            server.vm_blocked_clients--;
            aeCreateFileEvent(server.el, c->fd, AE_READABLE,
                readQueryFromClient, c);
            cmd = lookupCommand(c->argv[0]->ptr);
            assert(cmd != NULL);
            call(c,cmd);
            resetClient(c);
            /* There may be more data to process in the input buffer. */
            if (c->querybuf && sdslen(c->querybuf) > 0)
                processInputBuffer(c);
        }
    }
    /* Write the AOF buffer on disk */
    flushAppendOnlyFile();
}

static void createSharedObjects(void) {
    int j;

    shared.crlf = createObject(REDIS_STRING,sdsnew("\r\n"));
    shared.ok = createObject(REDIS_STRING,sdsnew("+OK\r\n"));
    shared.err = createObject(REDIS_STRING,sdsnew("-ERR\r\n"));
    shared.emptybulk = createObject(REDIS_STRING,sdsnew("$0\r\n\r\n"));
    shared.czero = createObject(REDIS_STRING,sdsnew(":0\r\n"));
    shared.cone = createObject(REDIS_STRING,sdsnew(":1\r\n"));
    shared.nullbulk = createObject(REDIS_STRING,sdsnew("$-1\r\n"));
    shared.nullmultibulk = createObject(REDIS_STRING,sdsnew("*-1\r\n"));
    shared.emptymultibulk = createObject(REDIS_STRING,sdsnew("*0\r\n"));
    shared.pong = createObject(REDIS_STRING,sdsnew("+PONG\r\n"));
    shared.queued = createObject(REDIS_STRING,sdsnew("+QUEUED\r\n"));
    shared.wrongtypeerr = createObject(REDIS_STRING,sdsnew(
        "-ERR Operation against a key holding the wrong kind of value\r\n"));
    shared.nokeyerr = createObject(REDIS_STRING,sdsnew(
        "-ERR no such key\r\n"));
    shared.syntaxerr = createObject(REDIS_STRING,sdsnew(
        "-ERR syntax error\r\n"));
    shared.sameobjecterr = createObject(REDIS_STRING,sdsnew(
        "-ERR source and destination objects are the same\r\n"));
    shared.outofrangeerr = createObject(REDIS_STRING,sdsnew(
        "-ERR index out of range\r\n"));
    shared.space = createObject(REDIS_STRING,sdsnew(" "));
    shared.colon = createObject(REDIS_STRING,sdsnew(":"));
    shared.plus = createObject(REDIS_STRING,sdsnew("+"));
    shared.select0 = createStringObject("select 0\r\n",10);
    shared.select1 = createStringObject("select 1\r\n",10);
    shared.select2 = createStringObject("select 2\r\n",10);
    shared.select3 = createStringObject("select 3\r\n",10);
    shared.select4 = createStringObject("select 4\r\n",10);
    shared.select5 = createStringObject("select 5\r\n",10);
    shared.select6 = createStringObject("select 6\r\n",10);
    shared.select7 = createStringObject("select 7\r\n",10);
    shared.select8 = createStringObject("select 8\r\n",10);
    shared.select9 = createStringObject("select 9\r\n",10);
    shared.messagebulk = createStringObject("$7\r\nmessage\r\n",13);
    shared.pmessagebulk = createStringObject("$8\r\npmessage\r\n",14);
    shared.subscribebulk = createStringObject("$9\r\nsubscribe\r\n",15);
    shared.unsubscribebulk = createStringObject("$11\r\nunsubscribe\r\n",18);
    shared.psubscribebulk = createStringObject("$10\r\npsubscribe\r\n",17);
    shared.punsubscribebulk = createStringObject("$12\r\npunsubscribe\r\n",19);
    shared.mbulk3 = createStringObject("*3\r\n",4);
    shared.mbulk4 = createStringObject("*4\r\n",4);
    for (j = 0; j < REDIS_SHARED_INTEGERS; j++) {
        shared.integers[j] = createObject(REDIS_STRING,(void*)(long)j);
        shared.integers[j]->encoding = REDIS_ENCODING_INT;
    }
}

static void appendServerSaveParams(time_t seconds, int changes) {
    server.saveparams = zrealloc(server.saveparams,sizeof(struct saveparam)*(server.saveparamslen+1));
    server.saveparams[server.saveparamslen].seconds = seconds;
    server.saveparams[server.saveparamslen].changes = changes;
    server.saveparamslen++;
}

static void resetServerSaveParams() {
    zfree(server.saveparams);
    server.saveparams = NULL;
    server.saveparamslen = 0;
}

static void initServerConfig() {
    server.dbnum = REDIS_DEFAULT_DBNUM;
    server.port = REDIS_SERVERPORT;
    server.verbosity = REDIS_VERBOSE;
    server.maxidletime = REDIS_MAXIDLETIME;
    server.saveparams = NULL;
    server.logfile = NULL; /* NULL = log on standard output */
    server.bindaddr = NULL;
    server.glueoutputbuf = 1;
    server.daemonize = 0;
    server.appendonly = 0;
    server.appendfsync = APPENDFSYNC_EVERYSEC;
    server.lastfsync = time(NULL);
    server.appendfd = -1;
    server.appendseldb = -1; /* Make sure the first time will not match */
    server.pidfile = zstrdup("/var/run/redis.pid");
    server.dbfilename = zstrdup("dump.rdb");
    server.appendfilename = zstrdup("appendonly.aof");
    server.requirepass = NULL;
    server.rdbcompression = 1;
    server.activerehashing = 1;
    server.maxclients = 0;
    server.blpop_blocked_clients = 0;
    server.maxmemory = 0;
    server.vm_enabled = 0;
    server.vm_swap_file = zstrdup("/tmp/redis-%p.vm");
    server.vm_page_size = 256;          /* 256 bytes per page */
    server.vm_pages = 1024*1024*100;    /* 104 millions of pages */
    server.vm_max_memory = 1024LL*1024*1024*1; /* 1 GB of RAM */
    server.vm_max_threads = 4;
    server.vm_blocked_clients = 0;
    server.hash_max_zipmap_entries = REDIS_HASH_MAX_ZIPMAP_ENTRIES;
    server.hash_max_zipmap_value = REDIS_HASH_MAX_ZIPMAP_VALUE;
    server.shutdown_asap = 0;

    resetServerSaveParams();

    appendServerSaveParams(60*60,1);  /* save after 1 hour and 1 change */
    appendServerSaveParams(300,100);  /* save after 5 minutes and 100 changes */
    appendServerSaveParams(60,10000); /* save after 1 minute and 10000 changes */
    /* Replication related */
    server.isslave = 0;
    server.masterauth = NULL;
    server.masterhost = NULL;
    server.masterport = 6379;
    server.master = NULL;
    server.replstate = REDIS_REPL_NONE;

    /* Double constants initialization */
    R_Zero = 0.0;
    R_PosInf = 1.0/R_Zero;
    R_NegInf = -1.0/R_Zero;
    R_Nan = R_Zero/R_Zero;
}

static void initServer() {
    int j;

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setupSigSegvAction();

    server.devnull = fopen("/dev/null","w");
    if (server.devnull == NULL) {
        redisLog(REDIS_WARNING, "Can't open /dev/null: %s", server.neterr);
        exit(1);
    }
    server.clients = listCreate();
    server.slaves = listCreate();
    server.monitors = listCreate();
    server.objfreelist = listCreate();
    createSharedObjects();
    server.el = aeCreateEventLoop();
    server.db = zmalloc(sizeof(redisDb)*server.dbnum);
    server.fd = anetTcpServer(server.neterr, server.port, server.bindaddr);
    if (server.fd == -1) {
        redisLog(REDIS_WARNING, "Opening TCP port: %s", server.neterr);
        exit(1);
    }
    for (j = 0; j < server.dbnum; j++) {
        server.db[j].dict = dictCreate(&dbDictType,NULL);
        server.db[j].expires = dictCreate(&keyptrDictType,NULL);
        server.db[j].blocking_keys = dictCreate(&keylistDictType,NULL);
        server.db[j].watched_keys = dictCreate(&keylistDictType,NULL);
        if (server.vm_enabled)
            server.db[j].io_keys = dictCreate(&keylistDictType,NULL);
        server.db[j].id = j;
    }
    server.pubsub_channels = dictCreate(&keylistDictType,NULL);
    server.pubsub_patterns = listCreate();
    listSetFreeMethod(server.pubsub_patterns,freePubsubPattern);
    listSetMatchMethod(server.pubsub_patterns,listMatchPubsubPattern);
    server.cronloops = 0;
    server.bgsavechildpid = -1;
    server.bgrewritechildpid = -1;
    server.bgrewritebuf = sdsempty();
    server.aofbuf = sdsempty();
    server.lastsave = time(NULL);
    server.dirty = 0;
    server.stat_numcommands = 0;
    server.stat_numconnections = 0;
    server.stat_expiredkeys = 0;
    server.stat_starttime = time(NULL);
    server.unixtime = time(NULL);
    aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL);
    if (aeCreateFileEvent(server.el, server.fd, AE_READABLE,
        acceptHandler, NULL) == AE_ERR) oom("creating file event");

    if (server.appendonly) {
        server.appendfd = open(server.appendfilename,O_WRONLY|O_APPEND|O_CREAT,0644);
        if (server.appendfd == -1) {
            redisLog(REDIS_WARNING, "Can't open the append-only file: %s",
                strerror(errno));
            exit(1);
        }
    }

    if (server.vm_enabled) vmInit();
}

/* Empty the whole database */
static long long emptyDb() {
    int j;
    long long removed = 0;

    for (j = 0; j < server.dbnum; j++) {
        removed += dictSize(server.db[j].dict);
        dictEmpty(server.db[j].dict);
        dictEmpty(server.db[j].expires);
    }
    return removed;
}

static int yesnotoi(char *s) {
    if (!strcasecmp(s,"yes")) return 1;
    else if (!strcasecmp(s,"no")) return 0;
    else return -1;
}

/* I agree, this is a very rudimental way to load a configuration...
   will improve later if the config gets more complex */
static void loadServerConfig(char *filename) {
    FILE *fp;
    char buf[REDIS_CONFIGLINE_MAX+1], *err = NULL;
    int linenum = 0;
    sds line = NULL;

    if (filename[0] == '-' && filename[1] == '\0')
        fp = stdin;
    else {
        if ((fp = fopen(filename,"r")) == NULL) {
            redisLog(REDIS_WARNING, "Fatal error, can't open config file '%s'", filename);
            exit(1);
        }
    }

    while(fgets(buf,REDIS_CONFIGLINE_MAX+1,fp) != NULL) {
        sds *argv;
        int argc, j;

        linenum++;
        line = sdsnew(buf);
        line = sdstrim(line," \t\r\n");

        /* Skip comments and blank lines*/
        if (line[0] == '#' || line[0] == '\0') {
            sdsfree(line);
            continue;
        }

        /* Split into arguments */
        argv = sdssplitlen(line,sdslen(line)," ",1,&argc);
        sdstolower(argv[0]);

        /* Execute config directives */
        if (!strcasecmp(argv[0],"timeout") && argc == 2) {
            server.maxidletime = atoi(argv[1]);
            if (server.maxidletime < 0) {
                err = "Invalid timeout value"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"port") && argc == 2) {
            server.port = atoi(argv[1]);
            if (server.port < 1 || server.port > 65535) {
                err = "Invalid port"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"bind") && argc == 2) {
            server.bindaddr = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"save") && argc == 3) {
            int seconds = atoi(argv[1]);
            int changes = atoi(argv[2]);
            if (seconds < 1 || changes < 0) {
                err = "Invalid save parameters"; goto loaderr;
            }
            appendServerSaveParams(seconds,changes);
        } else if (!strcasecmp(argv[0],"dir") && argc == 2) {
            if (chdir(argv[1]) == -1) {
                redisLog(REDIS_WARNING,"Can't chdir to '%s': %s",
                    argv[1], strerror(errno));
                exit(1);
            }
        } else if (!strcasecmp(argv[0],"loglevel") && argc == 2) {
            if (!strcasecmp(argv[1],"debug")) server.verbosity = REDIS_DEBUG;
            else if (!strcasecmp(argv[1],"verbose")) server.verbosity = REDIS_VERBOSE;
            else if (!strcasecmp(argv[1],"notice")) server.verbosity = REDIS_NOTICE;
            else if (!strcasecmp(argv[1],"warning")) server.verbosity = REDIS_WARNING;
            else {
                err = "Invalid log level. Must be one of debug, notice, warning";
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"logfile") && argc == 2) {
            FILE *logfp;

            server.logfile = zstrdup(argv[1]);
            if (!strcasecmp(server.logfile,"stdout")) {
                zfree(server.logfile);
                server.logfile = NULL;
            }
            if (server.logfile) {
                /* Test if we are able to open the file. The server will not
                 * be able to abort just for this problem later... */
                logfp = fopen(server.logfile,"a");
                if (logfp == NULL) {
                    err = sdscatprintf(sdsempty(),
                        "Can't open the log file: %s", strerror(errno));
                    goto loaderr;
                }
                fclose(logfp);
            }
        } else if (!strcasecmp(argv[0],"databases") && argc == 2) {
            server.dbnum = atoi(argv[1]);
            if (server.dbnum < 1) {
                err = "Invalid number of databases"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"include") && argc == 2) {
            loadServerConfig(argv[1]);
        } else if (!strcasecmp(argv[0],"maxclients") && argc == 2) {
            server.maxclients = atoi(argv[1]);
        } else if (!strcasecmp(argv[0],"maxmemory") && argc == 2) {
            server.maxmemory = memtoll(argv[1],NULL);
        } else if (!strcasecmp(argv[0],"slaveof") && argc == 3) {
            server.masterhost = sdsnew(argv[1]);
            server.masterport = atoi(argv[2]);
            server.replstate = REDIS_REPL_CONNECT;
        } else if (!strcasecmp(argv[0],"masterauth") && argc == 2) {
        	server.masterauth = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"glueoutputbuf") && argc == 2) {
            if ((server.glueoutputbuf = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"rdbcompression") && argc == 2) {
            if ((server.rdbcompression = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"activerehashing") && argc == 2) {
            if ((server.activerehashing = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"daemonize") && argc == 2) {
            if ((server.daemonize = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"appendonly") && argc == 2) {
            if ((server.appendonly = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"appendfilename") && argc == 2) {
            zfree(server.appendfilename);
            server.appendfilename = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"appendfsync") && argc == 2) {
            if (!strcasecmp(argv[1],"no")) {
                server.appendfsync = APPENDFSYNC_NO;
            } else if (!strcasecmp(argv[1],"always")) {
                server.appendfsync = APPENDFSYNC_ALWAYS;
            } else if (!strcasecmp(argv[1],"everysec")) {
                server.appendfsync = APPENDFSYNC_EVERYSEC;
            } else {
                err = "argument must be 'no', 'always' or 'everysec'";
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"requirepass") && argc == 2) {
            server.requirepass = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"pidfile") && argc == 2) {
            zfree(server.pidfile);
            server.pidfile = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"dbfilename") && argc == 2) {
            zfree(server.dbfilename);
            server.dbfilename = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"vm-enabled") && argc == 2) {
            if ((server.vm_enabled = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"vm-swap-file") && argc == 2) {
            zfree(server.vm_swap_file);
            server.vm_swap_file = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"vm-max-memory") && argc == 2) {
            server.vm_max_memory = memtoll(argv[1],NULL);
        } else if (!strcasecmp(argv[0],"vm-page-size") && argc == 2) {
            server.vm_page_size = memtoll(argv[1], NULL);
        } else if (!strcasecmp(argv[0],"vm-pages") && argc == 2) {
            server.vm_pages = memtoll(argv[1], NULL);
        } else if (!strcasecmp(argv[0],"vm-max-threads") && argc == 2) {
            server.vm_max_threads = strtoll(argv[1], NULL, 10);
        } else if (!strcasecmp(argv[0],"hash-max-zipmap-entries") && argc == 2){
            server.hash_max_zipmap_entries = memtoll(argv[1], NULL);
        } else if (!strcasecmp(argv[0],"hash-max-zipmap-value") && argc == 2){
            server.hash_max_zipmap_value = memtoll(argv[1], NULL);
        } else {
            err = "Bad directive or wrong number of arguments"; goto loaderr;
        }
        for (j = 0; j < argc; j++)
            sdsfree(argv[j]);
        zfree(argv);
        sdsfree(line);
    }
    if (fp != stdin) fclose(fp);
    return;

loaderr:
    fprintf(stderr, "\n*** FATAL CONFIG FILE ERROR ***\n");
    fprintf(stderr, "Reading the configuration file, at line %d\n", linenum);
    fprintf(stderr, ">>> '%s'\n", line);
    fprintf(stderr, "%s\n", err);
    exit(1);
}

static void freeClientArgv(redisClient *c) {
    int j;

    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    for (j = 0; j < c->mbargc; j++)
        decrRefCount(c->mbargv[j]);
    c->argc = 0;
    c->mbargc = 0;
}

static void freeClient(redisClient *c) {
    listNode *ln;

    /* Note that if the client we are freeing is blocked into a blocking
     * call, we have to set querybuf to NULL *before* to call
     * unblockClientWaitingData() to avoid processInputBuffer() will get
     * called. Also it is important to remove the file events after
     * this, because this call adds the READABLE event. */
    sdsfree(c->querybuf);
    c->querybuf = NULL;
    if (c->flags & REDIS_BLOCKED)
        unblockClientWaitingData(c);

    /* UNWATCH all the keys */
    unwatchAllKeys(c);
    listRelease(c->watched_keys);
    /* Unsubscribe from all the pubsub channels */
    pubsubUnsubscribeAllChannels(c,0);
    pubsubUnsubscribeAllPatterns(c,0);
    dictRelease(c->pubsub_channels);
    listRelease(c->pubsub_patterns);
    /* Obvious cleanup */
    aeDeleteFileEvent(server.el,c->fd,AE_READABLE);
    aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);
    listRelease(c->reply);
    freeClientArgv(c);
    close(c->fd);
    /* Remove from the list of clients */
    ln = listSearchKey(server.clients,c);
    redisAssert(ln != NULL);
    listDelNode(server.clients,ln);
    /* Remove from the list of clients that are now ready to be restarted
     * after waiting for swapped keys */
    if (c->flags & REDIS_IO_WAIT && listLength(c->io_keys) == 0) {
        ln = listSearchKey(server.io_ready_clients,c);
        if (ln) {
            listDelNode(server.io_ready_clients,ln);
            server.vm_blocked_clients--;
        }
    }
    /* Remove from the list of clients waiting for swapped keys */
    while (server.vm_enabled && listLength(c->io_keys)) {
        ln = listFirst(c->io_keys);
        dontWaitForSwappedKey(c,ln->value);
    }
    listRelease(c->io_keys);
    /* Master/slave cleanup */
    if (c->flags & REDIS_SLAVE) {
        if (c->replstate == REDIS_REPL_SEND_BULK && c->repldbfd != -1)
            close(c->repldbfd);
        list *l = (c->flags & REDIS_MONITOR) ? server.monitors : server.slaves;
        ln = listSearchKey(l,c);
        redisAssert(ln != NULL);
        listDelNode(l,ln);
    }
    if (c->flags & REDIS_MASTER) {
        server.master = NULL;
        server.replstate = REDIS_REPL_CONNECT;
    }
    /* Release memory */
    zfree(c->argv);
    zfree(c->mbargv);
    freeClientMultiState(c);
    zfree(c);
}

#define GLUEREPLY_UP_TO (1024)
static void glueReplyBuffersIfNeeded(redisClient *c) {
    int copylen = 0;
    char buf[GLUEREPLY_UP_TO];
    listNode *ln;
    listIter li;
    robj *o;

    listRewind(c->reply,&li);
    while((ln = listNext(&li))) {
        int objlen;

        o = ln->value;
        objlen = sdslen(o->ptr);
        if (copylen + objlen <= GLUEREPLY_UP_TO) {
            memcpy(buf+copylen,o->ptr,objlen);
            copylen += objlen;
            listDelNode(c->reply,ln);
        } else {
            if (copylen == 0) return;
            break;
        }
    }
    /* Now the output buffer is empty, add the new single element */
    o = createObject(REDIS_STRING,sdsnewlen(buf,copylen));
    listAddNodeHead(c->reply,o);
}

static void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = privdata;
    int nwritten = 0, totwritten = 0, objlen;
    robj *o;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    /* Use writev() if we have enough buffers to send */
    if (!server.glueoutputbuf &&
        listLength(c->reply) > REDIS_WRITEV_THRESHOLD &&
        !(c->flags & REDIS_MASTER))
    {
        sendReplyToClientWritev(el, fd, privdata, mask);
        return;
    }

    while(listLength(c->reply)) {
        if (server.glueoutputbuf && listLength(c->reply) > 1)
            glueReplyBuffersIfNeeded(c);

        o = listNodeValue(listFirst(c->reply));
        objlen = sdslen(o->ptr);

        if (objlen == 0) {
            listDelNode(c->reply,listFirst(c->reply));
            continue;
        }

        if (c->flags & REDIS_MASTER) {
            /* Don't reply to a master */
            nwritten = objlen - c->sentlen;
        } else {
            nwritten = write(fd, ((char*)o->ptr)+c->sentlen, objlen - c->sentlen);
            if (nwritten <= 0) break;
        }
        c->sentlen += nwritten;
        totwritten += nwritten;
        /* If we fully sent the object on head go to the next one */
        if (c->sentlen == objlen) {
            listDelNode(c->reply,listFirst(c->reply));
            c->sentlen = 0;
        }
        /* Note that we avoid to send more thank REDIS_MAX_WRITE_PER_EVENT
         * bytes, in a single threaded server it's a good idea to serve
         * other clients as well, even if a very large request comes from
         * super fast link that is always able to accept data (in real world
         * scenario think about 'KEYS *' against the loopback interfae) */
        if (totwritten > REDIS_MAX_WRITE_PER_EVENT) break;
    }
    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            redisLog(REDIS_VERBOSE,
                "Error writing to client: %s", strerror(errno));
            freeClient(c);
            return;
        }
    }
    if (totwritten > 0) c->lastinteraction = time(NULL);
    if (listLength(c->reply) == 0) {
        c->sentlen = 0;
        aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);
    }
}

static void sendReplyToClientWritev(aeEventLoop *el, int fd, void *privdata, int mask)
{
    redisClient *c = privdata;
    int nwritten = 0, totwritten = 0, objlen, willwrite;
    robj *o;
    struct iovec iov[REDIS_WRITEV_IOVEC_COUNT];
    int offset, ion = 0;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    listNode *node;
    while (listLength(c->reply)) {
        offset = c->sentlen;
        ion = 0;
        willwrite = 0;

        /* fill-in the iov[] array */
        for(node = listFirst(c->reply); node; node = listNextNode(node)) {
            o = listNodeValue(node);
            objlen = sdslen(o->ptr);

            if (totwritten + objlen - offset > REDIS_MAX_WRITE_PER_EVENT)
                break;

            if(ion == REDIS_WRITEV_IOVEC_COUNT)
                break; /* no more iovecs */

            iov[ion].iov_base = ((char*)o->ptr) + offset;
            iov[ion].iov_len = objlen - offset;
            willwrite += objlen - offset;
            offset = 0; /* just for the first item */
            ion++;
        }

        if(willwrite == 0)
            break;

        /* write all collected blocks at once */
        if((nwritten = writev(fd, iov, ion)) < 0) {
            if (errno != EAGAIN) {
                redisLog(REDIS_VERBOSE,
                         "Error writing to client: %s", strerror(errno));
                freeClient(c);
                return;
            }
            break;
        }

        totwritten += nwritten;
        offset = c->sentlen;

        /* remove written robjs from c->reply */
        while (nwritten && listLength(c->reply)) {
            o = listNodeValue(listFirst(c->reply));
            objlen = sdslen(o->ptr);

            if(nwritten >= objlen - offset) {
                listDelNode(c->reply, listFirst(c->reply));
                nwritten -= objlen - offset;
                c->sentlen = 0;
            } else {
                /* partial write */
                c->sentlen += nwritten;
                break;
            }
            offset = 0;
        }
    }

    if (totwritten > 0)
        c->lastinteraction = time(NULL);

    if (listLength(c->reply) == 0) {
        c->sentlen = 0;
        aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);
    }
}

static struct redisCommand *lookupCommand(char *name) {
    int j = 0;
    while(cmdTable[j].name != NULL) {
        if (!strcasecmp(name,cmdTable[j].name)) return &cmdTable[j];
        j++;
    }
    return NULL;
}

/* resetClient prepare the client to process the next command */
static void resetClient(redisClient *c) {
    freeClientArgv(c);
    c->bulklen = -1;
    c->multibulk = 0;
}

/* Call() is the core of Redis execution of a command */
static void call(redisClient *c, struct redisCommand *cmd) {
    long long dirty;

    dirty = server.dirty;
    cmd->proc(c);
    dirty = server.dirty-dirty;

    if (server.appendonly && dirty)
        feedAppendOnlyFile(cmd,c->db->id,c->argv,c->argc);
    if ((dirty || cmd->flags & REDIS_CMD_FORCE_REPLICATION) &&
        listLength(server.slaves))
        replicationFeedSlaves(server.slaves,c->db->id,c->argv,c->argc);
    if (listLength(server.monitors))
        replicationFeedMonitors(server.monitors,c->db->id,c->argv,c->argc);
    server.stat_numcommands++;
}

/* If this function gets called we already read a whole
 * command, argments are in the client argv/argc fields.
 * processCommand() execute the command or prepare the
 * server for a bulk read from the client.
 *
 * If 1 is returned the client is still alive and valid and
 * and other operations can be performed by the caller. Otherwise
 * if 0 is returned the client was destroied (i.e. after QUIT). */
static int processCommand(redisClient *c) {
    struct redisCommand *cmd;

    /* Free some memory if needed (maxmemory setting) */
    if (server.maxmemory) freeMemoryIfNeeded();

    /* Handle the multi bulk command type. This is an alternative protocol
     * supported by Redis in order to receive commands that are composed of
     * multiple binary-safe "bulk" arguments. The latency of processing is
     * a bit higher but this allows things like multi-sets, so if this
     * protocol is used only for MSET and similar commands this is a big win. */
    if (c->multibulk == 0 && c->argc == 1 && ((char*)(c->argv[0]->ptr))[0] == '*') {
        c->multibulk = atoi(((char*)c->argv[0]->ptr)+1);
        if (c->multibulk <= 0) {
            resetClient(c);
            return 1;
        } else {
            decrRefCount(c->argv[c->argc-1]);
            c->argc--;
            return 1;
        }
    } else if (c->multibulk) {
        if (c->bulklen == -1) {
            if (((char*)c->argv[0]->ptr)[0] != '$') {
                addReplySds(c,sdsnew("-ERR multi bulk protocol error\r\n"));
                resetClient(c);
                return 1;
            } else {
                int bulklen = atoi(((char*)c->argv[0]->ptr)+1);
                decrRefCount(c->argv[0]);
                if (bulklen < 0 || bulklen > 1024*1024*1024) {
                    c->argc--;
                    addReplySds(c,sdsnew("-ERR invalid bulk write count\r\n"));
                    resetClient(c);
                    return 1;
                }
                c->argc--;
                c->bulklen = bulklen+2; /* add two bytes for CR+LF */
                return 1;
            }
        } else {
            c->mbargv = zrealloc(c->mbargv,(sizeof(robj*))*(c->mbargc+1));
            c->mbargv[c->mbargc] = c->argv[0];
            c->mbargc++;
            c->argc--;
            c->multibulk--;
            if (c->multibulk == 0) {
                robj **auxargv;
                int auxargc;

                /* Here we need to swap the multi-bulk argc/argv with the
                 * normal argc/argv of the client structure. */
                auxargv = c->argv;
                c->argv = c->mbargv;
                c->mbargv = auxargv;

                auxargc = c->argc;
                c->argc = c->mbargc;
                c->mbargc = auxargc;

                /* We need to set bulklen to something different than -1
                 * in order for the code below to process the command without
                 * to try to read the last argument of a bulk command as
                 * a special argument. */
                c->bulklen = 0;
                /* continue below and process the command */
            } else {
                c->bulklen = -1;
                return 1;
            }
        }
    }
    /* -- end of multi bulk commands processing -- */

    /* The QUIT command is handled as a special case. Normal command
     * procs are unable to close the client connection safely */
    if (!strcasecmp(c->argv[0]->ptr,"quit")) {
        freeClient(c);
        return 0;
    }

    /* Now lookup the command and check ASAP about trivial error conditions
     * such wrong arity, bad command name and so forth. */
    cmd = lookupCommand(c->argv[0]->ptr);
    if (!cmd) {
        addReplySds(c,
            sdscatprintf(sdsempty(), "-ERR unknown command '%s'\r\n",
                (char*)c->argv[0]->ptr));
        resetClient(c);
        return 1;
    } else if ((cmd->arity > 0 && cmd->arity != c->argc) ||
               (c->argc < -cmd->arity)) {
        addReplySds(c,
            sdscatprintf(sdsempty(),
                "-ERR wrong number of arguments for '%s' command\r\n",
                cmd->name));
        resetClient(c);
        return 1;
    } else if (cmd->flags & REDIS_CMD_BULK && c->bulklen == -1) {
        /* This is a bulk command, we have to read the last argument yet. */
        int bulklen = atoi(c->argv[c->argc-1]->ptr);

        decrRefCount(c->argv[c->argc-1]);
        if (bulklen < 0 || bulklen > 1024*1024*1024) {
            c->argc--;
            addReplySds(c,sdsnew("-ERR invalid bulk write count\r\n"));
            resetClient(c);
            return 1;
        }
        c->argc--;
        c->bulklen = bulklen+2; /* add two bytes for CR+LF */
        /* It is possible that the bulk read is already in the
         * buffer. Check this condition and handle it accordingly.
         * This is just a fast path, alternative to call processInputBuffer().
         * It's a good idea since the code is small and this condition
         * happens most of the times. */
        if ((signed)sdslen(c->querybuf) >= c->bulklen) {
            c->argv[c->argc] = createStringObject(c->querybuf,c->bulklen-2);
            c->argc++;
            c->querybuf = sdsrange(c->querybuf,c->bulklen,-1);
        } else {
            /* Otherwise return... there is to read the last argument
             * from the socket. */
            return 1;
        }
    }
    /* Let's try to encode the bulk object to save space. */
    if (cmd->flags & REDIS_CMD_BULK)
        c->argv[c->argc-1] = tryObjectEncoding(c->argv[c->argc-1]);

    /* Check if the user is authenticated */
    if (server.requirepass && !c->authenticated && cmd->proc != authCommand) {
        addReplySds(c,sdsnew("-ERR operation not permitted\r\n"));
        resetClient(c);
        return 1;
    }

    /* Handle the maxmemory directive */
    if (server.maxmemory && (cmd->flags & REDIS_CMD_DENYOOM) &&
        zmalloc_used_memory() > server.maxmemory)
    {
        addReplySds(c,sdsnew("-ERR command not allowed when used memory > 'maxmemory'\r\n"));
        resetClient(c);
        return 1;
    }

    /* Only allow SUBSCRIBE and UNSUBSCRIBE in the context of Pub/Sub */
    if ((dictSize(c->pubsub_channels) > 0 || listLength(c->pubsub_patterns) > 0)
        &&
        cmd->proc != subscribeCommand && cmd->proc != unsubscribeCommand &&
        cmd->proc != psubscribeCommand && cmd->proc != punsubscribeCommand) {
        addReplySds(c,sdsnew("-ERR only (P)SUBSCRIBE / (P)UNSUBSCRIBE / QUIT allowed in this context\r\n"));
        resetClient(c);
        return 1;
    }

    /* Exec the command */
    if (c->flags & REDIS_MULTI &&
        cmd->proc != execCommand && cmd->proc != discardCommand &&
        cmd->proc != multiCommand && cmd->proc != watchCommand)
    {
        queueMultiCommand(c,cmd);
        addReply(c,shared.queued);
    } else {
        if (server.vm_enabled && server.vm_max_threads > 0 &&
            blockClientOnSwappedKeys(c,cmd)) return 1;
        call(c,cmd);
    }

    /* Prepare the client for the next command */
    resetClient(c);
    return 1;
}

static void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int outc = 0, j;
    robj **outv;
    /* We need 1+(ARGS*3) objects since commands are using the new protocol
     * and we one 1 object for the first "*<count>\r\n" multibulk count, then
     * for every additional object we have "$<count>\r\n" + object + "\r\n". */
    robj *static_outv[REDIS_STATIC_ARGS*3+1];
    robj *lenobj;

    if (argc <= REDIS_STATIC_ARGS) {
        outv = static_outv;
    } else {
        outv = zmalloc(sizeof(robj*)*(argc*3+1));
    }

    lenobj = createObject(REDIS_STRING,
            sdscatprintf(sdsempty(), "*%d\r\n", argc));
    lenobj->refcount = 0;
    outv[outc++] = lenobj;
    for (j = 0; j < argc; j++) {
        lenobj = createObject(REDIS_STRING,
            sdscatprintf(sdsempty(),"$%lu\r\n",
                (unsigned long) stringObjectLen(argv[j])));
        lenobj->refcount = 0;
        outv[outc++] = lenobj;
        outv[outc++] = argv[j];
        outv[outc++] = shared.crlf;
    }

    /* Increment all the refcounts at start and decrement at end in order to
     * be sure to free objects if there is no slave in a replication state
     * able to be feed with commands */
    for (j = 0; j < outc; j++) incrRefCount(outv[j]);
    listRewind(slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;

        /* Don't feed slaves that are still waiting for BGSAVE to start */
        if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) continue;

        /* Feed all the other slaves, MONITORs and so on */
        if (slave->slaveseldb != dictid) {
            robj *selectcmd;

            switch(dictid) {
            case 0: selectcmd = shared.select0; break;
            case 1: selectcmd = shared.select1; break;
            case 2: selectcmd = shared.select2; break;
            case 3: selectcmd = shared.select3; break;
            case 4: selectcmd = shared.select4; break;
            case 5: selectcmd = shared.select5; break;
            case 6: selectcmd = shared.select6; break;
            case 7: selectcmd = shared.select7; break;
            case 8: selectcmd = shared.select8; break;
            case 9: selectcmd = shared.select9; break;
            default:
                selectcmd = createObject(REDIS_STRING,
                    sdscatprintf(sdsempty(),"select %d\r\n",dictid));
                selectcmd->refcount = 0;
                break;
            }
            addReply(slave,selectcmd);
            slave->slaveseldb = dictid;
        }
        for (j = 0; j < outc; j++) addReply(slave,outv[j]);
    }
    for (j = 0; j < outc; j++) decrRefCount(outv[j]);
    if (outv != static_outv) zfree(outv);
}

static sds sdscatrepr(sds s, char *p, size_t len) {
    s = sdscatlen(s,"\"",1);
    while(len--) {
        switch(*p) {
        case '\\':
        case '"':
            s = sdscatprintf(s,"\\%c",*p);
            break;
        case '\n': s = sdscatlen(s,"\\n",1); break;
        case '\r': s = sdscatlen(s,"\\r",1); break;
        case '\t': s = sdscatlen(s,"\\t",1); break;
        case '\a': s = sdscatlen(s,"\\a",1); break;
        case '\b': s = sdscatlen(s,"\\b",1); break;
        default:
            if (isprint(*p))
                s = sdscatprintf(s,"%c",*p);
            else
                s = sdscatprintf(s,"\\x%02x",(unsigned char)*p);
            break;
        }
        p++;
    }
    return sdscatlen(s,"\"",1);
}

static void replicationFeedMonitors(list *monitors, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int j;
    sds cmdrepr = sdsnew("+");
    robj *cmdobj;
    struct timeval tv;

    gettimeofday(&tv,NULL);
    cmdrepr = sdscatprintf(cmdrepr,"%ld.%ld ",(long)tv.tv_sec,(long)tv.tv_usec);
    if (dictid != 0) cmdrepr = sdscatprintf(cmdrepr,"(db %d) ", dictid);

    for (j = 0; j < argc; j++) {
        if (argv[j]->encoding == REDIS_ENCODING_INT) {
            cmdrepr = sdscatprintf(cmdrepr, "%ld", (long)argv[j]->ptr);
        } else {
            cmdrepr = sdscatrepr(cmdrepr,(char*)argv[j]->ptr,
                        sdslen(argv[j]->ptr));
        }
        if (j != argc-1)
            cmdrepr = sdscatlen(cmdrepr," ",1);
    }
    cmdrepr = sdscatlen(cmdrepr,"\r\n",2);
    cmdobj = createObject(REDIS_STRING,cmdrepr);

    listRewind(monitors,&li);
    while((ln = listNext(&li))) {
        redisClient *monitor = ln->value;
        addReply(monitor,cmdobj);
    }
    decrRefCount(cmdobj);
}

static void processInputBuffer(redisClient *c) {
again:
    /* Before to process the input buffer, make sure the client is not
     * waitig for a blocking operation such as BLPOP. Note that the first
     * iteration the client is never blocked, otherwise the processInputBuffer
     * would not be called at all, but after the execution of the first commands
     * in the input buffer the client may be blocked, and the "goto again"
     * will try to reiterate. The following line will make it return asap. */
    if (c->flags & REDIS_BLOCKED || c->flags & REDIS_IO_WAIT) return;
    if (c->bulklen == -1) {
        /* Read the first line of the query */
        char *p = strchr(c->querybuf,'\n');
        size_t querylen;

        if (p) {
            sds query, *argv;
            int argc, j;

            query = c->querybuf;
            c->querybuf = sdsempty();
            querylen = 1+(p-(query));
            if (sdslen(query) > querylen) {
                /* leave data after the first line of the query in the buffer */
                c->querybuf = sdscatlen(c->querybuf,query+querylen,sdslen(query)-querylen);
            }
            *p = '\0'; /* remove "\n" */
            if (*(p-1) == '\r') *(p-1) = '\0'; /* and "\r" if any */
            sdsupdatelen(query);

            /* Now we can split the query in arguments */
            argv = sdssplitlen(query,sdslen(query)," ",1,&argc);
            sdsfree(query);

            if (c->argv) zfree(c->argv);
            c->argv = zmalloc(sizeof(robj*)*argc);

            for (j = 0; j < argc; j++) {
                if (sdslen(argv[j])) {
                    c->argv[c->argc] = createObject(REDIS_STRING,argv[j]);
                    c->argc++;
                } else {
                    sdsfree(argv[j]);
                }
            }
            zfree(argv);
            if (c->argc) {
                /* Execute the command. If the client is still valid
                 * after processCommand() return and there is something
                 * on the query buffer try to process the next command. */
                if (processCommand(c) && sdslen(c->querybuf)) goto again;
            } else {
                /* Nothing to process, argc == 0. Just process the query
                 * buffer if it's not empty or return to the caller */
                if (sdslen(c->querybuf)) goto again;
            }
            return;
        } else if (sdslen(c->querybuf) >= REDIS_REQUEST_MAX_SIZE) {
            redisLog(REDIS_VERBOSE, "Client protocol error");
            freeClient(c);
            return;
        }
    } else {
        /* Bulk read handling. Note that if we are at this point
           the client already sent a command terminated with a newline,
           we are reading the bulk data that is actually the last
           argument of the command. */
        int qbl = sdslen(c->querybuf);

        if (c->bulklen <= qbl) {
            /* Copy everything but the final CRLF as final argument */
            c->argv[c->argc] = createStringObject(c->querybuf,c->bulklen-2);
            c->argc++;
            c->querybuf = sdsrange(c->querybuf,c->bulklen,-1);
            /* Process the command. If the client is still valid after
             * the processing and there is more data in the buffer
             * try to parse it. */
            if (processCommand(c) && sdslen(c->querybuf)) goto again;
            return;
        }
    }
}

static void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = (redisClient*) privdata;
    char buf[REDIS_IOBUF_LEN];
    int nread;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    nread = read(fd, buf, REDIS_IOBUF_LEN);
    if (nread == -1) {
        if (errno == EAGAIN) {
            nread = 0;
        } else {
            redisLog(REDIS_VERBOSE, "Reading from client: %s",strerror(errno));
            freeClient(c);
            return;
        }
    } else if (nread == 0) {
        redisLog(REDIS_VERBOSE, "Client closed connection");
        freeClient(c);
        return;
    }
    if (nread) {
        c->querybuf = sdscatlen(c->querybuf, buf, nread);
        c->lastinteraction = time(NULL);
    } else {
        return;
    }
    processInputBuffer(c);
}

static int selectDb(redisClient *c, int id) {
    if (id < 0 || id >= server.dbnum)
        return REDIS_ERR;
    c->db = &server.db[id];
    return REDIS_OK;
}

static void *dupClientReplyValue(void *o) {
    incrRefCount((robj*)o);
    return o;
}

static int listMatchObjects(void *a, void *b) {
    return equalStringObjects(a,b);
}

static redisClient *createClient(int fd) {
    redisClient *c = zmalloc(sizeof(*c));

    anetNonBlock(NULL,fd);
    anetTcpNoDelay(NULL,fd);
    if (!c) return NULL;
    selectDb(c,0);
    c->fd = fd;
    c->querybuf = sdsempty();
    c->argc = 0;
    c->argv = NULL;
    c->bulklen = -1;
    c->multibulk = 0;
    c->mbargc = 0;
    c->mbargv = NULL;
    c->sentlen = 0;
    c->flags = 0;
    c->lastinteraction = time(NULL);
    c->authenticated = 0;
    c->replstate = REDIS_REPL_NONE;
    c->reply = listCreate();
    listSetFreeMethod(c->reply,decrRefCount);
    listSetDupMethod(c->reply,dupClientReplyValue);
    c->blocking_keys = NULL;
    c->blocking_keys_num = 0;
    c->io_keys = listCreate();
    c->watched_keys = listCreate();
    listSetFreeMethod(c->io_keys,decrRefCount);
    c->pubsub_channels = dictCreate(&setDictType,NULL);
    c->pubsub_patterns = listCreate();
    listSetFreeMethod(c->pubsub_patterns,decrRefCount);
    listSetMatchMethod(c->pubsub_patterns,listMatchObjects);
    if (aeCreateFileEvent(server.el, c->fd, AE_READABLE,
        readQueryFromClient, c) == AE_ERR) {
        freeClient(c);
        return NULL;
    }
    listAddNodeTail(server.clients,c);
    initClientMultiState(c);
    return c;
}

static void addReply(redisClient *c, robj *obj) {
    if (listLength(c->reply) == 0 &&
        (c->replstate == REDIS_REPL_NONE ||
         c->replstate == REDIS_REPL_ONLINE) &&
        aeCreateFileEvent(server.el, c->fd, AE_WRITABLE,
        sendReplyToClient, c) == AE_ERR) return;

    if (server.vm_enabled && obj->storage != REDIS_VM_MEMORY) {
        obj = dupStringObject(obj);
        obj->refcount = 0; /* getDecodedObject() will increment the refcount */
    }
    listAddNodeTail(c->reply,getDecodedObject(obj));
}

static void addReplySds(redisClient *c, sds s) {
    robj *o = createObject(REDIS_STRING,s);
    addReply(c,o);
    decrRefCount(o);
}

static void addReplyDouble(redisClient *c, double d) {
    char buf[128];

    snprintf(buf,sizeof(buf),"%.17g",d);
    addReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n%s\r\n",
        (unsigned long) strlen(buf),buf));
}

static void addReplyLongLong(redisClient *c, long long ll) {
    char buf[128];
    size_t len;

    if (ll == 0) {
        addReply(c,shared.czero);
        return;
    } else if (ll == 1) {
        addReply(c,shared.cone);
        return;
    }
    buf[0] = ':';
    len = ll2string(buf+1,sizeof(buf)-1,ll);
    buf[len+1] = '\r';
    buf[len+2] = '\n';
    addReplySds(c,sdsnewlen(buf,len+3));
}

static void addReplyUlong(redisClient *c, unsigned long ul) {
    char buf[128];
    size_t len;

    if (ul == 0) {
        addReply(c,shared.czero);
        return;
    } else if (ul == 1) {
        addReply(c,shared.cone);
        return;
    }
    len = snprintf(buf,sizeof(buf),":%lu\r\n",ul);
    addReplySds(c,sdsnewlen(buf,len));
}

static void addReplyBulkLen(redisClient *c, robj *obj) {
    size_t len, intlen;
    char buf[128];

    if (obj->encoding == REDIS_ENCODING_RAW) {
        len = sdslen(obj->ptr);
    } else {
        long n = (long)obj->ptr;

        /* Compute how many bytes will take this integer as a radix 10 string */
        len = 1;
        if (n < 0) {
            len++;
            n = -n;
        }
        while((n = n/10) != 0) {
            len++;
        }
    }
    buf[0] = '$';
    intlen = ll2string(buf+1,sizeof(buf)-1,(long long)len);
    buf[intlen+1] = '\r';
    buf[intlen+2] = '\n';
    addReplySds(c,sdsnewlen(buf,intlen+3));
}

static void addReplyBulk(redisClient *c, robj *obj) {
    addReplyBulkLen(c,obj);
    addReply(c,obj);
    addReply(c,shared.crlf);
}

/* In the CONFIG command we need to add vanilla C string as bulk replies */
static void addReplyBulkCString(redisClient *c, char *s) {
    if (s == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        robj *o = createStringObject(s,strlen(s));
        addReplyBulk(c,o);
        decrRefCount(o);
    }
}

static void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    char cip[128];
    redisClient *c;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);

    cfd = anetAccept(server.neterr, fd, cip, &cport);
    if (cfd == AE_ERR) {
        redisLog(REDIS_VERBOSE,"Accepting client connection: %s", server.neterr);
        return;
    }
    redisLog(REDIS_VERBOSE,"Accepted %s:%d", cip, cport);
    if ((c = createClient(cfd)) == NULL) {
        redisLog(REDIS_WARNING,"Error allocating resoures for the client");
        close(cfd); /* May be already closed, just ingore errors */
        return;
    }
    /* If maxclient directive is set and this is one client more... close the
     * connection. Note that we create the client instead to check before
     * for this condition, since now the socket is already set in nonblocking
     * mode and we can send an error for free using the Kernel I/O */
    if (server.maxclients && listLength(server.clients) > server.maxclients) {
        char *err = "-ERR max number of clients reached\r\n";

        /* That's a best effort error message, don't check write errors */
        if (write(c->fd,err,strlen(err)) == -1) {
            /* Nothing to do, Just to avoid the warning... */
        }
        freeClient(c);
        return;
    }
    server.stat_numconnections++;
}

/* ======================= Redis objects implementation ===================== */

static robj *createObject(int type, void *ptr) {
    robj *o;

    if (server.vm_enabled) pthread_mutex_lock(&server.obj_freelist_mutex);
    if (listLength(server.objfreelist)) {
        listNode *head = listFirst(server.objfreelist);
        o = listNodeValue(head);
        listDelNode(server.objfreelist,head);
        if (server.vm_enabled) pthread_mutex_unlock(&server.obj_freelist_mutex);
    } else {
        if (server.vm_enabled) {
            pthread_mutex_unlock(&server.obj_freelist_mutex);
            o = zmalloc(sizeof(*o));
        } else {
            o = zmalloc(sizeof(*o)-sizeof(struct redisObjectVM));
        }
    }
    o->type = type;
    o->encoding = REDIS_ENCODING_RAW;
    o->ptr = ptr;
    o->refcount = 1;
    if (server.vm_enabled) {
        /* Note that this code may run in the context of an I/O thread
         * and accessing to server.unixtime in theory is an error
         * (no locks). But in practice this is safe, and even if we read
         * garbage Redis will not fail, as it's just a statistical info */
        o->vm.atime = server.unixtime;
        o->storage = REDIS_VM_MEMORY;
    }
    return o;
}

static robj *createStringObject(char *ptr, size_t len) {
    return createObject(REDIS_STRING,sdsnewlen(ptr,len));
}

static robj *createStringObjectFromLongLong(long long value) {
    robj *o;
    if (value >= 0 && value < REDIS_SHARED_INTEGERS) {
        incrRefCount(shared.integers[value]);
        o = shared.integers[value];
    } else {
        if (value >= LONG_MIN && value <= LONG_MAX) {
            o = createObject(REDIS_STRING, NULL);
            o->encoding = REDIS_ENCODING_INT;
            o->ptr = (void*)((long)value);
        } else {
            o = createObject(REDIS_STRING,sdsfromlonglong(value));
        }
    }
    return o;
}

static robj *dupStringObject(robj *o) {
    assert(o->encoding == REDIS_ENCODING_RAW);
    return createStringObject(o->ptr,sdslen(o->ptr));
}

static robj *createListObject(void) {
    list *l = listCreate();

    listSetFreeMethod(l,decrRefCount);
    return createObject(REDIS_LIST,l);
}

static robj *createSetObject(void) {
    dict *d = dictCreate(&setDictType,NULL);
    return createObject(REDIS_SET,d);
}

static robj *createHashObject(void) {
    /* All the Hashes start as zipmaps. Will be automatically converted
     * into hash tables if there are enough elements or big elements
     * inside. */
    unsigned char *zm = zipmapNew();
    robj *o = createObject(REDIS_HASH,zm);
    o->encoding = REDIS_ENCODING_ZIPMAP;
    return o;
}

static robj *createZsetObject(void) {
    zset *zs = zmalloc(sizeof(*zs));

    zs->dict = dictCreate(&zsetDictType,NULL);
    zs->zsl = zslCreate();
    return createObject(REDIS_ZSET,zs);
}

static void freeStringObject(robj *o) {
    if (o->encoding == REDIS_ENCODING_RAW) {
        sdsfree(o->ptr);
    }
}

static void freeListObject(robj *o) {
    listRelease((list*) o->ptr);
}

static void freeSetObject(robj *o) {
    dictRelease((dict*) o->ptr);
}

static void freeZsetObject(robj *o) {
    zset *zs = o->ptr;

    dictRelease(zs->dict);
    zslFree(zs->zsl);
    zfree(zs);
}

static void freeHashObject(robj *o) {
    switch (o->encoding) {
    case REDIS_ENCODING_HT:
        dictRelease((dict*) o->ptr);
        break;
    case REDIS_ENCODING_ZIPMAP:
        zfree(o->ptr);
        break;
    default:
        redisPanic("Unknown hash encoding type");
        break;
    }
}

static void incrRefCount(robj *o) {
    o->refcount++;
}

static void decrRefCount(void *obj) {
    robj *o = obj;

    if (o->refcount <= 0) redisPanic("decrRefCount against refcount <= 0");
    /* Object is a key of a swapped out value, or in the process of being
     * loaded. */
    if (server.vm_enabled &&
        (o->storage == REDIS_VM_SWAPPED || o->storage == REDIS_VM_LOADING))
    {
        if (o->storage == REDIS_VM_LOADING) vmCancelThreadedIOJob(obj);
        redisAssert(o->type == REDIS_STRING);
        freeStringObject(o);
        vmMarkPagesFree(o->vm.page,o->vm.usedpages);
        pthread_mutex_lock(&server.obj_freelist_mutex);
        if (listLength(server.objfreelist) > REDIS_OBJFREELIST_MAX ||
            !listAddNodeHead(server.objfreelist,o))
            zfree(o);
        pthread_mutex_unlock(&server.obj_freelist_mutex);
        server.vm_stats_swapped_objects--;
        return;
    }
    /* Object is in memory, or in the process of being swapped out. */
    if (--(o->refcount) == 0) {
        if (server.vm_enabled && o->storage == REDIS_VM_SWAPPING)
            vmCancelThreadedIOJob(obj);
        switch(o->type) {
        case REDIS_STRING: freeStringObject(o); break;
        case REDIS_LIST: freeListObject(o); break;
        case REDIS_SET: freeSetObject(o); break;
        case REDIS_ZSET: freeZsetObject(o); break;
        case REDIS_HASH: freeHashObject(o); break;
        default: redisPanic("Unknown object type"); break;
        }
        if (server.vm_enabled) pthread_mutex_lock(&server.obj_freelist_mutex);
        if (listLength(server.objfreelist) > REDIS_OBJFREELIST_MAX ||
            !listAddNodeHead(server.objfreelist,o))
            zfree(o);
        if (server.vm_enabled) pthread_mutex_unlock(&server.obj_freelist_mutex);
    }
}

static robj *lookupKey(redisDb *db, robj *key) {
    dictEntry *de = dictFind(db->dict,key);
    if (de) {
        robj *key = dictGetEntryKey(de);
        robj *val = dictGetEntryVal(de);

        if (server.vm_enabled) {
            if (key->storage == REDIS_VM_MEMORY ||
                key->storage == REDIS_VM_SWAPPING)
            {
                /* If we were swapping the object out, stop it, this key
                 * was requested. */
                if (key->storage == REDIS_VM_SWAPPING)
                    vmCancelThreadedIOJob(key);
                /* Update the access time of the key for the aging algorithm. */
                key->vm.atime = server.unixtime;
            } else {
                int notify = (key->storage == REDIS_VM_LOADING);

                /* Our value was swapped on disk. Bring it at home. */
                redisAssert(val == NULL);
                val = vmLoadObject(key);
                dictGetEntryVal(de) = val;

                /* Clients blocked by the VM subsystem may be waiting for
                 * this key... */
                if (notify) handleClientsBlockedOnSwappedKey(db,key);
            }
        }
        return val;
    } else {
        return NULL;
    }
}

static robj *lookupKeyRead(redisDb *db, robj *key) {
    expireIfNeeded(db,key);
    return lookupKey(db,key);
}

static robj *lookupKeyWrite(redisDb *db, robj *key) {
    deleteIfVolatile(db,key);
    touchWatchedKey(db,key);
    return lookupKey(db,key);
}

static robj *lookupKeyReadOrReply(redisClient *c, robj *key, robj *reply) {
    robj *o = lookupKeyRead(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

static robj *lookupKeyWriteOrReply(redisClient *c, robj *key, robj *reply) {
    robj *o = lookupKeyWrite(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

static int checkType(redisClient *c, robj *o, int type) {
    if (o->type != type) {
        addReply(c,shared.wrongtypeerr);
        return 1;
    }
    return 0;
}

static int deleteKey(redisDb *db, robj *key) {
    int retval;

    /* We need to protect key from destruction: after the first dictDelete()
     * it may happen that 'key' is no longer valid if we don't increment
     * it's count. This may happen when we get the object reference directly
     * from the hash table with dictRandomKey() or dict iterators */
    incrRefCount(key);
    if (dictSize(db->expires)) dictDelete(db->expires,key);
    retval = dictDelete(db->dict,key);
    decrRefCount(key);

    return retval == DICT_OK;
}

/* Check if the nul-terminated string 's' can be represented by a long
 * (that is, is a number that fits into long without any other space or
 * character before or after the digits).
 *
 * If so, the function returns REDIS_OK and *longval is set to the value
 * of the number. Otherwise REDIS_ERR is returned */
static int isStringRepresentableAsLong(sds s, long *longval) {
    char buf[32], *endptr;
    long value;
    int slen;

    value = strtol(s, &endptr, 10);
    if (endptr[0] != '\0') return REDIS_ERR;
    slen = ll2string(buf,32,value);

    /* If the number converted back into a string is not identical
     * then it's not possible to encode the string as integer */
    if (sdslen(s) != (unsigned)slen || memcmp(buf,s,slen)) return REDIS_ERR;
    if (longval) *longval = value;
    return REDIS_OK;
}

/* Try to encode a string object in order to save space */
static robj *tryObjectEncoding(robj *o) {
    long value;
    sds s = o->ptr;

    if (o->encoding != REDIS_ENCODING_RAW)
        return o; /* Already encoded */

    /* It's not safe to encode shared objects: shared objects can be shared
     * everywhere in the "object space" of Redis. Encoded objects can only
     * appear as "values" (and not, for instance, as keys) */
     if (o->refcount > 1) return o;

    /* Currently we try to encode only strings */
    redisAssert(o->type == REDIS_STRING);

    /* Check if we can represent this string as a long integer */
    if (isStringRepresentableAsLong(s,&value) == REDIS_ERR) return o;

    /* Ok, this object can be encoded */
    if (value >= 0 && value < REDIS_SHARED_INTEGERS) {
        decrRefCount(o);
        incrRefCount(shared.integers[value]);
        return shared.integers[value];
    } else {
        o->encoding = REDIS_ENCODING_INT;
        sdsfree(o->ptr);
        o->ptr = (void*) value;
        return o;
    }
}

/* Get a decoded version of an encoded object (returned as a new object).
 * If the object is already raw-encoded just increment the ref count. */
static robj *getDecodedObject(robj *o) {
    robj *dec;

    if (o->encoding == REDIS_ENCODING_RAW) {
        incrRefCount(o);
        return o;
    }
    if (o->type == REDIS_STRING && o->encoding == REDIS_ENCODING_INT) {
        char buf[32];

        ll2string(buf,32,(long)o->ptr);
        dec = createStringObject(buf,strlen(buf));
        return dec;
    } else {
        redisPanic("Unknown encoding type");
    }
}

/* Compare two string objects via strcmp() or alike.
 * Note that the objects may be integer-encoded. In such a case we
 * use ll2string() to get a string representation of the numbers on the stack
 * and compare the strings, it's much faster than calling getDecodedObject().
 *
 * Important note: if objects are not integer encoded, but binary-safe strings,
 * sdscmp() from sds.c will apply memcmp() so this function ca be considered
 * binary safe. */
static int compareStringObjects(robj *a, robj *b) {
    redisAssert(a->type == REDIS_STRING && b->type == REDIS_STRING);
    char bufa[128], bufb[128], *astr, *bstr;
    int bothsds = 1;

    if (a == b) return 0;
    if (a->encoding != REDIS_ENCODING_RAW) {
        ll2string(bufa,sizeof(bufa),(long) a->ptr);
        astr = bufa;
        bothsds = 0;
    } else {
        astr = a->ptr;
    }
    if (b->encoding != REDIS_ENCODING_RAW) {
        ll2string(bufb,sizeof(bufb),(long) b->ptr);
        bstr = bufb;
        bothsds = 0;
    } else {
        bstr = b->ptr;
    }
    return bothsds ? sdscmp(astr,bstr) : strcmp(astr,bstr);
}

/* Equal string objects return 1 if the two objects are the same from the
 * point of view of a string comparison, otherwise 0 is returned. Note that
 * this function is faster then checking for (compareStringObject(a,b) == 0)
 * because it can perform some more optimization. */
static int equalStringObjects(robj *a, robj *b) {
    if (a->encoding != REDIS_ENCODING_RAW && b->encoding != REDIS_ENCODING_RAW){
        return a->ptr == b->ptr;
    } else {
        return compareStringObjects(a,b) == 0;
    }
}

static size_t stringObjectLen(robj *o) {
    redisAssert(o->type == REDIS_STRING);
    if (o->encoding == REDIS_ENCODING_RAW) {
        return sdslen(o->ptr);
    } else {
        char buf[32];

        return ll2string(buf,32,(long)o->ptr);
    }
}

static int getDoubleFromObject(robj *o, double *target) {
    double value;
    char *eptr;

    if (o == NULL) {
        value = 0;
    } else {
        redisAssert(o->type == REDIS_STRING);
        if (o->encoding == REDIS_ENCODING_RAW) {
            value = strtod(o->ptr, &eptr);
            if (eptr[0] != '\0') return REDIS_ERR;
        } else if (o->encoding == REDIS_ENCODING_INT) {
            value = (long)o->ptr;
        } else {
            redisPanic("Unknown string encoding");
        }
    }

    *target = value;
    return REDIS_OK;
}

static int getDoubleFromObjectOrReply(redisClient *c, robj *o, double *target, const char *msg) {
    double value;
    if (getDoubleFromObject(o, &value) != REDIS_OK) {
        if (msg != NULL) {
            addReplySds(c, sdscatprintf(sdsempty(), "-ERR %s\r\n", msg));
        } else {
            addReplySds(c, sdsnew("-ERR value is not a double\r\n"));
        }
        return REDIS_ERR;
    }

    *target = value;
    return REDIS_OK;
}

static int getLongLongFromObject(robj *o, long long *target) {
    long long value;
    char *eptr;

    if (o == NULL) {
        value = 0;
    } else {
        redisAssert(o->type == REDIS_STRING);
        if (o->encoding == REDIS_ENCODING_RAW) {
            value = strtoll(o->ptr, &eptr, 10);
            if (eptr[0] != '\0') return REDIS_ERR;
        } else if (o->encoding == REDIS_ENCODING_INT) {
            value = (long)o->ptr;
        } else {
            redisPanic("Unknown string encoding");
        }
    }

    *target = value;
    return REDIS_OK;
}

static int getLongLongFromObjectOrReply(redisClient *c, robj *o, long long *target, const char *msg) {
    long long value;
    if (getLongLongFromObject(o, &value) != REDIS_OK) {
        if (msg != NULL) {
            addReplySds(c, sdscatprintf(sdsempty(), "-ERR %s\r\n", msg));
        } else {
            addReplySds(c, sdsnew("-ERR value is not an integer\r\n"));
        }
        return REDIS_ERR;
    }

    *target = value;
    return REDIS_OK;
}

static int getLongFromObjectOrReply(redisClient *c, robj *o, long *target, const char *msg) {
    long long value;

    if (getLongLongFromObjectOrReply(c, o, &value, msg) != REDIS_OK) return REDIS_ERR;
    if (value < LONG_MIN || value > LONG_MAX) {
        if (msg != NULL) {
            addReplySds(c, sdscatprintf(sdsempty(), "-ERR %s\r\n", msg));
        } else {
            addReplySds(c, sdsnew("-ERR value is out of range\r\n"));
        }
        return REDIS_ERR;
    }

    *target = value;
    return REDIS_OK;
}

/*============================ RDB saving/loading =========================== */

static int rdbSaveType(FILE *fp, unsigned char type) {
    if (fwrite(&type,1,1,fp) == 0) return -1;
    return 0;
}

static int rdbSaveTime(FILE *fp, time_t t) {
    int32_t t32 = (int32_t) t;
    if (fwrite(&t32,4,1,fp) == 0) return -1;
    return 0;
}

/* check rdbLoadLen() comments for more info */
static int rdbSaveLen(FILE *fp, uint32_t len) {
    unsigned char buf[2];

    if (len < (1<<6)) {
        /* Save a 6 bit len */
        buf[0] = (len&0xFF)|(REDIS_RDB_6BITLEN<<6);
        if (fwrite(buf,1,1,fp) == 0) return -1;
    } else if (len < (1<<14)) {
        /* Save a 14 bit len */
        buf[0] = ((len>>8)&0xFF)|(REDIS_RDB_14BITLEN<<6);
        buf[1] = len&0xFF;
        if (fwrite(buf,2,1,fp) == 0) return -1;
    } else {
        /* Save a 32 bit len */
        buf[0] = (REDIS_RDB_32BITLEN<<6);
        if (fwrite(buf,1,1,fp) == 0) return -1;
        len = htonl(len);
        if (fwrite(&len,4,1,fp) == 0) return -1;
    }
    return 0;
}

/* Encode 'value' as an integer if possible (if integer will fit the
 * supported range). If the function sucessful encoded the integer
 * then the (up to 5 bytes) encoded representation is written in the
 * string pointed by 'enc' and the length is returned. Otherwise
 * 0 is returned. */
static int rdbEncodeInteger(long long value, unsigned char *enc) {
    /* Finally check if it fits in our ranges */
    if (value >= -(1<<7) && value <= (1<<7)-1) {
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT8;
        enc[1] = value&0xFF;
        return 2;
    } else if (value >= -(1<<15) && value <= (1<<15)-1) {
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT16;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        return 3;
    } else if (value >= -((long long)1<<31) && value <= ((long long)1<<31)-1) {
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT32;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        enc[3] = (value>>16)&0xFF;
        enc[4] = (value>>24)&0xFF;
        return 5;
    } else {
        return 0;
    }
}

/* String objects in the form "2391" "-100" without any space and with a
 * range of values that can fit in an 8, 16 or 32 bit signed value can be
 * encoded as integers to save space */
static int rdbTryIntegerEncoding(char *s, size_t len, unsigned char *enc) {
    long long value;
    char *endptr, buf[32];

    /* Check if it's possible to encode this value as a number */
    value = strtoll(s, &endptr, 10);
    if (endptr[0] != '\0') return 0;
    ll2string(buf,32,value);

    /* If the number converted back into a string is not identical
     * then it's not possible to encode the string as integer */
    if (strlen(buf) != len || memcmp(buf,s,len)) return 0;

    return rdbEncodeInteger(value,enc);
}

static int rdbSaveLzfStringObject(FILE *fp, unsigned char *s, size_t len) {
    size_t comprlen, outlen;
    unsigned char byte;
    void *out;

    /* We require at least four bytes compression for this to be worth it */
    if (len <= 4) return 0;
    outlen = len-4;
    if ((out = zmalloc(outlen+1)) == NULL) return 0;
    comprlen = lzf_compress(s, len, out, outlen);
    if (comprlen == 0) {
        zfree(out);
        return 0;
    }
    /* Data compressed! Let's save it on disk */
    byte = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_LZF;
    if (fwrite(&byte,1,1,fp) == 0) goto writeerr;
    if (rdbSaveLen(fp,comprlen) == -1) goto writeerr;
    if (rdbSaveLen(fp,len) == -1) goto writeerr;
    if (fwrite(out,comprlen,1,fp) == 0) goto writeerr;
    zfree(out);
    return comprlen;

writeerr:
    zfree(out);
    return -1;
}

/* Save a string objet as [len][data] on disk. If the object is a string
 * representation of an integer value we try to safe it in a special form */
static int rdbSaveRawString(FILE *fp, unsigned char *s, size_t len) {
    int enclen;

    /* Try integer encoding */
    if (len <= 11) {
        unsigned char buf[5];
        if ((enclen = rdbTryIntegerEncoding((char*)s,len,buf)) > 0) {
            if (fwrite(buf,enclen,1,fp) == 0) return -1;
            return 0;
        }
    }

    /* Try LZF compression - under 20 bytes it's unable to compress even
     * aaaaaaaaaaaaaaaaaa so skip it */
    if (server.rdbcompression && len > 20) {
        int retval;

        retval = rdbSaveLzfStringObject(fp,s,len);
        if (retval == -1) return -1;
        if (retval > 0) return 0;
        /* retval == 0 means data can't be compressed, save the old way */
    }

    /* Store verbatim */
    if (rdbSaveLen(fp,len) == -1) return -1;
    if (len && fwrite(s,len,1,fp) == 0) return -1;
    return 0;
}

/* Like rdbSaveStringObjectRaw() but handle encoded objects */
static int rdbSaveStringObject(FILE *fp, robj *obj) {
    int retval;

    /* Avoid to decode the object, then encode it again, if the
     * object is alrady integer encoded. */
    if (obj->encoding == REDIS_ENCODING_INT) {
        long val = (long) obj->ptr;
        unsigned char buf[5];
        int enclen;

        if ((enclen = rdbEncodeInteger(val,buf)) > 0) {
            if (fwrite(buf,enclen,1,fp) == 0) return -1;
            return 0;
        }
        /* otherwise... fall throught and continue with the usual
         * code path. */
    }

    /* Avoid incr/decr ref count business when possible.
     * This plays well with copy-on-write given that we are probably
     * in a child process (BGSAVE). Also this makes sure key objects
     * of swapped objects are not incRefCount-ed (an assert does not allow
     * this in order to avoid bugs) */
    if (obj->encoding != REDIS_ENCODING_RAW) {
        obj = getDecodedObject(obj);
        retval = rdbSaveRawString(fp,obj->ptr,sdslen(obj->ptr));
        decrRefCount(obj);
    } else {
        retval = rdbSaveRawString(fp,obj->ptr,sdslen(obj->ptr));
    }
    return retval;
}

/* Save a double value. Doubles are saved as strings prefixed by an unsigned
 * 8 bit integer specifing the length of the representation.
 * This 8 bit integer has special values in order to specify the following
 * conditions:
 * 253: not a number
 * 254: + inf
 * 255: - inf
 */
static int rdbSaveDoubleValue(FILE *fp, double val) {
    unsigned char buf[128];
    int len;

    if (isnan(val)) {
        buf[0] = 253;
        len = 1;
    } else if (!isfinite(val)) {
        len = 1;
        buf[0] = (val < 0) ? 255 : 254;
    } else {
#if (DBL_MANT_DIG >= 52) && (LLONG_MAX == 0x7fffffffffffffffLL)
        /* Check if the float is in a safe range to be casted into a
         * long long. We are assuming that long long is 64 bit here.
         * Also we are assuming that there are no implementations around where
         * double has precision < 52 bit.
         *
         * Under this assumptions we test if a double is inside an interval
         * where casting to long long is safe. Then using two castings we
         * make sure the decimal part is zero. If all this is true we use
         * integer printing function that is much faster. */
        double min = -4503599627370495; /* (2^52)-1 */
        double max = 4503599627370496; /* -(2^52) */
        if (val > min && val < max && val == ((double)((long long)val)))
            ll2string((char*)buf+1,sizeof(buf),(long long)val);
        else
#endif
            snprintf((char*)buf+1,sizeof(buf)-1,"%.17g",val);
        buf[0] = strlen((char*)buf+1);
        len = buf[0]+1;
    }
    if (fwrite(buf,len,1,fp) == 0) return -1;
    return 0;
}

/* Save a Redis object. */
static int rdbSaveObject(FILE *fp, robj *o) {
    if (o->type == REDIS_STRING) {
        /* Save a string value */
        if (rdbSaveStringObject(fp,o) == -1) return -1;
    } else if (o->type == REDIS_LIST) {
        /* Save a list value */
        list *list = o->ptr;
        listIter li;
        listNode *ln;

        if (rdbSaveLen(fp,listLength(list)) == -1) return -1;
        listRewind(list,&li);
        while((ln = listNext(&li))) {
            robj *eleobj = listNodeValue(ln);

            if (rdbSaveStringObject(fp,eleobj) == -1) return -1;
        }
    } else if (o->type == REDIS_SET) {
        /* Save a set value */
        dict *set = o->ptr;
        dictIterator *di = dictGetIterator(set);
        dictEntry *de;

        if (rdbSaveLen(fp,dictSize(set)) == -1) return -1;
        while((de = dictNext(di)) != NULL) {
            robj *eleobj = dictGetEntryKey(de);

            if (rdbSaveStringObject(fp,eleobj) == -1) return -1;
        }
        dictReleaseIterator(di);
    } else if (o->type == REDIS_ZSET) {
        /* Save a set value */
        zset *zs = o->ptr;
        dictIterator *di = dictGetIterator(zs->dict);
        dictEntry *de;

        if (rdbSaveLen(fp,dictSize(zs->dict)) == -1) return -1;
        while((de = dictNext(di)) != NULL) {
            robj *eleobj = dictGetEntryKey(de);
            double *score = dictGetEntryVal(de);

            if (rdbSaveStringObject(fp,eleobj) == -1) return -1;
            if (rdbSaveDoubleValue(fp,*score) == -1) return -1;
        }
        dictReleaseIterator(di);
    } else if (o->type == REDIS_HASH) {
        /* Save a hash value */
        if (o->encoding == REDIS_ENCODING_ZIPMAP) {
            unsigned char *p = zipmapRewind(o->ptr);
            unsigned int count = zipmapLen(o->ptr);
            unsigned char *key, *val;
            unsigned int klen, vlen;

            if (rdbSaveLen(fp,count) == -1) return -1;
            while((p = zipmapNext(p,&key,&klen,&val,&vlen)) != NULL) {
                if (rdbSaveRawString(fp,key,klen) == -1) return -1;
                if (rdbSaveRawString(fp,val,vlen) == -1) return -1;
            }
        } else {
            dictIterator *di = dictGetIterator(o->ptr);
            dictEntry *de;

            if (rdbSaveLen(fp,dictSize((dict*)o->ptr)) == -1) return -1;
            while((de = dictNext(di)) != NULL) {
                robj *key = dictGetEntryKey(de);
                robj *val = dictGetEntryVal(de);

                if (rdbSaveStringObject(fp,key) == -1) return -1;
                if (rdbSaveStringObject(fp,val) == -1) return -1;
            }
            dictReleaseIterator(di);
        }
    } else {
        redisPanic("Unknown object type");
    }
    return 0;
}

/* Return the length the object will have on disk if saved with
 * the rdbSaveObject() function. Currently we use a trick to get
 * this length with very little changes to the code. In the future
 * we could switch to a faster solution. */
static off_t rdbSavedObjectLen(robj *o, FILE *fp) {
    if (fp == NULL) fp = server.devnull;
    rewind(fp);
    assert(rdbSaveObject(fp,o) != 1);
    return ftello(fp);
}

/* Return the number of pages required to save this object in the swap file */
static off_t rdbSavedObjectPages(robj *o, FILE *fp) {
    off_t bytes = rdbSavedObjectLen(o,fp);

    return (bytes+(server.vm_page_size-1))/server.vm_page_size;
}

/* Save the DB on disk. Return REDIS_ERR on error, REDIS_OK on success */
static int rdbSave(char *filename) {
    dictIterator *di = NULL;
    dictEntry *de;
    FILE *fp;
    char tmpfile[256];
    int j;
    time_t now = time(NULL);

    /* Wait for I/O therads to terminate, just in case this is a
     * foreground-saving, to avoid seeking the swap file descriptor at the
     * same time. */
    if (server.vm_enabled)
        waitEmptyIOJobsQueue();

    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());
    fp = fopen(tmpfile,"w");
    if (!fp) {
        redisLog(REDIS_WARNING, "Failed saving the DB: %s", strerror(errno));
        return REDIS_ERR;
    }
    if (fwrite("REDIS0001",9,1,fp) == 0) goto werr;
    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetIterator(d);
        if (!di) {
            fclose(fp);
            return REDIS_ERR;
        }

        /* Write the SELECT DB opcode */
        if (rdbSaveType(fp,REDIS_SELECTDB) == -1) goto werr;
        if (rdbSaveLen(fp,j) == -1) goto werr;

        /* Iterate this DB writing every entry */
        while((de = dictNext(di)) != NULL) {
            robj *key = dictGetEntryKey(de);
            robj *o = dictGetEntryVal(de);
            time_t expiretime = getExpire(db,key);

            /* Save the expire time */
            if (expiretime != -1) {
                /* If this key is already expired skip it */
                if (expiretime < now) continue;
                if (rdbSaveType(fp,REDIS_EXPIRETIME) == -1) goto werr;
                if (rdbSaveTime(fp,expiretime) == -1) goto werr;
            }
            /* Save the key and associated value. This requires special
             * handling if the value is swapped out. */
            if (!server.vm_enabled || key->storage == REDIS_VM_MEMORY ||
                                      key->storage == REDIS_VM_SWAPPING) {
                /* Save type, key, value */
                if (rdbSaveType(fp,o->type) == -1) goto werr;
                if (rdbSaveStringObject(fp,key) == -1) goto werr;
                if (rdbSaveObject(fp,o) == -1) goto werr;
            } else {
                /* REDIS_VM_SWAPPED or REDIS_VM_LOADING */
                robj *po;
                /* Get a preview of the object in memory */
                po = vmPreviewObject(key);
                /* Save type, key, value */
                if (rdbSaveType(fp,key->vtype) == -1) goto werr;
                if (rdbSaveStringObject(fp,key) == -1) goto werr;
                if (rdbSaveObject(fp,po) == -1) goto werr;
                /* Remove the loaded object from memory */
                decrRefCount(po);
            }
        }
        dictReleaseIterator(di);
    }
    /* EOF opcode */
    if (rdbSaveType(fp,REDIS_EOF) == -1) goto werr;

    /* Make sure data will not remain on the OS's output buffers */
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    if (rename(tmpfile,filename) == -1) {
        redisLog(REDIS_WARNING,"Error moving temp DB file on the final destination: %s", strerror(errno));
        unlink(tmpfile);
        return REDIS_ERR;
    }
    redisLog(REDIS_NOTICE,"DB saved on disk");
    server.dirty = 0;
    server.lastsave = time(NULL);
    return REDIS_OK;

werr:
    fclose(fp);
    unlink(tmpfile);
    redisLog(REDIS_WARNING,"Write error saving DB on disk: %s", strerror(errno));
    if (di) dictReleaseIterator(di);
    return REDIS_ERR;
}

static int rdbSaveBackground(char *filename) {
    pid_t childpid;

    if (server.bgsavechildpid != -1) return REDIS_ERR;
    if (server.vm_enabled) waitEmptyIOJobsQueue();
    if ((childpid = fork()) == 0) {
        /* Child */
        if (server.vm_enabled) vmReopenSwapFile();
        close(server.fd);
        if (rdbSave(filename) == REDIS_OK) {
            _exit(0);
        } else {
            _exit(1);
        }
    } else {
        /* Parent */
        if (childpid == -1) {
            redisLog(REDIS_WARNING,"Can't save in background: fork: %s",
                strerror(errno));
            return REDIS_ERR;
        }
        redisLog(REDIS_NOTICE,"Background saving started by pid %d",childpid);
        server.bgsavechildpid = childpid;
        updateDictResizePolicy();
        return REDIS_OK;
    }
    return REDIS_OK; /* unreached */
}

static void rdbRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,256,"temp-%d.rdb", (int) childpid);
    unlink(tmpfile);
}

static int rdbLoadType(FILE *fp) {
    unsigned char type;
    if (fread(&type,1,1,fp) == 0) return -1;
    return type;
}

static time_t rdbLoadTime(FILE *fp) {
    int32_t t32;
    if (fread(&t32,4,1,fp) == 0) return -1;
    return (time_t) t32;
}

/* Load an encoded length from the DB, see the REDIS_RDB_* defines on the top
 * of this file for a description of how this are stored on disk.
 *
 * isencoded is set to 1 if the readed length is not actually a length but
 * an "encoding type", check the above comments for more info */
static uint32_t rdbLoadLen(FILE *fp, int *isencoded) {
    unsigned char buf[2];
    uint32_t len;
    int type;

    if (isencoded) *isencoded = 0;
    if (fread(buf,1,1,fp) == 0) return REDIS_RDB_LENERR;
    type = (buf[0]&0xC0)>>6;
    if (type == REDIS_RDB_6BITLEN) {
        /* Read a 6 bit len */
        return buf[0]&0x3F;
    } else if (type == REDIS_RDB_ENCVAL) {
        /* Read a 6 bit len encoding type */
        if (isencoded) *isencoded = 1;
        return buf[0]&0x3F;
    } else if (type == REDIS_RDB_14BITLEN) {
        /* Read a 14 bit len */
        if (fread(buf+1,1,1,fp) == 0) return REDIS_RDB_LENERR;
        return ((buf[0]&0x3F)<<8)|buf[1];
    } else {
        /* Read a 32 bit len */
        if (fread(&len,4,1,fp) == 0) return REDIS_RDB_LENERR;
        return ntohl(len);
    }
}

/* Load an integer-encoded object from file 'fp', with the specified
 * encoding type 'enctype'. If encode is true the function may return
 * an integer-encoded object as reply, otherwise the returned object
 * will always be encoded as a raw string. */
static robj *rdbLoadIntegerObject(FILE *fp, int enctype, int encode) {
    unsigned char enc[4];
    long long val;

    if (enctype == REDIS_RDB_ENC_INT8) {
        if (fread(enc,1,1,fp) == 0) return NULL;
        val = (signed char)enc[0];
    } else if (enctype == REDIS_RDB_ENC_INT16) {
        uint16_t v;
        if (fread(enc,2,1,fp) == 0) return NULL;
        v = enc[0]|(enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == REDIS_RDB_ENC_INT32) {
        uint32_t v;
        if (fread(enc,4,1,fp) == 0) return NULL;
        v = enc[0]|(enc[1]<<8)|(enc[2]<<16)|(enc[3]<<24);
        val = (int32_t)v;
    } else {
        val = 0; /* anti-warning */
        redisPanic("Unknown RDB integer encoding type");
    }
    if (encode)
        return createStringObjectFromLongLong(val);
    else
        return createObject(REDIS_STRING,sdsfromlonglong(val));
}

static robj *rdbLoadLzfStringObject(FILE*fp) {
    unsigned int len, clen;
    unsigned char *c = NULL;
    sds val = NULL;

    if ((clen = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;
    if ((len = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;
    if ((c = zmalloc(clen)) == NULL) goto err;
    if ((val = sdsnewlen(NULL,len)) == NULL) goto err;
    if (fread(c,clen,1,fp) == 0) goto err;
    if (lzf_decompress(c,clen,val,len) == 0) goto err;
    zfree(c);
    return createObject(REDIS_STRING,val);
err:
    zfree(c);
    sdsfree(val);
    return NULL;
}

static robj *rdbGenericLoadStringObject(FILE*fp, int encode) {
    int isencoded;
    uint32_t len;
    sds val;

    len = rdbLoadLen(fp,&isencoded);
    if (isencoded) {
        switch(len) {
        case REDIS_RDB_ENC_INT8:
        case REDIS_RDB_ENC_INT16:
        case REDIS_RDB_ENC_INT32:
            return rdbLoadIntegerObject(fp,len,encode);
        case REDIS_RDB_ENC_LZF:
            return rdbLoadLzfStringObject(fp);
        default:
            redisPanic("Unknown RDB encoding type");
        }
    }

    if (len == REDIS_RDB_LENERR) return NULL;
    val = sdsnewlen(NULL,len);
    if (len && fread(val,len,1,fp) == 0) {
        sdsfree(val);
        return NULL;
    }
    return createObject(REDIS_STRING,val);
}

static robj *rdbLoadStringObject(FILE *fp) {
    return rdbGenericLoadStringObject(fp,0);
}

static robj *rdbLoadEncodedStringObject(FILE *fp) {
    return rdbGenericLoadStringObject(fp,1);
}

/* For information about double serialization check rdbSaveDoubleValue() */
static int rdbLoadDoubleValue(FILE *fp, double *val) {
    char buf[128];
    unsigned char len;

    if (fread(&len,1,1,fp) == 0) return -1;
    switch(len) {
    case 255: *val = R_NegInf; return 0;
    case 254: *val = R_PosInf; return 0;
    case 253: *val = R_Nan; return 0;
    default:
        if (fread(buf,len,1,fp) == 0) return -1;
        buf[len] = '\0';
        sscanf(buf, "%lg", val);
        return 0;
    }
}

/* Load a Redis object of the specified type from the specified file.
 * On success a newly allocated object is returned, otherwise NULL. */
static robj *rdbLoadObject(int type, FILE *fp) {
    robj *o;

    redisLog(REDIS_DEBUG,"LOADING OBJECT %d (at %d)\n",type,ftell(fp));
    if (type == REDIS_STRING) {
        /* Read string value */
        if ((o = rdbLoadEncodedStringObject(fp)) == NULL) return NULL;
        o = tryObjectEncoding(o);
    } else if (type == REDIS_LIST || type == REDIS_SET) {
        /* Read list/set value */
        uint32_t listlen;

        if ((listlen = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;
        o = (type == REDIS_LIST) ? createListObject() : createSetObject();
        /* It's faster to expand the dict to the right size asap in order
         * to avoid rehashing */
        if (type == REDIS_SET && listlen > DICT_HT_INITIAL_SIZE)
            dictExpand(o->ptr,listlen);
        /* Load every single element of the list/set */
        while(listlen--) {
            robj *ele;

            if ((ele = rdbLoadEncodedStringObject(fp)) == NULL) return NULL;
            ele = tryObjectEncoding(ele);
            if (type == REDIS_LIST) {
                listAddNodeTail((list*)o->ptr,ele);
            } else {
                dictAdd((dict*)o->ptr,ele,NULL);
            }
        }
    } else if (type == REDIS_ZSET) {
        /* Read list/set value */
        size_t zsetlen;
        zset *zs;

        if ((zsetlen = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;
        o = createZsetObject();
        zs = o->ptr;
        /* Load every single element of the list/set */
        while(zsetlen--) {
            robj *ele;
            double *score = zmalloc(sizeof(double));

            if ((ele = rdbLoadEncodedStringObject(fp)) == NULL) return NULL;
            ele = tryObjectEncoding(ele);
            if (rdbLoadDoubleValue(fp,score) == -1) return NULL;
            dictAdd(zs->dict,ele,score);
            zslInsert(zs->zsl,*score,ele);
            incrRefCount(ele); /* added to skiplist */
        }
    } else if (type == REDIS_HASH) {
        size_t hashlen;

        if ((hashlen = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;
        o = createHashObject();
        /* Too many entries? Use an hash table. */
        if (hashlen > server.hash_max_zipmap_entries)
            convertToRealHash(o);
        /* Load every key/value, then set it into the zipmap or hash
         * table, as needed. */
        while(hashlen--) {
            robj *key, *val;

            if ((key = rdbLoadStringObject(fp)) == NULL) return NULL;
            if ((val = rdbLoadStringObject(fp)) == NULL) return NULL;
            /* If we are using a zipmap and there are too big values
             * the object is converted to real hash table encoding. */
            if (o->encoding != REDIS_ENCODING_HT &&
               (sdslen(key->ptr) > server.hash_max_zipmap_value ||
                sdslen(val->ptr) > server.hash_max_zipmap_value))
            {
                    convertToRealHash(o);
            }

            if (o->encoding == REDIS_ENCODING_ZIPMAP) {
                unsigned char *zm = o->ptr;

                zm = zipmapSet(zm,key->ptr,sdslen(key->ptr),
                                  val->ptr,sdslen(val->ptr),NULL);
                o->ptr = zm;
                decrRefCount(key);
                decrRefCount(val);
            } else {
                key = tryObjectEncoding(key);
                val = tryObjectEncoding(val);
                dictAdd((dict*)o->ptr,key,val);
            }
        }
    } else {
        redisPanic("Unknown object type");
    }
    return o;
}

static int rdbLoad(char *filename) {
    FILE *fp;
    uint32_t dbid;
    int type, retval, rdbver;
    int swap_all_values = 0;
    dict *d = server.db[0].dict;
    redisDb *db = server.db+0;
    char buf[1024];
    time_t expiretime, now = time(NULL);
    long long loadedkeys = 0;

    fp = fopen(filename,"r");
    if (!fp) return REDIS_ERR;
    if (fread(buf,9,1,fp) == 0) goto eoferr;
    buf[9] = '\0';
    if (memcmp(buf,"REDIS",5) != 0) {
        fclose(fp);
        redisLog(REDIS_WARNING,"Wrong signature trying to load DB from file");
        return REDIS_ERR;
    }
    rdbver = atoi(buf+5);
    if (rdbver != 1) {
        fclose(fp);
        redisLog(REDIS_WARNING,"Can't handle RDB format version %d",rdbver);
        return REDIS_ERR;
    }
    while(1) {
        robj *key, *val;

        expiretime = -1;
        /* Read type. */
        if ((type = rdbLoadType(fp)) == -1) goto eoferr;
        if (type == REDIS_EXPIRETIME) {
            if ((expiretime = rdbLoadTime(fp)) == -1) goto eoferr;
            /* We read the time so we need to read the object type again */
            if ((type = rdbLoadType(fp)) == -1) goto eoferr;
        }
        if (type == REDIS_EOF) break;
        /* Handle SELECT DB opcode as a special case */
        if (type == REDIS_SELECTDB) {
            if ((dbid = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR)
                goto eoferr;
            if (dbid >= (unsigned)server.dbnum) {
                redisLog(REDIS_WARNING,"FATAL: Data file was created with a Redis server configured to handle more than %d databases. Exiting\n", server.dbnum);
                exit(1);
            }
            db = server.db+dbid;
            d = db->dict;
            continue;
        }
        /* Read key */
        if ((key = rdbLoadStringObject(fp)) == NULL) goto eoferr;
        /* Read value */
        if ((val = rdbLoadObject(type,fp)) == NULL) goto eoferr;
        /* Check if the key already expired */
        if (expiretime != -1 && expiretime < now) {
            decrRefCount(key);
            decrRefCount(val);
            continue;
        }
        /* Add the new object in the hash table */
        retval = dictAdd(d,key,val);
        if (retval == DICT_ERR) {
            redisLog(REDIS_WARNING,"Loading DB, duplicated key (%s) found! Unrecoverable error, exiting now.", key->ptr);
            exit(1);
        }
        loadedkeys++;
        /* Set the expire time if needed */
        if (expiretime != -1) setExpire(db,key,expiretime);

        /* Handle swapping while loading big datasets when VM is on */

        /* If we detecter we are hopeless about fitting something in memory
         * we just swap every new key on disk. Directly...
         * Note that's important to check for this condition before resorting
         * to random sampling, otherwise we may try to swap already
         * swapped keys. */
        if (swap_all_values) {
            dictEntry *de = dictFind(d,key);

            /* de may be NULL since the key already expired */
            if (de) {
                key = dictGetEntryKey(de);
                val = dictGetEntryVal(de);

                if (vmSwapObjectBlocking(key,val) == REDIS_OK) {
                    dictGetEntryVal(de) = NULL;
                }
            }
            continue;
        }

        /* If we have still some hope of having some value fitting memory
         * then we try random sampling. */
        if (!swap_all_values && server.vm_enabled && (loadedkeys % 5000) == 0) {
            while (zmalloc_used_memory() > server.vm_max_memory) {
                if (vmSwapOneObjectBlocking() == REDIS_ERR) break;
            }
            if (zmalloc_used_memory() > server.vm_max_memory)
                swap_all_values = 1; /* We are already using too much mem */
        }
    }
    fclose(fp);
    return REDIS_OK;

eoferr: /* unexpected end of file is handled here with a fatal exit */
    redisLog(REDIS_WARNING,"Short read or OOM loading DB. Unrecoverable error, aborting now.");
    exit(1);
    return REDIS_ERR; /* Just to avoid warning */
}

/*================================== Shutdown =============================== */
static int prepareForShutdown() {
    redisLog(REDIS_WARNING,"User requested shutdown, saving DB...");
    /* Kill the saving child if there is a background saving in progress.
       We want to avoid race conditions, for instance our saving child may
       overwrite the synchronous saving did by SHUTDOWN. */
    if (server.bgsavechildpid != -1) {
        redisLog(REDIS_WARNING,"There is a live saving child. Killing it!");
        kill(server.bgsavechildpid,SIGKILL);
        rdbRemoveTempFile(server.bgsavechildpid);
    }
    if (server.appendonly) {
        /* Append only file: fsync() the AOF and exit */
        fsync(server.appendfd);
        if (server.vm_enabled) unlink(server.vm_swap_file);
    } else {
        /* Snapshotting. Perform a SYNC SAVE and exit */
        if (rdbSave(server.dbfilename) == REDIS_OK) {
            if (server.daemonize)
                unlink(server.pidfile);
            redisLog(REDIS_WARNING,"%zu bytes used at exit",zmalloc_used_memory());
        } else {
            /* Ooops.. error saving! The best we can do is to continue
             * operating. Note that if there was a background saving process,
             * in the next cron() Redis will be notified that the background
             * saving aborted, handling special stuff like slaves pending for
             * synchronization... */
            redisLog(REDIS_WARNING,"Error trying to save the DB, can't exit");
            return REDIS_ERR;
        }
    }
    redisLog(REDIS_WARNING,"Server exit now, bye bye...");
    return REDIS_OK;
}

/*================================== Commands =============================== */

static void authCommand(redisClient *c) {
    if (!server.requirepass || !strcmp(c->argv[1]->ptr, server.requirepass)) {
      c->authenticated = 1;
      addReply(c,shared.ok);
    } else {
      c->authenticated = 0;
      addReplySds(c,sdscatprintf(sdsempty(),"-ERR invalid password\r\n"));
    }
}

static void pingCommand(redisClient *c) {
    addReply(c,shared.pong);
}

static void echoCommand(redisClient *c) {
    addReplyBulk(c,c->argv[1]);
}

/*=================================== Strings =============================== */

static void setGenericCommand(redisClient *c, int nx, robj *key, robj *val, robj *expire) {
    int retval;
    long seconds = 0; /* initialized to avoid an harmness warning */

    if (expire) {
        if (getLongFromObjectOrReply(c, expire, &seconds, NULL) != REDIS_OK)
            return;
        if (seconds <= 0) {
            addReplySds(c,sdsnew("-ERR invalid expire time in SETEX\r\n"));
            return;
        }
    }

    touchWatchedKey(c->db,key);
    if (nx) deleteIfVolatile(c->db,key);
    retval = dictAdd(c->db->dict,key,val);
    if (retval == DICT_ERR) {
        if (!nx) {
            /* If the key is about a swapped value, we want a new key object
             * to overwrite the old. So we delete the old key in the database.
             * This will also make sure that swap pages about the old object
             * will be marked as free. */
            if (server.vm_enabled && deleteIfSwapped(c->db,key))
                incrRefCount(key);
            dictReplace(c->db->dict,key,val);
            incrRefCount(val);
        } else {
            addReply(c,shared.czero);
            return;
        }
    } else {
        incrRefCount(key);
        incrRefCount(val);
    }
    server.dirty++;
    removeExpire(c->db,key);
    if (expire) setExpire(c->db,key,time(NULL)+seconds);
    addReply(c, nx ? shared.cone : shared.ok);
}

static void setCommand(redisClient *c) {
    setGenericCommand(c,0,c->argv[1],c->argv[2],NULL);
}

static void setnxCommand(redisClient *c) {
    setGenericCommand(c,1,c->argv[1],c->argv[2],NULL);
}

static void setexCommand(redisClient *c) {
    setGenericCommand(c,0,c->argv[1],c->argv[3],c->argv[2]);
}

static int getGenericCommand(redisClient *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL)
        return REDIS_OK;

    if (o->type != REDIS_STRING) {
        addReply(c,shared.wrongtypeerr);
        return REDIS_ERR;
    } else {
        addReplyBulk(c,o);
        return REDIS_OK;
    }
}

static void getCommand(redisClient *c) {
    getGenericCommand(c);
}

static void getsetCommand(redisClient *c) {
    if (getGenericCommand(c) == REDIS_ERR) return;
    if (dictAdd(c->db->dict,c->argv[1],c->argv[2]) == DICT_ERR) {
        dictReplace(c->db->dict,c->argv[1],c->argv[2]);
    } else {
        incrRefCount(c->argv[1]);
    }
    incrRefCount(c->argv[2]);
    server.dirty++;
    removeExpire(c->db,c->argv[1]);
}

static void mgetCommand(redisClient *c) {
    int j;

    addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",c->argc-1));
    for (j = 1; j < c->argc; j++) {
        robj *o = lookupKeyRead(c->db,c->argv[j]);
        if (o == NULL) {
            addReply(c,shared.nullbulk);
        } else {
            if (o->type != REDIS_STRING) {
                addReply(c,shared.nullbulk);
            } else {
                addReplyBulk(c,o);
            }
        }
    }
}

static void msetGenericCommand(redisClient *c, int nx) {
    int j, busykeys = 0;

    if ((c->argc % 2) == 0) {
        addReplySds(c,sdsnew("-ERR wrong number of arguments for MSET\r\n"));
        return;
    }
    /* Handle the NX flag. The MSETNX semantic is to return zero and don't
     * set nothing at all if at least one already key exists. */
    if (nx) {
        for (j = 1; j < c->argc; j += 2) {
            if (lookupKeyWrite(c->db,c->argv[j]) != NULL) {
                busykeys++;
            }
        }
    }
    if (busykeys) {
        addReply(c, shared.czero);
        return;
    }

    for (j = 1; j < c->argc; j += 2) {
        int retval;

        c->argv[j+1] = tryObjectEncoding(c->argv[j+1]);
        retval = dictAdd(c->db->dict,c->argv[j],c->argv[j+1]);
        if (retval == DICT_ERR) {
            dictReplace(c->db->dict,c->argv[j],c->argv[j+1]);
            incrRefCount(c->argv[j+1]);
        } else {
            incrRefCount(c->argv[j]);
            incrRefCount(c->argv[j+1]);
        }
        removeExpire(c->db,c->argv[j]);
    }
    server.dirty += (c->argc-1)/2;
    addReply(c, nx ? shared.cone : shared.ok);
}

static void msetCommand(redisClient *c) {
    msetGenericCommand(c,0);
}

static void msetnxCommand(redisClient *c) {
    msetGenericCommand(c,1);
}

static void incrDecrCommand(redisClient *c, long long incr) {
    long long value;
    int retval;
    robj *o;

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o != NULL && checkType(c,o,REDIS_STRING)) return;
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != REDIS_OK) return;

    value += incr;
    o = createStringObjectFromLongLong(value);
    retval = dictAdd(c->db->dict,c->argv[1],o);
    if (retval == DICT_ERR) {
        dictReplace(c->db->dict,c->argv[1],o);
        removeExpire(c->db,c->argv[1]);
    } else {
        incrRefCount(c->argv[1]);
    }
    server.dirty++;
    addReply(c,shared.colon);
    addReply(c,o);
    addReply(c,shared.crlf);
}

static void incrCommand(redisClient *c) {
    incrDecrCommand(c,1);
}

static void decrCommand(redisClient *c) {
    incrDecrCommand(c,-1);
}

static void incrbyCommand(redisClient *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != REDIS_OK) return;
    incrDecrCommand(c,incr);
}

static void decrbyCommand(redisClient *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != REDIS_OK) return;
    incrDecrCommand(c,-incr);
}

static void appendCommand(redisClient *c) {
    int retval;
    size_t totlen;
    robj *o;

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        /* Create the key */
        retval = dictAdd(c->db->dict,c->argv[1],c->argv[2]);
        incrRefCount(c->argv[1]);
        incrRefCount(c->argv[2]);
        totlen = stringObjectLen(c->argv[2]);
    } else {
        dictEntry *de;

        de = dictFind(c->db->dict,c->argv[1]);
        assert(de != NULL);

        o = dictGetEntryVal(de);
        if (o->type != REDIS_STRING) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
        /* If the object is specially encoded or shared we have to make
         * a copy */
        if (o->refcount != 1 || o->encoding != REDIS_ENCODING_RAW) {
            robj *decoded = getDecodedObject(o);

            o = createStringObject(decoded->ptr, sdslen(decoded->ptr));
            decrRefCount(decoded);
            dictReplace(c->db->dict,c->argv[1],o);
        }
        /* APPEND! */
        if (c->argv[2]->encoding == REDIS_ENCODING_RAW) {
            o->ptr = sdscatlen(o->ptr,
                c->argv[2]->ptr, sdslen(c->argv[2]->ptr));
        } else {
            o->ptr = sdscatprintf(o->ptr, "%ld",
                (unsigned long) c->argv[2]->ptr);
        }
        totlen = sdslen(o->ptr);
    }
    server.dirty++;
    addReplySds(c,sdscatprintf(sdsempty(),":%lu\r\n",(unsigned long)totlen));
}

static void substrCommand(redisClient *c) {
    robj *o;
    long start = atoi(c->argv[2]->ptr);
    long end = atoi(c->argv[3]->ptr);
    size_t rangelen, strlen;
    sds range;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,o,REDIS_STRING)) return;

    o = getDecodedObject(o);
    strlen = sdslen(o->ptr);

    /* convert negative indexes */
    if (start < 0) start = strlen+start;
    if (end < 0) end = strlen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;

    /* indexes sanity checks */
    if (start > end || (size_t)start >= strlen) {
        /* Out of range start or start > end result in null reply */
        addReply(c,shared.nullbulk);
        decrRefCount(o);
        return;
    }
    if ((size_t)end >= strlen) end = strlen-1;
    rangelen = (end-start)+1;

    /* Return the result */
    addReplySds(c,sdscatprintf(sdsempty(),"$%zu\r\n",rangelen));
    range = sdsnewlen((char*)o->ptr+start,rangelen);
    addReplySds(c,range);
    addReply(c,shared.crlf);
    decrRefCount(o);
}

/* ========================= Type agnostic commands ========================= */

static void delCommand(redisClient *c) {
    int deleted = 0, j;

    for (j = 1; j < c->argc; j++) {
        if (deleteKey(c->db,c->argv[j])) {
            touchWatchedKey(c->db,c->argv[j]);
            server.dirty++;
            deleted++;
        }
    }
    addReplyLongLong(c,deleted);
}

static void existsCommand(redisClient *c) {
    expireIfNeeded(c->db,c->argv[1]);
    if (dictFind(c->db->dict,c->argv[1])) {
        addReply(c, shared.cone);
    } else {
        addReply(c, shared.czero);
    }
}

static void selectCommand(redisClient *c) {
    int id = atoi(c->argv[1]->ptr);

    if (selectDb(c,id) == REDIS_ERR) {
        addReplySds(c,sdsnew("-ERR invalid DB index\r\n"));
    } else {
        addReply(c,shared.ok);
    }
}

static void randomkeyCommand(redisClient *c) {
    dictEntry *de;
    robj *key;

    while(1) {
        de = dictGetRandomKey(c->db->dict);
        if (!de || expireIfNeeded(c->db,dictGetEntryKey(de)) == 0) break;
    }

    if (de == NULL) {
        addReply(c,shared.nullbulk);
        return;
    }

    key = dictGetEntryKey(de);
    if (server.vm_enabled) {
        key = dupStringObject(key);
        addReplyBulk(c,key);
        decrRefCount(key);
    } else {
        addReplyBulk(c,key);
    }
}

static void keysCommand(redisClient *c) {
    dictIterator *di;
    dictEntry *de;
    sds pattern = c->argv[1]->ptr;
    int plen = sdslen(pattern);
    unsigned long numkeys = 0;
    robj *lenobj = createObject(REDIS_STRING,NULL);

    di = dictGetIterator(c->db->dict);
    addReply(c,lenobj);
    decrRefCount(lenobj);
    while((de = dictNext(di)) != NULL) {
        robj *keyobj = dictGetEntryKey(de);

        sds key = keyobj->ptr;
        if ((pattern[0] == '*' && pattern[1] == '\0') ||
            stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            if (expireIfNeeded(c->db,keyobj) == 0) {
                addReplyBulk(c,keyobj);
                numkeys++;
            }
        }
    }
    dictReleaseIterator(di);
    lenobj->ptr = sdscatprintf(sdsempty(),"*%lu\r\n",numkeys);
}

static void dbsizeCommand(redisClient *c) {
    addReplySds(c,
        sdscatprintf(sdsempty(),":%lu\r\n",dictSize(c->db->dict)));
}

static void lastsaveCommand(redisClient *c) {
    addReplySds(c,
        sdscatprintf(sdsempty(),":%lu\r\n",server.lastsave));
}

static void typeCommand(redisClient *c) {
    robj *o;
    char *type;

    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        type = "+none";
    } else {
        switch(o->type) {
        case REDIS_STRING: type = "+string"; break;
        case REDIS_LIST: type = "+list"; break;
        case REDIS_SET: type = "+set"; break;
        case REDIS_ZSET: type = "+zset"; break;
        case REDIS_HASH: type = "+hash"; break;
        default: type = "+unknown"; break;
        }
    }
    addReplySds(c,sdsnew(type));
    addReply(c,shared.crlf);
}

static void saveCommand(redisClient *c) {
    if (server.bgsavechildpid != -1) {
        addReplySds(c,sdsnew("-ERR background save in progress\r\n"));
        return;
    }
    if (rdbSave(server.dbfilename) == REDIS_OK) {
        addReply(c,shared.ok);
    } else {
        addReply(c,shared.err);
    }
}

static void bgsaveCommand(redisClient *c) {
    if (server.bgsavechildpid != -1) {
        addReplySds(c,sdsnew("-ERR background save already in progress\r\n"));
        return;
    }
    if (rdbSaveBackground(server.dbfilename) == REDIS_OK) {
        char *status = "+Background saving started\r\n";
        addReplySds(c,sdsnew(status));
    } else {
        addReply(c,shared.err);
    }
}

static void shutdownCommand(redisClient *c) {
    if (prepareForShutdown() == REDIS_OK)
        exit(0);
    addReplySds(c, sdsnew("-ERR Errors trying to SHUTDOWN. Check logs.\r\n"));
}

static void renameGenericCommand(redisClient *c, int nx) {
    robj *o;

    /* To use the same key as src and dst is probably an error */
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) {
        addReply(c,shared.sameobjecterr);
        return;
    }

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;

    incrRefCount(o);
    deleteIfVolatile(c->db,c->argv[2]);
    if (dictAdd(c->db->dict,c->argv[2],o) == DICT_ERR) {
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        dictReplace(c->db->dict,c->argv[2],o);
    } else {
        incrRefCount(c->argv[2]);
    }
    deleteKey(c->db,c->argv[1]);
    touchWatchedKey(c->db,c->argv[2]);
    server.dirty++;
    addReply(c,nx ? shared.cone : shared.ok);
}

static void renameCommand(redisClient *c) {
    renameGenericCommand(c,0);
}

static void renamenxCommand(redisClient *c) {
    renameGenericCommand(c,1);
}

static void moveCommand(redisClient *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid;

    /* Obtain source and target DB pointers */
    src = c->db;
    srcid = c->db->id;
    if (selectDb(c,atoi(c->argv[2]->ptr)) == REDIS_ERR) {
        addReply(c,shared.outofrangeerr);
        return;
    }
    dst = c->db;
    selectDb(c,srcid); /* Back to the source DB */

    /* If the user is moving using as target the same
     * DB as the source DB it is probably an error. */
    if (src == dst) {
        addReply(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference */
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (!o) {
        addReply(c,shared.czero);
        return;
    }

    /* Try to add the element to the target DB */
    deleteIfVolatile(dst,c->argv[1]);
    if (dictAdd(dst->dict,c->argv[1],o) == DICT_ERR) {
        addReply(c,shared.czero);
        return;
    }
    incrRefCount(c->argv[1]);
    incrRefCount(o);

    /* OK! key moved, free the entry in the source DB */
    deleteKey(src,c->argv[1]);
    server.dirty++;
    addReply(c,shared.cone);
}

/* =================================== Lists ================================ */
static void pushGenericCommand(redisClient *c, int where) {
    robj *lobj;
    list *list;

    lobj = lookupKeyWrite(c->db,c->argv[1]);
    if (lobj == NULL) {
        if (handleClientsWaitingListPush(c,c->argv[1],c->argv[2])) {
            addReply(c,shared.cone);
            return;
        }
        lobj = createListObject();
        list = lobj->ptr;
        if (where == REDIS_HEAD) {
            listAddNodeHead(list,c->argv[2]);
        } else {
            listAddNodeTail(list,c->argv[2]);
        }
        dictAdd(c->db->dict,c->argv[1],lobj);
        incrRefCount(c->argv[1]);
        incrRefCount(c->argv[2]);
    } else {
        if (lobj->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
        if (handleClientsWaitingListPush(c,c->argv[1],c->argv[2])) {
            addReply(c,shared.cone);
            return;
        }
        list = lobj->ptr;
        if (where == REDIS_HEAD) {
            listAddNodeHead(list,c->argv[2]);
        } else {
            listAddNodeTail(list,c->argv[2]);
        }
        incrRefCount(c->argv[2]);
    }
    server.dirty++;
    addReplyLongLong(c,listLength(list));
}

static void lpushCommand(redisClient *c) {
    pushGenericCommand(c,REDIS_HEAD);
}

static void rpushCommand(redisClient *c) {
    pushGenericCommand(c,REDIS_TAIL);
}

static void llenCommand(redisClient *c) {
    robj *o;
    list *l;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_LIST)) return;

    l = o->ptr;
    addReplyUlong(c,listLength(l));
}

static void lindexCommand(redisClient *c) {
    robj *o;
    int index = atoi(c->argv[2]->ptr);
    list *list;
    listNode *ln;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,o,REDIS_LIST)) return;
    list = o->ptr;

    ln = listIndex(list, index);
    if (ln == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        robj *ele = listNodeValue(ln);
        addReplyBulk(c,ele);
    }
}

static void lsetCommand(redisClient *c) {
    robj *o;
    int index = atoi(c->argv[2]->ptr);
    list *list;
    listNode *ln;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL ||
        checkType(c,o,REDIS_LIST)) return;
    list = o->ptr;

    ln = listIndex(list, index);
    if (ln == NULL) {
        addReply(c,shared.outofrangeerr);
    } else {
        robj *ele = listNodeValue(ln);

        decrRefCount(ele);
        listNodeValue(ln) = c->argv[3];
        incrRefCount(c->argv[3]);
        addReply(c,shared.ok);
        server.dirty++;
    }
}

static void popGenericCommand(redisClient *c, int where) {
    robj *o;
    list *list;
    listNode *ln;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,o,REDIS_LIST)) return;
    list = o->ptr;

    if (where == REDIS_HEAD)
        ln = listFirst(list);
    else
        ln = listLast(list);

    if (ln == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        robj *ele = listNodeValue(ln);
        addReplyBulk(c,ele);
        listDelNode(list,ln);
        if (listLength(list) == 0) deleteKey(c->db,c->argv[1]);
        server.dirty++;
    }
}

static void lpopCommand(redisClient *c) {
    popGenericCommand(c,REDIS_HEAD);
}

static void rpopCommand(redisClient *c) {
    popGenericCommand(c,REDIS_TAIL);
}

static void lrangeCommand(redisClient *c) {
    robj *o;
    int start = atoi(c->argv[2]->ptr);
    int end = atoi(c->argv[3]->ptr);
    int llen;
    int rangelen, j;
    list *list;
    listNode *ln;
    robj *ele;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
         || checkType(c,o,REDIS_LIST)) return;
    list = o->ptr;
    llen = listLength(list);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;

    /* indexes sanity checks */
    if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list */
        addReply(c,shared.emptymultibulk);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    /* Return the result in form of a multi-bulk reply */
    ln = listIndex(list, start);
    addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",rangelen));
    for (j = 0; j < rangelen; j++) {
        ele = listNodeValue(ln);
        addReplyBulk(c,ele);
        ln = ln->next;
    }
}

static void ltrimCommand(redisClient *c) {
    robj *o;
    int start = atoi(c->argv[2]->ptr);
    int end = atoi(c->argv[3]->ptr);
    int llen;
    int j, ltrim, rtrim;
    list *list;
    listNode *ln;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.ok)) == NULL ||
        checkType(c,o,REDIS_LIST)) return;
    list = o->ptr;
    llen = listLength(list);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;

    /* indexes sanity checks */
    if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list */
        ltrim = llen;
        rtrim = 0;
    } else {
        if (end >= llen) end = llen-1;
        ltrim = start;
        rtrim = llen-end-1;
    }

    /* Remove list elements to perform the trim */
    for (j = 0; j < ltrim; j++) {
        ln = listFirst(list);
        listDelNode(list,ln);
    }
    for (j = 0; j < rtrim; j++) {
        ln = listLast(list);
        listDelNode(list,ln);
    }
    if (listLength(list) == 0) deleteKey(c->db,c->argv[1]);
    server.dirty++;
    addReply(c,shared.ok);
}

static void lremCommand(redisClient *c) {
    robj *o;
    list *list;
    listNode *ln, *next;
    int toremove = atoi(c->argv[2]->ptr);
    int removed = 0;
    int fromtail = 0;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_LIST)) return;
    list = o->ptr;

    if (toremove < 0) {
        toremove = -toremove;
        fromtail = 1;
    }
    ln = fromtail ? list->tail : list->head;
    while (ln) {
        robj *ele = listNodeValue(ln);

        next = fromtail ? ln->prev : ln->next;
        if (equalStringObjects(ele,c->argv[3])) {
            listDelNode(list,ln);
            server.dirty++;
            removed++;
            if (toremove && removed == toremove) break;
        }
        ln = next;
    }
    if (listLength(list) == 0) deleteKey(c->db,c->argv[1]);
    addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",removed));
}

/* This is the semantic of this command:
 *  RPOPLPUSH srclist dstlist:
 *   IF LLEN(srclist) > 0
 *     element = RPOP srclist
 *     LPUSH dstlist element
 *     RETURN element
 *   ELSE
 *     RETURN nil
 *   END
 *  END
 *
 * The idea is to be able to get an element from a list in a reliable way
 * since the element is not just returned but pushed against another list
 * as well. This command was originally proposed by Ezra Zygmuntowicz.
 */
static void rpoplpushcommand(redisClient *c) {
    robj *sobj;
    list *srclist;
    listNode *ln;

    if ((sobj = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,sobj,REDIS_LIST)) return;
    srclist = sobj->ptr;
    ln = listLast(srclist);

    if (ln == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        robj *dobj = lookupKeyWrite(c->db,c->argv[2]);
        robj *ele = listNodeValue(ln);
        list *dstlist;

        if (dobj && dobj->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
            return;
        }

        /* Add the element to the target list (unless it's directly
         * passed to some BLPOP-ing client */
        if (!handleClientsWaitingListPush(c,c->argv[2],ele)) {
            if (dobj == NULL) {
                /* Create the list if the key does not exist */
                dobj = createListObject();
                dictAdd(c->db->dict,c->argv[2],dobj);
                incrRefCount(c->argv[2]);
            }
            dstlist = dobj->ptr;
            listAddNodeHead(dstlist,ele);
            incrRefCount(ele);
        }

        /* Send the element to the client as reply as well */
        addReplyBulk(c,ele);

        /* Finally remove the element from the source list */
        listDelNode(srclist,ln);
        if (listLength(srclist) == 0) deleteKey(c->db,c->argv[1]);
        server.dirty++;
    }
}

/* ==================================== Sets ================================ */

static void saddCommand(redisClient *c) {
    robj *set;

    set = lookupKeyWrite(c->db,c->argv[1]);
    if (set == NULL) {
        set = createSetObject();
        dictAdd(c->db->dict,c->argv[1],set);
        incrRefCount(c->argv[1]);
    } else {
        if (set->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
    }
    if (dictAdd(set->ptr,c->argv[2],NULL) == DICT_OK) {
        incrRefCount(c->argv[2]);
        server.dirty++;
        addReply(c,shared.cone);
    } else {
        addReply(c,shared.czero);
    }
}

static void sremCommand(redisClient *c) {
    robj *set;

    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,REDIS_SET)) return;

    if (dictDelete(set->ptr,c->argv[2]) == DICT_OK) {
        server.dirty++;
        if (htNeedsResize(set->ptr)) dictResize(set->ptr);
        if (dictSize((dict*)set->ptr) == 0) deleteKey(c->db,c->argv[1]);
        addReply(c,shared.cone);
    } else {
        addReply(c,shared.czero);
    }
}

static void smoveCommand(redisClient *c) {
    robj *srcset, *dstset;

    srcset = lookupKeyWrite(c->db,c->argv[1]);
    dstset = lookupKeyWrite(c->db,c->argv[2]);

    /* If the source key does not exist return 0, if it's of the wrong type
     * raise an error */
    if (srcset == NULL || srcset->type != REDIS_SET) {
        addReply(c, srcset ? shared.wrongtypeerr : shared.czero);
        return;
    }
    /* Error if the destination key is not a set as well */
    if (dstset && dstset->type != REDIS_SET) {
        addReply(c,shared.wrongtypeerr);
        return;
    }
    /* Remove the element from the source set */
    if (dictDelete(srcset->ptr,c->argv[3]) == DICT_ERR) {
        /* Key not found in the src set! return zero */
        addReply(c,shared.czero);
        return;
    }
    if (dictSize((dict*)srcset->ptr) == 0 && srcset != dstset)
        deleteKey(c->db,c->argv[1]);
    server.dirty++;
    /* Add the element to the destination set */
    if (!dstset) {
        dstset = createSetObject();
        dictAdd(c->db->dict,c->argv[2],dstset);
        incrRefCount(c->argv[2]);
    }
    if (dictAdd(dstset->ptr,c->argv[3],NULL) == DICT_OK)
        incrRefCount(c->argv[3]);
    addReply(c,shared.cone);
}

static void sismemberCommand(redisClient *c) {
    robj *set;

    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,REDIS_SET)) return;

    if (dictFind(set->ptr,c->argv[2]))
        addReply(c,shared.cone);
    else
        addReply(c,shared.czero);
}

static void scardCommand(redisClient *c) {
    robj *o;
    dict *s;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_SET)) return;

    s = o->ptr;
    addReplyUlong(c,dictSize(s));
}

static void spopCommand(redisClient *c) {
    robj *set;
    dictEntry *de;

    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,set,REDIS_SET)) return;

    de = dictGetRandomKey(set->ptr);
    if (de == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        robj *ele = dictGetEntryKey(de);

        addReplyBulk(c,ele);
        dictDelete(set->ptr,ele);
        if (htNeedsResize(set->ptr)) dictResize(set->ptr);
        if (dictSize((dict*)set->ptr) == 0) deleteKey(c->db,c->argv[1]);
        server.dirty++;
    }
}

static void srandmemberCommand(redisClient *c) {
    robj *set;
    dictEntry *de;

    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,set,REDIS_SET)) return;

    de = dictGetRandomKey(set->ptr);
    if (de == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        robj *ele = dictGetEntryKey(de);

        addReplyBulk(c,ele);
    }
}

static int qsortCompareSetsByCardinality(const void *s1, const void *s2) {
    dict **d1 = (void*) s1, **d2 = (void*) s2;

    return dictSize(*d1)-dictSize(*d2);
}

static void sinterGenericCommand(redisClient *c, robj **setskeys, unsigned long setsnum, robj *dstkey) {
    dict **dv = zmalloc(sizeof(dict*)*setsnum);
    dictIterator *di;
    dictEntry *de;
    robj *lenobj = NULL, *dstset = NULL;
    unsigned long j, cardinality = 0;

    for (j = 0; j < setsnum; j++) {
        robj *setobj;

        setobj = dstkey ?
                    lookupKeyWrite(c->db,setskeys[j]) :
                    lookupKeyRead(c->db,setskeys[j]);
        if (!setobj) {
            zfree(dv);
            if (dstkey) {
                if (deleteKey(c->db,dstkey))
                    server.dirty++;
                addReply(c,shared.czero);
            } else {
                addReply(c,shared.emptymultibulk);
            }
            return;
        }
        if (setobj->type != REDIS_SET) {
            zfree(dv);
            addReply(c,shared.wrongtypeerr);
            return;
        }
        dv[j] = setobj->ptr;
    }
    /* Sort sets from the smallest to largest, this will improve our
     * algorithm's performace */
    qsort(dv,setsnum,sizeof(dict*),qsortCompareSetsByCardinality);

    /* The first thing we should output is the total number of elements...
     * since this is a multi-bulk write, but at this stage we don't know
     * the intersection set size, so we use a trick, append an empty object
     * to the output list and save the pointer to later modify it with the
     * right length */
    if (!dstkey) {
        lenobj = createObject(REDIS_STRING,NULL);
        addReply(c,lenobj);
        decrRefCount(lenobj);
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with an empty set inside */
        dstset = createSetObject();
    }

    /* Iterate all the elements of the first (smallest) set, and test
     * the element against all the other sets, if at least one set does
     * not include the element it is discarded */
    di = dictGetIterator(dv[0]);

    while((de = dictNext(di)) != NULL) {
        robj *ele;

        for (j = 1; j < setsnum; j++)
            if (dictFind(dv[j],dictGetEntryKey(de)) == NULL) break;
        if (j != setsnum)
            continue; /* at least one set does not contain the member */
        ele = dictGetEntryKey(de);
        if (!dstkey) {
            addReplyBulk(c,ele);
            cardinality++;
        } else {
            dictAdd(dstset->ptr,ele,NULL);
            incrRefCount(ele);
        }
    }
    dictReleaseIterator(di);

    if (dstkey) {
        /* Store the resulting set into the target, if the intersection
         * is not an empty set. */
        deleteKey(c->db,dstkey);
        if (dictSize((dict*)dstset->ptr) > 0) {
            dictAdd(c->db->dict,dstkey,dstset);
            incrRefCount(dstkey);
            addReplyLongLong(c,dictSize((dict*)dstset->ptr));
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);
        }
        server.dirty++;
    } else {
        lenobj->ptr = sdscatprintf(sdsempty(),"*%lu\r\n",cardinality);
    }
    zfree(dv);
}

static void sinterCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+1,c->argc-1,NULL);
}

static void sinterstoreCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+2,c->argc-2,c->argv[1]);
}

#define REDIS_OP_UNION 0
#define REDIS_OP_DIFF 1
#define REDIS_OP_INTER 2

static void sunionDiffGenericCommand(redisClient *c, robj **setskeys, int setsnum, robj *dstkey, int op) {
    dict **dv = zmalloc(sizeof(dict*)*setsnum);
    dictIterator *di;
    dictEntry *de;
    robj *dstset = NULL;
    int j, cardinality = 0;

    for (j = 0; j < setsnum; j++) {
        robj *setobj;

        setobj = dstkey ?
                    lookupKeyWrite(c->db,setskeys[j]) :
                    lookupKeyRead(c->db,setskeys[j]);
        if (!setobj) {
            dv[j] = NULL;
            continue;
        }
        if (setobj->type != REDIS_SET) {
            zfree(dv);
            addReply(c,shared.wrongtypeerr);
            return;
        }
        dv[j] = setobj->ptr;
    }

    /* We need a temp set object to store our union. If the dstkey
     * is not NULL (that is, we are inside an SUNIONSTORE operation) then
     * this set object will be the resulting object to set into the target key*/
    dstset = createSetObject();

    /* Iterate all the elements of all the sets, add every element a single
     * time to the result set */
    for (j = 0; j < setsnum; j++) {
        if (op == REDIS_OP_DIFF && j == 0 && !dv[j]) break; /* result set is empty */
        if (!dv[j]) continue; /* non existing keys are like empty sets */

        di = dictGetIterator(dv[j]);

        while((de = dictNext(di)) != NULL) {
            robj *ele;

            /* dictAdd will not add the same element multiple times */
            ele = dictGetEntryKey(de);
            if (op == REDIS_OP_UNION || j == 0) {
                if (dictAdd(dstset->ptr,ele,NULL) == DICT_OK) {
                    incrRefCount(ele);
                    cardinality++;
                }
            } else if (op == REDIS_OP_DIFF) {
                if (dictDelete(dstset->ptr,ele) == DICT_OK) {
                    cardinality--;
                }
            }
        }
        dictReleaseIterator(di);

        /* result set is empty? Exit asap. */
        if (op == REDIS_OP_DIFF && cardinality == 0) break;
    }

    /* Output the content of the resulting set, if not in STORE mode */
    if (!dstkey) {
        addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",cardinality));
        di = dictGetIterator(dstset->ptr);
        while((de = dictNext(di)) != NULL) {
            robj *ele;

            ele = dictGetEntryKey(de);
            addReplyBulk(c,ele);
        }
        dictReleaseIterator(di);
        decrRefCount(dstset);
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with the result set inside */
        deleteKey(c->db,dstkey);
        if (dictSize((dict*)dstset->ptr) > 0) {
            dictAdd(c->db->dict,dstkey,dstset);
            incrRefCount(dstkey);
            addReplyLongLong(c,dictSize((dict*)dstset->ptr));
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);
        }
        server.dirty++;
    }
    zfree(dv);
}

static void sunionCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,REDIS_OP_UNION);
}

static void sunionstoreCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],REDIS_OP_UNION);
}

static void sdiffCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,REDIS_OP_DIFF);
}

static void sdiffstoreCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],REDIS_OP_DIFF);
}

/* ==================================== ZSets =============================== */

/* ZSETs are ordered sets using two data structures to hold the same elements
 * in order to get O(log(N)) INSERT and REMOVE operations into a sorted
 * data structure.
 *
 * The elements are added to an hash table mapping Redis objects to scores.
 * At the same time the elements are added to a skip list mapping scores
 * to Redis objects (so objects are sorted by scores in this "view"). */

/* This skiplist implementation is almost a C translation of the original
 * algorithm described by William Pugh in "Skip Lists: A Probabilistic
 * Alternative to Balanced Trees", modified in three ways:
 * a) this implementation allows for repeated values.
 * b) the comparison is not just by key (our 'score') but by satellite data.
 * c) there is a back pointer, so it's a doubly linked list with the back
 * pointers being only at "level 1". This allows to traverse the list
 * from tail to head, useful for ZREVRANGE. */

static zskiplistNode *zslCreateNode(int level, double score, robj *obj) {
    zskiplistNode *zn = zmalloc(sizeof(*zn));

    zn->forward = zmalloc(sizeof(zskiplistNode*) * level);
    if (level > 1)
        zn->span = zmalloc(sizeof(unsigned int) * (level - 1));
    else
        zn->span = NULL;
    zn->score = score;
    zn->obj = obj;
    return zn;
}

static zskiplist *zslCreate(void) {
    int j;
    zskiplist *zsl;

    zsl = zmalloc(sizeof(*zsl));
    zsl->level = 1;
    zsl->length = 0;
    zsl->header = zslCreateNode(ZSKIPLIST_MAXLEVEL,0,NULL);
    for (j = 0; j < ZSKIPLIST_MAXLEVEL; j++) {
        zsl->header->forward[j] = NULL;

        /* span has space for ZSKIPLIST_MAXLEVEL-1 elements */
        if (j < ZSKIPLIST_MAXLEVEL-1)
            zsl->header->span[j] = 0;
    }
    zsl->header->backward = NULL;
    zsl->tail = NULL;
    return zsl;
}

static void zslFreeNode(zskiplistNode *node) {
    decrRefCount(node->obj);
    zfree(node->forward);
    zfree(node->span);
    zfree(node);
}

static void zslFree(zskiplist *zsl) {
    zskiplistNode *node = zsl->header->forward[0], *next;

    zfree(zsl->header->forward);
    zfree(zsl->header->span);
    zfree(zsl->header);
    while(node) {
        next = node->forward[0];
        zslFreeNode(node);
        node = next;
    }
    zfree(zsl);
}

static int zslRandomLevel(void) {
    int level = 1;
    while ((random()&0xFFFF) < (ZSKIPLIST_P * 0xFFFF))
        level += 1;
    return (level<ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL;
}

static void zslInsert(zskiplist *zsl, double score, robj *obj) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned int rank[ZSKIPLIST_MAXLEVEL];
    int i, level;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        /* store rank that is crossed to reach the insert position */
        rank[i] = i == (zsl->level-1) ? 0 : rank[i+1];

        while (x->forward[i] &&
            (x->forward[i]->score < score ||
                (x->forward[i]->score == score &&
                compareStringObjects(x->forward[i]->obj,obj) < 0))) {
            rank[i] += i > 0 ? x->span[i-1] : 1;
            x = x->forward[i];
        }
        update[i] = x;
    }
    /* we assume the key is not already inside, since we allow duplicated
     * scores, and the re-insertion of score and redis object should never
     * happpen since the caller of zslInsert() should test in the hash table
     * if the element is already inside or not. */
    level = zslRandomLevel();
    if (level > zsl->level) {
        for (i = zsl->level; i < level; i++) {
            rank[i] = 0;
            update[i] = zsl->header;
            update[i]->span[i-1] = zsl->length;
        }
        zsl->level = level;
    }
    x = zslCreateNode(level,score,obj);
    for (i = 0; i < level; i++) {
        x->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = x;

        /* update span covered by update[i] as x is inserted here */
        if (i > 0) {
            x->span[i-1] = update[i]->span[i-1] - (rank[0] - rank[i]);
            update[i]->span[i-1] = (rank[0] - rank[i]) + 1;
        }
    }

    /* increment span for untouched levels */
    for (i = level; i < zsl->level; i++) {
        update[i]->span[i-1]++;
    }

    x->backward = (update[0] == zsl->header) ? NULL : update[0];
    if (x->forward[0])
        x->forward[0]->backward = x;
    else
        zsl->tail = x;
    zsl->length++;
}

/* Internal function used by zslDelete, zslDeleteByScore and zslDeleteByRank */
void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update) {
    int i;
    for (i = 0; i < zsl->level; i++) {
        if (update[i]->forward[i] == x) {
            if (i > 0) {
                update[i]->span[i-1] += x->span[i-1] - 1;
            }
            update[i]->forward[i] = x->forward[i];
        } else {
            /* invariant: i > 0, because update[0]->forward[0]
             * is always equal to x */
            update[i]->span[i-1] -= 1;
        }
    }
    if (x->forward[0]) {
        x->forward[0]->backward = x->backward;
    } else {
        zsl->tail = x->backward;
    }
    while(zsl->level > 1 && zsl->header->forward[zsl->level-1] == NULL)
        zsl->level--;
    zsl->length--;
}

/* Delete an element with matching score/object from the skiplist. */
static int zslDelete(zskiplist *zsl, double score, robj *obj) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] &&
            (x->forward[i]->score < score ||
                (x->forward[i]->score == score &&
                compareStringObjects(x->forward[i]->obj,obj) < 0)))
            x = x->forward[i];
        update[i] = x;
    }
    /* We may have multiple elements with the same score, what we need
     * is to find the element with both the right score and object. */
    x = x->forward[0];
    if (x && score == x->score && equalStringObjects(x->obj,obj)) {
        zslDeleteNode(zsl, x, update);
        zslFreeNode(x);
        return 1;
    } else {
        return 0; /* not found */
    }
    return 0; /* not found */
}

/* Delete all the elements with score between min and max from the skiplist.
 * Min and mx are inclusive, so a score >= min || score <= max is deleted.
 * Note that this function takes the reference to the hash table view of the
 * sorted set, in order to remove the elements from the hash table too. */
static unsigned long zslDeleteRangeByScore(zskiplist *zsl, double min, double max, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] && x->forward[i]->score < min)
            x = x->forward[i];
        update[i] = x;
    }
    /* We may have multiple elements with the same score, what we need
     * is to find the element with both the right score and object. */
    x = x->forward[0];
    while (x && x->score <= max) {
        zskiplistNode *next = x->forward[0];
        zslDeleteNode(zsl, x, update);
        dictDelete(dict,x->obj);
        zslFreeNode(x);
        removed++;
        x = next;
    }
    return removed; /* not found */
}

/* Delete all the elements with rank between start and end from the skiplist.
 * Start and end are inclusive. Note that start and end need to be 1-based */
static unsigned long zslDeleteRangeByRank(zskiplist *zsl, unsigned int start, unsigned int end, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long traversed = 0, removed = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] && (traversed + (i > 0 ? x->span[i-1] : 1)) < start) {
            traversed += i > 0 ? x->span[i-1] : 1;
            x = x->forward[i];
        }
        update[i] = x;
    }

    traversed++;
    x = x->forward[0];
    while (x && traversed <= end) {
        zskiplistNode *next = x->forward[0];
        zslDeleteNode(zsl, x, update);
        dictDelete(dict,x->obj);
        zslFreeNode(x);
        removed++;
        traversed++;
        x = next;
    }
    return removed;
}

/* Find the first node having a score equal or greater than the specified one.
 * Returns NULL if there is no match. */
static zskiplistNode *zslFirstWithScore(zskiplist *zsl, double score) {
    zskiplistNode *x;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] && x->forward[i]->score < score)
            x = x->forward[i];
    }
    /* We may have multiple elements with the same score, what we need
     * is to find the element with both the right score and object. */
    return x->forward[0];
}

/* Find the rank for an element by both score and key.
 * Returns 0 when the element cannot be found, rank otherwise.
 * Note that the rank is 1-based due to the span of zsl->header to the
 * first element. */
static unsigned long zslGetRank(zskiplist *zsl, double score, robj *o) {
    zskiplistNode *x;
    unsigned long rank = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] &&
            (x->forward[i]->score < score ||
                (x->forward[i]->score == score &&
                compareStringObjects(x->forward[i]->obj,o) <= 0))) {
            rank += i > 0 ? x->span[i-1] : 1;
            x = x->forward[i];
        }

        /* x might be equal to zsl->header, so test if obj is non-NULL */
        if (x->obj && equalStringObjects(x->obj,o)) {
            return rank;
        }
    }
    return 0;
}

/* Finds an element by its rank. The rank argument needs to be 1-based. */
zskiplistNode* zslGetElementByRank(zskiplist *zsl, unsigned long rank) {
    zskiplistNode *x;
    unsigned long traversed = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] && (traversed + (i>0 ? x->span[i-1] : 1)) <= rank)
        {
            traversed += i > 0 ? x->span[i-1] : 1;
            x = x->forward[i];
        }
        if (traversed == rank) {
            return x;
        }
    }
    return NULL;
}

/* The actual Z-commands implementations */

/* This generic command implements both ZADD and ZINCRBY.
 * scoreval is the score if the operation is a ZADD (doincrement == 0) or
 * the increment if the operation is a ZINCRBY (doincrement == 1). */
static void zaddGenericCommand(redisClient *c, robj *key, robj *ele, double scoreval, int doincrement) {
    robj *zsetobj;
    zset *zs;
    double *score;

    zsetobj = lookupKeyWrite(c->db,key);
    if (zsetobj == NULL) {
        zsetobj = createZsetObject();
        dictAdd(c->db->dict,key,zsetobj);
        incrRefCount(key);
    } else {
        if (zsetobj->type != REDIS_ZSET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
    }
    zs = zsetobj->ptr;

    /* Ok now since we implement both ZADD and ZINCRBY here the code
     * needs to handle the two different conditions. It's all about setting
     * '*score', that is, the new score to set, to the right value. */
    score = zmalloc(sizeof(double));
    if (doincrement) {
        dictEntry *de;

        /* Read the old score. If the element was not present starts from 0 */
        de = dictFind(zs->dict,ele);
        if (de) {
            double *oldscore = dictGetEntryVal(de);
            *score = *oldscore + scoreval;
        } else {
            *score = scoreval;
        }
    } else {
        *score = scoreval;
    }

    /* What follows is a simple remove and re-insert operation that is common
     * to both ZADD and ZINCRBY... */
    if (dictAdd(zs->dict,ele,score) == DICT_OK) {
        /* case 1: New element */
        incrRefCount(ele); /* added to hash */
        zslInsert(zs->zsl,*score,ele);
        incrRefCount(ele); /* added to skiplist */
        server.dirty++;
        if (doincrement)
            addReplyDouble(c,*score);
        else
            addReply(c,shared.cone);
    } else {
        dictEntry *de;
        double *oldscore;

        /* case 2: Score update operation */
        de = dictFind(zs->dict,ele);
        redisAssert(de != NULL);
        oldscore = dictGetEntryVal(de);
        if (*score != *oldscore) {
            int deleted;

            /* Remove and insert the element in the skip list with new score */
            deleted = zslDelete(zs->zsl,*oldscore,ele);
            redisAssert(deleted != 0);
            zslInsert(zs->zsl,*score,ele);
            incrRefCount(ele);
            /* Update the score in the hash table */
            dictReplace(zs->dict,ele,score);
            server.dirty++;
        } else {
            zfree(score);
        }
        if (doincrement)
            addReplyDouble(c,*score);
        else
            addReply(c,shared.czero);
    }
}

static void zaddCommand(redisClient *c) {
    double scoreval;

    if (getDoubleFromObjectOrReply(c, c->argv[2], &scoreval, NULL) != REDIS_OK) return;
    zaddGenericCommand(c,c->argv[1],c->argv[3],scoreval,0);
}

static void zincrbyCommand(redisClient *c) {
    double scoreval;

    if (getDoubleFromObjectOrReply(c, c->argv[2], &scoreval, NULL) != REDIS_OK) return;
    zaddGenericCommand(c,c->argv[1],c->argv[3],scoreval,1);
}

static void zremCommand(redisClient *c) {
    robj *zsetobj;
    zset *zs;
    dictEntry *de;
    double *oldscore;
    int deleted;

    if ((zsetobj = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,zsetobj,REDIS_ZSET)) return;

    zs = zsetobj->ptr;
    de = dictFind(zs->dict,c->argv[2]);
    if (de == NULL) {
        addReply(c,shared.czero);
        return;
    }
    /* Delete from the skiplist */
    oldscore = dictGetEntryVal(de);
    deleted = zslDelete(zs->zsl,*oldscore,c->argv[2]);
    redisAssert(deleted != 0);

    /* Delete from the hash table */
    dictDelete(zs->dict,c->argv[2]);
    if (htNeedsResize(zs->dict)) dictResize(zs->dict);
    if (dictSize(zs->dict) == 0) deleteKey(c->db,c->argv[1]);
    server.dirty++;
    addReply(c,shared.cone);
}

static void zremrangebyscoreCommand(redisClient *c) {
    double min;
    double max;
    long deleted;
    robj *zsetobj;
    zset *zs;

    if ((getDoubleFromObjectOrReply(c, c->argv[2], &min, NULL) != REDIS_OK) ||
        (getDoubleFromObjectOrReply(c, c->argv[3], &max, NULL) != REDIS_OK)) return;

    if ((zsetobj = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,zsetobj,REDIS_ZSET)) return;

    zs = zsetobj->ptr;
    deleted = zslDeleteRangeByScore(zs->zsl,min,max,zs->dict);
    if (htNeedsResize(zs->dict)) dictResize(zs->dict);
    if (dictSize(zs->dict) == 0) deleteKey(c->db,c->argv[1]);
    server.dirty += deleted;
    addReplyLongLong(c,deleted);
}

static void zremrangebyrankCommand(redisClient *c) {
    long start;
    long end;
    int llen;
    long deleted;
    robj *zsetobj;
    zset *zs;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;

    if ((zsetobj = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,zsetobj,REDIS_ZSET)) return;
    zs = zsetobj->ptr;
    llen = zs->zsl->length;

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;

    /* indexes sanity checks */
    if (start > end || start >= llen) {
        addReply(c,shared.czero);
        return;
    }
    if (end >= llen) end = llen-1;

    /* increment start and end because zsl*Rank functions
     * use 1-based rank */
    deleted = zslDeleteRangeByRank(zs->zsl,start+1,end+1,zs->dict);
    if (htNeedsResize(zs->dict)) dictResize(zs->dict);
    if (dictSize(zs->dict) == 0) deleteKey(c->db,c->argv[1]);
    server.dirty += deleted;
    addReplyLongLong(c, deleted);
}

typedef struct {
    dict *dict;
    double weight;
} zsetopsrc;

static int qsortCompareZsetopsrcByCardinality(const void *s1, const void *s2) {
    zsetopsrc *d1 = (void*) s1, *d2 = (void*) s2;
    unsigned long size1, size2;
    size1 = d1->dict ? dictSize(d1->dict) : 0;
    size2 = d2->dict ? dictSize(d2->dict) : 0;
    return size1 - size2;
}

#define REDIS_AGGR_SUM 1
#define REDIS_AGGR_MIN 2
#define REDIS_AGGR_MAX 3
#define zunionInterDictValue(_e) (dictGetEntryVal(_e) == NULL ? 1.0 : *(double*)dictGetEntryVal(_e))

inline static void zunionInterAggregate(double *target, double val, int aggregate) {
    if (aggregate == REDIS_AGGR_SUM) {
        *target = *target + val;
    } else if (aggregate == REDIS_AGGR_MIN) {
        *target = val < *target ? val : *target;
    } else if (aggregate == REDIS_AGGR_MAX) {
        *target = val > *target ? val : *target;
    } else {
        /* safety net */
        redisPanic("Unknown ZUNION/INTER aggregate type");
    }
}

static void zunionInterGenericCommand(redisClient *c, robj *dstkey, int op) {
    int i, j, setnum;
    int aggregate = REDIS_AGGR_SUM;
    zsetopsrc *src;
    robj *dstobj;
    zset *dstzset;
    dictIterator *di;
    dictEntry *de;

    /* expect setnum input keys to be given */
    setnum = atoi(c->argv[2]->ptr);
    if (setnum < 1) {
        addReplySds(c,sdsnew("-ERR at least 1 input key is needed for ZUNIONSTORE/ZINTERSTORE\r\n"));
        return;
    }

    /* test if the expected number of keys would overflow */
    if (3+setnum > c->argc) {
        addReply(c,shared.syntaxerr);
        return;
    }

    /* read keys to be used for input */
    src = zmalloc(sizeof(zsetopsrc) * setnum);
    for (i = 0, j = 3; i < setnum; i++, j++) {
        robj *obj = lookupKeyWrite(c->db,c->argv[j]);
        if (!obj) {
            src[i].dict = NULL;
        } else {
            if (obj->type == REDIS_ZSET) {
                src[i].dict = ((zset*)obj->ptr)->dict;
            } else if (obj->type == REDIS_SET) {
                src[i].dict = (obj->ptr);
            } else {
                zfree(src);
                addReply(c,shared.wrongtypeerr);
                return;
            }
        }

        /* default all weights to 1 */
        src[i].weight = 1.0;
    }

    /* parse optional extra arguments */
    if (j < c->argc) {
        int remaining = c->argc - j;

        while (remaining) {
            if (remaining >= (setnum + 1) && !strcasecmp(c->argv[j]->ptr,"weights")) {
                j++; remaining--;
                for (i = 0; i < setnum; i++, j++, remaining--) {
                    if (getDoubleFromObjectOrReply(c, c->argv[j], &src[i].weight, NULL) != REDIS_OK)
                        return;
                }
            } else if (remaining >= 2 && !strcasecmp(c->argv[j]->ptr,"aggregate")) {
                j++; remaining--;
                if (!strcasecmp(c->argv[j]->ptr,"sum")) {
                    aggregate = REDIS_AGGR_SUM;
                } else if (!strcasecmp(c->argv[j]->ptr,"min")) {
                    aggregate = REDIS_AGGR_MIN;
                } else if (!strcasecmp(c->argv[j]->ptr,"max")) {
                    aggregate = REDIS_AGGR_MAX;
                } else {
                    zfree(src);
                    addReply(c,shared.syntaxerr);
                    return;
                }
                j++; remaining--;
            } else {
                zfree(src);
                addReply(c,shared.syntaxerr);
                return;
            }
        }
    }

    /* sort sets from the smallest to largest, this will improve our
     * algorithm's performance */
    qsort(src,setnum,sizeof(zsetopsrc),qsortCompareZsetopsrcByCardinality);

    dstobj = createZsetObject();
    dstzset = dstobj->ptr;

    if (op == REDIS_OP_INTER) {
        /* skip going over all entries if the smallest zset is NULL or empty */
        if (src[0].dict && dictSize(src[0].dict) > 0) {
            /* precondition: as src[0].dict is non-empty and the zsets are ordered
             * from small to large, all src[i > 0].dict are non-empty too */
            di = dictGetIterator(src[0].dict);
            while((de = dictNext(di)) != NULL) {
                double *score = zmalloc(sizeof(double)), value;
                *score = src[0].weight * zunionInterDictValue(de);

                for (j = 1; j < setnum; j++) {
                    dictEntry *other = dictFind(src[j].dict,dictGetEntryKey(de));
                    if (other) {
                        value = src[j].weight * zunionInterDictValue(other);
                        zunionInterAggregate(score, value, aggregate);
                    } else {
                        break;
                    }
                }

                /* skip entry when not present in every source dict */
                if (j != setnum) {
                    zfree(score);
                } else {
                    robj *o = dictGetEntryKey(de);
                    dictAdd(dstzset->dict,o,score);
                    incrRefCount(o); /* added to dictionary */
                    zslInsert(dstzset->zsl,*score,o);
                    incrRefCount(o); /* added to skiplist */
                }
            }
            dictReleaseIterator(di);
        }
    } else if (op == REDIS_OP_UNION) {
        for (i = 0; i < setnum; i++) {
            if (!src[i].dict) continue;

            di = dictGetIterator(src[i].dict);
            while((de = dictNext(di)) != NULL) {
                /* skip key when already processed */
                if (dictFind(dstzset->dict,dictGetEntryKey(de)) != NULL) continue;

                double *score = zmalloc(sizeof(double)), value;
                *score = src[i].weight * zunionInterDictValue(de);

                /* because the zsets are sorted by size, its only possible
                 * for sets at larger indices to hold this entry */
                for (j = (i+1); j < setnum; j++) {
                    dictEntry *other = dictFind(src[j].dict,dictGetEntryKey(de));
                    if (other) {
                        value = src[j].weight * zunionInterDictValue(other);
                        zunionInterAggregate(score, value, aggregate);
                    }
                }

                robj *o = dictGetEntryKey(de);
                dictAdd(dstzset->dict,o,score);
                incrRefCount(o); /* added to dictionary */
                zslInsert(dstzset->zsl,*score,o);
                incrRefCount(o); /* added to skiplist */
            }
            dictReleaseIterator(di);
        }
    } else {
        /* unknown operator */
        redisAssert(op == REDIS_OP_INTER || op == REDIS_OP_UNION);
    }

    deleteKey(c->db,dstkey);
    if (dstzset->zsl->length) {
        dictAdd(c->db->dict,dstkey,dstobj);
        incrRefCount(dstkey);
        addReplyLongLong(c, dstzset->zsl->length);
        server.dirty++;
    } else {
        decrRefCount(dstobj);
        addReply(c, shared.czero);
    }
    zfree(src);
}

static void zunionstoreCommand(redisClient *c) {
    zunionInterGenericCommand(c,c->argv[1], REDIS_OP_UNION);
}

static void zinterstoreCommand(redisClient *c) {
    zunionInterGenericCommand(c,c->argv[1], REDIS_OP_INTER);
}

static void zrangeGenericCommand(redisClient *c, int reverse) {
    robj *o;
    long start;
    long end;
    int withscores = 0;
    int llen;
    int rangelen, j;
    zset *zsetobj;
    zskiplist *zsl;
    zskiplistNode *ln;
    robj *ele;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;

    if (c->argc == 5 && !strcasecmp(c->argv[4]->ptr,"withscores")) {
        withscores = 1;
    } else if (c->argc >= 5) {
        addReply(c,shared.syntaxerr);
        return;
    }

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
         || checkType(c,o,REDIS_ZSET)) return;
    zsetobj = o->ptr;
    zsl = zsetobj->zsl;
    llen = zsl->length;

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;

    /* indexes sanity checks */
    if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list */
        addReply(c,shared.emptymultibulk);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    /* check if starting point is trivial, before searching
     * the element in log(N) time */
    if (reverse) {
        ln = start == 0 ? zsl->tail : zslGetElementByRank(zsl, llen-start);
    } else {
        ln = start == 0 ?
            zsl->header->forward[0] : zslGetElementByRank(zsl, start+1);
    }

    /* Return the result in form of a multi-bulk reply */
    addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",
        withscores ? (rangelen*2) : rangelen));
    for (j = 0; j < rangelen; j++) {
        ele = ln->obj;
        addReplyBulk(c,ele);
        if (withscores)
            addReplyDouble(c,ln->score);
        ln = reverse ? ln->backward : ln->forward[0];
    }
}

static void zrangeCommand(redisClient *c) {
    zrangeGenericCommand(c,0);
}

static void zrevrangeCommand(redisClient *c) {
    zrangeGenericCommand(c,1);
}

/* This command implements both ZRANGEBYSCORE and ZCOUNT.
 * If justcount is non-zero, just the count is returned. */
static void genericZrangebyscoreCommand(redisClient *c, int justcount) {
    robj *o;
    double min, max;
    int minex = 0, maxex = 0; /* are min or max exclusive? */
    int offset = 0, limit = -1;
    int withscores = 0;
    int badsyntax = 0;

    /* Parse the min-max interval. If one of the values is prefixed
     * by the "(" character, it's considered "open". For instance
     * ZRANGEBYSCORE zset (1.5 (2.5 will match min < x < max
     * ZRANGEBYSCORE zset 1.5 2.5 will instead match min <= x <= max */
    if (((char*)c->argv[2]->ptr)[0] == '(') {
        min = strtod((char*)c->argv[2]->ptr+1,NULL);
        minex = 1;
    } else {
        min = strtod(c->argv[2]->ptr,NULL);
    }
    if (((char*)c->argv[3]->ptr)[0] == '(') {
        max = strtod((char*)c->argv[3]->ptr+1,NULL);
        maxex = 1;
    } else {
        max = strtod(c->argv[3]->ptr,NULL);
    }

    /* Parse "WITHSCORES": note that if the command was called with
     * the name ZCOUNT then we are sure that c->argc == 4, so we'll never
     * enter the following paths to parse WITHSCORES and LIMIT. */
    if (c->argc == 5 || c->argc == 8) {
        if (strcasecmp(c->argv[c->argc-1]->ptr,"withscores") == 0)
            withscores = 1;
        else
            badsyntax = 1;
    }
    if (c->argc != (4 + withscores) && c->argc != (7 + withscores))
        badsyntax = 1;
    if (badsyntax) {
        addReplySds(c,
            sdsnew("-ERR wrong number of arguments for ZRANGEBYSCORE\r\n"));
        return;
    }

    /* Parse "LIMIT" */
    if (c->argc == (7 + withscores) && strcasecmp(c->argv[4]->ptr,"limit")) {
        addReply(c,shared.syntaxerr);
        return;
    } else if (c->argc == (7 + withscores)) {
        offset = atoi(c->argv[5]->ptr);
        limit = atoi(c->argv[6]->ptr);
        if (offset < 0) offset = 0;
    }

    /* Ok, lookup the key and get the range */
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,justcount ? shared.czero : shared.emptymultibulk);
    } else {
        if (o->type != REDIS_ZSET) {
            addReply(c,shared.wrongtypeerr);
        } else {
            zset *zsetobj = o->ptr;
            zskiplist *zsl = zsetobj->zsl;
            zskiplistNode *ln;
            robj *ele, *lenobj = NULL;
            unsigned long rangelen = 0;

            /* Get the first node with the score >= min, or with
             * score > min if 'minex' is true. */
            ln = zslFirstWithScore(zsl,min);
            while (minex && ln && ln->score == min) ln = ln->forward[0];

            if (ln == NULL) {
                /* No element matching the speciifed interval */
                addReply(c,justcount ? shared.czero : shared.emptymultibulk);
                return;
            }

            /* We don't know in advance how many matching elements there
             * are in the list, so we push this object that will represent
             * the multi-bulk length in the output buffer, and will "fix"
             * it later */
            if (!justcount) {
                lenobj = createObject(REDIS_STRING,NULL);
                addReply(c,lenobj);
                decrRefCount(lenobj);
            }

            while(ln && (maxex ? (ln->score < max) : (ln->score <= max))) {
                if (offset) {
                    offset--;
                    ln = ln->forward[0];
                    continue;
                }
                if (limit == 0) break;
                if (!justcount) {
                    ele = ln->obj;
                    addReplyBulk(c,ele);
                    if (withscores)
                        addReplyDouble(c,ln->score);
                }
                ln = ln->forward[0];
                rangelen++;
                if (limit > 0) limit--;
            }
            if (justcount) {
                addReplyLongLong(c,(long)rangelen);
            } else {
                lenobj->ptr = sdscatprintf(sdsempty(),"*%lu\r\n",
                     withscores ? (rangelen*2) : rangelen);
            }
        }
    }
}

static void zrangebyscoreCommand(redisClient *c) {
    genericZrangebyscoreCommand(c,0);
}

static void zcountCommand(redisClient *c) {
    genericZrangebyscoreCommand(c,1);
}

static void zcardCommand(redisClient *c) {
    robj *o;
    zset *zs;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_ZSET)) return;

    zs = o->ptr;
    addReplyUlong(c,zs->zsl->length);
}

static void zscoreCommand(redisClient *c) {
    robj *o;
    zset *zs;
    dictEntry *de;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,o,REDIS_ZSET)) return;

    zs = o->ptr;
    de = dictFind(zs->dict,c->argv[2]);
    if (!de) {
        addReply(c,shared.nullbulk);
    } else {
        double *score = dictGetEntryVal(de);

        addReplyDouble(c,*score);
    }
}

static void zrankGenericCommand(redisClient *c, int reverse) {
    robj *o;
    zset *zs;
    zskiplist *zsl;
    dictEntry *de;
    unsigned long rank;
    double *score;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,o,REDIS_ZSET)) return;

    zs = o->ptr;
    zsl = zs->zsl;
    de = dictFind(zs->dict,c->argv[2]);
    if (!de) {
        addReply(c,shared.nullbulk);
        return;
    }

    score = dictGetEntryVal(de);
    rank = zslGetRank(zsl, *score, c->argv[2]);
    if (rank) {
        if (reverse) {
            addReplyLongLong(c, zsl->length - rank);
        } else {
            addReplyLongLong(c, rank-1);
        }
    } else {
        addReply(c,shared.nullbulk);
    }
}

static void zrankCommand(redisClient *c) {
    zrankGenericCommand(c, 0);
}

static void zrevrankCommand(redisClient *c) {
    zrankGenericCommand(c, 1);
}

/* ========================= Hashes utility functions ======================= */
#define REDIS_HASH_KEY 1
#define REDIS_HASH_VALUE 2

/* Check the length of a number of objects to see if we need to convert a
 * zipmap to a real hash. Note that we only check string encoded objects
 * as their string length can be queried in constant time. */
static void hashTryConversion(robj *subject, robj **argv, int start, int end) {
    int i;
    if (subject->encoding != REDIS_ENCODING_ZIPMAP) return;

    for (i = start; i <= end; i++) {
        if (argv[i]->encoding == REDIS_ENCODING_RAW &&
            sdslen(argv[i]->ptr) > server.hash_max_zipmap_value)
        {
            convertToRealHash(subject);
            return;
        }
    }
}

/* Encode given objects in-place when the hash uses a dict. */
static void hashTryObjectEncoding(robj *subject, robj **o1, robj **o2) {
    if (subject->encoding == REDIS_ENCODING_HT) {
        if (o1) *o1 = tryObjectEncoding(*o1);
        if (o2) *o2 = tryObjectEncoding(*o2);
    }
}

/* Get the value from a hash identified by key. Returns either a string
 * object or NULL if the value cannot be found. The refcount of the object
 * is always increased by 1 when the value was found. */
static robj *hashGet(robj *o, robj *key) {
    robj *value = NULL;
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        unsigned char *v;
        unsigned int vlen;
        key = getDecodedObject(key);
        if (zipmapGet(o->ptr,key->ptr,sdslen(key->ptr),&v,&vlen)) {
            value = createStringObject((char*)v,vlen);
        }
        decrRefCount(key);
    } else {
        dictEntry *de = dictFind(o->ptr,key);
        if (de != NULL) {
            value = dictGetEntryVal(de);
            incrRefCount(value);
        }
    }
    return value;
}

/* Test if the key exists in the given hash. Returns 1 if the key
 * exists and 0 when it doesn't. */
static int hashExists(robj *o, robj *key) {
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        key = getDecodedObject(key);
        if (zipmapExists(o->ptr,key->ptr,sdslen(key->ptr))) {
            decrRefCount(key);
            return 1;
        }
        decrRefCount(key);
    } else {
        if (dictFind(o->ptr,key) != NULL) {
            return 1;
        }
    }
    return 0;
}

/* Add an element, discard the old if the key already exists.
 * Return 0 on insert and 1 on update. */
static int hashSet(robj *o, robj *key, robj *value) {
    int update = 0;
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        key = getDecodedObject(key);
        value = getDecodedObject(value);
        o->ptr = zipmapSet(o->ptr,
            key->ptr,sdslen(key->ptr),
            value->ptr,sdslen(value->ptr), &update);
        decrRefCount(key);
        decrRefCount(value);

        /* Check if the zipmap needs to be upgraded to a real hash table */
        if (zipmapLen(o->ptr) > server.hash_max_zipmap_entries)
            convertToRealHash(o);
    } else {
        if (dictReplace(o->ptr,key,value)) {
            /* Insert */
            incrRefCount(key);
        } else {
            /* Update */
            update = 1;
        }
        incrRefCount(value);
    }
    return update;
}

/* Delete an element from a hash.
 * Return 1 on deleted and 0 on not found. */
static int hashDelete(robj *o, robj *key) {
    int deleted = 0;
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        key = getDecodedObject(key);
        o->ptr = zipmapDel(o->ptr,key->ptr,sdslen(key->ptr), &deleted);
        decrRefCount(key);
    } else {
        deleted = dictDelete((dict*)o->ptr,key) == DICT_OK;
        /* Always check if the dictionary needs a resize after a delete. */
        if (deleted && htNeedsResize(o->ptr)) dictResize(o->ptr);
    }
    return deleted;
}

/* Return the number of elements in a hash. */
static unsigned long hashLength(robj *o) {
    return (o->encoding == REDIS_ENCODING_ZIPMAP) ?
        zipmapLen((unsigned char*)o->ptr) : dictSize((dict*)o->ptr);
}

/* Structure to hold hash iteration abstration. Note that iteration over
 * hashes involves both fields and values. Because it is possible that
 * not both are required, store pointers in the iterator to avoid
 * unnecessary memory allocation for fields/values. */
typedef struct {
    int encoding;
    unsigned char *zi;
    unsigned char *zk, *zv;
    unsigned int zklen, zvlen;

    dictIterator *di;
    dictEntry *de;
} hashIterator;

static hashIterator *hashInitIterator(robj *subject) {
    hashIterator *hi = zmalloc(sizeof(hashIterator));
    hi->encoding = subject->encoding;
    if (hi->encoding == REDIS_ENCODING_ZIPMAP) {
        hi->zi = zipmapRewind(subject->ptr);
    } else if (hi->encoding == REDIS_ENCODING_HT) {
        hi->di = dictGetIterator(subject->ptr);
    } else {
        redisAssert(NULL);
    }
    return hi;
}

static void hashReleaseIterator(hashIterator *hi) {
    if (hi->encoding == REDIS_ENCODING_HT) {
        dictReleaseIterator(hi->di);
    }
    zfree(hi);
}

/* Move to the next entry in the hash. Return REDIS_OK when the next entry
 * could be found and REDIS_ERR when the iterator reaches the end. */
static int hashNext(hashIterator *hi) {
    if (hi->encoding == REDIS_ENCODING_ZIPMAP) {
        if ((hi->zi = zipmapNext(hi->zi, &hi->zk, &hi->zklen,
            &hi->zv, &hi->zvlen)) == NULL) return REDIS_ERR;
    } else {
        if ((hi->de = dictNext(hi->di)) == NULL) return REDIS_ERR;
    }
    return REDIS_OK;
}

/* Get key or value object at current iteration position.
 * This increases the refcount of the field object by 1. */
static robj *hashCurrent(hashIterator *hi, int what) {
    robj *o;
    if (hi->encoding == REDIS_ENCODING_ZIPMAP) {
        if (what & REDIS_HASH_KEY) {
            o = createStringObject((char*)hi->zk,hi->zklen);
        } else {
            o = createStringObject((char*)hi->zv,hi->zvlen);
        }
    } else {
        if (what & REDIS_HASH_KEY) {
            o = dictGetEntryKey(hi->de);
        } else {
            o = dictGetEntryVal(hi->de);
        }
        incrRefCount(o);
    }
    return o;
}

static robj *hashLookupWriteOrCreate(redisClient *c, robj *key) {
    robj *o = lookupKeyWrite(c->db,key);
    if (o == NULL) {
        o = createHashObject();
        dictAdd(c->db->dict,key,o);
        incrRefCount(key);
    } else {
        if (o->type != REDIS_HASH) {
            addReply(c,shared.wrongtypeerr);
            return NULL;
        }
    }
    return o;
}

/* ============================= Hash commands ============================== */
static void hsetCommand(redisClient *c) {
    int update;
    robj *o;

    if ((o = hashLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    hashTryConversion(o,c->argv,2,3);
    hashTryObjectEncoding(o,&c->argv[2], &c->argv[3]);
    update = hashSet(o,c->argv[2],c->argv[3]);
    addReply(c, update ? shared.czero : shared.cone);
    server.dirty++;
}

static void hsetnxCommand(redisClient *c) {
    robj *o;
    if ((o = hashLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    hashTryConversion(o,c->argv,2,3);

    if (hashExists(o, c->argv[2])) {
        addReply(c, shared.czero);
    } else {
        hashTryObjectEncoding(o,&c->argv[2], &c->argv[3]);
        hashSet(o,c->argv[2],c->argv[3]);
        addReply(c, shared.cone);
        server.dirty++;
    }
}

static void hmsetCommand(redisClient *c) {
    int i;
    robj *o;

    if ((c->argc % 2) == 1) {
        addReplySds(c,sdsnew("-ERR wrong number of arguments for HMSET\r\n"));
        return;
    }

    if ((o = hashLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    hashTryConversion(o,c->argv,2,c->argc-1);
    for (i = 2; i < c->argc; i += 2) {
        hashTryObjectEncoding(o,&c->argv[i], &c->argv[i+1]);
        hashSet(o,c->argv[i],c->argv[i+1]);
    }
    addReply(c, shared.ok);
    server.dirty++;
}

static void hincrbyCommand(redisClient *c) {
    long long value, incr;
    robj *o, *current, *new;

    if (getLongLongFromObjectOrReply(c,c->argv[3],&incr,NULL) != REDIS_OK) return;
    if ((o = hashLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    if ((current = hashGet(o,c->argv[2])) != NULL) {
        if (getLongLongFromObjectOrReply(c,current,&value,
            "hash value is not an integer") != REDIS_OK) {
            decrRefCount(current);
            return;
        }
        decrRefCount(current);
    } else {
        value = 0;
    }

    value += incr;
    new = createStringObjectFromLongLong(value);
    hashTryObjectEncoding(o,&c->argv[2],NULL);
    hashSet(o,c->argv[2],new);
    decrRefCount(new);
    addReplyLongLong(c,value);
    server.dirty++;
}

static void hgetCommand(redisClient *c) {
    robj *o, *value;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;

    if ((value = hashGet(o,c->argv[2])) != NULL) {
        addReplyBulk(c,value);
        decrRefCount(value);
    } else {
        addReply(c,shared.nullbulk);
    }
}

static void hmgetCommand(redisClient *c) {
    int i;
    robj *o, *value;
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o != NULL && o->type != REDIS_HASH) {
        addReply(c,shared.wrongtypeerr);
    }

    /* Note the check for o != NULL happens inside the loop. This is
     * done because objects that cannot be found are considered to be
     * an empty hash. The reply should then be a series of NULLs. */
    addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",c->argc-2));
    for (i = 2; i < c->argc; i++) {
        if (o != NULL && (value = hashGet(o,c->argv[i])) != NULL) {
            addReplyBulk(c,value);
            decrRefCount(value);
        } else {
            addReply(c,shared.nullbulk);
        }
    }
}

static void hdelCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;

    if (hashDelete(o,c->argv[2])) {
        if (hashLength(o) == 0) deleteKey(c->db,c->argv[1]);
        addReply(c,shared.cone);
        server.dirty++;
    } else {
        addReply(c,shared.czero);
    }
}

static void hlenCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;

    addReplyUlong(c,hashLength(o));
}

static void genericHgetallCommand(redisClient *c, int flags) {
    robj *o, *lenobj, *obj;
    unsigned long count = 0;
    hashIterator *hi;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
        || checkType(c,o,REDIS_HASH)) return;

    lenobj = createObject(REDIS_STRING,NULL);
    addReply(c,lenobj);
    decrRefCount(lenobj);

    hi = hashInitIterator(o);
    while (hashNext(hi) != REDIS_ERR) {
        if (flags & REDIS_HASH_KEY) {
            obj = hashCurrent(hi,REDIS_HASH_KEY);
            addReplyBulk(c,obj);
            decrRefCount(obj);
            count++;
        }
        if (flags & REDIS_HASH_VALUE) {
            obj = hashCurrent(hi,REDIS_HASH_VALUE);
            addReplyBulk(c,obj);
            decrRefCount(obj);
            count++;
        }
    }
    hashReleaseIterator(hi);

    lenobj->ptr = sdscatprintf(sdsempty(),"*%lu\r\n",count);
}

static void hkeysCommand(redisClient *c) {
    genericHgetallCommand(c,REDIS_HASH_KEY);
}

static void hvalsCommand(redisClient *c) {
    genericHgetallCommand(c,REDIS_HASH_VALUE);
}

static void hgetallCommand(redisClient *c) {
    genericHgetallCommand(c,REDIS_HASH_KEY|REDIS_HASH_VALUE);
}

static void hexistsCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;

    addReply(c, hashExists(o,c->argv[2]) ? shared.cone : shared.czero);
}

static void convertToRealHash(robj *o) {
    unsigned char *key, *val, *p, *zm = o->ptr;
    unsigned int klen, vlen;
    dict *dict = dictCreate(&hashDictType,NULL);

    assert(o->type == REDIS_HASH && o->encoding != REDIS_ENCODING_HT);
    p = zipmapRewind(zm);
    while((p = zipmapNext(p,&key,&klen,&val,&vlen)) != NULL) {
        robj *keyobj, *valobj;

        keyobj = createStringObject((char*)key,klen);
        valobj = createStringObject((char*)val,vlen);
        keyobj = tryObjectEncoding(keyobj);
        valobj = tryObjectEncoding(valobj);
        dictAdd(dict,keyobj,valobj);
    }
    o->encoding = REDIS_ENCODING_HT;
    o->ptr = dict;
    zfree(zm);
}

/* ========================= Non type-specific commands  ==================== */

static void flushdbCommand(redisClient *c) {
    server.dirty += dictSize(c->db->dict);
    touchWatchedKeysOnFlush(c->db->id);
    dictEmpty(c->db->dict);
    dictEmpty(c->db->expires);
    addReply(c,shared.ok);
}

static void flushallCommand(redisClient *c) {
    touchWatchedKeysOnFlush(-1);
    server.dirty += emptyDb();
    addReply(c,shared.ok);
    if (server.bgsavechildpid != -1) {
        kill(server.bgsavechildpid,SIGKILL);
        rdbRemoveTempFile(server.bgsavechildpid);
    }
    rdbSave(server.dbfilename);
    server.dirty++;
}

static redisSortOperation *createSortOperation(int type, robj *pattern) {
    redisSortOperation *so = zmalloc(sizeof(*so));
    so->type = type;
    so->pattern = pattern;
    return so;
}

/* Return the value associated to the key with a name obtained
 * substituting the first occurence of '*' in 'pattern' with 'subst'.
 * The returned object will always have its refcount increased by 1
 * when it is non-NULL. */
static robj *lookupKeyByPattern(redisDb *db, robj *pattern, robj *subst) {
    char *p, *f;
    sds spat, ssub;
    robj keyobj, fieldobj, *o;
    int prefixlen, sublen, postfixlen, fieldlen;
    /* Expoit the internal sds representation to create a sds string allocated on the stack in order to make this function faster */
    struct {
        long len;
        long free;
        char buf[REDIS_SORTKEY_MAX+1];
    } keyname, fieldname;

    /* If the pattern is "#" return the substitution object itself in order
     * to implement the "SORT ... GET #" feature. */
    spat = pattern->ptr;
    if (spat[0] == '#' && spat[1] == '\0') {
        incrRefCount(subst);
        return subst;
    }

    /* The substitution object may be specially encoded. If so we create
     * a decoded object on the fly. Otherwise getDecodedObject will just
     * increment the ref count, that we'll decrement later. */
    subst = getDecodedObject(subst);

    ssub = subst->ptr;
    if (sdslen(spat)+sdslen(ssub)-1 > REDIS_SORTKEY_MAX) return NULL;
    p = strchr(spat,'*');
    if (!p) {
        decrRefCount(subst);
        return NULL;
    }

    /* Find out if we're dealing with a hash dereference. */
    if ((f = strstr(p+1, "->")) != NULL) {
        fieldlen = sdslen(spat)-(f-spat);
        /* this also copies \0 character */
        memcpy(fieldname.buf,f+2,fieldlen-1);
        fieldname.len = fieldlen-2;
    } else {
        fieldlen = 0;
    }

    prefixlen = p-spat;
    sublen = sdslen(ssub);
    postfixlen = sdslen(spat)-(prefixlen+1)-fieldlen;
    memcpy(keyname.buf,spat,prefixlen);
    memcpy(keyname.buf+prefixlen,ssub,sublen);
    memcpy(keyname.buf+prefixlen+sublen,p+1,postfixlen);
    keyname.buf[prefixlen+sublen+postfixlen] = '\0';
    keyname.len = prefixlen+sublen+postfixlen;
    decrRefCount(subst);

    /* Lookup substituted key */
    initStaticStringObject(keyobj,((char*)&keyname)+(sizeof(long)*2));
    o = lookupKeyRead(db,&keyobj);
    if (o == NULL) return NULL;

    if (fieldlen > 0) {
        if (o->type != REDIS_HASH || fieldname.len < 1) return NULL;

        /* Retrieve value from hash by the field name. This operation
         * already increases the refcount of the returned object. */
        initStaticStringObject(fieldobj,((char*)&fieldname)+(sizeof(long)*2));
        o = hashGet(o, &fieldobj);
    } else {
        if (o->type != REDIS_STRING) return NULL;

        /* Every object that this function returns needs to have its refcount
         * increased. sortCommand decreases it again. */
        incrRefCount(o);
    }

    return o;
}

/* sortCompare() is used by qsort in sortCommand(). Given that qsort_r with
 * the additional parameter is not standard but a BSD-specific we have to
 * pass sorting parameters via the global 'server' structure */
static int sortCompare(const void *s1, const void *s2) {
    const redisSortObject *so1 = s1, *so2 = s2;
    int cmp;

    if (!server.sort_alpha) {
        /* Numeric sorting. Here it's trivial as we precomputed scores */
        if (so1->u.score > so2->u.score) {
            cmp = 1;
        } else if (so1->u.score < so2->u.score) {
            cmp = -1;
        } else {
            cmp = 0;
        }
    } else {
        /* Alphanumeric sorting */
        if (server.sort_bypattern) {
            if (!so1->u.cmpobj || !so2->u.cmpobj) {
                /* At least one compare object is NULL */
                if (so1->u.cmpobj == so2->u.cmpobj)
                    cmp = 0;
                else if (so1->u.cmpobj == NULL)
                    cmp = -1;
                else
                    cmp = 1;
            } else {
                /* We have both the objects, use strcoll */
                cmp = strcoll(so1->u.cmpobj->ptr,so2->u.cmpobj->ptr);
            }
        } else {
            /* Compare elements directly. */
            cmp = compareStringObjects(so1->obj,so2->obj);
        }
    }
    return server.sort_desc ? -cmp : cmp;
}

/* The SORT command is the most complex command in Redis. Warning: this code
 * is optimized for speed and a bit less for readability */
static void sortCommand(redisClient *c) {
    list *operations;
    int outputlen = 0;
    int desc = 0, alpha = 0;
    int limit_start = 0, limit_count = -1, start, end;
    int j, dontsort = 0, vectorlen;
    int getop = 0; /* GET operation counter */
    robj *sortval, *sortby = NULL, *storekey = NULL;
    redisSortObject *vector; /* Resulting vector to sort */

    /* Lookup the key to sort. It must be of the right types */
    sortval = lookupKeyRead(c->db,c->argv[1]);
    if (sortval == NULL) {
        addReply(c,shared.emptymultibulk);
        return;
    }
    if (sortval->type != REDIS_SET && sortval->type != REDIS_LIST &&
        sortval->type != REDIS_ZSET)
    {
        addReply(c,shared.wrongtypeerr);
        return;
    }

    /* Create a list of operations to perform for every sorted element.
     * Operations can be GET/DEL/INCR/DECR */
    operations = listCreate();
    listSetFreeMethod(operations,zfree);
    j = 2;

    /* Now we need to protect sortval incrementing its count, in the future
     * SORT may have options able to overwrite/delete keys during the sorting
     * and the sorted key itself may get destroied */
    incrRefCount(sortval);

    /* The SORT command has an SQL-alike syntax, parse it */
    while(j < c->argc) {
        int leftargs = c->argc-j-1;
        if (!strcasecmp(c->argv[j]->ptr,"asc")) {
            desc = 0;
        } else if (!strcasecmp(c->argv[j]->ptr,"desc")) {
            desc = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"alpha")) {
            alpha = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"limit") && leftargs >= 2) {
            limit_start = atoi(c->argv[j+1]->ptr);
            limit_count = atoi(c->argv[j+2]->ptr);
            j+=2;
        } else if (!strcasecmp(c->argv[j]->ptr,"store") && leftargs >= 1) {
            storekey = c->argv[j+1];
            j++;
        } else if (!strcasecmp(c->argv[j]->ptr,"by") && leftargs >= 1) {
            sortby = c->argv[j+1];
            /* If the BY pattern does not contain '*', i.e. it is constant,
             * we don't need to sort nor to lookup the weight keys. */
            if (strchr(c->argv[j+1]->ptr,'*') == NULL) dontsort = 1;
            j++;
        } else if (!strcasecmp(c->argv[j]->ptr,"get") && leftargs >= 1) {
            listAddNodeTail(operations,createSortOperation(
                REDIS_SORT_GET,c->argv[j+1]));
            getop++;
            j++;
        } else {
            decrRefCount(sortval);
            listRelease(operations);
            addReply(c,shared.syntaxerr);
            return;
        }
        j++;
    }

    /* Load the sorting vector with all the objects to sort */
    switch(sortval->type) {
    case REDIS_LIST: vectorlen = listLength((list*)sortval->ptr); break;
    case REDIS_SET: vectorlen =  dictSize((dict*)sortval->ptr); break;
    case REDIS_ZSET: vectorlen = dictSize(((zset*)sortval->ptr)->dict); break;
    default: vectorlen = 0; redisPanic("Bad SORT type"); /* Avoid GCC warning */
    }
    vector = zmalloc(sizeof(redisSortObject)*vectorlen);
    j = 0;

    if (sortval->type == REDIS_LIST) {
        list *list = sortval->ptr;
        listNode *ln;
        listIter li;

        listRewind(list,&li);
        while((ln = listNext(&li))) {
            robj *ele = ln->value;
            vector[j].obj = ele;
            vector[j].u.score = 0;
            vector[j].u.cmpobj = NULL;
            j++;
        }
    } else {
        dict *set;
        dictIterator *di;
        dictEntry *setele;

        if (sortval->type == REDIS_SET) {
            set = sortval->ptr;
        } else {
            zset *zs = sortval->ptr;
            set = zs->dict;
        }

        di = dictGetIterator(set);
        while((setele = dictNext(di)) != NULL) {
            vector[j].obj = dictGetEntryKey(setele);
            vector[j].u.score = 0;
            vector[j].u.cmpobj = NULL;
            j++;
        }
        dictReleaseIterator(di);
    }
    redisAssert(j == vectorlen);

    /* Now it's time to load the right scores in the sorting vector */
    if (dontsort == 0) {
        for (j = 0; j < vectorlen; j++) {
            robj *byval;
            if (sortby) {
                /* lookup value to sort by */
                byval = lookupKeyByPattern(c->db,sortby,vector[j].obj);
                if (!byval) continue;
            } else {
                /* use object itself to sort by */
                byval = vector[j].obj;
            }

            if (alpha) {
                if (sortby) vector[j].u.cmpobj = getDecodedObject(byval);
            } else {
                if (byval->encoding == REDIS_ENCODING_RAW) {
                    vector[j].u.score = strtod(byval->ptr,NULL);
                } else if (byval->encoding == REDIS_ENCODING_INT) {
                    /* Don't need to decode the object if it's
                     * integer-encoded (the only encoding supported) so
                     * far. We can just cast it */
                    vector[j].u.score = (long)byval->ptr;
                } else {
                    redisAssert(1 != 1);
                }
            }

            /* when the object was retrieved using lookupKeyByPattern,
             * its refcount needs to be decreased. */
            if (sortby) {
                decrRefCount(byval);
            }
        }
    }

    /* We are ready to sort the vector... perform a bit of sanity check
     * on the LIMIT option too. We'll use a partial version of quicksort. */
    start = (limit_start < 0) ? 0 : limit_start;
    end = (limit_count < 0) ? vectorlen-1 : start+limit_count-1;
    if (start >= vectorlen) {
        start = vectorlen-1;
        end = vectorlen-2;
    }
    if (end >= vectorlen) end = vectorlen-1;

    if (dontsort == 0) {
        server.sort_desc = desc;
        server.sort_alpha = alpha;
        server.sort_bypattern = sortby ? 1 : 0;
        if (sortby && (start != 0 || end != vectorlen-1))
            pqsort(vector,vectorlen,sizeof(redisSortObject),sortCompare, start,end);
        else
            qsort(vector,vectorlen,sizeof(redisSortObject),sortCompare);
    }

    /* Send command output to the output buffer, performing the specified
     * GET/DEL/INCR/DECR operations if any. */
    outputlen = getop ? getop*(end-start+1) : end-start+1;
    if (storekey == NULL) {
        /* STORE option not specified, sent the sorting result to client */
        addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",outputlen));
        for (j = start; j <= end; j++) {
            listNode *ln;
            listIter li;

            if (!getop) addReplyBulk(c,vector[j].obj);
            listRewind(operations,&li);
            while((ln = listNext(&li))) {
                redisSortOperation *sop = ln->value;
                robj *val = lookupKeyByPattern(c->db,sop->pattern,
                    vector[j].obj);

                if (sop->type == REDIS_SORT_GET) {
                    if (!val) {
                        addReply(c,shared.nullbulk);
                    } else {
                        addReplyBulk(c,val);
                        decrRefCount(val);
                    }
                } else {
                    redisAssert(sop->type == REDIS_SORT_GET); /* always fails */
                }
            }
        }
    } else {
        robj *listObject = createListObject();
        list *listPtr = (list*) listObject->ptr;

        /* STORE option specified, set the sorting result as a List object */
        for (j = start; j <= end; j++) {
            listNode *ln;
            listIter li;

            if (!getop) {
                listAddNodeTail(listPtr,vector[j].obj);
                incrRefCount(vector[j].obj);
            }
            listRewind(operations,&li);
            while((ln = listNext(&li))) {
                redisSortOperation *sop = ln->value;
                robj *val = lookupKeyByPattern(c->db,sop->pattern,
                    vector[j].obj);

                if (sop->type == REDIS_SORT_GET) {
                    if (!val) {
                        listAddNodeTail(listPtr,createStringObject("",0));
                    } else {
                        /* We should do a incrRefCount on val because it is
                         * added to the list, but also a decrRefCount because
                         * it is returned by lookupKeyByPattern. This results
                         * in doing nothing at all. */
                        listAddNodeTail(listPtr,val);
                    }
                } else {
                    redisAssert(sop->type == REDIS_SORT_GET); /* always fails */
                }
            }
        }
        if (dictReplace(c->db->dict,storekey,listObject)) {
            incrRefCount(storekey);
        }
        /* Note: we add 1 because the DB is dirty anyway since even if the
         * SORT result is empty a new key is set and maybe the old content
         * replaced. */
        server.dirty += 1+outputlen;
        addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",outputlen));
    }

    /* Cleanup */
    decrRefCount(sortval);
    listRelease(operations);
    for (j = 0; j < vectorlen; j++) {
        if (alpha && vector[j].u.cmpobj)
            decrRefCount(vector[j].u.cmpobj);
    }
    zfree(vector);
}

/* Convert an amount of bytes into a human readable string in the form
 * of 100B, 2G, 100M, 4K, and so forth. */
static void bytesToHuman(char *s, unsigned long long n) {
    double d;

    if (n < 1024) {
        /* Bytes */
        sprintf(s,"%lluB",n);
        return;
    } else if (n < (1024*1024)) {
        d = (double)n/(1024);
        sprintf(s,"%.2fK",d);
    } else if (n < (1024LL*1024*1024)) {
        d = (double)n/(1024*1024);
        sprintf(s,"%.2fM",d);
    } else if (n < (1024LL*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024);
        sprintf(s,"%.2fG",d);
    }
}

/* Create the string returned by the INFO command. This is decoupled
 * by the INFO command itself as we need to report the same information
 * on memory corruption problems. */
static sds genRedisInfoString(void) {
    sds info;
    time_t uptime = time(NULL)-server.stat_starttime;
    int j;
    char hmem[64];

    bytesToHuman(hmem,zmalloc_used_memory());
    info = sdscatprintf(sdsempty(),
        "redis_version:%s\r\n"
        "redis_git_sha1:%s\r\n"
        "redis_git_dirty:%d\r\n"
        "arch_bits:%s\r\n"
        "multiplexing_api:%s\r\n"
        "process_id:%ld\r\n"
        "uptime_in_seconds:%ld\r\n"
        "uptime_in_days:%ld\r\n"
        "connected_clients:%d\r\n"
        "connected_slaves:%d\r\n"
        "blocked_clients:%d\r\n"
        "used_memory:%zu\r\n"
        "used_memory_human:%s\r\n"
        "changes_since_last_save:%lld\r\n"
        "bgsave_in_progress:%d\r\n"
        "last_save_time:%ld\r\n"
        "bgrewriteaof_in_progress:%d\r\n"
        "total_connections_received:%lld\r\n"
        "total_commands_processed:%lld\r\n"
        "expired_keys:%lld\r\n"
        "hash_max_zipmap_entries:%zu\r\n"
        "hash_max_zipmap_value:%zu\r\n"
        "pubsub_channels:%ld\r\n"
        "pubsub_patterns:%u\r\n"
        "vm_enabled:%d\r\n"
        "role:%s\r\n"
        ,REDIS_VERSION,
        REDIS_GIT_SHA1,
        strtol(REDIS_GIT_DIRTY,NULL,10) > 0,
        (sizeof(long) == 8) ? "64" : "32",
        aeGetApiName(),
        (long) getpid(),
        uptime,
        uptime/(3600*24),
        listLength(server.clients)-listLength(server.slaves),
        listLength(server.slaves),
        server.blpop_blocked_clients,
        zmalloc_used_memory(),
        hmem,
        server.dirty,
        server.bgsavechildpid != -1,
        server.lastsave,
        server.bgrewritechildpid != -1,
        server.stat_numconnections,
        server.stat_numcommands,
        server.stat_expiredkeys,
        server.hash_max_zipmap_entries,
        server.hash_max_zipmap_value,
        dictSize(server.pubsub_channels),
        listLength(server.pubsub_patterns),
        server.vm_enabled != 0,
        server.masterhost == NULL ? "master" : "slave"
    );
    if (server.masterhost) {
        info = sdscatprintf(info,
            "master_host:%s\r\n"
            "master_port:%d\r\n"
            "master_link_status:%s\r\n"
            "master_last_io_seconds_ago:%d\r\n"
            ,server.masterhost,
            server.masterport,
            (server.replstate == REDIS_REPL_CONNECTED) ?
                "up" : "down",
            server.master ? ((int)(time(NULL)-server.master->lastinteraction)) : -1
        );
    }
    if (server.vm_enabled) {
        lockThreadedIO();
        info = sdscatprintf(info,
            "vm_conf_max_memory:%llu\r\n"
            "vm_conf_page_size:%llu\r\n"
            "vm_conf_pages:%llu\r\n"
            "vm_stats_used_pages:%llu\r\n"
            "vm_stats_swapped_objects:%llu\r\n"
            "vm_stats_swappin_count:%llu\r\n"
            "vm_stats_swappout_count:%llu\r\n"
            "vm_stats_io_newjobs_len:%lu\r\n"
            "vm_stats_io_processing_len:%lu\r\n"
            "vm_stats_io_processed_len:%lu\r\n"
            "vm_stats_io_active_threads:%lu\r\n"
            "vm_stats_blocked_clients:%lu\r\n"
            ,(unsigned long long) server.vm_max_memory,
            (unsigned long long) server.vm_page_size,
            (unsigned long long) server.vm_pages,
            (unsigned long long) server.vm_stats_used_pages,
            (unsigned long long) server.vm_stats_swapped_objects,
            (unsigned long long) server.vm_stats_swapins,
            (unsigned long long) server.vm_stats_swapouts,
            (unsigned long) listLength(server.io_newjobs),
            (unsigned long) listLength(server.io_processing),
            (unsigned long) listLength(server.io_processed),
            (unsigned long) server.io_active_threads,
            (unsigned long) server.vm_blocked_clients
        );
        unlockThreadedIO();
    }
    for (j = 0; j < server.dbnum; j++) {
        long long keys, vkeys;

        keys = dictSize(server.db[j].dict);
        vkeys = dictSize(server.db[j].expires);
        if (keys || vkeys) {
            info = sdscatprintf(info, "db%d:keys=%lld,expires=%lld\r\n",
                j, keys, vkeys);
        }
    }
    return info;
}

static void infoCommand(redisClient *c) {
    sds info = genRedisInfoString();
    addReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n",
        (unsigned long)sdslen(info)));
    addReplySds(c,info);
    addReply(c,shared.crlf);
}

static void monitorCommand(redisClient *c) {
    /* ignore MONITOR if aleady slave or in monitor mode */
    if (c->flags & REDIS_SLAVE) return;

    c->flags |= (REDIS_SLAVE|REDIS_MONITOR);
    c->slaveseldb = 0;
    listAddNodeTail(server.monitors,c);
    addReply(c,shared.ok);
}

/* ================================= Expire ================================= */
static int removeExpire(redisDb *db, robj *key) {
    if (dictDelete(db->expires,key) == DICT_OK) {
        return 1;
    } else {
        return 0;
    }
}

static int setExpire(redisDb *db, robj *key, time_t when) {
    if (dictAdd(db->expires,key,(void*)when) == DICT_ERR) {
        return 0;
    } else {
        incrRefCount(key);
        return 1;
    }
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) */
static time_t getExpire(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP */
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key)) == NULL) return -1;

    return (time_t) dictGetEntryVal(de);
}

static int expireIfNeeded(redisDb *db, robj *key) {
    time_t when;
    dictEntry *de;

    /* No expire? return ASAP */
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key)) == NULL) return 0;

    /* Lookup the expire */
    when = (time_t) dictGetEntryVal(de);
    if (time(NULL) <= when) return 0;

    /* Delete the key */
    dictDelete(db->expires,key);
    server.stat_expiredkeys++;
    return dictDelete(db->dict,key) == DICT_OK;
}

static int deleteIfVolatile(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP */
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key)) == NULL) return 0;

    /* Delete the key */
    server.dirty++;
    server.stat_expiredkeys++;
    dictDelete(db->expires,key);
    return dictDelete(db->dict,key) == DICT_OK;
}

static void expireGenericCommand(redisClient *c, robj *key, robj *param, long offset) {
    dictEntry *de;
    time_t seconds;

    if (getLongFromObjectOrReply(c, param, &seconds, NULL) != REDIS_OK) return;

    seconds -= offset;

    de = dictFind(c->db->dict,key);
    if (de == NULL) {
        addReply(c,shared.czero);
        return;
    }
    if (seconds <= 0) {
        if (deleteKey(c->db,key)) server.dirty++;
        addReply(c, shared.cone);
        return;
    } else {
        time_t when = time(NULL)+seconds;
        if (setExpire(c->db,key,when)) {
            addReply(c,shared.cone);
            server.dirty++;
        } else {
            addReply(c,shared.czero);
        }
        return;
    }
}

static void expireCommand(redisClient *c) {
    expireGenericCommand(c,c->argv[1],c->argv[2],0);
}

static void expireatCommand(redisClient *c) {
    expireGenericCommand(c,c->argv[1],c->argv[2],time(NULL));
}

static void ttlCommand(redisClient *c) {
    time_t expire;
    int ttl = -1;

    expire = getExpire(c->db,c->argv[1]);
    if (expire != -1) {
        ttl = (int) (expire-time(NULL));
        if (ttl < 0) ttl = -1;
    }
    addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",ttl));
}

/* ================================ MULTI/EXEC ============================== */

/* Client state initialization for MULTI/EXEC */
static void initClientMultiState(redisClient *c) {
    c->mstate.commands = NULL;
    c->mstate.count = 0;
}

/* Release all the resources associated with MULTI/EXEC state */
static void freeClientMultiState(redisClient *c) {
    int j;

    for (j = 0; j < c->mstate.count; j++) {
        int i;
        multiCmd *mc = c->mstate.commands+j;

        for (i = 0; i < mc->argc; i++)
            decrRefCount(mc->argv[i]);
        zfree(mc->argv);
    }
    zfree(c->mstate.commands);
}

/* Add a new command into the MULTI commands queue */
static void queueMultiCommand(redisClient *c, struct redisCommand *cmd) {
    multiCmd *mc;
    int j;

    c->mstate.commands = zrealloc(c->mstate.commands,
            sizeof(multiCmd)*(c->mstate.count+1));
    mc = c->mstate.commands+c->mstate.count;
    mc->cmd = cmd;
    mc->argc = c->argc;
    mc->argv = zmalloc(sizeof(robj*)*c->argc);
    memcpy(mc->argv,c->argv,sizeof(robj*)*c->argc);
    for (j = 0; j < c->argc; j++)
        incrRefCount(mc->argv[j]);
    c->mstate.count++;
}

static void multiCommand(redisClient *c) {
    if (c->flags & REDIS_MULTI) {
        addReplySds(c,sdsnew("-ERR MULTI calls can not be nested\r\n"));
        return;
    }
    c->flags |= REDIS_MULTI;
    addReply(c,shared.ok);
}

static void discardCommand(redisClient *c) {
    if (!(c->flags & REDIS_MULTI)) {
        addReplySds(c,sdsnew("-ERR DISCARD without MULTI\r\n"));
        return;
    }

    freeClientMultiState(c);
    initClientMultiState(c);
    c->flags &= (~REDIS_MULTI);
    addReply(c,shared.ok);
}

/* Send a MULTI command to all the slaves and AOF file. Check the execCommand
 * implememntation for more information. */
static void execCommandReplicateMulti(redisClient *c) {
    struct redisCommand *cmd;
    robj *multistring = createStringObject("MULTI",5);

    cmd = lookupCommand("multi");
    if (server.appendonly)
        feedAppendOnlyFile(cmd,c->db->id,&multistring,1);
    if (listLength(server.slaves))
        replicationFeedSlaves(server.slaves,c->db->id,&multistring,1);
    decrRefCount(multistring);
}

static void execCommand(redisClient *c) {
    int j;
    robj **orig_argv;
    int orig_argc;

    if (!(c->flags & REDIS_MULTI)) {
        addReplySds(c,sdsnew("-ERR EXEC without MULTI\r\n"));
        return;
    }

    /* Check if we need to abort the EXEC if some WATCHed key was touched.
     * A failed EXEC will return a multi bulk nil object. */
    if (c->flags & REDIS_DIRTY_CAS) {
        freeClientMultiState(c);
        initClientMultiState(c);
        c->flags &= ~(REDIS_MULTI|REDIS_DIRTY_CAS);
        unwatchAllKeys(c);
        addReply(c,shared.nullmultibulk);
        return;
    }

    /* Replicate a MULTI request now that we are sure the block is executed.
     * This way we'll deliver the MULTI/..../EXEC block as a whole and
     * both the AOF and the replication link will have the same consistency
     * and atomicity guarantees. */
    execCommandReplicateMulti(c);

    /* Exec all the queued commands */
    orig_argv = c->argv;
    orig_argc = c->argc;
    addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",c->mstate.count));
    for (j = 0; j < c->mstate.count; j++) {
        c->argc = c->mstate.commands[j].argc;
        c->argv = c->mstate.commands[j].argv;
        call(c,c->mstate.commands[j].cmd);
    }
    c->argv = orig_argv;
    c->argc = orig_argc;
    freeClientMultiState(c);
    initClientMultiState(c);
    c->flags &= (~REDIS_MULTI);
    unwatchAllKeys(c);
    /* Make sure the EXEC command is always replicated / AOF, since we
     * always send the MULTI command (we can't know beforehand if the
     * next operations will contain at least a modification to the DB). */
    server.dirty++;
}

/* =========================== Blocking Operations  ========================= */

/* Currently Redis blocking operations support is limited to list POP ops,
 * so the current implementation is not fully generic, but it is also not
 * completely specific so it will not require a rewrite to support new
 * kind of blocking operations in the future.
 *
 * Still it's important to note that list blocking operations can be already
 * used as a notification mechanism in order to implement other blocking
 * operations at application level, so there must be a very strong evidence
 * of usefulness and generality before new blocking operations are implemented.
 *
 * This is how the current blocking POP works, we use BLPOP as example:
 * - If the user calls BLPOP and the key exists and contains a non empty list
 *   then LPOP is called instead. So BLPOP is semantically the same as LPOP
 *   if there is not to block.
 * - If instead BLPOP is called and the key does not exists or the list is
 *   empty we need to block. In order to do so we remove the notification for
 *   new data to read in the client socket (so that we'll not serve new
 *   requests if the blocking request is not served). Also we put the client
 *   in a dictionary (db->blocking_keys) mapping keys to a list of clients
 *   blocking for this keys.
 * - If a PUSH operation against a key with blocked clients waiting is
 *   performed, we serve the first in the list: basically instead to push
 *   the new element inside the list we return it to the (first / oldest)
 *   blocking client, unblock the client, and remove it form the list.
 *
 * The above comment and the source code should be enough in order to understand
 * the implementation and modify / fix it later.
 */

/* Set a client in blocking mode for the specified key, with the specified
 * timeout */
static void blockForKeys(redisClient *c, robj **keys, int numkeys, time_t timeout) {
    dictEntry *de;
    list *l;
    int j;

    c->blocking_keys = zmalloc(sizeof(robj*)*numkeys);
    c->blocking_keys_num = numkeys;
    c->blockingto = timeout;
    for (j = 0; j < numkeys; j++) {
        /* Add the key in the client structure, to map clients -> keys */
        c->blocking_keys[j] = keys[j];
        incrRefCount(keys[j]);

        /* And in the other "side", to map keys -> clients */
        de = dictFind(c->db->blocking_keys,keys[j]);
        if (de == NULL) {
            int retval;

            /* For every key we take a list of clients blocked for it */
            l = listCreate();
            retval = dictAdd(c->db->blocking_keys,keys[j],l);
            incrRefCount(keys[j]);
            assert(retval == DICT_OK);
        } else {
            l = dictGetEntryVal(de);
        }
        listAddNodeTail(l,c);
    }
    /* Mark the client as a blocked client */
    c->flags |= REDIS_BLOCKED;
    server.blpop_blocked_clients++;
}

/* Unblock a client that's waiting in a blocking operation such as BLPOP */
static void unblockClientWaitingData(redisClient *c) {
    dictEntry *de;
    list *l;
    int j;

    assert(c->blocking_keys != NULL);
    /* The client may wait for multiple keys, so unblock it for every key. */
    for (j = 0; j < c->blocking_keys_num; j++) {
        /* Remove this client from the list of clients waiting for this key. */
        de = dictFind(c->db->blocking_keys,c->blocking_keys[j]);
        assert(de != NULL);
        l = dictGetEntryVal(de);
        listDelNode(l,listSearchKey(l,c));
        /* If the list is empty we need to remove it to avoid wasting memory */
        if (listLength(l) == 0)
            dictDelete(c->db->blocking_keys,c->blocking_keys[j]);
        decrRefCount(c->blocking_keys[j]);
    }
    /* Cleanup the client structure */
    zfree(c->blocking_keys);
    c->blocking_keys = NULL;
    c->flags &= (~REDIS_BLOCKED);
    server.blpop_blocked_clients--;
    /* We want to process data if there is some command waiting
     * in the input buffer. Note that this is safe even if
     * unblockClientWaitingData() gets called from freeClient() because
     * freeClient() will be smart enough to call this function
     * *after* c->querybuf was set to NULL. */
    if (c->querybuf && sdslen(c->querybuf) > 0) processInputBuffer(c);
}

/* This should be called from any function PUSHing into lists.
 * 'c' is the "pushing client", 'key' is the key it is pushing data against,
 * 'ele' is the element pushed.
 *
 * If the function returns 0 there was no client waiting for a list push
 * against this key.
 *
 * If the function returns 1 there was a client waiting for a list push
 * against this key, the element was passed to this client thus it's not
 * needed to actually add it to the list and the caller should return asap. */
static int handleClientsWaitingListPush(redisClient *c, robj *key, robj *ele) {
    struct dictEntry *de;
    redisClient *receiver;
    list *l;
    listNode *ln;

    de = dictFind(c->db->blocking_keys,key);
    if (de == NULL) return 0;
    l = dictGetEntryVal(de);
    ln = listFirst(l);
    assert(ln != NULL);
    receiver = ln->value;

    addReplySds(receiver,sdsnew("*2\r\n"));
    addReplyBulk(receiver,key);
    addReplyBulk(receiver,ele);
    unblockClientWaitingData(receiver);
    return 1;
}

/* Blocking RPOP/LPOP */
static void blockingPopGenericCommand(redisClient *c, int where) {
    robj *o;
    time_t timeout;
    int j;

    for (j = 1; j < c->argc-1; j++) {
        o = lookupKeyWrite(c->db,c->argv[j]);
        if (o != NULL) {
            if (o->type != REDIS_LIST) {
                addReply(c,shared.wrongtypeerr);
                return;
            } else {
                list *list = o->ptr;
                if (listLength(list) != 0) {
                    /* If the list contains elements fall back to the usual
                     * non-blocking POP operation */
                    robj *argv[2], **orig_argv;
                    int orig_argc;

                    /* We need to alter the command arguments before to call
                     * popGenericCommand() as the command takes a single key. */
                    orig_argv = c->argv;
                    orig_argc = c->argc;
                    argv[1] = c->argv[j];
                    c->argv = argv;
                    c->argc = 2;

                    /* Also the return value is different, we need to output
                     * the multi bulk reply header and the key name. The
                     * "real" command will add the last element (the value)
                     * for us. If this souds like an hack to you it's just
                     * because it is... */
                    addReplySds(c,sdsnew("*2\r\n"));
                    addReplyBulk(c,argv[1]);
                    popGenericCommand(c,where);

                    /* Fix the client structure with the original stuff */
                    c->argv = orig_argv;
                    c->argc = orig_argc;
                    return;
                }
            }
        }
    }
    /* If the list is empty or the key does not exists we must block */
    timeout = strtol(c->argv[c->argc-1]->ptr,NULL,10);
    if (timeout > 0) timeout += time(NULL);
    blockForKeys(c,c->argv+1,c->argc-2,timeout);
}

static void blpopCommand(redisClient *c) {
    blockingPopGenericCommand(c,REDIS_HEAD);
}

static void brpopCommand(redisClient *c) {
    blockingPopGenericCommand(c,REDIS_TAIL);
}

/* =============================== Replication  ============================= */

static int syncWrite(int fd, char *ptr, ssize_t size, int timeout) {
    ssize_t nwritten, ret = size;
    time_t start = time(NULL);

    timeout++;
    while(size) {
        if (aeWait(fd,AE_WRITABLE,1000) & AE_WRITABLE) {
            nwritten = write(fd,ptr,size);
            if (nwritten == -1) return -1;
            ptr += nwritten;
            size -= nwritten;
        }
        if ((time(NULL)-start) > timeout) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
    return ret;
}

static int syncRead(int fd, char *ptr, ssize_t size, int timeout) {
    ssize_t nread, totread = 0;
    time_t start = time(NULL);

    timeout++;
    while(size) {
        if (aeWait(fd,AE_READABLE,1000) & AE_READABLE) {
            nread = read(fd,ptr,size);
            if (nread == -1) return -1;
            ptr += nread;
            size -= nread;
            totread += nread;
        }
        if ((time(NULL)-start) > timeout) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
    return totread;
}

static int syncReadLine(int fd, char *ptr, ssize_t size, int timeout) {
    ssize_t nread = 0;

    size--;
    while(size) {
        char c;

        if (syncRead(fd,&c,1,timeout) == -1) return -1;
        if (c == '\n') {
            *ptr = '\0';
            if (nread && *(ptr-1) == '\r') *(ptr-1) = '\0';
            return nread;
        } else {
            *ptr++ = c;
            *ptr = '\0';
            nread++;
        }
    }
    return nread;
}

static void syncCommand(redisClient *c) {
    /* ignore SYNC if aleady slave or in monitor mode */
    if (c->flags & REDIS_SLAVE) return;

    /* SYNC can't be issued when the server has pending data to send to
     * the client about already issued commands. We need a fresh reply
     * buffer registering the differences between the BGSAVE and the current
     * dataset, so that we can copy to other slaves if needed. */
    if (listLength(c->reply) != 0) {
        addReplySds(c,sdsnew("-ERR SYNC is invalid with pending input\r\n"));
        return;
    }

    redisLog(REDIS_NOTICE,"Slave ask for synchronization");
    /* Here we need to check if there is a background saving operation
     * in progress, or if it is required to start one */
    if (server.bgsavechildpid != -1) {
        /* Ok a background save is in progress. Let's check if it is a good
         * one for replication, i.e. if there is another slave that is
         * registering differences since the server forked to save */
        redisClient *slave;
        listNode *ln;
        listIter li;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            slave = ln->value;
            if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) break;
        }
        if (ln) {
            /* Perfect, the server is already registering differences for
             * another slave. Set the right state, and copy the buffer. */
            listRelease(c->reply);
            c->reply = listDup(slave->reply);
            c->replstate = REDIS_REPL_WAIT_BGSAVE_END;
            redisLog(REDIS_NOTICE,"Waiting for end of BGSAVE for SYNC");
        } else {
            /* No way, we need to wait for the next BGSAVE in order to
             * register differences */
            c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
            redisLog(REDIS_NOTICE,"Waiting for next BGSAVE for SYNC");
        }
    } else {
        /* Ok we don't have a BGSAVE in progress, let's start one */
        redisLog(REDIS_NOTICE,"Starting BGSAVE for SYNC");
        if (rdbSaveBackground(server.dbfilename) != REDIS_OK) {
            redisLog(REDIS_NOTICE,"Replication failed, can't BGSAVE");
            addReplySds(c,sdsnew("-ERR Unalbe to perform background save\r\n"));
            return;
        }
        c->replstate = REDIS_REPL_WAIT_BGSAVE_END;
    }
    c->repldbfd = -1;
    c->flags |= REDIS_SLAVE;
    c->slaveseldb = 0;
    listAddNodeTail(server.slaves,c);
    return;
}

static void sendBulkToSlave(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *slave = privdata;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    char buf[REDIS_IOBUF_LEN];
    ssize_t nwritten, buflen;

    if (slave->repldboff == 0) {
        /* Write the bulk write count before to transfer the DB. In theory here
         * we don't know how much room there is in the output buffer of the
         * socket, but in pratice SO_SNDLOWAT (the minimum count for output
         * operations) will never be smaller than the few bytes we need. */
        sds bulkcount;

        bulkcount = sdscatprintf(sdsempty(),"$%lld\r\n",(unsigned long long)
            slave->repldbsize);
        if (write(fd,bulkcount,sdslen(bulkcount)) != (signed)sdslen(bulkcount))
        {
            sdsfree(bulkcount);
            freeClient(slave);
            return;
        }
        sdsfree(bulkcount);
    }
    lseek(slave->repldbfd,slave->repldboff,SEEK_SET);
    buflen = read(slave->repldbfd,buf,REDIS_IOBUF_LEN);
    if (buflen <= 0) {
        redisLog(REDIS_WARNING,"Read error sending DB to slave: %s",
            (buflen == 0) ? "premature EOF" : strerror(errno));
        freeClient(slave);
        return;
    }
    if ((nwritten = write(fd,buf,buflen)) == -1) {
        redisLog(REDIS_VERBOSE,"Write error sending DB to slave: %s",
            strerror(errno));
        freeClient(slave);
        return;
    }
    slave->repldboff += nwritten;
    if (slave->repldboff == slave->repldbsize) {
        close(slave->repldbfd);
        slave->repldbfd = -1;
        aeDeleteFileEvent(server.el,slave->fd,AE_WRITABLE);
        slave->replstate = REDIS_REPL_ONLINE;
        if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE,
            sendReplyToClient, slave) == AE_ERR) {
            freeClient(slave);
            return;
        }
        addReplySds(slave,sdsempty());
        redisLog(REDIS_NOTICE,"Synchronization with slave succeeded");
    }
}

/* This function is called at the end of every backgrond saving.
 * The argument bgsaveerr is REDIS_OK if the background saving succeeded
 * otherwise REDIS_ERR is passed to the function.
 *
 * The goal of this function is to handle slaves waiting for a successful
 * background saving in order to perform non-blocking synchronization. */
static void updateSlavesWaitingBgsave(int bgsaveerr) {
    listNode *ln;
    int startbgsave = 0;
    listIter li;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;

        if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) {
            startbgsave = 1;
            slave->replstate = REDIS_REPL_WAIT_BGSAVE_END;
        } else if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) {
            struct redis_stat buf;

            if (bgsaveerr != REDIS_OK) {
                freeClient(slave);
                redisLog(REDIS_WARNING,"SYNC failed. BGSAVE child returned an error");
                continue;
            }
            if ((slave->repldbfd = open(server.dbfilename,O_RDONLY)) == -1 ||
                redis_fstat(slave->repldbfd,&buf) == -1) {
                freeClient(slave);
                redisLog(REDIS_WARNING,"SYNC failed. Can't open/stat DB after BGSAVE: %s", strerror(errno));
                continue;
            }
            slave->repldboff = 0;
            slave->repldbsize = buf.st_size;
            slave->replstate = REDIS_REPL_SEND_BULK;
            aeDeleteFileEvent(server.el,slave->fd,AE_WRITABLE);
            if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE, sendBulkToSlave, slave) == AE_ERR) {
                freeClient(slave);
                continue;
            }
        }
    }
    if (startbgsave) {
        if (rdbSaveBackground(server.dbfilename) != REDIS_OK) {
            listIter li;

            listRewind(server.slaves,&li);
            redisLog(REDIS_WARNING,"SYNC failed. BGSAVE failed");
            while((ln = listNext(&li))) {
                redisClient *slave = ln->value;

                if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START)
                    freeClient(slave);
            }
        }
    }
}

static int syncWithMaster(void) {
    char buf[1024], tmpfile[256], authcmd[1024];
    long dumpsize;
    int fd = anetTcpConnect(NULL,server.masterhost,server.masterport);
    int dfd, maxtries = 5;

    if (fd == -1) {
        redisLog(REDIS_WARNING,"Unable to connect to MASTER: %s",
            strerror(errno));
        return REDIS_ERR;
    }

    /* AUTH with the master if required. */
    if(server.masterauth) {
    	snprintf(authcmd, 1024, "AUTH %s\r\n", server.masterauth);
    	if (syncWrite(fd, authcmd, strlen(server.masterauth)+7, 5) == -1) {
            close(fd);
            redisLog(REDIS_WARNING,"Unable to AUTH to MASTER: %s",
                strerror(errno));
            return REDIS_ERR;
    	}
        /* Read the AUTH result.  */
        if (syncReadLine(fd,buf,1024,3600) == -1) {
            close(fd);
            redisLog(REDIS_WARNING,"I/O error reading auth result from MASTER: %s",
                strerror(errno));
            return REDIS_ERR;
        }
        if (buf[0] != '+') {
            close(fd);
            redisLog(REDIS_WARNING,"Cannot AUTH to MASTER, is the masterauth password correct?");
            return REDIS_ERR;
        }
    }

    /* Issue the SYNC command */
    if (syncWrite(fd,"SYNC \r\n",7,5) == -1) {
        close(fd);
        redisLog(REDIS_WARNING,"I/O error writing to MASTER: %s",
            strerror(errno));
        return REDIS_ERR;
    }
    /* Read the bulk write count */
    if (syncReadLine(fd,buf,1024,3600) == -1) {
        close(fd);
        redisLog(REDIS_WARNING,"I/O error reading bulk count from MASTER: %s",
            strerror(errno));
        return REDIS_ERR;
    }
    if (buf[0] != '$') {
        close(fd);
        redisLog(REDIS_WARNING,"Bad protocol from MASTER, the first byte is not '$', are you sure the host and port are right?");
        return REDIS_ERR;
    }
    dumpsize = strtol(buf+1,NULL,10);
    redisLog(REDIS_NOTICE,"Receiving %ld bytes data dump from MASTER",dumpsize);
    /* Read the bulk write data on a temp file */
    while(maxtries--) {
        snprintf(tmpfile,256,
            "temp-%d.%ld.rdb",(int)time(NULL),(long int)getpid());
        dfd = open(tmpfile,O_CREAT|O_WRONLY|O_EXCL,0644);
        if (dfd != -1) break;
        sleep(1);
    }
    if (dfd == -1) {
        close(fd);
        redisLog(REDIS_WARNING,"Opening the temp file needed for MASTER <-> SLAVE synchronization: %s",strerror(errno));
        return REDIS_ERR;
    }
    while(dumpsize) {
        int nread, nwritten;

        nread = read(fd,buf,(dumpsize < 1024)?dumpsize:1024);
        if (nread == -1) {
            redisLog(REDIS_WARNING,"I/O error trying to sync with MASTER: %s",
                strerror(errno));
            close(fd);
            close(dfd);
            return REDIS_ERR;
        }
        nwritten = write(dfd,buf,nread);
        if (nwritten == -1) {
            redisLog(REDIS_WARNING,"Write error writing to the DB dump file needed for MASTER <-> SLAVE synchrnonization: %s", strerror(errno));
            close(fd);
            close(dfd);
            return REDIS_ERR;
        }
        dumpsize -= nread;
    }
    close(dfd);
    if (rename(tmpfile,server.dbfilename) == -1) {
        redisLog(REDIS_WARNING,"Failed trying to rename the temp DB into dump.rdb in MASTER <-> SLAVE synchronization: %s", strerror(errno));
        unlink(tmpfile);
        close(fd);
        return REDIS_ERR;
    }
    emptyDb();
    if (rdbLoad(server.dbfilename) != REDIS_OK) {
        redisLog(REDIS_WARNING,"Failed trying to load the MASTER synchronization DB from disk");
        close(fd);
        return REDIS_ERR;
    }
    server.master = createClient(fd);
    server.master->flags |= REDIS_MASTER;
    server.master->authenticated = 1;
    server.replstate = REDIS_REPL_CONNECTED;
    return REDIS_OK;
}

static void slaveofCommand(redisClient *c) {
    if (!strcasecmp(c->argv[1]->ptr,"no") &&
        !strcasecmp(c->argv[2]->ptr,"one")) {
        if (server.masterhost) {
            sdsfree(server.masterhost);
            server.masterhost = NULL;
            if (server.master) freeClient(server.master);
            server.replstate = REDIS_REPL_NONE;
            redisLog(REDIS_NOTICE,"MASTER MODE enabled (user request)");
        }
    } else {
        sdsfree(server.masterhost);
        server.masterhost = sdsdup(c->argv[1]->ptr);
        server.masterport = atoi(c->argv[2]->ptr);
        if (server.master) freeClient(server.master);
        server.replstate = REDIS_REPL_CONNECT;
        redisLog(REDIS_NOTICE,"SLAVE OF %s:%d enabled (user request)",
            server.masterhost, server.masterport);
    }
    addReply(c,shared.ok);
}

/* ============================ Maxmemory directive  ======================== */

/* Try to free one object form the pre-allocated objects free list.
 * This is useful under low mem conditions as by default we take 1 million
 * free objects allocated. On success REDIS_OK is returned, otherwise
 * REDIS_ERR. */
static int tryFreeOneObjectFromFreelist(void) {
    robj *o;

    if (server.vm_enabled) pthread_mutex_lock(&server.obj_freelist_mutex);
    if (listLength(server.objfreelist)) {
        listNode *head = listFirst(server.objfreelist);
        o = listNodeValue(head);
        listDelNode(server.objfreelist,head);
        if (server.vm_enabled) pthread_mutex_unlock(&server.obj_freelist_mutex);
        zfree(o);
        return REDIS_OK;
    } else {
        if (server.vm_enabled) pthread_mutex_unlock(&server.obj_freelist_mutex);
        return REDIS_ERR;
    }
}

/* This function gets called when 'maxmemory' is set on the config file to limit
 * the max memory used by the server, and we are out of memory.
 * This function will try to, in order:
 *
 * - Free objects from the free list
 * - Try to remove keys with an EXPIRE set
 *
 * It is not possible to free enough memory to reach used-memory < maxmemory
 * the server will start refusing commands that will enlarge even more the
 * memory usage.
 */
static void freeMemoryIfNeeded(void) {
    while (server.maxmemory && zmalloc_used_memory() > server.maxmemory) {
        int j, k, freed = 0;

        if (tryFreeOneObjectFromFreelist() == REDIS_OK) continue;
        for (j = 0; j < server.dbnum; j++) {
            int minttl = -1;
            robj *minkey = NULL;
            struct dictEntry *de;

            if (dictSize(server.db[j].expires)) {
                freed = 1;
                /* From a sample of three keys drop the one nearest to
                 * the natural expire */
                for (k = 0; k < 3; k++) {
                    time_t t;

                    de = dictGetRandomKey(server.db[j].expires);
                    t = (time_t) dictGetEntryVal(de);
                    if (minttl == -1 || t < minttl) {
                        minkey = dictGetEntryKey(de);
                        minttl = t;
                    }
                }
                deleteKey(server.db+j,minkey);
            }
        }
        if (!freed) return; /* nothing to free... */
    }
}

/* ============================== Append Only file ========================== */

/* Write the append only file buffer on disk.
 *
 * Since we are required to write the AOF before replying to the client,
 * and the only way the client socket can get a write is entering when the
 * the event loop, we accumulate all the AOF writes in a memory
 * buffer and write it on disk using this function just before entering
 * the event loop again. */
static void flushAppendOnlyFile(void) {
    time_t now;
    ssize_t nwritten;

    if (sdslen(server.aofbuf) == 0) return;

    /* We want to perform a single write. This should be guaranteed atomic
     * at least if the filesystem we are writing is a real physical one.
     * While this will save us against the server being killed I don't think
     * there is much to do about the whole server stopping for power problems
     * or alike */
     nwritten = write(server.appendfd,server.aofbuf,sdslen(server.aofbuf));
     if (nwritten != (signed)sdslen(server.aofbuf)) {
        /* Ooops, we are in troubles. The best thing to do for now is
         * aborting instead of giving the illusion that everything is
         * working as expected. */
         if (nwritten == -1) {
            redisLog(REDIS_WARNING,"Exiting on error writing to the append-only file: %s",strerror(errno));
         } else {
            redisLog(REDIS_WARNING,"Exiting on short write while writing to the append-only file: %s",strerror(errno));
         }
         exit(1);
    }
    sdsfree(server.aofbuf);
    server.aofbuf = sdsempty();

    /* Fsync if needed */
    now = time(NULL);
    if (server.appendfsync == APPENDFSYNC_ALWAYS ||
        (server.appendfsync == APPENDFSYNC_EVERYSEC &&
         now-server.lastfsync > 1))
    {
        /* aof_fsync is defined as fdatasync() for Linux in order to avoid
         * flushing metadata. */
        aof_fsync(server.appendfd); /* Let's try to get this data on the disk */
        server.lastfsync = now;
    }
}

static sds catAppendOnlyGenericCommand(sds buf, int argc, robj **argv) {
    int j;
    buf = sdscatprintf(buf,"*%d\r\n",argc);
    for (j = 0; j < argc; j++) {
        robj *o = getDecodedObject(argv[j]);
        buf = sdscatprintf(buf,"$%lu\r\n",(unsigned long)sdslen(o->ptr));
        buf = sdscatlen(buf,o->ptr,sdslen(o->ptr));
        buf = sdscatlen(buf,"\r\n",2);
        decrRefCount(o);
    }
    return buf;
}

static sds catAppendOnlyExpireAtCommand(sds buf, robj *key, robj *seconds) {
    int argc = 3;
    long when;
    robj *argv[3];

    /* Make sure we can use strtol */
    seconds = getDecodedObject(seconds);
    when = time(NULL)+strtol(seconds->ptr,NULL,10);
    decrRefCount(seconds);

    argv[0] = createStringObject("EXPIREAT",8);
    argv[1] = key;
    argv[2] = createObject(REDIS_STRING,
        sdscatprintf(sdsempty(),"%ld",when));
    buf = catAppendOnlyGenericCommand(buf, argc, argv);
    decrRefCount(argv[0]);
    decrRefCount(argv[2]);
    return buf;
}

static void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc) {
    sds buf = sdsempty();
    robj *tmpargv[3];

    /* The DB this command was targetting is not the same as the last command
     * we appendend. To issue a SELECT command is needed. */
    if (dictid != server.appendseldb) {
        char seldb[64];

        snprintf(seldb,sizeof(seldb),"%d",dictid);
        buf = sdscatprintf(buf,"*2\r\n$6\r\nSELECT\r\n$%lu\r\n%s\r\n",
            (unsigned long)strlen(seldb),seldb);
        server.appendseldb = dictid;
    }

    if (cmd->proc == expireCommand) {
        /* Translate EXPIRE into EXPIREAT */
        buf = catAppendOnlyExpireAtCommand(buf,argv[1],argv[2]);
    } else if (cmd->proc == setexCommand) {
        /* Translate SETEX to SET and EXPIREAT */
        tmpargv[0] = createStringObject("SET",3);
        tmpargv[1] = argv[1];
        tmpargv[2] = argv[3];
        buf = catAppendOnlyGenericCommand(buf,3,tmpargv);
        decrRefCount(tmpargv[0]);
        buf = catAppendOnlyExpireAtCommand(buf,argv[1],argv[2]);
    } else {
        buf = catAppendOnlyGenericCommand(buf,argc,argv);
    }

    /* Append to the AOF buffer. This will be flushed on disk just before
     * of re-entering the event loop, so before the client will get a
     * positive reply about the operation performed. */
    server.aofbuf = sdscatlen(server.aofbuf,buf,sdslen(buf));

    /* If a background append only file rewriting is in progress we want to
     * accumulate the differences between the child DB and the current one
     * in a buffer, so that when the child process will do its work we
     * can append the differences to the new append only file. */
    if (server.bgrewritechildpid != -1)
        server.bgrewritebuf = sdscatlen(server.bgrewritebuf,buf,sdslen(buf));

    sdsfree(buf);
}

/* In Redis commands are always executed in the context of a client, so in
 * order to load the append only file we need to create a fake client. */
static struct redisClient *createFakeClient(void) {
    struct redisClient *c = zmalloc(sizeof(*c));

    selectDb(c,0);
    c->fd = -1;
    c->querybuf = sdsempty();
    c->argc = 0;
    c->argv = NULL;
    c->flags = 0;
    /* We set the fake client as a slave waiting for the synchronization
     * so that Redis will not try to send replies to this client. */
    c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
    c->reply = listCreate();
    listSetFreeMethod(c->reply,decrRefCount);
    listSetDupMethod(c->reply,dupClientReplyValue);
    initClientMultiState(c);
    return c;
}

static void freeFakeClient(struct redisClient *c) {
    sdsfree(c->querybuf);
    listRelease(c->reply);
    freeClientMultiState(c);
    zfree(c);
}

/* Replay the append log file. On error REDIS_OK is returned. On non fatal
 * error (the append only file is zero-length) REDIS_ERR is returned. On
 * fatal error an error message is logged and the program exists. */
int loadAppendOnlyFile(char *filename) {
    struct redisClient *fakeClient;
    FILE *fp = fopen(filename,"r");
    struct redis_stat sb;
    unsigned long long loadedkeys = 0;
    int appendonly = server.appendonly;

    if (redis_fstat(fileno(fp),&sb) != -1 && sb.st_size == 0)
        return REDIS_ERR;

    if (fp == NULL) {
        redisLog(REDIS_WARNING,"Fatal error: can't open the append log file for reading: %s",strerror(errno));
        exit(1);
    }

    /* Temporarily disable AOF, to prevent EXEC from feeding a MULTI
     * to the same file we're about to read. */
    server.appendonly = 0;

    fakeClient = createFakeClient();
    while(1) {
        int argc, j;
        unsigned long len;
        robj **argv;
        char buf[128];
        sds argsds;
        struct redisCommand *cmd;

        if (fgets(buf,sizeof(buf),fp) == NULL) {
            if (feof(fp))
                break;
            else
                goto readerr;
        }
        if (buf[0] != '*') goto fmterr;
        argc = atoi(buf+1);
        argv = zmalloc(sizeof(robj*)*argc);
        for (j = 0; j < argc; j++) {
            if (fgets(buf,sizeof(buf),fp) == NULL) goto readerr;
            if (buf[0] != '$') goto fmterr;
            len = strtol(buf+1,NULL,10);
            argsds = sdsnewlen(NULL,len);
            if (len && fread(argsds,len,1,fp) == 0) goto fmterr;
            argv[j] = createObject(REDIS_STRING,argsds);
            if (fread(buf,2,1,fp) == 0) goto fmterr; /* discard CRLF */
        }

        /* Command lookup */
        cmd = lookupCommand(argv[0]->ptr);
        if (!cmd) {
            redisLog(REDIS_WARNING,"Unknown command '%s' reading the append only file", argv[0]->ptr);
            exit(1);
        }
        /* Try object encoding */
        if (cmd->flags & REDIS_CMD_BULK)
            argv[argc-1] = tryObjectEncoding(argv[argc-1]);
        /* Run the command in the context of a fake client */
        fakeClient->argc = argc;
        fakeClient->argv = argv;
        cmd->proc(fakeClient);
        /* Discard the reply objects list from the fake client */
        while(listLength(fakeClient->reply))
            listDelNode(fakeClient->reply,listFirst(fakeClient->reply));
        /* Clean up, ready for the next command */
        for (j = 0; j < argc; j++) decrRefCount(argv[j]);
        zfree(argv);
        /* Handle swapping while loading big datasets when VM is on */
        loadedkeys++;
        if (server.vm_enabled && (loadedkeys % 5000) == 0) {
            while (zmalloc_used_memory() > server.vm_max_memory) {
                if (vmSwapOneObjectBlocking() == REDIS_ERR) break;
            }
        }
    }

    /* This point can only be reached when EOF is reached without errors.
     * If the client is in the middle of a MULTI/EXEC, log error and quit. */
    if (fakeClient->flags & REDIS_MULTI) goto readerr;

    fclose(fp);
    freeFakeClient(fakeClient);
    server.appendonly = appendonly;
    return REDIS_OK;

readerr:
    if (feof(fp)) {
        redisLog(REDIS_WARNING,"Unexpected end of file reading the append only file");
    } else {
        redisLog(REDIS_WARNING,"Unrecoverable error reading the append only file: %s", strerror(errno));
    }
    exit(1);
fmterr:
    redisLog(REDIS_WARNING,"Bad file format reading the append only file");
    exit(1);
}

/* Write an object into a file in the bulk format $<count>\r\n<payload>\r\n */
static int fwriteBulkObject(FILE *fp, robj *obj) {
    char buf[128];
    int decrrc = 0;

    /* Avoid the incr/decr ref count business if possible to help
     * copy-on-write (we are often in a child process when this function
     * is called).
     * Also makes sure that key objects don't get incrRefCount-ed when VM
     * is enabled */
    if (obj->encoding != REDIS_ENCODING_RAW) {
        obj = getDecodedObject(obj);
        decrrc = 1;
    }
    snprintf(buf,sizeof(buf),"$%ld\r\n",(long)sdslen(obj->ptr));
    if (fwrite(buf,strlen(buf),1,fp) == 0) goto err;
    if (sdslen(obj->ptr) && fwrite(obj->ptr,sdslen(obj->ptr),1,fp) == 0)
        goto err;
    if (fwrite("\r\n",2,1,fp) == 0) goto err;
    if (decrrc) decrRefCount(obj);
    return 1;
err:
    if (decrrc) decrRefCount(obj);
    return 0;
}

/* Write binary-safe string into a file in the bulkformat
 * $<count>\r\n<payload>\r\n */
static int fwriteBulkString(FILE *fp, char *s, unsigned long len) {
    char buf[128];

    snprintf(buf,sizeof(buf),"$%ld\r\n",(unsigned long)len);
    if (fwrite(buf,strlen(buf),1,fp) == 0) return 0;
    if (len && fwrite(s,len,1,fp) == 0) return 0;
    if (fwrite("\r\n",2,1,fp) == 0) return 0;
    return 1;
}

/* Write a double value in bulk format $<count>\r\n<payload>\r\n */
static int fwriteBulkDouble(FILE *fp, double d) {
    char buf[128], dbuf[128];

    snprintf(dbuf,sizeof(dbuf),"%.17g\r\n",d);
    snprintf(buf,sizeof(buf),"$%lu\r\n",(unsigned long)strlen(dbuf)-2);
    if (fwrite(buf,strlen(buf),1,fp) == 0) return 0;
    if (fwrite(dbuf,strlen(dbuf),1,fp) == 0) return 0;
    return 1;
}

/* Write a long value in bulk format $<count>\r\n<payload>\r\n */
static int fwriteBulkLong(FILE *fp, long l) {
    char buf[128], lbuf[128];

    snprintf(lbuf,sizeof(lbuf),"%ld\r\n",l);
    snprintf(buf,sizeof(buf),"$%lu\r\n",(unsigned long)strlen(lbuf)-2);
    if (fwrite(buf,strlen(buf),1,fp) == 0) return 0;
    if (fwrite(lbuf,strlen(lbuf),1,fp) == 0) return 0;
    return 1;
}

/* Write a sequence of commands able to fully rebuild the dataset into
 * "filename". Used both by REWRITEAOF and BGREWRITEAOF. */
static int rewriteAppendOnlyFile(char *filename) {
    dictIterator *di = NULL;
    dictEntry *de;
    FILE *fp;
    char tmpfile[256];
    int j;
    time_t now = time(NULL);

    /* Note that we have to use a different temp name here compared to the
     * one used by rewriteAppendOnlyFileBackground() function. */
    snprintf(tmpfile,256,"temp-rewriteaof-%d.aof", (int) getpid());
    fp = fopen(tmpfile,"w");
    if (!fp) {
        redisLog(REDIS_WARNING, "Failed rewriting the append only file: %s", strerror(errno));
        return REDIS_ERR;
    }
    for (j = 0; j < server.dbnum; j++) {
        char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
        redisDb *db = server.db+j;
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetIterator(d);
        if (!di) {
            fclose(fp);
            return REDIS_ERR;
        }

        /* SELECT the new DB */
        if (fwrite(selectcmd,sizeof(selectcmd)-1,1,fp) == 0) goto werr;
        if (fwriteBulkLong(fp,j) == 0) goto werr;

        /* Iterate this DB writing every entry */
        while((de = dictNext(di)) != NULL) {
            robj *key, *o;
            time_t expiretime;
            int swapped;

            key = dictGetEntryKey(de);
            /* If the value for this key is swapped, load a preview in memory.
             * We use a "swapped" flag to remember if we need to free the
             * value object instead to just increment the ref count anyway
             * in order to avoid copy-on-write of pages if we are forked() */
            if (!server.vm_enabled || key->storage == REDIS_VM_MEMORY ||
                key->storage == REDIS_VM_SWAPPING) {
                o = dictGetEntryVal(de);
                swapped = 0;
            } else {
                o = vmPreviewObject(key);
                swapped = 1;
            }
            expiretime = getExpire(db,key);

            /* Save the key and associated value */
            if (o->type == REDIS_STRING) {
                /* Emit a SET command */
                char cmd[]="*3\r\n$3\r\nSET\r\n";
                if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                /* Key and value */
                if (fwriteBulkObject(fp,key) == 0) goto werr;
                if (fwriteBulkObject(fp,o) == 0) goto werr;
            } else if (o->type == REDIS_LIST) {
                /* Emit the RPUSHes needed to rebuild the list */
                list *list = o->ptr;
                listNode *ln;
                listIter li;

                listRewind(list,&li);
                while((ln = listNext(&li))) {
                    char cmd[]="*3\r\n$5\r\nRPUSH\r\n";
                    robj *eleobj = listNodeValue(ln);

                    if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                    if (fwriteBulkObject(fp,key) == 0) goto werr;
                    if (fwriteBulkObject(fp,eleobj) == 0) goto werr;
                }
            } else if (o->type == REDIS_SET) {
                /* Emit the SADDs needed to rebuild the set */
                dict *set = o->ptr;
                dictIterator *di = dictGetIterator(set);
                dictEntry *de;

                while((de = dictNext(di)) != NULL) {
                    char cmd[]="*3\r\n$4\r\nSADD\r\n";
                    robj *eleobj = dictGetEntryKey(de);

                    if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                    if (fwriteBulkObject(fp,key) == 0) goto werr;
                    if (fwriteBulkObject(fp,eleobj) == 0) goto werr;
                }
                dictReleaseIterator(di);
            } else if (o->type == REDIS_ZSET) {
                /* Emit the ZADDs needed to rebuild the sorted set */
                zset *zs = o->ptr;
                dictIterator *di = dictGetIterator(zs->dict);
                dictEntry *de;

                while((de = dictNext(di)) != NULL) {
                    char cmd[]="*4\r\n$4\r\nZADD\r\n";
                    robj *eleobj = dictGetEntryKey(de);
                    double *score = dictGetEntryVal(de);

                    if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                    if (fwriteBulkObject(fp,key) == 0) goto werr;
                    if (fwriteBulkDouble(fp,*score) == 0) goto werr;
                    if (fwriteBulkObject(fp,eleobj) == 0) goto werr;
                }
                dictReleaseIterator(di);
            } else if (o->type == REDIS_HASH) {
                char cmd[]="*4\r\n$4\r\nHSET\r\n";

                /* Emit the HSETs needed to rebuild the hash */
                if (o->encoding == REDIS_ENCODING_ZIPMAP) {
                    unsigned char *p = zipmapRewind(o->ptr);
                    unsigned char *field, *val;
                    unsigned int flen, vlen;

                    while((p = zipmapNext(p,&field,&flen,&val,&vlen)) != NULL) {
                        if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                        if (fwriteBulkObject(fp,key) == 0) goto werr;
                        if (fwriteBulkString(fp,(char*)field,flen) == -1)
                            return -1;
                        if (fwriteBulkString(fp,(char*)val,vlen) == -1)
                            return -1;
                    }
                } else {
                    dictIterator *di = dictGetIterator(o->ptr);
                    dictEntry *de;

                    while((de = dictNext(di)) != NULL) {
                        robj *field = dictGetEntryKey(de);
                        robj *val = dictGetEntryVal(de);

                        if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                        if (fwriteBulkObject(fp,key) == 0) goto werr;
                        if (fwriteBulkObject(fp,field) == -1) return -1;
                        if (fwriteBulkObject(fp,val) == -1) return -1;
                    }
                    dictReleaseIterator(di);
                }
            } else {
                redisPanic("Unknown object type");
            }
            /* Save the expire time */
            if (expiretime != -1) {
                char cmd[]="*3\r\n$8\r\nEXPIREAT\r\n";
                /* If this key is already expired skip it */
                if (expiretime < now) continue;
                if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                if (fwriteBulkObject(fp,key) == 0) goto werr;
                if (fwriteBulkLong(fp,expiretime) == 0) goto werr;
            }
            if (swapped) decrRefCount(o);
        }
        dictReleaseIterator(di);
    }

    /* Make sure data will not remain on the OS's output buffers */
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    if (rename(tmpfile,filename) == -1) {
        redisLog(REDIS_WARNING,"Error moving temp append only file on the final destination: %s", strerror(errno));
        unlink(tmpfile);
        return REDIS_ERR;
    }
    redisLog(REDIS_NOTICE,"SYNC append only file rewrite performed");
    return REDIS_OK;

werr:
    fclose(fp);
    unlink(tmpfile);
    redisLog(REDIS_WARNING,"Write error writing append only file on disk: %s", strerror(errno));
    if (di) dictReleaseIterator(di);
    return REDIS_ERR;
}

/* This is how rewriting of the append only file in background works:
 *
 * 1) The user calls BGREWRITEAOF
 * 2) Redis calls this function, that forks():
 *    2a) the child rewrite the append only file in a temp file.
 *    2b) the parent accumulates differences in server.bgrewritebuf.
 * 3) When the child finished '2a' exists.
 * 4) The parent will trap the exit code, if it's OK, will append the
 *    data accumulated into server.bgrewritebuf into the temp file, and
 *    finally will rename(2) the temp file in the actual file name.
 *    The the new file is reopened as the new append only file. Profit!
 */
static int rewriteAppendOnlyFileBackground(void) {
    pid_t childpid;

    if (server.bgrewritechildpid != -1) return REDIS_ERR;
    if (server.vm_enabled) waitEmptyIOJobsQueue();
    if ((childpid = fork()) == 0) {
        /* Child */
        char tmpfile[256];

        if (server.vm_enabled) vmReopenSwapFile();
        close(server.fd);
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) getpid());
        if (rewriteAppendOnlyFile(tmpfile) == REDIS_OK) {
            _exit(0);
        } else {
            _exit(1);
        }
    } else {
        /* Parent */
        if (childpid == -1) {
            redisLog(REDIS_WARNING,
                "Can't rewrite append only file in background: fork: %s",
                strerror(errno));
            return REDIS_ERR;
        }
        redisLog(REDIS_NOTICE,
            "Background append only file rewriting started by pid %d",childpid);
        server.bgrewritechildpid = childpid;
        updateDictResizePolicy();
        /* We set appendseldb to -1 in order to force the next call to the
         * feedAppendOnlyFile() to issue a SELECT command, so the differences
         * accumulated by the parent into server.bgrewritebuf will start
         * with a SELECT statement and it will be safe to merge. */
        server.appendseldb = -1;
        return REDIS_OK;
    }
    return REDIS_OK; /* unreached */
}

static void bgrewriteaofCommand(redisClient *c) {
    if (server.bgrewritechildpid != -1) {
        addReplySds(c,sdsnew("-ERR background append only file rewriting already in progress\r\n"));
        return;
    }
    if (rewriteAppendOnlyFileBackground() == REDIS_OK) {
        char *status = "+Background append only file rewriting started\r\n";
        addReplySds(c,sdsnew(status));
    } else {
        addReply(c,shared.err);
    }
}

static void aofRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) childpid);
    unlink(tmpfile);
}

/* Virtual Memory is composed mainly of two subsystems:
 * - Blocking Virutal Memory
 * - Threaded Virtual Memory I/O
 * The two parts are not fully decoupled, but functions are split among two
 * different sections of the source code (delimited by comments) in order to
 * make more clear what functionality is about the blocking VM and what about
 * the threaded (not blocking) VM.
 *
 * Redis VM design:
 *
 * Redis VM is a blocking VM (one that blocks reading swapped values from
 * disk into memory when a value swapped out is needed in memory) that is made
 * unblocking by trying to examine the command argument vector in order to
 * load in background values that will likely be needed in order to exec
 * the command. The command is executed only once all the relevant keys
 * are loaded into memory.
 *
 * This basically is almost as simple of a blocking VM, but almost as parallel
 * as a fully non-blocking VM.
 */

/* Called when the user switches from "appendonly yes" to "appendonly no"
 * at runtime using the CONFIG command. */
static void stopAppendOnly(void) {
    flushAppendOnlyFile();
    fsync(server.appendfd);
    close(server.appendfd);

    server.appendfd = -1;
    server.appendseldb = -1;
    server.appendonly = 0;
    /* rewrite operation in progress? kill it, wait child exit */
    if (server.bgsavechildpid != -1) {
        int statloc;

        if (kill(server.bgsavechildpid,SIGKILL) != -1)
            wait3(&statloc,0,NULL);
        /* reset the buffer accumulating changes while the child saves */
        sdsfree(server.bgrewritebuf);
        server.bgrewritebuf = sdsempty();
        server.bgsavechildpid = -1;
    }
}

/* Called when the user switches from "appendonly no" to "appendonly yes"
 * at runtime using the CONFIG command. */
static int startAppendOnly(void) {
    server.appendonly = 1;
    server.lastfsync = time(NULL);
    server.appendfd = open(server.appendfilename,O_WRONLY|O_APPEND|O_CREAT,0644);
    if (server.appendfd == -1) {
        redisLog(REDIS_WARNING,"Used tried to switch on AOF via CONFIG, but I can't open the AOF file: %s",strerror(errno));
        return REDIS_ERR;
    }
    if (rewriteAppendOnlyFileBackground() == REDIS_ERR) {
        server.appendonly = 0;
        close(server.appendfd);
        redisLog(REDIS_WARNING,"Used tried to switch on AOF via CONFIG, I can't trigger a background AOF rewrite operation. Check the above logs for more info about the error.",strerror(errno));
        return REDIS_ERR;
    }
    return REDIS_OK;
}

/* =================== Virtual Memory - Blocking Side  ====================== */

static void vmInit(void) {
    off_t totsize;
    int pipefds[2];
    size_t stacksize;
    struct flock fl;

    if (server.vm_max_threads != 0)
        zmalloc_enable_thread_safeness(); /* we need thread safe zmalloc() */

    redisLog(REDIS_NOTICE,"Using '%s' as swap file",server.vm_swap_file);
    /* Try to open the old swap file, otherwise create it */
    if ((server.vm_fp = fopen(server.vm_swap_file,"r+b")) == NULL) {
        server.vm_fp = fopen(server.vm_swap_file,"w+b");
    }
    if (server.vm_fp == NULL) {
        redisLog(REDIS_WARNING,
            "Can't open the swap file: %s. Exiting.",
            strerror(errno));
        exit(1);
    }
    server.vm_fd = fileno(server.vm_fp);
    /* Lock the swap file for writing, this is useful in order to avoid
     * another instance to use the same swap file for a config error. */
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = fl.l_len = 0;
    if (fcntl(server.vm_fd,F_SETLK,&fl) == -1) {
        redisLog(REDIS_WARNING,
            "Can't lock the swap file at '%s': %s. Make sure it is not used by another Redis instance.", server.vm_swap_file, strerror(errno));
        exit(1);
    }
    /* Initialize */
    server.vm_next_page = 0;
    server.vm_near_pages = 0;
    server.vm_stats_used_pages = 0;
    server.vm_stats_swapped_objects = 0;
    server.vm_stats_swapouts = 0;
    server.vm_stats_swapins = 0;
    totsize = server.vm_pages*server.vm_page_size;
    redisLog(REDIS_NOTICE,"Allocating %lld bytes of swap file",totsize);
    if (ftruncate(server.vm_fd,totsize) == -1) {
        redisLog(REDIS_WARNING,"Can't ftruncate swap file: %s. Exiting.",
            strerror(errno));
        exit(1);
    } else {
        redisLog(REDIS_NOTICE,"Swap file allocated with success");
    }
    server.vm_bitmap = zmalloc((server.vm_pages+7)/8);
    redisLog(REDIS_VERBOSE,"Allocated %lld bytes page table for %lld pages",
        (long long) (server.vm_pages+7)/8, server.vm_pages);
    memset(server.vm_bitmap,0,(server.vm_pages+7)/8);

    /* Initialize threaded I/O (used by Virtual Memory) */
    server.io_newjobs = listCreate();
    server.io_processing = listCreate();
    server.io_processed = listCreate();
    server.io_ready_clients = listCreate();
    pthread_mutex_init(&server.io_mutex,NULL);
    pthread_mutex_init(&server.obj_freelist_mutex,NULL);
    pthread_mutex_init(&server.io_swapfile_mutex,NULL);
    server.io_active_threads = 0;
    if (pipe(pipefds) == -1) {
        redisLog(REDIS_WARNING,"Unable to intialized VM: pipe(2): %s. Exiting."
            ,strerror(errno));
        exit(1);
    }
    server.io_ready_pipe_read = pipefds[0];
    server.io_ready_pipe_write = pipefds[1];
    redisAssert(anetNonBlock(NULL,server.io_ready_pipe_read) != ANET_ERR);
    /* LZF requires a lot of stack */
    pthread_attr_init(&server.io_threads_attr);
    pthread_attr_getstacksize(&server.io_threads_attr, &stacksize);
    while (stacksize < REDIS_THREAD_STACK_SIZE) stacksize *= 2;
    pthread_attr_setstacksize(&server.io_threads_attr, stacksize);
    /* Listen for events in the threaded I/O pipe */
    if (aeCreateFileEvent(server.el, server.io_ready_pipe_read, AE_READABLE,
        vmThreadedIOCompletedJob, NULL) == AE_ERR)
        oom("creating file event");
}

/* Mark the page as used */
static void vmMarkPageUsed(off_t page) {
    off_t byte = page/8;
    int bit = page&7;
    redisAssert(vmFreePage(page) == 1);
    server.vm_bitmap[byte] |= 1<<bit;
}

/* Mark N contiguous pages as used, with 'page' being the first. */
static void vmMarkPagesUsed(off_t page, off_t count) {
    off_t j;

    for (j = 0; j < count; j++)
        vmMarkPageUsed(page+j);
    server.vm_stats_used_pages += count;
    redisLog(REDIS_DEBUG,"Mark USED pages: %lld pages at %lld\n",
        (long long)count, (long long)page);
}

/* Mark the page as free */
static void vmMarkPageFree(off_t page) {
    off_t byte = page/8;
    int bit = page&7;
    redisAssert(vmFreePage(page) == 0);
    server.vm_bitmap[byte] &= ~(1<<bit);
}

/* Mark N contiguous pages as free, with 'page' being the first. */
static void vmMarkPagesFree(off_t page, off_t count) {
    off_t j;

    for (j = 0; j < count; j++)
        vmMarkPageFree(page+j);
    server.vm_stats_used_pages -= count;
    redisLog(REDIS_DEBUG,"Mark FREE pages: %lld pages at %lld\n",
        (long long)count, (long long)page);
}

/* Test if the page is free */
static int vmFreePage(off_t page) {
    off_t byte = page/8;
    int bit = page&7;
    return (server.vm_bitmap[byte] & (1<<bit)) == 0;
}

/* Find N contiguous free pages storing the first page of the cluster in *first.
 * Returns REDIS_OK if it was able to find N contiguous pages, otherwise
 * REDIS_ERR is returned.
 *
 * This function uses a simple algorithm: we try to allocate
 * REDIS_VM_MAX_NEAR_PAGES sequentially, when we reach this limit we start
 * again from the start of the swap file searching for free spaces.
 *
 * If it looks pretty clear that there are no free pages near our offset
 * we try to find less populated places doing a forward jump of
 * REDIS_VM_MAX_RANDOM_JUMP, then we start scanning again a few pages
 * without hurry, and then we jump again and so forth...
 *
 * This function can be improved using a free list to avoid to guess
 * too much, since we could collect data about freed pages.
 *
 * note: I implemented this function just after watching an episode of
 * Battlestar Galactica, where the hybrid was continuing to say "JUMP!"
 */
static int vmFindContiguousPages(off_t *first, off_t n) {
    off_t base, offset = 0, since_jump = 0, numfree = 0;

    if (server.vm_near_pages == REDIS_VM_MAX_NEAR_PAGES) {
        server.vm_near_pages = 0;
        server.vm_next_page = 0;
    }
    server.vm_near_pages++; /* Yet another try for pages near to the old ones */
    base = server.vm_next_page;

    while(offset < server.vm_pages) {
        off_t this = base+offset;

        /* If we overflow, restart from page zero */
        if (this >= server.vm_pages) {
            this -= server.vm_pages;
            if (this == 0) {
                /* Just overflowed, what we found on tail is no longer
                 * interesting, as it's no longer contiguous. */
                numfree = 0;
            }
        }
        if (vmFreePage(this)) {
            /* This is a free page */
            numfree++;
            /* Already got N free pages? Return to the caller, with success */
            if (numfree == n) {
                *first = this-(n-1);
                server.vm_next_page = this+1;
                redisLog(REDIS_DEBUG, "FOUND CONTIGUOUS PAGES: %lld pages at %lld\n", (long long) n, (long long) *first);
                return REDIS_OK;
            }
        } else {
            /* The current one is not a free page */
            numfree = 0;
        }

        /* Fast-forward if the current page is not free and we already
         * searched enough near this place. */
        since_jump++;
        if (!numfree && since_jump >= REDIS_VM_MAX_RANDOM_JUMP/4) {
            offset += random() % REDIS_VM_MAX_RANDOM_JUMP;
            since_jump = 0;
            /* Note that even if we rewind after the jump, we are don't need
             * to make sure numfree is set to zero as we only jump *if* it
             * is set to zero. */
        } else {
            /* Otherwise just check the next page */
            offset++;
        }
    }
    return REDIS_ERR;
}

/* Write the specified object at the specified page of the swap file */
static int vmWriteObjectOnSwap(robj *o, off_t page) {
    if (server.vm_enabled) pthread_mutex_lock(&server.io_swapfile_mutex);
    if (fseeko(server.vm_fp,page*server.vm_page_size,SEEK_SET) == -1) {
        if (server.vm_enabled) pthread_mutex_unlock(&server.io_swapfile_mutex);
        redisLog(REDIS_WARNING,
            "Critical VM problem in vmWriteObjectOnSwap(): can't seek: %s",
            strerror(errno));
        return REDIS_ERR;
    }
    rdbSaveObject(server.vm_fp,o);
    fflush(server.vm_fp);
    if (server.vm_enabled) pthread_mutex_unlock(&server.io_swapfile_mutex);
    return REDIS_OK;
}

/* Swap the 'val' object relative to 'key' into disk. Store all the information
 * needed to later retrieve the object into the key object.
 * If we can't find enough contiguous empty pages to swap the object on disk
 * REDIS_ERR is returned. */
static int vmSwapObjectBlocking(robj *key, robj *val) {
    off_t pages = rdbSavedObjectPages(val,NULL);
    off_t page;

    assert(key->storage == REDIS_VM_MEMORY);
    assert(key->refcount == 1);
    if (vmFindContiguousPages(&page,pages) == REDIS_ERR) return REDIS_ERR;
    if (vmWriteObjectOnSwap(val,page) == REDIS_ERR) return REDIS_ERR;
    key->vm.page = page;
    key->vm.usedpages = pages;
    key->storage = REDIS_VM_SWAPPED;
    key->vtype = val->type;
    decrRefCount(val); /* Deallocate the object from memory. */
    vmMarkPagesUsed(page,pages);
    redisLog(REDIS_DEBUG,"VM: object %s swapped out at %lld (%lld pages)",
        (unsigned char*) key->ptr,
        (unsigned long long) page, (unsigned long long) pages);
    server.vm_stats_swapped_objects++;
    server.vm_stats_swapouts++;
    return REDIS_OK;
}

static robj *vmReadObjectFromSwap(off_t page, int type) {
    robj *o;

    if (server.vm_enabled) pthread_mutex_lock(&server.io_swapfile_mutex);
    if (fseeko(server.vm_fp,page*server.vm_page_size,SEEK_SET) == -1) {
        redisLog(REDIS_WARNING,
            "Unrecoverable VM problem in vmReadObjectFromSwap(): can't seek: %s",
            strerror(errno));
        _exit(1);
    }
    o = rdbLoadObject(type,server.vm_fp);
    if (o == NULL) {
        redisLog(REDIS_WARNING, "Unrecoverable VM problem in vmReadObjectFromSwap(): can't load object from swap file: %s", strerror(errno));
        _exit(1);
    }
    if (server.vm_enabled) pthread_mutex_unlock(&server.io_swapfile_mutex);
    return o;
}

/* Load the value object relative to the 'key' object from swap to memory.
 * The newly allocated object is returned.
 *
 * If preview is true the unserialized object is returned to the caller but
 * no changes are made to the key object, nor the pages are marked as freed */
static robj *vmGenericLoadObject(robj *key, int preview) {
    robj *val;

    redisAssert(key->storage == REDIS_VM_SWAPPED || key->storage == REDIS_VM_LOADING);
    val = vmReadObjectFromSwap(key->vm.page,key->vtype);
    if (!preview) {
        key->storage = REDIS_VM_MEMORY;
        key->vm.atime = server.unixtime;
        vmMarkPagesFree(key->vm.page,key->vm.usedpages);
        redisLog(REDIS_DEBUG, "VM: object %s loaded from disk",
            (unsigned char*) key->ptr);
        server.vm_stats_swapped_objects--;
    } else {
        redisLog(REDIS_DEBUG, "VM: object %s previewed from disk",
            (unsigned char*) key->ptr);
    }
    server.vm_stats_swapins++;
    return val;
}

/* Plain object loading, from swap to memory */
static robj *vmLoadObject(robj *key) {
    /* If we are loading the object in background, stop it, we
     * need to load this object synchronously ASAP. */
    if (key->storage == REDIS_VM_LOADING)
        vmCancelThreadedIOJob(key);
    return vmGenericLoadObject(key,0);
}

/* Just load the value on disk, without to modify the key.
 * This is useful when we want to perform some operation on the value
 * without to really bring it from swap to memory, like while saving the
 * dataset or rewriting the append only log. */
static robj *vmPreviewObject(robj *key) {
    return vmGenericLoadObject(key,1);
}

/* How a good candidate is this object for swapping?
 * The better candidate it is, the greater the returned value.
 *
 * Currently we try to perform a fast estimation of the object size in
 * memory, and combine it with aging informations.
 *
 * Basically swappability = idle-time * log(estimated size)
 *
 * Bigger objects are preferred over smaller objects, but not
 * proportionally, this is why we use the logarithm. This algorithm is
 * just a first try and will probably be tuned later. */
static double computeObjectSwappability(robj *o) {
    time_t age = server.unixtime - o->vm.atime;
    long asize = 0;
    list *l;
    dict *d;
    struct dictEntry *de;
    int z;

    if (age <= 0) return 0;
    switch(o->type) {
    case REDIS_STRING:
        if (o->encoding != REDIS_ENCODING_RAW) {
            asize = sizeof(*o);
        } else {
            asize = sdslen(o->ptr)+sizeof(*o)+sizeof(long)*2;
        }
        break;
    case REDIS_LIST:
        l = o->ptr;
        listNode *ln = listFirst(l);

        asize = sizeof(list);
        if (ln) {
            robj *ele = ln->value;
            long elesize;

            elesize = (ele->encoding == REDIS_ENCODING_RAW) ?
                            (sizeof(*o)+sdslen(ele->ptr)) :
                            sizeof(*o);
            asize += (sizeof(listNode)+elesize)*listLength(l);
        }
        break;
    case REDIS_SET:
    case REDIS_ZSET:
        z = (o->type == REDIS_ZSET);
        d = z ? ((zset*)o->ptr)->dict : o->ptr;

        asize = sizeof(dict)+(sizeof(struct dictEntry*)*dictSlots(d));
        if (z) asize += sizeof(zset)-sizeof(dict);
        if (dictSize(d)) {
            long elesize;
            robj *ele;

            de = dictGetRandomKey(d);
            ele = dictGetEntryKey(de);
            elesize = (ele->encoding == REDIS_ENCODING_RAW) ?
                            (sizeof(*o)+sdslen(ele->ptr)) :
                            sizeof(*o);
            asize += (sizeof(struct dictEntry)+elesize)*dictSize(d);
            if (z) asize += sizeof(zskiplistNode)*dictSize(d);
        }
        break;
    case REDIS_HASH:
        if (o->encoding == REDIS_ENCODING_ZIPMAP) {
            unsigned char *p = zipmapRewind((unsigned char*)o->ptr);
            unsigned int len = zipmapLen((unsigned char*)o->ptr);
            unsigned int klen, vlen;
            unsigned char *key, *val;

            if ((p = zipmapNext(p,&key,&klen,&val,&vlen)) == NULL) {
                klen = 0;
                vlen = 0;
            }
            asize = len*(klen+vlen+3);
        } else if (o->encoding == REDIS_ENCODING_HT) {
            d = o->ptr;
            asize = sizeof(dict)+(sizeof(struct dictEntry*)*dictSlots(d));
            if (dictSize(d)) {
                long elesize;
                robj *ele;

                de = dictGetRandomKey(d);
                ele = dictGetEntryKey(de);
                elesize = (ele->encoding == REDIS_ENCODING_RAW) ?
                                (sizeof(*o)+sdslen(ele->ptr)) :
                                sizeof(*o);
                ele = dictGetEntryVal(de);
                elesize = (ele->encoding == REDIS_ENCODING_RAW) ?
                                (sizeof(*o)+sdslen(ele->ptr)) :
                                sizeof(*o);
                asize += (sizeof(struct dictEntry)+elesize)*dictSize(d);
            }
        }
        break;
    }
    return (double)age*log(1+asize);
}

/* Try to swap an object that's a good candidate for swapping.
 * Returns REDIS_OK if the object was swapped, REDIS_ERR if it's not possible
 * to swap any object at all.
 *
 * If 'usethreaded' is true, Redis will try to swap the object in background
 * using I/O threads. */
static int vmSwapOneObject(int usethreads) {
    int j, i;
    struct dictEntry *best = NULL;
    double best_swappability = 0;
    redisDb *best_db = NULL;
    robj *key, *val;

    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        /* Why maxtries is set to 100?
         * Because this way (usually) we'll find 1 object even if just 1% - 2%
         * are swappable objects */
        int maxtries = 100;

        if (dictSize(db->dict) == 0) continue;
        for (i = 0; i < 5; i++) {
            dictEntry *de;
            double swappability;

            if (maxtries) maxtries--;
            de = dictGetRandomKey(db->dict);
            key = dictGetEntryKey(de);
            val = dictGetEntryVal(de);
            /* Only swap objects that are currently in memory.
             *
             * Also don't swap shared objects if threaded VM is on, as we
             * try to ensure that the main thread does not touch the
             * object while the I/O thread is using it, but we can't
             * control other keys without adding additional mutex. */
            if (key->storage != REDIS_VM_MEMORY ||
                (server.vm_max_threads != 0 && val->refcount != 1)) {
                if (maxtries) i--; /* don't count this try */
                continue;
            }
            swappability = computeObjectSwappability(val);
            if (!best || swappability > best_swappability) {
                best = de;
                best_swappability = swappability;
                best_db = db;
            }
        }
    }
    if (best == NULL) return REDIS_ERR;
    key = dictGetEntryKey(best);
    val = dictGetEntryVal(best);

    redisLog(REDIS_DEBUG,"Key with best swappability: %s, %f",
        key->ptr, best_swappability);

    /* Unshare the key if needed */
    if (key->refcount > 1) {
        robj *newkey = dupStringObject(key);
        decrRefCount(key);
        key = dictGetEntryKey(best) = newkey;
    }
    /* Swap it */
    if (usethreads) {
        vmSwapObjectThreaded(key,val,best_db);
        return REDIS_OK;
    } else {
        if (vmSwapObjectBlocking(key,val) == REDIS_OK) {
            dictGetEntryVal(best) = NULL;
            return REDIS_OK;
        } else {
            return REDIS_ERR;
        }
    }
}

static int vmSwapOneObjectBlocking() {
    return vmSwapOneObject(0);
}

static int vmSwapOneObjectThreaded() {
    return vmSwapOneObject(1);
}

/* Return true if it's safe to swap out objects in a given moment.
 * Basically we don't want to swap objects out while there is a BGSAVE
 * or a BGAEOREWRITE running in backgroud. */
static int vmCanSwapOut(void) {
    return (server.bgsavechildpid == -1 && server.bgrewritechildpid == -1);
}

/* Delete a key if swapped. Returns 1 if the key was found, was swapped
 * and was deleted. Otherwise 0 is returned. */
static int deleteIfSwapped(redisDb *db, robj *key) {
    dictEntry *de;
    robj *foundkey;

    if ((de = dictFind(db->dict,key)) == NULL) return 0;
    foundkey = dictGetEntryKey(de);
    if (foundkey->storage == REDIS_VM_MEMORY) return 0;
    deleteKey(db,key);
    return 1;
}

/* =================== Virtual Memory - Threaded I/O  ======================= */

static void freeIOJob(iojob *j) {
    if ((j->type == REDIS_IOJOB_PREPARE_SWAP ||
        j->type == REDIS_IOJOB_DO_SWAP ||
        j->type == REDIS_IOJOB_LOAD) && j->val != NULL)
        decrRefCount(j->val);
    /* We don't decrRefCount the j->key field as we did't incremented
     * the count creating IO Jobs. This is because the key field here is
     * just used as an indentifier and if a key is removed the Job should
     * never be touched again. */
    zfree(j);
}

/* Every time a thread finished a Job, it writes a byte into the write side
 * of an unix pipe in order to "awake" the main thread, and this function
 * is called. */
static void vmThreadedIOCompletedJob(aeEventLoop *el, int fd, void *privdata,
            int mask)
{
    char buf[1];
    int retval, processed = 0, toprocess = -1, trytoswap = 1;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);

    /* For every byte we read in the read side of the pipe, there is one
     * I/O job completed to process. */
    while((retval = read(fd,buf,1)) == 1) {
        iojob *j;
        listNode *ln;
        robj *key;
        struct dictEntry *de;

        redisLog(REDIS_DEBUG,"Processing I/O completed job");

        /* Get the processed element (the oldest one) */
        lockThreadedIO();
        assert(listLength(server.io_processed) != 0);
        if (toprocess == -1) {
            toprocess = (listLength(server.io_processed)*REDIS_MAX_COMPLETED_JOBS_PROCESSED)/100;
            if (toprocess <= 0) toprocess = 1;
        }
        ln = listFirst(server.io_processed);
        j = ln->value;
        listDelNode(server.io_processed,ln);
        unlockThreadedIO();
        /* If this job is marked as canceled, just ignore it */
        if (j->canceled) {
            freeIOJob(j);
            continue;
        }
        /* Post process it in the main thread, as there are things we
         * can do just here to avoid race conditions and/or invasive locks */
        redisLog(REDIS_DEBUG,"Job %p type: %d, key at %p (%s) refcount: %d\n", (void*) j, j->type, (void*)j->key, (char*)j->key->ptr, j->key->refcount);
        de = dictFind(j->db->dict,j->key);
        assert(de != NULL);
        key = dictGetEntryKey(de);
        if (j->type == REDIS_IOJOB_LOAD) {
            redisDb *db;

            /* Key loaded, bring it at home */
            key->storage = REDIS_VM_MEMORY;
            key->vm.atime = server.unixtime;
            vmMarkPagesFree(key->vm.page,key->vm.usedpages);
            redisLog(REDIS_DEBUG, "VM: object %s loaded from disk (threaded)",
                (unsigned char*) key->ptr);
            server.vm_stats_swapped_objects--;
            server.vm_stats_swapins++;
            dictGetEntryVal(de) = j->val;
            incrRefCount(j->val);
            db = j->db;
            freeIOJob(j);
            /* Handle clients waiting for this key to be loaded. */
            handleClientsBlockedOnSwappedKey(db,key);
        } else if (j->type == REDIS_IOJOB_PREPARE_SWAP) {
            /* Now we know the amount of pages required to swap this object.
             * Let's find some space for it, and queue this task again
             * rebranded as REDIS_IOJOB_DO_SWAP. */
            if (!vmCanSwapOut() ||
                vmFindContiguousPages(&j->page,j->pages) == REDIS_ERR)
            {
                /* Ooops... no space or we can't swap as there is
                 * a fork()ed Redis trying to save stuff on disk. */
                freeIOJob(j);
                key->storage = REDIS_VM_MEMORY; /* undo operation */
            } else {
                /* Note that we need to mark this pages as used now,
                 * if the job will be canceled, we'll mark them as freed
                 * again. */
                vmMarkPagesUsed(j->page,j->pages);
                j->type = REDIS_IOJOB_DO_SWAP;
                lockThreadedIO();
                queueIOJob(j);
                unlockThreadedIO();
            }
        } else if (j->type == REDIS_IOJOB_DO_SWAP) {
            robj *val;

            /* Key swapped. We can finally free some memory. */
            if (key->storage != REDIS_VM_SWAPPING) {
                printf("key->storage: %d\n",key->storage);
                printf("key->name: %s\n",(char*)key->ptr);
                printf("key->refcount: %d\n",key->refcount);
                printf("val: %p\n",(void*)j->val);
                printf("val->type: %d\n",j->val->type);
                printf("val->ptr: %s\n",(char*)j->val->ptr);
            }
            redisAssert(key->storage == REDIS_VM_SWAPPING);
            val = dictGetEntryVal(de);
            key->vm.page = j->page;
            key->vm.usedpages = j->pages;
            key->storage = REDIS_VM_SWAPPED;
            key->vtype = j->val->type;
            decrRefCount(val); /* Deallocate the object from memory. */
            dictGetEntryVal(de) = NULL;
            redisLog(REDIS_DEBUG,
                "VM: object %s swapped out at %lld (%lld pages) (threaded)",
                (unsigned char*) key->ptr,
                (unsigned long long) j->page, (unsigned long long) j->pages);
            server.vm_stats_swapped_objects++;
            server.vm_stats_swapouts++;
            freeIOJob(j);
            /* Put a few more swap requests in queue if we are still
             * out of memory */
            if (trytoswap && vmCanSwapOut() &&
                zmalloc_used_memory() > server.vm_max_memory)
            {
                int more = 1;
                while(more) {
                    lockThreadedIO();
                    more = listLength(server.io_newjobs) <
                            (unsigned) server.vm_max_threads;
                    unlockThreadedIO();
                    /* Don't waste CPU time if swappable objects are rare. */
                    if (vmSwapOneObjectThreaded() == REDIS_ERR) {
                        trytoswap = 0;
                        break;
                    }
                }
            }
        }
        processed++;
        if (processed == toprocess) return;
    }
    if (retval < 0 && errno != EAGAIN) {
        redisLog(REDIS_WARNING,
            "WARNING: read(2) error in vmThreadedIOCompletedJob() %s",
            strerror(errno));
    }
}

static void lockThreadedIO(void) {
    pthread_mutex_lock(&server.io_mutex);
}

static void unlockThreadedIO(void) {
    pthread_mutex_unlock(&server.io_mutex);
}

/* Remove the specified object from the threaded I/O queue if still not
 * processed, otherwise make sure to flag it as canceled. */
static void vmCancelThreadedIOJob(robj *o) {
    list *lists[3] = {
        server.io_newjobs,      /* 0 */
        server.io_processing,   /* 1 */
        server.io_processed     /* 2 */
    };
    int i;

    assert(o->storage == REDIS_VM_LOADING || o->storage == REDIS_VM_SWAPPING);
again:
    lockThreadedIO();
    /* Search for a matching key in one of the queues */
    for (i = 0; i < 3; i++) {
        listNode *ln;
        listIter li;

        listRewind(lists[i],&li);
        while ((ln = listNext(&li)) != NULL) {
            iojob *job = ln->value;

            if (job->canceled) continue; /* Skip this, already canceled. */
            if (job->key == o) {
                redisLog(REDIS_DEBUG,"*** CANCELED %p (%s) (type %d) (LIST ID %d)\n",
                    (void*)job, (char*)o->ptr, job->type, i);
                /* Mark the pages as free since the swap didn't happened
                 * or happened but is now discarded. */
                if (i != 1 && job->type == REDIS_IOJOB_DO_SWAP)
                    vmMarkPagesFree(job->page,job->pages);
                /* Cancel the job. It depends on the list the job is
                 * living in. */
                switch(i) {
                case 0: /* io_newjobs */
                    /* If the job was yet not processed the best thing to do
                     * is to remove it from the queue at all */
                    freeIOJob(job);
                    listDelNode(lists[i],ln);
                    break;
                case 1: /* io_processing */
                    /* Oh Shi- the thread is messing with the Job:
                     *
                     * Probably it's accessing the object if this is a
                     * PREPARE_SWAP or DO_SWAP job.
                     * If it's a LOAD job it may be reading from disk and
                     * if we don't wait for the job to terminate before to
                     * cancel it, maybe in a few microseconds data can be
                     * corrupted in this pages. So the short story is:
                     *
                     * Better to wait for the job to move into the
                     * next queue (processed)... */

                    /* We try again and again until the job is completed. */
                    unlockThreadedIO();
                    /* But let's wait some time for the I/O thread
                     * to finish with this job. After all this condition
                     * should be very rare. */
                    usleep(1);
                    goto again;
                case 2: /* io_processed */
                    /* The job was already processed, that's easy...
                     * just mark it as canceled so that we'll ignore it
                     * when processing completed jobs. */
                    job->canceled = 1;
                    break;
                }
                /* Finally we have to adjust the storage type of the object
                 * in order to "UNDO" the operaiton. */
                if (o->storage == REDIS_VM_LOADING)
                    o->storage = REDIS_VM_SWAPPED;
                else if (o->storage == REDIS_VM_SWAPPING)
                    o->storage = REDIS_VM_MEMORY;
                unlockThreadedIO();
                return;
            }
        }
    }
    unlockThreadedIO();
    assert(1 != 1); /* We should never reach this */
}

static void *IOThreadEntryPoint(void *arg) {
    iojob *j;
    listNode *ln;
    REDIS_NOTUSED(arg);

    pthread_detach(pthread_self());
    while(1) {
        /* Get a new job to process */
        lockThreadedIO();
        if (listLength(server.io_newjobs) == 0) {
            /* No new jobs in queue, exit. */
            redisLog(REDIS_DEBUG,"Thread %ld exiting, nothing to do",
                (long) pthread_self());
            server.io_active_threads--;
            unlockThreadedIO();
            return NULL;
        }
        ln = listFirst(server.io_newjobs);
        j = ln->value;
        listDelNode(server.io_newjobs,ln);
        /* Add the job in the processing queue */
        j->thread = pthread_self();
        listAddNodeTail(server.io_processing,j);
        ln = listLast(server.io_processing); /* We use ln later to remove it */
        unlockThreadedIO();
        redisLog(REDIS_DEBUG,"Thread %ld got a new job (type %d): %p about key '%s'",
            (long) pthread_self(), j->type, (void*)j, (char*)j->key->ptr);

        /* Process the Job */
        if (j->type == REDIS_IOJOB_LOAD) {
            j->val = vmReadObjectFromSwap(j->page,j->key->vtype);
        } else if (j->type == REDIS_IOJOB_PREPARE_SWAP) {
            FILE *fp = fopen("/dev/null","w+");
            j->pages = rdbSavedObjectPages(j->val,fp);
            fclose(fp);
        } else if (j->type == REDIS_IOJOB_DO_SWAP) {
            if (vmWriteObjectOnSwap(j->val,j->page) == REDIS_ERR)
                j->canceled = 1;
        }

        /* Done: insert the job into the processed queue */
        redisLog(REDIS_DEBUG,"Thread %ld completed the job: %p (key %s)",
            (long) pthread_self(), (void*)j, (char*)j->key->ptr);
        lockThreadedIO();
        listDelNode(server.io_processing,ln);
        listAddNodeTail(server.io_processed,j);
        unlockThreadedIO();

        /* Signal the main thread there is new stuff to process */
        assert(write(server.io_ready_pipe_write,"x",1) == 1);
    }
    return NULL; /* never reached */
}

static void spawnIOThread(void) {
    pthread_t thread;
    sigset_t mask, omask;
    int err;

    sigemptyset(&mask);
    sigaddset(&mask,SIGCHLD);
    sigaddset(&mask,SIGHUP);
    sigaddset(&mask,SIGPIPE);
    pthread_sigmask(SIG_SETMASK, &mask, &omask);
    while ((err = pthread_create(&thread,&server.io_threads_attr,IOThreadEntryPoint,NULL)) != 0) {
        redisLog(REDIS_WARNING,"Unable to spawn an I/O thread: %s",
            strerror(err));
        usleep(1000000);
    }
    pthread_sigmask(SIG_SETMASK, &omask, NULL);
    server.io_active_threads++;
}

/* We need to wait for the last thread to exit before we are able to
 * fork() in order to BGSAVE or BGREWRITEAOF. */
static void waitEmptyIOJobsQueue(void) {
    while(1) {
        int io_processed_len;

        lockThreadedIO();
        if (listLength(server.io_newjobs) == 0 &&
            listLength(server.io_processing) == 0 &&
            server.io_active_threads == 0)
        {
            unlockThreadedIO();
            return;
        }
        /* While waiting for empty jobs queue condition we post-process some
         * finshed job, as I/O threads may be hanging trying to write against
         * the io_ready_pipe_write FD but there are so much pending jobs that
         * it's blocking. */
        io_processed_len = listLength(server.io_processed);
        unlockThreadedIO();
        if (io_processed_len) {
            vmThreadedIOCompletedJob(NULL,server.io_ready_pipe_read,NULL,0);
            usleep(1000); /* 1 millisecond */
        } else {
            usleep(10000); /* 10 milliseconds */
        }
    }
}

static void vmReopenSwapFile(void) {
    /* Note: we don't close the old one as we are in the child process
     * and don't want to mess at all with the original file object. */
    server.vm_fp = fopen(server.vm_swap_file,"r+b");
    if (server.vm_fp == NULL) {
        redisLog(REDIS_WARNING,"Can't re-open the VM swap file: %s. Exiting.",
            server.vm_swap_file);
        _exit(1);
    }
    server.vm_fd = fileno(server.vm_fp);
}

/* This function must be called while with threaded IO locked */
static void queueIOJob(iojob *j) {
    redisLog(REDIS_DEBUG,"Queued IO Job %p type %d about key '%s'\n",
        (void*)j, j->type, (char*)j->key->ptr);
    listAddNodeTail(server.io_newjobs,j);
    if (server.io_active_threads < server.vm_max_threads)
        spawnIOThread();
}

static int vmSwapObjectThreaded(robj *key, robj *val, redisDb *db) {
    iojob *j;

    assert(key->storage == REDIS_VM_MEMORY);
    assert(key->refcount == 1);

    j = zmalloc(sizeof(*j));
    j->type = REDIS_IOJOB_PREPARE_SWAP;
    j->db = db;
    j->key = key;
    j->val = val;
    incrRefCount(val);
    j->canceled = 0;
    j->thread = (pthread_t) -1;
    key->storage = REDIS_VM_SWAPPING;

    lockThreadedIO();
    queueIOJob(j);
    unlockThreadedIO();
    return REDIS_OK;
}

/* ============ Virtual Memory - Blocking clients on missing keys =========== */

/* This function makes the clinet 'c' waiting for the key 'key' to be loaded.
 * If there is not already a job loading the key, it is craeted.
 * The key is added to the io_keys list in the client structure, and also
 * in the hash table mapping swapped keys to waiting clients, that is,
 * server.io_waited_keys. */
static int waitForSwappedKey(redisClient *c, robj *key) {
    struct dictEntry *de;
    robj *o;
    list *l;

    /* If the key does not exist or is already in RAM we don't need to
     * block the client at all. */
    de = dictFind(c->db->dict,key);
    if (de == NULL) return 0;
    o = dictGetEntryKey(de);
    if (o->storage == REDIS_VM_MEMORY) {
        return 0;
    } else if (o->storage == REDIS_VM_SWAPPING) {
        /* We were swapping the key, undo it! */
        vmCancelThreadedIOJob(o);
        return 0;
    }

    /* OK: the key is either swapped, or being loaded just now. */

    /* Add the key to the list of keys this client is waiting for.
     * This maps clients to keys they are waiting for. */
    listAddNodeTail(c->io_keys,key);
    incrRefCount(key);

    /* Add the client to the swapped keys => clients waiting map. */
    de = dictFind(c->db->io_keys,key);
    if (de == NULL) {
        int retval;

        /* For every key we take a list of clients blocked for it */
        l = listCreate();
        retval = dictAdd(c->db->io_keys,key,l);
        incrRefCount(key);
        assert(retval == DICT_OK);
    } else {
        l = dictGetEntryVal(de);
    }
    listAddNodeTail(l,c);

    /* Are we already loading the key from disk? If not create a job */
    if (o->storage == REDIS_VM_SWAPPED) {
        iojob *j;

        o->storage = REDIS_VM_LOADING;
        j = zmalloc(sizeof(*j));
        j->type = REDIS_IOJOB_LOAD;
        j->db = c->db;
        j->key = o;
        j->key->vtype = o->vtype;
        j->page = o->vm.page;
        j->val = NULL;
        j->canceled = 0;
        j->thread = (pthread_t) -1;
        lockThreadedIO();
        queueIOJob(j);
        unlockThreadedIO();
    }
    return 1;
}

/* Preload keys for any command with first, last and step values for
 * the command keys prototype, as defined in the command table. */
static void waitForMultipleSwappedKeys(redisClient *c, struct redisCommand *cmd, int argc, robj **argv) {
    int j, last;
    if (cmd->vm_firstkey == 0) return;
    last = cmd->vm_lastkey;
    if (last < 0) last = argc+last;
    for (j = cmd->vm_firstkey; j <= last; j += cmd->vm_keystep) {
        redisAssert(j < argc);
        waitForSwappedKey(c,argv[j]);
    }
}

/* Preload keys needed for the ZUNIONSTORE and ZINTERSTORE commands.
 * Note that the number of keys to preload is user-defined, so we need to
 * apply a sanity check against argc. */
static void zunionInterBlockClientOnSwappedKeys(redisClient *c, struct redisCommand *cmd, int argc, robj **argv) {
    int i, num;
    REDIS_NOTUSED(cmd);

    num = atoi(argv[2]->ptr);
    if (num > (argc-3)) return;
    for (i = 0; i < num; i++) {
        waitForSwappedKey(c,argv[3+i]);
    }
}

/* Preload keys needed to execute the entire MULTI/EXEC block.
 *
 * This function is called by blockClientOnSwappedKeys when EXEC is issued,
 * and will block the client when any command requires a swapped out value. */
static void execBlockClientOnSwappedKeys(redisClient *c, struct redisCommand *cmd, int argc, robj **argv) {
    int i, margc;
    struct redisCommand *mcmd;
    robj **margv;
    REDIS_NOTUSED(cmd);
    REDIS_NOTUSED(argc);
    REDIS_NOTUSED(argv);

    if (!(c->flags & REDIS_MULTI)) return;
    for (i = 0; i < c->mstate.count; i++) {
        mcmd = c->mstate.commands[i].cmd;
        margc = c->mstate.commands[i].argc;
        margv = c->mstate.commands[i].argv;

        if (mcmd->vm_preload_proc != NULL) {
            mcmd->vm_preload_proc(c,mcmd,margc,margv);
        } else {
            waitForMultipleSwappedKeys(c,mcmd,margc,margv);
        }
    }
}

/* Is this client attempting to run a command against swapped keys?
 * If so, block it ASAP, load the keys in background, then resume it.
 *
 * The important idea about this function is that it can fail! If keys will
 * still be swapped when the client is resumed, this key lookups will
 * just block loading keys from disk. In practical terms this should only
 * happen with SORT BY command or if there is a bug in this function.
 *
 * Return 1 if the client is marked as blocked, 0 if the client can
 * continue as the keys it is going to access appear to be in memory. */
static int blockClientOnSwappedKeys(redisClient *c, struct redisCommand *cmd) {
    if (cmd->vm_preload_proc != NULL) {
        cmd->vm_preload_proc(c,cmd,c->argc,c->argv);
    } else {
        waitForMultipleSwappedKeys(c,cmd,c->argc,c->argv);
    }

    /* If the client was blocked for at least one key, mark it as blocked. */
    if (listLength(c->io_keys)) {
        c->flags |= REDIS_IO_WAIT;
        aeDeleteFileEvent(server.el,c->fd,AE_READABLE);
        server.vm_blocked_clients++;
        return 1;
    } else {
        return 0;
    }
}

/* Remove the 'key' from the list of blocked keys for a given client.
 *
 * The function returns 1 when there are no longer blocking keys after
 * the current one was removed (and the client can be unblocked). */
static int dontWaitForSwappedKey(redisClient *c, robj *key) {
    list *l;
    listNode *ln;
    listIter li;
    struct dictEntry *de;

    /* Remove the key from the list of keys this client is waiting for. */
    listRewind(c->io_keys,&li);
    while ((ln = listNext(&li)) != NULL) {
        if (equalStringObjects(ln->value,key)) {
            listDelNode(c->io_keys,ln);
            break;
        }
    }
    assert(ln != NULL);

    /* Remove the client form the key => waiting clients map. */
    de = dictFind(c->db->io_keys,key);
    assert(de != NULL);
    l = dictGetEntryVal(de);
    ln = listSearchKey(l,c);
    assert(ln != NULL);
    listDelNode(l,ln);
    if (listLength(l) == 0)
        dictDelete(c->db->io_keys,key);

    return listLength(c->io_keys) == 0;
}

static void handleClientsBlockedOnSwappedKey(redisDb *db, robj *key) {
    struct dictEntry *de;
    list *l;
    listNode *ln;
    int len;

    de = dictFind(db->io_keys,key);
    if (!de) return;

    l = dictGetEntryVal(de);
    len = listLength(l);
    /* Note: we can't use something like while(listLength(l)) as the list
     * can be freed by the calling function when we remove the last element. */
    while (len--) {
        ln = listFirst(l);
        redisClient *c = ln->value;

        if (dontWaitForSwappedKey(c,key)) {
            /* Put the client in the list of clients ready to go as we
             * loaded all the keys about it. */
            listAddNodeTail(server.io_ready_clients,c);
        }
    }
}

/* =========================== Remote Configuration ========================= */

static void configSetCommand(redisClient *c) {
    robj *o = getDecodedObject(c->argv[3]);
    long long ll;

    if (!strcasecmp(c->argv[2]->ptr,"dbfilename")) {
        zfree(server.dbfilename);
        server.dbfilename = zstrdup(o->ptr);
    } else if (!strcasecmp(c->argv[2]->ptr,"requirepass")) {
        zfree(server.requirepass);
        server.requirepass = zstrdup(o->ptr);
    } else if (!strcasecmp(c->argv[2]->ptr,"masterauth")) {
        zfree(server.masterauth);
        server.masterauth = zstrdup(o->ptr);
    } else if (!strcasecmp(c->argv[2]->ptr,"maxmemory")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR ||
            ll < 0) goto badfmt;
        server.maxmemory = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"timeout")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR ||
            ll < 0 || ll > LONG_MAX) goto badfmt;
        server.maxidletime = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"appendfsync")) {
        if (!strcasecmp(o->ptr,"no")) {
            server.appendfsync = APPENDFSYNC_NO;
        } else if (!strcasecmp(o->ptr,"everysec")) {
            server.appendfsync = APPENDFSYNC_EVERYSEC;
        } else if (!strcasecmp(o->ptr,"always")) {
            server.appendfsync = APPENDFSYNC_ALWAYS;
        } else {
            goto badfmt;
        }
    } else if (!strcasecmp(c->argv[2]->ptr,"appendonly")) {
        int old = server.appendonly;
        int new = yesnotoi(o->ptr);

        if (new == -1) goto badfmt;
        if (old != new) {
            if (new == 0) {
                stopAppendOnly();
            } else {
                if (startAppendOnly() == REDIS_ERR) {
                    addReplySds(c,sdscatprintf(sdsempty(),
                        "-ERR Unable to turn on AOF. Check server logs.\r\n"));
                    decrRefCount(o);
                    return;
                }
            }
        }
    } else if (!strcasecmp(c->argv[2]->ptr,"save")) {
        int vlen, j;
        sds *v = sdssplitlen(o->ptr,sdslen(o->ptr)," ",1,&vlen);

        /* Perform sanity check before setting the new config:
         * - Even number of args
         * - Seconds >= 1, changes >= 0 */
        if (vlen & 1) {
            sdsfreesplitres(v,vlen);
            goto badfmt;
        }
        for (j = 0; j < vlen; j++) {
            char *eptr;
            long val;

            val = strtoll(v[j], &eptr, 10);
            if (eptr[0] != '\0' ||
                ((j & 1) == 0 && val < 1) ||
                ((j & 1) == 1 && val < 0)) {
                sdsfreesplitres(v,vlen);
                goto badfmt;
            }
        }
        /* Finally set the new config */
        resetServerSaveParams();
        for (j = 0; j < vlen; j += 2) {
            time_t seconds;
            int changes;

            seconds = strtoll(v[j],NULL,10);
            changes = strtoll(v[j+1],NULL,10);
            appendServerSaveParams(seconds, changes);
        }
        sdsfreesplitres(v,vlen);
    } else {
        addReplySds(c,sdscatprintf(sdsempty(),
            "-ERR not supported CONFIG parameter %s\r\n",
            (char*)c->argv[2]->ptr));
        decrRefCount(o);
        return;
    }
    decrRefCount(o);
    addReply(c,shared.ok);
    return;

badfmt: /* Bad format errors */
    addReplySds(c,sdscatprintf(sdsempty(),
        "-ERR invalid argument '%s' for CONFIG SET '%s'\r\n",
            (char*)o->ptr,
            (char*)c->argv[2]->ptr));
    decrRefCount(o);
}

static void configGetCommand(redisClient *c) {
    robj *o = getDecodedObject(c->argv[2]);
    robj *lenobj = createObject(REDIS_STRING,NULL);
    char *pattern = o->ptr;
    int matches = 0;

    addReply(c,lenobj);
    decrRefCount(lenobj);

    if (stringmatch(pattern,"dbfilename",0)) {
        addReplyBulkCString(c,"dbfilename");
        addReplyBulkCString(c,server.dbfilename);
        matches++;
    }
    if (stringmatch(pattern,"requirepass",0)) {
        addReplyBulkCString(c,"requirepass");
        addReplyBulkCString(c,server.requirepass);
        matches++;
    }
    if (stringmatch(pattern,"masterauth",0)) {
        addReplyBulkCString(c,"masterauth");
        addReplyBulkCString(c,server.masterauth);
        matches++;
    }
    if (stringmatch(pattern,"maxmemory",0)) {
        char buf[128];

        ll2string(buf,128,server.maxmemory);
        addReplyBulkCString(c,"maxmemory");
        addReplyBulkCString(c,buf);
        matches++;
    }
    if (stringmatch(pattern,"timeout",0)) {
        char buf[128];

        ll2string(buf,128,server.maxidletime);
        addReplyBulkCString(c,"timeout");
        addReplyBulkCString(c,buf);
        matches++;
    }
    if (stringmatch(pattern,"appendonly",0)) {
        addReplyBulkCString(c,"appendonly");
        addReplyBulkCString(c,server.appendonly ? "yes" : "no");
        matches++;
    }
    if (stringmatch(pattern,"appendfsync",0)) {
        char *policy;

        switch(server.appendfsync) {
        case APPENDFSYNC_NO: policy = "no"; break;
        case APPENDFSYNC_EVERYSEC: policy = "everysec"; break;
        case APPENDFSYNC_ALWAYS: policy = "always"; break;
        default: policy = "unknown"; break; /* too harmless to panic */
        }
        addReplyBulkCString(c,"appendfsync");
        addReplyBulkCString(c,policy);
        matches++;
    }
    if (stringmatch(pattern,"save",0)) {
        sds buf = sdsempty();
        int j;

        for (j = 0; j < server.saveparamslen; j++) {
            buf = sdscatprintf(buf,"%ld %d",
                    server.saveparams[j].seconds,
                    server.saveparams[j].changes);
            if (j != server.saveparamslen-1)
                buf = sdscatlen(buf," ",1);
        }
        addReplyBulkCString(c,"save");
        addReplyBulkCString(c,buf);
        sdsfree(buf);
        matches++;
    }
    decrRefCount(o);
    lenobj->ptr = sdscatprintf(sdsempty(),"*%d\r\n",matches*2);
}

static void configCommand(redisClient *c) {
    if (!strcasecmp(c->argv[1]->ptr,"set")) {
        if (c->argc != 4) goto badarity;
        configSetCommand(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"get")) {
        if (c->argc != 3) goto badarity;
        configGetCommand(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"resetstat")) {
        if (c->argc != 2) goto badarity;
        server.stat_numcommands = 0;
        server.stat_numconnections = 0;
        server.stat_expiredkeys = 0;
        server.stat_starttime = time(NULL);
        addReply(c,shared.ok);
    } else {
        addReplySds(c,sdscatprintf(sdsempty(),
            "-ERR CONFIG subcommand must be one of GET, SET, RESETSTAT\r\n"));
    }
    return;

badarity:
    addReplySds(c,sdscatprintf(sdsempty(),
        "-ERR Wrong number of arguments for CONFIG %s\r\n",
        (char*) c->argv[1]->ptr));
}

/* =========================== Pubsub implementation ======================== */

static void freePubsubPattern(void *p) {
    pubsubPattern *pat = p;

    decrRefCount(pat->pattern);
    zfree(pat);
}

static int listMatchPubsubPattern(void *a, void *b) {
    pubsubPattern *pa = a, *pb = b;

    return (pa->client == pb->client) &&
           (equalStringObjects(pa->pattern,pb->pattern));
}

/* Subscribe a client to a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was already subscribed to that channel. */
static int pubsubSubscribeChannel(redisClient *c, robj *channel) {
    struct dictEntry *de;
    list *clients = NULL;
    int retval = 0;

    /* Add the channel to the client -> channels hash table */
    if (dictAdd(c->pubsub_channels,channel,NULL) == DICT_OK) {
        retval = 1;
        incrRefCount(channel);
        /* Add the client to the channel -> list of clients hash table */
        de = dictFind(server.pubsub_channels,channel);
        if (de == NULL) {
            clients = listCreate();
            dictAdd(server.pubsub_channels,channel,clients);
            incrRefCount(channel);
        } else {
            clients = dictGetEntryVal(de);
        }
        listAddNodeTail(clients,c);
    }
    /* Notify the client */
    addReply(c,shared.mbulk3);
    addReply(c,shared.subscribebulk);
    addReplyBulk(c,channel);
    addReplyLongLong(c,dictSize(c->pubsub_channels)+listLength(c->pubsub_patterns));
    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
static int pubsubUnsubscribeChannel(redisClient *c, robj *channel, int notify) {
    struct dictEntry *de;
    list *clients;
    listNode *ln;
    int retval = 0;

    /* Remove the channel from the client -> channels hash table */
    incrRefCount(channel); /* channel may be just a pointer to the same object
                            we have in the hash tables. Protect it... */
    if (dictDelete(c->pubsub_channels,channel) == DICT_OK) {
        retval = 1;
        /* Remove the client from the channel -> clients list hash table */
        de = dictFind(server.pubsub_channels,channel);
        assert(de != NULL);
        clients = dictGetEntryVal(de);
        ln = listSearchKey(clients,c);
        assert(ln != NULL);
        listDelNode(clients,ln);
        if (listLength(clients) == 0) {
            /* Free the list and associated hash entry at all if this was
             * the latest client, so that it will be possible to abuse
             * Redis PUBSUB creating millions of channels. */
            dictDelete(server.pubsub_channels,channel);
        }
    }
    /* Notify the client */
    if (notify) {
        addReply(c,shared.mbulk3);
        addReply(c,shared.unsubscribebulk);
        addReplyBulk(c,channel);
        addReplyLongLong(c,dictSize(c->pubsub_channels)+
                       listLength(c->pubsub_patterns));

    }
    decrRefCount(channel); /* it is finally safe to release it */
    return retval;
}

/* Subscribe a client to a pattern. Returns 1 if the operation succeeded, or 0 if the clinet was already subscribed to that pattern. */
static int pubsubSubscribePattern(redisClient *c, robj *pattern) {
    int retval = 0;

    if (listSearchKey(c->pubsub_patterns,pattern) == NULL) {
        retval = 1;
        pubsubPattern *pat;
        listAddNodeTail(c->pubsub_patterns,pattern);
        incrRefCount(pattern);
        pat = zmalloc(sizeof(*pat));
        pat->pattern = getDecodedObject(pattern);
        pat->client = c;
        listAddNodeTail(server.pubsub_patterns,pat);
    }
    /* Notify the client */
    addReply(c,shared.mbulk3);
    addReply(c,shared.psubscribebulk);
    addReplyBulk(c,pattern);
    addReplyLongLong(c,dictSize(c->pubsub_channels)+listLength(c->pubsub_patterns));
    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
static int pubsubUnsubscribePattern(redisClient *c, robj *pattern, int notify) {
    listNode *ln;
    pubsubPattern pat;
    int retval = 0;

    incrRefCount(pattern); /* Protect the object. May be the same we remove */
    if ((ln = listSearchKey(c->pubsub_patterns,pattern)) != NULL) {
        retval = 1;
        listDelNode(c->pubsub_patterns,ln);
        pat.client = c;
        pat.pattern = pattern;
        ln = listSearchKey(server.pubsub_patterns,&pat);
        listDelNode(server.pubsub_patterns,ln);
    }
    /* Notify the client */
    if (notify) {
        addReply(c,shared.mbulk3);
        addReply(c,shared.punsubscribebulk);
        addReplyBulk(c,pattern);
        addReplyLongLong(c,dictSize(c->pubsub_channels)+
                       listLength(c->pubsub_patterns));
    }
    decrRefCount(pattern);
    return retval;
}

/* Unsubscribe from all the channels. Return the number of channels the
 * client was subscribed from. */
static int pubsubUnsubscribeAllChannels(redisClient *c, int notify) {
    dictIterator *di = dictGetIterator(c->pubsub_channels);
    dictEntry *de;
    int count = 0;

    while((de = dictNext(di)) != NULL) {
        robj *channel = dictGetEntryKey(de);

        count += pubsubUnsubscribeChannel(c,channel,notify);
    }
    dictReleaseIterator(di);
    return count;
}

/* Unsubscribe from all the patterns. Return the number of patterns the
 * client was subscribed from. */
static int pubsubUnsubscribeAllPatterns(redisClient *c, int notify) {
    listNode *ln;
    listIter li;
    int count = 0;

    listRewind(c->pubsub_patterns,&li);
    while ((ln = listNext(&li)) != NULL) {
        robj *pattern = ln->value;

        count += pubsubUnsubscribePattern(c,pattern,notify);
    }
    return count;
}

/* Publish a message */
static int pubsubPublishMessage(robj *channel, robj *message) {
    int receivers = 0;
    struct dictEntry *de;
    listNode *ln;
    listIter li;

    /* Send to clients listening for that channel */
    de = dictFind(server.pubsub_channels,channel);
    if (de) {
        list *list = dictGetEntryVal(de);
        listNode *ln;
        listIter li;

        listRewind(list,&li);
        while ((ln = listNext(&li)) != NULL) {
            redisClient *c = ln->value;

            addReply(c,shared.mbulk3);
            addReply(c,shared.messagebulk);
            addReplyBulk(c,channel);
            addReplyBulk(c,message);
            receivers++;
        }
    }
    /* Send to clients listening to matching channels */
    if (listLength(server.pubsub_patterns)) {
        listRewind(server.pubsub_patterns,&li);
        channel = getDecodedObject(channel);
        while ((ln = listNext(&li)) != NULL) {
            pubsubPattern *pat = ln->value;

            if (stringmatchlen((char*)pat->pattern->ptr,
                                sdslen(pat->pattern->ptr),
                                (char*)channel->ptr,
                                sdslen(channel->ptr),0)) {
                addReply(pat->client,shared.mbulk4);
                addReply(pat->client,shared.pmessagebulk);
                addReplyBulk(pat->client,pat->pattern);
                addReplyBulk(pat->client,channel);
                addReplyBulk(pat->client,message);
                receivers++;
            }
        }
        decrRefCount(channel);
    }
    return receivers;
}

static void subscribeCommand(redisClient *c) {
    int j;

    for (j = 1; j < c->argc; j++)
        pubsubSubscribeChannel(c,c->argv[j]);
}

static void unsubscribeCommand(redisClient *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeAllChannels(c,1);
        return;
    } else {
        int j;

        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribeChannel(c,c->argv[j],1);
    }
}

static void psubscribeCommand(redisClient *c) {
    int j;

    for (j = 1; j < c->argc; j++)
        pubsubSubscribePattern(c,c->argv[j]);
}

static void punsubscribeCommand(redisClient *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeAllPatterns(c,1);
        return;
    } else {
        int j;

        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribePattern(c,c->argv[j],1);
    }
}

static void publishCommand(redisClient *c) {
    int receivers = pubsubPublishMessage(c->argv[1],c->argv[2]);
    addReplyLongLong(c,receivers);
}

/* ===================== WATCH (CAS alike for MULTI/EXEC) ===================
 *
 * The implementation uses a per-DB hash table mapping keys to list of clients
 * WATCHing those keys, so that given a key that is going to be modified
 * we can mark all the associated clients as dirty.
 *
 * Also every client contains a list of WATCHed keys so that's possible to
 * un-watch such keys when the client is freed or when UNWATCH is called. */

/* In the client->watched_keys list we need to use watchedKey structures
 * as in order to identify a key in Redis we need both the key name and the
 * DB */
typedef struct watchedKey {
    robj *key;
    redisDb *db;
} watchedKey;

/* Watch for the specified key */
static void watchForKey(redisClient *c, robj *key) {
    list *clients = NULL;
    listIter li;
    listNode *ln;
    watchedKey *wk;

    /* Check if we are already watching for this key */
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li))) {
        wk = listNodeValue(ln);
        if (wk->db == c->db && equalStringObjects(key,wk->key))
            return; /* Key already watched */
    }
    /* This key is not already watched in this DB. Let's add it */
    clients = dictFetchValue(c->db->watched_keys,key);
    if (!clients) { 
        clients = listCreate();
        dictAdd(c->db->watched_keys,key,clients);
        incrRefCount(key);
    }
    listAddNodeTail(clients,c);
    /* Add the new key to the lits of keys watched by this client */
    wk = zmalloc(sizeof(*wk));
    wk->key = key;
    wk->db = c->db;
    incrRefCount(key);
    listAddNodeTail(c->watched_keys,wk);
}

/* Unwatch all the keys watched by this client. To clean the EXEC dirty
 * flag is up to the caller. */
static void unwatchAllKeys(redisClient *c) {
    listIter li;
    listNode *ln;

    if (listLength(c->watched_keys) == 0) return;
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li))) {
        list *clients;
        watchedKey *wk;

        /* Lookup the watched key -> clients list and remove the client
         * from the list */
        wk = listNodeValue(ln);
        clients = dictFetchValue(wk->db->watched_keys, wk->key);
        assert(clients != NULL);
        listDelNode(clients,listSearchKey(clients,c));
        /* Kill the entry at all if this was the only client */
        if (listLength(clients) == 0)
            dictDelete(wk->db->watched_keys, wk->key);
        /* Remove this watched key from the client->watched list */
        listDelNode(c->watched_keys,ln);
        decrRefCount(wk->key);
        zfree(wk);
    }
}

/* "Touch" a key, so that if this key is being WATCHed by soem client the
 * next EXEC will fail. */
static void touchWatchedKey(redisDb *db, robj *key) {
    list *clients;
    listIter li;
    listNode *ln;

    if (dictSize(db->watched_keys) == 0) return;
    clients = dictFetchValue(db->watched_keys, key);
    if (!clients) return;

    /* Mark all the clients watching this key as REDIS_DIRTY_CAS */
    /* Check if we are already watching for this key */
    listRewind(clients,&li);
    while((ln = listNext(&li))) {
        redisClient *c = listNodeValue(ln);

        c->flags |= REDIS_DIRTY_CAS;
    }
}

/* On FLUSHDB or FLUSHALL all the watched keys that are present before the
 * flush but will be deleted as effect of the flushing operation should
 * be touched. "dbid" is the DB that's getting the flush. -1 if it is
 * a FLUSHALL operation (all the DBs flushed). */
static void touchWatchedKeysOnFlush(int dbid) {
    listIter li1, li2;
    listNode *ln;

    /* For every client, check all the waited keys */
    listRewind(server.clients,&li1);
    while((ln = listNext(&li1))) {
        redisClient *c = listNodeValue(ln);
        listRewind(c->watched_keys,&li2);
        while((ln = listNext(&li2))) {
            watchedKey *wk = listNodeValue(ln);

            /* For every watched key matching the specified DB, if the
             * key exists, mark the client as dirty, as the key will be
             * removed. */
            if (dbid == -1 || wk->db->id == dbid) {
                if (dictFind(wk->db->dict, wk->key) != NULL)
                    c->flags |= REDIS_DIRTY_CAS;
            }
        }
    }
}

static void watchCommand(redisClient *c) {
    int j;

    if (c->flags & REDIS_MULTI) {
        addReplySds(c,sdsnew("-ERR WATCH inside MULTI is not allowed\r\n"));
        return;
    }
    for (j = 1; j < c->argc; j++)
        watchForKey(c,c->argv[j]);
    addReply(c,shared.ok);
}

static void unwatchCommand(redisClient *c) {
    unwatchAllKeys(c);
    c->flags &= (~REDIS_DIRTY_CAS);
    addReply(c,shared.ok);
}

/* ================================= Debugging ============================== */

/* Compute the sha1 of string at 's' with 'len' bytes long.
 * The SHA1 is then xored againt the string pointed by digest.
 * Since xor is commutative, this operation is used in order to
 * "add" digests relative to unordered elements.
 *
 * So digest(a,b,c,d) will be the same of digest(b,a,c,d) */
static void xorDigest(unsigned char *digest, void *ptr, size_t len) {
    SHA1_CTX ctx;
    unsigned char hash[20], *s = ptr;
    int j;

    SHA1Init(&ctx);
    SHA1Update(&ctx,s,len);
    SHA1Final(hash,&ctx);

    for (j = 0; j < 20; j++)
        digest[j] ^= hash[j];
}

static void xorObjectDigest(unsigned char *digest, robj *o) {
    o = getDecodedObject(o);
    xorDigest(digest,o->ptr,sdslen(o->ptr));
    decrRefCount(o);
}

/* This function instead of just computing the SHA1 and xoring it
 * against diget, also perform the digest of "digest" itself and
 * replace the old value with the new one.
 *
 * So the final digest will be:
 *
 * digest = SHA1(digest xor SHA1(data))
 *
 * This function is used every time we want to preserve the order so
 * that digest(a,b,c,d) will be different than digest(b,c,d,a)
 *
 * Also note that mixdigest("foo") followed by mixdigest("bar")
 * will lead to a different digest compared to "fo", "obar".
 */
static void mixDigest(unsigned char *digest, void *ptr, size_t len) {
    SHA1_CTX ctx;
    char *s = ptr;

    xorDigest(digest,s,len);
    SHA1Init(&ctx);
    SHA1Update(&ctx,digest,20);
    SHA1Final(digest,&ctx);
}

static void mixObjectDigest(unsigned char *digest, robj *o) {
    o = getDecodedObject(o);
    mixDigest(digest,o->ptr,sdslen(o->ptr));
    decrRefCount(o);
}

/* Compute the dataset digest. Since keys, sets elements, hashes elements
 * are not ordered, we use a trick: every aggregate digest is the xor
 * of the digests of their elements. This way the order will not change
 * the result. For list instead we use a feedback entering the output digest
 * as input in order to ensure that a different ordered list will result in
 * a different digest. */
static void computeDatasetDigest(unsigned char *final) {
    unsigned char digest[20];
    char buf[128];
    dictIterator *di = NULL;
    dictEntry *de;
    int j;
    uint32_t aux;

    memset(final,0,20); /* Start with a clean result */

    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;

        if (dictSize(db->dict) == 0) continue;
        di = dictGetIterator(db->dict);

        /* hash the DB id, so the same dataset moved in a different
         * DB will lead to a different digest */
        aux = htonl(j);
        mixDigest(final,&aux,sizeof(aux));

        /* Iterate this DB writing every entry */
        while((de = dictNext(di)) != NULL) {
            robj *key, *o, *kcopy;
            time_t expiretime;

            memset(digest,0,20); /* This key-val digest */
            key = dictGetEntryKey(de);

            if (!server.vm_enabled) {
                mixObjectDigest(digest,key);
                o = dictGetEntryVal(de);
            } else {
                /* Don't work with the key directly as when VM is active
                 * this is unsafe: TODO: fix decrRefCount to check if the
                 * count really reached 0 to avoid this mess */
                kcopy = dupStringObject(key);
                mixObjectDigest(digest,kcopy);
                o = lookupKeyRead(db,kcopy);
                decrRefCount(kcopy);
            }
            aux = htonl(o->type);
            mixDigest(digest,&aux,sizeof(aux));
            expiretime = getExpire(db,key);

            /* Save the key and associated value */
            if (o->type == REDIS_STRING) {
                mixObjectDigest(digest,o);
            } else if (o->type == REDIS_LIST) {
                list *list = o->ptr;
                listNode *ln;
                listIter li;

                listRewind(list,&li);
                while((ln = listNext(&li))) {
                    robj *eleobj = listNodeValue(ln);

                    mixObjectDigest(digest,eleobj);
                }
            } else if (o->type == REDIS_SET) {
                dict *set = o->ptr;
                dictIterator *di = dictGetIterator(set);
                dictEntry *de;

                while((de = dictNext(di)) != NULL) {
                    robj *eleobj = dictGetEntryKey(de);

                    xorObjectDigest(digest,eleobj);
                }
                dictReleaseIterator(di);
            } else if (o->type == REDIS_ZSET) {
                zset *zs = o->ptr;
                dictIterator *di = dictGetIterator(zs->dict);
                dictEntry *de;

                while((de = dictNext(di)) != NULL) {
                    robj *eleobj = dictGetEntryKey(de);
                    double *score = dictGetEntryVal(de);
                    unsigned char eledigest[20];

                    snprintf(buf,sizeof(buf),"%.17g",*score);
                    memset(eledigest,0,20);
                    mixObjectDigest(eledigest,eleobj);
                    mixDigest(eledigest,buf,strlen(buf));
                    xorDigest(digest,eledigest,20);
                }
                dictReleaseIterator(di);
            } else if (o->type == REDIS_HASH) {
                hashIterator *hi;
                robj *obj;

                hi = hashInitIterator(o);
                while (hashNext(hi) != REDIS_ERR) {
                    unsigned char eledigest[20];

                    memset(eledigest,0,20);
                    obj = hashCurrent(hi,REDIS_HASH_KEY);
                    mixObjectDigest(eledigest,obj);
                    decrRefCount(obj);
                    obj = hashCurrent(hi,REDIS_HASH_VALUE);
                    mixObjectDigest(eledigest,obj);
                    decrRefCount(obj);
                    xorDigest(digest,eledigest,20);
                }
                hashReleaseIterator(hi);
            } else {
                redisPanic("Unknown object type");
            }
            /* If the key has an expire, add it to the mix */
            if (expiretime != -1) xorDigest(digest,"!!expire!!",10);
            /* We can finally xor the key-val digest to the final digest */
            xorDigest(final,digest,20);
        }
        dictReleaseIterator(di);
    }
}

static void debugCommand(redisClient *c) {
    if (!strcasecmp(c->argv[1]->ptr,"segfault")) {
        *((char*)-1) = 'x';
    } else if (!strcasecmp(c->argv[1]->ptr,"reload")) {
        if (rdbSave(server.dbfilename) != REDIS_OK) {
            addReply(c,shared.err);
            return;
        }
        emptyDb();
        if (rdbLoad(server.dbfilename) != REDIS_OK) {
            addReply(c,shared.err);
            return;
        }
        redisLog(REDIS_WARNING,"DB reloaded by DEBUG RELOAD");
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"loadaof")) {
        emptyDb();
        if (loadAppendOnlyFile(server.appendfilename) != REDIS_OK) {
            addReply(c,shared.err);
            return;
        }
        redisLog(REDIS_WARNING,"Append Only File loaded by DEBUG LOADAOF");
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"object") && c->argc == 3) {
        dictEntry *de = dictFind(c->db->dict,c->argv[2]);
        robj *key, *val;

        if (!de) {
            addReply(c,shared.nokeyerr);
            return;
        }
        key = dictGetEntryKey(de);
        val = dictGetEntryVal(de);
        if (!server.vm_enabled || (key->storage == REDIS_VM_MEMORY ||
                                   key->storage == REDIS_VM_SWAPPING)) {
            char *strenc;
            char buf[128];

            if (val->encoding < (sizeof(strencoding)/sizeof(char*))) {
                strenc = strencoding[val->encoding];
            } else {
                snprintf(buf,64,"unknown encoding %d\n", val->encoding);
                strenc = buf;
            }
            addReplySds(c,sdscatprintf(sdsempty(),
                "+Key at:%p refcount:%d, value at:%p refcount:%d "
                "encoding:%s serializedlength:%lld\r\n",
                (void*)key, key->refcount, (void*)val, val->refcount,
                strenc, (long long) rdbSavedObjectLen(val,NULL)));
        } else {
            addReplySds(c,sdscatprintf(sdsempty(),
                "+Key at:%p refcount:%d, value swapped at: page %llu "
                "using %llu pages\r\n",
                (void*)key, key->refcount, (unsigned long long) key->vm.page,
                (unsigned long long) key->vm.usedpages));
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"swapin") && c->argc == 3) {
        lookupKeyRead(c->db,c->argv[2]);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"swapout") && c->argc == 3) {
        dictEntry *de = dictFind(c->db->dict,c->argv[2]);
        robj *key, *val;

        if (!server.vm_enabled) {
            addReplySds(c,sdsnew("-ERR Virtual Memory is disabled\r\n"));
            return;
        }
        if (!de) {
            addReply(c,shared.nokeyerr);
            return;
        }
        key = dictGetEntryKey(de);
        val = dictGetEntryVal(de);
        /* If the key is shared we want to create a copy */
        if (key->refcount > 1) {
            robj *newkey = dupStringObject(key);
            decrRefCount(key);
            key = dictGetEntryKey(de) = newkey;
        }
        /* Swap it */
        if (key->storage != REDIS_VM_MEMORY) {
            addReplySds(c,sdsnew("-ERR This key is not in memory\r\n"));
        } else if (vmSwapObjectBlocking(key,val) == REDIS_OK) {
            dictGetEntryVal(de) = NULL;
            addReply(c,shared.ok);
        } else {
            addReply(c,shared.err);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"populate") && c->argc == 3) {
        long keys, j;
        robj *key, *val;
        char buf[128];

        if (getLongFromObjectOrReply(c, c->argv[2], &keys, NULL) != REDIS_OK)
            return;
        for (j = 0; j < keys; j++) {
            snprintf(buf,sizeof(buf),"key:%lu",j);
            key = createStringObject(buf,strlen(buf));
            if (lookupKeyRead(c->db,key) != NULL) {
                decrRefCount(key);
                continue;
            }
            snprintf(buf,sizeof(buf),"value:%lu",j);
            val = createStringObject(buf,strlen(buf));
            dictAdd(c->db->dict,key,val);
        }
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"digest") && c->argc == 2) {
        unsigned char digest[20];
        sds d = sdsnew("+");
        int j;

        computeDatasetDigest(digest);
        for (j = 0; j < 20; j++)
            d = sdscatprintf(d, "%02x",digest[j]);

        d = sdscatlen(d,"\r\n",2);
        addReplySds(c,d);
    } else {
        addReplySds(c,sdsnew(
            "-ERR Syntax error, try DEBUG [SEGFAULT|OBJECT <key>|SWAPIN <key>|SWAPOUT <key>|RELOAD]\r\n"));
    }
}

static void _redisAssert(char *estr, char *file, int line) {
    redisLog(REDIS_WARNING,"=== ASSERTION FAILED ===");
    redisLog(REDIS_WARNING,"==> %s:%d '%s' is not true",file,line,estr);
#ifdef HAVE_BACKTRACE
    redisLog(REDIS_WARNING,"(forcing SIGSEGV in order to print the stack trace)");
    *((char*)-1) = 'x';
#endif
}

static void _redisPanic(char *msg, char *file, int line) {
    redisLog(REDIS_WARNING,"!!! Software Failure. Press left mouse button to continue");
    redisLog(REDIS_WARNING,"Guru Meditation: %s #%s:%d",msg,file,line);
#ifdef HAVE_BACKTRACE
    redisLog(REDIS_WARNING,"(forcing SIGSEGV in order to print the stack trace)");
    *((char*)-1) = 'x';
#endif
}

/* =================================== Main! ================================ */

#ifdef __linux__
int linuxOvercommitMemoryValue(void) {
    FILE *fp = fopen("/proc/sys/vm/overcommit_memory","r");
    char buf[64];

    if (!fp) return -1;
    if (fgets(buf,64,fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    return atoi(buf);
}

void linuxOvercommitMemoryWarning(void) {
    if (linuxOvercommitMemoryValue() == 0) {
        redisLog(REDIS_WARNING,"WARNING overcommit_memory is set to 0! Background save may fail under low memory condition. To fix this issue add 'vm.overcommit_memory = 1' to /etc/sysctl.conf and then reboot or run the command 'sysctl vm.overcommit_memory=1' for this to take effect.");
    }
}
#endif /* __linux__ */

static void daemonize(void) {
    int fd;
    FILE *fp;

    if (fork() != 0) exit(0); /* parent exits */
    setsid(); /* create a new session */

    /* Every output goes to /dev/null. If Redis is daemonized but
     * the 'logfile' is set to 'stdout' in the configuration file
     * it will not log at all. */
    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
    /* Try to write the pid file */
    fp = fopen(server.pidfile,"w");
    if (fp) {
        fprintf(fp,"%d\n",getpid());
        fclose(fp);
    }
}

static void version() {
    printf("Redis server version %s\n", REDIS_VERSION);
    exit(0);
}

static void usage() {
    fprintf(stderr,"Usage: ./redis-server [/path/to/redis.conf]\n");
    fprintf(stderr,"       ./redis-server - (read config from stdin)\n");
    exit(1);
}

int main(int argc, char **argv) {
    time_t start;

    initServerConfig();
    if (argc == 2) {
        if (strcmp(argv[1], "-v") == 0 ||
            strcmp(argv[1], "--version") == 0) version();
        if (strcmp(argv[1], "--help") == 0) usage();
        resetServerSaveParams();
        loadServerConfig(argv[1]);
    } else if ((argc > 2)) {
        usage();
    } else {
        redisLog(REDIS_WARNING,"Warning: no config file specified, using the default config. In order to specify a config file use 'redis-server /path/to/redis.conf'");
    }
    if (server.daemonize) daemonize();
    initServer();
    redisLog(REDIS_NOTICE,"Server started, Redis version " REDIS_VERSION);
#ifdef __linux__
    linuxOvercommitMemoryWarning();
#endif
    start = time(NULL);
    if (server.appendonly) {
        if (loadAppendOnlyFile(server.appendfilename) == REDIS_OK)
            redisLog(REDIS_NOTICE,"DB loaded from append only file: %ld seconds",time(NULL)-start);
    } else {
        if (rdbLoad(server.dbfilename) == REDIS_OK)
            redisLog(REDIS_NOTICE,"DB loaded from disk: %ld seconds",time(NULL)-start);
    }
    redisLog(REDIS_NOTICE,"The server is now ready to accept connections on port %d", server.port);
    aeSetBeforeSleepProc(server.el,beforeSleep);
    aeMain(server.el);
    aeDeleteEventLoop(server.el);
    return 0;
}

/* ============================= Backtrace support ========================= */

#ifdef HAVE_BACKTRACE
static char *findFuncName(void *pointer, unsigned long *offset);

static void *getMcontextEip(ucontext_t *uc) {
#if defined(__FreeBSD__)
    return (void*) uc->uc_mcontext.mc_eip;
#elif defined(__dietlibc__)
    return (void*) uc->uc_mcontext.eip;
#elif defined(__APPLE__) && !defined(MAC_OS_X_VERSION_10_6)
  #if __x86_64__
    return (void*) uc->uc_mcontext->__ss.__rip;
  #else
    return (void*) uc->uc_mcontext->__ss.__eip;
  #endif
#elif defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6)
  #if defined(_STRUCT_X86_THREAD_STATE64) && !defined(__i386__)
    return (void*) uc->uc_mcontext->__ss.__rip;
  #else
    return (void*) uc->uc_mcontext->__ss.__eip;
  #endif
#elif defined(__i386__) || defined(__X86_64__) || defined(__x86_64__)
    return (void*) uc->uc_mcontext.gregs[REG_EIP]; /* Linux 32/64 bit */
#elif defined(__ia64__) /* Linux IA64 */
    return (void*) uc->uc_mcontext.sc_ip;
#else
    return NULL;
#endif
}

static void segvHandler(int sig, siginfo_t *info, void *secret) {
    void *trace[100];
    char **messages = NULL;
    int i, trace_size = 0;
    unsigned long offset=0;
    ucontext_t *uc = (ucontext_t*) secret;
    sds infostring;
    REDIS_NOTUSED(info);

    redisLog(REDIS_WARNING,
        "======= Ooops! Redis %s got signal: -%d- =======", REDIS_VERSION, sig);
    infostring = genRedisInfoString();
    redisLog(REDIS_WARNING, "%s",infostring);
    /* It's not safe to sdsfree() the returned string under memory
     * corruption conditions. Let it leak as we are going to abort */

    trace_size = backtrace(trace, 100);
    /* overwrite sigaction with caller's address */
    if (getMcontextEip(uc) != NULL) {
        trace[1] = getMcontextEip(uc);
    }
    messages = backtrace_symbols(trace, trace_size);

    for (i=1; i<trace_size; ++i) {
        char *fn = findFuncName(trace[i], &offset), *p;

        p = strchr(messages[i],'+');
        if (!fn || (p && ((unsigned long)strtol(p+1,NULL,10)) < offset)) {
            redisLog(REDIS_WARNING,"%s", messages[i]);
        } else {
            redisLog(REDIS_WARNING,"%d redis-server %p %s + %d", i, trace[i], fn, (unsigned int)offset);
        }
    }
    /* free(messages); Don't call free() with possibly corrupted memory. */
    _exit(0);
}

static void sigtermHandler(int sig) {
    REDIS_NOTUSED(sig);

    redisLog(REDIS_WARNING,"SIGTERM received, scheduling shutting down...");
    server.shutdown_asap = 1;
}

static void setupSigSegvAction(void) {
    struct sigaction act;

    sigemptyset (&act.sa_mask);
    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction
     * is used. Otherwise, sa_handler is used */
    act.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESETHAND | SA_SIGINFO;
    act.sa_sigaction = segvHandler;
    sigaction (SIGSEGV, &act, NULL);
    sigaction (SIGBUS, &act, NULL);
    sigaction (SIGFPE, &act, NULL);
    sigaction (SIGILL, &act, NULL);
    sigaction (SIGBUS, &act, NULL);

    act.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESETHAND;
    act.sa_handler = sigtermHandler;
    sigaction (SIGTERM, &act, NULL);
    return;
}

#include "staticsymbols.h"
/* This function try to convert a pointer into a function name. It's used in
 * oreder to provide a backtrace under segmentation fault that's able to
 * display functions declared as static (otherwise the backtrace is useless). */
static char *findFuncName(void *pointer, unsigned long *offset){
    int i, ret = -1;
    unsigned long off, minoff = 0;

    /* Try to match against the Symbol with the smallest offset */
    for (i=0; symsTable[i].pointer; i++) {
        unsigned long lp = (unsigned long) pointer;

        if (lp != (unsigned long)-1 && lp >= symsTable[i].pointer) {
            off=lp-symsTable[i].pointer;
            if (ret < 0 || off < minoff) {
                minoff=off;
                ret=i;
            }
        }
    }
    if (ret == -1) return NULL;
    *offset = minoff;
    return symsTable[ret].name;
}
#else /* HAVE_BACKTRACE */
static void setupSigSegvAction(void) {
}
#endif /* HAVE_BACKTRACE */



/* The End */




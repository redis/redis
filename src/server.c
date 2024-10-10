/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 *
 * Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
 */

#include "server.h"
#include "monotonic.h"
#include "cluster.h"
#include "slowlog.h"
#include "bio.h"
#include "latency.h"
#include "atomicvar.h"
#include "mt19937-64.h"
#include "functions.h"
#include "hdr_histogram.h"
#include "syscheck.h"
#include "threads_mngr.h"
#include "fmtargs.h"
#include "mstr.h"
#include "ebuckets.h"

#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <sys/utsname.h>
#include <locale.h>
#include <sys/socket.h>

#ifdef __linux__
#include <sys/mman.h>
#endif

#if defined(HAVE_SYSCTL_KIPC_SOMAXCONN) || defined(HAVE_SYSCTL_KERN_SOMAXCONN)
#include <sys/sysctl.h>
#endif

#ifdef __GNUC__
#define GNUC_VERSION_STR STRINGIFY(__GNUC__) "." STRINGIFY(__GNUC_MINOR__) "." STRINGIFY(__GNUC_PATCHLEVEL__)
#else
#define GNUC_VERSION_STR "0.0.0"
#endif

/* Our shared "common" objects */

struct sharedObjectsStruct shared;

/* Global vars that are actually used as constants. The following double
 * values are used for double on-disk serialization, and are initialized
 * at runtime to avoid strange compiler optimizations. */

double R_Zero, R_PosInf, R_NegInf, R_Nan;

/*================================= Globals ================================= */

/* Global vars */
struct redisServer server; /* Server global state */

/*============================ Internal prototypes ========================== */

static inline int isShutdownInitiated(void);
int isReadyToShutdown(void);
int finishShutdown(void);
const char *replstateToString(int replstate);

/*============================ Utility functions ============================ */

/* This macro tells if we are in the context of loading an AOF. */
#define isAOFLoadingContext() \
    ((server.current_client && server.current_client->id == CLIENT_ID_AOF) ? 1 : 0)

/* We use a private localtime implementation which is fork-safe. The logging
 * function of Redis may be called from other threads. */
void nolocks_localtime(struct tm *tmp, time_t t, time_t tz, int dst);

/* Low level logging. To use only for very big messages, otherwise
 * serverLog() is to prefer. */
void serverLogRaw(int level, const char *msg) {
    const int syslogLevelMap[] = { LOG_DEBUG, LOG_INFO, LOG_NOTICE, LOG_WARNING };
    const char *c = ".-*#";
    FILE *fp;
    char buf[64];
    int rawmode = (level & LL_RAW);
    int log_to_stdout = server.logfile[0] == '\0';

    level &= 0xff; /* clear flags */
    if (level < server.verbosity) return;

    fp = log_to_stdout ? stdout : fopen(server.logfile,"a");
    if (!fp) return;

    if (rawmode) {
        fprintf(fp,"%s",msg);
    } else {
        int off;
        struct timeval tv;
        int role_char;
        int daylight_active = 0;
        pid_t pid = getpid();

        gettimeofday(&tv,NULL);
        struct tm tm;
        atomicGet(server.daylight_active, daylight_active);
        nolocks_localtime(&tm,tv.tv_sec,server.timezone,daylight_active);
        off = strftime(buf,sizeof(buf),"%d %b %Y %H:%M:%S.",&tm);
        snprintf(buf+off,sizeof(buf)-off,"%03d",(int)tv.tv_usec/1000);
        if (server.sentinel_mode) {
            role_char = 'X'; /* Sentinel. */
        } else if (pid != server.pid) {
            role_char = 'C'; /* RDB / AOF writing child. */
        } else {
            role_char = (server.masterhost ? 'S':'M'); /* Slave or Master. */
        }
        fprintf(fp,"%d:%c %s %c %s\n",
            (int)getpid(),role_char, buf,c[level],msg);
    }
    fflush(fp);

    if (!log_to_stdout) fclose(fp);
    if (server.syslog_enabled) syslog(syslogLevelMap[level], "%s", msg);
}

/* Like serverLogRaw() but with printf-alike support. This is the function that
 * is used across the code. The raw version is only used in order to dump
 * the INFO output on crash. */
void _serverLog(int level, const char *fmt, ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    serverLogRaw(level,msg);
}

/* Low level logging from signal handler. Should be used with pre-formatted strings. 
   See serverLogFromHandler. */
void serverLogRawFromHandler(int level, const char *msg) {
    int fd;
    int log_to_stdout = server.logfile[0] == '\0';
    char buf[64];

    if ((level&0xff) < server.verbosity || (log_to_stdout && server.daemonize))
        return;
    fd = log_to_stdout ? STDOUT_FILENO :
                         open(server.logfile, O_APPEND|O_CREAT|O_WRONLY, 0644);
    if (fd == -1) return;
    if (level & LL_RAW) {
        if (write(fd,msg,strlen(msg)) == -1) goto err;
    }
    else {
        ll2string(buf,sizeof(buf),getpid());
        if (write(fd,buf,strlen(buf)) == -1) goto err;
        if (write(fd,":signal-handler (",17) == -1) goto err;
        ll2string(buf,sizeof(buf),time(NULL));
        if (write(fd,buf,strlen(buf)) == -1) goto err;
        if (write(fd,") ",2) == -1) goto err;
        if (write(fd,msg,strlen(msg)) == -1) goto err;
        if (write(fd,"\n",1) == -1) goto err;
    }
err:
    if (!log_to_stdout) close(fd);
}

/* An async-signal-safe version of serverLog. if LL_RAW is not included in level flags,
 * The message format is: <pid>:signal-handler (<time>) <msg> \n
 * with LL_RAW flag only the msg is printed (with no new line at the end)
 *
 * We actually use this only for signals that are not fatal from the point
 * of view of Redis. Signals that are going to kill the server anyway and
 * where we need printf-alike features are served by serverLog(). */
void serverLogFromHandler(int level, const char *fmt, ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];

    va_start(ap, fmt);
    vsnprintf_async_signal_safe(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    serverLogRawFromHandler(level, msg);
}

/* Return the UNIX time in microseconds */
long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}

/* Return the UNIX time in milliseconds */
mstime_t mstime(void) {
    return ustime()/1000;
}

/* Return the command time snapshot in milliseconds.
 * The time the command started is the logical time it runs,
 * and all the time readings during the execution time should
 * reflect the same time.
 * More details can be found in the comments below. */
mstime_t commandTimeSnapshot(void) {
    /* When we are in the middle of a command execution, we want to use a
     * reference time that does not change: in that case we just use the
     * cached time, that we update before each call in the call() function.
     * This way we avoid that commands such as RPOPLPUSH or similar, that
     * may re-open the same key multiple times, can invalidate an already
     * open object in a next call, if the next call will see the key expired,
     * while the first did not.
     * This is specifically important in the context of scripts, where we
     * pretend that time freezes. This way a key can expire only the first time
     * it is accessed and not in the middle of the script execution, making
     * propagation to slaves / AOF consistent. See issue #1525 for more info.
     * Note that we cannot use the cached server.mstime because it can change
     * in processEventsWhileBlocked etc. */
    return server.cmd_time_snapshot;
}

/* After an RDB dump or AOF rewrite we exit from children using _exit() instead of
 * exit(), because the latter may interact with the same file objects used by
 * the parent process. However if we are testing the coverage normal exit() is
 * used in order to obtain the right coverage information. */
void exitFromChild(int retcode) {
#ifdef COVERAGE_TEST
    exit(retcode);
#else
    _exit(retcode);
#endif
}

/*====================== Hash table type implementation  ==================== */

/* This is a hash table type that uses the SDS dynamic strings library as
 * keys and redis objects as values (objects can hold SDS strings,
 * lists, sets). */

void dictVanillaFree(dict *d, void *val)
{
    UNUSED(d);
    zfree(val);
}

void dictListDestructor(dict *d, void *val)
{
    UNUSED(d);
    listRelease((list*)val);
}

void dictDictDestructor(dict *d, void *val)
{
    UNUSED(d);
    dictRelease((dict*)val);
}

int dictSdsKeyCompare(dict *d, const void *key1,
        const void *key2)
{
    int l1,l2;
    UNUSED(d);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

int dictSdsMstrKeyCompare(dict *d, const void *sdsLookup, const void *mstrStored)
{
    int l1,l2;
    UNUSED(d);

    l1 = sdslen((sds)sdsLookup);
    l2 = hfieldlen((hfield)mstrStored);
    if (l1 != l2) return 0;
    return memcmp(sdsLookup, mstrStored, l1) == 0;
}


/* A case insensitive version used for the command lookup table and other
 * places where case insensitive non binary-safe comparison is needed. */
int dictSdsKeyCaseCompare(dict *d, const void *key1,
        const void *key2)
{
    UNUSED(d);
    return strcasecmp(key1, key2) == 0;
}

void dictObjectDestructor(dict *d, void *val)
{
    UNUSED(d);
    if (val == NULL) return; /* Lazy freeing will set value to NULL. */
    decrRefCount(val);
}

void dictSdsDestructor(dict *d, void *val)
{
    UNUSED(d);
    sdsfree(val);
}

void *dictSdsDup(dict *d, const void *key) {
    UNUSED(d);
    return sdsdup((const sds) key);
}

int dictObjKeyCompare(dict *d, const void *key1,
        const void *key2)
{
    const robj *o1 = key1, *o2 = key2;
    return dictSdsKeyCompare(d, o1->ptr,o2->ptr);
}

uint64_t dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}

uint64_t dictPtrHash(const void *key) {
    return dictGenHashFunction((unsigned char*)&key,sizeof(key));
}

uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

uint64_t dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char*)key, sdslen((char*)key));
}

/* Dict hash function for null terminated string */
uint64_t dictCStrHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, strlen((char*)key));
}

/* Dict hash function for null terminated string */
uint64_t dictCStrCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char*)key, strlen((char*)key));
}

/* Dict hash function for client */
uint64_t dictClientHash(const void *key) {
    return ((client *)key)->id;
}

/* Dict compare function for client */
int dictClientKeyCompare(dict *d, const void *key1, const void *key2) {
    UNUSED(d);
    return ((client *)key1)->id == ((client *)key2)->id;
}

/* Dict compare function for null terminated string */
int dictCStrKeyCompare(dict *d, const void *key1, const void *key2) {
    int l1,l2;
    UNUSED(d);

    l1 = strlen((char*)key1);
    l2 = strlen((char*)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

/* Dict case insensitive compare function for null terminated string */
int dictCStrKeyCaseCompare(dict *d, const void *key1, const void *key2) {
    UNUSED(d);
    return strcasecmp(key1, key2) == 0;
}

int dictEncObjKeyCompare(dict *d, const void *key1, const void *key2)
{
    robj *o1 = (robj*) key1, *o2 = (robj*) key2;
    int cmp;

    if (o1->encoding == OBJ_ENCODING_INT &&
        o2->encoding == OBJ_ENCODING_INT)
            return o1->ptr == o2->ptr;

    /* Due to OBJ_STATIC_REFCOUNT, we avoid calling getDecodedObject() without
     * good reasons, because it would incrRefCount() the object, which
     * is invalid. So we check to make sure dictFind() works with static
     * objects as well. */
    if (o1->refcount != OBJ_STATIC_REFCOUNT) o1 = getDecodedObject(o1);
    if (o2->refcount != OBJ_STATIC_REFCOUNT) o2 = getDecodedObject(o2);
    cmp = dictSdsKeyCompare(d,o1->ptr,o2->ptr);
    if (o1->refcount != OBJ_STATIC_REFCOUNT) decrRefCount(o1);
    if (o2->refcount != OBJ_STATIC_REFCOUNT) decrRefCount(o2);
    return cmp;
}

uint64_t dictEncObjHash(const void *key) {
    robj *o = (robj*) key;

    if (sdsEncodedObject(o)) {
        return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
    } else if (o->encoding == OBJ_ENCODING_INT) {
        char buf[32];
        int len;

        len = ll2string(buf,32,(long)o->ptr);
        return dictGenHashFunction((unsigned char*)buf, len);
    } else {
        serverPanic("Unknown string encoding");
    }
}

/* Return 1 if currently we allow dict to expand. Dict may allocate huge
 * memory to contain hash buckets when dict expands, that may lead redis
 * rejects user's requests or evicts some keys, we can stop dict to expand
 * provisionally if used memory will be over maxmemory after dict expands,
 * but to guarantee the performance of redis, we still allow dict to expand
 * if dict load factor exceeds HASHTABLE_MAX_LOAD_FACTOR. */
int dictResizeAllowed(size_t moreMem, double usedRatio) {
    /* for debug purposes: dict is not allowed to be resized. */
    if (!server.dict_resizing) return 0;

    if (usedRatio <= HASHTABLE_MAX_LOAD_FACTOR) {
        return !overMaxmemoryAfterAlloc(moreMem);
    } else {
        return 1;
    }
}

/* Generic hash table type where keys are Redis Objects, Values
 * dummy pointers. */
dictType objectKeyPointerValueDictType = {
    dictEncObjHash,            /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictEncObjKeyCompare,      /* key compare */
    dictObjectDestructor,      /* key destructor */
    NULL,                      /* val destructor */
    NULL                       /* allow to expand */
};

/* Like objectKeyPointerValueDictType(), but values can be destroyed, if
 * not NULL, calling zfree(). */
dictType objectKeyHeapPointerValueDictType = {
    dictEncObjHash,            /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictEncObjKeyCompare,      /* key compare */
    dictObjectDestructor,      /* key destructor */
    dictVanillaFree,           /* val destructor */
    NULL                       /* allow to expand */
};

/* Set dictionary type. Keys are SDS strings, values are not used. */
dictType setDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    dictSdsDestructor,         /* key destructor */
    NULL,                      /* val destructor */
    NULL,                      /* allow to expand */
    .no_value = 1,             /* no values in this dict */
    .keys_are_odd = 1          /* an SDS string is always an odd pointer */
};

/* Sorted sets hash (note: a skiplist is used in addition to the hash table) */
dictType zsetDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    NULL,                      /* Note: SDS string shared & freed by skiplist */
    NULL,                      /* val destructor */
    NULL,                      /* allow to expand */
};

/* Db->dict, keys are sds strings, vals are Redis objects. */
dictType dbDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictObjectDestructor,       /* val destructor */
    dictResizeAllowed,          /* allow to resize */
};

/* Db->expires */
dictType dbExpiresDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    NULL,                       /* key destructor */
    NULL,                       /* val destructor */
    dictResizeAllowed,          /* allow to resize */
};

/* Command table. sds string -> command struct pointer. */
dictType commandTableDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL,                       /* val destructor */
    NULL                        /* allow to expand */
};

/* Hash type hash table (note that small hashes are represented with listpacks) */
dictType hashDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictSdsDestructor,          /* val destructor */
    NULL,                       /* allow to expand */
};

/* Dict type without destructor */
dictType sdsReplyDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    NULL,                       /* key destructor */
    NULL,                       /* val destructor */
    NULL                        /* allow to expand */
};

/* Keylist hash table type has unencoded redis objects as keys and
 * lists as values. It's used for blocking operations (BLPOP) and to
 * map swapped keys to a list of clients waiting for this keys to be loaded. */
dictType keylistDictType = {
    dictObjHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictObjKeyCompare,          /* key compare */
    dictObjectDestructor,       /* key destructor */
    dictListDestructor,         /* val destructor */
    NULL                        /* allow to expand */
};

/* KeyDict hash table type has unencoded redis objects as keys and
 * dicts as values. It's used for PUBSUB command to track clients subscribing the channels. */
dictType objToDictDictType = {
    dictObjHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictObjKeyCompare,          /* key compare */
    dictObjectDestructor,       /* key destructor */
    dictDictDestructor,         /* val destructor */
    NULL                        /* allow to expand */
};

/* Modules system dictionary type. Keys are module name,
 * values are pointer to RedisModule struct. */
dictType modulesDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL,                       /* val destructor */
    NULL                        /* allow to expand */
};

/* Migrate cache dict type. */
dictType migrateCacheDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL,                       /* val destructor */
    NULL                        /* allow to expand */
};

/* Dict for for case-insensitive search using null terminated C strings.
 * The keys stored in dict are sds though. */
dictType stringSetDictType = {
    dictCStrCaseHash,           /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictCStrKeyCaseCompare,     /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL,                       /* val destructor */
    NULL                        /* allow to expand */
};

/* Dict for for case-insensitive search using null terminated C strings.
 * The key and value do not have a destructor. */
dictType externalStringType = {
    dictCStrCaseHash,           /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictCStrKeyCaseCompare,     /* key compare */
    NULL,                       /* key destructor */
    NULL,                       /* val destructor */
    NULL                        /* allow to expand */
};

/* Dict for case-insensitive search using sds objects with a zmalloc
 * allocated object as the value. */
dictType sdsHashDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictVanillaFree,            /* val destructor */
    NULL                        /* allow to expand */
};

/* Client Set dictionary type. Keys are client, values are not used. */
dictType clientDictType = {
    dictClientHash,             /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictClientKeyCompare,       /* key compare */
    .no_value = 1               /* no values in this dict */
};

/* This function is called once a background process of some kind terminates,
 * as we want to avoid resizing the hash tables when there is a child in order
 * to play well with copy-on-write (otherwise when a resize happens lots of
 * memory pages are copied). The goal of this function is to update the ability
 * for dict.c to resize or rehash the tables accordingly to the fact we have an
 * active fork child running. */
void updateDictResizePolicy(void) {
    if (server.in_fork_child != CHILD_TYPE_NONE)
        dictSetResizeEnabled(DICT_RESIZE_FORBID);
    else if (hasActiveChildProcess())
        dictSetResizeEnabled(DICT_RESIZE_AVOID);
    else
        dictSetResizeEnabled(DICT_RESIZE_ENABLE);
}

const char *strChildType(int type) {
    switch(type) {
        case CHILD_TYPE_RDB: return "RDB";
        case CHILD_TYPE_AOF: return "AOF";
        case CHILD_TYPE_LDB: return "LDB";
        case CHILD_TYPE_MODULE: return "MODULE";
        default: return "Unknown";
    }
}

/* Return true if there are active children processes doing RDB saving,
 * AOF rewriting, or some side process spawned by a loaded module. */
int hasActiveChildProcess(void) {
    return server.child_pid != -1;
}

void resetChildState(void) {
    server.child_type = CHILD_TYPE_NONE;
    server.child_pid = -1;
    server.stat_current_cow_peak = 0;
    server.stat_current_cow_bytes = 0;
    server.stat_current_cow_updated = 0;
    server.stat_current_save_keys_processed = 0;
    server.stat_module_progress = 0;
    server.stat_current_save_keys_total = 0;
    updateDictResizePolicy();
    closeChildInfoPipe();
    moduleFireServerEvent(REDISMODULE_EVENT_FORK_CHILD,
                          REDISMODULE_SUBEVENT_FORK_CHILD_DIED,
                          NULL);
}

/* Return if child type is mutually exclusive with other fork children */
int isMutuallyExclusiveChildType(int type) {
    return type == CHILD_TYPE_RDB || type == CHILD_TYPE_AOF || type == CHILD_TYPE_MODULE;
}

/* Returns true when we're inside a long command that yielded to the event loop. */
int isInsideYieldingLongCommand(void) {
    return scriptIsTimedout() || server.busy_module_yield_flags;
}

/* Return true if this instance has persistence completely turned off:
 * both RDB and AOF are disabled. */
int allPersistenceDisabled(void) {
    return server.saveparamslen == 0 && server.aof_state == AOF_OFF;
}

/* ======================= Cron: called every 100 ms ======================== */

/* Add a sample to the instantaneous metric. This function computes the quotient
 * of the increment of value and base, which is useful to record operation count
 * per second, or the average time consumption of an operation.
 *
 * current_value - The dividend
 * current_base - The divisor
 * */
void trackInstantaneousMetric(int metric, long long current_value, long long current_base, long long factor) {
    if (server.inst_metric[metric].last_sample_base > 0) {
        long long base = current_base - server.inst_metric[metric].last_sample_base;
        long long value = current_value - server.inst_metric[metric].last_sample_value;
        long long avg = base > 0 ? (value * factor / base) : 0;
        server.inst_metric[metric].samples[server.inst_metric[metric].idx] = avg;
        server.inst_metric[metric].idx++;
        server.inst_metric[metric].idx %= STATS_METRIC_SAMPLES;
    }
    server.inst_metric[metric].last_sample_base = current_base;
    server.inst_metric[metric].last_sample_value = current_value;
}

/* Return the mean of all the samples. */
long long getInstantaneousMetric(int metric) {
    int j;
    long long sum = 0;

    for (j = 0; j < STATS_METRIC_SAMPLES; j++)
        sum += server.inst_metric[metric].samples[j];
    return sum / STATS_METRIC_SAMPLES;
}

/* The client query buffer is an sds.c string that can end with a lot of
 * free space not used, this function reclaims space if needed.
 *
 * The function always returns 0 as it never terminates the client. */
int clientsCronResizeQueryBuffer(client *c) {
    /* If the client query buffer is NULL, it is using the reusable query buffer and there is nothing to do. */
    if (c->querybuf == NULL) return 0;
    size_t querybuf_size = sdsalloc(c->querybuf);
    time_t idletime = server.unixtime - c->lastinteraction;

    /* Only resize the query buffer if the buffer is actually wasting at least a
     * few kbytes */
    if (sdsavail(c->querybuf) > 1024*4) {
        /* There are two conditions to resize the query buffer: */
        if (idletime > 2) {
            /* 1) Query is idle for a long time. */
            size_t remaining = sdslen(c->querybuf) - c->qb_pos;
            if (!(c->flags & CLIENT_MASTER) && !remaining) {
                /* If the client is not a master and no data is pending,
                 * The client can safely use the reusable query buffer in the next read - free the client's querybuf. */
                sdsfree(c->querybuf);
                /* By setting the querybuf to NULL, the client will use the reusable query buffer in the next read.
                 * We don't move the client to the reusable query buffer immediately, because if we allocated a private
                 * query buffer for the client, it's likely that the client will use it again soon. */
                c->querybuf = NULL;
            } else {
                c->querybuf = sdsRemoveFreeSpace(c->querybuf, 1);
            }
        } else if (querybuf_size > PROTO_RESIZE_THRESHOLD && querybuf_size/2 > c->querybuf_peak) {
            /* 2) Query buffer is too big for latest peak and is larger than
             *    resize threshold. Trim excess space but only up to a limit,
             *    not below the recent peak and current c->querybuf (which will
             *    be soon get used). If we're in the middle of a bulk then make
             *    sure not to resize to less than the bulk length. */
            size_t resize = sdslen(c->querybuf);
            if (resize < c->querybuf_peak) resize = c->querybuf_peak;
            if (c->bulklen != -1 && resize < (size_t)c->bulklen + 2) resize = c->bulklen + 2;
            c->querybuf = sdsResize(c->querybuf, resize, 1);
        }
    }

    /* Reset the peak again to capture the peak memory usage in the next
     * cycle. */
    c->querybuf_peak = c->querybuf ? sdslen(c->querybuf) : 0;
    /* We reset to either the current used, or currently processed bulk size,
     * which ever is bigger. */
    if (c->bulklen != -1 && (size_t)c->bulklen + 2 > c->querybuf_peak) c->querybuf_peak = c->bulklen + 2;
    return 0;
}

/* The client output buffer can be adjusted to better fit the memory requirements.
 *
 * the logic is:
 * in case the last observed peak size of the buffer equals the buffer size - we double the size
 * in case the last observed peak size of the buffer is less than half the buffer size - we shrink by half.
 * The buffer peak will be reset back to the buffer position every server.reply_buffer_peak_reset_time milliseconds
 * The function always returns 0 as it never terminates the client. */
int clientsCronResizeOutputBuffer(client *c, mstime_t now_ms) {

    size_t new_buffer_size = 0;
    char *oldbuf = NULL;
    const size_t buffer_target_shrink_size = c->buf_usable_size/2;
    const size_t buffer_target_expand_size = c->buf_usable_size*2;

    /* in case the resizing is disabled return immediately */
    if(!server.reply_buffer_resizing_enabled)
        return 0;

    if (buffer_target_shrink_size >= PROTO_REPLY_MIN_BYTES &&
        c->buf_peak < buffer_target_shrink_size )
    {
        new_buffer_size = max(PROTO_REPLY_MIN_BYTES,c->buf_peak+1);
        server.stat_reply_buffer_shrinks++;
    } else if (buffer_target_expand_size < PROTO_REPLY_CHUNK_BYTES*2 &&
        c->buf_peak == c->buf_usable_size)
    {
        new_buffer_size = min(PROTO_REPLY_CHUNK_BYTES,buffer_target_expand_size);
        server.stat_reply_buffer_expands++;
    }

    serverAssertWithInfo(c, NULL, (!new_buffer_size) || (new_buffer_size >= (size_t)c->bufpos));

    /* reset the peak value each server.reply_buffer_peak_reset_time seconds. in case the client will be idle
     * it will start to shrink.
     */
    if (server.reply_buffer_peak_reset_time >=0 &&
        now_ms - c->buf_peak_last_reset_time >= server.reply_buffer_peak_reset_time)
    {
        c->buf_peak = c->bufpos;
        c->buf_peak_last_reset_time = now_ms;
    }

    if (new_buffer_size) {
        oldbuf = c->buf;
        c->buf = zmalloc_usable(new_buffer_size, &c->buf_usable_size);
        memcpy(c->buf,oldbuf,c->bufpos);
        zfree(oldbuf);
    }
    return 0;
}

/* This function is used in order to track clients using the biggest amount
 * of memory in the latest few seconds. This way we can provide such information
 * in the INFO output (clients section), without having to do an O(N) scan for
 * all the clients.
 *
 * This is how it works. We have an array of CLIENTS_PEAK_MEM_USAGE_SLOTS slots
 * where we track, for each, the biggest client output and input buffers we
 * saw in that slot. Every slot corresponds to one of the latest seconds, since
 * the array is indexed by doing UNIXTIME % CLIENTS_PEAK_MEM_USAGE_SLOTS.
 *
 * When we want to know what was recently the peak memory usage, we just scan
 * such few slots searching for the maximum value. */
#define CLIENTS_PEAK_MEM_USAGE_SLOTS 8
size_t ClientsPeakMemInput[CLIENTS_PEAK_MEM_USAGE_SLOTS] = {0};
size_t ClientsPeakMemOutput[CLIENTS_PEAK_MEM_USAGE_SLOTS] = {0};

int clientsCronTrackExpansiveClients(client *c, int time_idx) {
    size_t qb_size = c->querybuf ? sdsZmallocSize(c->querybuf) : 0;
    size_t argv_size = c->argv ? zmalloc_size(c->argv) : 0;
    size_t in_usage = qb_size + c->argv_len_sum + argv_size;
    size_t out_usage = getClientOutputBufferMemoryUsage(c);

    /* Track the biggest values observed so far in this slot. */
    if (in_usage > ClientsPeakMemInput[time_idx]) ClientsPeakMemInput[time_idx] = in_usage;
    if (out_usage > ClientsPeakMemOutput[time_idx]) ClientsPeakMemOutput[time_idx] = out_usage;

    return 0; /* This function never terminates the client. */
}

/* All normal clients are placed in one of the "mem usage buckets" according
 * to how much memory they currently use. We use this function to find the
 * appropriate bucket based on a given memory usage value. The algorithm simply
 * does a log2(mem) to ge the bucket. This means, for examples, that if a
 * client's memory usage doubles it's moved up to the next bucket, if it's
 * halved we move it down a bucket.
 * For more details see CLIENT_MEM_USAGE_BUCKETS documentation in server.h. */
static inline clientMemUsageBucket *getMemUsageBucket(size_t mem) {
    int size_in_bits = 8*(int)sizeof(mem);
    int clz = mem > 0 ? __builtin_clzl(mem) : size_in_bits;
    int bucket_idx = size_in_bits - clz;
    if (bucket_idx > CLIENT_MEM_USAGE_BUCKET_MAX_LOG)
        bucket_idx = CLIENT_MEM_USAGE_BUCKET_MAX_LOG;
    else if (bucket_idx < CLIENT_MEM_USAGE_BUCKET_MIN_LOG)
        bucket_idx = CLIENT_MEM_USAGE_BUCKET_MIN_LOG;
    bucket_idx -= CLIENT_MEM_USAGE_BUCKET_MIN_LOG;
    return &server.client_mem_usage_buckets[bucket_idx];
}

/*
 * This method updates the client memory usage and update the
 * server stats for client type.
 *
 * This method is called from the clientsCron to have updated
 * stats for non CLIENT_TYPE_NORMAL/PUBSUB clients to accurately
 * provide information around clients memory usage.
 *
 * It is also used in updateClientMemUsageAndBucket to have latest
 * client memory usage information to place it into appropriate client memory
 * usage bucket.
 */
void updateClientMemoryUsage(client *c) {
    serverAssert(c->conn);
    size_t mem = getClientMemoryUsage(c, NULL);
    int type = getClientType(c);
    /* Now that we have the memory used by the client, remove the old
     * value from the old category, and add it back. */
    server.stat_clients_type_memory[c->last_memory_type] -= c->last_memory_usage;
    server.stat_clients_type_memory[type] += mem;
    /* Remember what we added and where, to remove it next time. */
    c->last_memory_type = type;
    c->last_memory_usage = mem;
}

int clientEvictionAllowed(client *c) {
    if (server.maxmemory_clients == 0 || c->flags & CLIENT_NO_EVICT || !c->conn) {
        return 0;
    }
    int type = getClientType(c);
    return (type == CLIENT_TYPE_NORMAL || type == CLIENT_TYPE_PUBSUB);
}


/* This function is used to cleanup the client's previously tracked memory usage.
 * This is called during incremental client memory usage tracking as well as
 * used to reset when client to bucket allocation is not required when
 * client eviction is disabled.  */
void removeClientFromMemUsageBucket(client *c, int allow_eviction) {
    if (c->mem_usage_bucket) {
        c->mem_usage_bucket->mem_usage_sum -= c->last_memory_usage;
        /* If this client can't be evicted then remove it from the mem usage
         * buckets */
        if (!allow_eviction) {
            listDelNode(c->mem_usage_bucket->clients, c->mem_usage_bucket_node);
            c->mem_usage_bucket = NULL;
            c->mem_usage_bucket_node = NULL;
        }
    }
}

/* This is called only if explicit clients when something changed their buffers,
 * so we can track clients' memory and enforce clients' maxmemory in real time.
 *
 * This also adds the client to the correct memory usage bucket. Each bucket contains
 * all clients with roughly the same amount of memory. This way we group
 * together clients consuming about the same amount of memory and can quickly
 * free them in case we reach maxmemory-clients (client eviction).
 *
 * Note: This function filters clients of type no-evict, master or replica regardless
 * of whether the eviction is enabled or not, so the memory usage we get from these
 * types of clients via the INFO command may be out of date.
 *
 * returns 1 if client eviction for this client is allowed, 0 otherwise.
 */
int updateClientMemUsageAndBucket(client *c) {
    serverAssert(io_threads_op == IO_THREADS_OP_IDLE && c->conn);
    int allow_eviction = clientEvictionAllowed(c);
    removeClientFromMemUsageBucket(c, allow_eviction);

    if (!allow_eviction) {
        return 0;
    }

    /* Update client memory usage. */
    updateClientMemoryUsage(c);

    /* Update the client in the mem usage buckets */
    clientMemUsageBucket *bucket = getMemUsageBucket(c->last_memory_usage);
    bucket->mem_usage_sum += c->last_memory_usage;
    if (bucket != c->mem_usage_bucket) {
        if (c->mem_usage_bucket)
            listDelNode(c->mem_usage_bucket->clients,
                        c->mem_usage_bucket_node);
        c->mem_usage_bucket = bucket;
        listAddNodeTail(bucket->clients, c);
        c->mem_usage_bucket_node = listLast(bucket->clients);
    }
    return 1;
}

/* Return the max samples in the memory usage of clients tracked by
 * the function clientsCronTrackExpansiveClients(). */
void getExpansiveClientsInfo(size_t *in_usage, size_t *out_usage) {
    size_t i = 0, o = 0;
    for (int j = 0; j < CLIENTS_PEAK_MEM_USAGE_SLOTS; j++) {
        if (ClientsPeakMemInput[j] > i) i = ClientsPeakMemInput[j];
        if (ClientsPeakMemOutput[j] > o) o = ClientsPeakMemOutput[j];
    }
    *in_usage = i;
    *out_usage = o;
}

/* This function is called by serverCron() and is used in order to perform
 * operations on clients that are important to perform constantly. For instance
 * we use this function in order to disconnect clients after a timeout, including
 * clients blocked in some blocking command with a non-zero timeout.
 *
 * The function makes some effort to process all the clients every second, even
 * if this cannot be strictly guaranteed, since serverCron() may be called with
 * an actual frequency lower than server.hz in case of latency events like slow
 * commands.
 *
 * It is very important for this function, and the functions it calls, to be
 * very fast: sometimes Redis has tens of hundreds of connected clients, and the
 * default server.hz value is 10, so sometimes here we need to process thousands
 * of clients per second, turning this function into a source of latency.
 */
#define CLIENTS_CRON_MIN_ITERATIONS 5
void clientsCron(void) {
    /* Try to process at least numclients/server.hz of clients
     * per call. Since normally (if there are no big latency events) this
     * function is called server.hz times per second, in the average case we
     * process all the clients in 1 second. */
    int numclients = listLength(server.clients);
    int iterations = numclients/server.hz;
    mstime_t now = mstime();

    /* Process at least a few clients while we are at it, even if we need
     * to process less than CLIENTS_CRON_MIN_ITERATIONS to meet our contract
     * of processing each client once per second. */
    if (iterations < CLIENTS_CRON_MIN_ITERATIONS)
        iterations = (numclients < CLIENTS_CRON_MIN_ITERATIONS) ?
                     numclients : CLIENTS_CRON_MIN_ITERATIONS;


    int curr_peak_mem_usage_slot = server.unixtime % CLIENTS_PEAK_MEM_USAGE_SLOTS;
    /* Always zero the next sample, so that when we switch to that second, we'll
     * only register samples that are greater in that second without considering
     * the history of such slot.
     *
     * Note: our index may jump to any random position if serverCron() is not
     * called for some reason with the normal frequency, for instance because
     * some slow command is called taking multiple seconds to execute. In that
     * case our array may end containing data which is potentially older
     * than CLIENTS_PEAK_MEM_USAGE_SLOTS seconds: however this is not a problem
     * since here we want just to track if "recently" there were very expansive
     * clients from the POV of memory usage. */
    int zeroidx = (curr_peak_mem_usage_slot+1) % CLIENTS_PEAK_MEM_USAGE_SLOTS;
    ClientsPeakMemInput[zeroidx] = 0;
    ClientsPeakMemOutput[zeroidx] = 0;


    while(listLength(server.clients) && iterations--) {
        client *c;
        listNode *head;

        /* Take the current head, process, and then rotate the head to tail.
         * This way we can fairly iterate all clients step by step. */
        head = listFirst(server.clients);
        c = listNodeValue(head);
        listRotateHeadToTail(server.clients);
        /* The following functions do different service checks on the client.
         * The protocol is that they return non-zero if the client was
         * terminated. */
        if (clientsCronHandleTimeout(c,now)) continue;
        if (clientsCronResizeQueryBuffer(c)) continue;
        if (clientsCronResizeOutputBuffer(c,now)) continue;

        if (clientsCronTrackExpansiveClients(c, curr_peak_mem_usage_slot)) continue;

        /* Iterating all the clients in getMemoryOverheadData() is too slow and
         * in turn would make the INFO command too slow. So we perform this
         * computation incrementally and track the (not instantaneous but updated
         * to the second) total memory used by clients using clientsCron() in
         * a more incremental way (depending on server.hz).
         * If client eviction is enabled, update the bucket as well. */
        if (!updateClientMemUsageAndBucket(c))
            updateClientMemoryUsage(c);

        if (closeClientOnOutputBufferLimitReached(c, 0)) continue;
    }
}

/* This function handles 'background' operations we are required to do
 * incrementally in Redis databases, such as active key expiring, resizing,
 * rehashing. */
void databasesCron(void) {
    /* Expire keys by random sampling. Not required for slaves
     * as master will synthesize DELs for us. */
    if (server.active_expire_enabled) {
        if (iAmMaster()) {
            activeExpireCycle(ACTIVE_EXPIRE_CYCLE_SLOW);
        } else {
            expireSlaveKeys();
        }
    }

    /* Defrag keys gradually. */
    activeDefragCycle();

    /* Perform hash tables rehashing if needed, but only if there are no
     * other processes saving the DB on disk. Otherwise rehashing is bad
     * as will cause a lot of copy-on-write of memory pages. */
    if (!hasActiveChildProcess()) {
        /* We use global counters so if we stop the computation at a given
         * DB we'll be able to start from the successive in the next
         * cron loop iteration. */
        static unsigned int resize_db = 0;
        static unsigned int rehash_db = 0;
        int dbs_per_call = CRON_DBS_PER_CALL;
        int j;

        /* Don't test more DBs than we have. */
        if (dbs_per_call > server.dbnum) dbs_per_call = server.dbnum;

        for (j = 0; j < dbs_per_call; j++) {
            redisDb *db = &server.db[resize_db % server.dbnum];
            kvstoreTryResizeDicts(db->keys, CRON_DICTS_PER_DB);
            kvstoreTryResizeDicts(db->expires, CRON_DICTS_PER_DB);
            resize_db++;
        }

        /* Rehash */
        if (server.activerehashing) {
            uint64_t elapsed_us = 0;
            for (j = 0; j < dbs_per_call; j++) {
                redisDb *db = &server.db[rehash_db % server.dbnum];
                elapsed_us += kvstoreIncrementallyRehash(db->keys, INCREMENTAL_REHASHING_THRESHOLD_US - elapsed_us);
                if (elapsed_us >= INCREMENTAL_REHASHING_THRESHOLD_US)
                    break;
                elapsed_us += kvstoreIncrementallyRehash(db->expires, INCREMENTAL_REHASHING_THRESHOLD_US - elapsed_us);
                if (elapsed_us >= INCREMENTAL_REHASHING_THRESHOLD_US)
                    break;
                rehash_db++;
            }
        }
    }
}

static inline void updateCachedTimeWithUs(int update_daylight_info, const long long ustime) {
    server.ustime = ustime;
    server.mstime = server.ustime / 1000;
    time_t unixtime = server.mstime / 1000;
    atomicSet(server.unixtime, unixtime);

    /* To get information about daylight saving time, we need to call
     * localtime_r and cache the result. However calling localtime_r in this
     * context is safe since we will never fork() while here, in the main
     * thread. The logging function will call a thread safe version of
     * localtime that has no locks. */
    if (update_daylight_info) {
        struct tm tm;
        time_t ut = server.unixtime;
        localtime_r(&ut,&tm);
        atomicSet(server.daylight_active, tm.tm_isdst);
    }
}

/* We take a cached value of the unix time in the global state because with
 * virtual memory and aging there is to store the current time in objects at
 * every object access, and accuracy is not needed. To access a global var is
 * a lot faster than calling time(NULL).
 *
 * This function should be fast because it is called at every command execution
 * in call(), so it is possible to decide if to update the daylight saving
 * info or not using the 'update_daylight_info' argument. Normally we update
 * such info only when calling this function from serverCron() but not when
 * calling it from call(). */
void updateCachedTime(int update_daylight_info) {
    const long long us = ustime();
    updateCachedTimeWithUs(update_daylight_info, us);
}

/* Performing required operations in order to enter an execution unit.
 * In general, if we are already inside an execution unit then there is nothing to do,
 * otherwise we need to update cache times so the same cached time will be used all over
 * the execution unit.
 * update_cached_time - if 0, will not update the cached time even if required.
 * us - if not zero, use this time for cached time, otherwise get current time. */
void enterExecutionUnit(int update_cached_time, long long us) {
    if (server.execution_nesting++ == 0 && update_cached_time) {
        if (us == 0) {
            us = ustime();
        }
        updateCachedTimeWithUs(0, us);
        server.cmd_time_snapshot = server.mstime;
    }
}

void exitExecutionUnit(void) {
    --server.execution_nesting;
}

void checkChildrenDone(void) {
    int statloc = 0;
    pid_t pid;

    if ((pid = waitpid(-1, &statloc, WNOHANG)) != 0) {
        int exitcode = WIFEXITED(statloc) ? WEXITSTATUS(statloc) : -1;
        int bysignal = 0;

        if (WIFSIGNALED(statloc)) bysignal = WTERMSIG(statloc);

        /* sigKillChildHandler catches the signal and calls exit(), but we
         * must make sure not to flag lastbgsave_status, etc incorrectly.
         * We could directly terminate the child process via SIGUSR1
         * without handling it */
        if (exitcode == SERVER_CHILD_NOERROR_RETVAL) {
            bysignal = SIGUSR1;
            exitcode = 1;
        }

        if (pid == -1) {
            serverLog(LL_WARNING,"waitpid() returned an error: %s. "
                "child_type: %s, child_pid = %d",
                strerror(errno),
                strChildType(server.child_type),
                (int) server.child_pid);
        } else if (pid == server.child_pid) {
            if (server.child_type == CHILD_TYPE_RDB) {
                backgroundSaveDoneHandler(exitcode, bysignal);
            } else if (server.child_type == CHILD_TYPE_AOF) {
                backgroundRewriteDoneHandler(exitcode, bysignal);
            } else if (server.child_type == CHILD_TYPE_MODULE) {
                ModuleForkDoneHandler(exitcode, bysignal);
            } else {
                serverPanic("Unknown child type %d for child pid %d", server.child_type, server.child_pid);
                exit(1);
            }
            if (!bysignal && exitcode == 0) receiveChildInfo();
            resetChildState();
        } else {
            if (!ldbRemoveChild(pid)) {
                serverLog(LL_WARNING,
                          "Warning, detected child with unmatched pid: %ld",
                          (long) pid);
            }
        }

        /* start any pending forks immediately. */
        replicationStartPendingFork();
    }
}

/* Called from serverCron and cronUpdateMemoryStats to update cached memory metrics. */
void cronUpdateMemoryStats(void) {
    /* Record the max memory used since the server was started. */
    if (zmalloc_used_memory() > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used_memory();

    run_with_period(100) {
        /* Sample the RSS and other metrics here since this is a relatively slow call.
         * We must sample the zmalloc_used at the same time we take the rss, otherwise
         * the frag ratio calculate may be off (ratio of two samples at different times) */
        server.cron_malloc_stats.process_rss = zmalloc_get_rss();
        server.cron_malloc_stats.zmalloc_used = zmalloc_used_memory();
        /* Sampling the allocator info can be slow too.
         * The fragmentation ratio it'll show is potentially more accurate
         * it excludes other RSS pages such as: shared libraries, LUA and other non-zmalloc
         * allocations, and allocator reserved pages that can be pursed (all not actual frag) */
        zmalloc_get_allocator_info(1,
                                   &server.cron_malloc_stats.allocator_allocated,
                                   &server.cron_malloc_stats.allocator_active,
                                   &server.cron_malloc_stats.allocator_resident,
                                   NULL,
                                   &server.cron_malloc_stats.allocator_muzzy,
                                   &server.cron_malloc_stats.allocator_frag_smallbins_bytes);
        if (server.lua_arena != UINT_MAX) {
            zmalloc_get_allocator_info_by_arena(server.lua_arena,
                                                0,
                                                &server.cron_malloc_stats.lua_allocator_allocated,
                                                &server.cron_malloc_stats.lua_allocator_active,
                                                &server.cron_malloc_stats.lua_allocator_resident,
                                                &server.cron_malloc_stats.lua_allocator_frag_smallbins_bytes);
        }
        /* in case the allocator isn't providing these stats, fake them so that
         * fragmentation info still shows some (inaccurate metrics) */
        if (!server.cron_malloc_stats.allocator_resident)
            server.cron_malloc_stats.allocator_resident = server.cron_malloc_stats.process_rss;
        if (!server.cron_malloc_stats.allocator_active)
            server.cron_malloc_stats.allocator_active = server.cron_malloc_stats.allocator_resident;
        if (!server.cron_malloc_stats.allocator_allocated)
            server.cron_malloc_stats.allocator_allocated = server.cron_malloc_stats.zmalloc_used;
    }
}

/* This is our timer interrupt, called server.hz times per second.
 * Here is where we do a number of things that need to be done asynchronously.
 * For instance:
 *
 * - Active expired keys collection (it is also performed in a lazy way on
 *   lookup).
 * - Software watchdog.
 * - Update some statistic.
 * - Incremental rehashing of the DBs hash tables.
 * - Triggering BGSAVE / AOF rewrite, and handling of terminated children.
 * - Clients timeout of different kinds.
 * - Replication reconnection.
 * - Many more...
 *
 * Everything directly called here will be called server.hz times per second,
 * so in order to throttle execution of things we want to do less frequently
 * a macro is used: run_with_period(milliseconds) { .... }
 */

int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    int j;
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    /* Software watchdog: deliver the SIGALRM that will reach the signal
     * handler if we don't return here fast enough. */
    if (server.watchdog_period) watchdogScheduleSignal(server.watchdog_period);

    server.hz = server.config_hz;
    /* Adapt the server.hz value to the number of configured clients. If we have
     * many clients, we want to call serverCron() with an higher frequency. */
    if (server.dynamic_hz) {
        while (listLength(server.clients) / server.hz >
               MAX_CLIENTS_PER_CLOCK_TICK)
        {
            server.hz *= 2;
            if (server.hz > CONFIG_MAX_HZ) {
                server.hz = CONFIG_MAX_HZ;
                break;
            }
        }
    }

    /* for debug purposes: skip actual cron work if pause_cron is on */
    if (server.pause_cron) return 1000/server.hz;

    monotime cron_start = getMonotonicUs();

    run_with_period(100) {
        long long stat_net_input_bytes, stat_net_output_bytes;
        long long stat_net_repl_input_bytes, stat_net_repl_output_bytes;
        atomicGet(server.stat_net_input_bytes, stat_net_input_bytes);
        atomicGet(server.stat_net_output_bytes, stat_net_output_bytes);
        atomicGet(server.stat_net_repl_input_bytes, stat_net_repl_input_bytes);
        atomicGet(server.stat_net_repl_output_bytes, stat_net_repl_output_bytes);
        monotime current_time = getMonotonicUs();
        long long factor = 1000000;  // us
        trackInstantaneousMetric(STATS_METRIC_COMMAND, server.stat_numcommands, current_time, factor);
        trackInstantaneousMetric(STATS_METRIC_NET_INPUT, stat_net_input_bytes + stat_net_repl_input_bytes,
                                 current_time, factor);
        trackInstantaneousMetric(STATS_METRIC_NET_OUTPUT, stat_net_output_bytes + stat_net_repl_output_bytes,
                                 current_time, factor);
        trackInstantaneousMetric(STATS_METRIC_NET_INPUT_REPLICATION, stat_net_repl_input_bytes, current_time,
                                 factor);
        trackInstantaneousMetric(STATS_METRIC_NET_OUTPUT_REPLICATION, stat_net_repl_output_bytes,
                                 current_time, factor);
        trackInstantaneousMetric(STATS_METRIC_EL_CYCLE, server.duration_stats[EL_DURATION_TYPE_EL].cnt,
                                 current_time, factor);
        trackInstantaneousMetric(STATS_METRIC_EL_DURATION, server.duration_stats[EL_DURATION_TYPE_EL].sum,
                                 server.duration_stats[EL_DURATION_TYPE_EL].cnt, 1);
    }

    /* We have just LRU_BITS bits per object for LRU information.
     * So we use an (eventually wrapping) LRU clock.
     *
     * Note that even if the counter wraps it's not a big problem,
     * everything will still work but some object will appear younger
     * to Redis. However for this to happen a given object should never be
     * touched for all the time needed to the counter to wrap, which is
     * not likely.
     *
     * Note that you can change the resolution altering the
     * LRU_CLOCK_RESOLUTION define. */
    server.lruclock = getLRUClock();

    cronUpdateMemoryStats();

    /* We received a SIGTERM or SIGINT, shutting down here in a safe way, as it is
     * not ok doing so inside the signal handler. */
    if (server.shutdown_asap && !isShutdownInitiated()) {
        int shutdownFlags = SHUTDOWN_NOFLAGS;
        if (server.last_sig_received == SIGINT && server.shutdown_on_sigint)
            shutdownFlags = server.shutdown_on_sigint;
        else if (server.last_sig_received == SIGTERM && server.shutdown_on_sigterm)
            shutdownFlags = server.shutdown_on_sigterm;

        if (prepareForShutdown(shutdownFlags) == C_OK) exit(0);
    } else if (isShutdownInitiated()) {
        if (server.mstime >= server.shutdown_mstime || isReadyToShutdown()) {
            if (finishShutdown() == C_OK) exit(0);
            /* Shutdown failed. Continue running. An error has been logged. */
        }
    }

    /* Show some info about non-empty databases */
    if (server.verbosity <= LL_VERBOSE) {
        run_with_period(5000) {
            for (j = 0; j < server.dbnum; j++) {
                long long size, used, vkeys;

                size = kvstoreBuckets(server.db[j].keys);
                used = kvstoreSize(server.db[j].keys);
                vkeys = kvstoreSize(server.db[j].expires);
                if (used || vkeys) {
                    serverLog(LL_VERBOSE,"DB %d: %lld keys (%lld volatile) in %lld slots HT.",j,used,vkeys,size);
                }
            }
        }
    }

    /* Show information about connected clients */
    if (!server.sentinel_mode) {
        run_with_period(5000) {
            serverLog(LL_DEBUG,
                "%lu clients connected (%lu replicas), %zu bytes in use",
                listLength(server.clients)-listLength(server.slaves),
                listLength(server.slaves),
                zmalloc_used_memory());
        }
    }

    /* We need to do a few operations on clients asynchronously. */
    clientsCron();

    /* Handle background operations on Redis databases. */
    databasesCron();

    /* Start a scheduled AOF rewrite if this was requested by the user while
     * a BGSAVE was in progress. */
    if (!hasActiveChildProcess() &&
        server.aof_rewrite_scheduled &&
        !aofRewriteLimited())
    {
        rewriteAppendOnlyFileBackground();
    }

    /* Check if a background saving or AOF rewrite in progress terminated. */
    if (hasActiveChildProcess() || ldbPendingChildren())
    {
        run_with_period(1000) receiveChildInfo();
        checkChildrenDone();
    } else {
        /* If there is not a background saving/rewrite in progress check if
         * we have to save/rewrite now. */
        for (j = 0; j < server.saveparamslen; j++) {
            struct saveparam *sp = server.saveparams+j;

            /* Save if we reached the given amount of changes,
             * the given amount of seconds, and if the latest bgsave was
             * successful or if, in case of an error, at least
             * CONFIG_BGSAVE_RETRY_DELAY seconds already elapsed. */
            if (server.dirty >= sp->changes &&
                server.unixtime-server.lastsave > sp->seconds &&
                (server.unixtime-server.lastbgsave_try >
                 CONFIG_BGSAVE_RETRY_DELAY ||
                 server.lastbgsave_status == C_OK))
            {
                serverLog(LL_NOTICE,"%d changes in %d seconds. Saving...",
                    sp->changes, (int)sp->seconds);
                rdbSaveInfo rsi, *rsiptr;
                rsiptr = rdbPopulateSaveInfo(&rsi);
                rdbSaveBackground(SLAVE_REQ_NONE,server.rdb_filename,rsiptr,RDBFLAGS_NONE);
                break;
            }
        }

        /* Trigger an AOF rewrite if needed. */
        if (server.aof_state == AOF_ON &&
            !hasActiveChildProcess() &&
            server.aof_rewrite_perc &&
            server.aof_current_size > server.aof_rewrite_min_size)
        {
            long long base = server.aof_rewrite_base_size ?
                server.aof_rewrite_base_size : 1;
            long long growth = (server.aof_current_size*100/base) - 100;
            if (growth >= server.aof_rewrite_perc && !aofRewriteLimited()) {
                serverLog(LL_NOTICE,"Starting automatic rewriting of AOF on %lld%% growth",growth);
                rewriteAppendOnlyFileBackground();
            }
        }
    }
    /* Just for the sake of defensive programming, to avoid forgetting to
     * call this function when needed. */
    updateDictResizePolicy();

    /* AOF postponed flush: Try at every cron cycle if the slow fsync
     * completed. */
    if ((server.aof_state == AOF_ON || server.aof_state == AOF_WAIT_REWRITE) &&
        server.aof_flush_postponed_start)
    {
        flushAppendOnlyFile(0);
    }

    /* AOF write errors: in this case we have a buffer to flush as well and
     * clear the AOF error in case of success to make the DB writable again,
     * however to try every second is enough in case of 'hz' is set to
     * a higher frequency. */
    run_with_period(1000) {
        if ((server.aof_state == AOF_ON || server.aof_state == AOF_WAIT_REWRITE) &&
            server.aof_last_write_status == C_ERR) 
            {
                flushAppendOnlyFile(0);
            }
    }

    /* Clear the paused actions state if needed. */
    updatePausedActions();

    /* Replication cron function -- used to reconnect to master,
     * detect transfer failures, start background RDB transfers and so forth. 
     * 
     * If Redis is trying to failover then run the replication cron faster so
     * progress on the handshake happens more quickly. */
    if (server.failover_state != NO_FAILOVER) {
        run_with_period(100) replicationCron();
    } else {
        run_with_period(1000) replicationCron();
    }

    /* Run the Redis Cluster cron. */
    run_with_period(100) {
        if (server.cluster_enabled) clusterCron();
    }

    /* Run the Sentinel timer if we are in sentinel mode. */
    if (server.sentinel_mode) sentinelTimer();

    /* Cleanup expired MIGRATE cached sockets. */
    run_with_period(1000) {
        migrateCloseTimedoutSockets();
    }

    /* Stop the I/O threads if we don't have enough pending work. */
    stopThreadedIOIfNeeded();

    /* Resize tracking keys table if needed. This is also done at every
     * command execution, but we want to be sure that if the last command
     * executed changes the value via CONFIG SET, the server will perform
     * the operation even if completely idle. */
    if (server.tracking_clients) trackingLimitUsedSlots();

    /* Start a scheduled BGSAVE if the corresponding flag is set. This is
     * useful when we are forced to postpone a BGSAVE because an AOF
     * rewrite is in progress.
     *
     * Note: this code must be after the replicationCron() call above so
     * make sure when refactoring this file to keep this order. This is useful
     * because we want to give priority to RDB savings for replication. */
    if (!hasActiveChildProcess() &&
        server.rdb_bgsave_scheduled &&
        (server.unixtime-server.lastbgsave_try > CONFIG_BGSAVE_RETRY_DELAY ||
         server.lastbgsave_status == C_OK))
    {
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        if (rdbSaveBackground(SLAVE_REQ_NONE,server.rdb_filename,rsiptr,RDBFLAGS_NONE) == C_OK)
            server.rdb_bgsave_scheduled = 0;
    }

    run_with_period(100) {
        if (moduleCount()) modulesCron();
    }

    /* Fire the cron loop modules event. */
    RedisModuleCronLoopV1 ei = {REDISMODULE_CRON_LOOP_VERSION,server.hz};
    moduleFireServerEvent(REDISMODULE_EVENT_CRON_LOOP,
                          0,
                          &ei);

    server.cronloops++;

    server.el_cron_duration = getMonotonicUs() - cron_start;

    return 1000/server.hz;
}


void blockingOperationStarts(void) {
    if(!server.blocking_op_nesting++){
        updateCachedTime(0);
        server.blocked_last_cron = server.mstime;
    }
}

void blockingOperationEnds(void) {
    if(!(--server.blocking_op_nesting)){
        server.blocked_last_cron = 0;
    }
}

/* This function fills in the role of serverCron during RDB or AOF loading, and
 * also during blocked scripts.
 * It attempts to do its duties at a similar rate as the configured server.hz,
 * and updates cronloops variable so that similarly to serverCron, the
 * run_with_period can be used. */
void whileBlockedCron(void) {
    /* Here we may want to perform some cron jobs (normally done server.hz times
     * per second). */

    /* Since this function depends on a call to blockingOperationStarts, let's
     * make sure it was done. */
    serverAssert(server.blocked_last_cron);

    /* In case we were called too soon, leave right away. This way one time
     * jobs after the loop below don't need an if. and we don't bother to start
     * latency monitor if this function is called too often. */
    if (server.blocked_last_cron >= server.mstime)
        return;

    mstime_t latency;
    latencyStartMonitor(latency);

    /* In some cases we may be called with big intervals, so we may need to do
     * extra work here. This is because some of the functions in serverCron rely
     * on the fact that it is performed every 10 ms or so. For instance, if
     * activeDefragCycle needs to utilize 25% cpu, it will utilize 2.5ms, so we
     * need to call it multiple times. */
    long hz_ms = 1000/server.hz;
    while (server.blocked_last_cron < server.mstime) {

        /* Defrag keys gradually. */
        activeDefragCycle();

        server.blocked_last_cron += hz_ms;

        /* Increment cronloop so that run_with_period works. */
        server.cronloops++;
    }

    /* Other cron jobs do not need to be done in a loop. No need to check
     * server.blocked_last_cron since we have an early exit at the top. */

    /* Update memory stats during loading (excluding blocked scripts) */
    if (server.loading) cronUpdateMemoryStats();

    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("while-blocked-cron",latency);

    /* We received a SIGTERM during loading, shutting down here in a safe way,
     * as it isn't ok doing so inside the signal handler. */
    if (server.shutdown_asap && server.loading) {
        if (prepareForShutdown(SHUTDOWN_NOSAVE) == C_OK) exit(0);
        serverLog(LL_WARNING,"SIGTERM received but errors trying to shut down the server, check the logs for more information");
        server.shutdown_asap = 0;
        server.last_sig_received = 0;
    }
}

static void sendGetackToReplicas(void) {
    robj *argv[3];
    argv[0] = shared.replconf;
    argv[1] = shared.getack;
    argv[2] = shared.special_asterick; /* Not used argument. */
    replicationFeedSlaves(server.slaves, -1, argv, 3);
}

extern int ProcessingEventsWhileBlocked;

/* This function gets called every time Redis is entering the
 * main loop of the event driven library, that is, before to sleep
 * for ready file descriptors.
 *
 * Note: This function is (currently) called from two functions:
 * 1. aeMain - The main server loop
 * 2. processEventsWhileBlocked - Process clients during RDB/AOF load
 *
 * If it was called from processEventsWhileBlocked we don't want
 * to perform all actions (For example, we don't want to expire
 * keys), but we do need to perform some actions.
 *
 * The most important is freeClientsInAsyncFreeQueue but we also
 * call some other low-risk functions. */
void beforeSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);

    size_t zmalloc_used = zmalloc_used_memory();
    if (zmalloc_used > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used;

    /* Just call a subset of vital functions in case we are re-entering
     * the event loop from processEventsWhileBlocked(). Note that in this
     * case we keep track of the number of events we are processing, since
     * processEventsWhileBlocked() wants to stop ASAP if there are no longer
     * events to handle. */
    if (ProcessingEventsWhileBlocked) {
        uint64_t processed = 0;
        processed += handleClientsWithPendingReadsUsingThreads();
        processed += connTypeProcessPendingData();
        if (server.aof_state == AOF_ON || server.aof_state == AOF_WAIT_REWRITE)
            flushAppendOnlyFile(0);
        processed += handleClientsWithPendingWrites();
        processed += freeClientsInAsyncFreeQueue();
        server.events_processed_while_blocked += processed;
        return;
    }

    /* We should handle pending reads clients ASAP after event loop. */
    handleClientsWithPendingReadsUsingThreads();

    /* Handle pending data(typical TLS). (must be done before flushAppendOnlyFile) */
    connTypeProcessPendingData();

    /* If any connection type(typical TLS) still has pending unread data don't sleep at all. */
    int dont_sleep = connTypeHasPendingData();

    /* Call the Redis Cluster before sleep function. Note that this function
     * may change the state of Redis Cluster (from ok to fail or vice versa),
     * so it's a good idea to call it before serving the unblocked clients
     * later in this function, must be done before blockedBeforeSleep. */
    if (server.cluster_enabled) clusterBeforeSleep();

    /* Handle blocked clients.
     * must be done before flushAppendOnlyFile, in case of appendfsync=always,
     * since the unblocked clients may write data. */
    blockedBeforeSleep();

    /* Record cron time in beforeSleep, which is the sum of active-expire, active-defrag and all other
     * tasks done by cron and beforeSleep, but excluding read, write and AOF, that are counted by other
     * sets of metrics. */
    monotime cron_start_time_before_aof = getMonotonicUs();

    /* Run a fast expire cycle (the called function will return
     * ASAP if a fast cycle is not needed). */
    if (server.active_expire_enabled && iAmMaster())
        activeExpireCycle(ACTIVE_EXPIRE_CYCLE_FAST);

    if (moduleCount()) {
        moduleFireServerEvent(REDISMODULE_EVENT_EVENTLOOP,
                              REDISMODULE_SUBEVENT_EVENTLOOP_BEFORE_SLEEP,
                              NULL);
    }

    /* Send all the slaves an ACK request if at least one client blocked
     * during the previous event loop iteration. Note that we do this after
     * processUnblockedClients(), so if there are multiple pipelined WAITs
     * and the just unblocked WAIT gets blocked again, we don't have to wait
     * a server cron cycle in absence of other event loop events. See #6623.
     * 
     * We also don't send the ACKs while clients are paused, since it can
     * increment the replication backlog, they'll be sent after the pause
     * if we are still the master. */
    if (server.get_ack_from_slaves && !isPausedActionsWithUpdate(PAUSE_ACTION_REPLICA)) {
        sendGetackToReplicas();
        server.get_ack_from_slaves = 0;
    }

    /* We may have received updates from clients about their current offset. NOTE:
     * this can't be done where the ACK is received since failover will disconnect 
     * our clients. */
    updateFailoverStatus();

    /* Since we rely on current_client to send scheduled invalidation messages
     * we have to flush them after each command, so when we get here, the list
     * must be empty. */
    serverAssert(listLength(server.tracking_pending_keys) == 0);
    serverAssert(listLength(server.pending_push_messages) == 0);

    /* Send the invalidation messages to clients participating to the
     * client side caching protocol in broadcasting (BCAST) mode. */
    trackingBroadcastInvalidationMessages();

    /* Record time consumption of AOF writing. */
    monotime aof_start_time = getMonotonicUs();
    /* Record cron time in beforeSleep. This does not include the time consumed by AOF writing and IO writing below. */
    monotime duration_before_aof = aof_start_time - cron_start_time_before_aof;
    /* Record the fsync'd offset before flushAppendOnly */
    long long prev_fsynced_reploff = server.fsynced_reploff;

    /* Write the AOF buffer on disk,
     * must be done before handleClientsWithPendingWritesUsingThreads,
     * in case of appendfsync=always. */
    if (server.aof_state == AOF_ON || server.aof_state == AOF_WAIT_REWRITE)
        flushAppendOnlyFile(0);

    /* Record time consumption of AOF writing. */
    durationAddSample(EL_DURATION_TYPE_AOF, getMonotonicUs() - aof_start_time);

    /* Update the fsynced replica offset.
     * If an initial rewrite is in progress then not all data is guaranteed to have actually been
     * persisted to disk yet, so we cannot update the field. We will wait for the rewrite to complete. */
    if (server.aof_state == AOF_ON && server.fsynced_reploff != -1) {
        long long fsynced_reploff_pending;
        atomicGet(server.fsynced_reploff_pending, fsynced_reploff_pending);
        server.fsynced_reploff = fsynced_reploff_pending;

        /* If we have blocked [WAIT]AOF clients, and fsynced_reploff changed, we want to try to
         * wake them up ASAP. */
        if (listLength(server.clients_waiting_acks) && prev_fsynced_reploff != server.fsynced_reploff)
            dont_sleep = 1;
    }

    /* Handle writes with pending output buffers. */
    handleClientsWithPendingWritesUsingThreads();

    /* Record cron time in beforeSleep. This does not include the time consumed by AOF writing and IO writing above. */
    monotime cron_start_time_after_write = getMonotonicUs();

    /* Close clients that need to be closed asynchronous */
    freeClientsInAsyncFreeQueue();

    /* Incrementally trim replication backlog, 10 times the normal speed is
     * to free replication backlog as much as possible. */
    if (server.repl_backlog)
        incrementalTrimReplicationBacklog(10*REPL_BACKLOG_TRIM_BLOCKS_PER_CALL);

    /* Disconnect some clients if they are consuming too much memory. */
    evictClients();

    /* Record cron time in beforeSleep. */
    monotime duration_after_write = getMonotonicUs() - cron_start_time_after_write;

    /* Record eventloop latency. */
    if (server.el_start > 0) {
        monotime el_duration = getMonotonicUs() - server.el_start;
        durationAddSample(EL_DURATION_TYPE_EL, el_duration);
    }
    server.el_cron_duration += duration_before_aof + duration_after_write;
    durationAddSample(EL_DURATION_TYPE_CRON, server.el_cron_duration);
    server.el_cron_duration = 0;
    /* Record max command count per cycle. */
    if (server.stat_numcommands > server.el_cmd_cnt_start) {
        long long el_command_cnt = server.stat_numcommands - server.el_cmd_cnt_start;
        if (el_command_cnt > server.el_cmd_cnt_max) {
            server.el_cmd_cnt_max = el_command_cnt;
        }
    }

    /* Don't sleep at all before the next beforeSleep() if needed (e.g. a
     * connection has pending data) */
    aeSetDontWait(server.el, dont_sleep);

    /* Before we are going to sleep, let the threads access the dataset by
     * releasing the GIL. Redis main thread will not touch anything at this
     * time. */
    if (moduleCount()) moduleReleaseGIL();
    /********************* WARNING ********************
     * Do NOT add anything below moduleReleaseGIL !!! *
     ***************************** ********************/
}

/* This function is called immediately after the event loop multiplexing
 * API returned, and the control is going to soon return to Redis by invoking
 * the different events callbacks. */
void afterSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);
    /********************* WARNING ********************
     * Do NOT add anything above moduleAcquireGIL !!! *
     ***************************** ********************/
    if (!ProcessingEventsWhileBlocked) {
        /* Acquire the modules GIL so that their threads won't touch anything. */
        if (moduleCount()) {
            mstime_t latency;
            latencyStartMonitor(latency);

            atomicSet(server.module_gil_acquring, 1);
            moduleAcquireGIL();
            atomicSet(server.module_gil_acquring, 0);
            moduleFireServerEvent(REDISMODULE_EVENT_EVENTLOOP,
                                  REDISMODULE_SUBEVENT_EVENTLOOP_AFTER_SLEEP,
                                  NULL);
            latencyEndMonitor(latency);
            latencyAddSampleIfNeeded("module-acquire-GIL",latency);
        }
        /* Set the eventloop start time. */
        server.el_start = getMonotonicUs();
        /* Set the eventloop command count at start. */
        server.el_cmd_cnt_start = server.stat_numcommands;
    }

    /* Update the time cache. */
    updateCachedTime(1);

    /* Update command time snapshot in case it'll be required without a command
     * e.g. somehow used by module timers. Don't update it while yielding to a
     * blocked command, call() will handle that and restore the original time. */
    if (!ProcessingEventsWhileBlocked) {
        server.cmd_time_snapshot = server.mstime;
    }
}

/* =========================== Server initialization ======================== */

void createSharedObjects(void) {
    int j;

    /* Shared command responses */
    shared.ok = createObject(OBJ_STRING,sdsnew("+OK\r\n"));
    shared.emptybulk = createObject(OBJ_STRING,sdsnew("$0\r\n\r\n"));
    shared.czero = createObject(OBJ_STRING,sdsnew(":0\r\n"));
    shared.cone = createObject(OBJ_STRING,sdsnew(":1\r\n"));
    shared.emptyarray = createObject(OBJ_STRING,sdsnew("*0\r\n"));
    shared.pong = createObject(OBJ_STRING,sdsnew("+PONG\r\n"));
    shared.queued = createObject(OBJ_STRING,sdsnew("+QUEUED\r\n"));
    shared.emptyscan = createObject(OBJ_STRING,sdsnew("*2\r\n$1\r\n0\r\n*0\r\n"));
    shared.space = createObject(OBJ_STRING,sdsnew(" "));
    shared.plus = createObject(OBJ_STRING,sdsnew("+"));

    /* Shared command error responses */
    shared.wrongtypeerr = createObject(OBJ_STRING,sdsnew(
        "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"));
    shared.err = createObject(OBJ_STRING,sdsnew("-ERR\r\n"));
    shared.nokeyerr = createObject(OBJ_STRING,sdsnew(
        "-ERR no such key\r\n"));
    shared.syntaxerr = createObject(OBJ_STRING,sdsnew(
        "-ERR syntax error\r\n"));
    shared.sameobjecterr = createObject(OBJ_STRING,sdsnew(
        "-ERR source and destination objects are the same\r\n"));
    shared.outofrangeerr = createObject(OBJ_STRING,sdsnew(
        "-ERR index out of range\r\n"));
    shared.noscripterr = createObject(OBJ_STRING,sdsnew(
        "-NOSCRIPT No matching script. Please use EVAL.\r\n"));
    shared.loadingerr = createObject(OBJ_STRING,sdsnew(
        "-LOADING Redis is loading the dataset in memory\r\n"));
    shared.slowevalerr = createObject(OBJ_STRING,sdsnew(
        "-BUSY Redis is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.\r\n"));
    shared.slowscripterr = createObject(OBJ_STRING,sdsnew(
        "-BUSY Redis is busy running a script. You can only call FUNCTION KILL or SHUTDOWN NOSAVE.\r\n"));
    shared.slowmoduleerr = createObject(OBJ_STRING,sdsnew(
        "-BUSY Redis is busy running a module command.\r\n"));
    shared.masterdownerr = createObject(OBJ_STRING,sdsnew(
        "-MASTERDOWN Link with MASTER is down and replica-serve-stale-data is set to 'no'.\r\n"));
    shared.bgsaveerr = createObject(OBJ_STRING,sdsnew(
        "-MISCONF Redis is configured to save RDB snapshots, but it's currently unable to persist to disk. Commands that may modify the data set are disabled, because this instance is configured to report errors during writes if RDB snapshotting fails (stop-writes-on-bgsave-error option). Please check the Redis logs for details about the RDB error.\r\n"));
    shared.roslaveerr = createObject(OBJ_STRING,sdsnew(
        "-READONLY You can't write against a read only replica.\r\n"));
    shared.noautherr = createObject(OBJ_STRING,sdsnew(
        "-NOAUTH Authentication required.\r\n"));
    shared.oomerr = createObject(OBJ_STRING,sdsnew(
        "-OOM command not allowed when used memory > 'maxmemory'.\r\n"));
    shared.execaborterr = createObject(OBJ_STRING,sdsnew(
        "-EXECABORT Transaction discarded because of previous errors.\r\n"));
    shared.noreplicaserr = createObject(OBJ_STRING,sdsnew(
        "-NOREPLICAS Not enough good replicas to write.\r\n"));
    shared.busykeyerr = createObject(OBJ_STRING,sdsnew(
        "-BUSYKEY Target key name already exists.\r\n"));

    /* The shared NULL depends on the protocol version. */
    shared.null[0] = NULL;
    shared.null[1] = NULL;
    shared.null[2] = createObject(OBJ_STRING,sdsnew("$-1\r\n"));
    shared.null[3] = createObject(OBJ_STRING,sdsnew("_\r\n"));

    shared.nullarray[0] = NULL;
    shared.nullarray[1] = NULL;
    shared.nullarray[2] = createObject(OBJ_STRING,sdsnew("*-1\r\n"));
    shared.nullarray[3] = createObject(OBJ_STRING,sdsnew("_\r\n"));

    shared.emptymap[0] = NULL;
    shared.emptymap[1] = NULL;
    shared.emptymap[2] = createObject(OBJ_STRING,sdsnew("*0\r\n"));
    shared.emptymap[3] = createObject(OBJ_STRING,sdsnew("%0\r\n"));

    shared.emptyset[0] = NULL;
    shared.emptyset[1] = NULL;
    shared.emptyset[2] = createObject(OBJ_STRING,sdsnew("*0\r\n"));
    shared.emptyset[3] = createObject(OBJ_STRING,sdsnew("~0\r\n"));

    for (j = 0; j < PROTO_SHARED_SELECT_CMDS; j++) {
        char dictid_str[64];
        int dictid_len;

        dictid_len = ll2string(dictid_str,sizeof(dictid_str),j);
        shared.select[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),
                "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
                dictid_len, dictid_str));
    }
    shared.messagebulk = createStringObject("$7\r\nmessage\r\n",13);
    shared.pmessagebulk = createStringObject("$8\r\npmessage\r\n",14);
    shared.subscribebulk = createStringObject("$9\r\nsubscribe\r\n",15);
    shared.unsubscribebulk = createStringObject("$11\r\nunsubscribe\r\n",18);
    shared.ssubscribebulk = createStringObject("$10\r\nssubscribe\r\n", 17);
    shared.sunsubscribebulk = createStringObject("$12\r\nsunsubscribe\r\n", 19);
    shared.smessagebulk = createStringObject("$8\r\nsmessage\r\n", 14);
    shared.psubscribebulk = createStringObject("$10\r\npsubscribe\r\n",17);
    shared.punsubscribebulk = createStringObject("$12\r\npunsubscribe\r\n",19);

    /* Shared command names */
    shared.del = createStringObject("DEL",3);
    shared.unlink = createStringObject("UNLINK",6);
    shared.rpop = createStringObject("RPOP",4);
    shared.lpop = createStringObject("LPOP",4);
    shared.lpush = createStringObject("LPUSH",5);
    shared.rpoplpush = createStringObject("RPOPLPUSH",9);
    shared.lmove = createStringObject("LMOVE",5);
    shared.blmove = createStringObject("BLMOVE",6);
    shared.zpopmin = createStringObject("ZPOPMIN",7);
    shared.zpopmax = createStringObject("ZPOPMAX",7);
    shared.multi = createStringObject("MULTI",5);
    shared.exec = createStringObject("EXEC",4);
    shared.hset = createStringObject("HSET",4);
    shared.srem = createStringObject("SREM",4);
    shared.xgroup = createStringObject("XGROUP",6);
    shared.xclaim = createStringObject("XCLAIM",6);
    shared.script = createStringObject("SCRIPT",6);
    shared.replconf = createStringObject("REPLCONF",8);
    shared.pexpireat = createStringObject("PEXPIREAT",9);
    shared.pexpire = createStringObject("PEXPIRE",7);
    shared.persist = createStringObject("PERSIST",7);
    shared.set = createStringObject("SET",3);
    shared.eval = createStringObject("EVAL",4);
    shared.hpexpireat = createStringObject("HPEXPIREAT",10);
    shared.hdel = createStringObject("HDEL",4);

    /* Shared command argument */
    shared.left = createStringObject("left",4);
    shared.right = createStringObject("right",5);
    shared.pxat = createStringObject("PXAT", 4);
    shared.time = createStringObject("TIME",4);
    shared.retrycount = createStringObject("RETRYCOUNT",10);
    shared.force = createStringObject("FORCE",5);
    shared.justid = createStringObject("JUSTID",6);
    shared.entriesread = createStringObject("ENTRIESREAD",11);
    shared.lastid = createStringObject("LASTID",6);
    shared.default_username = createStringObject("default",7);
    shared.ping = createStringObject("ping",4);
    shared.setid = createStringObject("SETID",5);
    shared.keepttl = createStringObject("KEEPTTL",7);
    shared.absttl = createStringObject("ABSTTL",6);
    shared.load = createStringObject("LOAD",4);
    shared.createconsumer = createStringObject("CREATECONSUMER",14);
    shared.getack = createStringObject("GETACK",6);
    shared.special_asterick = createStringObject("*",1);
    shared.special_equals = createStringObject("=",1);
    shared.redacted = makeObjectShared(createStringObject("(redacted)",10));

    for (j = 0; j < OBJ_SHARED_INTEGERS; j++) {
        shared.integers[j] =
            makeObjectShared(createObject(OBJ_STRING,(void*)(long)j));
        initObjectLRUOrLFU(shared.integers[j]);
        shared.integers[j]->encoding = OBJ_ENCODING_INT;
    }
    for (j = 0; j < OBJ_SHARED_BULKHDR_LEN; j++) {
        shared.mbulkhdr[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),"*%d\r\n",j));
        shared.bulkhdr[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),"$%d\r\n",j));
        shared.maphdr[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),"%%%d\r\n",j));
        shared.sethdr[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),"~%d\r\n",j));
    }
    /* The following two shared objects, minstring and maxstring, are not
     * actually used for their value but as a special object meaning
     * respectively the minimum possible string and the maximum possible
     * string in string comparisons for the ZRANGEBYLEX command. */
    shared.minstring = sdsnew("minstring");
    shared.maxstring = sdsnew("maxstring");
}

void initServerClientMemUsageBuckets(void) {
    if (server.client_mem_usage_buckets)
        return;
    server.client_mem_usage_buckets = zmalloc(sizeof(clientMemUsageBucket)*CLIENT_MEM_USAGE_BUCKETS);
    for (int j = 0; j < CLIENT_MEM_USAGE_BUCKETS; j++) {
        server.client_mem_usage_buckets[j].mem_usage_sum = 0;
        server.client_mem_usage_buckets[j].clients = listCreate();
    }
}

void freeServerClientMemUsageBuckets(void) {
    if (!server.client_mem_usage_buckets)
        return;
    for (int j = 0; j < CLIENT_MEM_USAGE_BUCKETS; j++)
        listRelease(server.client_mem_usage_buckets[j].clients);
    zfree(server.client_mem_usage_buckets);
    server.client_mem_usage_buckets = NULL;
}

void initServerConfig(void) {
    int j;
    char *default_bindaddr[CONFIG_DEFAULT_BINDADDR_COUNT] = CONFIG_DEFAULT_BINDADDR;

    initConfigValues();
    updateCachedTime(1);
    server.cmd_time_snapshot = server.mstime;
    getRandomHexChars(server.runid,CONFIG_RUN_ID_SIZE);
    server.runid[CONFIG_RUN_ID_SIZE] = '\0';
    changeReplicationId();
    clearReplicationId2();
    server.hz = CONFIG_DEFAULT_HZ; /* Initialize it ASAP, even if it may get
                                      updated later after loading the config.
                                      This value may be used before the server
                                      is initialized. */
    server.timezone = getTimeZone(); /* Initialized by tzset(). */
    server.configfile = NULL;
    server.executable = NULL;
    server.arch_bits = (sizeof(long) == 8) ? 64 : 32;
    server.bindaddr_count = CONFIG_DEFAULT_BINDADDR_COUNT;
    for (j = 0; j < CONFIG_DEFAULT_BINDADDR_COUNT; j++)
        server.bindaddr[j] = zstrdup(default_bindaddr[j]);
    memset(server.listeners, 0x00, sizeof(server.listeners));
    server.active_expire_enabled = 1;
    server.allow_access_expired = 0;
    server.skip_checksum_validation = 0;
    server.loading = 0;
    server.async_loading = 0;
    server.loading_rdb_used_mem = 0;
    server.aof_state = AOF_OFF;
    server.aof_rewrite_base_size = 0;
    server.aof_rewrite_scheduled = 0;
    server.aof_flush_sleep = 0;
    server.aof_last_fsync = time(NULL) * 1000;
    server.aof_cur_timestamp = 0;
    atomicSet(server.aof_bio_fsync_status,C_OK);
    server.aof_rewrite_time_last = -1;
    server.aof_rewrite_time_start = -1;
    server.aof_lastbgrewrite_status = C_OK;
    server.aof_delayed_fsync = 0;
    server.aof_fd = -1;
    server.aof_selected_db = -1; /* Make sure the first time will not match */
    server.aof_flush_postponed_start = 0;
    server.aof_last_incr_size = 0;
    server.aof_last_incr_fsync_offset = 0;
    server.active_defrag_running = 0;
    server.active_defrag_configuration_changed = 0;
    server.notify_keyspace_events = 0;
    server.blocked_clients = 0;
    memset(server.blocked_clients_by_type,0,
           sizeof(server.blocked_clients_by_type));
    server.shutdown_asap = 0;
    server.shutdown_flags = 0;
    server.shutdown_mstime = 0;
    server.cluster_module_flags = CLUSTER_MODULE_FLAG_NONE;
    server.migrate_cached_sockets = dictCreate(&migrateCacheDictType);
    server.next_client_id = 1; /* Client IDs, start from 1 .*/
    server.page_size = sysconf(_SC_PAGESIZE);
    server.pause_cron = 0;
    server.dict_resizing = 1;

    server.latency_tracking_info_percentiles_len = 3;
    server.latency_tracking_info_percentiles = zmalloc(sizeof(double)*(server.latency_tracking_info_percentiles_len));
    server.latency_tracking_info_percentiles[0] = 50.0;  /* p50 */
    server.latency_tracking_info_percentiles[1] = 99.0;  /* p99 */
    server.latency_tracking_info_percentiles[2] = 99.9;  /* p999 */

    server.lruclock = getLRUClock();
    resetServerSaveParams();

    appendServerSaveParams(60*60,1);  /* save after 1 hour and 1 change */
    appendServerSaveParams(300,100);  /* save after 5 minutes and 100 changes */
    appendServerSaveParams(60,10000); /* save after 1 minute and 10000 changes */

    /* Replication related */
    server.masterhost = NULL;
    server.masterport = 6379;
    server.master = NULL;
    server.cached_master = NULL;
    server.master_initial_offset = -1;
    server.repl_state = REPL_STATE_NONE;
    server.repl_transfer_tmpfile = NULL;
    server.repl_transfer_fd = -1;
    server.repl_transfer_s = NULL;
    server.repl_syncio_timeout = CONFIG_REPL_SYNCIO_TIMEOUT;
    server.repl_down_since = 0; /* Never connected, repl is down since EVER. */
    server.master_repl_offset = 0;
    server.fsynced_reploff_pending = 0;

    /* Replication partial resync backlog */
    server.repl_backlog = NULL;
    server.repl_no_slaves_since = time(NULL);

    /* Failover related */
    server.failover_end_time = 0;
    server.force_failover = 0;
    server.target_replica_host = NULL;
    server.target_replica_port = 0;
    server.failover_state = NO_FAILOVER;

    /* Client output buffer limits */
    for (j = 0; j < CLIENT_TYPE_OBUF_COUNT; j++)
        server.client_obuf_limits[j] = clientBufferLimitsDefaults[j];

    /* Linux OOM Score config */
    for (j = 0; j < CONFIG_OOM_COUNT; j++)
        server.oom_score_adj_values[j] = configOOMScoreAdjValuesDefaults[j];

    /* Double constants initialization */
    R_Zero = 0.0;
    R_PosInf = 1.0/R_Zero;
    R_NegInf = -1.0/R_Zero;
    R_Nan = R_Zero/R_Zero;

    /* Command table -- we initialize it here as it is part of the
     * initial configuration, since command names may be changed via
     * redis.conf using the rename-command directive. */
    server.commands = dictCreate(&commandTableDictType);
    server.orig_commands = dictCreate(&commandTableDictType);
    populateCommandTable();

    /* Debugging */
    server.watchdog_period = 0;
}

extern char **environ;

/* Restart the server, executing the same executable that started this
 * instance, with the same arguments and configuration file.
 *
 * The function is designed to directly call execve() so that the new
 * server instance will retain the PID of the previous one.
 *
 * The list of flags, that may be bitwise ORed together, alter the
 * behavior of this function:
 *
 * RESTART_SERVER_NONE              No flags.
 * RESTART_SERVER_GRACEFULLY        Do a proper shutdown before restarting.
 * RESTART_SERVER_CONFIG_REWRITE    Rewrite the config file before restarting.
 *
 * On success the function does not return, because the process turns into
 * a different process. On error C_ERR is returned. */
int restartServer(int flags, mstime_t delay) {
    int j;

    /* Check if we still have accesses to the executable that started this
     * server instance. */
    if (access(server.executable,X_OK) == -1) {
        serverLog(LL_WARNING,"Can't restart: this process has no "
                             "permissions to execute %s", server.executable);
        return C_ERR;
    }

    /* Config rewriting. */
    if (flags & RESTART_SERVER_CONFIG_REWRITE &&
        server.configfile &&
        rewriteConfig(server.configfile, 0) == -1)
    {
        serverLog(LL_WARNING,"Can't restart: configuration rewrite process "
                             "failed: %s", strerror(errno));
        return C_ERR;
    }

    /* Perform a proper shutdown. We don't wait for lagging replicas though. */
    if (flags & RESTART_SERVER_GRACEFULLY &&
        prepareForShutdown(SHUTDOWN_NOW) != C_OK)
    {
        serverLog(LL_WARNING,"Can't restart: error preparing for shutdown");
        return C_ERR;
    }

    /* Close all file descriptors, with the exception of stdin, stdout, stderr
     * which are useful if we restart a Redis server which is not daemonized. */
    for (j = 3; j < (int)server.maxclients + 1024; j++) {
        /* Test the descriptor validity before closing it, otherwise
         * Valgrind issues a warning on close(). */
        if (fcntl(j,F_GETFD) != -1) close(j);
    }

    /* Execute the server with the original command line. */
    if (delay) usleep(delay*1000);
    zfree(server.exec_argv[0]);
    server.exec_argv[0] = zstrdup(server.executable);
    execve(server.executable,server.exec_argv,environ);

    /* If an error occurred here, there is nothing we can do, but exit. */
    _exit(1);

    return C_ERR; /* Never reached. */
}

/* This function will configure the current process's oom_score_adj according
 * to user specified configuration. This is currently implemented on Linux
 * only.
 *
 * A process_class value of -1 implies OOM_CONFIG_MASTER or OOM_CONFIG_REPLICA,
 * depending on current role.
 */
int setOOMScoreAdj(int process_class) {
    if (process_class == -1)
        process_class = (server.masterhost ? CONFIG_OOM_REPLICA : CONFIG_OOM_MASTER);

    serverAssert(process_class >= 0 && process_class < CONFIG_OOM_COUNT);

#ifdef HAVE_PROC_OOM_SCORE_ADJ
    /* The following statics are used to indicate Redis has changed the process's oom score.
     * And to save the original score so we can restore it later if needed.
     * We need this so when we disabled oom-score-adj (also during configuration rollback
     * when another configuration parameter was invalid and causes a rollback after
     * applying a new oom-score) we can return to the oom-score value from before our
     * adjustments. */
    static int oom_score_adjusted_by_redis = 0;
    static int oom_score_adj_base = 0;

    int fd;
    int val;
    char buf[64];

    if (server.oom_score_adj != OOM_SCORE_ADJ_NO) {
        if (!oom_score_adjusted_by_redis) {
            oom_score_adjusted_by_redis = 1;
            /* Backup base value before enabling Redis control over oom score */
            fd = open("/proc/self/oom_score_adj", O_RDONLY);
            if (fd < 0 || read(fd, buf, sizeof(buf)) < 0) {
                serverLog(LL_WARNING, "Unable to read oom_score_adj: %s", strerror(errno));
                if (fd != -1) close(fd);
                return C_ERR;
            }
            oom_score_adj_base = atoi(buf);
            close(fd);
        }

        val = server.oom_score_adj_values[process_class];
        if (server.oom_score_adj == OOM_SCORE_RELATIVE)
            val += oom_score_adj_base;
        if (val > 1000) val = 1000;
        if (val < -1000) val = -1000;
    } else if (oom_score_adjusted_by_redis) {
        oom_score_adjusted_by_redis = 0;
        val = oom_score_adj_base;
    }
    else {
        return C_OK;
    }

    snprintf(buf, sizeof(buf) - 1, "%d\n", val);

    fd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (fd < 0 || write(fd, buf, strlen(buf)) < 0) {
        serverLog(LL_WARNING, "Unable to write oom_score_adj: %s", strerror(errno));
        if (fd != -1) close(fd);
        return C_ERR;
    }

    close(fd);
    return C_OK;
#else
    /* Unsupported */
    return C_ERR;
#endif
}

/* This function will try to raise the max number of open files accordingly to
 * the configured max number of clients. It also reserves a number of file
 * descriptors (CONFIG_MIN_RESERVED_FDS) for extra operations of
 * persistence, listening sockets, log files and so forth.
 *
 * If it will not be possible to set the limit accordingly to the configured
 * max number of clients, the function will do the reverse setting
 * server.maxclients to the value that we can actually handle. */
void adjustOpenFilesLimit(void) {
    rlim_t maxfiles = server.maxclients+CONFIG_MIN_RESERVED_FDS;
    struct rlimit limit;

    if (getrlimit(RLIMIT_NOFILE,&limit) == -1) {
        serverLog(LL_WARNING,"Unable to obtain the current NOFILE limit (%s), assuming 1024 and setting the max clients configuration accordingly.",
            strerror(errno));
        server.maxclients = 1024-CONFIG_MIN_RESERVED_FDS;
    } else {
        rlim_t oldlimit = limit.rlim_cur;

        /* Set the max number of files if the current limit is not enough
         * for our needs. */
        if (oldlimit < maxfiles) {
            rlim_t bestlimit;
            int setrlimit_error = 0;

            /* Try to set the file limit to match 'maxfiles' or at least
             * to the higher value supported less than maxfiles. */
            bestlimit = maxfiles;
            while(bestlimit > oldlimit) {
                rlim_t decr_step = 16;

                limit.rlim_cur = bestlimit;
                limit.rlim_max = bestlimit;
                if (setrlimit(RLIMIT_NOFILE,&limit) != -1) break;
                setrlimit_error = errno;

                /* We failed to set file limit to 'bestlimit'. Try with a
                 * smaller limit decrementing by a few FDs per iteration. */
                if (bestlimit < decr_step) {
                    bestlimit = oldlimit;
                    break;
                }
                bestlimit -= decr_step;
            }

            /* Assume that the limit we get initially is still valid if
             * our last try was even lower. */
            if (bestlimit < oldlimit) bestlimit = oldlimit;

            if (bestlimit < maxfiles) {
                unsigned int old_maxclients = server.maxclients;
                server.maxclients = bestlimit-CONFIG_MIN_RESERVED_FDS;
                /* maxclients is unsigned so may overflow: in order
                 * to check if maxclients is now logically less than 1
                 * we test indirectly via bestlimit. */
                if (bestlimit <= CONFIG_MIN_RESERVED_FDS) {
                    serverLog(LL_WARNING,"Your current 'ulimit -n' "
                        "of %llu is not enough for the server to start. "
                        "Please increase your open file limit to at least "
                        "%llu. Exiting.",
                        (unsigned long long) oldlimit,
                        (unsigned long long) maxfiles);
                    exit(1);
                }
                serverLog(LL_WARNING,"You requested maxclients of %d "
                    "requiring at least %llu max file descriptors.",
                    old_maxclients,
                    (unsigned long long) maxfiles);
                serverLog(LL_WARNING,"Server can't set maximum open files "
                    "to %llu because of OS error: %s.",
                    (unsigned long long) maxfiles, strerror(setrlimit_error));
                serverLog(LL_WARNING,"Current maximum open files is %llu. "
                    "maxclients has been reduced to %d to compensate for "
                    "low ulimit. "
                    "If you need higher maxclients increase 'ulimit -n'.",
                    (unsigned long long) bestlimit, server.maxclients);
            } else {
                serverLog(LL_NOTICE,"Increased maximum number of open files "
                    "to %llu (it was originally set to %llu).",
                    (unsigned long long) maxfiles,
                    (unsigned long long) oldlimit);
            }
        }
    }
}

/* Check that server.tcp_backlog can be actually enforced in Linux according
 * to the value of /proc/sys/net/core/somaxconn, or warn about it. */
void checkTcpBacklogSettings(void) {
#if defined(HAVE_PROC_SOMAXCONN)
    FILE *fp = fopen("/proc/sys/net/core/somaxconn","r");
    char buf[1024];
    if (!fp) return;
    if (fgets(buf,sizeof(buf),fp) != NULL) {
        int somaxconn = atoi(buf);
        if (somaxconn > 0 && somaxconn < server.tcp_backlog) {
            serverLog(LL_WARNING,"WARNING: The TCP backlog setting of %d cannot be enforced because /proc/sys/net/core/somaxconn is set to the lower value of %d.", server.tcp_backlog, somaxconn);
        }
    }
    fclose(fp);
#elif defined(HAVE_SYSCTL_KIPC_SOMAXCONN)
    int somaxconn, mib[3];
    size_t len = sizeof(int);

    mib[0] = CTL_KERN;
    mib[1] = KERN_IPC;
    mib[2] = KIPC_SOMAXCONN;

    if (sysctl(mib, 3, &somaxconn, &len, NULL, 0) == 0) {
        if (somaxconn > 0 && somaxconn < server.tcp_backlog) {
            serverLog(LL_WARNING,"WARNING: The TCP backlog setting of %d cannot be enforced because kern.ipc.somaxconn is set to the lower value of %d.", server.tcp_backlog, somaxconn);
        }
    }
#elif defined(HAVE_SYSCTL_KERN_SOMAXCONN)
    int somaxconn, mib[2];
    size_t len = sizeof(int);

    mib[0] = CTL_KERN;
    mib[1] = KERN_SOMAXCONN;

    if (sysctl(mib, 2, &somaxconn, &len, NULL, 0) == 0) {
        if (somaxconn > 0 && somaxconn < server.tcp_backlog) {
            serverLog(LL_WARNING,"WARNING: The TCP backlog setting of %d cannot be enforced because kern.somaxconn is set to the lower value of %d.", server.tcp_backlog, somaxconn);
        }
    }
#elif defined(SOMAXCONN)
    if (SOMAXCONN < server.tcp_backlog) {
        serverLog(LL_WARNING,"WARNING: The TCP backlog setting of %d cannot be enforced because SOMAXCONN is set to the lower value of %d.", server.tcp_backlog, SOMAXCONN);
    }
#endif
}

void closeListener(connListener *sfd) {
    int j;

    for (j = 0; j < sfd->count; j++) {
        if (sfd->fd[j] == -1) continue;

        aeDeleteFileEvent(server.el, sfd->fd[j], AE_READABLE);
        close(sfd->fd[j]);
    }

    sfd->count = 0;
}

/* Create an event handler for accepting new connections in TCP or TLS domain sockets.
 * This works atomically for all socket fds */
int createSocketAcceptHandler(connListener *sfd, aeFileProc *accept_handler) {
    int j;

    for (j = 0; j < sfd->count; j++) {
        if (aeCreateFileEvent(server.el, sfd->fd[j], AE_READABLE, accept_handler,sfd) == AE_ERR) {
            /* Rollback */
            for (j = j-1; j >= 0; j--) aeDeleteFileEvent(server.el, sfd->fd[j], AE_READABLE);
            return C_ERR;
        }
    }
    return C_OK;
}

/* Initialize a set of file descriptors to listen to the specified 'port'
 * binding the addresses specified in the Redis server configuration.
 *
 * The listening file descriptors are stored in the integer array 'fds'
 * and their number is set in '*count'. Actually @sfd should be 'listener',
 * for the historical reasons, let's keep 'sfd' here.
 *
 * The addresses to bind are specified in the global server.bindaddr array
 * and their number is server.bindaddr_count. If the server configuration
 * contains no specific addresses to bind, this function will try to
 * bind * (all addresses) for both the IPv4 and IPv6 protocols.
 *
 * On success the function returns C_OK.
 *
 * On error the function returns C_ERR. For the function to be on
 * error, at least one of the server.bindaddr addresses was
 * impossible to bind, or no bind addresses were specified in the server
 * configuration but the function is not able to bind * for at least
 * one of the IPv4 or IPv6 protocols. */
int listenToPort(connListener *sfd) {
    int j;
    int port = sfd->port;
    char **bindaddr = sfd->bindaddr;

    /* If we have no bind address, we don't listen on a TCP socket */
    if (sfd->bindaddr_count == 0) return C_OK;

    for (j = 0; j < sfd->bindaddr_count; j++) {
        char* addr = bindaddr[j];
        int optional = *addr == '-';
        if (optional) addr++;
        if (strchr(addr,':')) {
            /* Bind IPv6 address. */
            sfd->fd[sfd->count] = anetTcp6Server(server.neterr,port,addr,server.tcp_backlog);
        } else {
            /* Bind IPv4 address. */
            sfd->fd[sfd->count] = anetTcpServer(server.neterr,port,addr,server.tcp_backlog);
        }
        if (sfd->fd[sfd->count] == ANET_ERR) {
            int net_errno = errno;
            serverLog(LL_WARNING,
                "Warning: Could not create server TCP listening socket %s:%d: %s",
                addr, port, server.neterr);
            if (net_errno == EADDRNOTAVAIL && optional)
                continue;
            if (net_errno == ENOPROTOOPT     || net_errno == EPROTONOSUPPORT ||
                net_errno == ESOCKTNOSUPPORT || net_errno == EPFNOSUPPORT ||
                net_errno == EAFNOSUPPORT)
                continue;

            /* Rollback successful listens before exiting */
            closeListener(sfd);
            return C_ERR;
        }
        if (server.socket_mark_id > 0) anetSetSockMarkId(NULL, sfd->fd[sfd->count], server.socket_mark_id);
        anetNonBlock(NULL,sfd->fd[sfd->count]);
        anetCloexec(sfd->fd[sfd->count]);
        sfd->count++;
    }
    return C_OK;
}

/* Resets the stats that we expose via INFO or other means that we want
 * to reset via CONFIG RESETSTAT. The function is also used in order to
 * initialize these fields in initServer() at server startup. */
void resetServerStats(void) {
    int j;

    server.stat_numcommands = 0;
    server.stat_numconnections = 0;
    server.stat_expiredkeys = 0;
    server.stat_expired_subkeys = 0;
    server.stat_expired_stale_perc = 0;
    server.stat_expired_time_cap_reached_count = 0;
    server.stat_expire_cycle_time_used = 0;
    server.stat_evictedkeys = 0;
    server.stat_evictedclients = 0;
    server.stat_evictedscripts = 0;
    server.stat_total_eviction_exceeded_time = 0;
    server.stat_last_eviction_exceeded_time = 0;
    server.stat_keyspace_misses = 0;
    server.stat_keyspace_hits = 0;
    server.stat_active_defrag_hits = 0;
    server.stat_active_defrag_misses = 0;
    server.stat_active_defrag_key_hits = 0;
    server.stat_active_defrag_key_misses = 0;
    server.stat_active_defrag_scanned = 0;
    server.stat_total_active_defrag_time = 0;
    server.stat_last_active_defrag_time = 0;
    server.stat_fork_time = 0;
    server.stat_fork_rate = 0;
    server.stat_total_forks = 0;
    server.stat_rejected_conn = 0;
    server.stat_sync_full = 0;
    server.stat_sync_partial_ok = 0;
    server.stat_sync_partial_err = 0;
    server.stat_io_reads_processed = 0;
    atomicSet(server.stat_total_reads_processed, 0);
    server.stat_io_writes_processed = 0;
    atomicSet(server.stat_total_writes_processed, 0);
    atomicSet(server.stat_client_qbuf_limit_disconnections, 0);
    server.stat_client_outbuf_limit_disconnections = 0;
    for (j = 0; j < STATS_METRIC_COUNT; j++) {
        server.inst_metric[j].idx = 0;
        server.inst_metric[j].last_sample_base = 0;
        server.inst_metric[j].last_sample_value = 0;
        memset(server.inst_metric[j].samples,0,
            sizeof(server.inst_metric[j].samples));
    }
    server.stat_aof_rewrites = 0;
    server.stat_rdb_saves = 0;
    server.stat_aofrw_consecutive_failures = 0;
    atomicSet(server.stat_net_input_bytes, 0);
    atomicSet(server.stat_net_output_bytes, 0);
    atomicSet(server.stat_net_repl_input_bytes, 0);
    atomicSet(server.stat_net_repl_output_bytes, 0);
    server.stat_unexpected_error_replies = 0;
    server.stat_total_error_replies = 0;
    server.stat_dump_payload_sanitizations = 0;
    server.aof_delayed_fsync = 0;
    server.stat_reply_buffer_shrinks = 0;
    server.stat_reply_buffer_expands = 0;
    memset(server.duration_stats, 0, sizeof(durationStats) * EL_DURATION_TYPE_NUM);
    server.el_cmd_cnt_max = 0;
    lazyfreeResetStats();
}

/* Make the thread killable at any time, so that kill threads functions
 * can work reliably (default cancelability type is PTHREAD_CANCEL_DEFERRED).
 * Needed for pthread_cancel used by the fast memory test used by the crash report. */
void makeThreadKillable(void) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
}

void initServer(void) {
    int j;

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setupSignalHandlers();
    ThreadsManager_init();
    makeThreadKillable();

    if (server.syslog_enabled) {
        openlog(server.syslog_ident, LOG_PID | LOG_NDELAY | LOG_NOWAIT,
            server.syslog_facility);
    }

    /* Initialization after setting defaults from the config system. */
    server.aof_state = server.aof_enabled ? AOF_ON : AOF_OFF;
    server.fsynced_reploff = server.aof_enabled ? 0 : -1;
    server.hz = server.config_hz;
    server.pid = getpid();
    server.in_fork_child = CHILD_TYPE_NONE;
    server.main_thread_id = pthread_self();
    server.current_client = NULL;
    server.errors = raxNew();
    server.errors_enabled = 1;
    server.execution_nesting = 0;
    server.clients = listCreate();
    server.clients_index = raxNew();
    server.clients_to_close = listCreate();
    server.slaves = listCreate();
    server.monitors = listCreate();
    server.clients_pending_write = listCreate();
    server.clients_pending_read = listCreate();
    server.clients_timeout_table = raxNew();
    server.replication_allowed = 1;
    server.slaveseldb = -1; /* Force to emit the first SELECT command. */
    server.unblocked_clients = listCreate();
    server.ready_keys = listCreate();
    server.tracking_pending_keys = listCreate();
    server.pending_push_messages = listCreate();
    server.clients_waiting_acks = listCreate();
    server.get_ack_from_slaves = 0;
    server.paused_actions = 0;
    memset(server.client_pause_per_purpose, 0,
           sizeof(server.client_pause_per_purpose));
    server.postponed_clients = listCreate();
    server.events_processed_while_blocked = 0;
    server.system_memory_size = zmalloc_get_memory_size();
    server.blocked_last_cron = 0;
    server.blocking_op_nesting = 0;
    server.thp_enabled = 0;
    server.cluster_drop_packet_filter = -1;
    server.reply_buffer_peak_reset_time = REPLY_BUFFER_DEFAULT_PEAK_RESET_TIME;
    server.reply_buffer_resizing_enabled = 1;
    server.client_mem_usage_buckets = NULL;
    resetReplicationBuffer();

    /* Make sure the locale is set on startup based on the config file. */
    if (setlocale(LC_COLLATE,server.locale_collate) == NULL) {
        serverLog(LL_WARNING, "Failed to configure LOCALE for invalid locale name.");
        exit(1);
    }

    createSharedObjects();
    adjustOpenFilesLimit();
    const char *clk_msg = monotonicInit();
    serverLog(LL_NOTICE, "monotonic clock: %s", clk_msg);
    server.el = aeCreateEventLoop(server.maxclients+CONFIG_FDSET_INCR);
    if (server.el == NULL) {
        serverLog(LL_WARNING,
            "Failed creating the event loop. Error message: '%s'",
            strerror(errno));
        exit(1);
    }
    server.db = zmalloc(sizeof(redisDb)*server.dbnum);

    /* Create the Redis databases, and initialize other internal state. */
    int slot_count_bits = 0;
    int flags = KVSTORE_ALLOCATE_DICTS_ON_DEMAND;
    if (server.cluster_enabled) {
        slot_count_bits = CLUSTER_SLOT_MASK_BITS;
        flags |= KVSTORE_FREE_EMPTY_DICTS;
    }
    for (j = 0; j < server.dbnum; j++) {
        server.db[j].keys = kvstoreCreate(&dbDictType, slot_count_bits, flags);
        server.db[j].expires = kvstoreCreate(&dbExpiresDictType, slot_count_bits, flags);
        server.db[j].hexpires = ebCreate();
        server.db[j].expires_cursor = 0;
        server.db[j].blocking_keys = dictCreate(&keylistDictType);
        server.db[j].blocking_keys_unblock_on_nokey = dictCreate(&objectKeyPointerValueDictType);
        server.db[j].ready_keys = dictCreate(&objectKeyPointerValueDictType);
        server.db[j].watched_keys = dictCreate(&keylistDictType);
        server.db[j].id = j;
        server.db[j].avg_ttl = 0;
        server.db[j].defrag_later = listCreate();
        listSetFreeMethod(server.db[j].defrag_later,(void (*)(void*))sdsfree);
    }
    evictionPoolAlloc(); /* Initialize the LRU keys pool. */
    /* Note that server.pubsub_channels was chosen to be a kvstore (with only one dict, which
     * seems odd) just to make the code cleaner by making it be the same type as server.pubsubshard_channels
     * (which has to be kvstore), see pubsubtype.serverPubSubChannels */
    server.pubsub_channels = kvstoreCreate(&objToDictDictType, 0, KVSTORE_ALLOCATE_DICTS_ON_DEMAND);
    server.pubsub_patterns = dictCreate(&objToDictDictType);
    server.pubsubshard_channels = kvstoreCreate(&objToDictDictType, slot_count_bits, KVSTORE_ALLOCATE_DICTS_ON_DEMAND | KVSTORE_FREE_EMPTY_DICTS);
    server.pubsub_clients = 0;
    server.watching_clients = 0;
    server.cronloops = 0;
    server.in_exec = 0;
    server.busy_module_yield_flags = BUSY_MODULE_YIELD_NONE;
    server.busy_module_yield_reply = NULL;
    server.client_pause_in_transaction = 0;
    server.child_pid = -1;
    server.child_type = CHILD_TYPE_NONE;
    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    server.rdb_pipe_conns = NULL;
    server.rdb_pipe_numconns = 0;
    server.rdb_pipe_numconns_writing = 0;
    server.rdb_pipe_buff = NULL;
    server.rdb_pipe_bufflen = 0;
    server.rdb_bgsave_scheduled = 0;
    server.child_info_pipe[0] = -1;
    server.child_info_pipe[1] = -1;
    server.child_info_nread = 0;
    server.aof_buf = sdsempty();
    server.lastsave = time(NULL); /* At startup we consider the DB saved. */
    server.lastbgsave_try = 0;    /* At startup we never tried to BGSAVE. */
    server.rdb_save_time_last = -1;
    server.rdb_save_time_start = -1;
    server.rdb_last_load_keys_expired = 0;
    server.rdb_last_load_keys_loaded = 0;
    server.dirty = 0;
    resetServerStats();
    /* A few stats we don't want to reset: server startup time, and peak mem. */
    server.stat_starttime = time(NULL);
    server.stat_peak_memory = 0;
    server.stat_current_cow_peak = 0;
    server.stat_current_cow_bytes = 0;
    server.stat_current_cow_updated = 0;
    server.stat_current_save_keys_processed = 0;
    server.stat_current_save_keys_total = 0;
    server.stat_rdb_cow_bytes = 0;
    server.stat_aof_cow_bytes = 0;
    server.stat_module_cow_bytes = 0;
    server.stat_module_progress = 0;
    for (int j = 0; j < CLIENT_TYPE_COUNT; j++)
        server.stat_clients_type_memory[j] = 0;
    server.stat_cluster_links_memory = 0;
    server.cron_malloc_stats.zmalloc_used = 0;
    server.cron_malloc_stats.process_rss = 0;
    server.cron_malloc_stats.allocator_allocated = 0;
    server.cron_malloc_stats.allocator_active = 0;
    server.cron_malloc_stats.allocator_resident = 0;
    server.lastbgsave_status = C_OK;
    server.aof_last_write_status = C_OK;
    server.aof_last_write_errno = 0;
    server.repl_good_slaves_count = 0;
    server.last_sig_received = 0;

    /* Initiate acl info struct */
    server.acl_info.invalid_cmd_accesses = 0;
    server.acl_info.invalid_key_accesses  = 0;
    server.acl_info.user_auth_failures = 0;
    server.acl_info.invalid_channel_accesses = 0;

    /* Create the timer callback, this is our way to process many background
     * operations incrementally, like clients timeout, eviction of unaccessed
     * expired keys and so forth. */
    if (aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL) == AE_ERR) {
        serverPanic("Can't create event loop timers.");
        exit(1);
    }

    /* Register a readable event for the pipe used to awake the event loop
     * from module threads. */
    if (aeCreateFileEvent(server.el, server.module_pipe[0], AE_READABLE,
        modulePipeReadable,NULL) == AE_ERR) {
            serverPanic(
                "Error registering the readable event for the module pipe.");
    }

    /* Register before and after sleep handlers (note this needs to be done
     * before loading persistence since it is used by processEventsWhileBlocked. */
    aeSetBeforeSleepProc(server.el,beforeSleep);
    aeSetAfterSleepProc(server.el,afterSleep);

    /* 32 bit instances are limited to 4GB of address space, so if there is
     * no explicit limit in the user provided configuration we set a limit
     * at 3 GB using maxmemory with 'noeviction' policy'. This avoids
     * useless crashes of the Redis instance for out of memory. */
    if (server.arch_bits == 32 && server.maxmemory == 0) {
        serverLog(LL_WARNING,"Warning: 32 bit instance detected but no memory limit set. Setting 3 GB maxmemory limit with 'noeviction' policy now.");
        server.maxmemory = 3072LL*(1024*1024); /* 3 GB */
        server.maxmemory_policy = MAXMEMORY_NO_EVICTION;
    }

    luaEnvInit();
    scriptingInit(1);
    if (functionsInit() == C_ERR) {
        serverPanic("Functions initialization failed, check the server logs.");
        exit(1);
    }
    slowlogInit();
    latencyMonitorInit();

    /* Initialize ACL default password if it exists */
    ACLUpdateDefaultUserPassword(server.requirepass);

    applyWatchdogPeriod();

    if (server.maxmemory_clients != 0)
        initServerClientMemUsageBuckets();
}

void initListeners(void) {
    /* Setup listeners from server config for TCP/TLS/Unix */
    int conn_index;
    connListener *listener;
    if (server.port != 0) {
        conn_index = connectionIndexByType(CONN_TYPE_SOCKET);
        if (conn_index < 0)
            serverPanic("Failed finding connection listener of %s", CONN_TYPE_SOCKET);
        listener = &server.listeners[conn_index];
        listener->bindaddr = server.bindaddr;
        listener->bindaddr_count = server.bindaddr_count;
        listener->port = server.port;
        listener->ct = connectionByType(CONN_TYPE_SOCKET);
    }

    if (server.tls_port || server.tls_replication || server.tls_cluster) {
        ConnectionType *ct_tls = connectionTypeTls();
        if (!ct_tls) {
            serverLog(LL_WARNING, "Failed finding TLS support.");
            exit(1);
        }
        if (connTypeConfigure(ct_tls, &server.tls_ctx_config, 1) == C_ERR) {
            serverLog(LL_WARNING, "Failed to configure TLS. Check logs for more info.");
            exit(1);
        }
    }

    if (server.tls_port != 0) {
        conn_index = connectionIndexByType(CONN_TYPE_TLS);
        if (conn_index < 0)
            serverPanic("Failed finding connection listener of %s", CONN_TYPE_TLS);
        listener = &server.listeners[conn_index];
        listener->bindaddr = server.bindaddr;
        listener->bindaddr_count = server.bindaddr_count;
        listener->port = server.tls_port;
        listener->ct = connectionByType(CONN_TYPE_TLS);
    }
    if (server.unixsocket != NULL) {
        conn_index = connectionIndexByType(CONN_TYPE_UNIX);
        if (conn_index < 0)
            serverPanic("Failed finding connection listener of %s", CONN_TYPE_UNIX);
        listener = &server.listeners[conn_index];
        listener->bindaddr = &server.unixsocket;
        listener->bindaddr_count = 1;
        listener->ct = connectionByType(CONN_TYPE_UNIX);
        listener->priv = &server.unixsocketperm; /* Unix socket specified */
    }

    /* create all the configured listener, and add handler to start to accept */
    int listen_fds = 0;
    for (int j = 0; j < CONN_TYPE_MAX; j++) {
        listener = &server.listeners[j];
        if (listener->ct == NULL)
            continue;

        if (connListen(listener) == C_ERR) {
            serverLog(LL_WARNING, "Failed listening on port %u (%s), aborting.", listener->port, listener->ct->get_type(NULL));
            exit(1);
        }

        if (createSocketAcceptHandler(listener, connAcceptHandler(listener->ct)) != C_OK)
            serverPanic("Unrecoverable error creating %s listener accept handler.", listener->ct->get_type(NULL));

       listen_fds += listener->count;
    }

    if (listen_fds == 0) {
        serverLog(LL_WARNING, "Configured to not listen anywhere, exiting.");
        exit(1);
    }
}

/* Some steps in server initialization need to be done last (after modules
 * are loaded).
 * Specifically, creation of threads due to a race bug in ld.so, in which
 * Thread Local Storage initialization collides with dlopen call.
 * see: https://sourceware.org/bugzilla/show_bug.cgi?id=19329 */
void InitServerLast(void) {
    bioInit();
    initThreadedIO();
    set_jemalloc_bg_thread(server.jemalloc_bg_thread);
    server.initial_memory_usage = zmalloc_used_memory();
}

/* The purpose of this function is to try to "glue" consecutive range
 * key specs in order to build the legacy (first,last,step) spec
 * used by the COMMAND command.
 * By far the most common case is just one range spec (e.g. SET)
 * but some commands' ranges were split into two or more ranges
 * in order to have different flags for different keys (e.g. SMOVE,
 * first key is "RW ACCESS DELETE", second key is "RW INSERT").
 *
 * Additionally set the CMD_MOVABLE_KEYS flag for commands that may have key
 * names in their arguments, but the legacy range spec doesn't cover all of them.
 *
 * This function uses very basic heuristics and is "best effort":
 * 1. Only commands which have only "range" specs are considered.
 * 2. Only range specs with keystep of 1 are considered.
 * 3. The order of the range specs must be ascending (i.e.
 *    lastkey of spec[i] == firstkey-1 of spec[i+1]).
 *
 * This function will succeed on all native Redis commands and may
 * fail on module commands, even if it only has "range" specs that
 * could actually be "glued", in the following cases:
 * 1. The order of "range" specs is not ascending (e.g. the spec for
 *    the key at index 2 was added before the spec of the key at
 *    index 1).
 * 2. The "range" specs have keystep >1.
 *
 * If this functions fails it means that the legacy (first,last,step)
 * spec used by COMMAND will show 0,0,0. This is not a dire situation
 * because anyway the legacy (first,last,step) spec is to be deprecated
 * and one should use the new key specs scheme.
 */
void populateCommandLegacyRangeSpec(struct redisCommand *c) {
    memset(&c->legacy_range_key_spec, 0, sizeof(c->legacy_range_key_spec));

    /* Set the movablekeys flag if we have a GETKEYS flag for modules.
     * Note that for native redis commands, we always have keyspecs,
     * with enough information to rely on for movablekeys. */
    if (c->flags & CMD_MODULE_GETKEYS)
        c->flags |= CMD_MOVABLE_KEYS;

    /* no key-specs, no keys, exit. */
    if (c->key_specs_num == 0) {
        return;
    }

    if (c->key_specs_num == 1 &&
        c->key_specs[0].begin_search_type == KSPEC_BS_INDEX &&
        c->key_specs[0].find_keys_type == KSPEC_FK_RANGE)
    {
        /* Quick win, exactly one range spec. */
        c->legacy_range_key_spec = c->key_specs[0];
        /* If it has the incomplete flag, set the movablekeys flag on the command. */
        if (c->key_specs[0].flags & CMD_KEY_INCOMPLETE)
            c->flags |= CMD_MOVABLE_KEYS;
        return;
    }

    int firstkey = INT_MAX, lastkey = 0;
    int prev_lastkey = 0;
    for (int i = 0; i < c->key_specs_num; i++) {
        if (c->key_specs[i].begin_search_type != KSPEC_BS_INDEX ||
            c->key_specs[i].find_keys_type != KSPEC_FK_RANGE)
        {
            /* Found an incompatible (non range) spec, skip it, and set the movablekeys flag. */
            c->flags |= CMD_MOVABLE_KEYS;
            continue;
        }
        if (c->key_specs[i].fk.range.keystep != 1 ||
            (prev_lastkey && prev_lastkey != c->key_specs[i].bs.index.pos-1))
        {
            /* Found a range spec that's not plain (step of 1) or not consecutive to the previous one.
             * Skip it, and we set the movablekeys flag. */
            c->flags |= CMD_MOVABLE_KEYS;
            continue;
        }
        if (c->key_specs[i].flags & CMD_KEY_INCOMPLETE) {
            /* The spec we're using is incomplete, we can use it, but we also have to set the movablekeys flag. */
            c->flags |= CMD_MOVABLE_KEYS;
        }
        firstkey = min(firstkey, c->key_specs[i].bs.index.pos);
        /* Get the absolute index for lastkey (in the "range" spec, lastkey is relative to firstkey) */
        int lastkey_abs_index = c->key_specs[i].fk.range.lastkey;
        if (lastkey_abs_index >= 0)
            lastkey_abs_index += c->key_specs[i].bs.index.pos;
        /* For lastkey we use unsigned comparison to handle negative values correctly */
        lastkey = max((unsigned)lastkey, (unsigned)lastkey_abs_index);
        prev_lastkey = lastkey;
    }

    if (firstkey == INT_MAX) {
        /* Couldn't find range specs, the legacy range spec will remain empty, and we set the movablekeys flag. */
        c->flags |= CMD_MOVABLE_KEYS;
        return;
    }

    serverAssert(firstkey != 0);
    serverAssert(lastkey != 0);

    c->legacy_range_key_spec.begin_search_type = KSPEC_BS_INDEX;
    c->legacy_range_key_spec.bs.index.pos = firstkey;
    c->legacy_range_key_spec.find_keys_type = KSPEC_FK_RANGE;
    c->legacy_range_key_spec.fk.range.lastkey = lastkey < 0 ? lastkey : (lastkey-firstkey); /* in the "range" spec, lastkey is relative to firstkey */
    c->legacy_range_key_spec.fk.range.keystep = 1;
    c->legacy_range_key_spec.fk.range.limit = 0;
}

sds catSubCommandFullname(const char *parent_name, const char *sub_name) {
    return sdscatfmt(sdsempty(), "%s|%s", parent_name, sub_name);
}

void commandAddSubcommand(struct redisCommand *parent, struct redisCommand *subcommand, const char *declared_name) {
    if (!parent->subcommands_dict)
        parent->subcommands_dict = dictCreate(&commandTableDictType);

    subcommand->parent = parent; /* Assign the parent command */
    subcommand->id = ACLGetCommandID(subcommand->fullname); /* Assign the ID used for ACL. */

    serverAssert(dictAdd(parent->subcommands_dict, sdsnew(declared_name), subcommand) == DICT_OK);
}

/* Set implicit ACl categories (see comment above the definition of
 * struct redisCommand). */
void setImplicitACLCategories(struct redisCommand *c) {
    if (c->flags & CMD_WRITE)
        c->acl_categories |= ACL_CATEGORY_WRITE;
    /* Exclude scripting commands from the RO category. */
    if (c->flags & CMD_READONLY && !(c->acl_categories & ACL_CATEGORY_SCRIPTING))
        c->acl_categories |= ACL_CATEGORY_READ;
    if (c->flags & CMD_ADMIN)
        c->acl_categories |= ACL_CATEGORY_ADMIN|ACL_CATEGORY_DANGEROUS;
    if (c->flags & CMD_PUBSUB)
        c->acl_categories |= ACL_CATEGORY_PUBSUB;
    if (c->flags & CMD_FAST)
        c->acl_categories |= ACL_CATEGORY_FAST;
    if (c->flags & CMD_BLOCKING)
        c->acl_categories |= ACL_CATEGORY_BLOCKING;

    /* If it's not @fast is @slow in this binary world. */
    if (!(c->acl_categories & ACL_CATEGORY_FAST))
        c->acl_categories |= ACL_CATEGORY_SLOW;
}

/* Recursively populate the command structure.
 *
 * On success, the function return C_OK. Otherwise C_ERR is returned and we won't
 * add this command in the commands dict. */
int populateCommandStructure(struct redisCommand *c) {
    /* If the command marks with CMD_SENTINEL, it exists in sentinel. */
    if (!(c->flags & CMD_SENTINEL) && server.sentinel_mode)
        return C_ERR;

    /* If the command marks with CMD_ONLY_SENTINEL, it only exists in sentinel. */
    if (c->flags & CMD_ONLY_SENTINEL && !server.sentinel_mode)
        return C_ERR;

    /* Translate the command string flags description into an actual
     * set of flags. */
    setImplicitACLCategories(c);

    /* We start with an unallocated histogram and only allocate memory when a command
     * has been issued for the first time */
    c->latency_histogram = NULL;

    /* Handle the legacy range spec and the "movablekeys" flag (must be done after populating all key specs). */
    populateCommandLegacyRangeSpec(c);

    /* Assign the ID used for ACL. */
    c->id = ACLGetCommandID(c->fullname);

    /* Handle subcommands */
    if (c->subcommands) {
        for (int j = 0; c->subcommands[j].declared_name; j++) {
            struct redisCommand *sub = c->subcommands+j;

            sub->fullname = catSubCommandFullname(c->declared_name, sub->declared_name);
            if (populateCommandStructure(sub) == C_ERR)
                continue;

            commandAddSubcommand(c, sub, sub->declared_name);
        }
    }

    return C_OK;
}

extern struct redisCommand redisCommandTable[];

/* Populates the Redis Command Table dict from the static table in commands.c
 * which is auto generated from the json files in the commands folder. */
void populateCommandTable(void) {
    int j;
    struct redisCommand *c;

    for (j = 0;; j++) {
        c = redisCommandTable + j;
        if (c->declared_name == NULL)
            break;

        int retval1, retval2;

        c->fullname = sdsnew(c->declared_name);
        if (populateCommandStructure(c) == C_ERR)
            continue;

        retval1 = dictAdd(server.commands, sdsdup(c->fullname), c);
        /* Populate an additional dictionary that will be unaffected
         * by rename-command statements in redis.conf. */
        retval2 = dictAdd(server.orig_commands, sdsdup(c->fullname), c);
        serverAssert(retval1 == DICT_OK && retval2 == DICT_OK);
    }
}

void resetCommandTableStats(dict* commands) {
    struct redisCommand *c;
    dictEntry *de;
    dictIterator *di;

    di = dictGetSafeIterator(commands);
    while((de = dictNext(di)) != NULL) {
        c = (struct redisCommand *) dictGetVal(de);
        c->microseconds = 0;
        c->calls = 0;
        c->rejected_calls = 0;
        c->failed_calls = 0;
        if(c->latency_histogram) {
            hdr_close(c->latency_histogram);
            c->latency_histogram = NULL;
        }
        if (c->subcommands_dict)
            resetCommandTableStats(c->subcommands_dict);
    }
    dictReleaseIterator(di);
}

void resetErrorTableStats(void) {
    freeErrorsRadixTreeAsync(server.errors);
    server.errors = raxNew();
    server.errors_enabled = 1;
}

/* ========================== Redis OP Array API ============================ */

int redisOpArrayAppend(redisOpArray *oa, int dbid, robj **argv, int argc, int target) {
    redisOp *op;
    int prev_capacity = oa->capacity;

    if (oa->numops == 0) {
        oa->capacity = 16;
    } else if (oa->numops >= oa->capacity) {
        oa->capacity *= 2;
    }

    if (prev_capacity != oa->capacity)
        oa->ops = zrealloc(oa->ops,sizeof(redisOp)*oa->capacity);
    op = oa->ops+oa->numops;
    op->dbid = dbid;
    op->argv = argv;
    op->argc = argc;
    op->target = target;
    oa->numops++;
    return oa->numops;
}

void redisOpArrayFree(redisOpArray *oa) {
    while(oa->numops) {
        int j;
        redisOp *op;

        oa->numops--;
        op = oa->ops+oa->numops;
        for (j = 0; j < op->argc; j++)
            decrRefCount(op->argv[j]);
        zfree(op->argv);
    }
    /* no need to free the actual op array, we reuse the memory for future commands */
    serverAssert(!oa->numops);
}

/* ====================== Commands lookup and execution ===================== */

int isContainerCommandBySds(sds s) {
    struct redisCommand *base_cmd = dictFetchValue(server.commands, s);
    int has_subcommands = base_cmd && base_cmd->subcommands_dict;
    return has_subcommands;
}

struct redisCommand *lookupSubcommand(struct redisCommand *container, sds sub_name) {
    return dictFetchValue(container->subcommands_dict, sub_name);
}

/* Look up a command by argv and argc
 *
 * If `strict` is not 0 we expect argc to be exact (i.e. argc==2
 * for a subcommand and argc==1 for a top-level command)
 * `strict` should be used every time we want to look up a command
 * name (e.g. in COMMAND INFO) rather than to find the command
 * a user requested to execute (in processCommand).
 */
struct redisCommand *lookupCommandLogic(dict *commands, robj **argv, int argc, int strict) {
    struct redisCommand *base_cmd = dictFetchValue(commands, argv[0]->ptr);
    int has_subcommands = base_cmd && base_cmd->subcommands_dict;
    if (argc == 1 || !has_subcommands) {
        if (strict && argc != 1)
            return NULL;
        /* Note: It is possible that base_cmd->proc==NULL (e.g. CONFIG) */
        return base_cmd;
    } else { /* argc > 1 && has_subcommands */
        if (strict && argc != 2)
            return NULL;
        /* Note: Currently we support just one level of subcommands */
        return lookupSubcommand(base_cmd, argv[1]->ptr);
    }
}

struct redisCommand *lookupCommand(robj **argv, int argc) {
    return lookupCommandLogic(server.commands,argv,argc,0);
}

struct redisCommand *lookupCommandBySdsLogic(dict *commands, sds s) {
    int argc, j;
    sds *strings = sdssplitlen(s,sdslen(s),"|",1,&argc);
    if (strings == NULL)
        return NULL;
    if (argc < 1 || argc > 2) {
        /* Currently we support just one level of subcommands */
        sdsfreesplitres(strings,argc);
        return NULL;
    }

    serverAssert(argc > 0); /* Avoid warning `-Wmaybe-uninitialized` in lookupCommandLogic() */
    robj objects[argc];
    robj *argv[argc];
    for (j = 0; j < argc; j++) {
        initStaticStringObject(objects[j],strings[j]);
        argv[j] = &objects[j];
    }

    struct redisCommand *cmd = lookupCommandLogic(commands,argv,argc,1);
    sdsfreesplitres(strings,argc);
    return cmd;
}

struct redisCommand *lookupCommandBySds(sds s) {
    return lookupCommandBySdsLogic(server.commands,s);
}

struct redisCommand *lookupCommandByCStringLogic(dict *commands, const char *s) {
    struct redisCommand *cmd;
    sds name = sdsnew(s);

    cmd = lookupCommandBySdsLogic(commands,name);
    sdsfree(name);
    return cmd;
}

struct redisCommand *lookupCommandByCString(const char *s) {
    return lookupCommandByCStringLogic(server.commands,s);
}

/* Lookup the command in the current table, if not found also check in
 * the original table containing the original command names unaffected by
 * redis.conf rename-command statement.
 *
 * This is used by functions rewriting the argument vector such as
 * rewriteClientCommandVector() in order to set client->cmd pointer
 * correctly even if the command was renamed. */
struct redisCommand *lookupCommandOrOriginal(robj **argv ,int argc) {
    struct redisCommand *cmd = lookupCommandLogic(server.commands, argv, argc, 0);

    if (!cmd) cmd = lookupCommandLogic(server.orig_commands, argv, argc, 0);
    return cmd;
}

/* Commands arriving from the master client or AOF client, should never be rejected. */
int mustObeyClient(client *c) {
    return c->id == CLIENT_ID_AOF || c->flags & CLIENT_MASTER;
}

static int shouldPropagate(int target) {
    if (!server.replication_allowed || target == PROPAGATE_NONE || server.loading)
        return 0;

    if (target & PROPAGATE_AOF) {
        if (server.aof_state != AOF_OFF)
            return 1;
    }
    if (target & PROPAGATE_REPL) {
        if (server.masterhost == NULL && (server.repl_backlog || listLength(server.slaves) != 0))
            return 1;
    }

    return 0;
}

/* Propagate the specified command (in the context of the specified database id)
 * to AOF and Slaves.
 *
 * flags are an xor between:
 * + PROPAGATE_NONE (no propagation of command at all)
 * + PROPAGATE_AOF (propagate into the AOF file if is enabled)
 * + PROPAGATE_REPL (propagate into the replication link)
 *
 * This is an internal low-level function and should not be called!
 *
 * The API for propagating commands is alsoPropagate().
 *
 * dbid value of -1 is saved to indicate that the called do not want
 * to replicate SELECT for this command (used for database neutral commands).
 */
static void propagateNow(int dbid, robj **argv, int argc, int target) {
    if (!shouldPropagate(target))
        return;

    /* This needs to be unreachable since the dataset should be fixed during
     * replica pause (otherwise data may be lost during a failover) */
    serverAssert(!(isPausedActions(PAUSE_ACTION_REPLICA) &&
                   (!server.client_pause_in_transaction)));

    if (server.aof_state != AOF_OFF && target & PROPAGATE_AOF)
        feedAppendOnlyFile(dbid,argv,argc);
    if (target & PROPAGATE_REPL)
        replicationFeedSlaves(server.slaves,dbid,argv,argc);
}

/* Used inside commands to schedule the propagation of additional commands
 * after the current command is propagated to AOF / Replication.
 *
 * dbid is the database ID the command should be propagated into.
 * Arguments of the command to propagate are passed as an array of redis
 * objects pointers of len 'argc', using the 'argv' vector.
 *
 * The function does not take a reference to the passed 'argv' vector,
 * so it is up to the caller to release the passed argv (but it is usually
 * stack allocated).  The function automatically increments ref count of
 * passed objects, so the caller does not need to. */
void alsoPropagate(int dbid, robj **argv, int argc, int target) {
    robj **argvcopy;
    int j;

    if (!shouldPropagate(target))
        return;

    argvcopy = zmalloc(sizeof(robj*)*argc);
    for (j = 0; j < argc; j++) {
        argvcopy[j] = argv[j];
        incrRefCount(argv[j]);
    }
    redisOpArrayAppend(&server.also_propagate,dbid,argvcopy,argc,target);
}

/* It is possible to call the function forceCommandPropagation() inside a
 * Redis command implementation in order to to force the propagation of a
 * specific command execution into AOF / Replication. */
void forceCommandPropagation(client *c, int flags) {
    serverAssert(c->cmd->flags & (CMD_WRITE | CMD_MAY_REPLICATE));
    if (flags & PROPAGATE_REPL) c->flags |= CLIENT_FORCE_REPL;
    if (flags & PROPAGATE_AOF) c->flags |= CLIENT_FORCE_AOF;
}

/* Avoid that the executed command is propagated at all. This way we
 * are free to just propagate what we want using the alsoPropagate()
 * API. */
void preventCommandPropagation(client *c) {
    c->flags |= CLIENT_PREVENT_PROP;
}

/* AOF specific version of preventCommandPropagation(). */
void preventCommandAOF(client *c) {
    c->flags |= CLIENT_PREVENT_AOF_PROP;
}

/* Replication specific version of preventCommandPropagation(). */
void preventCommandReplication(client *c) {
    c->flags |= CLIENT_PREVENT_REPL_PROP;
}

/* Log the last command a client executed into the slowlog. */
void slowlogPushCurrentCommand(client *c, struct redisCommand *cmd, ustime_t duration) {
    /* Some commands may contain sensitive data that should not be available in the slowlog. */
    if (cmd->flags & CMD_SKIP_SLOWLOG)
        return;

    /* If command argument vector was rewritten, use the original
     * arguments. */
    robj **argv = c->original_argv ? c->original_argv : c->argv;
    int argc = c->original_argv ? c->original_argc : c->argc;
    slowlogPushEntryIfNeeded(c,argv,argc,duration);
}

/* This function is called in order to update the total command histogram duration.
 * The latency unit is nano-seconds.
 * If needed it will allocate the histogram memory and trim the duration to the upper/lower tracking limits*/
void updateCommandLatencyHistogram(struct hdr_histogram **latency_histogram, int64_t duration_hist){
    if (duration_hist < LATENCY_HISTOGRAM_MIN_VALUE)
        duration_hist=LATENCY_HISTOGRAM_MIN_VALUE;
    if (duration_hist>LATENCY_HISTOGRAM_MAX_VALUE)
        duration_hist=LATENCY_HISTOGRAM_MAX_VALUE;
    if (*latency_histogram==NULL)
        hdr_init(LATENCY_HISTOGRAM_MIN_VALUE,LATENCY_HISTOGRAM_MAX_VALUE,LATENCY_HISTOGRAM_PRECISION,latency_histogram);
    hdr_record_value(*latency_histogram,duration_hist);
}

/* Handle the alsoPropagate() API to handle commands that want to propagate
 * multiple separated commands. Note that alsoPropagate() is not affected
 * by CLIENT_PREVENT_PROP flag. */
static void propagatePendingCommands(void) {
    if (server.also_propagate.numops == 0)
        return;

    int j;
    redisOp *rop;

    /* If we got here it means we have finished an execution-unit.
     * If that unit has caused propagation of multiple commands, they
     * should be propagated as a transaction */
    int transaction = server.also_propagate.numops > 1;

    /* In case a command that may modify random keys was run *directly*
     * (i.e. not from within a script, MULTI/EXEC, RM_Call, etc.) we want
     * to avoid using a transaction (much like active-expire) */
    if (server.current_client &&
        server.current_client->cmd &&
        server.current_client->cmd->flags & CMD_TOUCHES_ARBITRARY_KEYS)
    {
        transaction = 0;
    }

    if (transaction) {
        /* We use dbid=-1 to indicate we do not want to replicate SELECT.
         * It'll be inserted together with the next command (inside the MULTI) */
        propagateNow(-1,&shared.multi,1,PROPAGATE_AOF|PROPAGATE_REPL);
    }

    for (j = 0; j < server.also_propagate.numops; j++) {
        rop = &server.also_propagate.ops[j];
        serverAssert(rop->target);
        propagateNow(rop->dbid,rop->argv,rop->argc,rop->target);
    }

    if (transaction) {
        /* We use dbid=-1 to indicate we do not want to replicate select */
        propagateNow(-1,&shared.exec,1,PROPAGATE_AOF|PROPAGATE_REPL);
    }

    redisOpArrayFree(&server.also_propagate);
}

/* Performs operations that should be performed after an execution unit ends.
 * Execution unit is a code that should be done atomically.
 * Execution units can be nested and are not necessarily starts with Redis command.
 *
 * For example the following is a logical unit:
 *   active expire ->
 *      trigger del notification of some module ->
 *          accessing a key ->
 *              trigger key miss notification of some other module
 *
 * What we want to achieve is that the entire execution unit will be done atomically,
 * currently with respect to replication and post jobs, but in the future there might
 * be other considerations. So we basically want the `postUnitOperations` to trigger
 * after the entire chain finished. */
void postExecutionUnitOperations(void) {
    if (server.execution_nesting)
        return;

    firePostExecutionUnitJobs();

    /* If we are at the top-most call() and not inside a an active module
     * context (e.g. within a module timer) we can propagate what we accumulated. */
    propagatePendingCommands();

    /* Module subsystem post-execution-unit logic */
    modulePostExecutionUnitOperations();
}

/* Increment the command failure counters (either rejected_calls or failed_calls).
 * The decision which counter to increment is done using the flags argument, options are:
 * * ERROR_COMMAND_REJECTED - update rejected_calls
 * * ERROR_COMMAND_FAILED - update failed_calls
 *
 * The function also reset the prev_err_count to make sure we will not count the same error
 * twice, its possible to pass a NULL cmd value to indicate that the error was counted elsewhere.
 *
 * The function returns true if stats was updated and false if not. */
int incrCommandStatsOnError(struct redisCommand *cmd, int flags) {
    /* hold the prev error count captured on the last command execution */
    static long long prev_err_count = 0;
    int res = 0;
    if (cmd) {
        if ((server.stat_total_error_replies - prev_err_count) > 0) {
            if (flags & ERROR_COMMAND_REJECTED) {
                cmd->rejected_calls++;
                res = 1;
            } else if (flags & ERROR_COMMAND_FAILED) {
                cmd->failed_calls++;
                res = 1;
            }
        }
    }
    prev_err_count = server.stat_total_error_replies;
    return res;
}

/* Call() is the core of Redis execution of a command.
 *
 * The following flags can be passed:
 * CMD_CALL_NONE        No flags.
 * CMD_CALL_PROPAGATE_AOF   Append command to AOF if it modified the dataset
 *                          or if the client flags are forcing propagation.
 * CMD_CALL_PROPAGATE_REPL  Send command to slaves if it modified the dataset
 *                          or if the client flags are forcing propagation.
 * CMD_CALL_PROPAGATE   Alias for PROPAGATE_AOF|PROPAGATE_REPL.
 * CMD_CALL_FULL        Alias for SLOWLOG|STATS|PROPAGATE.
 *
 * The exact propagation behavior depends on the client flags.
 * Specifically:
 *
 * 1. If the client flags CLIENT_FORCE_AOF or CLIENT_FORCE_REPL are set
 *    and assuming the corresponding CMD_CALL_PROPAGATE_AOF/REPL is set
 *    in the call flags, then the command is propagated even if the
 *    dataset was not affected by the command.
 * 2. If the client flags CLIENT_PREVENT_REPL_PROP or CLIENT_PREVENT_AOF_PROP
 *    are set, the propagation into AOF or to slaves is not performed even
 *    if the command modified the dataset.
 *
 * Note that regardless of the client flags, if CMD_CALL_PROPAGATE_AOF
 * or CMD_CALL_PROPAGATE_REPL are not set, then respectively AOF or
 * slaves propagation will never occur.
 *
 * Client flags are modified by the implementation of a given command
 * using the following API:
 *
 * forceCommandPropagation(client *c, int flags);
 * preventCommandPropagation(client *c);
 * preventCommandAOF(client *c);
 * preventCommandReplication(client *c);
 *
 */
void call(client *c, int flags) {
    long long dirty;
    uint64_t client_old_flags = c->flags;
    struct redisCommand *real_cmd = c->realcmd;
    client *prev_client = server.executing_client;
    server.executing_client = c;

    /* When call() is issued during loading the AOF we don't want commands called
     * from module, exec or LUA to go into the slowlog or to populate statistics. */
    int update_command_stats = !isAOFLoadingContext();

    /* We want to be aware of a client which is making a first time attempt to execute this command
     * and a client which is reprocessing command again (after being unblocked).
     * Blocked clients can be blocked in different places and not always it means the call() function has been
     * called. For example this is required for avoiding double logging to monitors.*/
    int reprocessing_command = flags & CMD_CALL_REPROCESSING;

    /* Initialization: clear the flags that must be set by the command on
     * demand, and initialize the array for additional commands propagation. */
    c->flags &= ~(CLIENT_FORCE_AOF|CLIENT_FORCE_REPL|CLIENT_PREVENT_PROP);

    /* Redis core is in charge of propagation when the first entry point
     * of call() is processCommand().
     * The only other option to get to call() without having processCommand
     * as an entry point is if a module triggers RM_Call outside of call()
     * context (for example, in a timer).
     * In that case, the module is in charge of propagation. */

    /* Call the command. */
    dirty = server.dirty;
    long long old_master_repl_offset = server.master_repl_offset;
    incrCommandStatsOnError(NULL, 0);

    const long long call_timer = ustime();
    enterExecutionUnit(1, call_timer);

    /* setting the CLIENT_EXECUTING_COMMAND flag so we will avoid
     * sending client side caching message in the middle of a command reply.
     * In case of blocking commands, the flag will be un-set only after successfully
     * re-processing and unblock the client.*/
    c->flags |= CLIENT_EXECUTING_COMMAND;

    /* Setting the CLIENT_REPROCESSING_COMMAND flag so that during the actual
     * processing of the command proc, the client is aware that it is being
     * re-processed. */
    if (reprocessing_command) c->flags |= CLIENT_REPROCESSING_COMMAND;

    monotime monotonic_start = 0;
    if (monotonicGetType() == MONOTONIC_CLOCK_HW)
        monotonic_start = getMonotonicUs();

    c->cmd->proc(c);

    /* Clear the CLIENT_REPROCESSING_COMMAND flag after the proc is executed. */
    if (reprocessing_command) c->flags &= ~CLIENT_REPROCESSING_COMMAND;

    exitExecutionUnit();

    /* In case client is blocked after trying to execute the command,
     * it means the execution is not yet completed and we MIGHT reprocess the command in the future. */
    if (!(c->flags & CLIENT_BLOCKED)) c->flags &= ~(CLIENT_EXECUTING_COMMAND);

    /* In order to avoid performance implication due to querying the clock using a system call 3 times,
     * we use a monotonic clock, when we are sure its cost is very low, and fall back to non-monotonic call otherwise. */
    ustime_t duration;
    if (monotonicGetType() == MONOTONIC_CLOCK_HW)
        duration = getMonotonicUs() - monotonic_start;
    else
        duration = ustime() - call_timer;

    c->duration += duration;
    dirty = server.dirty-dirty;
    if (dirty < 0) dirty = 0;

    /* Update failed command calls if required. */

    if (!incrCommandStatsOnError(real_cmd, ERROR_COMMAND_FAILED) && c->deferred_reply_errors) {
        /* When call is used from a module client, error stats, and total_error_replies
         * isn't updated since these errors, if handled by the module, are internal,
         * and not reflected to users. however, the commandstats does show these calls
         * (made by RM_Call), so it should log if they failed or succeeded. */
        real_cmd->failed_calls++;
    }

    /* After executing command, we will close the client after writing entire
     * reply if it is set 'CLIENT_CLOSE_AFTER_COMMAND' flag. */
    if (c->flags & CLIENT_CLOSE_AFTER_COMMAND) {
        c->flags &= ~CLIENT_CLOSE_AFTER_COMMAND;
        c->flags |= CLIENT_CLOSE_AFTER_REPLY;
    }

    /* Note: the code below uses the real command that was executed
     * c->cmd and c->lastcmd may be different, in case of MULTI-EXEC or
     * re-written commands such as EXPIRE, GEOADD, etc. */

    /* Record the latency this command induced on the main thread.
     * unless instructed by the caller not to log. (happens when processing
     * a MULTI-EXEC from inside an AOF). */
    if (update_command_stats) {
        char *latency_event = (real_cmd->flags & CMD_FAST) ?
                               "fast-command" : "command";
        latencyAddSampleIfNeeded(latency_event,duration/1000);
        if (server.execution_nesting == 0)
            durationAddSample(EL_DURATION_TYPE_CMD, duration);
    }

    /* Log the command into the Slow log if needed.
     * If the client is blocked we will handle slowlog when it is unblocked. */
    if (update_command_stats && !(c->flags & CLIENT_BLOCKED))
        slowlogPushCurrentCommand(c, real_cmd, c->duration);

    /* Send the command to clients in MONITOR mode if applicable,
     * since some administrative commands are considered too dangerous to be shown.
     * Other exceptions is a client which is unblocked and retrying to process the command
     * or we are currently in the process of loading AOF. */
    if (update_command_stats && !reprocessing_command &&
        !(c->cmd->flags & (CMD_SKIP_MONITOR|CMD_ADMIN))) {
        robj **argv = c->original_argv ? c->original_argv : c->argv;
        int argc = c->original_argv ? c->original_argc : c->argc;
        replicationFeedMonitors(c,server.monitors,c->db->id,argv,argc);
    }

    /* Clear the original argv.
     * If the client is blocked we will handle slowlog when it is unblocked. */
    if (!(c->flags & CLIENT_BLOCKED))
        freeClientOriginalArgv(c);

    /* populate the per-command statistics that we show in INFO commandstats.
     * If the client is blocked we will handle latency stats and duration when it is unblocked. */
    if (update_command_stats && !(c->flags & CLIENT_BLOCKED)) {
        real_cmd->calls++;
        real_cmd->microseconds += c->duration;
        if (server.latency_tracking_enabled && !(c->flags & CLIENT_BLOCKED))
            updateCommandLatencyHistogram(&(real_cmd->latency_histogram), c->duration*1000);
    }

    /* The duration needs to be reset after each call except for a blocked command,
     * which is expected to record and reset the duration after unblocking. */
    if (!(c->flags & CLIENT_BLOCKED)) {
        c->duration = 0;
    }

    /* Propagate the command into the AOF and replication link.
     * We never propagate EXEC explicitly, it will be implicitly
     * propagated if needed (see propagatePendingCommands).
     * Also, module commands take care of themselves */
    if (flags & CMD_CALL_PROPAGATE &&
        (c->flags & CLIENT_PREVENT_PROP) != CLIENT_PREVENT_PROP &&
        c->cmd->proc != execCommand &&
        !(c->cmd->flags & CMD_MODULE))
    {
        int propagate_flags = PROPAGATE_NONE;

        /* Check if the command operated changes in the data set. If so
         * set for replication / AOF propagation. */
        if (dirty) propagate_flags |= (PROPAGATE_AOF|PROPAGATE_REPL);

        /* If the client forced AOF / replication of the command, set
         * the flags regardless of the command effects on the data set. */
        if (c->flags & CLIENT_FORCE_REPL) propagate_flags |= PROPAGATE_REPL;
        if (c->flags & CLIENT_FORCE_AOF) propagate_flags |= PROPAGATE_AOF;

        /* However prevent AOF / replication propagation if the command
         * implementation called preventCommandPropagation() or similar,
         * or if we don't have the call() flags to do so. */
        if (c->flags & CLIENT_PREVENT_REPL_PROP        ||
            c->flags & CLIENT_MODULE_PREVENT_REPL_PROP ||
            !(flags & CMD_CALL_PROPAGATE_REPL))
                propagate_flags &= ~PROPAGATE_REPL;
        if (c->flags & CLIENT_PREVENT_AOF_PROP        ||
            c->flags & CLIENT_MODULE_PREVENT_AOF_PROP ||
            !(flags & CMD_CALL_PROPAGATE_AOF))
                propagate_flags &= ~PROPAGATE_AOF;

        /* Call alsoPropagate() only if at least one of AOF / replication
         * propagation is needed. */
        if (propagate_flags != PROPAGATE_NONE)
            alsoPropagate(c->db->id,c->argv,c->argc,propagate_flags);
    }

    /* Restore the old replication flags, since call() can be executed
     * recursively. */
    c->flags &= ~(CLIENT_FORCE_AOF|CLIENT_FORCE_REPL|CLIENT_PREVENT_PROP);
    c->flags |= client_old_flags &
        (CLIENT_FORCE_AOF|CLIENT_FORCE_REPL|CLIENT_PREVENT_PROP);

    /* If the client has keys tracking enabled for client side caching,
     * make sure to remember the keys it fetched via this command. For read-only
     * scripts, don't process the script, only the commands it executes. */
    if ((c->cmd->flags & CMD_READONLY) && (c->cmd->proc != evalRoCommand)
        && (c->cmd->proc != evalShaRoCommand) && (c->cmd->proc != fcallroCommand))
    {
        /* We use the tracking flag of the original external client that
         * triggered the command, but we take the keys from the actual command
         * being executed. */
        if (server.current_client &&
            (server.current_client->flags & CLIENT_TRACKING) &&
            !(server.current_client->flags & CLIENT_TRACKING_BCAST))
        {
            trackingRememberKeys(server.current_client, c);
        }
    }

    if (!(c->flags & CLIENT_BLOCKED))
        server.stat_numcommands++;

    /* Record peak memory after each command and before the eviction that runs
     * before the next command. */
    size_t zmalloc_used = zmalloc_used_memory();
    if (zmalloc_used > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used;

    /* Do some maintenance job and cleanup */
    afterCommand(c);

    /* Remember the replication offset of the client, right after its last
     * command that resulted in propagation. */
    if (old_master_repl_offset != server.master_repl_offset)
        c->woff = server.master_repl_offset;

    /* Client pause takes effect after a transaction has finished. This needs
     * to be located after everything is propagated. */
    if (!server.in_exec && server.client_pause_in_transaction) {
        server.client_pause_in_transaction = 0;
    }

    server.executing_client = prev_client;
}

/* Used when a command that is ready for execution needs to be rejected, due to
 * various pre-execution checks. it returns the appropriate error to the client.
 * If there's a transaction is flags it as dirty, and if the command is EXEC,
 * it aborts the transaction.
 * The duration is reset, since we reject the command, and it did not record.
 * Note: 'reply' is expected to end with \r\n */
void rejectCommand(client *c, robj *reply) {
    flagTransaction(c);
    c->duration = 0;
    if (c->cmd) c->cmd->rejected_calls++;
    if (c->cmd && c->cmd->proc == execCommand) {
        execCommandAbort(c, reply->ptr);
    } else {
        /* using addReplyError* rather than addReply so that the error can be logged. */
        addReplyErrorObject(c, reply);
    }
}

void rejectCommandSds(client *c, sds s) {
    flagTransaction(c);
    c->duration = 0;
    if (c->cmd) c->cmd->rejected_calls++;
    if (c->cmd && c->cmd->proc == execCommand) {
        execCommandAbort(c, s);
        sdsfree(s);
    } else {
        /* The following frees 's'. */
        addReplyErrorSds(c, s);
    }
}

void rejectCommandFormat(client *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted (The args come from the user, they may contain any character). */
    sdsmapchars(s, "\r\n", "  ",  2);
    rejectCommandSds(c, s);
}

/* This is called after a command in call, we can do some maintenance job in it. */
void afterCommand(client *c) {
    UNUSED(c);
    /* Should be done before trackingHandlePendingKeyInvalidations so that we
     * reply to client before invalidating cache (makes more sense) */
    postExecutionUnitOperations();

    /* Flush pending tracking invalidations. */
    trackingHandlePendingKeyInvalidations();

    /* Flush other pending push messages. only when we are not in nested call.
     * So the messages are not interleaved with transaction response. */
    if (!server.execution_nesting)
        listJoin(c->reply, server.pending_push_messages);
}

/* Check if c->cmd exists, fills `err` with details in case it doesn't.
 * Return 1 if exists. */
int commandCheckExistence(client *c, sds *err) {
    if (c->cmd)
        return 1;
    if (!err)
        return 0;
    if (isContainerCommandBySds(c->argv[0]->ptr)) {
        /* If we can't find the command but argv[0] by itself is a command
         * it means we're dealing with an invalid subcommand. Print Help. */
        sds cmd = sdsnew((char *)c->argv[0]->ptr);
        sdstoupper(cmd);
        *err = sdsnew(NULL);
        *err = sdscatprintf(*err, "unknown subcommand '%.128s'. Try %s HELP.",
                            (char *)c->argv[1]->ptr, cmd);
        sdsfree(cmd);
    } else {
        sds args = sdsempty();
        int i;
        for (i=1; i < c->argc && sdslen(args) < 128; i++)
            args = sdscatprintf(args, "'%.*s' ", 128-(int)sdslen(args), (char*)c->argv[i]->ptr);
        *err = sdsnew(NULL);
        *err = sdscatprintf(*err, "unknown command '%.128s', with args beginning with: %s",
                            (char*)c->argv[0]->ptr, args);
        sdsfree(args);
    }
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted (The args come from the user, they may contain any character). */
    sdsmapchars(*err, "\r\n", "  ",  2);
    return 0;
}

/* Check if c->argc is valid for c->cmd, fills `err` with details in case it isn't.
 * Return 1 if valid. */
int commandCheckArity(client *c, sds *err) {
    if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) ||
        (c->argc < -c->cmd->arity))
    {
        if (err) {
            *err = sdsnew(NULL);
            *err = sdscatprintf(*err, "wrong number of arguments for '%s' command", c->cmd->fullname);
        }
        return 0;
    }

    return 1;
}

/* If we're executing a script, try to extract a set of command flags from
 * it, in case it declared them. Note this is just an attempt, we don't yet
 * know the script command is well formed.*/
uint64_t getCommandFlags(client *c) {
    uint64_t cmd_flags = c->cmd->flags;

    if (c->cmd->proc == fcallCommand || c->cmd->proc == fcallroCommand) {
        cmd_flags = fcallGetCommandFlags(c, cmd_flags);
    } else if (c->cmd->proc == evalCommand || c->cmd->proc == evalRoCommand ||
               c->cmd->proc == evalShaCommand || c->cmd->proc == evalShaRoCommand)
    {
        cmd_flags = evalGetCommandFlags(c, cmd_flags);
    }

    return cmd_flags;
}

/* If this function gets called we already read a whole
 * command, arguments are in the client argv/argc fields.
 * processCommand() execute the command or prepare the
 * server for a bulk read from the client.
 *
 * If C_OK is returned the client is still alive and valid and
 * other operations can be performed by the caller. Otherwise
 * if C_ERR is returned the client was destroyed (i.e. after QUIT). */
int processCommand(client *c) {
    if (!scriptIsTimedout()) {
        /* Both EXEC and scripts call call() directly so there should be
         * no way in_exec or scriptIsRunning() is 1.
         * That is unless lua_timedout, in which case client may run
         * some commands. */
        serverAssert(!server.in_exec);
        serverAssert(!scriptIsRunning());
    }

    /* in case we are starting to ProcessCommand and we already have a command we assume
     * this is a reprocessing of this command, so we do not want to perform some of the actions again. */
    int client_reprocessing_command = c->cmd ? 1 : 0;

    /* only run command filter if not reprocessing command */
    if (!client_reprocessing_command) {
        moduleCallCommandFilters(c);
        reqresAppendRequest(c);
    }

    /* Handle possible security attacks. */
    if (!strcasecmp(c->argv[0]->ptr,"host:") || !strcasecmp(c->argv[0]->ptr,"post")) {
        securityWarningCommand(c);
        return C_ERR;
    }

    /* If we're inside a module blocked context yielding that wants to avoid
     * processing clients, postpone the command. */
    if (server.busy_module_yield_flags != BUSY_MODULE_YIELD_NONE &&
        !(server.busy_module_yield_flags & BUSY_MODULE_YIELD_CLIENTS))
    {
        blockPostponeClient(c);
        return C_OK;
    }

    /* Now lookup the command and check ASAP about trivial error conditions
     * such as wrong arity, bad command name and so forth.
     * In case we are reprocessing a command after it was blocked,
     * we do not have to repeat the same checks */
    if (!client_reprocessing_command) {
        c->cmd = c->lastcmd = c->realcmd = lookupCommand(c->argv,c->argc);
        sds err;
        if (!commandCheckExistence(c, &err)) {
            rejectCommandSds(c, err);
            return C_OK;
        }
        if (!commandCheckArity(c, &err)) {
            rejectCommandSds(c, err);
            return C_OK;
        }


        /* Check if the command is marked as protected and the relevant configuration allows it */
        if (c->cmd->flags & CMD_PROTECTED) {
            if ((c->cmd->proc == debugCommand && !allowProtectedAction(server.enable_debug_cmd, c)) ||
                (c->cmd->proc == moduleCommand && !allowProtectedAction(server.enable_module_cmd, c)))
            {
                rejectCommandFormat(c,"%s command not allowed. If the %s option is set to \"local\", "
                                      "you can run it from a local connection, otherwise you need to set this option "
                                      "in the configuration file, and then restart the server.",
                                      c->cmd->proc == debugCommand ? "DEBUG" : "MODULE",
                                      c->cmd->proc == debugCommand ? "enable-debug-command" : "enable-module-command");
                return C_OK;

            }
        }
    }

    const uint64_t cmd_flags = getCommandFlags(c);

    int is_read_command = (cmd_flags & CMD_READONLY) ||
                           (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_READONLY));
    int is_write_command = (cmd_flags & CMD_WRITE) ||
                           (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_WRITE));
    int is_denyoom_command = (cmd_flags & CMD_DENYOOM) ||
                             (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_DENYOOM));
    int is_denystale_command = !(cmd_flags & CMD_STALE) ||
                               (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_STALE));
    int is_denyloading_command = !(cmd_flags & CMD_LOADING) ||
                                 (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_LOADING));
    int is_may_replicate_command = (cmd_flags & (CMD_WRITE | CMD_MAY_REPLICATE)) ||
                                   (c->cmd->proc == execCommand && (c->mstate.cmd_flags & (CMD_WRITE | CMD_MAY_REPLICATE)));
    int is_deny_async_loading_command = (cmd_flags & CMD_NO_ASYNC_LOADING) ||
                                        (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_NO_ASYNC_LOADING));
    int obey_client = mustObeyClient(c);

    if (authRequired(c)) {
        /* AUTH and HELLO and no auth commands are valid even in
         * non-authenticated state. */
        if (!(c->cmd->flags & CMD_NO_AUTH)) {
            rejectCommand(c,shared.noautherr);
            return C_OK;
        }
    }

    if (c->flags & CLIENT_MULTI && c->cmd->flags & CMD_NO_MULTI) {
        rejectCommandFormat(c,"Command not allowed inside a transaction");
        return C_OK;
    }

    /* Check if the user can run this command according to the current
     * ACLs. */
    int acl_errpos;
    int acl_retval = ACLCheckAllPerm(c,&acl_errpos);
    if (acl_retval != ACL_OK) {
        addACLLogEntry(c,acl_retval,(c->flags & CLIENT_MULTI) ? ACL_LOG_CTX_MULTI : ACL_LOG_CTX_TOPLEVEL,acl_errpos,NULL,NULL);
        sds msg = getAclErrorMessage(acl_retval, c->user, c->cmd, c->argv[acl_errpos]->ptr, 0);
        rejectCommandFormat(c, "-NOPERM %s", msg);
        sdsfree(msg);
        return C_OK;
    }

    /* If cluster is enabled perform the cluster redirection here.
     * However we don't perform the redirection if:
     * 1) The sender of this command is our master.
     * 2) The command has no key arguments. */
    if (server.cluster_enabled &&
        !mustObeyClient(c) &&
        !(!(c->cmd->flags&CMD_MOVABLE_KEYS) && c->cmd->key_specs_num == 0 &&
          c->cmd->proc != execCommand))
    {
        int error_code;
        clusterNode *n = getNodeByQuery(c,c->cmd,c->argv,c->argc,
                                        &c->slot,cmd_flags,&error_code);
        if (n == NULL || !clusterNodeIsMyself(n)) {
            if (c->cmd->proc == execCommand) {
                discardTransaction(c);
            } else {
                flagTransaction(c);
            }
            clusterRedirectClient(c,n,c->slot,error_code);
            c->duration = 0;
            c->cmd->rejected_calls++;
            return C_OK;
        }
    }

    /* Disconnect some clients if total clients memory is too high. We do this
     * before key eviction, after the last command was executed and consumed
     * some client output buffer memory. */
    evictClients();
    if (server.current_client == NULL) {
        /* If we evicted ourself then abort processing the command */
        return C_ERR;
    }

    /* Handle the maxmemory directive.
     *
     * Note that we do not want to reclaim memory if we are here re-entering
     * the event loop since there is a busy Lua script running in timeout
     * condition, to avoid mixing the propagation of scripts with the
     * propagation of DELs due to eviction. */
    if (server.maxmemory && !isInsideYieldingLongCommand()) {
        int out_of_memory = (performEvictions() == EVICT_FAIL);

        /* performEvictions may evict keys, so we need flush pending tracking
         * invalidation keys. If we don't do this, we may get an invalidation
         * message after we perform operation on the key, where in fact this
         * message belongs to the old value of the key before it gets evicted.*/
        trackingHandlePendingKeyInvalidations();

        /* performEvictions may flush slave output buffers. This may result
         * in a slave, that may be the active client, to be freed. */
        if (server.current_client == NULL) return C_ERR;

        if (out_of_memory && is_denyoom_command) {
            rejectCommand(c, shared.oomerr);
            return C_OK;
        }

        /* Save out_of_memory result at command start, otherwise if we check OOM
         * in the first write within script, memory used by lua stack and
         * arguments might interfere. We need to save it for EXEC and module
         * calls too, since these can call EVAL, but avoid saving it during an
         * interrupted / yielding busy script / module. */
        server.pre_command_oom_state = out_of_memory;
    }

    /* Make sure to use a reasonable amount of memory for client side
     * caching metadata. */
    if (server.tracking_clients) trackingLimitUsedSlots();

    /* Don't accept write commands if there are problems persisting on disk
     * unless coming from our master, in which case check the replica ignore
     * disk write error config to either log or crash. */
    int deny_write_type = writeCommandsDeniedByDiskError();
    if (deny_write_type != DISK_ERROR_TYPE_NONE &&
        (is_write_command || c->cmd->proc == pingCommand))
    {
        if (obey_client) {
            if (!server.repl_ignore_disk_write_error && c->cmd->proc != pingCommand) {
                serverPanic("Replica was unable to write command to disk.");
            } else {
                static mstime_t last_log_time_ms = 0;
                const mstime_t log_interval_ms = 10000;
                if (server.mstime > last_log_time_ms + log_interval_ms) {
                    last_log_time_ms = server.mstime;
                    serverLog(LL_WARNING, "Replica is applying a command even though "
                                          "it is unable to write to disk.");
                }
            }
        } else {
            sds err = writeCommandsGetDiskErrorMessage(deny_write_type);
            /* remove the newline since rejectCommandSds adds it. */
            sdssubstr(err, 0, sdslen(err)-2);
            rejectCommandSds(c, err);
            return C_OK;
        }
    }

    /* Don't accept write commands if there are not enough good slaves and
     * user configured the min-slaves-to-write option. */
    if (is_write_command && !checkGoodReplicasStatus()) {
        rejectCommand(c, shared.noreplicaserr);
        return C_OK;
    }

    /* Don't accept write commands if this is a read only slave. But
     * accept write commands if this is our master. */
    if (server.masterhost && server.repl_slave_ro &&
        !obey_client &&
        is_write_command)
    {
        rejectCommand(c, shared.roslaveerr);
        return C_OK;
    }

    /* Only allow a subset of commands in the context of Pub/Sub if the
     * connection is in RESP2 mode. With RESP3 there are no limits. */
    if ((c->flags & CLIENT_PUBSUB && c->resp == 2) &&
        c->cmd->proc != pingCommand &&
        c->cmd->proc != subscribeCommand &&
        c->cmd->proc != ssubscribeCommand &&
        c->cmd->proc != unsubscribeCommand &&
        c->cmd->proc != sunsubscribeCommand &&
        c->cmd->proc != psubscribeCommand &&
        c->cmd->proc != punsubscribeCommand &&
        c->cmd->proc != quitCommand &&
        c->cmd->proc != resetCommand) {
        rejectCommandFormat(c,
            "Can't execute '%s': only (P|S)SUBSCRIBE / "
            "(P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context",
            c->cmd->fullname);
        return C_OK;
    }

    /* Only allow commands with flag "t", such as INFO, REPLICAOF and so on,
     * when replica-serve-stale-data is no and we are a replica with a broken
     * link with master. */
    if (server.masterhost && server.repl_state != REPL_STATE_CONNECTED &&
        server.repl_serve_stale_data == 0 &&
        is_denystale_command)
    {
        rejectCommand(c, shared.masterdownerr);
        return C_OK;
    }

    /* Loading DB? Return an error if the command has not the
     * CMD_LOADING flag. */
    if (server.loading && !server.async_loading && is_denyloading_command) {
        rejectCommand(c, shared.loadingerr);
        return C_OK;
    }

    /* During async-loading, block certain commands. */
    if (server.async_loading && is_deny_async_loading_command) {
        rejectCommand(c,shared.loadingerr);
        return C_OK;
    }

    /* when a busy job is being done (script / module)
     * Only allow a limited number of commands.
     * Note that we need to allow the transactions commands, otherwise clients
     * sending a transaction with pipelining without error checking, may have
     * the MULTI plus a few initial commands refused, then the timeout
     * condition resolves, and the bottom-half of the transaction gets
     * executed, see Github PR #7022. */
    if (isInsideYieldingLongCommand() && !(c->cmd->flags & CMD_ALLOW_BUSY)) {
        if (server.busy_module_yield_flags && server.busy_module_yield_reply) {
            rejectCommandFormat(c, "-BUSY %s", server.busy_module_yield_reply);
        } else if (server.busy_module_yield_flags) {
            rejectCommand(c, shared.slowmoduleerr);
        } else if (scriptIsEval()) {
            rejectCommand(c, shared.slowevalerr);
        } else {
            rejectCommand(c, shared.slowscripterr);
        }
        return C_OK;
    }

    /* Prevent a replica from sending commands that access the keyspace.
     * The main objective here is to prevent abuse of client pause check
     * from which replicas are exempt. */
    if ((c->flags & CLIENT_SLAVE) && (is_may_replicate_command || is_write_command || is_read_command)) {
        rejectCommandFormat(c, "Replica can't interact with the keyspace");
        return C_OK;
    }

    /* If the server is paused, block the client until
     * the pause has ended. Replicas are never paused. */
    if (!(c->flags & CLIENT_SLAVE) && 
        ((isPausedActions(PAUSE_ACTION_CLIENT_ALL)) ||
        ((isPausedActions(PAUSE_ACTION_CLIENT_WRITE)) && is_may_replicate_command)))
    {
        blockPostponeClient(c);
        return C_OK;       
    }

    /* Exec the command */
    if (c->flags & CLIENT_MULTI &&
        c->cmd->proc != execCommand &&
        c->cmd->proc != discardCommand &&
        c->cmd->proc != multiCommand &&
        c->cmd->proc != watchCommand &&
        c->cmd->proc != quitCommand &&
        c->cmd->proc != resetCommand)
    {
        queueMultiCommand(c, cmd_flags);
        addReply(c,shared.queued);
    } else {
        int flags = CMD_CALL_FULL;
        if (client_reprocessing_command) flags |= CMD_CALL_REPROCESSING;
        call(c,flags);
        if (listLength(server.ready_keys) && !isInsideYieldingLongCommand())
            handleClientsBlockedOnKeys();
    }
    return C_OK;
}

/* ====================== Error lookup and execution ===================== */

/* Users who abuse lua error_reply will generate a new error object on each
 * error call, which can make server.errors get bigger and bigger. This will
 * cause the server to block when calling INFO (we also return errorstats by
 * default). To prevent the damage it can cause, when a misuse is detected,
 * we will print the warning log and disable the errorstats to avoid adding
 * more new errors. It can be re-enabled via CONFIG RESETSTAT. */
#define ERROR_STATS_NUMBER 128
void incrementErrorCount(const char *fullerr, size_t namelen) {
    /* errorstats is disabled, return ASAP. */
    if (!server.errors_enabled) return;

    void *result;
    if (!raxFind(server.errors,(unsigned char*)fullerr,namelen,&result)) {
        if (server.errors->numele >= ERROR_STATS_NUMBER) {
            sds errors = sdsempty();
            raxIterator ri;
            raxStart(&ri, server.errors);
            raxSeek(&ri, "^", NULL, 0);
            while (raxNext(&ri)) {
                char *tmpsafe;
                errors = sdscatlen(errors, getSafeInfoString((char *)ri.key, ri.key_len, &tmpsafe), ri.key_len);
                errors = sdscatlen(errors, ", ", 2);
                if (tmpsafe != NULL) zfree(tmpsafe);
            }
            sdsrange(errors, 0, -3); /* Remove final ", ". */
            raxStop(&ri);

            /* Print the warning log and the contents of server.errors to the log. */
            serverLog(LL_WARNING,
                      "Errorstats stopped adding new errors because the number of "
                      "errors reached the limit, may be misuse of lua error_reply, "
                      "please check INFO ERRORSTATS, this can be re-enabled via "
                      "CONFIG RESETSTAT.");
            serverLog(LL_WARNING, "Current errors code list: %s", errors);
            sdsfree(errors);

            /* Reset the errors and add a single element to indicate that it is disabled. */
            resetErrorTableStats();
            incrementErrorCount("ERRORSTATS_DISABLED", 19);
            server.errors_enabled = 0;
            return;
        }

        struct redisError *error = zmalloc(sizeof(*error));
        error->count = 1;
        raxInsert(server.errors,(unsigned char*)fullerr,namelen,error,NULL);
    } else {
        struct redisError *error = result;
        error->count++;
    }
}

/*================================== Shutdown =============================== */

/* Close listening sockets. Also unlink the unix domain socket if
 * unlink_unix_socket is non-zero. */
void closeListeningSockets(int unlink_unix_socket) {
    int j;

    for (int i = 0; i < CONN_TYPE_MAX; i++) {
        connListener *listener = &server.listeners[i];
        if (listener->ct == NULL)
            continue;

        for (j = 0; j < listener->count; j++) close(listener->fd[j]);
    }

    if (server.cluster_enabled)
        for (j = 0; j < server.clistener.count; j++) close(server.clistener.fd[j]);
    if (unlink_unix_socket && server.unixsocket) {
        serverLog(LL_NOTICE,"Removing the unix socket file.");
        if (unlink(server.unixsocket) != 0)
            serverLog(LL_WARNING,"Error removing the unix socket file: %s",strerror(errno));
    }
}

/* Prepare for shutting down the server. Flags:
 *
 * - SHUTDOWN_SAVE: Save a database dump even if the server is configured not to
 *   save any dump.
 *
 * - SHUTDOWN_NOSAVE: Don't save any database dump even if the server is
 *   configured to save one.
 *
 * - SHUTDOWN_NOW: Don't wait for replicas to catch up before shutting down.
 *
 * - SHUTDOWN_FORCE: Ignore errors writing AOF and RDB files on disk, which
 *   would normally prevent a shutdown.
 *
 * Unless SHUTDOWN_NOW is set and if any replicas are lagging behind, C_ERR is
 * returned and server.shutdown_mstime is set to a timestamp to allow a grace
 * period for the replicas to catch up. This is checked and handled by
 * serverCron() which completes the shutdown as soon as possible.
 *
 * If shutting down fails due to errors writing RDB or AOF files, C_ERR is
 * returned and an error is logged. If the flag SHUTDOWN_FORCE is set, these
 * errors are logged but ignored and C_OK is returned.
 *
 * On success, this function returns C_OK and then it's OK to call exit(0). */
int prepareForShutdown(int flags) {
    if (isShutdownInitiated()) return C_ERR;

    /* When SHUTDOWN is called while the server is loading a dataset in
     * memory we need to make sure no attempt is performed to save
     * the dataset on shutdown (otherwise it could overwrite the current DB
     * with half-read data).
     *
     * Also when in Sentinel mode clear the SAVE flag and force NOSAVE. */
    if (server.loading || server.sentinel_mode)
        flags = (flags & ~SHUTDOWN_SAVE) | SHUTDOWN_NOSAVE;

    server.shutdown_flags = flags;

    serverLog(LL_NOTICE,"User requested shutdown...");
    if (server.supervised_mode == SUPERVISED_SYSTEMD)
        redisCommunicateSystemd("STOPPING=1\n");

    /* If we have any replicas, let them catch up the replication offset before
     * we shut down, to avoid data loss. */
    if (!(flags & SHUTDOWN_NOW) &&
        server.shutdown_timeout != 0 &&
        !isReadyToShutdown())
    {
        server.shutdown_mstime = server.mstime + server.shutdown_timeout * 1000;
        if (!isPausedActions(PAUSE_ACTION_REPLICA)) sendGetackToReplicas();
        pauseActions(PAUSE_DURING_SHUTDOWN,
                      LLONG_MAX,
                     PAUSE_ACTIONS_CLIENT_WRITE_SET);
        serverLog(LL_NOTICE, "Waiting for replicas before shutting down.");
        return C_ERR;
    }

    return finishShutdown();
}

static inline int isShutdownInitiated(void) {
    return server.shutdown_mstime != 0;
}

/* Returns 0 if there are any replicas which are lagging in replication which we
 * need to wait for before shutting down. Returns 1 if we're ready to shut
 * down now. */
int isReadyToShutdown(void) {
    if (listLength(server.slaves) == 0) return 1;  /* No replicas. */

    listIter li;
    listNode *ln;
    listRewind(server.slaves, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *replica = listNodeValue(ln);
        if (replica->repl_ack_off != server.master_repl_offset) return 0;
    }
    return 1;
}

static void cancelShutdown(void) {
    server.shutdown_asap = 0;
    server.shutdown_flags = 0;
    server.shutdown_mstime = 0;
    server.last_sig_received = 0;
    replyToClientsBlockedOnShutdown();
    unpauseActions(PAUSE_DURING_SHUTDOWN);
}

/* Returns C_OK if shutdown was aborted and C_ERR if shutdown wasn't ongoing. */
int abortShutdown(void) {
    if (isShutdownInitiated()) {
        cancelShutdown();
    } else if (server.shutdown_asap) {
        /* Signal handler has requested shutdown, but it hasn't been initiated
         * yet. Just clear the flag. */
        server.shutdown_asap = 0;
    } else {
        /* Shutdown neither initiated nor requested. */
        return C_ERR;
    }
    serverLog(LL_NOTICE, "Shutdown manually aborted.");
    return C_OK;
}

/* The final step of the shutdown sequence. Returns C_OK if the shutdown
 * sequence was successful and it's OK to call exit(). If C_ERR is returned,
 * it's not safe to call exit(). */
int finishShutdown(void) {

    int save = server.shutdown_flags & SHUTDOWN_SAVE;
    int nosave = server.shutdown_flags & SHUTDOWN_NOSAVE;
    int force = server.shutdown_flags & SHUTDOWN_FORCE;

    /* Log a warning for each replica that is lagging. */
    listIter replicas_iter;
    listNode *replicas_list_node;
    int num_replicas = 0, num_lagging_replicas = 0;
    listRewind(server.slaves, &replicas_iter);
    while ((replicas_list_node = listNext(&replicas_iter)) != NULL) {
        client *replica = listNodeValue(replicas_list_node);
        num_replicas++;
        if (replica->repl_ack_off != server.master_repl_offset) {
            num_lagging_replicas++;
            long lag = replica->replstate == SLAVE_STATE_ONLINE ?
                time(NULL) - replica->repl_ack_time : 0;
            serverLog(LL_NOTICE,
                      "Lagging replica %s reported offset %lld behind master, lag=%ld, state=%s.",
                      replicationGetSlaveName(replica),
                      server.master_repl_offset - replica->repl_ack_off,
                      lag,
                      replstateToString(replica->replstate));
        }
    }
    if (num_replicas > 0) {
        serverLog(LL_NOTICE,
                  "%d of %d replicas are in sync when shutting down.",
                  num_replicas - num_lagging_replicas,
                  num_replicas);
    }

    /* Kill all the Lua debugger forked sessions. */
    ldbKillForkedSessions();

    /* Kill the saving child if there is a background saving in progress.
       We want to avoid race conditions, for instance our saving child may
       overwrite the synchronous saving did by SHUTDOWN. */
    if (server.child_type == CHILD_TYPE_RDB) {
        serverLog(LL_WARNING,"There is a child saving an .rdb. Killing it!");
        killRDBChild();
        /* Note that, in killRDBChild normally has backgroundSaveDoneHandler
         * doing it's cleanup, but in this case this code will not be reached,
         * so we need to call rdbRemoveTempFile which will close fd(in order
         * to unlink file actually) in background thread.
         * The temp rdb file fd may won't be closed when redis exits quickly,
         * but OS will close this fd when process exits. */
        rdbRemoveTempFile(server.child_pid, 0);
    }

    /* Kill module child if there is one. */
    if (server.child_type == CHILD_TYPE_MODULE) {
        serverLog(LL_WARNING,"There is a module fork child. Killing it!");
        TerminateModuleForkChild(server.child_pid,0);
    }

    /* Kill the AOF saving child as the AOF we already have may be longer
     * but contains the full dataset anyway. */
    if (server.child_type == CHILD_TYPE_AOF) {
        /* If we have AOF enabled but haven't written the AOF yet, don't
         * shutdown or else the dataset will be lost. */
        if (server.aof_state == AOF_WAIT_REWRITE) {
            if (force) {
                serverLog(LL_WARNING, "Writing initial AOF. Exit anyway.");
            } else {
                serverLog(LL_WARNING, "Writing initial AOF, can't exit.");
                if (server.supervised_mode == SUPERVISED_SYSTEMD)
                    redisCommunicateSystemd("STATUS=Writing initial AOF, can't exit.\n");
                goto error;
            }
        }
        serverLog(LL_WARNING,
                  "There is a child rewriting the AOF. Killing it!");
        killAppendOnlyChild();
    }
    if (server.aof_state != AOF_OFF) {
        /* Append only file: flush buffers and fsync() the AOF at exit */
        serverLog(LL_NOTICE,"Calling fsync() on the AOF file.");
        flushAppendOnlyFile(1);
        if (redis_fsync(server.aof_fd) == -1) {
            serverLog(LL_WARNING,"Fail to fsync the AOF file: %s.",
                                 strerror(errno));
        }
    }

    /* Create a new RDB file before exiting. */
    if ((server.saveparamslen > 0 && !nosave) || save) {
        serverLog(LL_NOTICE,"Saving the final RDB snapshot before exiting.");
        if (server.supervised_mode == SUPERVISED_SYSTEMD)
            redisCommunicateSystemd("STATUS=Saving the final RDB snapshot\n");
        /* Snapshotting. Perform a SYNC SAVE and exit */
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        /* Keep the page cache since it's likely to restart soon */
        if (rdbSave(SLAVE_REQ_NONE,server.rdb_filename,rsiptr,RDBFLAGS_KEEP_CACHE) != C_OK) {
            /* Ooops.. error saving! The best we can do is to continue
             * operating. Note that if there was a background saving process,
             * in the next cron() Redis will be notified that the background
             * saving aborted, handling special stuff like slaves pending for
             * synchronization... */
            if (force) {
                serverLog(LL_WARNING,"Error trying to save the DB. Exit anyway.");
            } else {
                serverLog(LL_WARNING,"Error trying to save the DB, can't exit.");
                if (server.supervised_mode == SUPERVISED_SYSTEMD)
                    redisCommunicateSystemd("STATUS=Error trying to save the DB, can't exit.\n");
                goto error;
            }
        }
    }

    /* Free the AOF manifest. */
    if (server.aof_manifest) aofManifestFree(server.aof_manifest);

    /* Fire the shutdown modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_SHUTDOWN,0,NULL);

    /* Remove the pid file if possible and needed. */
    if (server.daemonize || server.pidfile) {
        serverLog(LL_NOTICE,"Removing the pid file.");
        unlink(server.pidfile);
    }

    /* Best effort flush of slave output buffers, so that we hopefully
     * send them pending writes. */
    flushSlavesOutputBuffers();

    /* Close the listening sockets. Apparently this allows faster restarts. */
    closeListeningSockets(1);

#if !defined(__sun)
    /* Unlock the cluster config file before shutdown */
    if (server.cluster_enabled && server.cluster_config_file_lock_fd != -1) {
        flock(server.cluster_config_file_lock_fd, LOCK_UN|LOCK_NB);
    }
#endif /* __sun */


    serverLog(LL_WARNING,"%s is now ready to exit, bye bye...",
        server.sentinel_mode ? "Sentinel" : "Redis");
    return C_OK;

error:
    serverLog(LL_WARNING, "Errors trying to shut down the server. Check the logs for more information.");
    cancelShutdown();
    return C_ERR;
}

/*================================== Commands =============================== */

/* Sometimes Redis cannot accept write commands because there is a persistence
 * error with the RDB or AOF file, and Redis is configured in order to stop
 * accepting writes in such situation. This function returns if such a
 * condition is active, and the type of the condition.
 *
 * Function return values:
 *
 * DISK_ERROR_TYPE_NONE:    No problems, we can accept writes.
 * DISK_ERROR_TYPE_AOF:     Don't accept writes: AOF errors.
 * DISK_ERROR_TYPE_RDB:     Don't accept writes: RDB errors.
 */
int writeCommandsDeniedByDiskError(void) {
    if (server.stop_writes_on_bgsave_err &&
        server.saveparamslen > 0 &&
        server.lastbgsave_status == C_ERR)
    {
        return DISK_ERROR_TYPE_RDB;
    } else if (server.aof_state != AOF_OFF) {
        if (server.aof_last_write_status == C_ERR) {
            return DISK_ERROR_TYPE_AOF;
        }
        /* AOF fsync error. */
        int aof_bio_fsync_status;
        atomicGet(server.aof_bio_fsync_status,aof_bio_fsync_status);
        if (aof_bio_fsync_status == C_ERR) {
            atomicGet(server.aof_bio_fsync_errno,server.aof_last_write_errno);
            return DISK_ERROR_TYPE_AOF;
        }
    }

    return DISK_ERROR_TYPE_NONE;
}

sds writeCommandsGetDiskErrorMessage(int error_code) {
    sds ret = NULL;
    if (error_code == DISK_ERROR_TYPE_RDB) {
        ret = sdsdup(shared.bgsaveerr->ptr);
    } else {
        ret = sdscatfmt(sdsempty(),
                "-MISCONF Errors writing to the AOF file: %s\r\n",
                strerror(server.aof_last_write_errno));
    }
    return ret;
}

/* The PING command. It works in a different way if the client is in
 * in Pub/Sub mode. */
void pingCommand(client *c) {
    /* The command takes zero or one arguments. */
    if (c->argc > 2) {
        addReplyErrorArity(c);
        return;
    }

    if (c->flags & CLIENT_PUBSUB && c->resp == 2) {
        addReply(c,shared.mbulkhdr[2]);
        addReplyBulkCBuffer(c,"pong",4);
        if (c->argc == 1)
            addReplyBulkCBuffer(c,"",0);
        else
            addReplyBulk(c,c->argv[1]);
    } else {
        if (c->argc == 1)
            addReply(c,shared.pong);
        else
            addReplyBulk(c,c->argv[1]);
    }
}

void echoCommand(client *c) {
    addReplyBulk(c,c->argv[1]);
}

void timeCommand(client *c) {
    addReplyArrayLen(c,2);
    addReplyBulkLongLong(c, server.unixtime);
    addReplyBulkLongLong(c, server.ustime-((long long)server.unixtime)*1000000);
}

typedef struct replyFlagNames {
    uint64_t flag;
    const char *name;
} replyFlagNames;

/* Helper function to output flags. */
void addReplyCommandFlags(client *c, uint64_t flags, replyFlagNames *replyFlags) {
    int count = 0, j=0;
    /* Count them so we don't have to use deferred reply. */
    while (replyFlags[j].name) {
        if (flags & replyFlags[j].flag)
            count++;
        j++;
    }

    addReplySetLen(c, count);
    j = 0;
    while (replyFlags[j].name) {
        if (flags & replyFlags[j].flag)
            addReplyStatus(c, replyFlags[j].name);
        j++;
    }
}

void addReplyFlagsForCommand(client *c, struct redisCommand *cmd) {
    replyFlagNames flagNames[] = {
        {CMD_WRITE,             "write"},
        {CMD_READONLY,          "readonly"},
        {CMD_DENYOOM,           "denyoom"},
        {CMD_MODULE,            "module"},
        {CMD_ADMIN,             "admin"},
        {CMD_PUBSUB,            "pubsub"},
        {CMD_NOSCRIPT,          "noscript"},
        {CMD_BLOCKING,          "blocking"},
        {CMD_LOADING,           "loading"},
        {CMD_STALE,             "stale"},
        {CMD_SKIP_MONITOR,      "skip_monitor"},
        {CMD_SKIP_SLOWLOG,      "skip_slowlog"},
        {CMD_ASKING,            "asking"},
        {CMD_FAST,              "fast"},
        {CMD_NO_AUTH,           "no_auth"},
        /* {CMD_MAY_REPLICATE,     "may_replicate"},, Hidden on purpose */
        /* {CMD_SENTINEL,          "sentinel"}, Hidden on purpose */
        /* {CMD_ONLY_SENTINEL,     "only_sentinel"}, Hidden on purpose */
        {CMD_NO_MANDATORY_KEYS, "no_mandatory_keys"},
        /* {CMD_PROTECTED,         "protected"}, Hidden on purpose */
        {CMD_NO_ASYNC_LOADING,  "no_async_loading"},
        {CMD_NO_MULTI,          "no_multi"},
        {CMD_MOVABLE_KEYS,      "movablekeys"},
        {CMD_ALLOW_BUSY,        "allow_busy"},
        /* {CMD_TOUCHES_ARBITRARY_KEYS,  "TOUCHES_ARBITRARY_KEYS"}, Hidden on purpose */
        {0,NULL}
    };
    addReplyCommandFlags(c, cmd->flags, flagNames);
}

void addReplyDocFlagsForCommand(client *c, struct redisCommand *cmd) {
    replyFlagNames docFlagNames[] = {
        {CMD_DOC_DEPRECATED,         "deprecated"},
        {CMD_DOC_SYSCMD,             "syscmd"},
        {0,NULL}
    };
    addReplyCommandFlags(c, cmd->doc_flags, docFlagNames);
}

void addReplyFlagsForKeyArgs(client *c, uint64_t flags) {
    replyFlagNames docFlagNames[] = {
        {CMD_KEY_RO,              "RO"},
        {CMD_KEY_RW,              "RW"},
        {CMD_KEY_OW,              "OW"},
        {CMD_KEY_RM,              "RM"},
        {CMD_KEY_ACCESS,          "access"},
        {CMD_KEY_UPDATE,          "update"},
        {CMD_KEY_INSERT,          "insert"},
        {CMD_KEY_DELETE,          "delete"},
        {CMD_KEY_NOT_KEY,         "not_key"},
        {CMD_KEY_INCOMPLETE,      "incomplete"},
        {CMD_KEY_VARIABLE_FLAGS,  "variable_flags"},
        {0,NULL}
    };
    addReplyCommandFlags(c, flags, docFlagNames);
}

/* Must match redisCommandArgType */
const char *ARG_TYPE_STR[] = {
    "string",
    "integer",
    "double",
    "key",
    "pattern",
    "unix-time",
    "pure-token",
    "oneof",
    "block",
};

void addReplyFlagsForArg(client *c, uint64_t flags) {
    replyFlagNames argFlagNames[] = {
        {CMD_ARG_OPTIONAL,          "optional"},
        {CMD_ARG_MULTIPLE,          "multiple"},
        {CMD_ARG_MULTIPLE_TOKEN,    "multiple_token"},
        {0,NULL}
    };
    addReplyCommandFlags(c, flags, argFlagNames);
}

void addReplyCommandArgList(client *c, struct redisCommandArg *args, int num_args) {
    addReplyArrayLen(c, num_args);
    for (int j = 0; j<num_args; j++) {
        /* Count our reply len so we don't have to use deferred reply. */
        int has_display_text = 1;
        long maplen = 2;
        if (args[j].key_spec_index != -1) maplen++;
        if (args[j].token) maplen++;
        if (args[j].summary) maplen++;
        if (args[j].since) maplen++;
        if (args[j].deprecated_since) maplen++;
        if (args[j].flags) maplen++;
        if (args[j].type == ARG_TYPE_ONEOF || args[j].type == ARG_TYPE_BLOCK) {
            has_display_text = 0;
            maplen++;
        }
        if (has_display_text) maplen++;
        addReplyMapLen(c, maplen);

        addReplyBulkCString(c, "name");
        addReplyBulkCString(c, args[j].name);

        addReplyBulkCString(c, "type");
        addReplyBulkCString(c, ARG_TYPE_STR[args[j].type]);

        if (has_display_text) {
            addReplyBulkCString(c, "display_text");
            addReplyBulkCString(c, args[j].display_text ? args[j].display_text : args[j].name);
        }
        if (args[j].key_spec_index != -1) {
            addReplyBulkCString(c, "key_spec_index");
            addReplyLongLong(c, args[j].key_spec_index);
        }
        if (args[j].token) {
            addReplyBulkCString(c, "token");
            addReplyBulkCString(c, args[j].token);
        }
        if (args[j].summary) {
            addReplyBulkCString(c, "summary");
            addReplyBulkCString(c, args[j].summary);
        }
        if (args[j].since) {
            addReplyBulkCString(c, "since");
            addReplyBulkCString(c, args[j].since);
        }
        if (args[j].deprecated_since) {
            addReplyBulkCString(c, "deprecated_since");
            addReplyBulkCString(c, args[j].deprecated_since);
        }
        if (args[j].flags) {
            addReplyBulkCString(c, "flags");
            addReplyFlagsForArg(c, args[j].flags);
        }
        if (args[j].type == ARG_TYPE_ONEOF || args[j].type == ARG_TYPE_BLOCK) {
            addReplyBulkCString(c, "arguments");
            addReplyCommandArgList(c, args[j].subargs, args[j].num_args);
        }
    }
}

#ifdef LOG_REQ_RES

void addReplyJson(client *c, struct jsonObject *rs) {
    addReplyMapLen(c, rs->length);

    for (int i = 0; i < rs->length; i++) {
        struct jsonObjectElement *curr = &rs->elements[i];
        addReplyBulkCString(c, curr->key);
        switch (curr->type) {
        case (JSON_TYPE_BOOLEAN):
            addReplyBool(c, curr->value.boolean);
            break;
        case (JSON_TYPE_INTEGER):
            addReplyLongLong(c, curr->value.integer);
            break;
        case (JSON_TYPE_STRING):
            addReplyBulkCString(c, curr->value.string);
            break;
        case (JSON_TYPE_OBJECT):
            addReplyJson(c, curr->value.object);
            break;
        case (JSON_TYPE_ARRAY):
            addReplyArrayLen(c, curr->value.array.length);
            for (int k = 0; k < curr->value.array.length; k++) {
                struct jsonObject *object = curr->value.array.objects[k];
                addReplyJson(c, object);
            }
            break;
        default:
            serverPanic("Invalid JSON type %d", curr->type);
        }
    }
}

#endif

void addReplyCommandHistory(client *c, struct redisCommand *cmd) {
    addReplySetLen(c, cmd->num_history);
    for (int j = 0; j<cmd->num_history; j++) {
        addReplyArrayLen(c, 2);
        addReplyBulkCString(c, cmd->history[j].since);
        addReplyBulkCString(c, cmd->history[j].changes);
    }
}

void addReplyCommandTips(client *c, struct redisCommand *cmd) {
    addReplySetLen(c, cmd->num_tips);
    for (int j = 0; j<cmd->num_tips; j++) {
        addReplyBulkCString(c, cmd->tips[j]);
    }
}

void addReplyCommandKeySpecs(client *c, struct redisCommand *cmd) {
    addReplySetLen(c, cmd->key_specs_num);
    for (int i = 0; i < cmd->key_specs_num; i++) {
        int maplen = 3;
        if (cmd->key_specs[i].notes) maplen++;

        addReplyMapLen(c, maplen);

        if (cmd->key_specs[i].notes) {
            addReplyBulkCString(c, "notes");
            addReplyBulkCString(c,cmd->key_specs[i].notes);
        }

        addReplyBulkCString(c, "flags");
        addReplyFlagsForKeyArgs(c,cmd->key_specs[i].flags);

        addReplyBulkCString(c, "begin_search");
        switch (cmd->key_specs[i].begin_search_type) {
            case KSPEC_BS_UNKNOWN:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "unknown");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 0);
                break;
            case KSPEC_BS_INDEX:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "index");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 1);
                addReplyBulkCString(c, "index");
                addReplyLongLong(c, cmd->key_specs[i].bs.index.pos);
                break;
            case KSPEC_BS_KEYWORD:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "keyword");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "keyword");
                addReplyBulkCString(c, cmd->key_specs[i].bs.keyword.keyword);
                addReplyBulkCString(c, "startfrom");
                addReplyLongLong(c, cmd->key_specs[i].bs.keyword.startfrom);
                break;
            default:
                serverPanic("Invalid begin_search key spec type %d", cmd->key_specs[i].begin_search_type);
        }

        addReplyBulkCString(c, "find_keys");
        switch (cmd->key_specs[i].find_keys_type) {
            case KSPEC_FK_UNKNOWN:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "unknown");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 0);
                break;
            case KSPEC_FK_RANGE:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "range");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 3);
                addReplyBulkCString(c, "lastkey");
                addReplyLongLong(c, cmd->key_specs[i].fk.range.lastkey);
                addReplyBulkCString(c, "keystep");
                addReplyLongLong(c, cmd->key_specs[i].fk.range.keystep);
                addReplyBulkCString(c, "limit");
                addReplyLongLong(c, cmd->key_specs[i].fk.range.limit);
                break;
            case KSPEC_FK_KEYNUM:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "keynum");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 3);
                addReplyBulkCString(c, "keynumidx");
                addReplyLongLong(c, cmd->key_specs[i].fk.keynum.keynumidx);
                addReplyBulkCString(c, "firstkey");
                addReplyLongLong(c, cmd->key_specs[i].fk.keynum.firstkey);
                addReplyBulkCString(c, "keystep");
                addReplyLongLong(c, cmd->key_specs[i].fk.keynum.keystep);
                break;
            default:
                serverPanic("Invalid find_keys key spec type %d", cmd->key_specs[i].begin_search_type);
        }
    }
}

/* Reply with an array of sub-command using the provided reply callback. */
void addReplyCommandSubCommands(client *c, struct redisCommand *cmd, void (*reply_function)(client*, struct redisCommand*), int use_map) {
    if (!cmd->subcommands_dict) {
        addReplySetLen(c, 0);
        return;
    }

    if (use_map)
        addReplyMapLen(c, dictSize(cmd->subcommands_dict));
    else
        addReplyArrayLen(c, dictSize(cmd->subcommands_dict));
    dictEntry *de;
    dictIterator *di = dictGetSafeIterator(cmd->subcommands_dict);
    while((de = dictNext(di)) != NULL) {
        struct redisCommand *sub = (struct redisCommand *)dictGetVal(de);
        if (use_map)
            addReplyBulkCBuffer(c, sub->fullname, sdslen(sub->fullname));
        reply_function(c, sub);
    }
    dictReleaseIterator(di);
}

/* Output the representation of a Redis command. Used by the COMMAND command and COMMAND INFO. */
void addReplyCommandInfo(client *c, struct redisCommand *cmd) {
    if (!cmd) {
        addReplyNull(c);
    } else {
        int firstkey = 0, lastkey = 0, keystep = 0;
        if (cmd->legacy_range_key_spec.begin_search_type != KSPEC_BS_INVALID) {
            firstkey = cmd->legacy_range_key_spec.bs.index.pos;
            lastkey = cmd->legacy_range_key_spec.fk.range.lastkey;
            if (lastkey >= 0)
                lastkey += firstkey;
            keystep = cmd->legacy_range_key_spec.fk.range.keystep;
        }

        addReplyArrayLen(c, 10);
        addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
        addReplyLongLong(c, cmd->arity);
        addReplyFlagsForCommand(c, cmd);
        addReplyLongLong(c, firstkey);
        addReplyLongLong(c, lastkey);
        addReplyLongLong(c, keystep);
        addReplyCommandCategories(c, cmd);
        addReplyCommandTips(c, cmd);
        addReplyCommandKeySpecs(c, cmd);
        addReplyCommandSubCommands(c, cmd, addReplyCommandInfo, 0);
    }
}

/* Output the representation of a Redis command. Used by the COMMAND DOCS. */
void addReplyCommandDocs(client *c, struct redisCommand *cmd) {
    /* Count our reply len so we don't have to use deferred reply. */
    long maplen = 1;
    if (cmd->summary) maplen++;
    if (cmd->since) maplen++;
    if (cmd->flags & CMD_MODULE) maplen++;
    if (cmd->complexity) maplen++;
    if (cmd->doc_flags) maplen++;
    if (cmd->deprecated_since) maplen++;
    if (cmd->replaced_by) maplen++;
    if (cmd->history) maplen++;
#ifdef LOG_REQ_RES
    if (cmd->reply_schema) maplen++;
#endif
    if (cmd->args) maplen++;
    if (cmd->subcommands_dict) maplen++;
    addReplyMapLen(c, maplen);

    if (cmd->summary) {
        addReplyBulkCString(c, "summary");
        addReplyBulkCString(c, cmd->summary);
    }
    if (cmd->since) {
        addReplyBulkCString(c, "since");
        addReplyBulkCString(c, cmd->since);
    }

    /* Always have the group, for module commands the group is always "module". */
    addReplyBulkCString(c, "group");
    addReplyBulkCString(c, commandGroupStr(cmd->group));

    if (cmd->complexity) {
        addReplyBulkCString(c, "complexity");
        addReplyBulkCString(c, cmd->complexity);
    }
    if (cmd->flags & CMD_MODULE) {
        addReplyBulkCString(c, "module");
        addReplyBulkCString(c, moduleNameFromCommand(cmd));
    }
    if (cmd->doc_flags) {
        addReplyBulkCString(c, "doc_flags");
        addReplyDocFlagsForCommand(c, cmd);
    }
    if (cmd->deprecated_since) {
        addReplyBulkCString(c, "deprecated_since");
        addReplyBulkCString(c, cmd->deprecated_since);
    }
    if (cmd->replaced_by) {
        addReplyBulkCString(c, "replaced_by");
        addReplyBulkCString(c, cmd->replaced_by);
    }
    if (cmd->history) {
        addReplyBulkCString(c, "history");
        addReplyCommandHistory(c, cmd);
    }
#ifdef LOG_REQ_RES
    if (cmd->reply_schema) {
        addReplyBulkCString(c, "reply_schema");
        addReplyJson(c, cmd->reply_schema);
    }
#endif
    if (cmd->args) {
        addReplyBulkCString(c, "arguments");
        addReplyCommandArgList(c, cmd->args, cmd->num_args);
    }
    if (cmd->subcommands_dict) {
        addReplyBulkCString(c, "subcommands");
        addReplyCommandSubCommands(c, cmd, addReplyCommandDocs, 1);
    }
}

/* Helper for COMMAND GETKEYS and GETKEYSANDFLAGS */
void getKeysSubcommandImpl(client *c, int with_flags) {
    struct redisCommand *cmd = lookupCommand(c->argv+2,c->argc-2);
    getKeysResult result = GETKEYS_RESULT_INIT;
    int j;

    if (!cmd) {
        addReplyError(c,"Invalid command specified");
        return;
    } else if (!doesCommandHaveKeys(cmd)) {
        addReplyError(c,"The command has no key arguments");
        return;
    } else if ((cmd->arity > 0 && cmd->arity != c->argc-2) ||
               ((c->argc-2) < -cmd->arity))
    {
        addReplyError(c,"Invalid number of arguments specified for command");
        return;
    }

    if (!getKeysFromCommandWithSpecs(cmd,c->argv+2,c->argc-2,GET_KEYSPEC_DEFAULT,&result)) {
        if (cmd->flags & CMD_NO_MANDATORY_KEYS) {
            addReplyArrayLen(c,0);
        } else {
            addReplyError(c,"Invalid arguments specified for command");
        }
    } else {
        addReplyArrayLen(c,result.numkeys);
        for (j = 0; j < result.numkeys; j++) {
            if (!with_flags) {
                addReplyBulk(c,c->argv[result.keys[j].pos+2]);
            } else {
                addReplyArrayLen(c,2);
                addReplyBulk(c,c->argv[result.keys[j].pos+2]);
                addReplyFlagsForKeyArgs(c,result.keys[j].flags);
            }
        }
    }
    getKeysFreeResult(&result);
}

/* COMMAND GETKEYSANDFLAGS cmd arg1 arg2 ... */
void commandGetKeysAndFlagsCommand(client *c) {
    getKeysSubcommandImpl(c, 1);
}

/* COMMAND GETKEYS cmd arg1 arg2 ... */
void getKeysSubcommand(client *c) {
    getKeysSubcommandImpl(c, 0);
}

/* COMMAND (no args) */
void commandCommand(client *c) {
    dictIterator *di;
    dictEntry *de;

    addReplyArrayLen(c, dictSize(server.commands));
    di = dictGetIterator(server.commands);
    while ((de = dictNext(di)) != NULL) {
        addReplyCommandInfo(c, dictGetVal(de));
    }
    dictReleaseIterator(di);
}

/* COMMAND COUNT */
void commandCountCommand(client *c) {
    addReplyLongLong(c, dictSize(server.commands));
}

typedef enum {
    COMMAND_LIST_FILTER_MODULE,
    COMMAND_LIST_FILTER_ACLCAT,
    COMMAND_LIST_FILTER_PATTERN,
} commandListFilterType;

typedef struct {
    commandListFilterType type;
    sds arg;
    struct {
        int valid;
        union {
            uint64_t aclcat;
            void *module_handle;
        } u;
    } cache;
} commandListFilter;

int shouldFilterFromCommandList(struct redisCommand *cmd, commandListFilter *filter) {
    switch (filter->type) {
        case (COMMAND_LIST_FILTER_MODULE):
            if (!filter->cache.valid) {
                filter->cache.u.module_handle = moduleGetHandleByName(filter->arg);
                filter->cache.valid = 1;
            }
            return !moduleIsModuleCommand(filter->cache.u.module_handle, cmd);
        case (COMMAND_LIST_FILTER_ACLCAT): {
            if (!filter->cache.valid) {
                filter->cache.u.aclcat = ACLGetCommandCategoryFlagByName(filter->arg);
                filter->cache.valid = 1;
            }
            uint64_t cat = filter->cache.u.aclcat;
            if (cat == 0)
                return 1; /* Invalid ACL category */
            return (!(cmd->acl_categories & cat));
            break;
        }
        case (COMMAND_LIST_FILTER_PATTERN):
            return !stringmatchlen(filter->arg, sdslen(filter->arg), cmd->fullname, sdslen(cmd->fullname), 1);
        default:
            serverPanic("Invalid filter type %d", filter->type);
    }
}

/* COMMAND LIST FILTERBY (MODULE <module-name>|ACLCAT <cat>|PATTERN <pattern>) */
void commandListWithFilter(client *c, dict *commands, commandListFilter filter, int *numcmds) {
    dictEntry *de;
    dictIterator *di = dictGetIterator(commands);

    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *cmd = dictGetVal(de);
        if (!shouldFilterFromCommandList(cmd,&filter)) {
            addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
            (*numcmds)++;
        }

        if (cmd->subcommands_dict) {
            commandListWithFilter(c, cmd->subcommands_dict, filter, numcmds);
        }
    }
    dictReleaseIterator(di);
}

/* COMMAND LIST */
void commandListWithoutFilter(client *c, dict *commands, int *numcmds) {
    dictEntry *de;
    dictIterator *di = dictGetIterator(commands);

    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *cmd = dictGetVal(de);
        addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
        (*numcmds)++;

        if (cmd->subcommands_dict) {
            commandListWithoutFilter(c, cmd->subcommands_dict, numcmds);
        }
    }
    dictReleaseIterator(di);
}

/* COMMAND LIST [FILTERBY (MODULE <module-name>|ACLCAT <cat>|PATTERN <pattern>)] */
void commandListCommand(client *c) {

    /* Parse options. */
    int i = 2, got_filter = 0;
    commandListFilter filter = {0};
    for (; i < c->argc; i++) {
        int moreargs = (c->argc-1) - i; /* Number of additional arguments. */
        char *opt = c->argv[i]->ptr;
        if (!strcasecmp(opt,"filterby") && moreargs == 2) {
            char *filtertype = c->argv[i+1]->ptr;
            if (!strcasecmp(filtertype,"module")) {
                filter.type = COMMAND_LIST_FILTER_MODULE;
            } else if (!strcasecmp(filtertype,"aclcat")) {
                filter.type = COMMAND_LIST_FILTER_ACLCAT;
            } else if (!strcasecmp(filtertype,"pattern")) {
                filter.type = COMMAND_LIST_FILTER_PATTERN;
            } else {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
            got_filter = 1;
            filter.arg = c->argv[i+2]->ptr;
            i += 2;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }

    int numcmds = 0;
    void *replylen = addReplyDeferredLen(c);

    if (got_filter) {
        commandListWithFilter(c, server.commands, filter, &numcmds);
    } else {
        commandListWithoutFilter(c, server.commands, &numcmds);
    }

    setDeferredArrayLen(c,replylen,numcmds);
}

/* COMMAND INFO [<command-name> ...] */
void commandInfoCommand(client *c) {
    int i;

    if (c->argc == 2) {
        dictIterator *di;
        dictEntry *de;
        addReplyArrayLen(c, dictSize(server.commands));
        di = dictGetIterator(server.commands);
        while ((de = dictNext(di)) != NULL) {
            addReplyCommandInfo(c, dictGetVal(de));
        }
        dictReleaseIterator(di);
    } else {
        addReplyArrayLen(c, c->argc-2);
        for (i = 2; i < c->argc; i++) {
            addReplyCommandInfo(c, lookupCommandBySds(c->argv[i]->ptr));
        }
    }
}

/* COMMAND DOCS [command-name [command-name ...]] */
void commandDocsCommand(client *c) {
    int i;
    if (c->argc == 2) {
        /* Reply with an array of all commands */
        dictIterator *di;
        dictEntry *de;
        addReplyMapLen(c, dictSize(server.commands));
        di = dictGetIterator(server.commands);
        while ((de = dictNext(di)) != NULL) {
            struct redisCommand *cmd = dictGetVal(de);
            addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
            addReplyCommandDocs(c, cmd);
        }
        dictReleaseIterator(di);
    } else {
        /* Reply with an array of the requested commands (if we find them) */
        int numcmds = 0;
        void *replylen = addReplyDeferredLen(c);
        for (i = 2; i < c->argc; i++) {
            struct redisCommand *cmd = lookupCommandBySds(c->argv[i]->ptr);
            if (!cmd)
                continue;
            addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
            addReplyCommandDocs(c, cmd);
            numcmds++;
        }
        setDeferredMapLen(c,replylen,numcmds);
    }
}

/* COMMAND GETKEYS arg0 arg1 arg2 ... */
void commandGetKeysCommand(client *c) {
    getKeysSubcommand(c);
}

/* COMMAND HELP */
void commandHelpCommand(client *c) {
    const char *help[] = {
"(no subcommand)",
"    Return details about all Redis commands.",
"COUNT",
"    Return the total number of commands in this Redis server.",
"LIST",
"    Return a list of all commands in this Redis server.",
"INFO [<command-name> ...]",
"    Return details about multiple Redis commands.",
"    If no command names are given, documentation details for all",
"    commands are returned.",
"DOCS [<command-name> ...]",
"    Return documentation details about multiple Redis commands.",
"    If no command names are given, documentation details for all",
"    commands are returned.",
"GETKEYS <full-command>",
"    Return the keys from a full Redis command.",
"GETKEYSANDFLAGS <full-command>",
"    Return the keys and the access flags from a full Redis command.",
NULL
    };

    addReplyHelp(c, help);
}

/* Convert an amount of bytes into a human readable string in the form
 * of 100B, 2G, 100M, 4K, and so forth. */
void bytesToHuman(char *s, size_t size, unsigned long long n) {
    double d;

    if (n < 1024) {
        /* Bytes */
        snprintf(s,size,"%lluB",n);
    } else if (n < (1024*1024)) {
        d = (double)n/(1024);
        snprintf(s,size,"%.2fK",d);
    } else if (n < (1024LL*1024*1024)) {
        d = (double)n/(1024*1024);
        snprintf(s,size,"%.2fM",d);
    } else if (n < (1024LL*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024);
        snprintf(s,size,"%.2fG",d);
    } else if (n < (1024LL*1024*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024*1024);
        snprintf(s,size,"%.2fT",d);
    } else if (n < (1024LL*1024*1024*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024*1024*1024);
        snprintf(s,size,"%.2fP",d);
    } else {
        /* Let's hope we never need this */
        snprintf(s,size,"%lluB",n);
    }
}

/* Fill percentile distribution of latencies. */
sds fillPercentileDistributionLatencies(sds info, const char* histogram_name, struct hdr_histogram* histogram) {
    info = sdscatfmt(info,"latency_percentiles_usec_%s:",histogram_name);
    for (int j = 0; j < server.latency_tracking_info_percentiles_len; j++) {
        char fbuf[128];
        size_t len = snprintf(fbuf, sizeof(fbuf), "%f", server.latency_tracking_info_percentiles[j]);
        trimDoubleString(fbuf, len);
        info = sdscatprintf(info,"p%s=%.3f", fbuf,
            ((double)hdr_value_at_percentile(histogram,server.latency_tracking_info_percentiles[j]))/1000.0f);
        if (j != server.latency_tracking_info_percentiles_len-1)
            info = sdscatlen(info,",",1);
        }
    info = sdscatprintf(info,"\r\n");
    return info;
}

const char *replstateToString(int replstate) {
    switch (replstate) {
    case SLAVE_STATE_WAIT_BGSAVE_START:
    case SLAVE_STATE_WAIT_BGSAVE_END:
        return "wait_bgsave";
    case SLAVE_STATE_SEND_BULK:
        return "send_bulk";
    case SLAVE_STATE_ONLINE:
        return "online";
    default:
        return "";
    }
}

/* Characters we sanitize on INFO output to maintain expected format. */
static char unsafe_info_chars[] = "#:\n\r";
static char unsafe_info_chars_substs[] = "____";   /* Must be same length as above */

/* Returns a sanitized version of s that contains no unsafe info string chars.
 * If no unsafe characters are found, simply returns s. Caller needs to
 * free tmp if it is non-null on return.
 */
const char *getSafeInfoString(const char *s, size_t len, char **tmp) {
    *tmp = NULL;
    if (mempbrk(s, len, unsafe_info_chars,sizeof(unsafe_info_chars)-1)
        == NULL) return s;
    char *new = *tmp = zmalloc(len + 1);
    memcpy(new, s, len);
    new[len] = '\0';
    return memmapchars(new, len, unsafe_info_chars, unsafe_info_chars_substs,
                       sizeof(unsafe_info_chars)-1);
}

sds genRedisInfoStringCommandStats(sds info, dict *commands) {
    struct redisCommand *c;
    dictEntry *de;
    dictIterator *di;
    di = dictGetSafeIterator(commands);
    while((de = dictNext(di)) != NULL) {
        char *tmpsafe;
        c = (struct redisCommand *) dictGetVal(de);
        if (c->calls || c->failed_calls || c->rejected_calls) {
            info = sdscatprintf(info,
                "cmdstat_%s:calls=%lld,usec=%lld,usec_per_call=%.2f"
                ",rejected_calls=%lld,failed_calls=%lld\r\n",
                getSafeInfoString(c->fullname, sdslen(c->fullname), &tmpsafe), c->calls, c->microseconds,
                (c->calls == 0) ? 0 : ((float)c->microseconds/c->calls),
                c->rejected_calls, c->failed_calls);
            if (tmpsafe != NULL) zfree(tmpsafe);
        }
        if (c->subcommands_dict) {
            info = genRedisInfoStringCommandStats(info, c->subcommands_dict);
        }
    }
    dictReleaseIterator(di);

    return info;
}

/* Writes the ACL metrics to the info */
sds genRedisInfoStringACLStats(sds info) {
    info = sdscatprintf(info,
         "acl_access_denied_auth:%lld\r\n"
         "acl_access_denied_cmd:%lld\r\n"
         "acl_access_denied_key:%lld\r\n"
         "acl_access_denied_channel:%lld\r\n",
         server.acl_info.user_auth_failures,
         server.acl_info.invalid_cmd_accesses,
         server.acl_info.invalid_key_accesses,
         server.acl_info.invalid_channel_accesses);
    return info;
}

sds genRedisInfoStringLatencyStats(sds info, dict *commands) {
    struct redisCommand *c;
    dictEntry *de;
    dictIterator *di;
    di = dictGetSafeIterator(commands);
    while((de = dictNext(di)) != NULL) {
        char *tmpsafe;
        c = (struct redisCommand *) dictGetVal(de);
        if (c->latency_histogram) {
            info = fillPercentileDistributionLatencies(info,
                getSafeInfoString(c->fullname, sdslen(c->fullname), &tmpsafe),
                c->latency_histogram);
            if (tmpsafe != NULL) zfree(tmpsafe);
        }
        if (c->subcommands_dict) {
            info = genRedisInfoStringLatencyStats(info, c->subcommands_dict);
        }
    }
    dictReleaseIterator(di);

    return info;
}

/* Takes a null terminated sections list, and adds them to the dict. */
void addInfoSectionsToDict(dict *section_dict, char **sections) {
    while (*sections) {
        sds section = sdsnew(*sections);
        if (dictAdd(section_dict, section, NULL)==DICT_ERR)
            sdsfree(section);
        sections++;
    }
}

/* Cached copy of the default sections, as an optimization. */
static dict *cached_default_info_sections = NULL;

void releaseInfoSectionDict(dict *sec) {
    if (sec != cached_default_info_sections)
        dictRelease(sec);
}

/* Create a dictionary with unique section names to be used by genRedisInfoString.
 * 'argv' and 'argc' are list of arguments for INFO.
 * 'defaults' is an optional null terminated list of default sections.
 * 'out_all' and 'out_everything' are optional.
 * The resulting dictionary should be released with releaseInfoSectionDict. */
dict *genInfoSectionDict(robj **argv, int argc, char **defaults, int *out_all, int *out_everything) {
    char *default_sections[] = {
        "server", "clients", "memory", "persistence", "stats", "replication",
        "cpu", "module_list", "errorstats", "cluster", "keyspace", NULL};
    if (!defaults)
        defaults = default_sections;

    if (argc == 0) {
        /* In this case we know the dict is not gonna be modified, so we cache
         * it as an optimization for a common case. */
        if (cached_default_info_sections)
            return cached_default_info_sections;
        cached_default_info_sections = dictCreate(&stringSetDictType);
        dictExpand(cached_default_info_sections, 16);
        addInfoSectionsToDict(cached_default_info_sections, defaults);
        return cached_default_info_sections;
    }

    dict *section_dict = dictCreate(&stringSetDictType);
    dictExpand(section_dict, min(argc,16));
    for (int i = 0; i < argc; i++) {
        if (!strcasecmp(argv[i]->ptr,"default")) {
            addInfoSectionsToDict(section_dict, defaults);
        } else if (!strcasecmp(argv[i]->ptr,"all")) {
            if (out_all) *out_all = 1;
        } else if (!strcasecmp(argv[i]->ptr,"everything")) {
            if (out_everything) *out_everything = 1;
            if (out_all) *out_all = 1;
        } else {
            sds section = sdsnew(argv[i]->ptr);
            if (dictAdd(section_dict, section, NULL) != DICT_OK)
                sdsfree(section);
        }
    }
    return section_dict;
}

/* sets blocking_keys to the total number of keys which has at least one client blocked on them.
 * sets blocking_keys_on_nokey to the total number of keys which has at least one client
 * blocked on them to be written or deleted.
 * sets watched_keys to the total number of keys which has at least on client watching on them. */
void totalNumberOfStatefulKeys(unsigned long *blocking_keys, unsigned long *blocking_keys_on_nokey, unsigned long *watched_keys) {
    unsigned long bkeys=0, bkeys_on_nokey=0, wkeys=0;
    for (int j = 0; j < server.dbnum; j++) {
        bkeys += dictSize(server.db[j].blocking_keys);
        bkeys_on_nokey += dictSize(server.db[j].blocking_keys_unblock_on_nokey);
        wkeys += dictSize(server.db[j].watched_keys);
    }
    if (blocking_keys)
        *blocking_keys = bkeys;
    if (blocking_keys_on_nokey)
        *blocking_keys_on_nokey = bkeys_on_nokey;
    if (watched_keys)
        *watched_keys = wkeys;
}

/* Create the string returned by the INFO command. This is decoupled
 * by the INFO command itself as we need to report the same information
 * on memory corruption problems. */
sds genRedisInfoString(dict *section_dict, int all_sections, int everything) {
    sds info = sdsempty();
    time_t uptime = server.unixtime-server.stat_starttime;
    int j;
    int sections = 0;
    if (everything) all_sections = 1;

    /* Server */
    if (all_sections || (dictFind(section_dict,"server") != NULL)) {
        static int call_uname = 1;
        static struct utsname name;
        char *mode;
        char *supervised;

        if (server.cluster_enabled) mode = "cluster";
        else if (server.sentinel_mode) mode = "sentinel";
        else mode = "standalone";

        if (server.supervised) {
            if (server.supervised_mode == SUPERVISED_UPSTART) supervised = "upstart";
            else if (server.supervised_mode == SUPERVISED_SYSTEMD) supervised = "systemd";
            else supervised = "unknown";
        } else {
            supervised = "no";
        }

        if (sections++) info = sdscat(info,"\r\n");

        if (call_uname) {
            /* Uname can be slow and is always the same output. Cache it. */
            uname(&name);
            call_uname = 0;
        }

        info = sdscatfmt(info, "# Server\r\n" FMTARGS(
            "redis_version:%s\r\n", REDIS_VERSION,
            "redis_git_sha1:%s\r\n", redisGitSHA1(),
            "redis_git_dirty:%i\r\n", strtol(redisGitDirty(),NULL,10) > 0,
            "redis_build_id:%s\r\n", redisBuildIdString(),
            "redis_mode:%s\r\n", mode,
            "os:%s", name.sysname,
            " %s", name.release,
            " %s\r\n", name.machine,
            "arch_bits:%i\r\n", server.arch_bits,
            "monotonic_clock:%s\r\n", monotonicInfoString(),
            "multiplexing_api:%s\r\n", aeGetApiName(),
            "atomicvar_api:%s\r\n", REDIS_ATOMIC_API,
            "gcc_version:%s\r\n", GNUC_VERSION_STR,
            "process_id:%I\r\n", (int64_t) getpid(),
            "process_supervised:%s\r\n", supervised,
            "run_id:%s\r\n", server.runid,
            "tcp_port:%i\r\n", server.port ? server.port : server.tls_port,
            "server_time_usec:%I\r\n", (int64_t)server.ustime,
            "uptime_in_seconds:%I\r\n", (int64_t)uptime,
            "uptime_in_days:%I\r\n", (int64_t)(uptime/(3600*24)),
            "hz:%i\r\n", server.hz,
            "configured_hz:%i\r\n", server.config_hz,
            "lru_clock:%u\r\n", server.lruclock,
            "executable:%s\r\n", server.executable ? server.executable : "",
            "config_file:%s\r\n", server.configfile ? server.configfile : "",
            "io_threads_active:%i\r\n", server.io_threads_active));

        /* Conditional properties */
        if (isShutdownInitiated()) {
            info = sdscatfmt(info,
                "shutdown_in_milliseconds:%I\r\n",
                (int64_t)(server.shutdown_mstime - commandTimeSnapshot()));
        }

        /* get all the listeners information */
        info = getListensInfoString(info);
    }

    /* Clients */
    if (all_sections || (dictFind(section_dict,"clients") != NULL)) {
        size_t maxin, maxout;
        unsigned long blocking_keys, blocking_keys_on_nokey, watched_keys;
        getExpansiveClientsInfo(&maxin,&maxout);
        totalNumberOfStatefulKeys(&blocking_keys, &blocking_keys_on_nokey, &watched_keys);
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Clients\r\n" FMTARGS(
            "connected_clients:%lu\r\n", listLength(server.clients) - listLength(server.slaves),
            "cluster_connections:%lu\r\n", getClusterConnectionsCount(),
            "maxclients:%u\r\n", server.maxclients,
            "client_recent_max_input_buffer:%zu\r\n", maxin,
            "client_recent_max_output_buffer:%zu\r\n", maxout,
            "blocked_clients:%d\r\n", server.blocked_clients,
            "tracking_clients:%d\r\n", server.tracking_clients,
            "pubsub_clients:%d\r\n", server.pubsub_clients,
            "watching_clients:%d\r\n", server.watching_clients,
            "clients_in_timeout_table:%llu\r\n", (unsigned long long) raxSize(server.clients_timeout_table),
            "total_watched_keys:%lu\r\n", watched_keys,
            "total_blocking_keys:%lu\r\n", blocking_keys,
            "total_blocking_keys_on_nokey:%lu\r\n", blocking_keys_on_nokey));
    }

    /* Memory */
    if (all_sections || (dictFind(section_dict,"memory") != NULL)) {
        char hmem[64];
        char peak_hmem[64];
        char total_system_hmem[64];
        char used_memory_lua_hmem[64];
        char used_memory_vm_total_hmem[64];
        char used_memory_scripts_hmem[64];
        char used_memory_rss_hmem[64];
        char maxmemory_hmem[64];
        size_t zmalloc_used = zmalloc_used_memory();
        size_t total_system_mem = server.system_memory_size;
        const char *evict_policy = evictPolicyToString();
        long long memory_lua = evalMemory();
        long long memory_functions = functionsMemory();
        struct redisMemOverhead *mh = getMemoryOverheadData();

        /* Peak memory is updated from time to time by serverCron() so it
         * may happen that the instantaneous value is slightly bigger than
         * the peak value. This may confuse users, so we update the peak
         * if found smaller than the current memory usage. */
        if (zmalloc_used > server.stat_peak_memory)
            server.stat_peak_memory = zmalloc_used;

        bytesToHuman(hmem,sizeof(hmem),zmalloc_used);
        bytesToHuman(peak_hmem,sizeof(peak_hmem),server.stat_peak_memory);
        bytesToHuman(total_system_hmem,sizeof(total_system_hmem),total_system_mem);
        bytesToHuman(used_memory_lua_hmem,sizeof(used_memory_lua_hmem),memory_lua);
        bytesToHuman(used_memory_vm_total_hmem,sizeof(used_memory_vm_total_hmem),memory_functions + memory_lua);
        bytesToHuman(used_memory_scripts_hmem,sizeof(used_memory_scripts_hmem),mh->lua_caches + mh->functions_caches);
        bytesToHuman(used_memory_rss_hmem,sizeof(used_memory_rss_hmem),server.cron_malloc_stats.process_rss);
        bytesToHuman(maxmemory_hmem,sizeof(maxmemory_hmem),server.maxmemory);

        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Memory\r\n" FMTARGS(
            "used_memory:%zu\r\n", zmalloc_used,
            "used_memory_human:%s\r\n", hmem,
            "used_memory_rss:%zu\r\n", server.cron_malloc_stats.process_rss,
            "used_memory_rss_human:%s\r\n", used_memory_rss_hmem,
            "used_memory_peak:%zu\r\n", server.stat_peak_memory,
            "used_memory_peak_human:%s\r\n", peak_hmem,
            "used_memory_peak_perc:%.2f%%\r\n", mh->peak_perc,
            "used_memory_overhead:%zu\r\n", mh->overhead_total,
            "used_memory_startup:%zu\r\n", mh->startup_allocated,
            "used_memory_dataset:%zu\r\n", mh->dataset,
            "used_memory_dataset_perc:%.2f%%\r\n", mh->dataset_perc,
            "allocator_allocated:%zu\r\n", server.cron_malloc_stats.allocator_allocated,
            "allocator_active:%zu\r\n", server.cron_malloc_stats.allocator_active,
            "allocator_resident:%zu\r\n", server.cron_malloc_stats.allocator_resident,
            "allocator_muzzy:%zu\r\n", server.cron_malloc_stats.allocator_muzzy,
            "total_system_memory:%lu\r\n", (unsigned long)total_system_mem,
            "total_system_memory_human:%s\r\n", total_system_hmem,
            "used_memory_lua:%lld\r\n", memory_lua, /* deprecated, renamed to used_memory_vm_eval */
            "used_memory_vm_eval:%lld\r\n", memory_lua,
            "used_memory_lua_human:%s\r\n", used_memory_lua_hmem, /* deprecated */
            "used_memory_scripts_eval:%lld\r\n", (long long)mh->lua_caches,
            "number_of_cached_scripts:%lu\r\n", dictSize(evalScriptsDict()),
            "number_of_functions:%lu\r\n", functionsNum(),
            "number_of_libraries:%lu\r\n", functionsLibNum(),
            "used_memory_vm_functions:%lld\r\n", memory_functions,
            "used_memory_vm_total:%lld\r\n", memory_functions + memory_lua,
            "used_memory_vm_total_human:%s\r\n", used_memory_vm_total_hmem,
            "used_memory_functions:%lld\r\n", (long long)mh->functions_caches,
            "used_memory_scripts:%lld\r\n", (long long)mh->lua_caches + (long long)mh->functions_caches,
            "used_memory_scripts_human:%s\r\n", used_memory_scripts_hmem,
            "maxmemory:%lld\r\n", server.maxmemory,
            "maxmemory_human:%s\r\n", maxmemory_hmem,
            "maxmemory_policy:%s\r\n", evict_policy,
            "allocator_frag_ratio:%.2f\r\n", mh->allocator_frag,
            "allocator_frag_bytes:%zu\r\n", mh->allocator_frag_bytes,
            "allocator_rss_ratio:%.2f\r\n", mh->allocator_rss,
            "allocator_rss_bytes:%zd\r\n", mh->allocator_rss_bytes,
            "rss_overhead_ratio:%.2f\r\n", mh->rss_extra,
            "rss_overhead_bytes:%zd\r\n", mh->rss_extra_bytes,
            /* The next field (mem_fragmentation_ratio) is the total RSS
             * overhead, including fragmentation, but not just it. This field
             * (and the next one) is named like that just for backward
             * compatibility. */
            "mem_fragmentation_ratio:%.2f\r\n", mh->total_frag,
            "mem_fragmentation_bytes:%zd\r\n", mh->total_frag_bytes,
            "mem_not_counted_for_evict:%zu\r\n", freeMemoryGetNotCountedMemory(),
            "mem_replication_backlog:%zu\r\n", mh->repl_backlog,
            "mem_total_replication_buffers:%zu\r\n", server.repl_buffer_mem,
            "mem_clients_slaves:%zu\r\n", mh->clients_slaves,
            "mem_clients_normal:%zu\r\n", mh->clients_normal,
            "mem_cluster_links:%zu\r\n", mh->cluster_links,
            "mem_aof_buffer:%zu\r\n", mh->aof_buffer,
            "mem_allocator:%s\r\n", ZMALLOC_LIB,
            "mem_overhead_db_hashtable_rehashing:%zu\r\n", mh->overhead_db_hashtable_rehashing,
            "active_defrag_running:%d\r\n", server.active_defrag_running,
            "lazyfree_pending_objects:%zu\r\n", lazyfreeGetPendingObjectsCount(),
            "lazyfreed_objects:%zu\r\n", lazyfreeGetFreedObjectsCount()));
        freeMemoryOverheadData(mh);
    }

    /* Persistence */
    if (all_sections || (dictFind(section_dict,"persistence") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");
        double fork_perc = 0;
        if (server.stat_module_progress) {
            fork_perc = server.stat_module_progress * 100;
        } else if (server.stat_current_save_keys_total) {
            fork_perc = ((double)server.stat_current_save_keys_processed / server.stat_current_save_keys_total) * 100;
        }
        int aof_bio_fsync_status;
        atomicGet(server.aof_bio_fsync_status,aof_bio_fsync_status);

        info = sdscatprintf(info, "# Persistence\r\n" FMTARGS(
            "loading:%d\r\n", (int)(server.loading && !server.async_loading),
            "async_loading:%d\r\n", (int)server.async_loading,
            "current_cow_peak:%zu\r\n", server.stat_current_cow_peak,
            "current_cow_size:%zu\r\n", server.stat_current_cow_bytes,
            "current_cow_size_age:%lu\r\n", (server.stat_current_cow_updated ?
                                             (unsigned long) elapsedMs(server.stat_current_cow_updated) / 1000 : 0),
            "current_fork_perc:%.2f\r\n", fork_perc,
            "current_save_keys_processed:%zu\r\n", server.stat_current_save_keys_processed,
            "current_save_keys_total:%zu\r\n", server.stat_current_save_keys_total,
            "rdb_changes_since_last_save:%lld\r\n", server.dirty,
            "rdb_bgsave_in_progress:%d\r\n", server.child_type == CHILD_TYPE_RDB,
            "rdb_last_save_time:%jd\r\n", (intmax_t)server.lastsave,
            "rdb_last_bgsave_status:%s\r\n", (server.lastbgsave_status == C_OK) ? "ok" : "err",
            "rdb_last_bgsave_time_sec:%jd\r\n", (intmax_t)server.rdb_save_time_last,
            "rdb_current_bgsave_time_sec:%jd\r\n", (intmax_t)((server.child_type != CHILD_TYPE_RDB) ?
                                                              -1 : time(NULL)-server.rdb_save_time_start),
            "rdb_saves:%lld\r\n", server.stat_rdb_saves,
            "rdb_last_cow_size:%zu\r\n", server.stat_rdb_cow_bytes,
            "rdb_last_load_keys_expired:%lld\r\n", server.rdb_last_load_keys_expired,
            "rdb_last_load_keys_loaded:%lld\r\n", server.rdb_last_load_keys_loaded,
            "aof_enabled:%d\r\n", server.aof_state != AOF_OFF,
            "aof_rewrite_in_progress:%d\r\n", server.child_type == CHILD_TYPE_AOF,
            "aof_rewrite_scheduled:%d\r\n", server.aof_rewrite_scheduled,
            "aof_last_rewrite_time_sec:%jd\r\n", (intmax_t)server.aof_rewrite_time_last,
            "aof_current_rewrite_time_sec:%jd\r\n", (intmax_t)((server.child_type != CHILD_TYPE_AOF) ?
                                                               -1 : time(NULL)-server.aof_rewrite_time_start),
            "aof_last_bgrewrite_status:%s\r\n", (server.aof_lastbgrewrite_status == C_OK ?
                                                 "ok" : "err"),
            "aof_rewrites:%lld\r\n", server.stat_aof_rewrites,
            "aof_rewrites_consecutive_failures:%lld\r\n", server.stat_aofrw_consecutive_failures,
            "aof_last_write_status:%s\r\n", (server.aof_last_write_status == C_OK &&
                                             aof_bio_fsync_status == C_OK) ? "ok" : "err",
            "aof_last_cow_size:%zu\r\n", server.stat_aof_cow_bytes,
            "module_fork_in_progress:%d\r\n", server.child_type == CHILD_TYPE_MODULE,
            "module_fork_last_cow_size:%zu\r\n", server.stat_module_cow_bytes));

        if (server.aof_enabled) {
            info = sdscatprintf(info, FMTARGS(
                "aof_current_size:%lld\r\n", (long long) server.aof_current_size,
                "aof_base_size:%lld\r\n", (long long) server.aof_rewrite_base_size,
                "aof_pending_rewrite:%d\r\n", server.aof_rewrite_scheduled,
                "aof_buffer_length:%zu\r\n", sdslen(server.aof_buf),
                "aof_pending_bio_fsync:%lu\r\n", bioPendingJobsOfType(BIO_AOF_FSYNC),
                "aof_delayed_fsync:%lu\r\n", server.aof_delayed_fsync));
        }

        if (server.loading) {
            double perc = 0;
            time_t eta, elapsed;
            off_t remaining_bytes = 1;

            if (server.loading_total_bytes) {
                perc = ((double)server.loading_loaded_bytes / server.loading_total_bytes) * 100;
                remaining_bytes = server.loading_total_bytes - server.loading_loaded_bytes;
            } else if(server.loading_rdb_used_mem) {
                perc = ((double)server.loading_loaded_bytes / server.loading_rdb_used_mem) * 100;
                remaining_bytes = server.loading_rdb_used_mem - server.loading_loaded_bytes;
                /* used mem is only a (bad) estimation of the rdb file size, avoid going over 100% */
                if (perc > 99.99) perc = 99.99;
                if (remaining_bytes < 1) remaining_bytes = 1;
            }

            elapsed = time(NULL)-server.loading_start_time;
            if (elapsed == 0) {
                eta = 1; /* A fake 1 second figure if we don't have
                            enough info */
            } else {
                eta = (elapsed*remaining_bytes)/(server.loading_loaded_bytes+1);
            }

            info = sdscatprintf(info, FMTARGS(
                "loading_start_time:%jd\r\n", (intmax_t) server.loading_start_time,
                "loading_total_bytes:%llu\r\n", (unsigned long long) server.loading_total_bytes,
                "loading_rdb_used_mem:%llu\r\n", (unsigned long long) server.loading_rdb_used_mem,
                "loading_loaded_bytes:%llu\r\n", (unsigned long long) server.loading_loaded_bytes,
                "loading_loaded_perc:%.2f\r\n", perc,
                "loading_eta_seconds:%jd\r\n", (intmax_t)eta));
        }
    }

    /* Stats */
    if (all_sections  || (dictFind(section_dict,"stats") != NULL)) {
        long long stat_total_reads_processed, stat_total_writes_processed;
        long long stat_net_input_bytes, stat_net_output_bytes;
        long long stat_net_repl_input_bytes, stat_net_repl_output_bytes;
        long long current_eviction_exceeded_time = server.stat_last_eviction_exceeded_time ?
            (long long) elapsedUs(server.stat_last_eviction_exceeded_time): 0;
        long long current_active_defrag_time = server.stat_last_active_defrag_time ?
            (long long) elapsedUs(server.stat_last_active_defrag_time): 0;
        long long stat_client_qbuf_limit_disconnections;
        atomicGet(server.stat_total_reads_processed, stat_total_reads_processed);
        atomicGet(server.stat_total_writes_processed, stat_total_writes_processed);
        atomicGet(server.stat_net_input_bytes, stat_net_input_bytes);
        atomicGet(server.stat_net_output_bytes, stat_net_output_bytes);
        atomicGet(server.stat_net_repl_input_bytes, stat_net_repl_input_bytes);
        atomicGet(server.stat_net_repl_output_bytes, stat_net_repl_output_bytes);
        atomicGet(server.stat_client_qbuf_limit_disconnections, stat_client_qbuf_limit_disconnections);

        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Stats\r\n" FMTARGS(
            "total_connections_received:%lld\r\n", server.stat_numconnections,
            "total_commands_processed:%lld\r\n", server.stat_numcommands,
            "instantaneous_ops_per_sec:%lld\r\n", getInstantaneousMetric(STATS_METRIC_COMMAND),
            "total_net_input_bytes:%lld\r\n", stat_net_input_bytes + stat_net_repl_input_bytes,
            "total_net_output_bytes:%lld\r\n", stat_net_output_bytes + stat_net_repl_output_bytes,
            "total_net_repl_input_bytes:%lld\r\n", stat_net_repl_input_bytes,
            "total_net_repl_output_bytes:%lld\r\n", stat_net_repl_output_bytes,
            "instantaneous_input_kbps:%.2f\r\n", (float)getInstantaneousMetric(STATS_METRIC_NET_INPUT)/1024,
            "instantaneous_output_kbps:%.2f\r\n", (float)getInstantaneousMetric(STATS_METRIC_NET_OUTPUT)/1024,
            "instantaneous_input_repl_kbps:%.2f\r\n", (float)getInstantaneousMetric(STATS_METRIC_NET_INPUT_REPLICATION)/1024,
            "instantaneous_output_repl_kbps:%.2f\r\n", (float)getInstantaneousMetric(STATS_METRIC_NET_OUTPUT_REPLICATION)/1024,
            "rejected_connections:%lld\r\n", server.stat_rejected_conn,
            "sync_full:%lld\r\n", server.stat_sync_full,
            "sync_partial_ok:%lld\r\n", server.stat_sync_partial_ok,
            "sync_partial_err:%lld\r\n", server.stat_sync_partial_err,
            "expired_subkeys:%lld\r\n", server.stat_expired_subkeys,
            "expired_keys:%lld\r\n", server.stat_expiredkeys,
            "expired_stale_perc:%.2f\r\n", server.stat_expired_stale_perc*100,
            "expired_time_cap_reached_count:%lld\r\n", server.stat_expired_time_cap_reached_count,
            "expire_cycle_cpu_milliseconds:%lld\r\n", server.stat_expire_cycle_time_used/1000,
            "evicted_keys:%lld\r\n", server.stat_evictedkeys,
            "evicted_clients:%lld\r\n", server.stat_evictedclients,
            "evicted_scripts:%lld\r\n", server.stat_evictedscripts,
            "total_eviction_exceeded_time:%lld\r\n", (server.stat_total_eviction_exceeded_time + current_eviction_exceeded_time) / 1000,
            "current_eviction_exceeded_time:%lld\r\n", current_eviction_exceeded_time / 1000,
            "keyspace_hits:%lld\r\n", server.stat_keyspace_hits,
            "keyspace_misses:%lld\r\n", server.stat_keyspace_misses,
            "pubsub_channels:%llu\r\n", kvstoreSize(server.pubsub_channels),
            "pubsub_patterns:%lu\r\n", dictSize(server.pubsub_patterns),
            "pubsubshard_channels:%llu\r\n", kvstoreSize(server.pubsubshard_channels),
            "latest_fork_usec:%lld\r\n", server.stat_fork_time,
            "total_forks:%lld\r\n", server.stat_total_forks,
            "migrate_cached_sockets:%ld\r\n", dictSize(server.migrate_cached_sockets),
            "slave_expires_tracked_keys:%zu\r\n", getSlaveKeyWithExpireCount(),
            "active_defrag_hits:%lld\r\n", server.stat_active_defrag_hits,
            "active_defrag_misses:%lld\r\n", server.stat_active_defrag_misses,
            "active_defrag_key_hits:%lld\r\n", server.stat_active_defrag_key_hits,
            "active_defrag_key_misses:%lld\r\n", server.stat_active_defrag_key_misses,
            "total_active_defrag_time:%lld\r\n", (server.stat_total_active_defrag_time + current_active_defrag_time) / 1000,
            "current_active_defrag_time:%lld\r\n", current_active_defrag_time / 1000,
            "tracking_total_keys:%lld\r\n", (unsigned long long) trackingGetTotalKeys(),
            "tracking_total_items:%lld\r\n", (unsigned long long) trackingGetTotalItems(),
            "tracking_total_prefixes:%lld\r\n", (unsigned long long) trackingGetTotalPrefixes(),
            "unexpected_error_replies:%lld\r\n", server.stat_unexpected_error_replies,
            "total_error_replies:%lld\r\n", server.stat_total_error_replies,
            "dump_payload_sanitizations:%lld\r\n", server.stat_dump_payload_sanitizations,
            "total_reads_processed:%lld\r\n", stat_total_reads_processed,
            "total_writes_processed:%lld\r\n", stat_total_writes_processed,
            "io_threaded_reads_processed:%lld\r\n", server.stat_io_reads_processed,
            "io_threaded_writes_processed:%lld\r\n", server.stat_io_writes_processed,
            "client_query_buffer_limit_disconnections:%lld\r\n", stat_client_qbuf_limit_disconnections,
            "client_output_buffer_limit_disconnections:%lld\r\n", server.stat_client_outbuf_limit_disconnections,
            "reply_buffer_shrinks:%lld\r\n", server.stat_reply_buffer_shrinks,
            "reply_buffer_expands:%lld\r\n", server.stat_reply_buffer_expands,
            "eventloop_cycles:%llu\r\n", server.duration_stats[EL_DURATION_TYPE_EL].cnt,
            "eventloop_duration_sum:%llu\r\n", server.duration_stats[EL_DURATION_TYPE_EL].sum,
            "eventloop_duration_cmd_sum:%llu\r\n", server.duration_stats[EL_DURATION_TYPE_CMD].sum,
            "instantaneous_eventloop_cycles_per_sec:%llu\r\n", getInstantaneousMetric(STATS_METRIC_EL_CYCLE),
            "instantaneous_eventloop_duration_usec:%llu\r\n", getInstantaneousMetric(STATS_METRIC_EL_DURATION)));
        info = genRedisInfoStringACLStats(info);
    }

    /* Replication */
    if (all_sections || (dictFind(section_dict,"replication") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Replication\r\n"
            "role:%s\r\n",
            server.masterhost == NULL ? "master" : "slave");
        if (server.masterhost) {
            long long slave_repl_offset = 1;
            long long slave_read_repl_offset = 1;

            if (server.master) {
                slave_repl_offset = server.master->reploff;
                slave_read_repl_offset = server.master->read_reploff;
            } else if (server.cached_master) {
                slave_repl_offset = server.cached_master->reploff;
                slave_read_repl_offset = server.cached_master->read_reploff;
            }

            info = sdscatprintf(info, FMTARGS(
                "master_host:%s\r\n", server.masterhost,
                "master_port:%d\r\n", server.masterport,
                "master_link_status:%s\r\n", (server.repl_state == REPL_STATE_CONNECTED) ? "up" : "down",
                "master_last_io_seconds_ago:%d\r\n", server.master ? ((int)(server.unixtime-server.master->lastinteraction)) : -1,
                "master_sync_in_progress:%d\r\n", server.repl_state == REPL_STATE_TRANSFER,
                "slave_read_repl_offset:%lld\r\n", slave_read_repl_offset,
                "slave_repl_offset:%lld\r\n", slave_repl_offset));

            if (server.repl_state == REPL_STATE_TRANSFER) {
                double perc = 0;
                if (server.repl_transfer_size) {
                    perc = ((double)server.repl_transfer_read / server.repl_transfer_size) * 100;
                }
                info = sdscatprintf(info, FMTARGS(
                    "master_sync_total_bytes:%lld\r\n", (long long) server.repl_transfer_size,
                    "master_sync_read_bytes:%lld\r\n", (long long) server.repl_transfer_read,
                    "master_sync_left_bytes:%lld\r\n", (long long) (server.repl_transfer_size - server.repl_transfer_read),
                    "master_sync_perc:%.2f\r\n", perc,
                    "master_sync_last_io_seconds_ago:%d\r\n", (int)(server.unixtime-server.repl_transfer_lastio)));
            }

            if (server.repl_state != REPL_STATE_CONNECTED) {
                info = sdscatprintf(info,
                    "master_link_down_since_seconds:%jd\r\n",
                    server.repl_down_since ?
                    (intmax_t)(server.unixtime-server.repl_down_since) : -1);
            }
            info = sdscatprintf(info, FMTARGS(
                "slave_priority:%d\r\n", server.slave_priority,
                "slave_read_only:%d\r\n", server.repl_slave_ro,
                "replica_announced:%d\r\n", server.replica_announced));
        }

        info = sdscatprintf(info,
            "connected_slaves:%lu\r\n",
            listLength(server.slaves));

        /* If min-slaves-to-write is active, write the number of slaves
         * currently considered 'good'. */
        if (server.repl_min_slaves_to_write &&
            server.repl_min_slaves_max_lag) {
            info = sdscatprintf(info,
                "min_slaves_good_slaves:%d\r\n",
                server.repl_good_slaves_count);
        }

        if (listLength(server.slaves)) {
            int slaveid = 0;
            listNode *ln;
            listIter li;

            listRewind(server.slaves,&li);
            while((ln = listNext(&li))) {
                client *slave = listNodeValue(ln);
                char ip[NET_IP_STR_LEN], *slaveip = slave->slave_addr;
                int port;
                long lag = 0;

                if (!slaveip) {
                    if (connAddrPeerName(slave->conn,ip,sizeof(ip),&port) == -1)
                        continue;
                    slaveip = ip;
                }
                const char *state = replstateToString(slave->replstate);
                if (state[0] == '\0') continue;
                if (slave->replstate == SLAVE_STATE_ONLINE)
                    lag = time(NULL) - slave->repl_ack_time;

                info = sdscatprintf(info,
                    "slave%d:ip=%s,port=%d,state=%s,"
                    "offset=%lld,lag=%ld\r\n",
                    slaveid,slaveip,slave->slave_listening_port,state,
                    slave->repl_ack_off, lag);
                slaveid++;
            }
        }
        info = sdscatprintf(info, FMTARGS(
            "master_failover_state:%s\r\n", getFailoverStateString(),
            "master_replid:%s\r\n", server.replid,
            "master_replid2:%s\r\n", server.replid2,
            "master_repl_offset:%lld\r\n", server.master_repl_offset,
            "second_repl_offset:%lld\r\n", server.second_replid_offset,
            "repl_backlog_active:%d\r\n", server.repl_backlog != NULL,
            "repl_backlog_size:%lld\r\n", server.repl_backlog_size,
            "repl_backlog_first_byte_offset:%lld\r\n", server.repl_backlog ? server.repl_backlog->offset : 0,
            "repl_backlog_histlen:%lld\r\n", server.repl_backlog ? server.repl_backlog->histlen : 0));
    }

    /* CPU */
    if (all_sections || (dictFind(section_dict,"cpu") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");

        struct rusage self_ru, c_ru;
        getrusage(RUSAGE_SELF, &self_ru);
        getrusage(RUSAGE_CHILDREN, &c_ru);
        info = sdscatprintf(info,
        "# CPU\r\n"
        "used_cpu_sys:%ld.%06ld\r\n"
        "used_cpu_user:%ld.%06ld\r\n"
        "used_cpu_sys_children:%ld.%06ld\r\n"
        "used_cpu_user_children:%ld.%06ld\r\n",
        (long)self_ru.ru_stime.tv_sec, (long)self_ru.ru_stime.tv_usec,
        (long)self_ru.ru_utime.tv_sec, (long)self_ru.ru_utime.tv_usec,
        (long)c_ru.ru_stime.tv_sec, (long)c_ru.ru_stime.tv_usec,
        (long)c_ru.ru_utime.tv_sec, (long)c_ru.ru_utime.tv_usec);
#ifdef RUSAGE_THREAD
        struct rusage m_ru;
        getrusage(RUSAGE_THREAD, &m_ru);
        info = sdscatprintf(info,
            "used_cpu_sys_main_thread:%ld.%06ld\r\n"
            "used_cpu_user_main_thread:%ld.%06ld\r\n",
            (long)m_ru.ru_stime.tv_sec, (long)m_ru.ru_stime.tv_usec,
            (long)m_ru.ru_utime.tv_sec, (long)m_ru.ru_utime.tv_usec);
#endif  /* RUSAGE_THREAD */
    }

    /* Modules */
    if (all_sections || (dictFind(section_dict,"module_list") != NULL) || (dictFind(section_dict,"modules") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,"# Modules\r\n");
        info = genModulesInfoString(info);
    }

    /* Command statistics */
    if (all_sections || (dictFind(section_dict,"commandstats") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Commandstats\r\n");
        info = genRedisInfoStringCommandStats(info, server.commands);
    }

    /* Error statistics */
    if (all_sections || (dictFind(section_dict,"errorstats") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscat(info, "# Errorstats\r\n");
        raxIterator ri;
        raxStart(&ri,server.errors);
        raxSeek(&ri,"^",NULL,0);
        struct redisError *e;
        while(raxNext(&ri)) {
            char *tmpsafe;
            e = (struct redisError *) ri.data;
            info = sdscatprintf(info,
                "errorstat_%.*s:count=%lld\r\n",
                (int)ri.key_len, getSafeInfoString((char *) ri.key, ri.key_len, &tmpsafe), e->count);
            if (tmpsafe != NULL) zfree(tmpsafe);
        }
        raxStop(&ri);
    }

    /* Latency by percentile distribution per command */
    if (all_sections || (dictFind(section_dict,"latencystats") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Latencystats\r\n");
        if (server.latency_tracking_enabled) {
            info = genRedisInfoStringLatencyStats(info, server.commands);
        }
    }

    /* Cluster */
    if (all_sections || (dictFind(section_dict,"cluster") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
        "# Cluster\r\n"
        "cluster_enabled:%d\r\n",
        server.cluster_enabled);
    }

    /* Key space */
    if (all_sections || (dictFind(section_dict,"keyspace") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Keyspace\r\n");
        for (j = 0; j < server.dbnum; j++) {
            long long keys, vkeys, hexpires;

            keys = kvstoreSize(server.db[j].keys);
            vkeys = kvstoreSize(server.db[j].expires);
            hexpires = ebGetTotalItems(server.db[j].hexpires, &hashExpireBucketsType);

            if (keys || vkeys) {
                info = sdscatprintf(info,
                    "db%d:keys=%lld,expires=%lld,avg_ttl=%lld,subexpiry=%lld\r\n",
                    j, keys, vkeys, server.db[j].avg_ttl, hexpires);
            }
        }
    }

    /* Get info from modules.
     * Returned when the user asked for "everything", "modules", or a specific module section.
     * We're not aware of the module section names here, and we rather avoid the search when we can.
     * so we proceed if there's a requested section name that's not found yet, or when the user asked
     * for "all" with any additional section names. */
    if (everything || dictFind(section_dict, "modules") != NULL || sections < (int)dictSize(section_dict) ||
        (all_sections && dictSize(section_dict)))
    {

        info = modulesCollectInfo(info,
                                  everything || dictFind(section_dict, "modules") != NULL ? NULL: section_dict,
                                  0, /* not a crash report */
                                  sections);
    }

    if (dictFind(section_dict, "debug") != NULL) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Debug\r\n" FMTARGS(
            "eventloop_duration_aof_sum:%llu\r\n", server.duration_stats[EL_DURATION_TYPE_AOF].sum,
            "eventloop_duration_cron_sum:%llu\r\n", server.duration_stats[EL_DURATION_TYPE_CRON].sum,
            "eventloop_duration_max:%llu\r\n", server.duration_stats[EL_DURATION_TYPE_EL].max,
            "eventloop_cmd_per_cycle_max:%lld\r\n", server.el_cmd_cnt_max,
            "allocator_allocated_lua:%zu\r\n", server.cron_malloc_stats.lua_allocator_allocated,
            "allocator_active_lua:%zu\r\n", server.cron_malloc_stats.lua_allocator_active,
            "allocator_resident_lua:%zu\r\n", server.cron_malloc_stats.lua_allocator_resident,
            "allocator_frag_bytes_lua:%zu\r\n", server.cron_malloc_stats.lua_allocator_frag_smallbins_bytes));
    }

    return info;
}

/* INFO [<section> [<section> ...]] */
void infoCommand(client *c) {
    if (server.sentinel_mode) {
        sentinelInfoCommand(c);
        return;
    }
    int all_sections = 0;
    int everything = 0;
    dict *sections_dict = genInfoSectionDict(c->argv+1, c->argc-1, NULL, &all_sections, &everything);
    sds info = genRedisInfoString(sections_dict, all_sections, everything);
    addReplyVerbatim(c,info,sdslen(info),"txt");
    sdsfree(info);
    releaseInfoSectionDict(sections_dict);
    return;
}

void monitorCommand(client *c) {
    if (c->flags & CLIENT_DENY_BLOCKING) {
        /**
         * A client that has CLIENT_DENY_BLOCKING flag on
         * expects a reply per command and so can't execute MONITOR. */
        addReplyError(c, "MONITOR isn't allowed for DENY BLOCKING client");
        return;
    }

    /* ignore MONITOR if already slave or in monitor mode */
    if (c->flags & CLIENT_SLAVE) return;

    c->flags |= (CLIENT_SLAVE|CLIENT_MONITOR);
    listAddNodeTail(server.monitors,c);
    addReply(c,shared.ok);
}

/* =================================== Main! ================================ */

int checkIgnoreWarning(const char *warning) {
    int argc, j;
    sds *argv = sdssplitargs(server.ignore_warnings, &argc);
    if (argv == NULL)
        return 0;

    for (j = 0; j < argc; j++) {
        char *flag = argv[j];
        if (!strcasecmp(flag, warning))
            break;
    }
    sdsfreesplitres(argv,argc);
    return j < argc;
}

#ifdef __linux__
#include <sys/prctl.h>
/* since linux-3.5, kernel supports to set the state of the "THP disable" flag
 * for the calling thread. PR_SET_THP_DISABLE is defined in linux/prctl.h */
static int THPDisable(void) {
    int ret = -EINVAL;

    if (!server.disable_thp)
        return ret;

#ifdef PR_SET_THP_DISABLE
    ret = prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
#endif

    return ret;
}

void linuxMemoryWarnings(void) {
    sds err_msg = NULL;
    if (checkOvercommit(&err_msg) < 0) {
        serverLog(LL_WARNING,"WARNING %s", err_msg);
        sdsfree(err_msg);
    }
    if (checkTHPEnabled(&err_msg) < 0) {
        server.thp_enabled = 1;
        if (THPDisable() == 0) {
            server.thp_enabled = 0;
        } else {
            serverLog(LL_WARNING, "WARNING %s", err_msg);
        }
        sdsfree(err_msg);
    }
}
#endif /* __linux__ */

void createPidFile(void) {
    /* If pidfile requested, but no pidfile defined, use
     * default pidfile path */
    if (!server.pidfile) server.pidfile = zstrdup(CONFIG_DEFAULT_PID_FILE);

    /* Try to write the pid file in a best-effort way. */
    FILE *fp = fopen(server.pidfile,"w");
    if (fp) {
        fprintf(fp,"%d\n",(int)getpid());
        fclose(fp);
    } else {
        serverLog(LL_WARNING, "Failed to write PID file: %s", strerror(errno));
    }
}

void daemonize(void) {
    int fd;

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
}

sds getVersion(void) {
    sds version = sdscatprintf(sdsempty(),
        "v=%s sha=%s:%d malloc=%s bits=%d build=%llx",
        REDIS_VERSION,
        redisGitSHA1(),
        atoi(redisGitDirty()) > 0,
        ZMALLOC_LIB,
        sizeof(long) == 4 ? 32 : 64,
        (unsigned long long) redisBuildId());
    return version;
}

void usage(void) {
    fprintf(stderr,"Usage: ./redis-server [/path/to/redis.conf] [options] [-]\n");
    fprintf(stderr,"       ./redis-server - (read config from stdin)\n");
    fprintf(stderr,"       ./redis-server -v or --version\n");
    fprintf(stderr,"       ./redis-server -h or --help\n");
    fprintf(stderr,"       ./redis-server --test-memory <megabytes>\n");
    fprintf(stderr,"       ./redis-server --check-system\n");
    fprintf(stderr,"\n");
    fprintf(stderr,"Examples:\n");
    fprintf(stderr,"       ./redis-server (run the server with default conf)\n");
    fprintf(stderr,"       echo 'maxmemory 128mb' | ./redis-server -\n");
    fprintf(stderr,"       ./redis-server /etc/redis/6379.conf\n");
    fprintf(stderr,"       ./redis-server --port 7777\n");
    fprintf(stderr,"       ./redis-server --port 7777 --replicaof 127.0.0.1 8888\n");
    fprintf(stderr,"       ./redis-server /etc/myredis.conf --loglevel verbose -\n");
    fprintf(stderr,"       ./redis-server /etc/myredis.conf --loglevel verbose\n\n");
    fprintf(stderr,"Sentinel mode:\n");
    fprintf(stderr,"       ./redis-server /etc/sentinel.conf --sentinel\n");
    exit(1);
}

void redisAsciiArt(void) {
#include "asciilogo.h"
    char *buf = zmalloc(1024*16);
    char *mode;

    if (server.cluster_enabled) mode = "cluster";
    else if (server.sentinel_mode) mode = "sentinel";
    else mode = "standalone";

    /* Show the ASCII logo if: log file is stdout AND stdout is a
     * tty AND syslog logging is disabled. Also show logo if the user
     * forced us to do so via redis.conf. */
    int show_logo = ((!server.syslog_enabled &&
                      server.logfile[0] == '\0' &&
                      isatty(fileno(stdout))) ||
                     server.always_show_logo);

    if (!show_logo) {
        serverLog(LL_NOTICE,
            "Running mode=%s, port=%d.",
            mode, server.port ? server.port : server.tls_port
        );
    } else {
        snprintf(buf,1024*16,ascii_logo,
            REDIS_VERSION,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            (sizeof(long) == 8) ? "64" : "32",
            mode, server.port ? server.port : server.tls_port,
            (long) getpid()
        );
        serverLogRaw(LL_NOTICE|LL_RAW,buf);
    }
    zfree(buf);
}

/* Get the server listener by type name */
connListener *listenerByType(const char *typename) {
    int conn_index;

    conn_index = connectionIndexByType(typename);
    if (conn_index < 0)
        return NULL;

    return &server.listeners[conn_index];
}

/* Close original listener, re-create a new listener from the updated bind address & port */
int changeListener(connListener *listener) {
    /* Close old servers */
    closeListener(listener);

    /* Just close the server if port disabled */
    if (listener->port == 0) {
        if (server.set_proc_title) redisSetProcTitle(NULL);
        return C_OK;
    }

    /* Re-create listener */
    if (connListen(listener) != C_OK) {
        return C_ERR;
    }

    /* Create event handlers */
    if (createSocketAcceptHandler(listener, listener->ct->accept_handler) != C_OK) {
        serverPanic("Unrecoverable error creating %s accept handler.", listener->ct->get_type(NULL));
    }

    if (server.set_proc_title) redisSetProcTitle(NULL);

    return C_OK;
}

static void sigShutdownHandler(int sig) {
    char *msg;

    switch (sig) {
    case SIGINT:
        msg = "Received SIGINT scheduling shutdown...";
        break;
    case SIGTERM:
        msg = "Received SIGTERM scheduling shutdown...";
        break;
    default:
        msg = "Received shutdown signal, scheduling shutdown...";
    };

    /* SIGINT is often delivered via Ctrl+C in an interactive session.
     * If we receive the signal the second time, we interpret this as
     * the user really wanting to quit ASAP without waiting to persist
     * on disk and without waiting for lagging replicas. */
    if (server.shutdown_asap && sig == SIGINT) {
        serverLogRawFromHandler(LL_WARNING, "You insist... exiting now.");
        rdbRemoveTempFile(getpid(), 1);
        exit(1); /* Exit with an error since this was not a clean shutdown. */
    } else if (server.loading) {
        msg = "Received shutdown signal during loading, scheduling shutdown.";
    }

    serverLogRawFromHandler(LL_WARNING, msg);
    server.shutdown_asap = 1;
    server.last_sig_received = sig;
}

void setupSignalHandlers(void) {
    struct sigaction act;

    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigShutdownHandler;
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT, &act, NULL);

    setupDebugSigHandlers();
}

/* This is the signal handler for children process. It is currently useful
 * in order to track the SIGUSR1, that we send to a child in order to terminate
 * it in a clean way, without the parent detecting an error and stop
 * accepting writes because of a write error condition. */
static void sigKillChildHandler(int sig) {
    UNUSED(sig);
    int level = server.in_fork_child == CHILD_TYPE_MODULE? LL_VERBOSE: LL_WARNING;
    serverLogRawFromHandler(level, "Received SIGUSR1 in child, exiting now.");
    exitFromChild(SERVER_CHILD_NOERROR_RETVAL);
}

void setupChildSignalHandlers(void) {
    struct sigaction act;

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used.
     * Otherwise, sa_handler is used. */
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigKillChildHandler;
    sigaction(SIGUSR1, &act, NULL);
}

/* After fork, the child process will inherit the resources
 * of the parent process, e.g. fd(socket or flock) etc.
 * should close the resources not used by the child process, so that if the
 * parent restarts it can bind/lock despite the child possibly still running. */
void closeChildUnusedResourceAfterFork(void) {
    closeListeningSockets(0);
    if (server.cluster_enabled && server.cluster_config_file_lock_fd != -1)
        close(server.cluster_config_file_lock_fd);  /* don't care if this fails */

    /* Clear server.pidfile, this is the parent pidfile which should not
     * be touched (or deleted) by the child (on exit / crash) */
    zfree(server.pidfile);
    server.pidfile = NULL;
}

/* purpose is one of CHILD_TYPE_ types */
int redisFork(int purpose) {
    if (isMutuallyExclusiveChildType(purpose)) {
        if (hasActiveChildProcess()) {
            errno = EEXIST;
            return -1;
        }

        openChildInfoPipe();
    }

    int childpid;
    long long start = ustime();
    if ((childpid = fork()) == 0) {
        /* Child.
         *
         * The order of setting things up follows some reasoning:
         * Setup signal handlers first because a signal could fire at any time.
         * Adjust OOM score before everything else to assist the OOM killer if
         * memory resources are low.
         */
        server.in_fork_child = purpose;
        setupChildSignalHandlers();
        setOOMScoreAdj(CONFIG_OOM_BGCHILD);
        updateDictResizePolicy();
        dismissMemoryInChild();
        closeChildUnusedResourceAfterFork();
        /* Close the reading part, so that if the parent crashes, the child will
         * get a write error and exit. */
        if (server.child_info_pipe[0] != -1)
            close(server.child_info_pipe[0]);
    } else {
        /* Parent */
        if (childpid == -1) {
            int fork_errno = errno;
            if (isMutuallyExclusiveChildType(purpose)) closeChildInfoPipe();
            errno = fork_errno;
            return -1;
        }

        server.stat_total_forks++;
        server.stat_fork_time = ustime()-start;
        server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
        latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);

        /* The child_pid and child_type are only for mutually exclusive children.
         * other child types should handle and store their pid's in dedicated variables.
         *
         * Today, we allows CHILD_TYPE_LDB to run in parallel with the other fork types:
         * - it isn't used for production, so it will not make the server be less efficient
         * - used for debugging, and we don't want to block it from running while other
         *   forks are running (like RDB and AOF) */
        if (isMutuallyExclusiveChildType(purpose)) {
            server.child_pid = childpid;
            server.child_type = purpose;
            server.stat_current_cow_peak = 0;
            server.stat_current_cow_bytes = 0;
            server.stat_current_cow_updated = 0;
            server.stat_current_save_keys_processed = 0;
            server.stat_module_progress = 0;
            server.stat_current_save_keys_total = dbTotalServerKeyCount();
        }

        updateDictResizePolicy();
        moduleFireServerEvent(REDISMODULE_EVENT_FORK_CHILD,
                              REDISMODULE_SUBEVENT_FORK_CHILD_BORN,
                              NULL);
    }
    return childpid;
}

void sendChildCowInfo(childInfoType info_type, char *pname) {
    sendChildInfoGeneric(info_type, 0, -1, pname);
}

void sendChildInfo(childInfoType info_type, size_t keys, char *pname) {
    sendChildInfoGeneric(info_type, keys, -1, pname);
}

/* Try to release pages back to the OS directly (bypassing the allocator),
 * in an effort to decrease CoW during fork. For small allocations, we can't
 * release any full page, so in an effort to avoid getting the size of the
 * allocation from the allocator (malloc_size) when we already know it's small,
 * we check the size_hint. If the size is not already known, passing a size_hint
 * of 0 will lead the checking the real size of the allocation.
 * Also please note that the size may be not accurate, so in order to make this
 * solution effective, the judgement for releasing memory pages should not be
 * too strict. */
void dismissMemory(void* ptr, size_t size_hint) {
    if (ptr == NULL) return;

    /* madvise(MADV_DONTNEED) can not release pages if the size of memory
     * is too small, we try to release only for the memory which the size
     * is more than half of page size. */
    if (size_hint && size_hint <= server.page_size/2) return;

    zmadvise_dontneed(ptr);
}

/* Dismiss big chunks of memory inside a client structure, see dismissMemory() */
void dismissClientMemory(client *c) {
    /* Dismiss client query buffer and static reply buffer. */
    dismissMemory(c->buf, c->buf_usable_size);
    if (c->querybuf) dismissSds(c->querybuf);
    /* Dismiss argv array only if we estimate it contains a big buffer. */
    if (c->argc && c->argv_len_sum/c->argc >= server.page_size) {
        for (int i = 0; i < c->argc; i++) {
            dismissObject(c->argv[i], 0);
        }
    }
    if (c->argc) dismissMemory(c->argv, c->argc*sizeof(robj*));

    /* Dismiss the reply array only if the average buffer size is bigger
     * than a page. */
    if (listLength(c->reply) &&
        c->reply_bytes/listLength(c->reply) >= server.page_size)
    {
        listIter li;
        listNode *ln;
        listRewind(c->reply, &li);
        while ((ln = listNext(&li))) {
            clientReplyBlock *bulk = listNodeValue(ln);
            /* Default bulk size is 16k, actually it has extra data, maybe it
             * occupies 20k according to jemalloc bin size if using jemalloc. */
            if (bulk) dismissMemory(bulk, bulk->size);
        }
    }
}

/* In the child process, we don't need some buffers anymore, and these are
 * likely to change in the parent when there's heavy write traffic.
 * We dismiss them right away, to avoid CoW.
 * see dismissMemeory(). */
void dismissMemoryInChild(void) {
    /* madvise(MADV_DONTNEED) may not work if Transparent Huge Pages is enabled. */
    if (server.thp_enabled) return;

    /* Currently we use zmadvise_dontneed only when we use jemalloc with Linux.
     * so we avoid these pointless loops when they're not going to do anything. */
#if defined(USE_JEMALLOC) && defined(__linux__)
    listIter li;
    listNode *ln;

    /* Dismiss replication buffer. We don't need to separately dismiss replication
     * backlog and replica' output buffer, because they just reference the global
     * replication buffer but don't cost real memory. */
    listRewind(server.repl_buffer_blocks, &li);
    while((ln = listNext(&li))) {
        replBufBlock *o = listNodeValue(ln);
        dismissMemory(o, o->size);
    }

    /* Dismiss all clients memory. */
    listRewind(server.clients, &li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        dismissClientMemory(c);
    }
#endif
}

void memtest(size_t megabytes, int passes);

/* Returns 1 if there is --sentinel among the arguments or if
 * executable name contains "redis-sentinel". */
int checkForSentinelMode(int argc, char **argv, char *exec_name) {
    if (strstr(exec_name,"redis-sentinel") != NULL) return 1;

    for (int j = 1; j < argc; j++)
        if (!strcmp(argv[j],"--sentinel")) return 1;
    return 0;
}

/* Function called at startup to load RDB or AOF file in memory. */
void loadDataFromDisk(void) {
    long long start = ustime();
    if (server.aof_state == AOF_ON) {
        int ret = loadAppendOnlyFiles(server.aof_manifest);
        if (ret == AOF_FAILED || ret == AOF_OPEN_ERR)
            exit(1);
        if (ret != AOF_NOT_EXIST)
            serverLog(LL_NOTICE, "DB loaded from append only file: %.3f seconds", (float)(ustime()-start)/1000000);
    } else {
        rdbSaveInfo rsi = RDB_SAVE_INFO_INIT;
        int rsi_is_valid = 0;
        errno = 0; /* Prevent a stale value from affecting error checking */
        int rdb_flags = RDBFLAGS_NONE;
        if (iAmMaster()) {
            /* Master may delete expired keys when loading, we should
             * propagate expire to replication backlog. */
            createReplicationBacklog();
            rdb_flags |= RDBFLAGS_FEED_REPL;
        }
        int rdb_load_ret = rdbLoad(server.rdb_filename, &rsi, rdb_flags);
        if (rdb_load_ret == RDB_OK) {
            serverLog(LL_NOTICE,"DB loaded from disk: %.3f seconds",
                (float)(ustime()-start)/1000000);

            /* Restore the replication ID / offset from the RDB file. */
            if (rsi.repl_id_is_set &&
                rsi.repl_offset != -1 &&
                /* Note that older implementations may save a repl_stream_db
                 * of -1 inside the RDB file in a wrong way, see more
                 * information in function rdbPopulateSaveInfo. */
                rsi.repl_stream_db != -1)
            {
                rsi_is_valid = 1;
                if (!iAmMaster()) {
                    memcpy(server.replid,rsi.repl_id,sizeof(server.replid));
                    server.master_repl_offset = rsi.repl_offset;
                    /* If this is a replica, create a cached master from this
                     * information, in order to allow partial resynchronizations
                     * with masters. */
                    replicationCacheMasterUsingMyself();
                    selectDb(server.cached_master,rsi.repl_stream_db);
                } else {
                    /* If this is a master, we can save the replication info
                     * as secondary ID and offset, in order to allow replicas
                     * to partial resynchronizations with masters. */
                    memcpy(server.replid2,rsi.repl_id,sizeof(server.replid));
                    server.second_replid_offset = rsi.repl_offset+1;
                    /* Rebase master_repl_offset from rsi.repl_offset. */
                    server.master_repl_offset += rsi.repl_offset;
                    serverAssert(server.repl_backlog);
                    server.repl_backlog->offset = server.master_repl_offset -
                              server.repl_backlog->histlen + 1;
                    rebaseReplicationBuffer(rsi.repl_offset);
                    server.repl_no_slaves_since = time(NULL);
                }
            }
        } else if (rdb_load_ret != RDB_NOT_EXIST) {
            serverLog(LL_WARNING, "Fatal error loading the DB, check server logs. Exiting.");
            exit(1);
        }

        /* We always create replication backlog if server is a master, we need
         * it because we put DELs in it when loading expired keys in RDB, but
         * if RDB doesn't have replication info or there is no rdb, it is not
         * possible to support partial resynchronization, to avoid extra memory
         * of replication backlog, we drop it. */
        if (!rsi_is_valid && server.repl_backlog)
            freeReplicationBacklog();
    }
}

void redisOutOfMemoryHandler(size_t allocation_size) {
    serverLog(LL_WARNING,"Out Of Memory allocating %zu bytes!",
        allocation_size);
    serverPanic("Redis aborting for OUT OF MEMORY. Allocating %zu bytes!",
        allocation_size);
}

/* Callback for sdstemplate on proc-title-template. See redis.conf for
 * supported variables.
 */
static sds redisProcTitleGetVariable(const sds varname, void *arg)
{
    if (!strcmp(varname, "title")) {
        return sdsnew(arg);
    } else if (!strcmp(varname, "listen-addr")) {
        if (server.port || server.tls_port)
            return sdscatprintf(sdsempty(), "%s:%u",
                                server.bindaddr_count ? server.bindaddr[0] : "*",
                                server.port ? server.port : server.tls_port);
        else
            return sdscatprintf(sdsempty(), "unixsocket:%s", server.unixsocket);
    } else if (!strcmp(varname, "server-mode")) {
        if (server.cluster_enabled) return sdsnew("[cluster]");
        else if (server.sentinel_mode) return sdsnew("[sentinel]");
        else return sdsempty();
    } else if (!strcmp(varname, "config-file")) {
        return sdsnew(server.configfile ? server.configfile : "-");
    } else if (!strcmp(varname, "port")) {
        return sdscatprintf(sdsempty(), "%u", server.port);
    } else if (!strcmp(varname, "tls-port")) {
        return sdscatprintf(sdsempty(), "%u", server.tls_port);
    } else if (!strcmp(varname, "unixsocket")) {
        return sdsnew(server.unixsocket);
    } else
        return NULL;    /* Unknown variable name */
}

/* Expand the specified proc-title-template string and return a newly
 * allocated sds, or NULL. */
static sds expandProcTitleTemplate(const char *template, const char *title) {
    sds res = sdstemplate(template, redisProcTitleGetVariable, (void *) title);
    if (!res)
        return NULL;
    return sdstrim(res, " ");
}
/* Validate the specified template, returns 1 if valid or 0 otherwise. */
int validateProcTitleTemplate(const char *template) {
    int ok = 1;
    sds res = expandProcTitleTemplate(template, "");
    if (!res)
        return 0;
    if (sdslen(res) == 0) ok = 0;
    sdsfree(res);
    return ok;
}

int redisSetProcTitle(char *title) {
#ifdef USE_SETPROCTITLE
    if (!title) title = server.exec_argv[0];
    sds proc_title = expandProcTitleTemplate(server.proc_title_template, title);
    if (!proc_title) return C_ERR;  /* Not likely, proc_title_template is validated */

    setproctitle("%s", proc_title);
    sdsfree(proc_title);
#else
    UNUSED(title);
#endif

    return C_OK;
}

void redisSetCpuAffinity(const char *cpulist) {
#ifdef USE_SETCPUAFFINITY
    setcpuaffinity(cpulist);
#else
    UNUSED(cpulist);
#endif
}

/* Send a notify message to systemd. Returns sd_notify return code which is
 * a positive number on success. */
int redisCommunicateSystemd(const char *sd_notify_msg) {
#ifdef HAVE_LIBSYSTEMD
    int ret = sd_notify(0, sd_notify_msg);

    if (ret == 0)
        serverLog(LL_WARNING, "systemd supervision error: NOTIFY_SOCKET not found!");
    else if (ret < 0)
        serverLog(LL_WARNING, "systemd supervision error: sd_notify: %d", ret);
    return ret;
#else
    UNUSED(sd_notify_msg);
    return 0;
#endif
}

/* Attempt to set up upstart supervision. Returns 1 if successful. */
static int redisSupervisedUpstart(void) {
    const char *upstart_job = getenv("UPSTART_JOB");

    if (!upstart_job) {
        serverLog(LL_WARNING,
                "upstart supervision requested, but UPSTART_JOB not found!");
        return 0;
    }

    serverLog(LL_NOTICE, "supervised by upstart, will stop to signal readiness.");
    raise(SIGSTOP);
    unsetenv("UPSTART_JOB");
    return 1;
}

/* Attempt to set up systemd supervision. Returns 1 if successful. */
static int redisSupervisedSystemd(void) {
#ifndef HAVE_LIBSYSTEMD
    serverLog(LL_WARNING,
            "systemd supervision requested or auto-detected, but Redis is compiled without libsystemd support!");
    return 0;
#else
    if (redisCommunicateSystemd("STATUS=Redis is loading...\n") <= 0)
        return 0;
    serverLog(LL_NOTICE,
        "Supervised by systemd. Please make sure you set appropriate values for TimeoutStartSec and TimeoutStopSec in your service unit.");
    return 1;
#endif
}

int redisIsSupervised(int mode) {
    int ret = 0;

    if (mode == SUPERVISED_AUTODETECT) {
        if (getenv("UPSTART_JOB")) {
            serverLog(LL_VERBOSE, "Upstart supervision detected.");
            mode = SUPERVISED_UPSTART;
        } else if (getenv("NOTIFY_SOCKET")) {
            serverLog(LL_VERBOSE, "Systemd supervision detected.");
            mode = SUPERVISED_SYSTEMD;
        }
    }

    switch (mode) {
        case SUPERVISED_UPSTART:
            ret = redisSupervisedUpstart();
            break;
        case SUPERVISED_SYSTEMD:
            ret = redisSupervisedSystemd();
            break;
        default:
            break;
    }

    if (ret)
        server.supervised_mode = mode;

    return ret;
}

int iAmMaster(void) {
    return ((!server.cluster_enabled && server.masterhost == NULL) ||
            (server.cluster_enabled && clusterNodeIsMaster(getMyClusterNode())));
}

#ifdef REDIS_TEST
#include "testhelp.h"
#include "intset.h"  /* Compact integer set structure */

int __failed_tests = 0;
int __test_num = 0;

/* The flags are the following:
* --accurate:     Runs tests with more iterations.
* --large-memory: Enables tests that consume more than 100mb. */
typedef int redisTestProc(int argc, char **argv, int flags);
struct redisTest {
    char *name;
    redisTestProc *proc;
    int failed;
} redisTests[] = {
    {"ziplist", ziplistTest},
    {"quicklist", quicklistTest},
    {"intset", intsetTest},
    {"zipmap", zipmapTest},
    {"sha1test", sha1Test},
    {"util", utilTest},
    {"endianconv", endianconvTest},
    {"crc64", crc64Test},
    {"zmalloc", zmalloc_test},
    {"sds", sdsTest},
    {"mstr", mstrTest},
    {"dict", dictTest},
    {"listpack", listpackTest},
    {"kvstore", kvstoreTest},
    {"ebuckets", ebucketsTest},
};
redisTestProc *getTestProcByName(const char *name) {
    int numtests = sizeof(redisTests)/sizeof(struct redisTest);
    for (int j = 0; j < numtests; j++) {
        if (!strcasecmp(name,redisTests[j].name)) {
            return redisTests[j].proc;
        }
    }
    return NULL;
}
#endif

int main(int argc, char **argv) {
    struct timeval tv;
    int j;
    char config_from_stdin = 0;

#ifdef REDIS_TEST
    monotonicInit(); /* Required for dict tests, that are relying on monotime during dict rehashing. */
    if (argc >= 3 && !strcasecmp(argv[1], "test")) {
        int flags = 0;
        for (j = 3; j < argc; j++) {
            char *arg = argv[j];
            if (!strcasecmp(arg, "--accurate")) flags |= REDIS_TEST_ACCURATE;
            else if (!strcasecmp(arg, "--large-memory")) flags |= REDIS_TEST_LARGE_MEMORY;
            else if (!strcasecmp(arg, "--valgrind")) flags |= REDIS_TEST_VALGRIND;
            else if (!strcasecmp(arg, "--verbose")) flags |= REDIS_TEST_VERBOSE;
        }

        if (!strcasecmp(argv[2], "all")) {
            int numtests = sizeof(redisTests)/sizeof(struct redisTest);
            for (j = 0; j < numtests; j++) {
                redisTests[j].failed = (redisTests[j].proc(argc,argv,flags) != 0);
            }

            /* Report tests result */
            int failed_num = 0;
            for (j = 0; j < numtests; j++) {
                if (redisTests[j].failed) {
                    failed_num++;
                    printf("[failed] Test - %s\n", redisTests[j].name);
                } else {
                    printf("[ok] Test - %s\n", redisTests[j].name);
                }
            }

            printf("%d tests, %d passed, %d failed\n", numtests,
                   numtests-failed_num, failed_num);

            return failed_num == 0 ? 0 : 1;
        } else {
            redisTestProc *proc = getTestProcByName(argv[2]);
            if (!proc) return -1; /* test not found */
            return proc(argc,argv,flags);
        }

        return 0;
    }
#endif

    /* We need to initialize our libraries, and the server configuration. */
#ifdef INIT_SETPROCTITLE_REPLACEMENT
    spt_init(argc, argv);
#endif
    tzset(); /* Populates 'timezone' global. */
    zmalloc_set_oom_handler(redisOutOfMemoryHandler);

    /* To achieve entropy, in case of containers, their time() and getpid() can
     * be the same. But value of tv_usec is fast enough to make the difference */
    gettimeofday(&tv,NULL);
    srand(time(NULL)^getpid()^tv.tv_usec);
    srandom(time(NULL)^getpid()^tv.tv_usec);
    init_genrand64(((long long) tv.tv_sec * 1000000 + tv.tv_usec) ^ getpid());
    crc64_init();

    /* Store umask value. Because umask(2) only offers a set-and-get API we have
     * to reset it and restore it back. We do this early to avoid a potential
     * race condition with threads that could be creating files or directories.
     */
    umask(server.umask = umask(0777));

    uint8_t hashseed[16];
    getRandomBytes(hashseed,sizeof(hashseed));
    dictSetHashFunctionSeed(hashseed);

    char *exec_name = strrchr(argv[0], '/');
    if (exec_name == NULL) exec_name = argv[0];
    server.sentinel_mode = checkForSentinelMode(argc,argv, exec_name);
    initServerConfig();
    ACLInit(); /* The ACL subsystem must be initialized ASAP because the
                  basic networking code and client creation depends on it. */
    moduleInitModulesSystem();
    connTypeInitialize();

    /* Store the executable path and arguments in a safe place in order
     * to be able to restart the server later. */
    server.executable = getAbsolutePath(argv[0]);
    server.exec_argv = zmalloc(sizeof(char*)*(argc+1));
    server.exec_argv[argc] = NULL;
    for (j = 0; j < argc; j++) server.exec_argv[j] = zstrdup(argv[j]);

    /* We need to init sentinel right now as parsing the configuration file
     * in sentinel mode will have the effect of populating the sentinel
     * data structures with master nodes to monitor. */
    if (server.sentinel_mode) {
        initSentinelConfig();
        initSentinel();
    }

    /* Check if we need to start in redis-check-rdb/aof mode. We just execute
     * the program main. However the program is part of the Redis executable
     * so that we can easily execute an RDB check on loading errors. */
    if (strstr(exec_name,"redis-check-rdb") != NULL)
        redis_check_rdb_main(argc,argv,NULL);
    else if (strstr(exec_name,"redis-check-aof") != NULL)
        redis_check_aof_main(argc,argv);

    if (argc >= 2) {
        j = 1; /* First option to parse in argv[] */
        sds options = sdsempty();

        /* Handle special options --help and --version */
        if (strcmp(argv[1], "-v") == 0 ||
            strcmp(argv[1], "--version") == 0)
        {
            sds version = getVersion();
            printf("Redis server %s\n", version);
            sdsfree(version);
            exit(0);
        }
        if (strcmp(argv[1], "--help") == 0 ||
            strcmp(argv[1], "-h") == 0) usage();
        if (strcmp(argv[1], "--test-memory") == 0) {
            if (argc == 3) {
                memtest(atoi(argv[2]),50);
                exit(0);
            } else {
                fprintf(stderr,"Please specify the amount of memory to test in megabytes.\n");
                fprintf(stderr,"Example: ./redis-server --test-memory 4096\n\n");
                exit(1);
            }
        } if (strcmp(argv[1], "--check-system") == 0) {
            exit(syscheck() ? 0 : 1);
        }
        /* Parse command line options
         * Precedence wise, File, stdin, explicit options -- last config is the one that matters.
         *
         * First argument is the config file name? */
        if (argv[1][0] != '-') {
            /* Replace the config file in server.exec_argv with its absolute path. */
            server.configfile = getAbsolutePath(argv[1]);
            zfree(server.exec_argv[1]);
            server.exec_argv[1] = zstrdup(server.configfile);
            j = 2; // Skip this arg when parsing options
        }
        sds *argv_tmp;
        int argc_tmp;
        int handled_last_config_arg = 1;
        while(j < argc) {
            /* Either first or last argument - Should we read config from stdin? */
            if (argv[j][0] == '-' && argv[j][1] == '\0' && (j == 1 || j == argc-1)) {
                config_from_stdin = 1;
            }
            /* All the other options are parsed and conceptually appended to the
             * configuration file. For instance --port 6380 will generate the
             * string "port 6380\n" to be parsed after the actual config file
             * and stdin input are parsed (if they exist).
             * Only consider that if the last config has at least one argument. */
            else if (handled_last_config_arg && argv[j][0] == '-' && argv[j][1] == '-') {
                /* Option name */
                if (sdslen(options)) options = sdscat(options,"\n");
                /* argv[j]+2 for removing the preceding `--` */
                options = sdscat(options,argv[j]+2);
                options = sdscat(options," ");

                argv_tmp = sdssplitargs(argv[j], &argc_tmp);
                if (argc_tmp == 1) {
                    /* Means that we only have one option name, like --port or "--port " */
                    handled_last_config_arg = 0;

                    if ((j != argc-1) && argv[j+1][0] == '-' && argv[j+1][1] == '-' &&
                        !strcasecmp(argv[j], "--save"))
                    {
                        /* Special case: handle some things like `--save --config value`.
                         * In this case, if next argument starts with `--`, we will reset
                         * handled_last_config_arg flag and append an empty "" config value
                         * to the options, so it will become `--save "" --config value`.
                         * We are doing it to be compatible with pre 7.0 behavior (which we
                         * break it in #10660, 7.0.1), since there might be users who generate
                         * a command line from an array and when it's empty that's what they produce. */
                        options = sdscat(options, "\"\"");
                        handled_last_config_arg = 1;
                    }
                    else if ((j == argc-1) && !strcasecmp(argv[j], "--save")) {
                        /* Special case: when empty save is the last argument.
                         * In this case, we append an empty "" config value to the options,
                         * so it will become `--save ""` and will follow the same reset thing. */
                        options = sdscat(options, "\"\"");
                    }
                    else if ((j != argc-1) && argv[j+1][0] == '-' && argv[j+1][1] == '-' &&
                        !strcasecmp(argv[j], "--sentinel"))
                    {
                        /* Special case: handle some things like `--sentinel --config value`.
                         * It is a pseudo config option with no value. In this case, if next
                         * argument starts with `--`, we will reset handled_last_config_arg flag.
                         * We are doing it to be compatible with pre 7.0 behavior (which we
                         * break it in #10660, 7.0.1). */
                        options = sdscat(options, "");
                        handled_last_config_arg = 1;
                    }
                    else if ((j == argc-1) && !strcasecmp(argv[j], "--sentinel")) {
                        /* Special case: when --sentinel is the last argument.
                         * It is a pseudo config option with no value. In this case, do nothing.
                         * We are doing it to be compatible with pre 7.0 behavior (which we
                         * break it in #10660, 7.0.1). */
                        options = sdscat(options, "");
                    }
                } else {
                    /* Means that we are passing both config name and it's value in the same arg,
                     * like "--port 6380", so we need to reset handled_last_config_arg flag. */
                    handled_last_config_arg = 1;
                }
                sdsfreesplitres(argv_tmp, argc_tmp);
            } else {
                /* Option argument */
                options = sdscatrepr(options,argv[j],strlen(argv[j]));
                options = sdscat(options," ");
                handled_last_config_arg = 1;
            }
            j++;
        }

        loadServerConfig(server.configfile, config_from_stdin, options);
        if (server.sentinel_mode) loadSentinelConfigFromQueue();
        sdsfree(options);
    }
    if (server.sentinel_mode) sentinelCheckConfigFile();

    /* Do system checks */
#ifdef __linux__
    linuxMemoryWarnings();
    sds err_msg = NULL;
    if (checkXenClocksource(&err_msg) < 0) {
        serverLog(LL_WARNING, "WARNING %s", err_msg);
        sdsfree(err_msg);
    }
#if defined (__arm64__)
    int ret;
    if ((ret = checkLinuxMadvFreeForkBug(&err_msg)) <= 0) {
        if (ret < 0) {
            serverLog(LL_WARNING, "WARNING %s", err_msg);
            sdsfree(err_msg);
        } else
            serverLog(LL_WARNING, "Failed to test the kernel for a bug that could lead to data corruption during background save. "
                                  "Your system could be affected, please report this error.");
        if (!checkIgnoreWarning("ARM64-COW-BUG")) {
            serverLog(LL_WARNING,"Redis will now exit to prevent data corruption. "
                                 "Note that it is possible to suppress this warning by setting the following config: ignore-warnings ARM64-COW-BUG");
            exit(1);
        }
    }
#endif /* __arm64__ */
#endif /* __linux__ */

    /* Daemonize if needed */
    server.supervised = redisIsSupervised(server.supervised_mode);
    int background = server.daemonize && !server.supervised;
    if (background) daemonize();

    serverLog(LL_NOTICE, "oO0OoO0OoO0Oo Redis is starting oO0OoO0OoO0Oo");
    serverLog(LL_NOTICE,
        "Redis version=%s, bits=%d, commit=%s, modified=%d, pid=%d, just started",
            REDIS_VERSION,
            (sizeof(long) == 8) ? 64 : 32,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            (int)getpid());

    if (argc == 1) {
        serverLog(LL_WARNING, "Warning: no config file specified, using the default config. In order to specify a config file use %s /path/to/redis.conf", argv[0]);
    } else {
        serverLog(LL_NOTICE, "Configuration loaded");
    }

    initServer();
    if (background || server.pidfile) createPidFile();
    if (server.set_proc_title) redisSetProcTitle(NULL);
    redisAsciiArt();
    checkTcpBacklogSettings();
    if (server.cluster_enabled) {
        clusterInit();
    }
    if (!server.sentinel_mode) {
        moduleInitModulesSystemLast();
        moduleLoadFromQueue();
    }
    ACLLoadUsersAtStartup();
    initListeners();
    if (server.cluster_enabled) {
        clusterInitLast();
    }
    InitServerLast();

    if (!server.sentinel_mode) {
        /* Things not needed when running in Sentinel mode. */
        serverLog(LL_NOTICE,"Server initialized");
        aofLoadManifestFromDisk();
        loadDataFromDisk();
        aofOpenIfNeededOnServerStart();
        aofDelHistoryFiles();
        /* While loading data, we delay applying "appendonly" config change.
         * If there was a config change while we were inside loadDataFromDisk()
         * above, we'll apply it here. */
        applyAppendOnlyConfig();

        if (server.cluster_enabled) {
            serverAssert(verifyClusterConfigWithData() == C_OK);
        }

        for (j = 0; j < CONN_TYPE_MAX; j++) {
            connListener *listener = &server.listeners[j];
            if (listener->ct == NULL)
                continue;

            serverLog(LL_NOTICE,"Ready to accept connections %s", listener->ct->get_type(NULL));
        }

        if (server.supervised_mode == SUPERVISED_SYSTEMD) {
            if (!server.masterhost) {
                redisCommunicateSystemd("STATUS=Ready to accept connections\n");
            } else {
                redisCommunicateSystemd("STATUS=Ready to accept connections in read-only mode. Waiting for MASTER <-> REPLICA sync\n");
            }
            redisCommunicateSystemd("READY=1\n");
        }
    } else {
        sentinelIsRunning();
        if (server.supervised_mode == SUPERVISED_SYSTEMD) {
            redisCommunicateSystemd("STATUS=Ready to accept connections\n");
            redisCommunicateSystemd("READY=1\n");
        }
    }

    /* Warning the user about suspicious maxmemory setting. */
    if (server.maxmemory > 0 && server.maxmemory < 1024*1024) {
        serverLog(LL_WARNING,"WARNING: You specified a maxmemory value that is less than 1MB (current value is %llu bytes). Are you sure this is what you really want?", server.maxmemory);
    }

    redisSetCpuAffinity(server.server_cpulist);
    setOOMScoreAdj(-1);

    aeMain(server.el);
    aeDeleteEventLoop(server.el);
    return 0;
}

/* The End */

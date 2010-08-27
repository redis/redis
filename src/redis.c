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

#include "redis.h"

#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#include <ucontext.h>
#endif /* HAVE_BACKTRACE */

#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
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

/* Our shared "common" objects */

struct sharedObjectsStruct shared;

/* Global vars that are actally used as constants. The following double
 * values are used for double on-disk serialization, and are initialized
 * at runtime to avoid strange compiler optimizations. */

double R_Zero, R_PosInf, R_NegInf, R_Nan;

/*================================= Globals ================================= */

/* Global vars */
struct redisServer server; /* server global state */
struct redisCommand *commandTable;
struct redisCommand readonlyCommandTable[] = {
    {"get",getCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"set",setCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,0,0,0},
    {"setnx",setnxCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,0,0,0},
    {"setex",setexCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,0,0,0},
    {"append",appendCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"substr",substrCommand,4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"strlen",strlenCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"del",delCommand,-2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"exists",existsCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"incr",incrCommand,2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"decr",decrCommand,2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"mget",mgetCommand,-2,REDIS_CMD_INLINE,NULL,1,-1,1},
    {"rpush",rpushCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"lpush",lpushCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"rpushx",rpushxCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"lpushx",lpushxCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"linsert",linsertCommand,5,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
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
    {"persist",persistCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"slaveof",slaveofCommand,3,REDIS_CMD_INLINE,NULL,0,0,0},
    {"debug",debugCommand,-2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"config",configCommand,-2,REDIS_CMD_BULK,NULL,0,0,0},
    {"subscribe",subscribeCommand,-2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"unsubscribe",unsubscribeCommand,-1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"psubscribe",psubscribeCommand,-2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"punsubscribe",punsubscribeCommand,-1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"publish",publishCommand,3,REDIS_CMD_BULK|REDIS_CMD_FORCE_REPLICATION,NULL,0,0,0},
    {"watch",watchCommand,-2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"unwatch",unwatchCommand,1,REDIS_CMD_INLINE,NULL,0,0,0}
};

/*============================ Utility functions ============================ */

void redisLog(int level, const char *fmt, ...) {
    va_list ap;
    FILE *fp;
    char *c = ".-*#";
    char buf[64];
    time_t now;

    if (level < server.verbosity) return;

    fp = (server.logfile == NULL) ? stdout : fopen(server.logfile,"a");
    if (!fp) return;

    va_start(ap, fmt);
    now = time(NULL);
    strftime(buf,64,"%d %b %H:%M:%S",localtime(&now));
    fprintf(fp,"[%d] %s %c ",(int)getpid(),buf,c[level]);
    vfprintf(fp, fmt, ap);
    fprintf(fp,"\n");
    fflush(fp);
    va_end(ap);

    if (server.logfile) fclose(fp);
}

/* Redis generally does not try to recover from out of memory conditions
 * when allocating objects or strings, it is not clear if it will be possible
 * to report this condition to the client since the networking layer itself
 * is based on heap allocation for send buffers, so we simply abort.
 * At least the code will be simpler to read... */
void oom(const char *msg) {
    redisLog(REDIS_WARNING, "%s: Out of memory\n",msg);
    sleep(1);
    abort();
}

/*====================== Hash table type implementation  ==================== */

/* This is an hash table type that uses the SDS dynamic strings libary as
 * keys and radis objects as values (objects can hold SDS strings,
 * lists, sets). */

void dictVanillaFree(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    zfree(val);
}

void dictListDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    listRelease((list*)val);
}

int dictSdsKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    int l1,l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

void dictRedisObjectDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    if (val == NULL) return; /* Values of swapped out keys as set to NULL */
    decrRefCount(val);
}

void dictSdsDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    sdsfree(val);
}

int dictObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    const robj *o1 = key1, *o2 = key2;
    return dictSdsKeyCompare(privdata,o1->ptr,o2->ptr);
}

unsigned int dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}

unsigned int dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

int dictEncObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    robj *o1 = (robj*) key1, *o2 = (robj*) key2;
    int cmp;

    if (o1->encoding == REDIS_ENCODING_INT &&
        o2->encoding == REDIS_ENCODING_INT)
            return o1->ptr == o2->ptr;

    o1 = getDecodedObject(o1);
    o2 = getDecodedObject(o2);
    cmp = dictSdsKeyCompare(privdata,o1->ptr,o2->ptr);
    decrRefCount(o1);
    decrRefCount(o2);
    return cmp;
}

unsigned int dictEncObjHash(const void *key) {
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

/* Sets type */
dictType setDictType = {
    dictEncObjHash,            /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictEncObjKeyCompare,      /* key compare */
    dictRedisObjectDestructor, /* key destructor */
    NULL                       /* val destructor */
};

/* Sorted sets hash (note: a skiplist is used in addition to the hash table) */
dictType zsetDictType = {
    dictEncObjHash,            /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictEncObjKeyCompare,      /* key compare */
    dictRedisObjectDestructor, /* key destructor */
    dictVanillaFree            /* val destructor of malloc(sizeof(double)) */
};

/* Db->dict, keys are sds strings, vals are Redis objects. */
dictType dbDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictRedisObjectDestructor   /* val destructor */
};

/* Db->expires */
dictType keyptrDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    NULL,                      /* key destructor */
    NULL                       /* val destructor */
};

/* Hash type hash table (note that small hashes are represented with zimpaps) */
dictType hashDictType = {
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
dictType keylistDictType = {
    dictObjHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictObjKeyCompare,          /* key compare */
    dictRedisObjectDestructor,  /* key destructor */
    dictListDestructor          /* val destructor */
};

int htNeedsResize(dict *dict) {
    long long size, used;

    size = dictSlots(dict);
    used = dictSize(dict);
    return (size && used && size > DICT_HT_INITIAL_SIZE &&
            (used*100/size < REDIS_HT_MINFILL));
}

/* If the percentage of used slots in the HT reaches REDIS_HT_MINFILL
 * we resize the hash table to save memory */
void tryResizeHashTables(void) {
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
void incrementallyRehash(void) {
    int j;

    for (j = 0; j < server.dbnum; j++) {
        if (dictIsRehashing(server.db[j].dict)) {
            dictRehashMilliseconds(server.db[j].dict,1);
            break; /* already used our millisecond for this loop... */
        }
    }
}

/* This function is called once a background process of some kind terminates,
 * as we want to avoid resizing the hash tables when there is a child in order
 * to play well with copy-on-write (otherwise when a resize happens lots of
 * memory pages are copied). The goal of this function is to update the ability
 * for dict.c to resize the hash tables accordingly to the fact we have o not
 * running childs. */
void updateDictResizePolicy(void) {
    if (server.bgsavechildpid == -1 && server.bgrewritechildpid == -1)
        dictEnableResize();
    else
        dictDisableResize();
}

/* ======================= Cron: called every 100 ms ======================== */

/* Try to expire a few timed out keys. The algorithm used is adaptive and
 * will use few CPU cycles if there are few expiring keys, otherwise
 * it will get more aggressive to avoid that too much memory is used by
 * keys that can be removed from the keyspace. */
void activeExpireCycle(void) {
    int j;

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
                    sds key = dictGetEntryKey(de);
                    robj *keyobj = createStringObject(key,sdslen(key));

                    propagateExpire(db,keyobj);
                    dbDelete(db,keyobj);
                    decrRefCount(keyobj);
                    expired++;
                    server.stat_expiredkeys++;
                }
            }
        } while (expired > REDIS_EXPIRELOOKUPS_PER_CRON/4);
    }
}


int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    int j, loops = server.cronloops++;
    REDIS_NOTUSED(eventLoop);
    REDIS_NOTUSED(id);
    REDIS_NOTUSED(clientData);

    /* We take a cached value of the unix time in the global state because
     * with virtual memory and aging there is to store the current time
     * in objects at every object access, and accuracy is not needed.
     * To access a global var is faster than calling time(NULL) */
    server.unixtime = time(NULL);
    /* We have just 21 bits per object for LRU information.
     * So we use an (eventually wrapping) LRU clock with minutes resolution.
     *
     * When we need to select what object to swap, we compute the minimum
     * time distance between the current lruclock and the object last access
     * lruclock info. Even if clocks will wrap on overflow, there is
     * the interesting property that we are sure that at least
     * ABS(A-B) minutes passed between current time and timestamp B.
     *
     * This is not precise but we don't need at all precision, but just
     * something statistically reasonable.
     */
    server.lruclock = (time(NULL)/60)&((1<<21)-1);

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

    /* Expire a few keys per cycle, only if this is a master.
     * On slaves we wait for DEL operations synthesized by the master
     * in order to guarantee a strict consistency. */
    if (server.masterhost == NULL) activeExpireCycle();

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
void beforeSleep(struct aeEventLoop *eventLoop) {
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
            redisAssert(cmd != NULL);
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

/* =========================== Server initialization ======================== */

void createSharedObjects(void) {
    int j;

    shared.crlf = createObject(REDIS_STRING,sdsnew("\r\n"));
    shared.ok = createObject(REDIS_STRING,sdsnew("+OK\r\n"));
    shared.err = createObject(REDIS_STRING,sdsnew("-ERR\r\n"));
    shared.emptybulk = createObject(REDIS_STRING,sdsnew("$0\r\n\r\n"));
    shared.czero = createObject(REDIS_STRING,sdsnew(":0\r\n"));
    shared.cone = createObject(REDIS_STRING,sdsnew(":1\r\n"));
    shared.cnegone = createObject(REDIS_STRING,sdsnew(":-1\r\n"));
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

void initServerConfig() {
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
    server.no_appendfsync_on_rewrite = 0;
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
    server.list_max_ziplist_entries = REDIS_LIST_MAX_ZIPLIST_ENTRIES;
    server.list_max_ziplist_value = REDIS_LIST_MAX_ZIPLIST_VALUE;
    server.set_max_intset_entries = REDIS_SET_MAX_INTSET_ENTRIES;
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

void initServer() {
    int j;

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setupSigSegvAction();

    server.mainthread = pthread_self();
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

int qsortRedisCommands(const void *r1, const void *r2) {
    return strcasecmp(
        ((struct redisCommand*)r1)->name,
        ((struct redisCommand*)r2)->name);
}

void sortCommandTable() {
    /* Copy and sort the read-only version of the command table */
    commandTable = (struct redisCommand*)zmalloc(sizeof(readonlyCommandTable));
    memcpy(commandTable,readonlyCommandTable,sizeof(readonlyCommandTable));
    qsort(commandTable,
        sizeof(readonlyCommandTable)/sizeof(struct redisCommand),
        sizeof(struct redisCommand),qsortRedisCommands);
}

/* ====================== Commands lookup and execution ===================== */

struct redisCommand *lookupCommand(char *name) {
    struct redisCommand tmp = {name,NULL,0,0,NULL,0,0,0};
    return bsearch(
        &tmp,
        commandTable,
        sizeof(readonlyCommandTable)/sizeof(struct redisCommand),
        sizeof(struct redisCommand),
        qsortRedisCommands);
}

/* Call() is the core of Redis execution of a command */
void call(redisClient *c, struct redisCommand *cmd) {
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
int processCommand(redisClient *c) {
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
                char *eptr;
                long bulklen = strtol(((char*)c->argv[0]->ptr)+1,&eptr,10);
                int perr = eptr[0] != '\0';

                decrRefCount(c->argv[0]);
                if (perr || bulklen == LONG_MIN || bulklen == LONG_MAX ||
                    bulklen < 0 || bulklen > 1024*1024*1024)
                {
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
        char *eptr;
        long bulklen = strtol(c->argv[c->argc-1]->ptr,&eptr,10);
        int perr = eptr[0] != '\0';

        decrRefCount(c->argv[c->argc-1]);
        if (perr || bulklen == LONG_MAX || bulklen == LONG_MIN ||
            bulklen < 0 || bulklen > 1024*1024*1024)
        {
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

/*================================== Shutdown =============================== */

int prepareForShutdown() {
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
        aof_fsync(server.appendfd);
        if (server.vm_enabled) unlink(server.vm_swap_file);
    } else {
        /* Snapshotting. Perform a SYNC SAVE and exit */
        if (rdbSave(server.dbfilename) != REDIS_OK) {
            /* Ooops.. error saving! The best we can do is to continue
             * operating. Note that if there was a background saving process,
             * in the next cron() Redis will be notified that the background
             * saving aborted, handling special stuff like slaves pending for
             * synchronization... */
            redisLog(REDIS_WARNING,"Error trying to save the DB, can't exit");
            return REDIS_ERR;
        }
    }
    if (server.daemonize) unlink(server.pidfile);
    redisLog(REDIS_WARNING,"Server exit now, bye bye...");
    return REDIS_OK;
}

/*================================== Commands =============================== */

void authCommand(redisClient *c) {
    if (!server.requirepass || !strcmp(c->argv[1]->ptr, server.requirepass)) {
      c->authenticated = 1;
      addReply(c,shared.ok);
    } else {
      c->authenticated = 0;
      addReplySds(c,sdscatprintf(sdsempty(),"-ERR invalid password\r\n"));
    }
}

void pingCommand(redisClient *c) {
    addReply(c,shared.pong);
}

void echoCommand(redisClient *c) {
    addReplyBulk(c,c->argv[1]);
}

/* Convert an amount of bytes into a human readable string in the form
 * of 100B, 2G, 100M, 4K, and so forth. */
void bytesToHuman(char *s, unsigned long long n) {
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
sds genRedisInfoString(void) {
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
        redisGitSHA1(),
        strtol(redisGitDirty(),NULL,10) > 0,
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

void infoCommand(redisClient *c) {
    sds info = genRedisInfoString();
    addReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n",
        (unsigned long)sdslen(info)));
    addReplySds(c,info);
    addReply(c,shared.crlf);
}

void monitorCommand(redisClient *c) {
    /* ignore MONITOR if aleady slave or in monitor mode */
    if (c->flags & REDIS_SLAVE) return;

    c->flags |= (REDIS_SLAVE|REDIS_MONITOR);
    c->slaveseldb = 0;
    listAddNodeTail(server.monitors,c);
    addReply(c,shared.ok);
}

/* ============================ Maxmemory directive  ======================== */

/* Try to free one object form the pre-allocated objects free list.
 * This is useful under low mem conditions as by default we take 1 million
 * free objects allocated. On success REDIS_OK is returned, otherwise
 * REDIS_ERR. */
int tryFreeOneObjectFromFreelist(void) {
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
void freeMemoryIfNeeded(void) {
    while (server.maxmemory && zmalloc_used_memory() > server.maxmemory) {
        int j, k, freed = 0;

        if (tryFreeOneObjectFromFreelist() == REDIS_OK) continue;
        for (j = 0; j < server.dbnum; j++) {
            int minttl = -1;
            sds minkey = NULL;
            robj *keyobj = NULL;
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
                keyobj = createStringObject(minkey,sdslen(minkey));
                dbDelete(server.db+j,keyobj);
                decrRefCount(keyobj);
            }
        }
        if (!freed) return; /* nothing to free... */
    }
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

void createPidFile(void) {
    /* Try to write the pid file in a best-effort way. */
    FILE *fp = fopen(server.pidfile,"w");
    if (fp) {
        fprintf(fp,"%d\n",getpid());
        fclose(fp);
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

void version() {
    printf("Redis server version %s (%s:%d)\n", REDIS_VERSION,
        redisGitSHA1(), atoi(redisGitDirty()) > 0);
    exit(0);
}

void usage() {
    fprintf(stderr,"Usage: ./redis-server [/path/to/redis.conf]\n");
    fprintf(stderr,"       ./redis-server - (read config from stdin)\n");
    exit(1);
}

int main(int argc, char **argv) {
    time_t start;

    initServerConfig();
    sortCommandTable();
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
    if (server.daemonize) createPidFile();
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
void *getMcontextEip(ucontext_t *uc) {
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
#elif defined(__i386__)
    return (void*) uc->uc_mcontext.gregs[14]; /* Linux 32 */
#elif defined(__X86_64__) || defined(__x86_64__)
    return (void*) uc->uc_mcontext.gregs[16]; /* Linux 64 */
#elif defined(__ia64__) /* Linux IA64 */
    return (void*) uc->uc_mcontext.sc_ip;
#else
    return NULL;
#endif
}

void segvHandler(int sig, siginfo_t *info, void *secret) {
    void *trace[100];
    char **messages = NULL;
    int i, trace_size = 0;
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

    for (i=1; i<trace_size; ++i)
        redisLog(REDIS_WARNING,"%s", messages[i]);

    /* free(messages); Don't call free() with possibly corrupted memory. */
    if (server.daemonize) unlink(server.pidfile);
    _exit(0);
}

void sigtermHandler(int sig) {
    REDIS_NOTUSED(sig);

    redisLog(REDIS_WARNING,"SIGTERM received, scheduling shutting down...");
    server.shutdown_asap = 1;
}

void setupSigSegvAction(void) {
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

#else /* HAVE_BACKTRACE */
void setupSigSegvAction(void) {
}
#endif /* HAVE_BACKTRACE */

/* The End */

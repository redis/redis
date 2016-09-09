/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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
#include "cluster.h"
#include "slowlog.h"
#include "bio.h"
#include "latency.h"

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
#include <sys/un.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <locale.h>
#include <sys/socket.h>

/* Our shared "common" objects */

struct sharedObjectsStruct shared;

/* Global vars that are actually used as constants. The following double
 * values are used for double on-disk serialization, and are initialized
 * at runtime to avoid strange compiler optimizations. */

double R_Zero, R_PosInf, R_NegInf, R_Nan;

/*================================= Globals ================================= */

/* Global vars */
struct redisServer server; /* server global state */

/* Our command table.
 *
 * Every entry is composed of the following fields:
 *
 * name: a string representing the command name.
 * function: pointer to the C function implementing the command.
 * arity: number of arguments, it is possible to use -N to say >= N
 * sflags: command flags as string. See below for a table of flags.
 * flags: flags as bitmask. Computed by Redis using the 'sflags' field.
 * get_keys_proc: an optional function to get key arguments from a command.
 *                This is only used when the following three fields are not
 *                enough to specify what arguments are keys.
 * first_key_index: first argument that is a key
 * last_key_index: last argument that is a key
 * key_step: step to get all the keys from first to last argument. For instance
 *           in MSET the step is two since arguments are key,val,key,val,...
 * microseconds: microseconds of total execution time for this command.
 * calls: total number of calls of this command.
 *
 * The flags, microseconds and calls fields are computed by Redis and should
 * always be set to zero.
 *
 * Command flags are expressed using strings where every character represents
 * a flag. Later the populateCommandTable() function will take care of
 * populating the real 'flags' field using this characters.
 *
 * This is the meaning of the flags:
 *
 * w: write command (may modify the key space).
 * r: read command  (will never modify the key space).
 * m: may increase memory usage once called. Don't allow if out of memory.
 * a: admin command, like SAVE or SHUTDOWN.
 * p: Pub/Sub related command.
 * f: force replication of this command, regardless of server.dirty.
 * s: command not allowed in scripts.
 * R: random command. Command is not deterministic, that is, the same command
 *    with the same arguments, with the same key space, may have different
 *    results. For instance SPOP and RANDOMKEY are two random commands.
 * S: Sort command output array if called from script, so that the output
 *    is deterministic.
 * l: Allow command while loading the database.
 * t: Allow command while a slave has stale data but is not allowed to
 *    server this data. Normally no command is accepted in this condition
 *    but just a few.
 * M: Do not automatically propagate the command on MONITOR.
 * k: Perform an implicit ASKING for this command, so the command will be
 *    accepted in cluster mode if the slot is marked as 'importing'.
 * F: Fast command: O(1) or O(log(N)) command that should never delay
 *    its execution as long as the kernel scheduler is giving us time.
 *    Note that commands that may trigger a DEL as a side effect (like SET)
 *    are not fast commands.
 */
struct redisCommand redisCommandTable[] = {
    {"get",getCommand,2,"rF",0,NULL,1,1,1,0,0},
    {"set",setCommand,-3,"wm",0,NULL,1,1,1,0,0},
    {"setnx",setnxCommand,3,"wmF",0,NULL,1,1,1,0,0},
    {"setex",setexCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"psetex",psetexCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"append",appendCommand,3,"wm",0,NULL,1,1,1,0,0},
    {"strlen",strlenCommand,2,"rF",0,NULL,1,1,1,0,0},
    {"del",delCommand,-2,"w",0,NULL,1,-1,1,0,0},
    {"exists",existsCommand,-2,"rF",0,NULL,1,-1,1,0,0},
    {"setbit",setbitCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"getbit",getbitCommand,3,"rF",0,NULL,1,1,1,0,0},
    {"bitfield",bitfieldCommand,-2,"wm",0,NULL,1,1,1,0,0},
    {"setrange",setrangeCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"getrange",getrangeCommand,4,"r",0,NULL,1,1,1,0,0},
    {"substr",getrangeCommand,4,"r",0,NULL,1,1,1,0,0},
    {"incr",incrCommand,2,"wmF",0,NULL,1,1,1,0,0},
    {"decr",decrCommand,2,"wmF",0,NULL,1,1,1,0,0},
    {"mget",mgetCommand,-2,"r",0,NULL,1,-1,1,0,0},
    {"rpush",rpushCommand,-3,"wmF",0,NULL,1,1,1,0,0},
    {"lpush",lpushCommand,-3,"wmF",0,NULL,1,1,1,0,0},
    {"rpushx",rpushxCommand,3,"wmF",0,NULL,1,1,1,0,0},
    {"lpushx",lpushxCommand,3,"wmF",0,NULL,1,1,1,0,0},
    {"linsert",linsertCommand,5,"wm",0,NULL,1,1,1,0,0},
    {"rpop",rpopCommand,2,"wF",0,NULL,1,1,1,0,0},
    {"lpop",lpopCommand,2,"wF",0,NULL,1,1,1,0,0},
    {"brpop",brpopCommand,-3,"ws",0,NULL,1,1,1,0,0},
    {"brpoplpush",brpoplpushCommand,4,"wms",0,NULL,1,2,1,0,0},
    {"blpop",blpopCommand,-3,"ws",0,NULL,1,-2,1,0,0},
    {"llen",llenCommand,2,"rF",0,NULL,1,1,1,0,0},
    {"lindex",lindexCommand,3,"r",0,NULL,1,1,1,0,0},
    {"lset",lsetCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"lrange",lrangeCommand,4,"r",0,NULL,1,1,1,0,0},
    {"ltrim",ltrimCommand,4,"w",0,NULL,1,1,1,0,0},
    {"lrem",lremCommand,4,"w",0,NULL,1,1,1,0,0},
    {"rpoplpush",rpoplpushCommand,3,"wm",0,NULL,1,2,1,0,0},
    {"sadd",saddCommand,-3,"wmF",0,NULL,1,1,1,0,0},
    {"srem",sremCommand,-3,"wF",0,NULL,1,1,1,0,0},
    {"smove",smoveCommand,4,"wF",0,NULL,1,2,1,0,0},
    {"sismember",sismemberCommand,3,"rF",0,NULL,1,1,1,0,0},
    {"scard",scardCommand,2,"rF",0,NULL,1,1,1,0,0},
    {"spop",spopCommand,-2,"wRF",0,NULL,1,1,1,0,0},
    {"srandmember",srandmemberCommand,-2,"rR",0,NULL,1,1,1,0,0},
    {"sinter",sinterCommand,-2,"rS",0,NULL,1,-1,1,0,0},
    {"sinterstore",sinterstoreCommand,-3,"wm",0,NULL,1,-1,1,0,0},
    {"sunion",sunionCommand,-2,"rS",0,NULL,1,-1,1,0,0},
    {"sunionstore",sunionstoreCommand,-3,"wm",0,NULL,1,-1,1,0,0},
    {"sdiff",sdiffCommand,-2,"rS",0,NULL,1,-1,1,0,0},
    {"sdiffstore",sdiffstoreCommand,-3,"wm",0,NULL,1,-1,1,0,0},
    {"smembers",sinterCommand,2,"rS",0,NULL,1,1,1,0,0},
    {"sscan",sscanCommand,-3,"rR",0,NULL,1,1,1,0,0},
    {"zadd",zaddCommand,-4,"wmF",0,NULL,1,1,1,0,0},
    {"zincrby",zincrbyCommand,4,"wmF",0,NULL,1,1,1,0,0},
    {"zrem",zremCommand,-3,"wF",0,NULL,1,1,1,0,0},
    {"zremrangebyscore",zremrangebyscoreCommand,4,"w",0,NULL,1,1,1,0,0},
    {"zremrangebyrank",zremrangebyrankCommand,4,"w",0,NULL,1,1,1,0,0},
    {"zremrangebylex",zremrangebylexCommand,4,"w",0,NULL,1,1,1,0,0},
    {"zunionstore",zunionstoreCommand,-4,"wm",0,zunionInterGetKeys,0,0,0,0,0},
    {"zinterstore",zinterstoreCommand,-4,"wm",0,zunionInterGetKeys,0,0,0,0,0},
    {"zrange",zrangeCommand,-4,"r",0,NULL,1,1,1,0,0},
    {"zrangebyscore",zrangebyscoreCommand,-4,"r",0,NULL,1,1,1,0,0},
    {"zrevrangebyscore",zrevrangebyscoreCommand,-4,"r",0,NULL,1,1,1,0,0},
    {"zrangebylex",zrangebylexCommand,-4,"r",0,NULL,1,1,1,0,0},
    {"zrevrangebylex",zrevrangebylexCommand,-4,"r",0,NULL,1,1,1,0,0},
    {"zcount",zcountCommand,4,"rF",0,NULL,1,1,1,0,0},
    {"zlexcount",zlexcountCommand,4,"rF",0,NULL,1,1,1,0,0},
    {"zrevrange",zrevrangeCommand,-4,"r",0,NULL,1,1,1,0,0},
    {"zcard",zcardCommand,2,"rF",0,NULL,1,1,1,0,0},
    {"zscore",zscoreCommand,3,"rF",0,NULL,1,1,1,0,0},
    {"zrank",zrankCommand,3,"rF",0,NULL,1,1,1,0,0},
    {"zrevrank",zrevrankCommand,3,"rF",0,NULL,1,1,1,0,0},
    {"zscan",zscanCommand,-3,"rR",0,NULL,1,1,1,0,0},
    {"hset",hsetCommand,4,"wmF",0,NULL,1,1,1,0,0},
    {"hsetnx",hsetnxCommand,4,"wmF",0,NULL,1,1,1,0,0},
    {"hget",hgetCommand,3,"rF",0,NULL,1,1,1,0,0},
    {"hmset",hmsetCommand,-4,"wm",0,NULL,1,1,1,0,0},
    {"hmget",hmgetCommand,-3,"r",0,NULL,1,1,1,0,0},
    {"hincrby",hincrbyCommand,4,"wmF",0,NULL,1,1,1,0,0},
    {"hincrbyfloat",hincrbyfloatCommand,4,"wmF",0,NULL,1,1,1,0,0},
    {"hdel",hdelCommand,-3,"wF",0,NULL,1,1,1,0,0},
    {"hlen",hlenCommand,2,"rF",0,NULL,1,1,1,0,0},
    {"hstrlen",hstrlenCommand,3,"rF",0,NULL,1,1,1,0,0},
    {"hkeys",hkeysCommand,2,"rS",0,NULL,1,1,1,0,0},
    {"hvals",hvalsCommand,2,"rS",0,NULL,1,1,1,0,0},
    {"hgetall",hgetallCommand,2,"r",0,NULL,1,1,1,0,0},
    {"hexists",hexistsCommand,3,"rF",0,NULL,1,1,1,0,0},
    {"hscan",hscanCommand,-3,"rR",0,NULL,1,1,1,0,0},
    {"incrby",incrbyCommand,3,"wmF",0,NULL,1,1,1,0,0},
    {"decrby",decrbyCommand,3,"wmF",0,NULL,1,1,1,0,0},
    {"incrbyfloat",incrbyfloatCommand,3,"wmF",0,NULL,1,1,1,0,0},
    {"getset",getsetCommand,3,"wm",0,NULL,1,1,1,0,0},
    {"mset",msetCommand,-3,"wm",0,NULL,1,-1,2,0,0},
    {"msetnx",msetnxCommand,-3,"wm",0,NULL,1,-1,2,0,0},
    {"randomkey",randomkeyCommand,1,"rR",0,NULL,0,0,0,0,0},
    {"select",selectCommand,2,"lF",0,NULL,0,0,0,0,0},
    {"move",moveCommand,3,"wF",0,NULL,1,1,1,0,0},
    {"rename",renameCommand,3,"w",0,NULL,1,2,1,0,0},
    {"renamenx",renamenxCommand,3,"wF",0,NULL,1,2,1,0,0},
    {"expire",expireCommand,3,"wF",0,NULL,1,1,1,0,0},
    {"expireat",expireatCommand,3,"wF",0,NULL,1,1,1,0,0},
    {"pexpire",pexpireCommand,3,"wF",0,NULL,1,1,1,0,0},
    {"pexpireat",pexpireatCommand,3,"wF",0,NULL,1,1,1,0,0},
    {"keys",keysCommand,2,"rS",0,NULL,0,0,0,0,0},
    {"scan",scanCommand,-2,"rR",0,NULL,0,0,0,0,0},
    {"dbsize",dbsizeCommand,1,"rF",0,NULL,0,0,0,0,0},
    {"auth",authCommand,2,"sltF",0,NULL,0,0,0,0,0},
    {"ping",pingCommand,-1,"tF",0,NULL,0,0,0,0,0},
    {"echo",echoCommand,2,"F",0,NULL,0,0,0,0,0},
    {"save",saveCommand,1,"as",0,NULL,0,0,0,0,0},
    {"bgsave",bgsaveCommand,-1,"a",0,NULL,0,0,0,0,0},
    {"bgrewriteaof",bgrewriteaofCommand,1,"a",0,NULL,0,0,0,0,0},
    {"shutdown",shutdownCommand,-1,"alt",0,NULL,0,0,0,0,0},
    {"lastsave",lastsaveCommand,1,"RF",0,NULL,0,0,0,0,0},
    {"type",typeCommand,2,"rF",0,NULL,1,1,1,0,0},
    {"multi",multiCommand,1,"sF",0,NULL,0,0,0,0,0},
    {"exec",execCommand,1,"sM",0,NULL,0,0,0,0,0},
    {"discard",discardCommand,1,"sF",0,NULL,0,0,0,0,0},
    {"sync",syncCommand,1,"ars",0,NULL,0,0,0,0,0},
    {"psync",syncCommand,3,"ars",0,NULL,0,0,0,0,0},
    {"replconf",replconfCommand,-1,"aslt",0,NULL,0,0,0,0,0},
    {"flushdb",flushdbCommand,1,"w",0,NULL,0,0,0,0,0},
    {"flushall",flushallCommand,1,"w",0,NULL,0,0,0,0,0},
    {"sort",sortCommand,-2,"wm",0,sortGetKeys,1,1,1,0,0},
    {"info",infoCommand,-1,"lt",0,NULL,0,0,0,0,0},
    {"monitor",monitorCommand,1,"as",0,NULL,0,0,0,0,0},
    {"ttl",ttlCommand,2,"rF",0,NULL,1,1,1,0,0},
    {"touch",touchCommand,-2,"rF",0,NULL,1,1,1,0,0},
    {"pttl",pttlCommand,2,"rF",0,NULL,1,1,1,0,0},
    {"persist",persistCommand,2,"wF",0,NULL,1,1,1,0,0},
    {"slaveof",slaveofCommand,3,"ast",0,NULL,0,0,0,0,0},
    {"role",roleCommand,1,"lst",0,NULL,0,0,0,0,0},
    {"debug",debugCommand,-1,"as",0,NULL,0,0,0,0,0},
    {"config",configCommand,-2,"lat",0,NULL,0,0,0,0,0},
    {"subscribe",subscribeCommand,-2,"pslt",0,NULL,0,0,0,0,0},
    {"unsubscribe",unsubscribeCommand,-1,"pslt",0,NULL,0,0,0,0,0},
    {"psubscribe",psubscribeCommand,-2,"pslt",0,NULL,0,0,0,0,0},
    {"punsubscribe",punsubscribeCommand,-1,"pslt",0,NULL,0,0,0,0,0},
    {"publish",publishCommand,3,"pltF",0,NULL,0,0,0,0,0},
    {"pubsub",pubsubCommand,-2,"pltR",0,NULL,0,0,0,0,0},
    {"watch",watchCommand,-2,"sF",0,NULL,1,-1,1,0,0},
    {"unwatch",unwatchCommand,1,"sF",0,NULL,0,0,0,0,0},
    {"cluster",clusterCommand,-2,"a",0,NULL,0,0,0,0,0},
    {"restore",restoreCommand,-4,"wm",0,NULL,1,1,1,0,0},
    {"restore-asking",restoreCommand,-4,"wmk",0,NULL,1,1,1,0,0},
    {"migrate",migrateCommand,-6,"w",0,migrateGetKeys,0,0,0,0,0},
    {"asking",askingCommand,1,"F",0,NULL,0,0,0,0,0},
    {"readonly",readonlyCommand,1,"F",0,NULL,0,0,0,0,0},
    {"readwrite",readwriteCommand,1,"F",0,NULL,0,0,0,0,0},
    {"dump",dumpCommand,2,"r",0,NULL,1,1,1,0,0},
    {"object",objectCommand,3,"r",0,NULL,2,2,2,0,0},
    {"client",clientCommand,-2,"as",0,NULL,0,0,0,0,0},
    {"eval",evalCommand,-3,"s",0,evalGetKeys,0,0,0,0,0},
    {"evalsha",evalShaCommand,-3,"s",0,evalGetKeys,0,0,0,0,0},
    {"slowlog",slowlogCommand,-2,"a",0,NULL,0,0,0,0,0},
    {"script",scriptCommand,-2,"s",0,NULL,0,0,0,0,0},
    {"time",timeCommand,1,"RF",0,NULL,0,0,0,0,0},
    {"bitop",bitopCommand,-4,"wm",0,NULL,2,-1,1,0,0},
    {"bitcount",bitcountCommand,-2,"r",0,NULL,1,1,1,0,0},
    {"bitpos",bitposCommand,-3,"r",0,NULL,1,1,1,0,0},
    {"wait",waitCommand,3,"s",0,NULL,0,0,0,0,0},
    {"command",commandCommand,0,"lt",0,NULL,0,0,0,0,0},
    {"geoadd",geoaddCommand,-5,"wm",0,NULL,1,1,1,0,0},
    {"georadius",georadiusCommand,-6,"w",0,NULL,1,1,1,0,0},
    {"georadiusbymember",georadiusByMemberCommand,-5,"w",0,NULL,1,1,1,0,0},
    {"geohash",geohashCommand,-2,"r",0,NULL,1,1,1,0,0},
    {"geopos",geoposCommand,-2,"r",0,NULL,1,1,1,0,0},
    {"geodist",geodistCommand,-4,"r",0,NULL,1,1,1,0,0},
    {"pfselftest",pfselftestCommand,1,"a",0,NULL,0,0,0,0,0},
    {"pfadd",pfaddCommand,-2,"wmF",0,NULL,1,1,1,0,0},
    {"pfcount",pfcountCommand,-2,"r",0,NULL,1,-1,1,0,0},
    {"pfmerge",pfmergeCommand,-2,"wm",0,NULL,1,-1,1,0,0},
    {"pfdebug",pfdebugCommand,-3,"w",0,NULL,0,0,0,0,0},
    {"latency",latencyCommand,-2,"aslt",0,NULL,0,0,0,0,0}
};

struct evictionPoolEntry *evictionPoolAlloc(void);

/*============================ Utility functions ============================ */

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
        pid_t pid = getpid();

        gettimeofday(&tv,NULL);
        off = strftime(buf,sizeof(buf),"%d %b %H:%M:%S.",localtime(&tv.tv_sec));
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
void serverLog(int level, const char *fmt, ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];

    if ((level&0xff) < server.verbosity) return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    serverLogRaw(level,msg);
}

/* Log a fixed message without printf-alike capabilities, in a way that is
 * safe to call from a signal handler.
 *
 * We actually use this only for signals that are not fatal from the point
 * of view of Redis. Signals that are going to kill the server anyway and
 * where we need printf-alike features are served by serverLog(). */
void serverLogFromHandler(int level, const char *msg) {
    int fd;
    int log_to_stdout = server.logfile[0] == '\0';
    char buf[64];

    if ((level&0xff) < server.verbosity || (log_to_stdout && server.daemonize))
        return;
    fd = log_to_stdout ? STDOUT_FILENO :
                         open(server.logfile, O_APPEND|O_CREAT|O_WRONLY, 0644);
    if (fd == -1) return;
    ll2string(buf,sizeof(buf),getpid());
    if (write(fd,buf,strlen(buf)) == -1) goto err;
    if (write(fd,":signal-handler (",17) == -1) goto err;
    ll2string(buf,sizeof(buf),time(NULL));
    if (write(fd,buf,strlen(buf)) == -1) goto err;
    if (write(fd,") ",2) == -1) goto err;
    if (write(fd,msg,strlen(msg)) == -1) goto err;
    if (write(fd,"\n",1) == -1) goto err;
err:
    if (!log_to_stdout) close(fd);
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

/* A case insensitive version used for the command lookup table and other
 * places where case insensitive non binary-safe comparison is needed. */
int dictSdsKeyCaseCompare(void *privdata, const void *key1,
        const void *key2)
{
    DICT_NOTUSED(privdata);

    return strcasecmp(key1, key2) == 0;
}

void dictObjectDestructor(void *privdata, void *val)
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

unsigned int dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char*)key, sdslen((char*)key));
}

int dictEncObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    robj *o1 = (robj*) key1, *o2 = (robj*) key2;
    int cmp;

    if (o1->encoding == OBJ_ENCODING_INT &&
        o2->encoding == OBJ_ENCODING_INT)
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

    if (sdsEncodedObject(o)) {
        return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
    } else {
        if (o->encoding == OBJ_ENCODING_INT) {
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

/* Sets type hash table */
dictType setDictType = {
    dictEncObjHash,            /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictEncObjKeyCompare,      /* key compare */
    dictObjectDestructor, /* key destructor */
    NULL                       /* val destructor */
};

/* Sorted sets hash (note: a skiplist is used in addition to the hash table) */
dictType zsetDictType = {
    dictEncObjHash,            /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictEncObjKeyCompare,      /* key compare */
    dictObjectDestructor, /* key destructor */
    NULL                       /* val destructor */
};

/* Db->dict, keys are sds strings, vals are Redis objects. */
dictType dbDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictObjectDestructor   /* val destructor */
};

/* server.lua_scripts sha (as sds string) -> scripts (as robj) cache. */
dictType shaScriptObjectDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictObjectDestructor   /* val destructor */
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

/* Command table. sds string -> command struct pointer. */
dictType commandTableDictType = {
    dictSdsCaseHash,           /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCaseCompare,     /* key compare */
    dictSdsDestructor,         /* key destructor */
    NULL                       /* val destructor */
};

/* Hash type hash table (note that small hashes are represented with ziplists) */
dictType hashDictType = {
    dictEncObjHash,             /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictEncObjKeyCompare,       /* key compare */
    dictObjectDestructor,  /* key destructor */
    dictObjectDestructor   /* val destructor */
};

/* Keylist hash table type has unencoded redis objects as keys and
 * lists as values. It's used for blocking operations (BLPOP) and to
 * map swapped keys to a list of clients waiting for this keys to be loaded. */
dictType keylistDictType = {
    dictObjHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictObjKeyCompare,          /* key compare */
    dictObjectDestructor,  /* key destructor */
    dictListDestructor          /* val destructor */
};

/* Cluster nodes hash table, mapping nodes addresses 1.2.3.4:6379 to
 * clusterNode structures. */
dictType clusterNodesDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL                        /* val destructor */
};

/* Cluster re-addition blacklist. This maps node IDs to the time
 * we can re-add this node. The goal is to avoid readding a removed
 * node for some time. */
dictType clusterNodesBlackListDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL                        /* val destructor */
};

/* Migrate cache dict type. */
dictType migrateCacheDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL                        /* val destructor */
};

/* Replication cached script dict (server.repl_scriptcache_dict).
 * Keys are sds SHA1 strings, while values are not used at all in the current
 * implementation. */
dictType replScriptCacheDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL                        /* val destructor */
};

int htNeedsResize(dict *dict) {
    long long size, used;

    size = dictSlots(dict);
    used = dictSize(dict);
    return (size > DICT_HT_INITIAL_SIZE &&
            (used*100/size < HASHTABLE_MIN_FILL));
}

/* If the percentage of used slots in the HT reaches HASHTABLE_MIN_FILL
 * we resize the hash table to save memory */
void tryResizeHashTables(int dbid) {
    if (htNeedsResize(server.db[dbid].dict))
        dictResize(server.db[dbid].dict);
    if (htNeedsResize(server.db[dbid].expires))
        dictResize(server.db[dbid].expires);
}

/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use 1 millisecond
 * of CPU time at every call of this function to perform some rehahsing.
 *
 * The function returns 1 if some rehashing was performed, otherwise 0
 * is returned. */
int incrementallyRehash(int dbid) {
    /* Keys dictionary */
    if (dictIsRehashing(server.db[dbid].dict)) {
        dictRehashMilliseconds(server.db[dbid].dict,1);
        return 1; /* already used our millisecond for this loop... */
    }
    /* Expires */
    if (dictIsRehashing(server.db[dbid].expires)) {
        dictRehashMilliseconds(server.db[dbid].expires,1);
        return 1; /* already used our millisecond for this loop... */
    }
    return 0;
}

/* This function is called once a background process of some kind terminates,
 * as we want to avoid resizing the hash tables when there is a child in order
 * to play well with copy-on-write (otherwise when a resize happens lots of
 * memory pages are copied). The goal of this function is to update the ability
 * for dict.c to resize the hash tables accordingly to the fact we have o not
 * running childs. */
void updateDictResizePolicy(void) {
    if (server.rdb_child_pid == -1 && server.aof_child_pid == -1)
        dictEnableResize();
    else
        dictDisableResize();
}

/* ======================= Cron: called every 100 ms ======================== */

/* Helper function for the activeExpireCycle() function.
 * This function will try to expire the key that is stored in the hash table
 * entry 'de' of the 'expires' hash table of a Redis database.
 *
 * If the key is found to be expired, it is removed from the database and
 * 1 is returned. Otherwise no operation is performed and 0 is returned.
 *
 * When a key is expired, server.stat_expiredkeys is incremented.
 *
 * The parameter 'now' is the current time in milliseconds as is passed
 * to the function to avoid too many gettimeofday() syscalls. */
int activeExpireCycleTryExpire(redisDb *db, dictEntry *de, long long now) {
    long long t = dictGetSignedIntegerVal(de);
    if (now > t) {
        sds key = dictGetKey(de);
        robj *keyobj = createStringObject(key,sdslen(key));

        propagateExpire(db,keyobj);
        dbDelete(db,keyobj);
        notifyKeyspaceEvent(NOTIFY_EXPIRED,
            "expired",keyobj,db->id);
        decrRefCount(keyobj);
        server.stat_expiredkeys++;
        return 1;
    } else {
        return 0;
    }
}

/* Try to expire a few timed out keys. The algorithm used is adaptive and
 * will use few CPU cycles if there are few expiring keys, otherwise
 * it will get more aggressive to avoid that too much memory is used by
 * keys that can be removed from the keyspace.
 *
 * No more than CRON_DBS_PER_CALL databases are tested at every
 * iteration.
 *
 * This kind of call is used when Redis detects that timelimit_exit is
 * true, so there is more work to do, and we do it more incrementally from
 * the beforeSleep() function of the event loop.
 *
 * Expire cycle type:
 *
 * If type is ACTIVE_EXPIRE_CYCLE_FAST the function will try to run a
 * "fast" expire cycle that takes no longer than EXPIRE_FAST_CYCLE_DURATION
 * microseconds, and is not repeated again before the same amount of time.
 *
 * If type is ACTIVE_EXPIRE_CYCLE_SLOW, that normal expire cycle is
 * executed, where the time limit is a percentage of the REDIS_HZ period
 * as specified by the REDIS_EXPIRELOOKUPS_TIME_PERC define. */

void activeExpireCycle(int type) {
    /* This function has some global state in order to continue the work
     * incrementally across calls. */
    static unsigned int current_db = 0; /* Last DB tested. */
    static int timelimit_exit = 0;      /* Time limit hit in previous call? */
    static long long last_fast_cycle = 0; /* When last fast cycle ran. */

    int j, iteration = 0;
    int dbs_per_call = CRON_DBS_PER_CALL;
    long long start = ustime(), timelimit;

    if (type == ACTIVE_EXPIRE_CYCLE_FAST) {
        /* Don't start a fast cycle if the previous cycle did not exited
         * for time limt. Also don't repeat a fast cycle for the same period
         * as the fast cycle total duration itself. */
        if (!timelimit_exit) return;
        if (start < last_fast_cycle + ACTIVE_EXPIRE_CYCLE_FAST_DURATION*2) return;
        last_fast_cycle = start;
    }

    /* We usually should test CRON_DBS_PER_CALL per iteration, with
     * two exceptions:
     *
     * 1) Don't test more DBs than we have.
     * 2) If last time we hit the time limit, we want to scan all DBs
     * in this iteration, as there is work to do in some DB and we don't want
     * expired keys to use memory for too much time. */
    if (dbs_per_call > server.dbnum || timelimit_exit)
        dbs_per_call = server.dbnum;

    /* We can use at max ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC percentage of CPU time
     * per iteration. Since this function gets called with a frequency of
     * server.hz times per second, the following is the max amount of
     * microseconds we can spend in this function. */
    timelimit = 1000000*ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC/server.hz/100;
    timelimit_exit = 0;
    if (timelimit <= 0) timelimit = 1;

    if (type == ACTIVE_EXPIRE_CYCLE_FAST)
        timelimit = ACTIVE_EXPIRE_CYCLE_FAST_DURATION; /* in microseconds. */

    for (j = 0; j < dbs_per_call; j++) {
        int expired;
        redisDb *db = server.db+(current_db % server.dbnum);

        /* Increment the DB now so we are sure if we run out of time
         * in the current DB we'll restart from the next. This allows to
         * distribute the time evenly across DBs. */
        current_db++;

        /* Continue to expire if at the end of the cycle more than 25%
         * of the keys were expired. */
        do {
            unsigned long num, slots;
            long long now, ttl_sum;
            int ttl_samples;

            /* If there is nothing to expire try next DB ASAP. */
            if ((num = dictSize(db->expires)) == 0) {
                db->avg_ttl = 0;
                break;
            }
            slots = dictSlots(db->expires);
            now = mstime();

            /* When there are less than 1% filled slots getting random
             * keys is expensive, so stop here waiting for better times...
             * The dictionary will be resized asap. */
            if (num && slots > DICT_HT_INITIAL_SIZE &&
                (num*100/slots < 1)) break;

            /* The main collection cycle. Sample random keys among keys
             * with an expire set, checking for expired ones. */
            expired = 0;
            ttl_sum = 0;
            ttl_samples = 0;

            if (num > ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP)
                num = ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP;

            while (num--) {
                dictEntry *de;
                long long ttl;

                if ((de = dictGetRandomKey(db->expires)) == NULL) break;
                ttl = dictGetSignedIntegerVal(de)-now;
                if (activeExpireCycleTryExpire(db,de,now)) expired++;
                if (ttl > 0) {
                    /* We want the average TTL of keys yet not expired. */
                    ttl_sum += ttl;
                    ttl_samples++;
                }
            }

            /* Update the average TTL stats for this database. */
            if (ttl_samples) {
                long long avg_ttl = ttl_sum/ttl_samples;

                /* Do a simple running average with a few samples.
                 * We just use the current estimate with a weight of 2%
                 * and the previous estimate with a weight of 98%. */
                if (db->avg_ttl == 0) db->avg_ttl = avg_ttl;
                db->avg_ttl = (db->avg_ttl/50)*49 + (avg_ttl/50);
            }

            /* We can't block forever here even if there are many keys to
             * expire. So after a given amount of milliseconds return to the
             * caller waiting for the other active expire cycle. */
            iteration++;
            if ((iteration & 0xf) == 0) { /* check once every 16 iterations. */
                long long elapsed = ustime()-start;

                latencyAddSampleIfNeeded("expire-cycle",elapsed/1000);
                if (elapsed > timelimit) timelimit_exit = 1;
            }
            if (timelimit_exit) return;
            /* We don't repeat the cycle if there are less than 25% of keys
             * found expired in the current DB. */
        } while (expired > ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP/4);
    }
}

unsigned int getLRUClock(void) {
    return (mstime()/LRU_CLOCK_RESOLUTION) & LRU_CLOCK_MAX;
}

/* Add a sample to the operations per second array of samples. */
void trackInstantaneousMetric(int metric, long long current_reading) {
    long long t = mstime() - server.inst_metric[metric].last_sample_time;
    long long ops = current_reading -
                    server.inst_metric[metric].last_sample_count;
    long long ops_sec;

    ops_sec = t > 0 ? (ops*1000/t) : 0;

    server.inst_metric[metric].samples[server.inst_metric[metric].idx] =
        ops_sec;
    server.inst_metric[metric].idx++;
    server.inst_metric[metric].idx %= STATS_METRIC_SAMPLES;
    server.inst_metric[metric].last_sample_time = mstime();
    server.inst_metric[metric].last_sample_count = current_reading;
}

/* Return the mean of all the samples. */
long long getInstantaneousMetric(int metric) {
    int j;
    long long sum = 0;

    for (j = 0; j < STATS_METRIC_SAMPLES; j++)
        sum += server.inst_metric[metric].samples[j];
    return sum / STATS_METRIC_SAMPLES;
}

/* Check for timeouts. Returns non-zero if the client was terminated.
 * The function gets the current time in milliseconds as argument since
 * it gets called multiple times in a loop, so calling gettimeofday() for
 * each iteration would be costly without any actual gain. */
int clientsCronHandleTimeout(client *c, mstime_t now_ms) {
    time_t now = now_ms/1000;

    if (server.maxidletime &&
        !(c->flags & CLIENT_SLAVE) &&    /* no timeout for slaves */
        !(c->flags & CLIENT_MASTER) &&   /* no timeout for masters */
        !(c->flags & CLIENT_BLOCKED) &&  /* no timeout for BLPOP */
        !(c->flags & CLIENT_PUBSUB) &&   /* no timeout for Pub/Sub clients */
        (now - c->lastinteraction > server.maxidletime))
    {
        serverLog(LL_VERBOSE,"Closing idle client");
        freeClient(c);
        return 1;
    } else if (c->flags & CLIENT_BLOCKED) {
        /* Blocked OPS timeout is handled with milliseconds resolution.
         * However note that the actual resolution is limited by
         * server.hz. */

        if (c->bpop.timeout != 0 && c->bpop.timeout < now_ms) {
            /* Handle blocking operation specific timeout. */
            replyToBlockedClientTimedOut(c);
            unblockClient(c);
        } else if (server.cluster_enabled) {
            /* Cluster: handle unblock & redirect of clients blocked
             * into keys no longer served by this server. */
            if (clusterRedirectBlockedClientIfNeeded(c))
                unblockClient(c);
        }
    }
    return 0;
}

/* The client query buffer is an sds.c string that can end with a lot of
 * free space not used, this function reclaims space if needed.
 *
 * The function always returns 0 as it never terminates the client. */
int clientsCronResizeQueryBuffer(client *c) {
    size_t querybuf_size = sdsAllocSize(c->querybuf);
    time_t idletime = server.unixtime - c->lastinteraction;

    /* There are two conditions to resize the query buffer:
     * 1) Query buffer is > BIG_ARG and too big for latest peak.
     * 2) Client is inactive and the buffer is bigger than 1k. */
    if (((querybuf_size > PROTO_MBULK_BIG_ARG) &&
         (querybuf_size/(c->querybuf_peak+1)) > 2) ||
         (querybuf_size > 1024 && idletime > 2))
    {
        /* Only resize the query buffer if it is actually wasting space. */
        if (sdsavail(c->querybuf) > 1024) {
            c->querybuf = sdsRemoveFreeSpace(c->querybuf);
        }
    }
    /* Reset the peak again to capture the peak memory usage in the next
     * cycle. */
    c->querybuf_peak = 0;
    return 0;
}

#define CLIENTS_CRON_MIN_ITERATIONS 5
void clientsCron(void) {
    /* Make sure to process at least numclients/server.hz of clients
     * per call. Since this function is called server.hz times per second
     * we are sure that in the worst case we process all the clients in 1
     * second. */
    int numclients = listLength(server.clients);
    int iterations = numclients/server.hz;
    mstime_t now = mstime();

    /* Process at least a few clients while we are at it, even if we need
     * to process less than CLIENTS_CRON_MIN_ITERATIONS to meet our contract
     * of processing each client once per second. */
    if (iterations < CLIENTS_CRON_MIN_ITERATIONS)
        iterations = (numclients < CLIENTS_CRON_MIN_ITERATIONS) ?
                     numclients : CLIENTS_CRON_MIN_ITERATIONS;

    while(listLength(server.clients) && iterations--) {
        client *c;
        listNode *head;

        /* Rotate the list, take the current head, process.
         * This way if the client must be removed from the list it's the
         * first element and we don't incur into O(N) computation. */
        listRotate(server.clients);
        head = listFirst(server.clients);
        c = listNodeValue(head);
        /* The following functions do different service checks on the client.
         * The protocol is that they return non-zero if the client was
         * terminated. */
        if (clientsCronHandleTimeout(c,now)) continue;
        if (clientsCronResizeQueryBuffer(c)) continue;
    }
}

/* This function handles 'background' operations we are required to do
 * incrementally in Redis databases, such as active key expiring, resizing,
 * rehashing. */
void databasesCron(void) {
    /* Expire keys by random sampling. Not required for slaves
     * as master will synthesize DELs for us. */
    if (server.active_expire_enabled && server.masterhost == NULL)
        activeExpireCycle(ACTIVE_EXPIRE_CYCLE_SLOW);

    /* Perform hash tables rehashing if needed, but only if there are no
     * other processes saving the DB on disk. Otherwise rehashing is bad
     * as will cause a lot of copy-on-write of memory pages. */
    if (server.rdb_child_pid == -1 && server.aof_child_pid == -1) {
        /* We use global counters so if we stop the computation at a given
         * DB we'll be able to start from the successive in the next
         * cron loop iteration. */
        static unsigned int resize_db = 0;
        static unsigned int rehash_db = 0;
        int dbs_per_call = CRON_DBS_PER_CALL;
        int j;

        /* Don't test more DBs than we have. */
        if (dbs_per_call > server.dbnum) dbs_per_call = server.dbnum;

        /* Resize */
        for (j = 0; j < dbs_per_call; j++) {
            tryResizeHashTables(resize_db % server.dbnum);
            resize_db++;
        }

        /* Rehash */
        if (server.activerehashing) {
            for (j = 0; j < dbs_per_call; j++) {
                int work_done = incrementallyRehash(rehash_db % server.dbnum);
                rehash_db++;
                if (work_done) {
                    /* If the function did some work, stop here, we'll do
                     * more at the next cron loop. */
                    break;
                }
            }
        }
    }
}

/* We take a cached value of the unix time in the global state because with
 * virtual memory and aging there is to store the current time in objects at
 * every object access, and accuracy is not needed. To access a global var is
 * a lot faster than calling time(NULL) */
void updateCachedTime(void) {
    server.unixtime = time(NULL);
    server.mstime = mstime();
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

    /* Update the time cache. */
    updateCachedTime();

    run_with_period(100) {
        trackInstantaneousMetric(STATS_METRIC_COMMAND,server.stat_numcommands);
        trackInstantaneousMetric(STATS_METRIC_NET_INPUT,
                server.stat_net_input_bytes);
        trackInstantaneousMetric(STATS_METRIC_NET_OUTPUT,
                server.stat_net_output_bytes);
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

    /* Record the max memory used since the server was started. */
    if (zmalloc_used_memory() > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used_memory();

    /* Sample the RSS here since this is a relatively slow call. */
    server.resident_set_size = zmalloc_get_rss();

    /* We received a SIGTERM, shutting down here in a safe way, as it is
     * not ok doing so inside the signal handler. */
    if (server.shutdown_asap) {
        if (prepareForShutdown(SHUTDOWN_NOFLAGS) == C_OK) exit(0);
        serverLog(LL_WARNING,"SIGTERM received but errors trying to shut down the server, check the logs for more information");
        server.shutdown_asap = 0;
    }

    /* Show some info about non-empty databases */
    run_with_period(5000) {
        for (j = 0; j < server.dbnum; j++) {
            long long size, used, vkeys;

            size = dictSlots(server.db[j].dict);
            used = dictSize(server.db[j].dict);
            vkeys = dictSize(server.db[j].expires);
            if (used || vkeys) {
                serverLog(LL_VERBOSE,"DB %d: %lld keys (%lld volatile) in %lld slots HT.",j,used,vkeys,size);
                /* dictPrintStats(server.dict); */
            }
        }
    }

    /* Show information about connected clients */
    if (!server.sentinel_mode) {
        run_with_period(5000) {
            serverLog(LL_VERBOSE,
                "%lu clients connected (%lu slaves), %zu bytes in use",
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
    if (server.rdb_child_pid == -1 && server.aof_child_pid == -1 &&
        server.aof_rewrite_scheduled)
    {
        rewriteAppendOnlyFileBackground();
    }

    /* Check if a background saving or AOF rewrite in progress terminated. */
    if (server.rdb_child_pid != -1 || server.aof_child_pid != -1 ||
        ldbPendingChildren())
    {
        int statloc;
        pid_t pid;

        if ((pid = wait3(&statloc,WNOHANG,NULL)) != 0) {
            int exitcode = WEXITSTATUS(statloc);
            int bysignal = 0;

            if (WIFSIGNALED(statloc)) bysignal = WTERMSIG(statloc);

            if (pid == -1) {
                serverLog(LL_WARNING,"wait3() returned an error: %s. "
                    "rdb_child_pid = %d, aof_child_pid = %d",
                    strerror(errno),
                    (int) server.rdb_child_pid,
                    (int) server.aof_child_pid);
            } else if (pid == server.rdb_child_pid) {
                backgroundSaveDoneHandler(exitcode,bysignal);
            } else if (pid == server.aof_child_pid) {
                backgroundRewriteDoneHandler(exitcode,bysignal);
            } else {
                if (!ldbRemoveChild(pid)) {
                    serverLog(LL_WARNING,
                        "Warning, detected child with unmatched pid: %ld",
                        (long)pid);
                }
            }
            updateDictResizePolicy();
        }
    } else {
        /* If there is not a background saving/rewrite in progress check if
         * we have to save/rewrite now */
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
                rdbSaveBackground(server.rdb_filename);
                break;
            }
         }

         /* Trigger an AOF rewrite if needed */
         if (server.rdb_child_pid == -1 &&
             server.aof_child_pid == -1 &&
             server.aof_rewrite_perc &&
             server.aof_current_size > server.aof_rewrite_min_size)
         {
            long long base = server.aof_rewrite_base_size ?
                            server.aof_rewrite_base_size : 1;
            long long growth = (server.aof_current_size*100/base) - 100;
            if (growth >= server.aof_rewrite_perc) {
                serverLog(LL_NOTICE,"Starting automatic rewriting of AOF on %lld%% growth",growth);
                rewriteAppendOnlyFileBackground();
            }
         }
    }


    /* AOF postponed flush: Try at every cron cycle if the slow fsync
     * completed. */
    if (server.aof_flush_postponed_start) flushAppendOnlyFile(0);

    /* AOF write errors: in this case we have a buffer to flush as well and
     * clear the AOF error in case of success to make the DB writable again,
     * however to try every second is enough in case of 'hz' is set to
     * an higher frequency. */
    run_with_period(1000) {
        if (server.aof_last_write_status == C_ERR)
            flushAppendOnlyFile(0);
    }

    /* Close clients that need to be closed asynchronous */
    freeClientsInAsyncFreeQueue();

    /* Clear the paused clients flag if needed. */
    clientsArePaused(); /* Don't check return value, just use the side effect. */

    /* Replication cron function -- used to reconnect to master,
     * detect transfer failures, start background RDB transfers and so forth. */
    run_with_period(1000) replicationCron();

    /* Run the Redis Cluster cron. */
    run_with_period(100) {
        if (server.cluster_enabled) clusterCron();
    }

    /* Run the Sentinel timer if we are in sentinel mode. */
    run_with_period(100) {
        if (server.sentinel_mode) sentinelTimer();
    }

    /* Cleanup expired MIGRATE cached sockets. */
    run_with_period(1000) {
        migrateCloseTimedoutSockets();
    }

    /* Start a scheduled BGSAVE if the corresponding flag is set. This is
     * useful when we are forced to postpone a BGSAVE because an AOF
     * rewrite is in progress.
     *
     * Note: this code must be after the replicationCron() call above so
     * make sure when refactoring this file to keep this order. This is useful
     * because we want to give priority to RDB savings for replication. */
    if (server.rdb_child_pid == -1 && server.aof_child_pid == -1 &&
        server.rdb_bgsave_scheduled &&
        (server.unixtime-server.lastbgsave_try > CONFIG_BGSAVE_RETRY_DELAY ||
         server.lastbgsave_status == C_OK))
    {
        if (rdbSaveBackground(server.rdb_filename) == C_OK)
            server.rdb_bgsave_scheduled = 0;
    }

    server.cronloops++;
    return 1000/server.hz;
}

/* This function gets called every time Redis is entering the
 * main loop of the event driven library, that is, before to sleep
 * for ready file descriptors. */
void beforeSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);

    /* Call the Redis Cluster before sleep function. Note that this function
     * may change the state of Redis Cluster (from ok to fail or vice versa),
     * so it's a good idea to call it before serving the unblocked clients
     * later in this function. */
    if (server.cluster_enabled) clusterBeforeSleep();

    /* Run a fast expire cycle (the called function will return
     * ASAP if a fast cycle is not needed). */
    if (server.active_expire_enabled && server.masterhost == NULL)
        activeExpireCycle(ACTIVE_EXPIRE_CYCLE_FAST);

    /* Send all the slaves an ACK request if at least one client blocked
     * during the previous event loop iteration. */
    if (server.get_ack_from_slaves) {
        robj *argv[3];

        argv[0] = createStringObject("REPLCONF",8);
        argv[1] = createStringObject("GETACK",6);
        argv[2] = createStringObject("*",1); /* Not used argument. */
        replicationFeedSlaves(server.slaves, server.slaveseldb, argv, 3);
        decrRefCount(argv[0]);
        decrRefCount(argv[1]);
        decrRefCount(argv[2]);
        server.get_ack_from_slaves = 0;
    }

    /* Unblock all the clients blocked for synchronous replication
     * in WAIT. */
    if (listLength(server.clients_waiting_acks))
        processClientsWaitingReplicas();

    /* Try to process pending commands for clients that were just unblocked. */
    if (listLength(server.unblocked_clients))
        processUnblockedClients();

    /* Write the AOF buffer on disk */
    flushAppendOnlyFile(0);

    /* Handle writes with pending output buffers. */
    handleClientsWithPendingWrites();
}

/* =========================== Server initialization ======================== */

void createSharedObjects(void) {
    int j;

    shared.crlf = createObject(OBJ_STRING,sdsnew("\r\n"));
    shared.ok = createObject(OBJ_STRING,sdsnew("+OK\r\n"));
    shared.err = createObject(OBJ_STRING,sdsnew("-ERR\r\n"));
    shared.emptybulk = createObject(OBJ_STRING,sdsnew("$0\r\n\r\n"));
    shared.czero = createObject(OBJ_STRING,sdsnew(":0\r\n"));
    shared.cone = createObject(OBJ_STRING,sdsnew(":1\r\n"));
    shared.cnegone = createObject(OBJ_STRING,sdsnew(":-1\r\n"));
    shared.nullbulk = createObject(OBJ_STRING,sdsnew("$-1\r\n"));
    shared.nullmultibulk = createObject(OBJ_STRING,sdsnew("*-1\r\n"));
    shared.emptymultibulk = createObject(OBJ_STRING,sdsnew("*0\r\n"));
    shared.pong = createObject(OBJ_STRING,sdsnew("+PONG\r\n"));
    shared.queued = createObject(OBJ_STRING,sdsnew("+QUEUED\r\n"));
    shared.emptyscan = createObject(OBJ_STRING,sdsnew("*2\r\n$1\r\n0\r\n*0\r\n"));
    shared.wrongtypeerr = createObject(OBJ_STRING,sdsnew(
        "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"));
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
    shared.slowscripterr = createObject(OBJ_STRING,sdsnew(
        "-BUSY Redis is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.\r\n"));
    shared.masterdownerr = createObject(OBJ_STRING,sdsnew(
        "-MASTERDOWN Link with MASTER is down and slave-serve-stale-data is set to 'no'.\r\n"));
    shared.bgsaveerr = createObject(OBJ_STRING,sdsnew(
        "-MISCONF Redis is configured to save RDB snapshots, but is currently not able to persist on disk. Commands that may modify the data set are disabled. Please check Redis logs for details about the error.\r\n"));
    shared.roslaveerr = createObject(OBJ_STRING,sdsnew(
        "-READONLY You can't write against a read only slave.\r\n"));
    shared.noautherr = createObject(OBJ_STRING,sdsnew(
        "-NOAUTH Authentication required.\r\n"));
    shared.oomerr = createObject(OBJ_STRING,sdsnew(
        "-OOM command not allowed when used memory > 'maxmemory'.\r\n"));
    shared.execaborterr = createObject(OBJ_STRING,sdsnew(
        "-EXECABORT Transaction discarded because of previous errors.\r\n"));
    shared.noreplicaserr = createObject(OBJ_STRING,sdsnew(
        "-NOREPLICAS Not enough good slaves to write.\r\n"));
    shared.busykeyerr = createObject(OBJ_STRING,sdsnew(
        "-BUSYKEY Target key name already exists.\r\n"));
    shared.space = createObject(OBJ_STRING,sdsnew(" "));
    shared.colon = createObject(OBJ_STRING,sdsnew(":"));
    shared.plus = createObject(OBJ_STRING,sdsnew("+"));

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
    shared.psubscribebulk = createStringObject("$10\r\npsubscribe\r\n",17);
    shared.punsubscribebulk = createStringObject("$12\r\npunsubscribe\r\n",19);
    shared.del = createStringObject("DEL",3);
    shared.rpop = createStringObject("RPOP",4);
    shared.lpop = createStringObject("LPOP",4);
    shared.lpush = createStringObject("LPUSH",5);
    for (j = 0; j < OBJ_SHARED_INTEGERS; j++) {
        shared.integers[j] = createObject(OBJ_STRING,(void*)(long)j);
        shared.integers[j]->encoding = OBJ_ENCODING_INT;
    }
    for (j = 0; j < OBJ_SHARED_BULKHDR_LEN; j++) {
        shared.mbulkhdr[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),"*%d\r\n",j));
        shared.bulkhdr[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),"$%d\r\n",j));
    }
    /* The following two shared objects, minstring and maxstrings, are not
     * actually used for their value but as a special object meaning
     * respectively the minimum possible string and the maximum possible
     * string in string comparisons for the ZRANGEBYLEX command. */
    shared.minstring = createStringObject("minstring",9);
    shared.maxstring = createStringObject("maxstring",9);
}

void initServerConfig(void) {
    int j;

    getRandomHexChars(server.runid,CONFIG_RUN_ID_SIZE);
    server.configfile = NULL;
    server.executable = NULL;
    server.hz = CONFIG_DEFAULT_HZ;
    server.runid[CONFIG_RUN_ID_SIZE] = '\0';
    server.arch_bits = (sizeof(long) == 8) ? 64 : 32;
    server.port = CONFIG_DEFAULT_SERVER_PORT;
    server.tcp_backlog = CONFIG_DEFAULT_TCP_BACKLOG;
    server.bindaddr_count = 0;
    server.unixsocket = NULL;
    server.unixsocketperm = CONFIG_DEFAULT_UNIX_SOCKET_PERM;
    server.ipfd_count = 0;
    server.sofd = -1;
    server.protected_mode = CONFIG_DEFAULT_PROTECTED_MODE;
    server.dbnum = CONFIG_DEFAULT_DBNUM;
    server.verbosity = CONFIG_DEFAULT_VERBOSITY;
    server.maxidletime = CONFIG_DEFAULT_CLIENT_TIMEOUT;
    server.tcpkeepalive = CONFIG_DEFAULT_TCP_KEEPALIVE;
    server.active_expire_enabled = 1;
    server.client_max_querybuf_len = PROTO_MAX_QUERYBUF_LEN;
    server.saveparams = NULL;
    server.loading = 0;
    server.logfile = zstrdup(CONFIG_DEFAULT_LOGFILE);
    server.syslog_enabled = CONFIG_DEFAULT_SYSLOG_ENABLED;
    server.syslog_ident = zstrdup(CONFIG_DEFAULT_SYSLOG_IDENT);
    server.syslog_facility = LOG_LOCAL0;
    server.daemonize = CONFIG_DEFAULT_DAEMONIZE;
    server.supervised = 0;
    server.supervised_mode = SUPERVISED_NONE;
    server.aof_state = AOF_OFF;
    server.aof_fsync = CONFIG_DEFAULT_AOF_FSYNC;
    server.aof_no_fsync_on_rewrite = CONFIG_DEFAULT_AOF_NO_FSYNC_ON_REWRITE;
    server.aof_rewrite_perc = AOF_REWRITE_PERC;
    server.aof_rewrite_min_size = AOF_REWRITE_MIN_SIZE;
    server.aof_rewrite_base_size = 0;
    server.aof_rewrite_scheduled = 0;
    server.aof_last_fsync = time(NULL);
    server.aof_rewrite_time_last = -1;
    server.aof_rewrite_time_start = -1;
    server.aof_lastbgrewrite_status = C_OK;
    server.aof_delayed_fsync = 0;
    server.aof_fd = -1;
    server.aof_selected_db = -1; /* Make sure the first time will not match */
    server.aof_flush_postponed_start = 0;
    server.aof_rewrite_incremental_fsync = CONFIG_DEFAULT_AOF_REWRITE_INCREMENTAL_FSYNC;
    server.aof_load_truncated = CONFIG_DEFAULT_AOF_LOAD_TRUNCATED;
    server.pidfile = NULL;
    server.rdb_filename = zstrdup(CONFIG_DEFAULT_RDB_FILENAME);
    server.aof_filename = zstrdup(CONFIG_DEFAULT_AOF_FILENAME);
    server.requirepass = NULL;
    server.rdb_compression = CONFIG_DEFAULT_RDB_COMPRESSION;
    server.rdb_checksum = CONFIG_DEFAULT_RDB_CHECKSUM;
    server.stop_writes_on_bgsave_err = CONFIG_DEFAULT_STOP_WRITES_ON_BGSAVE_ERROR;
    server.activerehashing = CONFIG_DEFAULT_ACTIVE_REHASHING;
    server.notify_keyspace_events = 0;
    server.maxclients = CONFIG_DEFAULT_MAX_CLIENTS;
    server.bpop_blocked_clients = 0;
    server.maxmemory = CONFIG_DEFAULT_MAXMEMORY;
    server.maxmemory_policy = CONFIG_DEFAULT_MAXMEMORY_POLICY;
    server.maxmemory_samples = CONFIG_DEFAULT_MAXMEMORY_SAMPLES;
    server.hash_max_ziplist_entries = OBJ_HASH_MAX_ZIPLIST_ENTRIES;
    server.hash_max_ziplist_value = OBJ_HASH_MAX_ZIPLIST_VALUE;
    server.list_max_ziplist_size = OBJ_LIST_MAX_ZIPLIST_SIZE;
    server.list_compress_depth = OBJ_LIST_COMPRESS_DEPTH;
    server.set_max_intset_entries = OBJ_SET_MAX_INTSET_ENTRIES;
    server.zset_max_ziplist_entries = OBJ_ZSET_MAX_ZIPLIST_ENTRIES;
    server.zset_max_ziplist_value = OBJ_ZSET_MAX_ZIPLIST_VALUE;
    server.hll_sparse_max_bytes = CONFIG_DEFAULT_HLL_SPARSE_MAX_BYTES;
    server.shutdown_asap = 0;
    server.repl_ping_slave_period = CONFIG_DEFAULT_REPL_PING_SLAVE_PERIOD;
    server.repl_timeout = CONFIG_DEFAULT_REPL_TIMEOUT;
    server.repl_min_slaves_to_write = CONFIG_DEFAULT_MIN_SLAVES_TO_WRITE;
    server.repl_min_slaves_max_lag = CONFIG_DEFAULT_MIN_SLAVES_MAX_LAG;
    server.cluster_enabled = 0;
    server.cluster_node_timeout = CLUSTER_DEFAULT_NODE_TIMEOUT;
    server.cluster_migration_barrier = CLUSTER_DEFAULT_MIGRATION_BARRIER;
    server.cluster_slave_validity_factor = CLUSTER_DEFAULT_SLAVE_VALIDITY;
    server.cluster_require_full_coverage = CLUSTER_DEFAULT_REQUIRE_FULL_COVERAGE;
    server.cluster_configfile = zstrdup(CONFIG_DEFAULT_CLUSTER_CONFIG_FILE);
    server.migrate_cached_sockets = dictCreate(&migrateCacheDictType,NULL);
    server.next_client_id = 1; /* Client IDs, start from 1 .*/
    server.loading_process_events_interval_bytes = (1024*1024*2);

    server.lruclock = getLRUClock();
    resetServerSaveParams();

    appendServerSaveParams(60*60,1);  /* save after 1 hour and 1 change */
    appendServerSaveParams(300,100);  /* save after 5 minutes and 100 changes */
    appendServerSaveParams(60,10000); /* save after 1 minute and 10000 changes */
    /* Replication related */
    server.masterauth = NULL;
    server.masterhost = NULL;
    server.masterport = 6379;
    server.master = NULL;
    server.cached_master = NULL;
    server.repl_master_initial_offset = -1;
    server.repl_state = REPL_STATE_NONE;
    server.repl_syncio_timeout = CONFIG_REPL_SYNCIO_TIMEOUT;
    server.repl_serve_stale_data = CONFIG_DEFAULT_SLAVE_SERVE_STALE_DATA;
    server.repl_slave_ro = CONFIG_DEFAULT_SLAVE_READ_ONLY;
    server.repl_down_since = 0; /* Never connected, repl is down since EVER. */
    server.repl_disable_tcp_nodelay = CONFIG_DEFAULT_REPL_DISABLE_TCP_NODELAY;
    server.repl_diskless_sync = CONFIG_DEFAULT_REPL_DISKLESS_SYNC;
    server.repl_diskless_sync_delay = CONFIG_DEFAULT_REPL_DISKLESS_SYNC_DELAY;
    server.slave_priority = CONFIG_DEFAULT_SLAVE_PRIORITY;
    server.slave_announce_ip = CONFIG_DEFAULT_SLAVE_ANNOUNCE_IP;
    server.slave_announce_port = CONFIG_DEFAULT_SLAVE_ANNOUNCE_PORT;
    server.master_repl_offset = 0;

    /* Replication partial resync backlog */
    server.repl_backlog = NULL;
    server.repl_backlog_size = CONFIG_DEFAULT_REPL_BACKLOG_SIZE;
    server.repl_backlog_histlen = 0;
    server.repl_backlog_idx = 0;
    server.repl_backlog_off = 0;
    server.repl_backlog_time_limit = CONFIG_DEFAULT_REPL_BACKLOG_TIME_LIMIT;
    server.repl_no_slaves_since = time(NULL);

    /* Client output buffer limits */
    for (j = 0; j < CLIENT_TYPE_OBUF_COUNT; j++)
        server.client_obuf_limits[j] = clientBufferLimitsDefaults[j];

    /* Double constants initialization */
    R_Zero = 0.0;
    R_PosInf = 1.0/R_Zero;
    R_NegInf = -1.0/R_Zero;
    R_Nan = R_Zero/R_Zero;

    /* Command table -- we initiialize it here as it is part of the
     * initial configuration, since command names may be changed via
     * redis.conf using the rename-command directive. */
    server.commands = dictCreate(&commandTableDictType,NULL);
    server.orig_commands = dictCreate(&commandTableDictType,NULL);
    populateCommandTable();
    server.delCommand = lookupCommandByCString("del");
    server.multiCommand = lookupCommandByCString("multi");
    server.lpushCommand = lookupCommandByCString("lpush");
    server.lpopCommand = lookupCommandByCString("lpop");
    server.rpopCommand = lookupCommandByCString("rpop");
    server.sremCommand = lookupCommandByCString("srem");
    server.execCommand = lookupCommandByCString("exec");

    /* Slow log */
    server.slowlog_log_slower_than = CONFIG_DEFAULT_SLOWLOG_LOG_SLOWER_THAN;
    server.slowlog_max_len = CONFIG_DEFAULT_SLOWLOG_MAX_LEN;

    /* Latency monitor */
    server.latency_monitor_threshold = CONFIG_DEFAULT_LATENCY_MONITOR_THRESHOLD;

    /* Debugging */
    server.assert_failed = "<no assertion failed>";
    server.assert_file = "<no file>";
    server.assert_line = 0;
    server.bug_report_start = 0;
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
    if (access(server.executable,X_OK) == -1) return C_ERR;

    /* Config rewriting. */
    if (flags & RESTART_SERVER_CONFIG_REWRITE &&
        server.configfile &&
        rewriteConfig(server.configfile) == -1) return C_ERR;

    /* Perform a proper shutdown. */
    if (flags & RESTART_SERVER_GRACEFULLY &&
        prepareForShutdown(SHUTDOWN_NOFLAGS) != C_OK) return C_ERR;

    /* Close all file descriptors, with the exception of stdin, stdout, strerr
     * which are useful if we restart a Redis server which is not daemonized. */
    for (j = 3; j < (int)server.maxclients + 1024; j++) close(j);

    /* Execute the server with the original command line. */
    if (delay) usleep(delay*1000);
    execve(server.executable,server.exec_argv,environ);

    /* If an error occurred here, there is nothing we can do, but exit. */
    _exit(1);

    return C_ERR; /* Never reached. */
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
                if (bestlimit < decr_step) break;
                bestlimit -= decr_step;
            }

            /* Assume that the limit we get initially is still valid if
             * our last try was even lower. */
            if (bestlimit < oldlimit) bestlimit = oldlimit;

            if (bestlimit < maxfiles) {
                int old_maxclients = server.maxclients;
                server.maxclients = bestlimit-CONFIG_MIN_RESERVED_FDS;
                if (server.maxclients < 1) {
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
#ifdef HAVE_PROC_SOMAXCONN
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
#endif
}

/* Initialize a set of file descriptors to listen to the specified 'port'
 * binding the addresses specified in the Redis server configuration.
 *
 * The listening file descriptors are stored in the integer array 'fds'
 * and their number is set in '*count'.
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
int listenToPort(int port, int *fds, int *count) {
    int j;

    /* Force binding of 0.0.0.0 if no bind address is specified, always
     * entering the loop if j == 0. */
    if (server.bindaddr_count == 0) server.bindaddr[0] = NULL;
    for (j = 0; j < server.bindaddr_count || j == 0; j++) {
        if (server.bindaddr[j] == NULL) {
            int unsupported = 0;
            /* Bind * for both IPv6 and IPv4, we enter here only if
             * server.bindaddr_count == 0. */
            fds[*count] = anetTcp6Server(server.neterr,port,NULL,
                server.tcp_backlog);
            if (fds[*count] != ANET_ERR) {
                anetNonBlock(NULL,fds[*count]);
                (*count)++;
            } else if (errno == EAFNOSUPPORT) {
                unsupported++;
                serverLog(LL_WARNING,"Not listening to IPv6: unsupproted");
            }

            if (*count == 1 || unsupported) {
                /* Bind the IPv4 address as well. */
                fds[*count] = anetTcpServer(server.neterr,port,NULL,
                    server.tcp_backlog);
                if (fds[*count] != ANET_ERR) {
                    anetNonBlock(NULL,fds[*count]);
                    (*count)++;
                } else if (errno == EAFNOSUPPORT) {
                    unsupported++;
                    serverLog(LL_WARNING,"Not listening to IPv4: unsupproted");
                }
            }
            /* Exit the loop if we were able to bind * on IPv4 and IPv6,
             * otherwise fds[*count] will be ANET_ERR and we'll print an
             * error and return to the caller with an error. */
            if (*count + unsupported == 2) break;
        } else if (strchr(server.bindaddr[j],':')) {
            /* Bind IPv6 address. */
            fds[*count] = anetTcp6Server(server.neterr,port,server.bindaddr[j],
                server.tcp_backlog);
        } else {
            /* Bind IPv4 address. */
            fds[*count] = anetTcpServer(server.neterr,port,server.bindaddr[j],
                server.tcp_backlog);
        }
        if (fds[*count] == ANET_ERR) {
            serverLog(LL_WARNING,
                "Creating Server TCP listening socket %s:%d: %s",
                server.bindaddr[j] ? server.bindaddr[j] : "*",
                port, server.neterr);
            return C_ERR;
        }
        anetNonBlock(NULL,fds[*count]);
        (*count)++;
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
    server.stat_evictedkeys = 0;
    server.stat_keyspace_misses = 0;
    server.stat_keyspace_hits = 0;
    server.stat_fork_time = 0;
    server.stat_fork_rate = 0;
    server.stat_rejected_conn = 0;
    server.stat_sync_full = 0;
    server.stat_sync_partial_ok = 0;
    server.stat_sync_partial_err = 0;
    for (j = 0; j < STATS_METRIC_COUNT; j++) {
        server.inst_metric[j].idx = 0;
        server.inst_metric[j].last_sample_time = mstime();
        server.inst_metric[j].last_sample_count = 0;
        memset(server.inst_metric[j].samples,0,
            sizeof(server.inst_metric[j].samples));
    }
    server.stat_net_input_bytes = 0;
    server.stat_net_output_bytes = 0;
    server.aof_delayed_fsync = 0;
}

void initServer(void) {
    int j;

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setupSignalHandlers();

    if (server.syslog_enabled) {
        openlog(server.syslog_ident, LOG_PID | LOG_NDELAY | LOG_NOWAIT,
            server.syslog_facility);
    }

    server.pid = getpid();
    server.current_client = NULL;
    server.clients = listCreate();
    server.clients_to_close = listCreate();
    server.slaves = listCreate();
    server.monitors = listCreate();
    server.clients_pending_write = listCreate();
    server.slaveseldb = -1; /* Force to emit the first SELECT command. */
    server.unblocked_clients = listCreate();
    server.ready_keys = listCreate();
    server.clients_waiting_acks = listCreate();
    server.get_ack_from_slaves = 0;
    server.clients_paused = 0;
    server.system_memory_size = zmalloc_get_memory_size();

    createSharedObjects();
    adjustOpenFilesLimit();
    server.el = aeCreateEventLoop(server.maxclients+CONFIG_FDSET_INCR);
    server.db = zmalloc(sizeof(redisDb)*server.dbnum);

    /* Open the TCP listening socket for the user commands. */
    if (server.port != 0 &&
        listenToPort(server.port,server.ipfd,&server.ipfd_count) == C_ERR)
        exit(1);

    /* Open the listening Unix domain socket. */
    if (server.unixsocket != NULL) {
        unlink(server.unixsocket); /* don't care if this fails */
        server.sofd = anetUnixServer(server.neterr,server.unixsocket,
            server.unixsocketperm, server.tcp_backlog);
        if (server.sofd == ANET_ERR) {
            serverLog(LL_WARNING, "Opening Unix socket: %s", server.neterr);
            exit(1);
        }
        anetNonBlock(NULL,server.sofd);
    }

    /* Abort if there are no listening sockets at all. */
    if (server.ipfd_count == 0 && server.sofd < 0) {
        serverLog(LL_WARNING, "Configured to not listen anywhere, exiting.");
        exit(1);
    }

    /* Create the Redis databases, and initialize other internal state. */
    for (j = 0; j < server.dbnum; j++) {
        server.db[j].dict = dictCreate(&dbDictType,NULL);
        server.db[j].expires = dictCreate(&keyptrDictType,NULL);
        server.db[j].blocking_keys = dictCreate(&keylistDictType,NULL);
        server.db[j].ready_keys = dictCreate(&setDictType,NULL);
        server.db[j].watched_keys = dictCreate(&keylistDictType,NULL);
        server.db[j].eviction_pool = evictionPoolAlloc();
        server.db[j].id = j;
        server.db[j].avg_ttl = 0;
    }
    server.pubsub_channels = dictCreate(&keylistDictType,NULL);
    server.pubsub_patterns = listCreate();
    listSetFreeMethod(server.pubsub_patterns,freePubsubPattern);
    listSetMatchMethod(server.pubsub_patterns,listMatchPubsubPattern);
    server.cronloops = 0;
    server.rdb_child_pid = -1;
    server.aof_child_pid = -1;
    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    server.rdb_bgsave_scheduled = 0;
    aofRewriteBufferReset();
    server.aof_buf = sdsempty();
    server.lastsave = time(NULL); /* At startup we consider the DB saved. */
    server.lastbgsave_try = 0;    /* At startup we never tried to BGSAVE. */
    server.rdb_save_time_last = -1;
    server.rdb_save_time_start = -1;
    server.dirty = 0;
    resetServerStats();
    /* A few stats we don't want to reset: server startup time, and peak mem. */
    server.stat_starttime = time(NULL);
    server.stat_peak_memory = 0;
    server.resident_set_size = 0;
    server.lastbgsave_status = C_OK;
    server.aof_last_write_status = C_OK;
    server.aof_last_write_errno = 0;
    server.repl_good_slaves_count = 0;
    updateCachedTime();

    /* Create the serverCron() time event, that's our main way to process
     * background operations. */
    if(aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL) == AE_ERR) {
        serverPanic("Can't create the serverCron time event.");
        exit(1);
    }

    /* Create an event handler for accepting new connections in TCP and Unix
     * domain sockets. */
    for (j = 0; j < server.ipfd_count; j++) {
        if (aeCreateFileEvent(server.el, server.ipfd[j], AE_READABLE,
            acceptTcpHandler,NULL) == AE_ERR)
            {
                serverPanic(
                    "Unrecoverable error creating server.ipfd file event.");
            }
    }
    if (server.sofd > 0 && aeCreateFileEvent(server.el,server.sofd,AE_READABLE,
        acceptUnixHandler,NULL) == AE_ERR) serverPanic("Unrecoverable error creating server.sofd file event.");

    /* Open the AOF file if needed. */
    if (server.aof_state == AOF_ON) {
        server.aof_fd = open(server.aof_filename,
                               O_WRONLY|O_APPEND|O_CREAT,0644);
        if (server.aof_fd == -1) {
            serverLog(LL_WARNING, "Can't open the append-only file: %s",
                strerror(errno));
            exit(1);
        }
    }

    /* 32 bit instances are limited to 4GB of address space, so if there is
     * no explicit limit in the user provided configuration we set a limit
     * at 3 GB using maxmemory with 'noeviction' policy'. This avoids
     * useless crashes of the Redis instance for out of memory. */
    if (server.arch_bits == 32 && server.maxmemory == 0) {
        serverLog(LL_WARNING,"Warning: 32 bit instance detected but no memory limit set. Setting 3 GB maxmemory limit with 'noeviction' policy now.");
        server.maxmemory = 3072LL*(1024*1024); /* 3 GB */
        server.maxmemory_policy = MAXMEMORY_NO_EVICTION;
    }

    if (server.cluster_enabled) clusterInit();
    replicationScriptCacheInit();
    scriptingInit(1);
    slowlogInit();
    latencyMonitorInit();
    bioInit();
}

/* Populates the Redis Command Table starting from the hard coded list
 * we have on top of redis.c file. */
void populateCommandTable(void) {
    int j;
    int numcommands = sizeof(redisCommandTable)/sizeof(struct redisCommand);

    for (j = 0; j < numcommands; j++) {
        struct redisCommand *c = redisCommandTable+j;
        char *f = c->sflags;
        int retval1, retval2;

        while(*f != '\0') {
            switch(*f) {
            case 'w': c->flags |= CMD_WRITE; break;
            case 'r': c->flags |= CMD_READONLY; break;
            case 'm': c->flags |= CMD_DENYOOM; break;
            case 'a': c->flags |= CMD_ADMIN; break;
            case 'p': c->flags |= CMD_PUBSUB; break;
            case 's': c->flags |= CMD_NOSCRIPT; break;
            case 'R': c->flags |= CMD_RANDOM; break;
            case 'S': c->flags |= CMD_SORT_FOR_SCRIPT; break;
            case 'l': c->flags |= CMD_LOADING; break;
            case 't': c->flags |= CMD_STALE; break;
            case 'M': c->flags |= CMD_SKIP_MONITOR; break;
            case 'k': c->flags |= CMD_ASKING; break;
            case 'F': c->flags |= CMD_FAST; break;
            default: serverPanic("Unsupported command flag"); break;
            }
            f++;
        }

        retval1 = dictAdd(server.commands, sdsnew(c->name), c);
        /* Populate an additional dictionary that will be unaffected
         * by rename-command statements in redis.conf. */
        retval2 = dictAdd(server.orig_commands, sdsnew(c->name), c);
        serverAssert(retval1 == DICT_OK && retval2 == DICT_OK);
    }
}

void resetCommandTableStats(void) {
    int numcommands = sizeof(redisCommandTable)/sizeof(struct redisCommand);
    int j;

    for (j = 0; j < numcommands; j++) {
        struct redisCommand *c = redisCommandTable+j;

        c->microseconds = 0;
        c->calls = 0;
    }
}

/* ========================== Redis OP Array API ============================ */

void redisOpArrayInit(redisOpArray *oa) {
    oa->ops = NULL;
    oa->numops = 0;
}

int redisOpArrayAppend(redisOpArray *oa, struct redisCommand *cmd, int dbid,
                       robj **argv, int argc, int target)
{
    redisOp *op;

    oa->ops = zrealloc(oa->ops,sizeof(redisOp)*(oa->numops+1));
    op = oa->ops+oa->numops;
    op->cmd = cmd;
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
    zfree(oa->ops);
}

/* ====================== Commands lookup and execution ===================== */

struct redisCommand *lookupCommand(sds name) {
    return dictFetchValue(server.commands, name);
}

struct redisCommand *lookupCommandByCString(char *s) {
    struct redisCommand *cmd;
    sds name = sdsnew(s);

    cmd = dictFetchValue(server.commands, name);
    sdsfree(name);
    return cmd;
}

/* Lookup the command in the current table, if not found also check in
 * the original table containing the original command names unaffected by
 * redis.conf rename-command statement.
 *
 * This is used by functions rewriting the argument vector such as
 * rewriteClientCommandVector() in order to set client->cmd pointer
 * correctly even if the command was renamed. */
struct redisCommand *lookupCommandOrOriginal(sds name) {
    struct redisCommand *cmd = dictFetchValue(server.commands, name);

    if (!cmd) cmd = dictFetchValue(server.orig_commands,name);
    return cmd;
}

/* Propagate the specified command (in the context of the specified database id)
 * to AOF and Slaves.
 *
 * flags are an xor between:
 * + PROPAGATE_NONE (no propagation of command at all)
 * + PROPAGATE_AOF (propagate into the AOF file if is enabled)
 * + PROPAGATE_REPL (propagate into the replication link)
 *
 * This should not be used inside commands implementation. Use instead
 * alsoPropagate(), preventCommandPropagation(), forceCommandPropagation().
 */
void propagate(struct redisCommand *cmd, int dbid, robj **argv, int argc,
               int flags)
{
    if (server.aof_state != AOF_OFF && flags & PROPAGATE_AOF)
        feedAppendOnlyFile(cmd,dbid,argv,argc);
    if (flags & PROPAGATE_REPL)
        replicationFeedSlaves(server.slaves,dbid,argv,argc);
}

/* Used inside commands to schedule the propagation of additional commands
 * after the current command is propagated to AOF / Replication.
 *
 * 'cmd' must be a pointer to the Redis command to replicate, dbid is the
 * database ID the command should be propagated into.
 * Arguments of the command to propagte are passed as an array of redis
 * objects pointers of len 'argc', using the 'argv' vector.
 *
 * The function does not take a reference to the passed 'argv' vector,
 * so it is up to the caller to release the passed argv (but it is usually
 * stack allocated).  The function autoamtically increments ref count of
 * passed objects, so the caller does not need to. */
void alsoPropagate(struct redisCommand *cmd, int dbid, robj **argv, int argc,
                   int target)
{
    robj **argvcopy;
    int j;

    if (server.loading) return; /* No propagation during loading. */

    argvcopy = zmalloc(sizeof(robj*)*argc);
    for (j = 0; j < argc; j++) {
        argvcopy[j] = argv[j];
        incrRefCount(argv[j]);
    }
    redisOpArrayAppend(&server.also_propagate,cmd,dbid,argvcopy,argc,target);
}

/* It is possible to call the function forceCommandPropagation() inside a
 * Redis command implementation in order to to force the propagation of a
 * specific command execution into AOF / Replication. */
void forceCommandPropagation(client *c, int flags) {
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

/* Call() is the core of Redis execution of a command.
 *
 * The following flags can be passed:
 * CMD_CALL_NONE        No flags.
 * CMD_CALL_SLOWLOG     Check command speed and log in the slow log if needed.
 * CMD_CALL_STATS       Populate command stats.
 * CMD_CALL_PROPAGATE_AOF   Append command to AOF if it modified the dataset
 *                          or if the client flags are forcing propagation.
 * CMD_CALL_PROPAGATE_REPL  Send command to salves if it modified the dataset
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
    long long dirty, start, duration;
    int client_old_flags = c->flags;

    /* Sent the command to clients in MONITOR mode, only if the commands are
     * not generated from reading an AOF. */
    if (listLength(server.monitors) &&
        !server.loading &&
        !(c->cmd->flags & (CMD_SKIP_MONITOR|CMD_ADMIN)))
    {
        replicationFeedMonitors(c,server.monitors,c->db->id,c->argv,c->argc);
    }

    /* Initialization: clear the flags that must be set by the command on
     * demand, and initialize the array for additional commands propagation. */
    c->flags &= ~(CLIENT_FORCE_AOF|CLIENT_FORCE_REPL|CLIENT_PREVENT_PROP);
    redisOpArrayInit(&server.also_propagate);

    /* Call the command. */
    dirty = server.dirty;
    start = ustime();
    c->cmd->proc(c);
    duration = ustime()-start;
    dirty = server.dirty-dirty;
    if (dirty < 0) dirty = 0;

    /* When EVAL is called loading the AOF we don't want commands called
     * from Lua to go into the slowlog or to populate statistics. */
    if (server.loading && c->flags & CLIENT_LUA)
        flags &= ~(CMD_CALL_SLOWLOG | CMD_CALL_STATS);

    /* If the caller is Lua, we want to force the EVAL caller to propagate
     * the script if the command flag or client flag are forcing the
     * propagation. */
    if (c->flags & CLIENT_LUA && server.lua_caller) {
        if (c->flags & CLIENT_FORCE_REPL)
            server.lua_caller->flags |= CLIENT_FORCE_REPL;
        if (c->flags & CLIENT_FORCE_AOF)
            server.lua_caller->flags |= CLIENT_FORCE_AOF;
    }

    /* Log the command into the Slow log if needed, and populate the
     * per-command statistics that we show in INFO commandstats. */
    if (flags & CMD_CALL_SLOWLOG && c->cmd->proc != execCommand) {
        char *latency_event = (c->cmd->flags & CMD_FAST) ?
                              "fast-command" : "command";
        latencyAddSampleIfNeeded(latency_event,duration/1000);
        slowlogPushEntryIfNeeded(c->argv,c->argc,duration);
    }
    if (flags & CMD_CALL_STATS) {
        c->lastcmd->microseconds += duration;
        c->lastcmd->calls++;
    }

    /* Propagate the command into the AOF and replication link */
    if (flags & CMD_CALL_PROPAGATE &&
        (c->flags & CLIENT_PREVENT_PROP) != CLIENT_PREVENT_PROP)
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
         * implementatino called preventCommandPropagation() or similar,
         * or if we don't have the call() flags to do so. */
        if (c->flags & CLIENT_PREVENT_REPL_PROP ||
            !(flags & CMD_CALL_PROPAGATE_REPL))
                propagate_flags &= ~PROPAGATE_REPL;
        if (c->flags & CLIENT_PREVENT_AOF_PROP ||
            !(flags & CMD_CALL_PROPAGATE_AOF))
                propagate_flags &= ~PROPAGATE_AOF;

        /* Call propagate() only if at least one of AOF / replication
         * propagation is needed. */
        if (propagate_flags != PROPAGATE_NONE)
            propagate(c->cmd,c->db->id,c->argv,c->argc,propagate_flags);
    }

    /* Restore the old replication flags, since call() can be executed
     * recursively. */
    c->flags &= ~(CLIENT_FORCE_AOF|CLIENT_FORCE_REPL|CLIENT_PREVENT_PROP);
    c->flags |= client_old_flags &
        (CLIENT_FORCE_AOF|CLIENT_FORCE_REPL|CLIENT_PREVENT_PROP);

    /* Handle the alsoPropagate() API to handle commands that want to propagate
     * multiple separated commands. Note that alsoPropagate() is not affected
     * by CLIENT_PREVENT_PROP flag. */
    if (server.also_propagate.numops) {
        int j;
        redisOp *rop;

        if (flags & CMD_CALL_PROPAGATE) {
            for (j = 0; j < server.also_propagate.numops; j++) {
                rop = &server.also_propagate.ops[j];
                int target = rop->target;
                /* Whatever the command wish is, we honor the call() flags. */
                if (!(flags&CMD_CALL_PROPAGATE_AOF)) target &= ~PROPAGATE_AOF;
                if (!(flags&CMD_CALL_PROPAGATE_REPL)) target &= ~PROPAGATE_REPL;
                if (target)
                    propagate(rop->cmd,rop->dbid,rop->argv,rop->argc,target);
            }
        }
        redisOpArrayFree(&server.also_propagate);
    }
    server.stat_numcommands++;
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
    /* The QUIT command is handled separately. Normal command procs will
     * go through checking for replication and QUIT will cause trouble
     * when FORCE_REPLICATION is enabled and would be implemented in
     * a regular command proc. */
    if (!strcasecmp(c->argv[0]->ptr,"quit")) {
        addReply(c,shared.ok);
        c->flags |= CLIENT_CLOSE_AFTER_REPLY;
        return C_ERR;
    }

    /* Now lookup the command and check ASAP about trivial error conditions
     * such as wrong arity, bad command name and so forth. */
    c->cmd = c->lastcmd = lookupCommand(c->argv[0]->ptr);
    if (!c->cmd) {
        flagTransaction(c);
        addReplyErrorFormat(c,"unknown command '%s'",
            (char*)c->argv[0]->ptr);
        return C_OK;
    } else if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) ||
               (c->argc < -c->cmd->arity)) {
        flagTransaction(c);
        addReplyErrorFormat(c,"wrong number of arguments for '%s' command",
            c->cmd->name);
        return C_OK;
    }

    /* Check if the user is authenticated */
    if (server.requirepass && !c->authenticated && c->cmd->proc != authCommand)
    {
        flagTransaction(c);
        addReply(c,shared.noautherr);
        return C_OK;
    }

    /* If cluster is enabled perform the cluster redirection here.
     * However we don't perform the redirection if:
     * 1) The sender of this command is our master.
     * 2) The command has no key arguments. */
    if (server.cluster_enabled &&
        !(c->flags & CLIENT_MASTER) &&
        !(c->flags & CLIENT_LUA &&
          server.lua_caller->flags & CLIENT_MASTER) &&
        !(c->cmd->getkeys_proc == NULL && c->cmd->firstkey == 0 &&
          c->cmd->proc != execCommand))
    {
        int hashslot;
        int error_code;
        clusterNode *n = getNodeByQuery(c,c->cmd,c->argv,c->argc,
                                        &hashslot,&error_code);
        if (n == NULL || n != server.cluster->myself) {
            if (c->cmd->proc == execCommand) {
                discardTransaction(c);
            } else {
                flagTransaction(c);
            }
            clusterRedirectClient(c,n,hashslot,error_code);
            return C_OK;
        }
    }

    /* Handle the maxmemory directive.
     *
     * First we try to free some memory if possible (if there are volatile
     * keys in the dataset). If there are not the only thing we can do
     * is returning an error. */
    if (server.maxmemory) {
        int retval = freeMemoryIfNeeded();
        /* freeMemoryIfNeeded may flush slave output buffers. This may result
         * into a slave, that may be the active client, to be freed. */
        if (server.current_client == NULL) return C_ERR;

        /* It was impossible to free enough memory, and the command the client
         * is trying to execute is denied during OOM conditions? Error. */
        if ((c->cmd->flags & CMD_DENYOOM) && retval == C_ERR) {
            flagTransaction(c);
            addReply(c, shared.oomerr);
            return C_OK;
        }
    }

    /* Don't accept write commands if there are problems persisting on disk
     * and if this is a master instance. */
    if (((server.stop_writes_on_bgsave_err &&
          server.saveparamslen > 0 &&
          server.lastbgsave_status == C_ERR) ||
          server.aof_last_write_status == C_ERR) &&
        server.masterhost == NULL &&
        (c->cmd->flags & CMD_WRITE ||
         c->cmd->proc == pingCommand))
    {
        flagTransaction(c);
        if (server.aof_last_write_status == C_OK)
            addReply(c, shared.bgsaveerr);
        else
            addReplySds(c,
                sdscatprintf(sdsempty(),
                "-MISCONF Errors writing to the AOF file: %s\r\n",
                strerror(server.aof_last_write_errno)));
        return C_OK;
    }

    /* Don't accept write commands if there are not enough good slaves and
     * user configured the min-slaves-to-write option. */
    if (server.masterhost == NULL &&
        server.repl_min_slaves_to_write &&
        server.repl_min_slaves_max_lag &&
        c->cmd->flags & CMD_WRITE &&
        server.repl_good_slaves_count < server.repl_min_slaves_to_write)
    {
        flagTransaction(c);
        addReply(c, shared.noreplicaserr);
        return C_OK;
    }

    /* Don't accept write commands if this is a read only slave. But
     * accept write commands if this is our master. */
    if (server.masterhost && server.repl_slave_ro &&
        !(c->flags & CLIENT_MASTER) &&
        c->cmd->flags & CMD_WRITE)
    {
        addReply(c, shared.roslaveerr);
        return C_OK;
    }

    /* Only allow SUBSCRIBE and UNSUBSCRIBE in the context of Pub/Sub */
    if (c->flags & CLIENT_PUBSUB &&
        c->cmd->proc != pingCommand &&
        c->cmd->proc != subscribeCommand &&
        c->cmd->proc != unsubscribeCommand &&
        c->cmd->proc != psubscribeCommand &&
        c->cmd->proc != punsubscribeCommand) {
        addReplyError(c,"only (P)SUBSCRIBE / (P)UNSUBSCRIBE / PING / QUIT allowed in this context");
        return C_OK;
    }

    /* Only allow INFO and SLAVEOF when slave-serve-stale-data is no and
     * we are a slave with a broken link with master. */
    if (server.masterhost && server.repl_state != REPL_STATE_CONNECTED &&
        server.repl_serve_stale_data == 0 &&
        !(c->cmd->flags & CMD_STALE))
    {
        flagTransaction(c);
        addReply(c, shared.masterdownerr);
        return C_OK;
    }

    /* Loading DB? Return an error if the command has not the
     * CMD_LOADING flag. */
    if (server.loading && !(c->cmd->flags & CMD_LOADING)) {
        addReply(c, shared.loadingerr);
        return C_OK;
    }

    /* Lua script too slow? Only allow a limited number of commands. */
    if (server.lua_timedout &&
          c->cmd->proc != authCommand &&
          c->cmd->proc != replconfCommand &&
        !(c->cmd->proc == shutdownCommand &&
          c->argc == 2 &&
          tolower(((char*)c->argv[1]->ptr)[0]) == 'n') &&
        !(c->cmd->proc == scriptCommand &&
          c->argc == 2 &&
          tolower(((char*)c->argv[1]->ptr)[0]) == 'k'))
    {
        flagTransaction(c);
        addReply(c, shared.slowscripterr);
        return C_OK;
    }

    /* Exec the command */
    if (c->flags & CLIENT_MULTI &&
        c->cmd->proc != execCommand && c->cmd->proc != discardCommand &&
        c->cmd->proc != multiCommand && c->cmd->proc != watchCommand)
    {
        queueMultiCommand(c);
        addReply(c,shared.queued);
    } else {
        call(c,CMD_CALL_FULL);
        c->woff = server.master_repl_offset;
        if (listLength(server.ready_keys))
            handleClientsBlockedOnLists();
    }
    return C_OK;
}

/*================================== Shutdown =============================== */

/* Close listening sockets. Also unlink the unix domain socket if
 * unlink_unix_socket is non-zero. */
void closeListeningSockets(int unlink_unix_socket) {
    int j;

    for (j = 0; j < server.ipfd_count; j++) close(server.ipfd[j]);
    if (server.sofd != -1) close(server.sofd);
    if (server.cluster_enabled)
        for (j = 0; j < server.cfd_count; j++) close(server.cfd[j]);
    if (unlink_unix_socket && server.unixsocket) {
        serverLog(LL_NOTICE,"Removing the unix socket file.");
        unlink(server.unixsocket); /* don't care if this fails */
    }
}

int prepareForShutdown(int flags) {
    int save = flags & SHUTDOWN_SAVE;
    int nosave = flags & SHUTDOWN_NOSAVE;

    serverLog(LL_WARNING,"User requested shutdown...");

    /* Kill all the Lua debugger forked sessions. */
    ldbKillForkedSessions();

    /* Kill the saving child if there is a background saving in progress.
       We want to avoid race conditions, for instance our saving child may
       overwrite the synchronous saving did by SHUTDOWN. */
    if (server.rdb_child_pid != -1) {
        serverLog(LL_WARNING,"There is a child saving an .rdb. Killing it!");
        kill(server.rdb_child_pid,SIGUSR1);
        rdbRemoveTempFile(server.rdb_child_pid);
    }

    if (server.aof_state != AOF_OFF) {
        /* Kill the AOF saving child as the AOF we already have may be longer
         * but contains the full dataset anyway. */
        if (server.aof_child_pid != -1) {
            /* If we have AOF enabled but haven't written the AOF yet, don't
             * shutdown or else the dataset will be lost. */
            if (server.aof_state == AOF_WAIT_REWRITE) {
                serverLog(LL_WARNING, "Writing initial AOF, can't exit.");
                return C_ERR;
            }
            serverLog(LL_WARNING,
                "There is a child rewriting the AOF. Killing it!");
            kill(server.aof_child_pid,SIGUSR1);
        }
        /* Append only file: fsync() the AOF and exit */
        serverLog(LL_NOTICE,"Calling fsync() on the AOF file.");
        aof_fsync(server.aof_fd);
    }

    /* Create a new RDB file before exiting. */
    if ((server.saveparamslen > 0 && !nosave) || save) {
        serverLog(LL_NOTICE,"Saving the final RDB snapshot before exiting.");
        /* Snapshotting. Perform a SYNC SAVE and exit */
        if (rdbSave(server.rdb_filename) != C_OK) {
            /* Ooops.. error saving! The best we can do is to continue
             * operating. Note that if there was a background saving process,
             * in the next cron() Redis will be notified that the background
             * saving aborted, handling special stuff like slaves pending for
             * synchronization... */
            serverLog(LL_WARNING,"Error trying to save the DB, can't exit.");
            return C_ERR;
        }
    }

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
    serverLog(LL_WARNING,"%s is now ready to exit, bye bye...",
        server.sentinel_mode ? "Sentinel" : "Redis");
    return C_OK;
}

/*================================== Commands =============================== */

/* Return zero if strings are the same, non-zero if they are not.
 * The comparison is performed in a way that prevents an attacker to obtain
 * information about the nature of the strings just monitoring the execution
 * time of the function.
 *
 * Note that limiting the comparison length to strings up to 512 bytes we
 * can avoid leaking any information about the password length and any
 * possible branch misprediction related leak.
 */
int time_independent_strcmp(char *a, char *b) {
    char bufa[CONFIG_AUTHPASS_MAX_LEN], bufb[CONFIG_AUTHPASS_MAX_LEN];
    /* The above two strlen perform len(a) + len(b) operations where either
     * a or b are fixed (our password) length, and the difference is only
     * relative to the length of the user provided string, so no information
     * leak is possible in the following two lines of code. */
    unsigned int alen = strlen(a);
    unsigned int blen = strlen(b);
    unsigned int j;
    int diff = 0;

    /* We can't compare strings longer than our static buffers.
     * Note that this will never pass the first test in practical circumstances
     * so there is no info leak. */
    if (alen > sizeof(bufa) || blen > sizeof(bufb)) return 1;

    memset(bufa,0,sizeof(bufa));        /* Constant time. */
    memset(bufb,0,sizeof(bufb));        /* Constant time. */
    /* Again the time of the following two copies is proportional to
     * len(a) + len(b) so no info is leaked. */
    memcpy(bufa,a,alen);
    memcpy(bufb,b,blen);

    /* Always compare all the chars in the two buffers without
     * conditional expressions. */
    for (j = 0; j < sizeof(bufa); j++) {
        diff |= (bufa[j] ^ bufb[j]);
    }
    /* Length must be equal as well. */
    diff |= alen ^ blen;
    return diff; /* If zero strings are the same. */
}

void authCommand(client *c) {
    if (!server.requirepass) {
        addReplyError(c,"Client sent AUTH, but no password is set");
    } else if (!time_independent_strcmp(c->argv[1]->ptr, server.requirepass)) {
      c->authenticated = 1;
      addReply(c,shared.ok);
    } else {
      c->authenticated = 0;
      addReplyError(c,"invalid password");
    }
}

/* The PING command. It works in a different way if the client is in
 * in Pub/Sub mode. */
void pingCommand(client *c) {
    /* The command takes zero or one arguments. */
    if (c->argc > 2) {
        addReplyErrorFormat(c,"wrong number of arguments for '%s' command",
            c->cmd->name);
        return;
    }

    if (c->flags & CLIENT_PUBSUB) {
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
    struct timeval tv;

    /* gettimeofday() can only fail if &tv is a bad address so we
     * don't check for errors. */
    gettimeofday(&tv,NULL);
    addReplyMultiBulkLen(c,2);
    addReplyBulkLongLong(c,tv.tv_sec);
    addReplyBulkLongLong(c,tv.tv_usec);
}

/* Helper function for addReplyCommand() to output flags. */
int addReplyCommandFlag(client *c, struct redisCommand *cmd, int f, char *reply) {
    if (cmd->flags & f) {
        addReplyStatus(c, reply);
        return 1;
    }
    return 0;
}

/* Output the representation of a Redis command. Used by the COMMAND command. */
void addReplyCommand(client *c, struct redisCommand *cmd) {
    if (!cmd) {
        addReply(c, shared.nullbulk);
    } else {
        /* We are adding: command name, arg count, flags, first, last, offset */
        addReplyMultiBulkLen(c, 6);
        addReplyBulkCString(c, cmd->name);
        addReplyLongLong(c, cmd->arity);

        int flagcount = 0;
        void *flaglen = addDeferredMultiBulkLength(c);
        flagcount += addReplyCommandFlag(c,cmd,CMD_WRITE, "write");
        flagcount += addReplyCommandFlag(c,cmd,CMD_READONLY, "readonly");
        flagcount += addReplyCommandFlag(c,cmd,CMD_DENYOOM, "denyoom");
        flagcount += addReplyCommandFlag(c,cmd,CMD_ADMIN, "admin");
        flagcount += addReplyCommandFlag(c,cmd,CMD_PUBSUB, "pubsub");
        flagcount += addReplyCommandFlag(c,cmd,CMD_NOSCRIPT, "noscript");
        flagcount += addReplyCommandFlag(c,cmd,CMD_RANDOM, "random");
        flagcount += addReplyCommandFlag(c,cmd,CMD_SORT_FOR_SCRIPT,"sort_for_script");
        flagcount += addReplyCommandFlag(c,cmd,CMD_LOADING, "loading");
        flagcount += addReplyCommandFlag(c,cmd,CMD_STALE, "stale");
        flagcount += addReplyCommandFlag(c,cmd,CMD_SKIP_MONITOR, "skip_monitor");
        flagcount += addReplyCommandFlag(c,cmd,CMD_ASKING, "asking");
        flagcount += addReplyCommandFlag(c,cmd,CMD_FAST, "fast");
        if (cmd->getkeys_proc) {
            addReplyStatus(c, "movablekeys");
            flagcount += 1;
        }
        setDeferredMultiBulkLength(c, flaglen, flagcount);

        addReplyLongLong(c, cmd->firstkey);
        addReplyLongLong(c, cmd->lastkey);
        addReplyLongLong(c, cmd->keystep);
    }
}

/* COMMAND <subcommand> <args> */
void commandCommand(client *c) {
    dictIterator *di;
    dictEntry *de;

    if (c->argc == 1) {
        addReplyMultiBulkLen(c, dictSize(server.commands));
        di = dictGetIterator(server.commands);
        while ((de = dictNext(di)) != NULL) {
            addReplyCommand(c, dictGetVal(de));
        }
        dictReleaseIterator(di);
    } else if (!strcasecmp(c->argv[1]->ptr, "info")) {
        int i;
        addReplyMultiBulkLen(c, c->argc-2);
        for (i = 2; i < c->argc; i++) {
            addReplyCommand(c, dictFetchValue(server.commands, c->argv[i]->ptr));
        }
    } else if (!strcasecmp(c->argv[1]->ptr, "count") && c->argc == 2) {
        addReplyLongLong(c, dictSize(server.commands));
    } else if (!strcasecmp(c->argv[1]->ptr,"getkeys") && c->argc >= 3) {
        struct redisCommand *cmd = lookupCommand(c->argv[2]->ptr);
        int *keys, numkeys, j;

        if (!cmd) {
            addReplyErrorFormat(c,"Invalid command specified");
            return;
        } else if ((cmd->arity > 0 && cmd->arity != c->argc-2) ||
                   ((c->argc-2) < -cmd->arity))
        {
            addReplyError(c,"Invalid number of arguments specified for command");
            return;
        }

        keys = getKeysFromCommand(cmd,c->argv+2,c->argc-2,&numkeys);
        addReplyMultiBulkLen(c,numkeys);
        for (j = 0; j < numkeys; j++) addReplyBulk(c,c->argv[keys[j]+2]);
        getKeysFreeResult(keys);
    } else {
        addReplyError(c, "Unknown subcommand or wrong number of arguments.");
        return;
    }
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
    } else if (n < (1024LL*1024*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024*1024);
        sprintf(s,"%.2fT",d);
    } else if (n < (1024LL*1024*1024*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024*1024*1024);
        sprintf(s,"%.2fP",d);
    } else {
        /* Let's hope we never need this */
        sprintf(s,"%lluB",n);
    }
}

/* Create the string returned by the INFO command. This is decoupled
 * by the INFO command itself as we need to report the same information
 * on memory corruption problems. */
sds genRedisInfoString(char *section) {
    sds info = sdsempty();
    time_t uptime = server.unixtime-server.stat_starttime;
    int j, numcommands;
    struct rusage self_ru, c_ru;
    unsigned long lol, bib;
    int allsections = 0, defsections = 0;
    int sections = 0;

    if (section == NULL) section = "default";
    allsections = strcasecmp(section,"all") == 0;
    defsections = strcasecmp(section,"default") == 0;

    getrusage(RUSAGE_SELF, &self_ru);
    getrusage(RUSAGE_CHILDREN, &c_ru);
    getClientsMaxBuffers(&lol,&bib);

    /* Server */
    if (allsections || defsections || !strcasecmp(section,"server")) {
        static int call_uname = 1;
        static struct utsname name;
        char *mode;

        if (server.cluster_enabled) mode = "cluster";
        else if (server.sentinel_mode) mode = "sentinel";
        else mode = "standalone";

        if (sections++) info = sdscat(info,"\r\n");

        if (call_uname) {
            /* Uname can be slow and is always the same output. Cache it. */
            uname(&name);
            call_uname = 0;
        }

        info = sdscatprintf(info,
            "# Server\r\n"
            "redis_version:%s\r\n"
            "redis_git_sha1:%s\r\n"
            "redis_git_dirty:%d\r\n"
            "redis_build_id:%llx\r\n"
            "redis_mode:%s\r\n"
            "os:%s %s %s\r\n"
            "arch_bits:%d\r\n"
            "multiplexing_api:%s\r\n"
            "gcc_version:%d.%d.%d\r\n"
            "process_id:%ld\r\n"
            "run_id:%s\r\n"
            "tcp_port:%d\r\n"
            "uptime_in_seconds:%jd\r\n"
            "uptime_in_days:%jd\r\n"
            "hz:%d\r\n"
            "lru_clock:%ld\r\n"
            "executable:%s\r\n"
            "config_file:%s\r\n",
            REDIS_VERSION,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            (unsigned long long) redisBuildId(),
            mode,
            name.sysname, name.release, name.machine,
            server.arch_bits,
            aeGetApiName(),
#ifdef __GNUC__
            __GNUC__,__GNUC_MINOR__,__GNUC_PATCHLEVEL__,
#else
            0,0,0,
#endif
            (long) getpid(),
            server.runid,
            server.port,
            (intmax_t)uptime,
            (intmax_t)(uptime/(3600*24)),
            server.hz,
            (unsigned long) server.lruclock,
            server.executable ? server.executable : "",
            server.configfile ? server.configfile : "");
    }

    /* Clients */
    if (allsections || defsections || !strcasecmp(section,"clients")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Clients\r\n"
            "connected_clients:%lu\r\n"
            "client_longest_output_list:%lu\r\n"
            "client_biggest_input_buf:%lu\r\n"
            "blocked_clients:%d\r\n",
            listLength(server.clients)-listLength(server.slaves),
            lol, bib,
            server.bpop_blocked_clients);
    }

    /* Memory */
    if (allsections || defsections || !strcasecmp(section,"memory")) {
        char hmem[64];
        char peak_hmem[64];
        char total_system_hmem[64];
        char used_memory_lua_hmem[64];
        char used_memory_rss_hmem[64];
        char maxmemory_hmem[64];
        size_t zmalloc_used = zmalloc_used_memory();
        size_t total_system_mem = server.system_memory_size;
        const char *evict_policy = evictPolicyToString();
        long long memory_lua = (long long)lua_gc(server.lua,LUA_GCCOUNT,0)*1024;

        /* Peak memory is updated from time to time by serverCron() so it
         * may happen that the instantaneous value is slightly bigger than
         * the peak value. This may confuse users, so we update the peak
         * if found smaller than the current memory usage. */
        if (zmalloc_used > server.stat_peak_memory)
            server.stat_peak_memory = zmalloc_used;

        bytesToHuman(hmem,zmalloc_used);
        bytesToHuman(peak_hmem,server.stat_peak_memory);
        bytesToHuman(total_system_hmem,total_system_mem);
        bytesToHuman(used_memory_lua_hmem,memory_lua);
        bytesToHuman(used_memory_rss_hmem,server.resident_set_size);
        bytesToHuman(maxmemory_hmem,server.maxmemory);

        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Memory\r\n"
            "used_memory:%zu\r\n"
            "used_memory_human:%s\r\n"
            "used_memory_rss:%zu\r\n"
            "used_memory_rss_human:%s\r\n"
            "used_memory_peak:%zu\r\n"
            "used_memory_peak_human:%s\r\n"
            "total_system_memory:%lu\r\n"
            "total_system_memory_human:%s\r\n"
            "used_memory_lua:%lld\r\n"
            "used_memory_lua_human:%s\r\n"
            "maxmemory:%lld\r\n"
            "maxmemory_human:%s\r\n"
            "maxmemory_policy:%s\r\n"
            "mem_fragmentation_ratio:%.2f\r\n"
            "mem_allocator:%s\r\n",
            zmalloc_used,
            hmem,
            server.resident_set_size,
            used_memory_rss_hmem,
            server.stat_peak_memory,
            peak_hmem,
            (unsigned long)total_system_mem,
            total_system_hmem,
            memory_lua,
            used_memory_lua_hmem,
            server.maxmemory,
            maxmemory_hmem,
            evict_policy,
            zmalloc_get_fragmentation_ratio(server.resident_set_size),
            ZMALLOC_LIB
            );
    }

    /* Persistence */
    if (allsections || defsections || !strcasecmp(section,"persistence")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Persistence\r\n"
            "loading:%d\r\n"
            "rdb_changes_since_last_save:%lld\r\n"
            "rdb_bgsave_in_progress:%d\r\n"
            "rdb_last_save_time:%jd\r\n"
            "rdb_last_bgsave_status:%s\r\n"
            "rdb_last_bgsave_time_sec:%jd\r\n"
            "rdb_current_bgsave_time_sec:%jd\r\n"
            "aof_enabled:%d\r\n"
            "aof_rewrite_in_progress:%d\r\n"
            "aof_rewrite_scheduled:%d\r\n"
            "aof_last_rewrite_time_sec:%jd\r\n"
            "aof_current_rewrite_time_sec:%jd\r\n"
            "aof_last_bgrewrite_status:%s\r\n"
            "aof_last_write_status:%s\r\n",
            server.loading,
            server.dirty,
            server.rdb_child_pid != -1,
            (intmax_t)server.lastsave,
            (server.lastbgsave_status == C_OK) ? "ok" : "err",
            (intmax_t)server.rdb_save_time_last,
            (intmax_t)((server.rdb_child_pid == -1) ?
                -1 : time(NULL)-server.rdb_save_time_start),
            server.aof_state != AOF_OFF,
            server.aof_child_pid != -1,
            server.aof_rewrite_scheduled,
            (intmax_t)server.aof_rewrite_time_last,
            (intmax_t)((server.aof_child_pid == -1) ?
                -1 : time(NULL)-server.aof_rewrite_time_start),
            (server.aof_lastbgrewrite_status == C_OK) ? "ok" : "err",
            (server.aof_last_write_status == C_OK) ? "ok" : "err");

        if (server.aof_state != AOF_OFF) {
            info = sdscatprintf(info,
                "aof_current_size:%lld\r\n"
                "aof_base_size:%lld\r\n"
                "aof_pending_rewrite:%d\r\n"
                "aof_buffer_length:%zu\r\n"
                "aof_rewrite_buffer_length:%lu\r\n"
                "aof_pending_bio_fsync:%llu\r\n"
                "aof_delayed_fsync:%lu\r\n",
                (long long) server.aof_current_size,
                (long long) server.aof_rewrite_base_size,
                server.aof_rewrite_scheduled,
                sdslen(server.aof_buf),
                aofRewriteBufferSize(),
                bioPendingJobsOfType(BIO_AOF_FSYNC),
                server.aof_delayed_fsync);
        }

        if (server.loading) {
            double perc;
            time_t eta, elapsed;
            off_t remaining_bytes = server.loading_total_bytes-
                                    server.loading_loaded_bytes;

            perc = ((double)server.loading_loaded_bytes /
                   (server.loading_total_bytes+1)) * 100;

            elapsed = time(NULL)-server.loading_start_time;
            if (elapsed == 0) {
                eta = 1; /* A fake 1 second figure if we don't have
                            enough info */
            } else {
                eta = (elapsed*remaining_bytes)/(server.loading_loaded_bytes+1);
            }

            info = sdscatprintf(info,
                "loading_start_time:%jd\r\n"
                "loading_total_bytes:%llu\r\n"
                "loading_loaded_bytes:%llu\r\n"
                "loading_loaded_perc:%.2f\r\n"
                "loading_eta_seconds:%jd\r\n",
                (intmax_t) server.loading_start_time,
                (unsigned long long) server.loading_total_bytes,
                (unsigned long long) server.loading_loaded_bytes,
                perc,
                (intmax_t)eta
            );
        }
    }

    /* Stats */
    if (allsections || defsections || !strcasecmp(section,"stats")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Stats\r\n"
            "total_connections_received:%lld\r\n"
            "total_commands_processed:%lld\r\n"
            "instantaneous_ops_per_sec:%lld\r\n"
            "total_net_input_bytes:%lld\r\n"
            "total_net_output_bytes:%lld\r\n"
            "instantaneous_input_kbps:%.2f\r\n"
            "instantaneous_output_kbps:%.2f\r\n"
            "rejected_connections:%lld\r\n"
            "sync_full:%lld\r\n"
            "sync_partial_ok:%lld\r\n"
            "sync_partial_err:%lld\r\n"
            "expired_keys:%lld\r\n"
            "evicted_keys:%lld\r\n"
            "keyspace_hits:%lld\r\n"
            "keyspace_misses:%lld\r\n"
            "pubsub_channels:%ld\r\n"
            "pubsub_patterns:%lu\r\n"
            "latest_fork_usec:%lld\r\n"
            "migrate_cached_sockets:%ld\r\n",
            server.stat_numconnections,
            server.stat_numcommands,
            getInstantaneousMetric(STATS_METRIC_COMMAND),
            server.stat_net_input_bytes,
            server.stat_net_output_bytes,
            (float)getInstantaneousMetric(STATS_METRIC_NET_INPUT)/1024,
            (float)getInstantaneousMetric(STATS_METRIC_NET_OUTPUT)/1024,
            server.stat_rejected_conn,
            server.stat_sync_full,
            server.stat_sync_partial_ok,
            server.stat_sync_partial_err,
            server.stat_expiredkeys,
            server.stat_evictedkeys,
            server.stat_keyspace_hits,
            server.stat_keyspace_misses,
            dictSize(server.pubsub_channels),
            listLength(server.pubsub_patterns),
            server.stat_fork_time,
            dictSize(server.migrate_cached_sockets));
    }

    /* Replication */
    if (allsections || defsections || !strcasecmp(section,"replication")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Replication\r\n"
            "role:%s\r\n",
            server.masterhost == NULL ? "master" : "slave");
        if (server.masterhost) {
            long long slave_repl_offset = 1;

            if (server.master)
                slave_repl_offset = server.master->reploff;
            else if (server.cached_master)
                slave_repl_offset = server.cached_master->reploff;

            info = sdscatprintf(info,
                "master_host:%s\r\n"
                "master_port:%d\r\n"
                "master_link_status:%s\r\n"
                "master_last_io_seconds_ago:%d\r\n"
                "master_sync_in_progress:%d\r\n"
                "slave_repl_offset:%lld\r\n"
                ,server.masterhost,
                server.masterport,
                (server.repl_state == REPL_STATE_CONNECTED) ?
                    "up" : "down",
                server.master ?
                ((int)(server.unixtime-server.master->lastinteraction)) : -1,
                server.repl_state == REPL_STATE_TRANSFER,
                slave_repl_offset
            );

            if (server.repl_state == REPL_STATE_TRANSFER) {
                info = sdscatprintf(info,
                    "master_sync_left_bytes:%lld\r\n"
                    "master_sync_last_io_seconds_ago:%d\r\n"
                    , (long long)
                        (server.repl_transfer_size - server.repl_transfer_read),
                    (int)(server.unixtime-server.repl_transfer_lastio)
                );
            }

            if (server.repl_state != REPL_STATE_CONNECTED) {
                info = sdscatprintf(info,
                    "master_link_down_since_seconds:%jd\r\n",
                    (intmax_t)server.unixtime-server.repl_down_since);
            }
            info = sdscatprintf(info,
                "slave_priority:%d\r\n"
                "slave_read_only:%d\r\n",
                server.slave_priority,
                server.repl_slave_ro);
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
                char *state = NULL;
                char ip[NET_IP_STR_LEN], *slaveip = slave->slave_ip;
                int port;
                long lag = 0;

                if (slaveip[0] == '\0') {
                    if (anetPeerToString(slave->fd,ip,sizeof(ip),&port) == -1)
                        continue;
                    slaveip = ip;
                }
                switch(slave->replstate) {
                case SLAVE_STATE_WAIT_BGSAVE_START:
                case SLAVE_STATE_WAIT_BGSAVE_END:
                    state = "wait_bgsave";
                    break;
                case SLAVE_STATE_SEND_BULK:
                    state = "send_bulk";
                    break;
                case SLAVE_STATE_ONLINE:
                    state = "online";
                    break;
                }
                if (state == NULL) continue;
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
        info = sdscatprintf(info,
            "master_repl_offset:%lld\r\n"
            "repl_backlog_active:%d\r\n"
            "repl_backlog_size:%lld\r\n"
            "repl_backlog_first_byte_offset:%lld\r\n"
            "repl_backlog_histlen:%lld\r\n",
            server.master_repl_offset,
            server.repl_backlog != NULL,
            server.repl_backlog_size,
            server.repl_backlog_off,
            server.repl_backlog_histlen);
    }

    /* CPU */
    if (allsections || defsections || !strcasecmp(section,"cpu")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
        "# CPU\r\n"
        "used_cpu_sys:%.2f\r\n"
        "used_cpu_user:%.2f\r\n"
        "used_cpu_sys_children:%.2f\r\n"
        "used_cpu_user_children:%.2f\r\n",
        (float)self_ru.ru_stime.tv_sec+(float)self_ru.ru_stime.tv_usec/1000000,
        (float)self_ru.ru_utime.tv_sec+(float)self_ru.ru_utime.tv_usec/1000000,
        (float)c_ru.ru_stime.tv_sec+(float)c_ru.ru_stime.tv_usec/1000000,
        (float)c_ru.ru_utime.tv_sec+(float)c_ru.ru_utime.tv_usec/1000000);
    }

    /* cmdtime */
    if (allsections || !strcasecmp(section,"commandstats")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Commandstats\r\n");
        numcommands = sizeof(redisCommandTable)/sizeof(struct redisCommand);
        for (j = 0; j < numcommands; j++) {
            struct redisCommand *c = redisCommandTable+j;

            if (!c->calls) continue;
            info = sdscatprintf(info,
                "cmdstat_%s:calls=%lld,usec=%lld,usec_per_call=%.2f\r\n",
                c->name, c->calls, c->microseconds,
                (c->calls == 0) ? 0 : ((float)c->microseconds/c->calls));
        }
    }

    /* Cluster */
    if (allsections || defsections || !strcasecmp(section,"cluster")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
        "# Cluster\r\n"
        "cluster_enabled:%d\r\n",
        server.cluster_enabled);
    }

    /* Key space */
    if (allsections || defsections || !strcasecmp(section,"keyspace")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Keyspace\r\n");
        for (j = 0; j < server.dbnum; j++) {
            long long keys, vkeys;

            keys = dictSize(server.db[j].dict);
            vkeys = dictSize(server.db[j].expires);
            if (keys || vkeys) {
                info = sdscatprintf(info,
                    "db%d:keys=%lld,expires=%lld,avg_ttl=%lld\r\n",
                    j, keys, vkeys, server.db[j].avg_ttl);
            }
        }
    }
    return info;
}

void infoCommand(client *c) {
    char *section = c->argc == 2 ? c->argv[1]->ptr : "default";

    if (c->argc > 2) {
        addReply(c,shared.syntaxerr);
        return;
    }
    addReplyBulkSds(c, genRedisInfoString(section));
}

void monitorCommand(client *c) {
    /* ignore MONITOR if already slave or in monitor mode */
    if (c->flags & CLIENT_SLAVE) return;

    c->flags |= (CLIENT_SLAVE|CLIENT_MONITOR);
    listAddNodeTail(server.monitors,c);
    addReply(c,shared.ok);
}

/* ============================ Maxmemory directive  ======================== */

/* freeMemoryIfNeeded() gets called when 'maxmemory' is set on the config
 * file to limit the max memory used by the server, before processing a
 * command.
 *
 * The goal of the function is to free enough memory to keep Redis under the
 * configured memory limit.
 *
 * The function starts calculating how many bytes should be freed to keep
 * Redis under the limit, and enters a loop selecting the best keys to
 * evict accordingly to the configured policy.
 *
 * If all the bytes needed to return back under the limit were freed the
 * function returns C_OK, otherwise C_ERR is returned, and the caller
 * should block the execution of commands that will result in more memory
 * used by the server.
 *
 * ------------------------------------------------------------------------
 *
 * LRU approximation algorithm
 *
 * Redis uses an approximation of the LRU algorithm that runs in constant
 * memory. Every time there is a key to expire, we sample N keys (with
 * N very small, usually in around 5) to populate a pool of best keys to
 * evict of M keys (the pool size is defined by MAXMEMORY_EVICTION_POOL_SIZE).
 *
 * The N keys sampled are added in the pool of good keys to expire (the one
 * with an old access time) if they are better than one of the current keys
 * in the pool.
 *
 * After the pool is populated, the best key we have in the pool is expired.
 * However note that we don't remove keys from the pool when they are deleted
 * so the pool may contain keys that no longer exist.
 *
 * When we try to evict a key, and all the entries in the pool don't exist
 * we populate it again. This time we'll be sure that the pool has at least
 * one key that can be evicted, if there is at least one key that can be
 * evicted in the whole database. */

/* Create a new eviction pool. */
struct evictionPoolEntry *evictionPoolAlloc(void) {
    struct evictionPoolEntry *ep;
    int j;

    ep = zmalloc(sizeof(*ep)*MAXMEMORY_EVICTION_POOL_SIZE);
    for (j = 0; j < MAXMEMORY_EVICTION_POOL_SIZE; j++) {
        ep[j].idle = 0;
        ep[j].key = NULL;
    }
    return ep;
}

/* This is an helper function for freeMemoryIfNeeded(), it is used in order
 * to populate the evictionPool with a few entries every time we want to
 * expire a key. Keys with idle time smaller than one of the current
 * keys are added. Keys are always added if there are free entries.
 *
 * We insert keys on place in ascending order, so keys with the smaller
 * idle time are on the left, and keys with the higher idle time on the
 * right. */

#define EVICTION_SAMPLES_ARRAY_SIZE 16
void evictionPoolPopulate(dict *sampledict, dict *keydict, struct evictionPoolEntry *pool) {
    int j, k, count;
    dictEntry *_samples[EVICTION_SAMPLES_ARRAY_SIZE];
    dictEntry **samples;

    /* Try to use a static buffer: this function is a big hit...
     * Note: it was actually measured that this helps. */
    if (server.maxmemory_samples <= EVICTION_SAMPLES_ARRAY_SIZE) {
        samples = _samples;
    } else {
        samples = zmalloc(sizeof(samples[0])*server.maxmemory_samples);
    }

    count = dictGetSomeKeys(sampledict,samples,server.maxmemory_samples);
    for (j = 0; j < count; j++) {
        unsigned long long idle;
        sds key;
        robj *o;
        dictEntry *de;

        de = samples[j];
        key = dictGetKey(de);
        /* If the dictionary we are sampling from is not the main
         * dictionary (but the expires one) we need to lookup the key
         * again in the key dictionary to obtain the value object. */
        if (sampledict != keydict) de = dictFind(keydict, key);
        o = dictGetVal(de);
        idle = estimateObjectIdleTime(o);

        /* Insert the element inside the pool.
         * First, find the first empty bucket or the first populated
         * bucket that has an idle time smaller than our idle time. */
        k = 0;
        while (k < MAXMEMORY_EVICTION_POOL_SIZE &&
               pool[k].key &&
               pool[k].idle < idle) k++;
        if (k == 0 && pool[MAXMEMORY_EVICTION_POOL_SIZE-1].key != NULL) {
            /* Can't insert if the element is < the worst element we have
             * and there are no empty buckets. */
            continue;
        } else if (k < MAXMEMORY_EVICTION_POOL_SIZE && pool[k].key == NULL) {
            /* Inserting into empty position. No setup needed before insert. */
        } else {
            /* Inserting in the middle. Now k points to the first element
             * greater than the element to insert.  */
            if (pool[MAXMEMORY_EVICTION_POOL_SIZE-1].key == NULL) {
                /* Free space on the right? Insert at k shifting
                 * all the elements from k to end to the right. */
                memmove(pool+k+1,pool+k,
                    sizeof(pool[0])*(MAXMEMORY_EVICTION_POOL_SIZE-k-1));
            } else {
                /* No free space on right? Insert at k-1 */
                k--;
                /* Shift all elements on the left of k (included) to the
                 * left, so we discard the element with smaller idle time. */
                sdsfree(pool[0].key);
                memmove(pool,pool+1,sizeof(pool[0])*k);
            }
        }
        pool[k].key = sdsdup(key);
        pool[k].idle = idle;
    }
    if (samples != _samples) zfree(samples);
}

int freeMemoryIfNeeded(void) {
    size_t mem_used, mem_tofree, mem_freed;
    int slaves = listLength(server.slaves);
    mstime_t latency, eviction_latency;

    /* Remove the size of slaves output buffers and AOF buffer from the
     * count of used memory. */
    mem_used = zmalloc_used_memory();
    if (slaves) {
        listIter li;
        listNode *ln;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = listNodeValue(ln);
            unsigned long obuf_bytes = getClientOutputBufferMemoryUsage(slave);
            if (obuf_bytes > mem_used)
                mem_used = 0;
            else
                mem_used -= obuf_bytes;
        }
    }
    if (server.aof_state != AOF_OFF) {
        mem_used -= sdslen(server.aof_buf);
        mem_used -= aofRewriteBufferSize();
    }

    /* Check if we are over the memory limit. */
    if (mem_used <= server.maxmemory) return C_OK;

    if (server.maxmemory_policy == MAXMEMORY_NO_EVICTION)
        return C_ERR; /* We need to free memory, but policy forbids. */

    /* Compute how much memory we need to free. */
    mem_tofree = mem_used - server.maxmemory;
    mem_freed = 0;
    latencyStartMonitor(latency);
    while (mem_freed < mem_tofree) {
        int j, k, keys_freed = 0;

        for (j = 0; j < server.dbnum; j++) {
            long bestval = 0; /* just to prevent warning */
            sds bestkey = NULL;
            dictEntry *de;
            redisDb *db = server.db+j;
            dict *dict;

            if (server.maxmemory_policy == MAXMEMORY_ALLKEYS_LRU ||
                server.maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM)
            {
                dict = server.db[j].dict;
            } else {
                dict = server.db[j].expires;
            }
            if (dictSize(dict) == 0) continue;

            /* volatile-random and allkeys-random policy */
            if (server.maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM ||
                server.maxmemory_policy == MAXMEMORY_VOLATILE_RANDOM)
            {
                de = dictGetRandomKey(dict);
                bestkey = dictGetKey(de);
            }

            /* volatile-lru and allkeys-lru policy */
            else if (server.maxmemory_policy == MAXMEMORY_ALLKEYS_LRU ||
                server.maxmemory_policy == MAXMEMORY_VOLATILE_LRU)
            {
                struct evictionPoolEntry *pool = db->eviction_pool;

                while(bestkey == NULL) {
                    evictionPoolPopulate(dict, db->dict, db->eviction_pool);
                    /* Go backward from best to worst element to evict. */
                    for (k = MAXMEMORY_EVICTION_POOL_SIZE-1; k >= 0; k--) {
                        if (pool[k].key == NULL) continue;
                        de = dictFind(dict,pool[k].key);

                        /* Remove the entry from the pool. */
                        sdsfree(pool[k].key);
                        /* Shift all elements on its right to left. */
                        memmove(pool+k,pool+k+1,
                            sizeof(pool[0])*(MAXMEMORY_EVICTION_POOL_SIZE-k-1));
                        /* Clear the element on the right which is empty
                         * since we shifted one position to the left.  */
                        pool[MAXMEMORY_EVICTION_POOL_SIZE-1].key = NULL;
                        pool[MAXMEMORY_EVICTION_POOL_SIZE-1].idle = 0;

                        /* If the key exists, is our pick. Otherwise it is
                         * a ghost and we need to try the next element. */
                        if (de) {
                            bestkey = dictGetKey(de);
                            break;
                        } else {
                            /* Ghost... */
                            continue;
                        }
                    }
                }
            }

            /* volatile-ttl */
            else if (server.maxmemory_policy == MAXMEMORY_VOLATILE_TTL) {
                for (k = 0; k < server.maxmemory_samples; k++) {
                    sds thiskey;
                    long thisval;

                    de = dictGetRandomKey(dict);
                    thiskey = dictGetKey(de);
                    thisval = (long) dictGetVal(de);

                    /* Expire sooner (minor expire unix timestamp) is better
                     * candidate for deletion */
                    if (bestkey == NULL || thisval < bestval) {
                        bestkey = thiskey;
                        bestval = thisval;
                    }
                }
            }

            /* Finally remove the selected key. */
            if (bestkey) {
                long long delta;

                robj *keyobj = createStringObject(bestkey,sdslen(bestkey));
                propagateExpire(db,keyobj);
                /* We compute the amount of memory freed by dbDelete() alone.
                 * It is possible that actually the memory needed to propagate
                 * the DEL in AOF and replication link is greater than the one
                 * we are freeing removing the key, but we can't account for
                 * that otherwise we would never exit the loop.
                 *
                 * AOF and Output buffer memory will be freed eventually so
                 * we only care about memory used by the key space. */
                delta = (long long) zmalloc_used_memory();
                latencyStartMonitor(eviction_latency);
                dbDelete(db,keyobj);
                latencyEndMonitor(eviction_latency);
                latencyAddSampleIfNeeded("eviction-del",eviction_latency);
                latencyRemoveNestedEvent(latency,eviction_latency);
                delta -= (long long) zmalloc_used_memory();
                mem_freed += delta;
                server.stat_evictedkeys++;
                notifyKeyspaceEvent(NOTIFY_EVICTED, "evicted",
                    keyobj, db->id);
                decrRefCount(keyobj);
                keys_freed++;

                /* When the memory to free starts to be big enough, we may
                 * start spending so much time here that is impossible to
                 * deliver data to the slaves fast enough, so we force the
                 * transmission here inside the loop. */
                if (slaves) flushSlavesOutputBuffers();
            }
        }
        if (!keys_freed) {
            latencyEndMonitor(latency);
            latencyAddSampleIfNeeded("eviction-cycle",latency);
            return C_ERR; /* nothing to free... */
        }
    }
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("eviction-cycle",latency);
    return C_OK;
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

void linuxMemoryWarnings(void) {
    if (linuxOvercommitMemoryValue() == 0) {
        serverLog(LL_WARNING,"WARNING overcommit_memory is set to 0! Background save may fail under low memory condition. To fix this issue add 'vm.overcommit_memory = 1' to /etc/sysctl.conf and then reboot or run the command 'sysctl vm.overcommit_memory=1' for this to take effect.");
    }
    if (THPIsEnabled()) {
        serverLog(LL_WARNING,"WARNING you have Transparent Huge Pages (THP) support enabled in your kernel. This will create latency and memory usage issues with Redis. To fix this issue run the command 'echo never > /sys/kernel/mm/transparent_hugepage/enabled' as root, and add it to your /etc/rc.local in order to retain the setting after a reboot. Redis must be restarted after THP is disabled.");
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

void version(void) {
    printf("Redis server v=%s sha=%s:%d malloc=%s bits=%d build=%llx\n",
        REDIS_VERSION,
        redisGitSHA1(),
        atoi(redisGitDirty()) > 0,
        ZMALLOC_LIB,
        sizeof(long) == 4 ? 32 : 64,
        (unsigned long long) redisBuildId());
    exit(0);
}

void usage(void) {
    fprintf(stderr,"Usage: ./redis-server [/path/to/redis.conf] [options]\n");
    fprintf(stderr,"       ./redis-server - (read config from stdin)\n");
    fprintf(stderr,"       ./redis-server -v or --version\n");
    fprintf(stderr,"       ./redis-server -h or --help\n");
    fprintf(stderr,"       ./redis-server --test-memory <megabytes>\n\n");
    fprintf(stderr,"Examples:\n");
    fprintf(stderr,"       ./redis-server (run the server with default conf)\n");
    fprintf(stderr,"       ./redis-server /etc/redis/6379.conf\n");
    fprintf(stderr,"       ./redis-server --port 7777\n");
    fprintf(stderr,"       ./redis-server --port 7777 --slaveof 127.0.0.1 8888\n");
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

    if (server.syslog_enabled) {
        serverLog(LL_NOTICE,
            "Redis %s (%s/%d) %s bit, %s mode, port %d, pid %ld ready to start.",
            REDIS_VERSION,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            (sizeof(long) == 8) ? "64" : "32",
            mode, server.port,
            (long) getpid()
        );
    } else {
        snprintf(buf,1024*16,ascii_logo,
            REDIS_VERSION,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            (sizeof(long) == 8) ? "64" : "32",
            mode, server.port,
            (long) getpid()
        );
        serverLogRaw(LL_NOTICE|LL_RAW,buf);
    }
    zfree(buf);
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
     * on disk. */
    if (server.shutdown_asap && sig == SIGINT) {
        serverLogFromHandler(LL_WARNING, "You insist... exiting now.");
        rdbRemoveTempFile(getpid());
        exit(1); /* Exit with an error since this was not a clean shutdown. */
    } else if (server.loading) {
        exit(0);
    }

    serverLogFromHandler(LL_WARNING, msg);
    server.shutdown_asap = 1;
}

void setupSignalHandlers(void) {
    struct sigaction act;

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used.
     * Otherwise, sa_handler is used. */
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigShutdownHandler;
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT, &act, NULL);

#ifdef HAVE_BACKTRACE
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESETHAND | SA_SIGINFO;
    act.sa_sigaction = sigsegvHandler;
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGFPE, &act, NULL);
    sigaction(SIGILL, &act, NULL);
#endif
    return;
}

void memtest(size_t megabytes, int passes);

/* Returns 1 if there is --sentinel among the arguments or if
 * argv[0] is exactly "redis-sentinel". */
int checkForSentinelMode(int argc, char **argv) {
    int j;

    if (strstr(argv[0],"redis-sentinel") != NULL) return 1;
    for (j = 1; j < argc; j++)
        if (!strcmp(argv[j],"--sentinel")) return 1;
    return 0;
}

/* Function called at startup to load RDB or AOF file in memory. */
void loadDataFromDisk(void) {
    long long start = ustime();
    if (server.aof_state == AOF_ON) {
        if (loadAppendOnlyFile(server.aof_filename) == C_OK)
            serverLog(LL_NOTICE,"DB loaded from append only file: %.3f seconds",(float)(ustime()-start)/1000000);
    } else {
        if (rdbLoad(server.rdb_filename) == C_OK) {
            serverLog(LL_NOTICE,"DB loaded from disk: %.3f seconds",
                (float)(ustime()-start)/1000000);
        } else if (errno != ENOENT) {
            serverLog(LL_WARNING,"Fatal error loading the DB: %s. Exiting.",strerror(errno));
            exit(1);
        }
    }
}

void redisOutOfMemoryHandler(size_t allocation_size) {
    serverLog(LL_WARNING,"Out Of Memory allocating %zu bytes!",
        allocation_size);
    serverPanic("Redis aborting for OUT OF MEMORY");
}

void redisSetProcTitle(char *title) {
#ifdef USE_SETPROCTITLE
    char *server_mode = "";
    if (server.cluster_enabled) server_mode = " [cluster]";
    else if (server.sentinel_mode) server_mode = " [sentinel]";

    setproctitle("%s %s:%d%s",
        title,
        server.bindaddr_count ? server.bindaddr[0] : "*",
        server.port,
        server_mode);
#else
    UNUSED(title);
#endif
}

/*
 * Check whether systemd or upstart have been used to start redis.
 */

int redisSupervisedUpstart(void) {
    const char *upstart_job = getenv("UPSTART_JOB");

    if (!upstart_job) {
        serverLog(LL_WARNING,
                "upstart supervision requested, but UPSTART_JOB not found");
        return 0;
    }

    serverLog(LL_NOTICE, "supervised by upstart, will stop to signal readiness");
    raise(SIGSTOP);
    unsetenv("UPSTART_JOB");
    return 1;
}

int redisSupervisedSystemd(void) {
    const char *notify_socket = getenv("NOTIFY_SOCKET");
    int fd = 1;
    struct sockaddr_un su;
    struct iovec iov;
    struct msghdr hdr;
    int sendto_flags = 0;

    if (!notify_socket) {
        serverLog(LL_WARNING,
                "systemd supervision requested, but NOTIFY_SOCKET not found");
        return 0;
    }

    if ((strchr("@/", notify_socket[0])) == NULL || strlen(notify_socket) < 2) {
        return 0;
    }

    serverLog(LL_NOTICE, "supervised by systemd, will signal readiness");
    if ((fd = socket(AF_UNIX, SOCK_DGRAM, 0)) == -1) {
        serverLog(LL_WARNING,
                "Can't connect to systemd socket %s", notify_socket);
        return 0;
    }

    memset(&su, 0, sizeof(su));
    su.sun_family = AF_UNIX;
    strncpy (su.sun_path, notify_socket, sizeof(su.sun_path) -1);
    su.sun_path[sizeof(su.sun_path) - 1] = '\0';

    if (notify_socket[0] == '@')
        su.sun_path[0] = '\0';

    memset(&iov, 0, sizeof(iov));
    iov.iov_base = "READY=1";
    iov.iov_len = strlen("READY=1");

    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_name = &su;
    hdr.msg_namelen = offsetof(struct sockaddr_un, sun_path) +
        strlen(notify_socket);
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;

    unsetenv("NOTIFY_SOCKET");
#ifdef HAVE_MSG_NOSIGNAL
    sendto_flags |= MSG_NOSIGNAL;
#endif
    if (sendmsg(fd, &hdr, sendto_flags) < 0) {
        serverLog(LL_WARNING, "Can't send notification to systemd");
        close(fd);
        return 0;
    }
    close(fd);
    return 1;
}

int redisIsSupervised(int mode) {
    if (mode == SUPERVISED_AUTODETECT) {
        const char *upstart_job = getenv("UPSTART_JOB");
        const char *notify_socket = getenv("NOTIFY_SOCKET");

        if (upstart_job) {
            redisSupervisedUpstart();
        } else if (notify_socket) {
            redisSupervisedSystemd();
        }
    } else if (mode == SUPERVISED_UPSTART) {
        return redisSupervisedUpstart();
    } else if (mode == SUPERVISED_SYSTEMD) {
        return redisSupervisedSystemd();
    }

    return 0;
}


int main(int argc, char **argv) {
    struct timeval tv;
    int j;

#ifdef REDIS_TEST
    if (argc == 3 && !strcasecmp(argv[1], "test")) {
        if (!strcasecmp(argv[2], "ziplist")) {
            return ziplistTest(argc, argv);
        } else if (!strcasecmp(argv[2], "quicklist")) {
            quicklistTest(argc, argv);
        } else if (!strcasecmp(argv[2], "intset")) {
            return intsetTest(argc, argv);
        } else if (!strcasecmp(argv[2], "zipmap")) {
            return zipmapTest(argc, argv);
        } else if (!strcasecmp(argv[2], "sha1test")) {
            return sha1Test(argc, argv);
        } else if (!strcasecmp(argv[2], "util")) {
            return utilTest(argc, argv);
        } else if (!strcasecmp(argv[2], "sds")) {
            return sdsTest(argc, argv);
        } else if (!strcasecmp(argv[2], "endianconv")) {
            return endianconvTest(argc, argv);
        } else if (!strcasecmp(argv[2], "crc64")) {
            return crc64Test(argc, argv);
        }

        return -1; /* test not found */
    }
#endif

    /* We need to initialize our libraries, and the server configuration. */
#ifdef INIT_SETPROCTITLE_REPLACEMENT
    spt_init(argc, argv);
#endif
    setlocale(LC_COLLATE,"");
    zmalloc_enable_thread_safeness();
    zmalloc_set_oom_handler(redisOutOfMemoryHandler);
    srand(time(NULL)^getpid());
    gettimeofday(&tv,NULL);
    dictSetHashFunctionSeed(tv.tv_sec^tv.tv_usec^getpid());
    server.sentinel_mode = checkForSentinelMode(argc,argv);
    initServerConfig();

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

    /* Check if we need to start in redis-check-rdb mode. We just execute
     * the program main. However the program is part of the Redis executable
     * so that we can easily execute an RDB check on loading errors. */
    if (strstr(argv[0],"redis-check-rdb") != NULL)
        redis_check_rdb_main(argc,argv);

    if (argc >= 2) {
        j = 1; /* First option to parse in argv[] */
        sds options = sdsempty();
        char *configfile = NULL;

        /* Handle special options --help and --version */
        if (strcmp(argv[1], "-v") == 0 ||
            strcmp(argv[1], "--version") == 0) version();
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
        }

        /* First argument is the config file name? */
        if (argv[j][0] != '-' || argv[j][1] != '-') {
            configfile = argv[j];
            server.configfile = getAbsolutePath(configfile);
            /* Replace the config file in server.exec_argv with
             * its absoulte path. */
            zfree(server.exec_argv[j]);
            server.exec_argv[j] = zstrdup(server.configfile);
            j++;
        }

        /* All the other options are parsed and conceptually appended to the
         * configuration file. For instance --port 6380 will generate the
         * string "port 6380\n" to be parsed after the actual file name
         * is parsed, if any. */
        while(j != argc) {
            if (argv[j][0] == '-' && argv[j][1] == '-') {
                /* Option name */
                if (!strcmp(argv[j], "--check-rdb")) {
                    /* Argument has no options, need to skip for parsing. */
                    j++;
                    continue;
                }
                if (sdslen(options)) options = sdscat(options,"\n");
                options = sdscat(options,argv[j]+2);
                options = sdscat(options," ");
            } else {
                /* Option argument */
                options = sdscatrepr(options,argv[j],strlen(argv[j]));
                options = sdscat(options," ");
            }
            j++;
        }
        if (server.sentinel_mode && configfile && *configfile == '-') {
            serverLog(LL_WARNING,
                "Sentinel config from STDIN not allowed.");
            serverLog(LL_WARNING,
                "Sentinel needs config file on disk to save state.  Exiting...");
            exit(1);
        }
        resetServerSaveParams();
        loadServerConfig(configfile,options);
        sdsfree(options);
    } else {
        serverLog(LL_WARNING, "Warning: no config file specified, using the default config. In order to specify a config file use %s /path/to/%s.conf", argv[0], server.sentinel_mode ? "sentinel" : "redis");
    }

    server.supervised = redisIsSupervised(server.supervised_mode);
    int background = server.daemonize && !server.supervised;
    if (background) daemonize();

    initServer();
    if (background || server.pidfile) createPidFile();
    redisSetProcTitle(argv[0]);
    redisAsciiArt();
    checkTcpBacklogSettings();

    if (!server.sentinel_mode) {
        /* Things not needed when running in Sentinel mode. */
        serverLog(LL_WARNING,"Server started, Redis version " REDIS_VERSION);
    #ifdef __linux__
        linuxMemoryWarnings();
    #endif
        loadDataFromDisk();
        if (server.cluster_enabled) {
            if (verifyClusterConfigWithData() == C_ERR) {
                serverLog(LL_WARNING,
                    "You can't have keys in a DB different than DB 0 when in "
                    "Cluster mode. Exiting.");
                exit(1);
            }
        }
        if (server.ipfd_count > 0)
            serverLog(LL_NOTICE,"The server is now ready to accept connections on port %d", server.port);
        if (server.sofd > 0)
            serverLog(LL_NOTICE,"The server is now ready to accept connections at %s", server.unixsocket);
    } else {
        sentinelIsRunning();
    }

    /* Warning the user about suspicious maxmemory setting. */
    if (server.maxmemory > 0 && server.maxmemory < 1024*1024) {
        serverLog(LL_WARNING,"WARNING: You specified a maxmemory value that is less than 1MB (current value is %llu bytes). Are you sure this is what you really want?", server.maxmemory);
    }

    aeSetBeforeSleepProc(server.el,beforeSleep);
    aeMain(server.el);
    aeDeleteEventLoop(server.el);
    return 0;
}

/* The End */

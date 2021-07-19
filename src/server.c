/*
 * Copyright (c) 2009-2016, Salvatore Sanfilippo <antirez at gmail dot com>
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
#include "monotonic.h"
#include "cluster.h"
#include "slowlog.h"
#include "bio.h"
#include "latency.h"
#include "atomicvar.h"
#include "mt19937-64.h"

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
#include <sys/resource.h>

#ifdef __linux__
#include <sys/mman.h>
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

/* Our command table.
 *
 * Every entry is composed of the following fields:
 *
 * name:        A string representing the command name.
 *
 * function:    Pointer to the C function implementing the command.
 *
 * arity:       Number of arguments, it is possible to use -N to say >= N
 *
 * sflags:      Command flags as string. See below for a table of flags.
 *
 * flags:       Flags as bitmask. Computed by Redis using the 'sflags' field.
 *
 * get_keys_proc: An optional function to get key arguments from a command.
 *                This is only used when the following three fields are not
 *                enough to specify what arguments are keys.
 *
 * first_key_index: First argument that is a key
 *
 * last_key_index: Last argument that is a key
 *
 * key_step:    Step to get all the keys from first to last argument.
 *              For instance in MSET the step is two since arguments
 *              are key,val,key,val,...
 *
 * microseconds: Microseconds of total execution time for this command.
 *
 * calls:       Total number of calls of this command.
 *
 * id:          Command bit identifier for ACLs or other goals.
 *
 * The flags, microseconds and calls fields are computed by Redis and should
 * always be set to zero.
 *
 * Command flags are expressed using space separated strings, that are turned
 * into actual flags by the populateCommandTable() function.
 *
 * This is the meaning of the flags:
 *
 * write:       Write command (may modify the key space).
 *
 * read-only:   Commands just reading from keys without changing the content.
 *              Note that commands that don't read from the keyspace such as
 *              TIME, SELECT, INFO, administrative commands, and connection
 *              or transaction related commands (multi, exec, discard, ...)
 *              are not flagged as read-only commands, since they affect the
 *              server or the connection in other ways.
 *
 * use-memory:  May increase memory usage once called. Don't allow if out
 *              of memory.
 *
 * admin:       Administrative command, like SAVE or SHUTDOWN.
 *
 * pub-sub:     Pub/Sub related command.
 *
 * no-script:   Command not allowed in scripts.
 *
 * random:      Random command. Command is not deterministic, that is, the same
 *              command with the same arguments, with the same key space, may
 *              have different results. For instance SPOP and RANDOMKEY are
 *              two random commands.
 *
 * to-sort:     Sort command output array if called from script, so that the
 *              output is deterministic. When this flag is used (not always
 *              possible), then the "random" flag is not needed.
 *
 * ok-loading:  Allow the command while loading the database.
 *
 * ok-stale:    Allow the command while a slave has stale data but is not
 *              allowed to serve this data. Normally no command is accepted
 *              in this condition but just a few.
 *
 * no-monitor:  Do not automatically propagate the command on MONITOR.
 *
 * no-slowlog:  Do not automatically propagate the command to the slowlog.
 *
 * cluster-asking: Perform an implicit ASKING for this command, so the
 *              command will be accepted in cluster mode if the slot is marked
 *              as 'importing'.
 *
 * fast:        Fast command: O(1) or O(log(N)) command that should never
 *              delay its execution as long as the kernel scheduler is giving
 *              us time. Note that commands that may trigger a DEL as a side
 *              effect (like SET) are not fast commands.
 * 
 * may-replicate: Command may produce replication traffic, but should be 
 *                allowed under circumstances where write commands are disallowed. 
 *                Examples include PUBLISH, which replicates pubsub messages,and 
 *                EVAL, which may execute write commands, which are replicated, 
 *                or may just execute read commands. A command can not be marked 
 *                both "write" and "may-replicate"
 *
 * The following additional flags are only used in order to put commands
 * in a specific ACL category. Commands can have multiple ACL categories.
 *
 * @keyspace, @read, @write, @set, @sortedset, @list, @hash, @string, @bitmap,
 * @hyperloglog, @stream, @admin, @fast, @slow, @pubsub, @blocking, @dangerous,
 * @connection, @transaction, @scripting, @geo.
 *
 * Note that:
 *
 * 1) The read-only flag implies the @read ACL category.
 * 2) The write flag implies the @write ACL category.
 * 3) The fast flag implies the @fast ACL category.
 * 4) The admin flag implies the @admin and @dangerous ACL category.
 * 5) The pub-sub flag implies the @pubsub ACL category.
 * 6) The lack of fast flag implies the @slow ACL category.
 * 7) The non obvious "keyspace" category includes the commands
 *    that interact with keys without having anything to do with
 *    specific data structures, such as: DEL, RENAME, MOVE, SELECT,
 *    TYPE, EXPIRE*, PEXPIRE*, TTL, PTTL, ...
 */

struct redisCommand redisCommandTable[] = {
    {"module",moduleCommand,-2,
     "admin no-script",
     0,NULL,0,0,0,0,0,0},

    {"get",getCommand,2,
     "read-only fast @string",
     0,NULL,1,1,1,0,0,0},

    {"getex",getexCommand,-2,
     "write fast @string",
     0,NULL,1,1,1,0,0,0},

    {"getdel",getdelCommand,2,
     "write fast @string",
     0,NULL,1,1,1,0,0,0},

    /* Note that we can't flag set as fast, since it may perform an
     * implicit DEL of a large key. */
    {"set",setCommand,-3,
     "write use-memory @string",
     0,NULL,1,1,1,0,0,0},

    {"setnx",setnxCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"setex",setexCommand,4,
     "write use-memory @string",
     0,NULL,1,1,1,0,0,0},

    {"psetex",psetexCommand,4,
     "write use-memory @string",
     0,NULL,1,1,1,0,0,0},

    {"append",appendCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"strlen",strlenCommand,2,
     "read-only fast @string",
     0,NULL,1,1,1,0,0,0},

    {"del",delCommand,-2,
     "write @keyspace",
     0,NULL,1,-1,1,0,0,0},

    {"unlink",unlinkCommand,-2,
     "write fast @keyspace",
     0,NULL,1,-1,1,0,0,0},

    {"exists",existsCommand,-2,
     "read-only fast @keyspace",
     0,NULL,1,-1,1,0,0,0},

    {"setbit",setbitCommand,4,
     "write use-memory @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"getbit",getbitCommand,3,
     "read-only fast @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"bitfield",bitfieldCommand,-2,
     "write use-memory @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"bitfield_ro",bitfieldroCommand,-2,
     "read-only fast @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"setrange",setrangeCommand,4,
     "write use-memory @string",
     0,NULL,1,1,1,0,0,0},

    {"getrange",getrangeCommand,4,
     "read-only @string",
     0,NULL,1,1,1,0,0,0},

    {"substr",getrangeCommand,4,
     "read-only @string",
     0,NULL,1,1,1,0,0,0},

    {"incr",incrCommand,2,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"decr",decrCommand,2,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"mget",mgetCommand,-2,
     "read-only fast @string",
     0,NULL,1,-1,1,0,0,0},

    {"rpush",rpushCommand,-3,
     "write use-memory fast @list",
     0,NULL,1,1,1,0,0,0},

    {"lpush",lpushCommand,-3,
     "write use-memory fast @list",
     0,NULL,1,1,1,0,0,0},

    {"rpushx",rpushxCommand,-3,
     "write use-memory fast @list",
     0,NULL,1,1,1,0,0,0},

    {"lpushx",lpushxCommand,-3,
     "write use-memory fast @list",
     0,NULL,1,1,1,0,0,0},

    {"linsert",linsertCommand,5,
     "write use-memory @list",
     0,NULL,1,1,1,0,0,0},

    {"rpop",rpopCommand,-2,
     "write fast @list",
     0,NULL,1,1,1,0,0,0},

    {"lpop",lpopCommand,-2,
     "write fast @list",
     0,NULL,1,1,1,0,0,0},

    {"brpop",brpopCommand,-3,
     "write no-script @list @blocking",
     0,NULL,1,-2,1,0,0,0},

    {"brpoplpush",brpoplpushCommand,4,
     "write use-memory no-script @list @blocking",
     0,NULL,1,2,1,0,0,0},

    {"blmove",blmoveCommand,6,
     "write use-memory no-script @list @blocking",
     0,NULL,1,2,1,0,0,0},

    {"blpop",blpopCommand,-3,
     "write no-script @list @blocking",
     0,NULL,1,-2,1,0,0,0},

    {"llen",llenCommand,2,
     "read-only fast @list",
     0,NULL,1,1,1,0,0,0},

    {"lindex",lindexCommand,3,
     "read-only @list",
     0,NULL,1,1,1,0,0,0},

    {"lset",lsetCommand,4,
     "write use-memory @list",
     0,NULL,1,1,1,0,0,0},

    {"lrange",lrangeCommand,4,
     "read-only @list",
     0,NULL,1,1,1,0,0,0},

    {"ltrim",ltrimCommand,4,
     "write @list",
     0,NULL,1,1,1,0,0,0},

    {"lpos",lposCommand,-3,
     "read-only @list",
     0,NULL,1,1,1,0,0,0},

    {"lrem",lremCommand,4,
     "write @list",
     0,NULL,1,1,1,0,0,0},

    {"rpoplpush",rpoplpushCommand,3,
     "write use-memory @list",
     0,NULL,1,2,1,0,0,0},

    {"lmove",lmoveCommand,5,
     "write use-memory @list",
     0,NULL,1,2,1,0,0,0},

    {"sadd",saddCommand,-3,
     "write use-memory fast @set",
     0,NULL,1,1,1,0,0,0},

    {"srem",sremCommand,-3,
     "write fast @set",
     0,NULL,1,1,1,0,0,0},

    {"smove",smoveCommand,4,
     "write fast @set",
     0,NULL,1,2,1,0,0,0},

    {"sismember",sismemberCommand,3,
     "read-only fast @set",
     0,NULL,1,1,1,0,0,0},

    {"smismember",smismemberCommand,-3,
     "read-only fast @set",
     0,NULL,1,1,1,0,0,0},

    {"scard",scardCommand,2,
     "read-only fast @set",
     0,NULL,1,1,1,0,0,0},

    {"spop",spopCommand,-2,
     "write random fast @set",
     0,NULL,1,1,1,0,0,0},

    {"srandmember",srandmemberCommand,-2,
     "read-only random @set",
     0,NULL,1,1,1,0,0,0},

    {"sinter",sinterCommand,-2,
     "read-only to-sort @set",
     0,NULL,1,-1,1,0,0,0},

    {"sinterstore",sinterstoreCommand,-3,
     "write use-memory @set",
     0,NULL,1,-1,1,0,0,0},

    {"sunion",sunionCommand,-2,
     "read-only to-sort @set",
     0,NULL,1,-1,1,0,0,0},

    {"sunionstore",sunionstoreCommand,-3,
     "write use-memory @set",
     0,NULL,1,-1,1,0,0,0},

    {"sdiff",sdiffCommand,-2,
     "read-only to-sort @set",
     0,NULL,1,-1,1,0,0,0},

    {"sdiffstore",sdiffstoreCommand,-3,
     "write use-memory @set",
     0,NULL,1,-1,1,0,0,0},

    {"smembers",sinterCommand,2,
     "read-only to-sort @set",
     0,NULL,1,1,1,0,0,0},

    {"sscan",sscanCommand,-3,
     "read-only random @set",
     0,NULL,1,1,1,0,0,0},

    {"zadd",zaddCommand,-4,
     "write use-memory fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zincrby",zincrbyCommand,4,
     "write use-memory fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrem",zremCommand,-3,
     "write fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zremrangebyscore",zremrangebyscoreCommand,4,
     "write @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zremrangebyrank",zremrangebyrankCommand,4,
     "write @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zremrangebylex",zremrangebylexCommand,4,
     "write @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zunionstore",zunionstoreCommand,-4,
     "write use-memory @sortedset",
     0,zunionInterDiffStoreGetKeys,1,1,1,0,0,0},

    {"zinterstore",zinterstoreCommand,-4,
     "write use-memory @sortedset",
     0,zunionInterDiffStoreGetKeys,1,1,1,0,0,0},

    {"zdiffstore",zdiffstoreCommand,-4,
     "write use-memory @sortedset",
     0,zunionInterDiffStoreGetKeys,1,1,1,0,0,0},

    {"zunion",zunionCommand,-3,
     "read-only @sortedset",
     0,zunionInterDiffGetKeys,0,0,0,0,0,0},

    {"zinter",zinterCommand,-3,
     "read-only @sortedset",
     0,zunionInterDiffGetKeys,0,0,0,0,0,0},

    {"zdiff",zdiffCommand,-3,
     "read-only @sortedset",
     0,zunionInterDiffGetKeys,0,0,0,0,0,0},

    {"zrange",zrangeCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrangestore",zrangestoreCommand,-5,
     "write use-memory @sortedset",
     0,NULL,1,2,1,0,0,0},

    {"zrangebyscore",zrangebyscoreCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrevrangebyscore",zrevrangebyscoreCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrangebylex",zrangebylexCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrevrangebylex",zrevrangebylexCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zcount",zcountCommand,4,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zlexcount",zlexcountCommand,4,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrevrange",zrevrangeCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zcard",zcardCommand,2,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zscore",zscoreCommand,3,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zmscore",zmscoreCommand,-3,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrank",zrankCommand,3,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrevrank",zrevrankCommand,3,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zscan",zscanCommand,-3,
     "read-only random @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zpopmin",zpopminCommand,-2,
     "write fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zpopmax",zpopmaxCommand,-2,
     "write fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"bzpopmin",bzpopminCommand,-3,
     "write no-script fast @sortedset @blocking",
     0,NULL,1,-2,1,0,0,0},

    {"bzpopmax",bzpopmaxCommand,-3,
     "write no-script fast @sortedset @blocking",
     0,NULL,1,-2,1,0,0,0},

    {"zrandmember",zrandmemberCommand,-2,
     "read-only random @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"hset",hsetCommand,-4,
     "write use-memory fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hsetnx",hsetnxCommand,4,
     "write use-memory fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hget",hgetCommand,3,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hmset",hsetCommand,-4,
     "write use-memory fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hmget",hmgetCommand,-3,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hincrby",hincrbyCommand,4,
     "write use-memory fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hincrbyfloat",hincrbyfloatCommand,4,
     "write use-memory fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hdel",hdelCommand,-3,
     "write fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hlen",hlenCommand,2,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hstrlen",hstrlenCommand,3,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hkeys",hkeysCommand,2,
     "read-only to-sort @hash",
     0,NULL,1,1,1,0,0,0},

    {"hvals",hvalsCommand,2,
     "read-only to-sort @hash",
     0,NULL,1,1,1,0,0,0},

    {"hgetall",hgetallCommand,2,
     "read-only random @hash",
     0,NULL,1,1,1,0,0,0},

    {"hexists",hexistsCommand,3,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hrandfield",hrandfieldCommand,-2,
     "read-only random @hash",
     0,NULL,1,1,1,0,0,0},

    {"hscan",hscanCommand,-3,
     "read-only random @hash",
     0,NULL,1,1,1,0,0,0},

    {"incrby",incrbyCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"decrby",decrbyCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"incrbyfloat",incrbyfloatCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"getset",getsetCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"mset",msetCommand,-3,
     "write use-memory @string",
     0,NULL,1,-1,2,0,0,0},

    {"msetnx",msetnxCommand,-3,
     "write use-memory @string",
     0,NULL,1,-1,2,0,0,0},

    {"randomkey",randomkeyCommand,1,
     "read-only random @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"select",selectCommand,2,
     "ok-loading fast ok-stale @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"swapdb",swapdbCommand,3,
     "write fast @keyspace @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"move",moveCommand,3,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"copy",copyCommand,-3,
     "write use-memory @keyspace",
     0,NULL,1,2,1,0,0,0},

    /* Like for SET, we can't mark rename as a fast command because
     * overwriting the target key may result in an implicit slow DEL. */
    {"rename",renameCommand,3,
     "write @keyspace",
     0,NULL,1,2,1,0,0,0},

    {"renamenx",renamenxCommand,3,
     "write fast @keyspace",
     0,NULL,1,2,1,0,0,0},

    {"expire",expireCommand,3,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"expireat",expireatCommand,3,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"pexpire",pexpireCommand,3,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"pexpireat",pexpireatCommand,3,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"keys",keysCommand,2,
     "read-only to-sort @keyspace @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"scan",scanCommand,-2,
     "read-only random @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"dbsize",dbsizeCommand,1,
     "read-only fast @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"auth",authCommand,-2,
     "no-auth no-script ok-loading ok-stale fast @connection",
     0,NULL,0,0,0,0,0,0},

    /* We don't allow PING during loading since in Redis PING is used as
     * failure detection, and a loading server is considered to be
     * not available. */
    {"ping",pingCommand,-1,
     "ok-stale fast @connection",
     0,NULL,0,0,0,0,0,0},

    {"echo",echoCommand,2,
     "fast @connection",
     0,NULL,0,0,0,0,0,0},

    {"save",saveCommand,1,
     "admin no-script",
     0,NULL,0,0,0,0,0,0},

    {"bgsave",bgsaveCommand,-1,
     "admin no-script",
     0,NULL,0,0,0,0,0,0},

    {"bgrewriteaof",bgrewriteaofCommand,1,
     "admin no-script",
     0,NULL,0,0,0,0,0,0},

    {"shutdown",shutdownCommand,-1,
     "admin no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"lastsave",lastsaveCommand,1,
     "random fast ok-loading ok-stale @admin @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"type",typeCommand,2,
     "read-only fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"multi",multiCommand,1,
     "no-script fast ok-loading ok-stale @transaction",
     0,NULL,0,0,0,0,0,0},

    {"exec",execCommand,1,
     "no-script no-slowlog ok-loading ok-stale @transaction",
     0,NULL,0,0,0,0,0,0},

    {"discard",discardCommand,1,
     "no-script fast ok-loading ok-stale @transaction",
     0,NULL,0,0,0,0,0,0},

    {"sync",syncCommand,1,
     "admin no-script",
     0,NULL,0,0,0,0,0,0},

    {"psync",syncCommand,-3,
     "admin no-script",
     0,NULL,0,0,0,0,0,0},

    {"replconf",replconfCommand,-1,
     "admin no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"flushdb",flushdbCommand,-1,
     "write @keyspace @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"flushall",flushallCommand,-1,
     "write @keyspace @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"sort",sortCommand,-2,
     "write use-memory @list @set @sortedset @dangerous",
     0,sortGetKeys,1,1,1,0,0,0},

    {"info",infoCommand,-1,
     "ok-loading ok-stale random @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"monitor",monitorCommand,1,
     "admin no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"ttl",ttlCommand,2,
     "read-only fast random @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"touch",touchCommand,-2,
     "read-only fast @keyspace",
     0,NULL,1,-1,1,0,0,0},

    {"pttl",pttlCommand,2,
     "read-only fast random @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"expiretime",expiretimeCommand,2,
     "read-only fast random @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"pexpiretime",pexpiretimeCommand,2,
     "read-only fast random @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"persist",persistCommand,2,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"slaveof",replicaofCommand,3,
     "admin no-script ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"replicaof",replicaofCommand,3,
     "admin no-script ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"role",roleCommand,1,
     "ok-loading ok-stale no-script fast @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"debug",debugCommand,-2,
     "admin no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"config",configCommand,-2,
     "admin ok-loading ok-stale no-script",
     0,NULL,0,0,0,0,0,0},

    {"subscribe",subscribeCommand,-2,
     "pub-sub no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"unsubscribe",unsubscribeCommand,-1,
     "pub-sub no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"psubscribe",psubscribeCommand,-2,
     "pub-sub no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"punsubscribe",punsubscribeCommand,-1,
     "pub-sub no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"publish",publishCommand,3,
     "pub-sub ok-loading ok-stale fast may-replicate",
     0,NULL,0,0,0,0,0,0},

    {"pubsub",pubsubCommand,-2,
     "pub-sub ok-loading ok-stale random",
     0,NULL,0,0,0,0,0,0},

    {"watch",watchCommand,-2,
     "no-script fast ok-loading ok-stale @transaction",
     0,NULL,1,-1,1,0,0,0},

    {"unwatch",unwatchCommand,1,
     "no-script fast ok-loading ok-stale @transaction",
     0,NULL,0,0,0,0,0,0},

    {"cluster",clusterCommand,-2,
     "admin ok-stale random",
     0,NULL,0,0,0,0,0,0},

    {"restore",restoreCommand,-4,
     "write use-memory @keyspace @dangerous",
     0,NULL,1,1,1,0,0,0},

    {"restore-asking",restoreCommand,-4,
    "write use-memory cluster-asking @keyspace @dangerous",
    0,NULL,1,1,1,0,0,0},

    {"migrate",migrateCommand,-6,
     "write random @keyspace @dangerous",
     0,migrateGetKeys,0,0,0,0,0,0},

    {"asking",askingCommand,1,
     "fast @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"readonly",readonlyCommand,1,
     "fast @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"readwrite",readwriteCommand,1,
     "fast @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"dump",dumpCommand,2,
     "read-only random @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"object",objectCommand,-2,
     "read-only random @keyspace",
     0,NULL,2,2,1,0,0,0},

    {"memory",memoryCommand,-2,
     "random read-only",
     0,memoryGetKeys,0,0,0,0,0,0},

    {"client",clientCommand,-2,
     "admin no-script random ok-loading ok-stale @connection",
     0,NULL,0,0,0,0,0,0},

    {"hello",helloCommand,-1,
     "no-auth no-script fast ok-loading ok-stale @connection",
     0,NULL,0,0,0,0,0,0},

    /* EVAL can modify the dataset, however it is not flagged as a write
     * command since we do the check while running commands from Lua.
     * 
     * EVAL and EVALSHA also feed monitors before the commands are executed,
     * as opposed to after.
      */
    {"eval",evalCommand,-3,
     "no-script no-monitor may-replicate @scripting",
     0,evalGetKeys,0,0,0,0,0,0},

    {"eval_ro",evalRoCommand,-3,
     "no-script no-monitor @scripting",
     0,evalGetKeys,0,0,0,0,0,0},

    {"evalsha",evalShaCommand,-3,
     "no-script no-monitor may-replicate @scripting",
     0,evalGetKeys,0,0,0,0,0,0},

    {"evalsha_ro",evalShaRoCommand,-3,
     "no-script no-monitor @scripting",
     0,evalGetKeys,0,0,0,0,0,0},

    {"slowlog",slowlogCommand,-2,
     "admin random ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"script",scriptCommand,-2,
     "no-script may-replicate @scripting",
     0,NULL,0,0,0,0,0,0},

    {"time",timeCommand,1,
     "random fast ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"bitop",bitopCommand,-4,
     "write use-memory @bitmap",
     0,NULL,2,-1,1,0,0,0},

    {"bitcount",bitcountCommand,-2,
     "read-only @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"bitpos",bitposCommand,-3,
     "read-only @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"wait",waitCommand,3,
     "no-script @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"command",commandCommand,-1,
     "ok-loading ok-stale random @connection",
     0,NULL,0,0,0,0,0,0},

    {"geoadd",geoaddCommand,-5,
     "write use-memory @geo",
     0,NULL,1,1,1,0,0,0},

    /* GEORADIUS has store options that may write. */
    {"georadius",georadiusCommand,-6,
     "write use-memory @geo",
     0,georadiusGetKeys,1,1,1,0,0,0},

    {"georadius_ro",georadiusroCommand,-6,
     "read-only @geo",
     0,NULL,1,1,1,0,0,0},

    {"georadiusbymember",georadiusbymemberCommand,-5,
     "write use-memory @geo",
     0,georadiusGetKeys,1,1,1,0,0,0},

    {"georadiusbymember_ro",georadiusbymemberroCommand,-5,
     "read-only @geo",
     0,NULL,1,1,1,0,0,0},

    {"geohash",geohashCommand,-2,
     "read-only @geo",
     0,NULL,1,1,1,0,0,0},

    {"geopos",geoposCommand,-2,
     "read-only @geo",
     0,NULL,1,1,1,0,0,0},

    {"geodist",geodistCommand,-4,
     "read-only @geo",
     0,NULL,1,1,1,0,0,0},

    {"geosearch",geosearchCommand,-7,
     "read-only @geo",
      0,NULL,1,1,1,0,0,0},

    {"geosearchstore",geosearchstoreCommand,-8,
     "write use-memory @geo",
      0,NULL,1,2,1,0,0,0},

    {"pfselftest",pfselftestCommand,1,
     "admin @hyperloglog",
      0,NULL,0,0,0,0,0,0},

    {"pfadd",pfaddCommand,-2,
     "write use-memory fast @hyperloglog",
     0,NULL,1,1,1,0,0,0},

    /* Technically speaking PFCOUNT may change the key since it changes the
     * final bytes in the HyperLogLog representation. However in this case
     * we claim that the representation, even if accessible, is an internal
     * affair, and the command is semantically read only. */
    {"pfcount",pfcountCommand,-2,
     "read-only may-replicate @hyperloglog",
     0,NULL,1,-1,1,0,0,0},

    {"pfmerge",pfmergeCommand,-2,
     "write use-memory @hyperloglog",
     0,NULL,1,-1,1,0,0,0},

    /* Unlike PFCOUNT that is considered as a read-only command (although
     * it changes a bit), PFDEBUG may change the entire key when converting
     * from sparse to dense representation */
    {"pfdebug",pfdebugCommand,-3,
     "admin write use-memory @hyperloglog",
     0,NULL,2,2,1,0,0,0},

    {"xadd",xaddCommand,-5,
     "write use-memory fast random @stream",
     0,NULL,1,1,1,0,0,0},

    {"xrange",xrangeCommand,-4,
     "read-only @stream",
     0,NULL,1,1,1,0,0,0},

    {"xrevrange",xrevrangeCommand,-4,
     "read-only @stream",
     0,NULL,1,1,1,0,0,0},

    {"xlen",xlenCommand,2,
     "read-only fast @stream",
     0,NULL,1,1,1,0,0,0},

    {"xread",xreadCommand,-4,
     "read-only @stream @blocking",
     0,xreadGetKeys,0,0,0,0,0,0},

    {"xreadgroup",xreadCommand,-7,
     "write @stream @blocking",
     0,xreadGetKeys,0,0,0,0,0,0},

    {"xgroup",xgroupCommand,-2,
     "write use-memory @stream",
     0,NULL,2,2,1,0,0,0},

    {"xsetid",xsetidCommand,3,
     "write use-memory fast @stream",
     0,NULL,1,1,1,0,0,0},

    {"xack",xackCommand,-4,
     "write fast random @stream",
     0,NULL,1,1,1,0,0,0},

    {"xpending",xpendingCommand,-3,
     "read-only random @stream",
     0,NULL,1,1,1,0,0,0},

    {"xclaim",xclaimCommand,-6,
     "write random fast @stream",
     0,NULL,1,1,1,0,0,0},

    {"xautoclaim",xautoclaimCommand,-6,
     "write random fast @stream",
     0,NULL,1,1,1,0,0,0},

    {"xinfo",xinfoCommand,-2,
     "read-only random @stream",
     0,NULL,2,2,1,0,0,0},

    {"xdel",xdelCommand,-3,
     "write fast @stream",
     0,NULL,1,1,1,0,0,0},

    {"xtrim",xtrimCommand,-4,
     "write random @stream",
     0,NULL,1,1,1,0,0,0},

    {"post",securityWarningCommand,-1,
     "ok-loading ok-stale read-only",
     0,NULL,0,0,0,0,0,0},

    {"host:",securityWarningCommand,-1,
     "ok-loading ok-stale read-only",
     0,NULL,0,0,0,0,0,0},

    {"latency",latencyCommand,-2,
     "admin no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"lolwut",lolwutCommand,-1,
     "read-only fast",
     0,NULL,0,0,0,0,0,0},

    {"acl",aclCommand,-2,
     "admin no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"stralgo",stralgoCommand,-2,
     "read-only @string",
     0,lcsGetKeys,0,0,0,0,0,0},

    {"reset",resetCommand,1,
     "no-script ok-stale ok-loading fast @connection",
     0,NULL,0,0,0,0,0,0},

    {"failover",failoverCommand,-1,
     "admin no-script ok-stale",
     0,NULL,0,0,0,0,0,0}
};

/*============================ Utility functions ============================ */

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
        pid_t pid = getpid();

        gettimeofday(&tv,NULL);
        struct tm tm;
        nolocks_localtime(&tm,tv.tv_sec,server.timezone,server.daylight_active);
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

    if (val == NULL) return; /* Lazy freeing will set value to NULL. */
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

uint64_t dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}

uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

uint64_t dictSdsCaseHash(const void *key) {
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

    /* Due to OBJ_STATIC_REFCOUNT, we avoid calling getDecodedObject() without
     * good reasons, because it would incrRefCount() the object, which
     * is invalid. So we check to make sure dictFind() works with static
     * objects as well. */
    if (o1->refcount != OBJ_STATIC_REFCOUNT) o1 = getDecodedObject(o1);
    if (o2->refcount != OBJ_STATIC_REFCOUNT) o2 = getDecodedObject(o2);
    cmp = dictSdsKeyCompare(privdata,o1->ptr,o2->ptr);
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
int dictExpandAllowed(size_t moreMem, double usedRatio) {
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
    NULL                       /* val destructor */
};

/* Sorted sets hash (note: a skiplist is used in addition to the hash table) */
dictType zsetDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    NULL,                      /* Note: SDS string shared & freed by skiplist */
    NULL,                      /* val destructor */
    NULL                       /* allow to expand */
};

/* Db->dict, keys are sds strings, vals are Redis objects. */
dictType dbDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictObjectDestructor,       /* val destructor */
    dictExpandAllowed           /* allow to expand */
};

/* server.lua_scripts sha (as sds string) -> scripts (as robj) cache. */
dictType shaScriptObjectDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictObjectDestructor,       /* val destructor */
    NULL                        /* allow to expand */
};

/* Db->expires */
dictType dbExpiresDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    NULL,                       /* key destructor */
    NULL,                       /* val destructor */
    dictExpandAllowed           /* allow to expand */
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

/* Hash type hash table (note that small hashes are represented with ziplists) */
dictType hashDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictSdsDestructor,          /* val destructor */
    NULL                        /* allow to expand */
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

/* Replication cached script dict (server.repl_scriptcache_dict).
 * Keys are sds SHA1 strings, while values are not used at all in the current
 * implementation. */
dictType replScriptCacheDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL,                       /* val destructor */
    NULL                        /* allow to expand */
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
 * of CPU time at every call of this function to perform some rehashing.
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
 * for dict.c to resize the hash tables accordingly to the fact we have an
 * active fork child running. */
void updateDictResizePolicy(void) {
    if (!hasActiveChildProcess())
        dictEnableResize();
    else
        dictDisableResize();
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
int hasActiveChildProcess() {
    return server.child_pid != -1;
}

void resetChildState() {
    server.child_type = CHILD_TYPE_NONE;
    server.child_pid = -1;
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

/* Return if child type is mutual exclusive with other fork children */
int isMutuallyExclusiveChildType(int type) {
    return type == CHILD_TYPE_RDB || type == CHILD_TYPE_AOF || type == CHILD_TYPE_MODULE;
}

/* Return true if this instance has persistence completely turned off:
 * both RDB and AOF are disabled. */
int allPersistenceDisabled(void) {
    return server.saveparamslen == 0 && server.aof_state == AOF_OFF;
}

/* ======================= Cron: called every 100 ms ======================== */

/* Add a sample to the operations per second array of samples. */
void trackInstantaneousMetric(int metric, long long current_reading) {
    long long now = mstime();
    long long t = now - server.inst_metric[metric].last_sample_time;
    long long ops = current_reading -
                    server.inst_metric[metric].last_sample_count;
    long long ops_sec;

    ops_sec = t > 0 ? (ops*1000/t) : 0;

    server.inst_metric[metric].samples[server.inst_metric[metric].idx] =
        ops_sec;
    server.inst_metric[metric].idx++;
    server.inst_metric[metric].idx %= STATS_METRIC_SAMPLES;
    server.inst_metric[metric].last_sample_time = now;
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

/* The client query buffer is an sds.c string that can end with a lot of
 * free space not used, this function reclaims space if needed.
 *
 * The function always returns 0 as it never terminates the client. */
int clientsCronResizeQueryBuffer(client *c) {
    size_t querybuf_size = sdsalloc(c->querybuf);
    time_t idletime = server.unixtime - c->lastinteraction;

    /* Only resize the query buffer if the buffer is bigger than
     * PROTO_RESIZE_THRESHOLD, and it is actually wasting at least a few kbytes. */
    if (querybuf_size > PROTO_RESIZE_THRESHOLD && sdsavail(c->querybuf) > 1024*4) {
        /* There are two conditions to resize the query buffer: */
        if (idletime > 2) {
            /* 1) Query is idle for a long time. */
            c->querybuf = sdsRemoveFreeSpace(c->querybuf);
        } else if (querybuf_size/2 > c->querybuf_peak) {
            /* 2) Query buffer is too big for latest peak. trim excess space but
             *    only up to a limit, not below the recent peak and current
             *    c->querybuf (which will be soon get used). */
            size_t resize = sdslen(c->querybuf);
            if (resize < c->querybuf_peak) resize = c->querybuf_peak;
            if (c->bulklen != -1 && resize < (size_t)c->bulklen) resize = c->bulklen;
            c->querybuf = sdsResize(c->querybuf, resize);
        }
    }

    /* Reset the peak again to capture the peak memory usage in the next
     * cycle. */
    c->querybuf_peak = sdslen(c->querybuf);
    /* We reset to either the current used, or currently processed bulk size,
     * which ever is bigger. */
    if (c->bulklen != -1 && (size_t)c->bulklen > c->querybuf_peak)
        c->querybuf_peak = c->bulklen;

    /* Clients representing masters also use a "pending query buffer" that
     * is the yet not applied part of the stream we are reading. Such buffer
     * also needs resizing from time to time, otherwise after a very large
     * transfer (a huge value or a big MIGRATE operation) it will keep using
     * a lot of memory. */
    if (c->flags & CLIENT_MASTER) {
        /* There are two conditions to resize the pending query buffer:
         * 1) Pending Query buffer is > LIMIT_PENDING_QUERYBUF.
         * 2) Used length is smaller than pending_querybuf_size/2 */
        size_t pending_querybuf_size = sdsAllocSize(c->pending_querybuf);
        if(pending_querybuf_size > LIMIT_PENDING_QUERYBUF &&
           sdslen(c->pending_querybuf) < (pending_querybuf_size/2))
        {
            c->pending_querybuf = sdsRemoveFreeSpace(c->pending_querybuf);
        }
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
 * saw in that slot. Every slot correspond to one of the latest seconds, since
 * the array is indexed by doing UNIXTIME % CLIENTS_PEAK_MEM_USAGE_SLOTS.
 *
 * When we want to know what was recently the peak memory usage, we just scan
 * such few slots searching for the maximum value. */
#define CLIENTS_PEAK_MEM_USAGE_SLOTS 8
size_t ClientsPeakMemInput[CLIENTS_PEAK_MEM_USAGE_SLOTS] = {0};
size_t ClientsPeakMemOutput[CLIENTS_PEAK_MEM_USAGE_SLOTS] = {0};

int clientsCronTrackExpansiveClients(client *c, int time_idx) {
    size_t in_usage = sdsZmallocSize(c->querybuf) + c->argv_len_sum +
	              (c->argv ? zmalloc_size(c->argv) : 0);
    size_t out_usage = getClientOutputBufferMemoryUsage(c);

    /* Track the biggest values observed so far in this slot. */
    if (in_usage > ClientsPeakMemInput[time_idx]) ClientsPeakMemInput[time_idx] = in_usage;
    if (out_usage > ClientsPeakMemOutput[time_idx]) ClientsPeakMemOutput[time_idx] = out_usage;

    return 0; /* This function never terminates the client. */
}

/* Iterating all the clients in getMemoryOverheadData() is too slow and
 * in turn would make the INFO command too slow. So we perform this
 * computation incrementally and track the (not instantaneous but updated
 * to the second) total memory used by clients using clientsCron() in
 * a more incremental way (depending on server.hz). */
int clientsCronTrackClientsMemUsage(client *c) {
    size_t mem = 0;
    int type = getClientType(c);
    mem += getClientOutputBufferMemoryUsage(c);
    mem += sdsZmallocSize(c->querybuf);
    mem += zmalloc_size(c);
    mem += c->argv_len_sum;
    if (c->argv) mem += zmalloc_size(c->argv);
    /* Now that we have the memory used by the client, remove the old
     * value from the old category, and add it back. */
    server.stat_clients_type_memory[c->client_cron_last_memory_type] -=
        c->client_cron_last_memory_usage;
    server.stat_clients_type_memory[type] += mem;
    /* Remember what we added and where, to remove it next time. */
    c->client_cron_last_memory_usage = mem;
    c->client_cron_last_memory_type = type;
    return 0;
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

        /* Rotate the list, take the current head, process.
         * This way if the client must be removed from the list it's the
         * first element and we don't incur into O(N) computation. */
        listRotateTailToHead(server.clients);
        head = listFirst(server.clients);
        c = listNodeValue(head);
        /* The following functions do different service checks on the client.
         * The protocol is that they return non-zero if the client was
         * terminated. */
        if (clientsCronHandleTimeout(c,now)) continue;
        if (clientsCronResizeQueryBuffer(c)) continue;
        if (clientsCronTrackExpansiveClients(c, curr_peak_mem_usage_slot)) continue;
        if (clientsCronTrackClientsMemUsage(c)) continue;
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

        /* Resize */
        for (j = 0; j < dbs_per_call; j++) {
            tryResizeHashTables(resize_db % server.dbnum);
            resize_db++;
        }

        /* Rehash */
        if (server.activerehashing) {
            for (j = 0; j < dbs_per_call; j++) {
                int work_done = incrementallyRehash(rehash_db);
                if (work_done) {
                    /* If the function did some work, stop here, we'll do
                     * more at the next cron loop. */
                    break;
                } else {
                    /* If this db didn't need rehash, we'll try the next one. */
                    rehash_db++;
                    rehash_db %= server.dbnum;
                }
            }
        }
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
    server.ustime = ustime();
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
        server.daylight_active = tm.tm_isdst;
    }
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
void cronUpdateMemoryStats() {
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
        zmalloc_get_allocator_info(&server.cron_malloc_stats.allocator_allocated,
                                   &server.cron_malloc_stats.allocator_active,
                                   &server.cron_malloc_stats.allocator_resident);
        /* in case the allocator isn't providing these stats, fake them so that
         * fragmentation info still shows some (inaccurate metrics) */
        if (!server.cron_malloc_stats.allocator_resident) {
            /* LUA memory isn't part of zmalloc_used, but it is part of the process RSS,
             * so we must deduct it in order to be able to calculate correct
             * "allocator fragmentation" ratio */
            size_t lua_memory = lua_gc(server.lua,LUA_GCCOUNT,0)*1024LL;
            server.cron_malloc_stats.allocator_resident = server.cron_malloc_stats.process_rss - lua_memory;
        }
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

    /* Update the time cache. */
    updateCachedTime(1);

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

    run_with_period(100) {
        long long stat_net_input_bytes, stat_net_output_bytes;
        atomicGet(server.stat_net_input_bytes, stat_net_input_bytes);
        atomicGet(server.stat_net_output_bytes, stat_net_output_bytes);

        trackInstantaneousMetric(STATS_METRIC_COMMAND,server.stat_numcommands);
        trackInstantaneousMetric(STATS_METRIC_NET_INPUT,
                stat_net_input_bytes);
        trackInstantaneousMetric(STATS_METRIC_NET_OUTPUT,
                stat_net_output_bytes);
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
    unsigned int lruclock = getLRUClock();
    atomicSet(server.lruclock,lruclock);

    cronUpdateMemoryStats();

    /* We received a SIGTERM, shutting down here in a safe way, as it is
     * not ok doing so inside the signal handler. */
    if (server.shutdown_asap) {
        if (prepareForShutdown(SHUTDOWN_NOFLAGS) == C_OK) exit(0);
        serverLog(LL_WARNING,"SIGTERM received but errors trying to shut down the server, check the logs for more information");
        server.shutdown_asap = 0;
    }

    /* Show some info about non-empty databases */
    if (server.verbosity <= LL_VERBOSE) {
        run_with_period(5000) {
            for (j = 0; j < server.dbnum; j++) {
                long long size, used, vkeys;

                size = dictSlots(server.db[j].dict);
                used = dictSize(server.db[j].dict);
                vkeys = dictSize(server.db[j].expires);
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
        server.aof_rewrite_scheduled)
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
                rdbSaveBackground(server.rdb_filename,rsiptr);
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
            if (growth >= server.aof_rewrite_perc) {
                serverLog(LL_NOTICE,"Starting automatic rewriting of AOF on %lld%% growth",growth);
                rewriteAppendOnlyFileBackground();
            }
        }
    }
    /* Just for the sake of defensive programming, to avoid forgetting to
     * call this function when need. */
    updateDictResizePolicy();


    /* AOF postponed flush: Try at every cron cycle if the slow fsync
     * completed. */
    if (server.aof_state == AOF_ON && server.aof_flush_postponed_start)
        flushAppendOnlyFile(0);

    /* AOF write errors: in this case we have a buffer to flush as well and
     * clear the AOF error in case of success to make the DB writable again,
     * however to try every second is enough in case of 'hz' is set to
     * a higher frequency. */
    run_with_period(1000) {
        if (server.aof_state == AOF_ON && server.aof_last_write_status == C_ERR)
            flushAppendOnlyFile(0);
    }

    /* Clear the paused clients state if needed. */
    checkClientPauseTimeoutAndReturnIfPaused();

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
        if (rdbSaveBackground(server.rdb_filename,rsiptr) == C_OK)
            server.rdb_bgsave_scheduled = 0;
    }

    /* Fire the cron loop modules event. */
    RedisModuleCronLoopV1 ei = {REDISMODULE_CRON_LOOP_VERSION,server.hz};
    moduleFireServerEvent(REDISMODULE_EVENT_CRON_LOOP,
                          0,
                          &ei);

    server.cronloops++;
    return 1000/server.hz;
}


void blockingOperationStarts() {
    if(!server.blocking_op_nesting++){
        updateCachedTime(0);
        server.blocked_last_cron = server.mstime;
    }
}

void blockingOperationEnds() {
    if(!(--server.blocking_op_nesting)){
        server.blocked_last_cron = 0;
    }
}

/* This function fill in the role of serverCron during RDB or AOF loading, and
 * also during blocked scripts.
 * It attempts to do its duties at a similar rate as the configured server.hz,
 * and updates cronloops variable so that similarly to serverCron, the
 * run_with_period can be used. */
void whileBlockedCron() {
    /* Here we may want to perform some cron jobs (normally done server.hz times
     * per second). */

    /* Since this function depends on a call to blockingOperationStarts, let's
     * make sure it was done. */
    serverAssert(server.blocked_last_cron);

    /* In case we where called too soon, leave right away. This way one time
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
        processed += tlsProcessPendingData();
        processed += handleClientsWithPendingWrites();
        processed += freeClientsInAsyncFreeQueue();
        server.events_processed_while_blocked += processed;
        return;
    }

    /* Handle precise timeouts of blocked clients. */
    handleBlockedClientsTimeout();

    /* We should handle pending reads clients ASAP after event loop. */
    handleClientsWithPendingReadsUsingThreads();

    /* Handle TLS pending data. (must be done before flushAppendOnlyFile) */
    tlsProcessPendingData();

    /* If tls still has pending unread data don't sleep at all. */
    aeSetDontWait(server.el, tlsHasPendingData());

    /* Call the Redis Cluster before sleep function. Note that this function
     * may change the state of Redis Cluster (from ok to fail or vice versa),
     * so it's a good idea to call it before serving the unblocked clients
     * later in this function. */
    if (server.cluster_enabled) clusterBeforeSleep();

    /* Run a fast expire cycle (the called function will return
     * ASAP if a fast cycle is not needed). */
    if (server.active_expire_enabled && server.masterhost == NULL)
        activeExpireCycle(ACTIVE_EXPIRE_CYCLE_FAST);

    /* Unblock all the clients blocked for synchronous replication
     * in WAIT. */
    if (listLength(server.clients_waiting_acks))
        processClientsWaitingReplicas();

    /* Check if there are clients unblocked by modules that implement
     * blocking commands. */
    if (moduleCount()) moduleHandleBlockedClients();

    /* Try to process pending commands for clients that were just unblocked. */
    if (listLength(server.unblocked_clients))
        processUnblockedClients();

    /* Send all the slaves an ACK request if at least one client blocked
     * during the previous event loop iteration. Note that we do this after
     * processUnblockedClients(), so if there are multiple pipelined WAITs
     * and the just unblocked WAIT gets blocked again, we don't have to wait
     * a server cron cycle in absence of other event loop events. See #6623.
     * 
     * We also don't send the ACKs while clients are paused, since it can
     * increment the replication backlog, they'll be sent after the pause
     * if we are still the master. */
    if (server.get_ack_from_slaves && !checkClientPauseTimeoutAndReturnIfPaused()) {
        robj *argv[3];

        argv[0] = shared.replconf;
        argv[1] = shared.getack;
        argv[2] = shared.special_asterick; /* Not used argument. */
        replicationFeedSlaves(server.slaves, server.slaveseldb, argv, 3);
        server.get_ack_from_slaves = 0;
    }

    /* We may have received updates from clients about their current offset. NOTE:
     * this can't be done where the ACK is received since failover will disconnect 
     * our clients. */
    updateFailoverStatus();

    /* Send the invalidation messages to clients participating to the
     * client side caching protocol in broadcasting (BCAST) mode. */
    trackingBroadcastInvalidationMessages();

    /* Write the AOF buffer on disk */
    if (server.aof_state == AOF_ON)
        flushAppendOnlyFile(0);

    /* Handle writes with pending output buffers. */
    handleClientsWithPendingWritesUsingThreads();

    /* Close clients that need to be closed asynchronous */
    freeClientsInAsyncFreeQueue();

    /* Try to process blocked clients every once in while. Example: A module
     * calls RM_SignalKeyAsReady from within a timer callback (So we don't
     * visit processCommand() at all). */
    handleClientsBlockedOnKeys();

    /* Before we are going to sleep, let the threads access the dataset by
     * releasing the GIL. Redis main thread will not touch anything at this
     * time. */
    if (moduleCount()) moduleReleaseGIL();

    /* Do NOT add anything below moduleReleaseGIL !!! */
}

/* This function is called immediately after the event loop multiplexing
 * API returned, and the control is going to soon return to Redis by invoking
 * the different events callbacks. */
void afterSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);

    /* Do NOT add anything above moduleAcquireGIL !!! */

    /* Acquire the modules GIL so that their threads won't touch anything. */
    if (!ProcessingEventsWhileBlocked) {
        if (moduleCount()) moduleAcquireGIL();
    }
}

/* =========================== Server initialization ======================== */

void createSharedObjects(void) {
    int j;

    /* Shared command responses */
    shared.crlf = createObject(OBJ_STRING,sdsnew("\r\n"));
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
    shared.slowscripterr = createObject(OBJ_STRING,sdsnew(
        "-BUSY Redis is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.\r\n"));
    shared.masterdownerr = createObject(OBJ_STRING,sdsnew(
        "-MASTERDOWN Link with MASTER is down and replica-serve-stale-data is set to 'no'.\r\n"));
    shared.bgsaveerr = createObject(OBJ_STRING,sdsnew(
        "-MISCONF Redis is configured to save RDB snapshots, but it is currently not able to persist on disk. Commands that may modify the data set are disabled, because this instance is configured to report errors during writes if RDB snapshotting fails (stop-writes-on-bgsave-error option). Please check the Redis logs for details about the RDB error.\r\n"));
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

    /* Shared command argument */
    shared.left = createStringObject("left",4);
    shared.right = createStringObject("right",5);
    shared.pxat = createStringObject("PXAT", 4);
    shared.time = createStringObject("TIME",4);
    shared.retrycount = createStringObject("RETRYCOUNT",10);
    shared.force = createStringObject("FORCE",5);
    shared.justid = createStringObject("JUSTID",6);
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
        shared.integers[j]->encoding = OBJ_ENCODING_INT;
    }
    for (j = 0; j < OBJ_SHARED_BULKHDR_LEN; j++) {
        shared.mbulkhdr[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),"*%d\r\n",j));
        shared.bulkhdr[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),"$%d\r\n",j));
    }
    /* The following two shared objects, minstring and maxstring, are not
     * actually used for their value but as a special object meaning
     * respectively the minimum possible string and the maximum possible
     * string in string comparisons for the ZRANGEBYLEX command. */
    shared.minstring = sdsnew("minstring");
    shared.maxstring = sdsnew("maxstring");
}

void initServerConfig(void) {
    int j;
    char *default_bindaddr[CONFIG_DEFAULT_BINDADDR_COUNT] = CONFIG_DEFAULT_BINDADDR;

    updateCachedTime(1);
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
    server.bind_source_addr = NULL;
    server.unixsocketperm = CONFIG_DEFAULT_UNIX_SOCKET_PERM;
    server.ipfd.count = 0;
    server.tlsfd.count = 0;
    server.sofd = -1;
    server.active_expire_enabled = 1;
    server.skip_checksum_validation = 0;
    server.saveparams = NULL;
    server.loading = 0;
    server.loading_rdb_used_mem = 0;
    server.logfile = zstrdup(CONFIG_DEFAULT_LOGFILE);
    server.aof_state = AOF_OFF;
    server.aof_rewrite_base_size = 0;
    server.aof_rewrite_scheduled = 0;
    server.aof_flush_sleep = 0;
    server.aof_last_fsync = time(NULL);
    atomicSet(server.aof_bio_fsync_status,C_OK);
    server.aof_rewrite_time_last = -1;
    server.aof_rewrite_time_start = -1;
    server.aof_lastbgrewrite_status = C_OK;
    server.aof_delayed_fsync = 0;
    server.aof_fd = -1;
    server.aof_selected_db = -1; /* Make sure the first time will not match */
    server.aof_flush_postponed_start = 0;
    server.pidfile = NULL;
    server.active_defrag_running = 0;
    server.notify_keyspace_events = 0;
    server.blocked_clients = 0;
    memset(server.blocked_clients_by_type,0,
           sizeof(server.blocked_clients_by_type));
    server.shutdown_asap = 0;
    server.cluster_configfile = zstrdup(CONFIG_DEFAULT_CLUSTER_CONFIG_FILE);
    server.cluster_module_flags = CLUSTER_MODULE_FLAG_NONE;
    server.migrate_cached_sockets = dictCreate(&migrateCacheDictType,NULL);
    server.next_client_id = 1; /* Client IDs, start from 1 .*/
    server.loading_process_events_interval_bytes = (1024*1024*2);

    unsigned int lruclock = getLRUClock();
    atomicSet(server.lruclock,lruclock);
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
    server.master_initial_offset = -1;
    server.repl_state = REPL_STATE_NONE;
    server.repl_transfer_tmpfile = NULL;
    server.repl_transfer_fd = -1;
    server.repl_transfer_s = NULL;
    server.repl_syncio_timeout = CONFIG_REPL_SYNCIO_TIMEOUT;
    server.repl_down_since = 0; /* Never connected, repl is down since EVER. */
    server.master_repl_offset = 0;

    /* Replication partial resync backlog */
    server.repl_backlog = NULL;
    server.repl_backlog_histlen = 0;
    server.repl_backlog_idx = 0;
    server.repl_backlog_off = 0;
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
    server.commands = dictCreate(&commandTableDictType,NULL);
    server.orig_commands = dictCreate(&commandTableDictType,NULL);
    populateCommandTable();
    server.delCommand = lookupCommandByCString("del");
    server.multiCommand = lookupCommandByCString("multi");
    server.lpushCommand = lookupCommandByCString("lpush");
    server.lpopCommand = lookupCommandByCString("lpop");
    server.rpopCommand = lookupCommandByCString("rpop");
    server.zpopminCommand = lookupCommandByCString("zpopmin");
    server.zpopmaxCommand = lookupCommandByCString("zpopmax");
    server.sremCommand = lookupCommandByCString("srem");
    server.execCommand = lookupCommandByCString("exec");
    server.expireCommand = lookupCommandByCString("expire");
    server.pexpireCommand = lookupCommandByCString("pexpire");
    server.xclaimCommand = lookupCommandByCString("xclaim");
    server.xgroupCommand = lookupCommandByCString("xgroup");
    server.rpoplpushCommand = lookupCommandByCString("rpoplpush");
    server.lmoveCommand = lookupCommandByCString("lmove");

    /* Debugging */
    server.watchdog_period = 0;

    /* By default we want scripts to be always replicated by effects
     * (single commands executed by the script), and not by sending the
     * script to the slave / AOF. This is the new way starting from
     * Redis 5. However it is possible to revert it via redis.conf. */
    server.lua_always_replicate_commands = 1;

    initConfigValues();
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
                             "failed");
        return C_ERR;
    }

    /* Perform a proper shutdown. */
    if (flags & RESTART_SERVER_GRACEFULLY &&
        prepareForShutdown(SHUTDOWN_NOFLAGS) != C_OK)
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

static void readOOMScoreAdj(void) {
#ifdef HAVE_PROC_OOM_SCORE_ADJ
    char buf[64];
    int fd = open("/proc/self/oom_score_adj", O_RDONLY);

    if (fd < 0) return;
    if (read(fd, buf, sizeof(buf)) > 0)
        server.oom_score_adj_base = atoi(buf);
    close(fd);
#endif
}

/* This function will configure the current process's oom_score_adj according
 * to user specified configuration. This is currently implemented on Linux
 * only.
 *
 * A process_class value of -1 implies OOM_CONFIG_MASTER or OOM_CONFIG_REPLICA,
 * depending on current role.
 */
int setOOMScoreAdj(int process_class) {

    if (server.oom_score_adj == OOM_SCORE_ADJ_NO) return C_OK;
    if (process_class == -1)
        process_class = (server.masterhost ? CONFIG_OOM_REPLICA : CONFIG_OOM_MASTER);

    serverAssert(process_class >= 0 && process_class < CONFIG_OOM_COUNT);

#ifdef HAVE_PROC_OOM_SCORE_ADJ
    int fd;
    int val;
    char buf[64];

    val = server.oom_score_adj_values[process_class];
    if (server.oom_score_adj == OOM_SCORE_RELATIVE)
        val += server.oom_score_adj_base;
    if (val > 1000) val = 1000;
    if (val < -1000) val = -1000;

    snprintf(buf, sizeof(buf) - 1, "%d\n", val);

    fd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (fd < 0 || write(fd, buf, strlen(buf)) < 0) {
        serverLog(LOG_WARNING, "Unable to write oom_score_adj: %s", strerror(errno));
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
                if (bestlimit < decr_step) break;
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

void closeSocketListeners(socketFds *sfd) {
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
int createSocketAcceptHandler(socketFds *sfd, aeFileProc *accept_handler) {
    int j;

    for (j = 0; j < sfd->count; j++) {
        if (aeCreateFileEvent(server.el, sfd->fd[j], AE_READABLE, accept_handler,NULL) == AE_ERR) {
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
int listenToPort(int port, socketFds *sfd) {
    int j;
    char **bindaddr = server.bindaddr;

    /* If we have no bind address, we don't listen on a TCP socket */
    if (server.bindaddr_count == 0) return C_OK;

    for (j = 0; j < server.bindaddr_count; j++) {
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
            closeSocketListeners(sfd);
            return C_ERR;
        }
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
    server.stat_expired_stale_perc = 0;
    server.stat_expired_time_cap_reached_count = 0;
    server.stat_expire_cycle_time_used = 0;
    server.stat_evictedkeys = 0;
    server.stat_keyspace_misses = 0;
    server.stat_keyspace_hits = 0;
    server.stat_active_defrag_hits = 0;
    server.stat_active_defrag_misses = 0;
    server.stat_active_defrag_key_hits = 0;
    server.stat_active_defrag_key_misses = 0;
    server.stat_active_defrag_scanned = 0;
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
    for (j = 0; j < STATS_METRIC_COUNT; j++) {
        server.inst_metric[j].idx = 0;
        server.inst_metric[j].last_sample_time = mstime();
        server.inst_metric[j].last_sample_count = 0;
        memset(server.inst_metric[j].samples,0,
            sizeof(server.inst_metric[j].samples));
    }
    atomicSet(server.stat_net_input_bytes, 0);
    atomicSet(server.stat_net_output_bytes, 0);
    server.stat_unexpected_error_replies = 0;
    server.stat_total_error_replies = 0;
    server.stat_dump_payload_sanitizations = 0;
    server.aof_delayed_fsync = 0;
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
    makeThreadKillable();

    if (server.syslog_enabled) {
        openlog(server.syslog_ident, LOG_PID | LOG_NDELAY | LOG_NOWAIT,
            server.syslog_facility);
    }

    /* Initialization after setting defaults from the config system. */
    server.aof_state = server.aof_enabled ? AOF_ON : AOF_OFF;
    server.hz = server.config_hz;
    server.pid = getpid();
    server.in_fork_child = CHILD_TYPE_NONE;
    server.main_thread_id = pthread_self();
    server.current_client = NULL;
    server.errors = raxNew();
    server.fixed_time_expire = 0;
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
    server.clients_waiting_acks = listCreate();
    server.get_ack_from_slaves = 0;
    server.client_pause_type = 0;
    server.paused_clients = listCreate();
    server.events_processed_while_blocked = 0;
    server.system_memory_size = zmalloc_get_memory_size();
    server.blocked_last_cron = 0;
    server.blocking_op_nesting = 0;

    if ((server.tls_port || server.tls_replication || server.tls_cluster)
                && tlsConfigure(&server.tls_ctx_config) == C_ERR) {
        serverLog(LL_WARNING, "Failed to configure TLS. Check logs for more info.");
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

    /* Open the TCP listening socket for the user commands. */
    if (server.port != 0 &&
        listenToPort(server.port,&server.ipfd) == C_ERR) {
        serverLog(LL_WARNING, "Failed listening on port %u (TCP), aborting.", server.port);
        exit(1);
    }
    if (server.tls_port != 0 &&
        listenToPort(server.tls_port,&server.tlsfd) == C_ERR) {
        serverLog(LL_WARNING, "Failed listening on port %u (TLS), aborting.", server.tls_port);
        exit(1);
    }

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
        anetCloexec(server.sofd);
    }

    /* Abort if there are no listening sockets at all. */
    if (server.ipfd.count == 0 && server.tlsfd.count == 0 && server.sofd < 0) {
        serverLog(LL_WARNING, "Configured to not listen anywhere, exiting.");
        exit(1);
    }

    /* Create the Redis databases, and initialize other internal state. */
    for (j = 0; j < server.dbnum; j++) {
        server.db[j].dict = dictCreate(&dbDictType,NULL);
        server.db[j].expires = dictCreate(&dbExpiresDictType,NULL);
        server.db[j].expires_cursor = 0;
        server.db[j].blocking_keys = dictCreate(&keylistDictType,NULL);
        server.db[j].ready_keys = dictCreate(&objectKeyPointerValueDictType,NULL);
        server.db[j].watched_keys = dictCreate(&keylistDictType,NULL);
        server.db[j].id = j;
        server.db[j].avg_ttl = 0;
        server.db[j].defrag_later = listCreate();
        listSetFreeMethod(server.db[j].defrag_later,(void (*)(void*))sdsfree);
    }
    evictionPoolAlloc(); /* Initialize the LRU keys pool. */
    server.pubsub_channels = dictCreate(&keylistDictType,NULL);
    server.pubsub_patterns = dictCreate(&keylistDictType,NULL);
    server.cronloops = 0;
    server.in_eval = 0;
    server.in_exec = 0;
    server.propagate_in_transaction = 0;
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
    server.cron_malloc_stats.zmalloc_used = 0;
    server.cron_malloc_stats.process_rss = 0;
    server.cron_malloc_stats.allocator_allocated = 0;
    server.cron_malloc_stats.allocator_active = 0;
    server.cron_malloc_stats.allocator_resident = 0;
    server.lastbgsave_status = C_OK;
    server.aof_last_write_status = C_OK;
    server.aof_last_write_errno = 0;
    server.repl_good_slaves_count = 0;

    /* Create the timer callback, this is our way to process many background
     * operations incrementally, like clients timeout, eviction of unaccessed
     * expired keys and so forth. */
    if (aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL) == AE_ERR) {
        serverPanic("Can't create event loop timers.");
        exit(1);
    }

    /* Create an event handler for accepting new connections in TCP and Unix
     * domain sockets. */
    if (createSocketAcceptHandler(&server.ipfd, acceptTcpHandler) != C_OK) {
        serverPanic("Unrecoverable error creating TCP socket accept handler.");
    }
    if (createSocketAcceptHandler(&server.tlsfd, acceptTLSHandler) != C_OK) {
        serverPanic("Unrecoverable error creating TLS socket accept handler.");
    }
    if (server.sofd > 0 && aeCreateFileEvent(server.el,server.sofd,AE_READABLE,
        acceptUnixHandler,NULL) == AE_ERR) serverPanic("Unrecoverable error creating server.sofd file event.");


    /* Register a readable event for the pipe used to awake the event loop
     * when a blocked client in a module needs attention. */
    if (aeCreateFileEvent(server.el, server.module_blocked_pipe[0], AE_READABLE,
        moduleBlockedClientPipeReadable,NULL) == AE_ERR) {
            serverPanic(
                "Error registering the readable event for the module "
                "blocked clients subsystem.");
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

    if (server.cluster_enabled) clusterInit();
    replicationScriptCacheInit();
    scriptingInit(1);
    slowlogInit();
    latencyMonitorInit();
    
    /* Initialize ACL default password if it exists */
    ACLUpdateDefaultUserPassword(server.requirepass);
}

/* Some steps in server initialization need to be done last (after modules
 * are loaded).
 * Specifically, creation of threads due to a race bug in ld.so, in which
 * Thread Local Storage initialization collides with dlopen call.
 * see: https://sourceware.org/bugzilla/show_bug.cgi?id=19329 */
void InitServerLast() {
    bioInit();
    initThreadedIO();
    set_jemalloc_bg_thread(server.jemalloc_bg_thread);
    server.initial_memory_usage = zmalloc_used_memory();
}

/* Parse the flags string description 'strflags' and set them to the
 * command 'c'. If the flags are all valid C_OK is returned, otherwise
 * C_ERR is returned (yet the recognized flags are set in the command). */
int populateCommandTableParseFlags(struct redisCommand *c, char *strflags) {
    int argc;
    sds *argv;

    /* Split the line into arguments for processing. */
    argv = sdssplitargs(strflags,&argc);
    if (argv == NULL) return C_ERR;

    for (int j = 0; j < argc; j++) {
        char *flag = argv[j];
        if (!strcasecmp(flag,"write")) {
            c->flags |= CMD_WRITE|CMD_CATEGORY_WRITE;
        } else if (!strcasecmp(flag,"read-only")) {
            c->flags |= CMD_READONLY|CMD_CATEGORY_READ;
        } else if (!strcasecmp(flag,"use-memory")) {
            c->flags |= CMD_DENYOOM;
        } else if (!strcasecmp(flag,"admin")) {
            c->flags |= CMD_ADMIN|CMD_CATEGORY_ADMIN|CMD_CATEGORY_DANGEROUS;
        } else if (!strcasecmp(flag,"pub-sub")) {
            c->flags |= CMD_PUBSUB|CMD_CATEGORY_PUBSUB;
        } else if (!strcasecmp(flag,"no-script")) {
            c->flags |= CMD_NOSCRIPT;
        } else if (!strcasecmp(flag,"random")) {
            c->flags |= CMD_RANDOM;
        } else if (!strcasecmp(flag,"to-sort")) {
            c->flags |= CMD_SORT_FOR_SCRIPT;
        } else if (!strcasecmp(flag,"ok-loading")) {
            c->flags |= CMD_LOADING;
        } else if (!strcasecmp(flag,"ok-stale")) {
            c->flags |= CMD_STALE;
        } else if (!strcasecmp(flag,"no-monitor")) {
            c->flags |= CMD_SKIP_MONITOR;
        } else if (!strcasecmp(flag,"no-slowlog")) {
            c->flags |= CMD_SKIP_SLOWLOG;
        } else if (!strcasecmp(flag,"cluster-asking")) {
            c->flags |= CMD_ASKING;
        } else if (!strcasecmp(flag,"fast")) {
            c->flags |= CMD_FAST | CMD_CATEGORY_FAST;
        } else if (!strcasecmp(flag,"no-auth")) {
            c->flags |= CMD_NO_AUTH;
        } else if (!strcasecmp(flag,"may-replicate")) {
            c->flags |= CMD_MAY_REPLICATE;
        } else {
            /* Parse ACL categories here if the flag name starts with @. */
            uint64_t catflag;
            if (flag[0] == '@' &&
                (catflag = ACLGetCommandCategoryFlagByName(flag+1)) != 0)
            {
                c->flags |= catflag;
            } else {
                sdsfreesplitres(argv,argc);
                return C_ERR;
            }
        }
    }
    /* If it's not @fast is @slow in this binary world. */
    if (!(c->flags & CMD_CATEGORY_FAST)) c->flags |= CMD_CATEGORY_SLOW;

    sdsfreesplitres(argv,argc);
    return C_OK;
}

/* Populates the Redis Command Table starting from the hard coded list
 * we have on top of server.c file. */
void populateCommandTable(void) {
    int j;
    int numcommands = sizeof(redisCommandTable)/sizeof(struct redisCommand);

    for (j = 0; j < numcommands; j++) {
        struct redisCommand *c = redisCommandTable+j;
        int retval1, retval2;

        /* Translate the command string flags description into an actual
         * set of flags. */
        if (populateCommandTableParseFlags(c,c->sflags) == C_ERR)
            serverPanic("Unsupported command flag");

        c->id = ACLGetCommandID(c->name); /* Assign the ID used for ACL. */
        retval1 = dictAdd(server.commands, sdsnew(c->name), c);
        /* Populate an additional dictionary that will be unaffected
         * by rename-command statements in redis.conf. */
        retval2 = dictAdd(server.orig_commands, sdsnew(c->name), c);
        serverAssert(retval1 == DICT_OK && retval2 == DICT_OK);
    }
}

void resetCommandTableStats(void) {
    struct redisCommand *c;
    dictEntry *de;
    dictIterator *di;

    di = dictGetSafeIterator(server.commands);
    while((de = dictNext(di)) != NULL) {
        c = (struct redisCommand *) dictGetVal(de);
        c->microseconds = 0;
        c->calls = 0;
        c->rejected_calls = 0;
        c->failed_calls = 0;
    }
    dictReleaseIterator(di);

}

void resetErrorTableStats(void) {
    raxFreeWithCallback(server.errors, zfree);
    server.errors = raxNew();
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
    oa->ops = NULL;
}

/* ====================== Commands lookup and execution ===================== */

struct redisCommand *lookupCommand(sds name) {
    return dictFetchValue(server.commands, name);
}

struct redisCommand *lookupCommandByCString(const char *s) {
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
 * This should not be used inside commands implementation since it will not
 * wrap the resulting commands in MULTI/EXEC. Use instead alsoPropagate(),
 * preventCommandPropagation(), forceCommandPropagation().
 *
 * However for functions that need to (also) propagate out of the context of a
 * command execution, for example when serving a blocked client, you
 * want to use propagate().
 */
void propagate(struct redisCommand *cmd, int dbid, robj **argv, int argc,
               int flags)
{
    UNUSED(cmd);

    if (!server.replication_allowed)
        return;

    /* Propagate a MULTI request once we encounter the first command which
     * is a write command.
     * This way we'll deliver the MULTI/..../EXEC block as a whole and
     * both the AOF and the replication link will have the same consistency
     * and atomicity guarantees. */
    if (server.in_exec && !server.propagate_in_transaction)
        execCommandPropagateMulti(dbid);

    /* This needs to be unreachable since the dataset should be fixed during 
     * client pause, otherwise data may be lost during a failover. */
    serverAssert(!(areClientsPaused() && !server.client_pause_in_transaction));

    if (server.aof_state != AOF_OFF && flags & PROPAGATE_AOF)
        feedAppendOnlyFile(dbid,argv,argc);
    if (flags & PROPAGATE_REPL)
        replicationFeedSlaves(server.slaves,dbid,argv,argc);
}

/* Used inside commands to schedule the propagation of additional commands
 * after the current command is propagated to AOF / Replication.
 *
 * 'cmd' must be a pointer to the Redis command to replicate, dbid is the
 * database ID the command should be propagated into.
 * Arguments of the command to propagate are passed as an array of redis
 * objects pointers of len 'argc', using the 'argv' vector.
 *
 * The function does not take a reference to the passed 'argv' vector,
 * so it is up to the caller to release the passed argv (but it is usually
 * stack allocated).  The function automatically increments ref count of
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

/* Call() is the core of Redis execution of a command.
 *
 * The following flags can be passed:
 * CMD_CALL_NONE        No flags.
 * CMD_CALL_SLOWLOG     Check command speed and log in the slow log if needed.
 * CMD_CALL_STATS       Populate command stats.
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
    monotime call_timer;
    int client_old_flags = c->flags;
    struct redisCommand *real_cmd = c->cmd;
    static long long prev_err_count;

    /* Initialization: clear the flags that must be set by the command on
     * demand, and initialize the array for additional commands propagation. */
    c->flags &= ~(CLIENT_FORCE_AOF|CLIENT_FORCE_REPL|CLIENT_PREVENT_PROP);
    redisOpArray prev_also_propagate = server.also_propagate;
    redisOpArrayInit(&server.also_propagate);

    /* Call the command. */
    dirty = server.dirty;
    prev_err_count = server.stat_total_error_replies;

    /* Update cache time, in case we have nested calls we want to
     * update only on the first call*/
    if (server.fixed_time_expire++ == 0) {
        updateCachedTime(0);
    }

    elapsedStart(&call_timer);
    c->cmd->proc(c);
    const long duration = elapsedUs(call_timer);
    c->duration = duration;
    dirty = server.dirty-dirty;
    if (dirty < 0) dirty = 0;

    /* Update failed command calls if required.
     * We leverage a static variable (prev_err_count) to retain
     * the counter across nested function calls and avoid logging
     * the same error twice. */
    if ((server.stat_total_error_replies - prev_err_count) > 0) {
        real_cmd->failed_calls++;
    }

    /* After executing command, we will close the client after writing entire
     * reply if it is set 'CLIENT_CLOSE_AFTER_COMMAND' flag. */
    if (c->flags & CLIENT_CLOSE_AFTER_COMMAND) {
        c->flags &= ~CLIENT_CLOSE_AFTER_COMMAND;
        c->flags |= CLIENT_CLOSE_AFTER_REPLY;
    }

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

    /* Note: the code below uses the real command that was executed
     * c->cmd and c->lastcmd may be different, in case of MULTI-EXEC or
     * re-written commands such as EXPIRE, GEOADD, etc. */

    /* Record the latency this command induced on the main thread.
     * unless instructed by the caller not to log. (happens when processing
     * a MULTI-EXEC from inside an AOF). */
    if (flags & CMD_CALL_SLOWLOG) {
        char *latency_event = (real_cmd->flags & CMD_FAST) ?
                               "fast-command" : "command";
        latencyAddSampleIfNeeded(latency_event,duration/1000);
    }

    /* Log the command into the Slow log if needed.
     * If the client is blocked we will handle slowlog when it is unblocked. */
    if ((flags & CMD_CALL_SLOWLOG) && !(c->flags & CLIENT_BLOCKED))
        slowlogPushCurrentCommand(c, real_cmd, duration);

    /* Send the command to clients in MONITOR mode if applicable.
     * Administrative commands are considered too dangerous to be shown. */
    if (!(c->cmd->flags & (CMD_SKIP_MONITOR|CMD_ADMIN))) {
        robj **argv = c->original_argv ? c->original_argv : c->argv;
        int argc = c->original_argv ? c->original_argc : c->argc;
        replicationFeedMonitors(c,server.monitors,c->db->id,argv,argc);
    }

    /* Clear the original argv.
     * If the client is blocked we will handle slowlog when it is unblocked. */
    if (!(c->flags & CLIENT_BLOCKED))
        freeClientOriginalArgv(c);

    /* populate the per-command statistics that we show in INFO commandstats. */
    if (flags & CMD_CALL_STATS) {
        real_cmd->microseconds += duration;
        real_cmd->calls++;
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
         * implementation called preventCommandPropagation() or similar,
         * or if we don't have the call() flags to do so. */
        if (c->flags & CLIENT_PREVENT_REPL_PROP ||
            !(flags & CMD_CALL_PROPAGATE_REPL))
                propagate_flags &= ~PROPAGATE_REPL;
        if (c->flags & CLIENT_PREVENT_AOF_PROP ||
            !(flags & CMD_CALL_PROPAGATE_AOF))
                propagate_flags &= ~PROPAGATE_AOF;

        /* Call propagate() only if at least one of AOF / replication
         * propagation is needed. Note that modules commands handle replication
         * in an explicit way, so we never replicate them automatically. */
        if (propagate_flags != PROPAGATE_NONE && !(c->cmd->flags & CMD_MODULE))
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
            int multi_emitted = 0;
            /* Wrap the commands in server.also_propagate array,
             * but don't wrap it if we are already in MULTI context,
             * in case the nested MULTI/EXEC.
             *
             * And if the array contains only one command, no need to
             * wrap it, since the single command is atomic. */
            if (server.also_propagate.numops > 1 &&
                !(c->cmd->flags & CMD_MODULE) &&
                !(c->flags & CLIENT_MULTI) &&
                !(flags & CMD_CALL_NOWRAP))
            {
                execCommandPropagateMulti(c->db->id);
                multi_emitted = 1;
            }

            for (j = 0; j < server.also_propagate.numops; j++) {
                rop = &server.also_propagate.ops[j];
                int target = rop->target;
                /* Whatever the command wish is, we honor the call() flags. */
                if (!(flags&CMD_CALL_PROPAGATE_AOF)) target &= ~PROPAGATE_AOF;
                if (!(flags&CMD_CALL_PROPAGATE_REPL)) target &= ~PROPAGATE_REPL;
                if (target)
                    propagate(rop->cmd,rop->dbid,rop->argv,rop->argc,target);
            }

            if (multi_emitted) {
                execCommandPropagateExec(c->db->id);
            }
        }
        redisOpArrayFree(&server.also_propagate);
    }
    server.also_propagate = prev_also_propagate;

    /* Client pause takes effect after a transaction has finished. This needs
     * to be located after everything is propagated. */
    if (!server.in_exec && server.client_pause_in_transaction) {
        server.client_pause_in_transaction = 0;
    }

    /* If the client has keys tracking enabled for client side caching,
     * make sure to remember the keys it fetched via this command. */
    if (c->cmd->flags & CMD_READONLY) {
        client *caller = (c->flags & CLIENT_LUA && server.lua_caller) ?
                            server.lua_caller : c;
        if (caller->flags & CLIENT_TRACKING &&
            !(caller->flags & CLIENT_TRACKING_BCAST))
        {
            trackingRememberKeys(caller);
        }
    }

    server.fixed_time_expire--;
    server.stat_numcommands++;
    prev_err_count = server.stat_total_error_replies;

    /* Record peak memory after each command and before the eviction that runs
     * before the next command. */
    size_t zmalloc_used = zmalloc_used_memory();
    if (zmalloc_used > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used;
}

/* Used when a command that is ready for execution needs to be rejected, due to
 * various pre-execution checks. it returns the appropriate error to the client.
 * If there's a transaction is flags it as dirty, and if the command is EXEC,
 * it aborts the transaction.
 * Note: 'reply' is expected to end with \r\n */
void rejectCommand(client *c, robj *reply) {
    flagTransaction(c);
    if (c->cmd) c->cmd->rejected_calls++;
    if (c->cmd && c->cmd->proc == execCommand) {
        execCommandAbort(c, reply->ptr);
    } else {
        /* using addReplyError* rather than addReply so that the error can be logged. */
        addReplyErrorObject(c, reply);
    }
}

void rejectCommandFormat(client *c, const char *fmt, ...) {
    if (c->cmd) c->cmd->rejected_calls++;
    flagTransaction(c);
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted (The args come from the user, they may contain any character). */
    sdsmapchars(s, "\r\n", "  ",  2);
    if (c->cmd && c->cmd->proc == execCommand) {
        execCommandAbort(c, s);
        sdsfree(s);
    } else {
        /* The following frees 's'. */
        addReplyErrorSds(c, s);
    }
}

/* Returns 1 for commands that may have key names in their arguments, but have
 * no pre-determined key positions. */
static int cmdHasMovableKeys(struct redisCommand *cmd) {
    return (cmd->getkeys_proc && !(cmd->flags & CMD_MODULE)) ||
            cmd->flags & CMD_MODULE_GETKEYS;
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
    if (!server.lua_timedout) {
        /* Both EXEC and EVAL call call() directly so there should be
         * no way in_exec or in_eval or propagate_in_transaction is 1.
         * That is unless lua_timedout, in which case client may run
         * some commands. */
        serverAssert(!server.propagate_in_transaction);
        serverAssert(!server.in_exec);
        serverAssert(!server.in_eval);
    }

    moduleCallCommandFilters(c);

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
        sds args = sdsempty();
        int i;
        for (i=1; i < c->argc && sdslen(args) < 128; i++)
            args = sdscatprintf(args, "`%.*s`, ", 128-(int)sdslen(args), (char*)c->argv[i]->ptr);
        rejectCommandFormat(c,"unknown command `%s`, with args beginning with: %s",
            (char*)c->argv[0]->ptr, args);
        sdsfree(args);
        return C_OK;
    } else if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) ||
               (c->argc < -c->cmd->arity)) {
        rejectCommandFormat(c,"wrong number of arguments for '%s' command",
            c->cmd->name);
        return C_OK;
    }

    int is_read_command = (c->cmd->flags & CMD_READONLY) ||
                           (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_READONLY));
    int is_write_command = (c->cmd->flags & CMD_WRITE) ||
                           (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_WRITE));
    int is_denyoom_command = (c->cmd->flags & CMD_DENYOOM) ||
                             (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_DENYOOM));
    int is_denystale_command = !(c->cmd->flags & CMD_STALE) ||
                               (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_STALE));
    int is_denyloading_command = !(c->cmd->flags & CMD_LOADING) ||
                                 (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_LOADING));
    int is_may_replicate_command = (c->cmd->flags & (CMD_WRITE | CMD_MAY_REPLICATE)) ||
                                   (c->cmd->proc == execCommand && (c->mstate.cmd_flags & (CMD_WRITE | CMD_MAY_REPLICATE)));

    /* Check if the user is authenticated. This check is skipped in case
     * the default user is flagged as "nopass" and is active. */
    int auth_required = (!(DefaultUser->flags & USER_FLAG_NOPASS) ||
                          (DefaultUser->flags & USER_FLAG_DISABLED)) &&
                        !c->authenticated;
    if (auth_required) {
        /* AUTH and HELLO and no auth modules are valid even in
         * non-authenticated state. */
        if (!(c->cmd->flags & CMD_NO_AUTH)) {
            rejectCommand(c,shared.noautherr);
            return C_OK;
        }
    }

    /* Check if the user can run this command according to the current
     * ACLs. */
    int acl_errpos;
    int acl_retval = ACLCheckAllPerm(c,&acl_errpos);
    if (acl_retval != ACL_OK) {
        addACLLogEntry(c,acl_retval,acl_errpos,NULL);
        switch (acl_retval) {
        case ACL_DENIED_CMD:
            rejectCommandFormat(c,
                "-NOPERM this user has no permissions to run "
                "the '%s' command or its subcommand", c->cmd->name);
            break;
        case ACL_DENIED_KEY:
            rejectCommandFormat(c,
                "-NOPERM this user has no permissions to access "
                "one of the keys used as arguments");
            break;
        case ACL_DENIED_CHANNEL:
            rejectCommandFormat(c,
                "-NOPERM this user has no permissions to access "
                "one of the channels used as arguments");
            break;
        default:
            rejectCommandFormat(c, "no permission");
            break;
        }
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
        !(!cmdHasMovableKeys(c->cmd) && c->cmd->firstkey == 0 &&
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
            c->cmd->rejected_calls++;
            return C_OK;
        }
    }

    /* Handle the maxmemory directive.
     *
     * Note that we do not want to reclaim memory if we are here re-entering
     * the event loop since there is a busy Lua script running in timeout
     * condition, to avoid mixing the propagation of scripts with the
     * propagation of DELs due to eviction. */
    if (server.maxmemory && !server.lua_timedout) {
        int out_of_memory = (performEvictions() == EVICT_FAIL);
        /* performEvictions may flush slave output buffers. This may result
         * in a slave, that may be the active client, to be freed. */
        if (server.current_client == NULL) return C_ERR;

        int reject_cmd_on_oom = is_denyoom_command;
        /* If client is in MULTI/EXEC context, queuing may consume an unlimited
         * amount of memory, so we want to stop that.
         * However, we never want to reject DISCARD, or even EXEC (unless it
         * contains denied commands, in which case is_denyoom_command is already
         * set. */
        if (c->flags & CLIENT_MULTI &&
            c->cmd->proc != execCommand &&
            c->cmd->proc != discardCommand &&
            c->cmd->proc != resetCommand) {
            reject_cmd_on_oom = 1;
        }

        if (out_of_memory && reject_cmd_on_oom) {
            rejectCommand(c, shared.oomerr);
            return C_OK;
        }

        /* Save out_of_memory result at script start, otherwise if we check OOM
         * until first write within script, memory used by lua stack and
         * arguments might interfere. */
        if (c->cmd->proc == evalCommand || c->cmd->proc == evalShaCommand) {
            server.lua_oom = out_of_memory;
        }
    }

    /* Make sure to use a reasonable amount of memory for client side
     * caching metadata. */
    if (server.tracking_clients) trackingLimitUsedSlots();

    /* Don't accept write commands if there are problems persisting on disk
     * and if this is a master instance. */
    int deny_write_type = writeCommandsDeniedByDiskError();
    if (deny_write_type != DISK_ERROR_TYPE_NONE &&
        server.masterhost == NULL &&
        (is_write_command ||c->cmd->proc == pingCommand))
    {
        if (deny_write_type == DISK_ERROR_TYPE_RDB)
            rejectCommand(c, shared.bgsaveerr);
        else
            rejectCommandFormat(c,
                "-MISCONF Errors writing to the AOF file: %s",
                strerror(server.aof_last_write_errno));
        return C_OK;
    }

    /* Don't accept write commands if there are not enough good slaves and
     * user configured the min-slaves-to-write option. */
    if (server.masterhost == NULL &&
        server.repl_min_slaves_to_write &&
        server.repl_min_slaves_max_lag &&
        is_write_command &&
        server.repl_good_slaves_count < server.repl_min_slaves_to_write)
    {
        rejectCommand(c, shared.noreplicaserr);
        return C_OK;
    }

    /* Don't accept write commands if this is a read only slave. But
     * accept write commands if this is our master. */
    if (server.masterhost && server.repl_slave_ro &&
        !(c->flags & CLIENT_MASTER) &&
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
        c->cmd->proc != unsubscribeCommand &&
        c->cmd->proc != psubscribeCommand &&
        c->cmd->proc != punsubscribeCommand &&
        c->cmd->proc != resetCommand) {
        rejectCommandFormat(c,
            "Can't execute '%s': only (P)SUBSCRIBE / "
            "(P)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context",
            c->cmd->name);
        return C_OK;
    }

    /* Only allow commands with flag "t", such as INFO, SLAVEOF and so on,
     * when slave-serve-stale-data is no and we are a slave with a broken
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
    if (server.loading && is_denyloading_command) {
        rejectCommand(c, shared.loadingerr);
        return C_OK;
    }

    /* Lua script too slow? Only allow a limited number of commands.
     * Note that we need to allow the transactions commands, otherwise clients
     * sending a transaction with pipelining without error checking, may have
     * the MULTI plus a few initial commands refused, then the timeout
     * condition resolves, and the bottom-half of the transaction gets
     * executed, see Github PR #7022. */
    if (server.lua_timedout &&
          c->cmd->proc != authCommand &&
          c->cmd->proc != helloCommand &&
          c->cmd->proc != replconfCommand &&
          c->cmd->proc != multiCommand &&
          c->cmd->proc != discardCommand &&
          c->cmd->proc != watchCommand &&
          c->cmd->proc != unwatchCommand &&
          c->cmd->proc != resetCommand &&
        !(c->cmd->proc == shutdownCommand &&
          c->argc == 2 &&
          tolower(((char*)c->argv[1]->ptr)[0]) == 'n') &&
        !(c->cmd->proc == scriptCommand &&
          c->argc == 2 &&
          tolower(((char*)c->argv[1]->ptr)[0]) == 'k'))
    {
        rejectCommand(c, shared.slowscripterr);
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
        ((server.client_pause_type == CLIENT_PAUSE_ALL) ||
        (server.client_pause_type == CLIENT_PAUSE_WRITE && is_may_replicate_command)))
    {
        c->bpop.timeout = 0;
        blockClient(c,BLOCKED_PAUSE);
        return C_OK;       
    }

    /* Exec the command */
    if (c->flags & CLIENT_MULTI &&
        c->cmd->proc != execCommand && c->cmd->proc != discardCommand &&
        c->cmd->proc != multiCommand && c->cmd->proc != watchCommand &&
        c->cmd->proc != resetCommand)
    {
        queueMultiCommand(c);
        addReply(c,shared.queued);
    } else {
        call(c,CMD_CALL_FULL);
        c->woff = server.master_repl_offset;
        if (listLength(server.ready_keys))
            handleClientsBlockedOnKeys();
    }

    return C_OK;
}

/* ====================== Error lookup and execution ===================== */

void incrementErrorCount(const char *fullerr, size_t namelen) {
    struct redisError *error = raxFind(server.errors,(unsigned char*)fullerr,namelen);
    if (error == raxNotFound) {
        error = zmalloc(sizeof(*error));
        error->count = 0;
        raxInsert(server.errors,(unsigned char*)fullerr,namelen,error,NULL);
    }
    error->count++;
}

/*================================== Shutdown =============================== */

/* Close listening sockets. Also unlink the unix domain socket if
 * unlink_unix_socket is non-zero. */
void closeListeningSockets(int unlink_unix_socket) {
    int j;

    for (j = 0; j < server.ipfd.count; j++) close(server.ipfd.fd[j]);
    for (j = 0; j < server.tlsfd.count; j++) close(server.tlsfd.fd[j]);
    if (server.sofd != -1) close(server.sofd);
    if (server.cluster_enabled)
        for (j = 0; j < server.cfd.count; j++) close(server.cfd.fd[j]);
    if (unlink_unix_socket && server.unixsocket) {
        serverLog(LL_NOTICE,"Removing the unix socket file.");
        unlink(server.unixsocket); /* don't care if this fails */
    }
}

int prepareForShutdown(int flags) {
    /* When SHUTDOWN is called while the server is loading a dataset in
     * memory we need to make sure no attempt is performed to save
     * the dataset on shutdown (otherwise it could overwrite the current DB
     * with half-read data).
     *
     * Also when in Sentinel mode clear the SAVE flag and force NOSAVE. */
    if (server.loading || server.sentinel_mode)
        flags = (flags & ~SHUTDOWN_SAVE) | SHUTDOWN_NOSAVE;

    int save = flags & SHUTDOWN_SAVE;
    int nosave = flags & SHUTDOWN_NOSAVE;

    serverLog(LL_WARNING,"User requested shutdown...");
    if (server.supervised_mode == SUPERVISED_SYSTEMD)
        redisCommunicateSystemd("STOPPING=1\n");

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

    if (server.aof_state != AOF_OFF) {
        /* Kill the AOF saving child as the AOF we already have may be longer
         * but contains the full dataset anyway. */
        if (server.child_type == CHILD_TYPE_AOF) {
            /* If we have AOF enabled but haven't written the AOF yet, don't
             * shutdown or else the dataset will be lost. */
            if (server.aof_state == AOF_WAIT_REWRITE) {
                serverLog(LL_WARNING, "Writing initial AOF, can't exit.");
                return C_ERR;
            }
            serverLog(LL_WARNING,
                "There is a child rewriting the AOF. Killing it!");
            killAppendOnlyChild();
        }
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
        if (rdbSave(server.rdb_filename,rsiptr) != C_OK) {
            /* Ooops.. error saving! The best we can do is to continue
             * operating. Note that if there was a background saving process,
             * in the next cron() Redis will be notified that the background
             * saving aborted, handling special stuff like slaves pending for
             * synchronization... */
            serverLog(LL_WARNING,"Error trying to save the DB, can't exit.");
            if (server.supervised_mode == SUPERVISED_SYSTEMD)
                redisCommunicateSystemd("STATUS=Error trying to save the DB, can't exit.\n");
            return C_ERR;
        }
    }

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
    serverLog(LL_WARNING,"%s is now ready to exit, bye bye...",
        server.sentinel_mode ? "Sentinel" : "Redis");
    return C_OK;
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

/* The PING command. It works in a different way if the client is in
 * in Pub/Sub mode. */
void pingCommand(client *c) {
    /* The command takes zero or one arguments. */
    if (c->argc > 2) {
        addReplyErrorFormat(c,"wrong number of arguments for '%s' command",
            c->cmd->name);
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
    struct timeval tv;

    /* gettimeofday() can only fail if &tv is a bad address so we
     * don't check for errors. */
    gettimeofday(&tv,NULL);
    addReplyArrayLen(c,2);
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
        addReplyNull(c);
    } else {
        /* We are adding: command name, arg count, flags, first, last, offset, categories */
        addReplyArrayLen(c, 7);
        addReplyBulkCString(c, cmd->name);
        addReplyLongLong(c, cmd->arity);

        int flagcount = 0;
        void *flaglen = addReplyDeferredLen(c);
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
        flagcount += addReplyCommandFlag(c,cmd,CMD_SKIP_SLOWLOG, "skip_slowlog");
        flagcount += addReplyCommandFlag(c,cmd,CMD_ASKING, "asking");
        flagcount += addReplyCommandFlag(c,cmd,CMD_FAST, "fast");
        flagcount += addReplyCommandFlag(c,cmd,CMD_NO_AUTH, "no_auth");
        flagcount += addReplyCommandFlag(c,cmd,CMD_MAY_REPLICATE, "may_replicate");
        if (cmdHasMovableKeys(cmd)) {
            addReplyStatus(c, "movablekeys");
            flagcount += 1;
        }
        setDeferredSetLen(c, flaglen, flagcount);

        addReplyLongLong(c, cmd->firstkey);
        addReplyLongLong(c, cmd->lastkey);
        addReplyLongLong(c, cmd->keystep);

        addReplyCommandCategories(c,cmd);
    }
}

/* COMMAND <subcommand> <args> */
void commandCommand(client *c) {
    dictIterator *di;
    dictEntry *de;

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"(no subcommand)",
"    Return details about all Redis commands.",
"COUNT",
"    Return the total number of commands in this Redis server.",
"GETKEYS <full-command>",
"    Return the keys from a full Redis command.",
"INFO [<command-name> ...]",
"    Return details about multiple Redis commands.",
NULL
        };
        addReplyHelp(c, help);
    } else if (c->argc == 1) {
        addReplyArrayLen(c, dictSize(server.commands));
        di = dictGetIterator(server.commands);
        while ((de = dictNext(di)) != NULL) {
            addReplyCommand(c, dictGetVal(de));
        }
        dictReleaseIterator(di);
    } else if (!strcasecmp(c->argv[1]->ptr, "info")) {
        int i;
        addReplyArrayLen(c, c->argc-2);
        for (i = 2; i < c->argc; i++) {
            addReplyCommand(c, dictFetchValue(server.commands, c->argv[i]->ptr));
        }
    } else if (!strcasecmp(c->argv[1]->ptr, "count") && c->argc == 2) {
        addReplyLongLong(c, dictSize(server.commands));
    } else if (!strcasecmp(c->argv[1]->ptr,"getkeys") && c->argc >= 3) {
        struct redisCommand *cmd = lookupCommand(c->argv[2]->ptr);
        getKeysResult result = GETKEYS_RESULT_INIT;
        int j;

        if (!cmd) {
            addReplyError(c,"Invalid command specified");
            return;
        } else if (cmd->getkeys_proc == NULL && cmd->firstkey == 0) {
            addReplyError(c,"The command has no key arguments");
            return;
        } else if ((cmd->arity > 0 && cmd->arity != c->argc-2) ||
                   ((c->argc-2) < -cmd->arity))
        {
            addReplyError(c,"Invalid number of arguments specified for command");
            return;
        }

        if (!getKeysFromCommand(cmd,c->argv+2,c->argc-2,&result)) {
            addReplyError(c,"Invalid arguments specified for command");
        } else {
            addReplyArrayLen(c,result.numkeys);
            for (j = 0; j < result.numkeys; j++) addReplyBulk(c,c->argv[result.keys[j]+2]);
        }
        getKeysFreeResult(&result);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

/* Convert an amount of bytes into a human readable string in the form
 * of 100B, 2G, 100M, 4K, and so forth. */
void bytesToHuman(char *s, unsigned long long n) {
    double d;

    if (n < 1024) {
        /* Bytes */
        sprintf(s,"%lluB",n);
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

/* Create the string returned by the INFO command. This is decoupled
 * by the INFO command itself as we need to report the same information
 * on memory corruption problems. */
sds genRedisInfoString(const char *section) {
    sds info = sdsempty();
    time_t uptime = server.unixtime-server.stat_starttime;
    int j;
    int allsections = 0, defsections = 0, everything = 0, modules = 0;
    int sections = 0;

    if (section == NULL) section = "default";
    allsections = strcasecmp(section,"all") == 0;
    defsections = strcasecmp(section,"default") == 0;
    everything = strcasecmp(section,"everything") == 0;
    modules = strcasecmp(section,"modules") == 0;
    if (everything) allsections = 1;

    /* Server */
    if (allsections || defsections || !strcasecmp(section,"server")) {
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

        unsigned int lruclock;
        atomicGet(server.lruclock,lruclock);
        info = sdscatfmt(info,
            "# Server\r\n"
            "redis_version:%s\r\n"
            "redis_git_sha1:%s\r\n"
            "redis_git_dirty:%i\r\n"
            "redis_build_id:%s\r\n"
            "redis_mode:%s\r\n"
            "os:%s %s %s\r\n"
            "arch_bits:%i\r\n"
            "multiplexing_api:%s\r\n"
            "atomicvar_api:%s\r\n"
            "gcc_version:%i.%i.%i\r\n"
            "process_id:%I\r\n"
            "process_supervised:%s\r\n"
            "run_id:%s\r\n"
            "tcp_port:%i\r\n"
            "server_time_usec:%I\r\n"
            "uptime_in_seconds:%I\r\n"
            "uptime_in_days:%I\r\n"
            "hz:%i\r\n"
            "configured_hz:%i\r\n"
            "lru_clock:%u\r\n"
            "executable:%s\r\n"
            "config_file:%s\r\n"
            "io_threads_active:%i\r\n",
            REDIS_VERSION,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            redisBuildIdString(),
            mode,
            name.sysname, name.release, name.machine,
            server.arch_bits,
            aeGetApiName(),
            REDIS_ATOMIC_API,
#ifdef __GNUC__
            __GNUC__,__GNUC_MINOR__,__GNUC_PATCHLEVEL__,
#else
            0,0,0,
#endif
            (int64_t) getpid(),
            supervised,
            server.runid,
            server.port ? server.port : server.tls_port,
            (int64_t)server.ustime,
            (int64_t)uptime,
            (int64_t)(uptime/(3600*24)),
            server.hz,
            server.config_hz,
            lruclock,
            server.executable ? server.executable : "",
            server.configfile ? server.configfile : "",
            server.io_threads_active);
    }

    /* Clients */
    if (allsections || defsections || !strcasecmp(section,"clients")) {
        size_t maxin, maxout;
        getExpansiveClientsInfo(&maxin,&maxout);
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Clients\r\n"
            "connected_clients:%lu\r\n"
            "cluster_connections:%lu\r\n"
            "maxclients:%u\r\n"
            "client_recent_max_input_buffer:%zu\r\n"
            "client_recent_max_output_buffer:%zu\r\n"
            "blocked_clients:%d\r\n"
            "tracking_clients:%d\r\n"
            "clients_in_timeout_table:%llu\r\n",
            listLength(server.clients)-listLength(server.slaves),
            getClusterConnectionsCount(),
            server.maxclients,
            maxin, maxout,
            server.blocked_clients,
            server.tracking_clients,
            (unsigned long long) raxSize(server.clients_timeout_table));
    }

    /* Memory */
    if (allsections || defsections || !strcasecmp(section,"memory")) {
        char hmem[64];
        char peak_hmem[64];
        char total_system_hmem[64];
        char used_memory_lua_hmem[64];
        char used_memory_scripts_hmem[64];
        char used_memory_rss_hmem[64];
        char maxmemory_hmem[64];
        size_t zmalloc_used = zmalloc_used_memory();
        size_t total_system_mem = server.system_memory_size;
        const char *evict_policy = evictPolicyToString();
        long long memory_lua = server.lua ? (long long)lua_gc(server.lua,LUA_GCCOUNT,0)*1024 : 0;
        struct redisMemOverhead *mh = getMemoryOverheadData();

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
        bytesToHuman(used_memory_scripts_hmem,mh->lua_caches);
        bytesToHuman(used_memory_rss_hmem,server.cron_malloc_stats.process_rss);
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
            "used_memory_peak_perc:%.2f%%\r\n"
            "used_memory_overhead:%zu\r\n"
            "used_memory_startup:%zu\r\n"
            "used_memory_dataset:%zu\r\n"
            "used_memory_dataset_perc:%.2f%%\r\n"
            "allocator_allocated:%zu\r\n"
            "allocator_active:%zu\r\n"
            "allocator_resident:%zu\r\n"
            "total_system_memory:%lu\r\n"
            "total_system_memory_human:%s\r\n"
            "used_memory_lua:%lld\r\n"
            "used_memory_lua_human:%s\r\n"
            "used_memory_scripts:%lld\r\n"
            "used_memory_scripts_human:%s\r\n"
            "number_of_cached_scripts:%lu\r\n"
            "maxmemory:%lld\r\n"
            "maxmemory_human:%s\r\n"
            "maxmemory_policy:%s\r\n"
            "allocator_frag_ratio:%.2f\r\n"
            "allocator_frag_bytes:%zu\r\n"
            "allocator_rss_ratio:%.2f\r\n"
            "allocator_rss_bytes:%zd\r\n"
            "rss_overhead_ratio:%.2f\r\n"
            "rss_overhead_bytes:%zd\r\n"
            "mem_fragmentation_ratio:%.2f\r\n"
            "mem_fragmentation_bytes:%zd\r\n"
            "mem_not_counted_for_evict:%zu\r\n"
            "mem_replication_backlog:%zu\r\n"
            "mem_clients_slaves:%zu\r\n"
            "mem_clients_normal:%zu\r\n"
            "mem_aof_buffer:%zu\r\n"
            "mem_allocator:%s\r\n"
            "active_defrag_running:%d\r\n"
            "lazyfree_pending_objects:%zu\r\n"
            "lazyfreed_objects:%zu\r\n",
            zmalloc_used,
            hmem,
            server.cron_malloc_stats.process_rss,
            used_memory_rss_hmem,
            server.stat_peak_memory,
            peak_hmem,
            mh->peak_perc,
            mh->overhead_total,
            mh->startup_allocated,
            mh->dataset,
            mh->dataset_perc,
            server.cron_malloc_stats.allocator_allocated,
            server.cron_malloc_stats.allocator_active,
            server.cron_malloc_stats.allocator_resident,
            (unsigned long)total_system_mem,
            total_system_hmem,
            memory_lua,
            used_memory_lua_hmem,
            (long long) mh->lua_caches,
            used_memory_scripts_hmem,
            dictSize(server.lua_scripts),
            server.maxmemory,
            maxmemory_hmem,
            evict_policy,
            mh->allocator_frag,
            mh->allocator_frag_bytes,
            mh->allocator_rss,
            mh->allocator_rss_bytes,
            mh->rss_extra,
            mh->rss_extra_bytes,
            mh->total_frag,       /* This is the total RSS overhead, including
                                     fragmentation, but not just it. This field
                                     (and the next one) is named like that just
                                     for backward compatibility. */
            mh->total_frag_bytes,
            freeMemoryGetNotCountedMemory(),
            mh->repl_backlog,
            mh->clients_slaves,
            mh->clients_normal,
            mh->aof_buffer,
            ZMALLOC_LIB,
            server.active_defrag_running,
            lazyfreeGetPendingObjectsCount(),
            lazyfreeGetFreedObjectsCount()
        );
        freeMemoryOverheadData(mh);
    }

    /* Persistence */
    if (allsections || defsections || !strcasecmp(section,"persistence")) {
        if (sections++) info = sdscat(info,"\r\n");
        double fork_perc = 0;
        if (server.stat_module_progress) {
            fork_perc = server.stat_module_progress * 100;
        } else if (server.stat_current_save_keys_total) {
            fork_perc = ((double)server.stat_current_save_keys_processed / server.stat_current_save_keys_total) * 100;
        }
        int aof_bio_fsync_status;
        atomicGet(server.aof_bio_fsync_status,aof_bio_fsync_status);

        info = sdscatprintf(info,
            "# Persistence\r\n"
            "loading:%d\r\n"
            "current_cow_size:%zu\r\n"
            "current_cow_size_age:%lu\r\n"
            "current_fork_perc:%.2f\r\n"
            "current_save_keys_processed:%zu\r\n"
            "current_save_keys_total:%zu\r\n"
            "rdb_changes_since_last_save:%lld\r\n"
            "rdb_bgsave_in_progress:%d\r\n"
            "rdb_last_save_time:%jd\r\n"
            "rdb_last_bgsave_status:%s\r\n"
            "rdb_last_bgsave_time_sec:%jd\r\n"
            "rdb_current_bgsave_time_sec:%jd\r\n"
            "rdb_last_cow_size:%zu\r\n"
            "aof_enabled:%d\r\n"
            "aof_rewrite_in_progress:%d\r\n"
            "aof_rewrite_scheduled:%d\r\n"
            "aof_last_rewrite_time_sec:%jd\r\n"
            "aof_current_rewrite_time_sec:%jd\r\n"
            "aof_last_bgrewrite_status:%s\r\n"
            "aof_last_write_status:%s\r\n"
            "aof_last_cow_size:%zu\r\n"
            "module_fork_in_progress:%d\r\n"
            "module_fork_last_cow_size:%zu\r\n",
            (int)server.loading,
            server.stat_current_cow_bytes,
            server.stat_current_cow_updated ? (unsigned long) elapsedMs(server.stat_current_cow_updated) / 1000 : 0,
            fork_perc,
            server.stat_current_save_keys_processed,
            server.stat_current_save_keys_total,
            server.dirty,
            server.child_type == CHILD_TYPE_RDB,
            (intmax_t)server.lastsave,
            (server.lastbgsave_status == C_OK) ? "ok" : "err",
            (intmax_t)server.rdb_save_time_last,
            (intmax_t)((server.child_type != CHILD_TYPE_RDB) ?
                -1 : time(NULL)-server.rdb_save_time_start),
            server.stat_rdb_cow_bytes,
            server.aof_state != AOF_OFF,
            server.child_type == CHILD_TYPE_AOF,
            server.aof_rewrite_scheduled,
            (intmax_t)server.aof_rewrite_time_last,
            (intmax_t)((server.child_type != CHILD_TYPE_AOF) ?
                -1 : time(NULL)-server.aof_rewrite_time_start),
            (server.aof_lastbgrewrite_status == C_OK) ? "ok" : "err",
            (server.aof_last_write_status == C_OK &&
                aof_bio_fsync_status == C_OK) ? "ok" : "err",
            server.stat_aof_cow_bytes,
            server.child_type == CHILD_TYPE_MODULE,
            server.stat_module_cow_bytes);

        if (server.aof_enabled) {
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

            info = sdscatprintf(info,
                "loading_start_time:%jd\r\n"
                "loading_total_bytes:%llu\r\n"
                "loading_rdb_used_mem:%llu\r\n"
                "loading_loaded_bytes:%llu\r\n"
                "loading_loaded_perc:%.2f\r\n"
                "loading_eta_seconds:%jd\r\n",
                (intmax_t) server.loading_start_time,
                (unsigned long long) server.loading_total_bytes,
                (unsigned long long) server.loading_rdb_used_mem,
                (unsigned long long) server.loading_loaded_bytes,
                perc,
                (intmax_t)eta
            );
        }
    }

    /* Stats */
    if (allsections || defsections || !strcasecmp(section,"stats")) {
        long long stat_total_reads_processed, stat_total_writes_processed;
        long long stat_net_input_bytes, stat_net_output_bytes;
        atomicGet(server.stat_total_reads_processed, stat_total_reads_processed);
        atomicGet(server.stat_total_writes_processed, stat_total_writes_processed);
        atomicGet(server.stat_net_input_bytes, stat_net_input_bytes);
        atomicGet(server.stat_net_output_bytes, stat_net_output_bytes);

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
            "expired_stale_perc:%.2f\r\n"
            "expired_time_cap_reached_count:%lld\r\n"
            "expire_cycle_cpu_milliseconds:%lld\r\n"
            "evicted_keys:%lld\r\n"
            "keyspace_hits:%lld\r\n"
            "keyspace_misses:%lld\r\n"
            "pubsub_channels:%ld\r\n"
            "pubsub_patterns:%lu\r\n"
            "latest_fork_usec:%lld\r\n"
            "total_forks:%lld\r\n"
            "migrate_cached_sockets:%ld\r\n"
            "slave_expires_tracked_keys:%zu\r\n"
            "active_defrag_hits:%lld\r\n"
            "active_defrag_misses:%lld\r\n"
            "active_defrag_key_hits:%lld\r\n"
            "active_defrag_key_misses:%lld\r\n"
            "tracking_total_keys:%lld\r\n"
            "tracking_total_items:%lld\r\n"
            "tracking_total_prefixes:%lld\r\n"
            "unexpected_error_replies:%lld\r\n"
            "total_error_replies:%lld\r\n"
            "dump_payload_sanitizations:%lld\r\n"
            "total_reads_processed:%lld\r\n"
            "total_writes_processed:%lld\r\n"
            "io_threaded_reads_processed:%lld\r\n"
            "io_threaded_writes_processed:%lld\r\n",
            server.stat_numconnections,
            server.stat_numcommands,
            getInstantaneousMetric(STATS_METRIC_COMMAND),
            stat_net_input_bytes,
            stat_net_output_bytes,
            (float)getInstantaneousMetric(STATS_METRIC_NET_INPUT)/1024,
            (float)getInstantaneousMetric(STATS_METRIC_NET_OUTPUT)/1024,
            server.stat_rejected_conn,
            server.stat_sync_full,
            server.stat_sync_partial_ok,
            server.stat_sync_partial_err,
            server.stat_expiredkeys,
            server.stat_expired_stale_perc*100,
            server.stat_expired_time_cap_reached_count,
            server.stat_expire_cycle_time_used/1000,
            server.stat_evictedkeys,
            server.stat_keyspace_hits,
            server.stat_keyspace_misses,
            dictSize(server.pubsub_channels),
            dictSize(server.pubsub_patterns),
            server.stat_fork_time,
            server.stat_total_forks,
            dictSize(server.migrate_cached_sockets),
            getSlaveKeyWithExpireCount(),
            server.stat_active_defrag_hits,
            server.stat_active_defrag_misses,
            server.stat_active_defrag_key_hits,
            server.stat_active_defrag_key_misses,
            (unsigned long long) trackingGetTotalKeys(),
            (unsigned long long) trackingGetTotalItems(),
            (unsigned long long) trackingGetTotalPrefixes(),
            server.stat_unexpected_error_replies,
            server.stat_total_error_replies,
            server.stat_dump_payload_sanitizations,
            stat_total_reads_processed,
            stat_total_writes_processed,
            server.stat_io_reads_processed,
            server.stat_io_writes_processed);
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
                double perc = 0;
                if (server.repl_transfer_size) {
                    perc = ((double)server.repl_transfer_read / server.repl_transfer_size) * 100;
                }
                info = sdscatprintf(info,
                    "master_sync_total_bytes:%lld\r\n"
                    "master_sync_read_bytes:%lld\r\n"
                    "master_sync_left_bytes:%lld\r\n"
                    "master_sync_perc:%.2f\r\n"
                    "master_sync_last_io_seconds_ago:%d\r\n",
                    (long long) server.repl_transfer_size,
                    (long long) server.repl_transfer_read,
                    (long long) (server.repl_transfer_size - server.repl_transfer_read),
                    perc,
                    (int)(server.unixtime-server.repl_transfer_lastio)
                );
            }

            if (server.repl_state != REPL_STATE_CONNECTED) {
                info = sdscatprintf(info,
                    "master_link_down_since_seconds:%jd\r\n",
                    server.repl_down_since ?
                    (intmax_t)(server.unixtime-server.repl_down_since) : -1);
            }
            info = sdscatprintf(info,
                "slave_priority:%d\r\n"
                "slave_read_only:%d\r\n"
                "replica_announced:%d\r\n",
                server.slave_priority,
                server.repl_slave_ro,
                server.replica_announced);
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
                char ip[NET_IP_STR_LEN], *slaveip = slave->slave_addr;
                int port;
                long lag = 0;

                if (!slaveip) {
                    if (connPeerToString(slave->conn,ip,sizeof(ip),&port) == -1)
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
            "master_failover_state:%s\r\n"
            "master_replid:%s\r\n"
            "master_replid2:%s\r\n"
            "master_repl_offset:%lld\r\n"
            "second_repl_offset:%lld\r\n"
            "repl_backlog_active:%d\r\n"
            "repl_backlog_size:%lld\r\n"
            "repl_backlog_first_byte_offset:%lld\r\n"
            "repl_backlog_histlen:%lld\r\n",
            getFailoverStateString(),
            server.replid,
            server.replid2,
            server.master_repl_offset,
            server.second_replid_offset,
            server.repl_backlog != NULL,
            server.repl_backlog_size,
            server.repl_backlog_off,
            server.repl_backlog_histlen);
    }

    /* CPU */
    if (allsections || defsections || !strcasecmp(section,"cpu")) {
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
    if (allsections || defsections || !strcasecmp(section,"modules")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,"# Modules\r\n");
        info = genModulesInfoString(info);
    }

    /* Command statistics */
    if (allsections || !strcasecmp(section,"commandstats")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Commandstats\r\n");

        struct redisCommand *c;
        dictEntry *de;
        dictIterator *di;
        di = dictGetSafeIterator(server.commands);
        while((de = dictNext(di)) != NULL) {
            char *tmpsafe;
            c = (struct redisCommand *) dictGetVal(de);
            if (!c->calls && !c->failed_calls && !c->rejected_calls)
                continue;
            info = sdscatprintf(info,
                "cmdstat_%s:calls=%lld,usec=%lld,usec_per_call=%.2f"
                ",rejected_calls=%lld,failed_calls=%lld\r\n",
                getSafeInfoString(c->name, strlen(c->name), &tmpsafe), c->calls, c->microseconds,
                (c->calls == 0) ? 0 : ((float)c->microseconds/c->calls),
                c->rejected_calls, c->failed_calls);
            if (tmpsafe != NULL) zfree(tmpsafe);
        }
        dictReleaseIterator(di);
    }
    /* Error statistics */
    if (allsections || defsections || !strcasecmp(section,"errorstats")) {
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

    /* Get info from modules.
     * if user asked for "everything" or "modules", or a specific section
     * that's not found yet. */
    if (everything || modules ||
        (!allsections && !defsections && sections==0)) {
        info = modulesCollectInfo(info,
                                  everything || modules ? NULL: section,
                                  0, /* not a crash report */
                                  sections);
    }
    return info;
}

void infoCommand(client *c) {
    char *section = c->argc == 2 ? c->argv[1]->ptr : "default";

    if (c->argc > 2) {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }
    sds info = genRedisInfoString(section);
    addReplyVerbatim(c,info,sdslen(info),"txt");
    sdsfree(info);
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
    if (THPIsEnabled() && THPDisable()) {
        serverLog(LL_WARNING,"WARNING you have Transparent Huge Pages (THP) support enabled in your kernel. This will create latency and memory usage issues with Redis. To fix this issue run the command 'echo madvise > /sys/kernel/mm/transparent_hugepage/enabled' as root, and add it to your /etc/rc.local in order to retain the setting after a reboot. Redis must be restarted after THP is disabled (set to 'madvise' or 'never').");
    }
}

#ifdef __arm64__

/* Get size in kilobytes of the Shared_Dirty pages of the calling process for the
 * memory map corresponding to the provided address, or -1 on error. */
static int smapsGetSharedDirty(unsigned long addr) {
    int ret, in_mapping = 0, val = -1;
    unsigned long from, to;
    char buf[64];
    FILE *f;

    f = fopen("/proc/self/smaps", "r");
    if (!f) return -1;

    while (1) {
        if (!fgets(buf, sizeof(buf), f))
            break;

        ret = sscanf(buf, "%lx-%lx", &from, &to);
        if (ret == 2)
            in_mapping = from <= addr && addr < to;

        if (in_mapping && !memcmp(buf, "Shared_Dirty:", 13)) {
            sscanf(buf, "%*s %d", &val);
            /* If parsing fails, we remain with val == -1 */
            break;
        }
    }

    fclose(f);
    return val;
}

/* Older arm64 Linux kernels have a bug that could lead to data corruption
 * during background save in certain scenarios. This function checks if the
 * kernel is affected.
 * The bug was fixed in commit ff1712f953e27f0b0718762ec17d0adb15c9fd0b
 * titled: "arm64: pgtable: Ensure dirty bit is preserved across pte_wrprotect()"
 * Return -1 on unexpected test failure, 1 if the kernel seems to be affected,
 * and 0 otherwise. */
int linuxMadvFreeForkBugCheck(void) {
    int ret, pipefd[2] = { -1, -1 };
    pid_t pid;
    char *p = NULL, *q;
    int bug_found = 0;
    long page_size = sysconf(_SC_PAGESIZE);
    long map_size = 3 * page_size;

    /* Create a memory map that's in our full control (not one used by the allocator). */
    p = mmap(NULL, map_size, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        serverLog(LL_WARNING, "Failed to mmap(): %s", strerror(errno));
        return -1;
    }

    q = p + page_size;

    /* Split the memory map in 3 pages by setting their protection as RO|RW|RO to prevent
     * Linux from merging this memory map with adjacent VMAs. */
    ret = mprotect(q, page_size, PROT_READ | PROT_WRITE);
    if (ret < 0) {
        serverLog(LL_WARNING, "Failed to mprotect(): %s", strerror(errno));
        bug_found = -1;
        goto exit;
    }

    /* Write to the page once to make it resident */
    *(volatile char*)q = 0;

    /* Tell the kernel that this page is free to be reclaimed. */
#ifndef MADV_FREE
#define MADV_FREE 8
#endif
    ret = madvise(q, page_size, MADV_FREE);
    if (ret < 0) {
        /* MADV_FREE is not available on older kernels that are presumably
         * not affected. */
        if (errno == EINVAL) goto exit;

        serverLog(LL_WARNING, "Failed to madvise(): %s", strerror(errno));
        bug_found = -1;
        goto exit;
    }

    /* Write to the page after being marked for freeing, this is supposed to take
     * ownership of that page again. */
    *(volatile char*)q = 0;

    /* Create a pipe for the child to return the info to the parent. */
    ret = pipe(pipefd);
    if (ret < 0) {
        serverLog(LL_WARNING, "Failed to create pipe: %s", strerror(errno));
        bug_found = -1;
        goto exit;
    }

    /* Fork the process. */
    pid = fork();
    if (pid < 0) {
        serverLog(LL_WARNING, "Failed to fork: %s", strerror(errno));
        bug_found = -1;
        goto exit;
    } else if (!pid) {
        /* Child: check if the page is marked as dirty, page_size in kb.
         * A value of 0 means the kernel is affected by the bug. */
        ret = smapsGetSharedDirty((unsigned long) q);
        if (!ret)
            bug_found = 1;
        else if (ret == -1)     /* Failed to read */
            bug_found = -1;

        if (write(pipefd[1], &bug_found, sizeof(bug_found)) < 0)
            serverLog(LL_WARNING, "Failed to write to parent: %s", strerror(errno));
        exitFromChild(0);
    } else {
        /* Read the result from the child. */
        ret = read(pipefd[0], &bug_found, sizeof(bug_found));
        if (ret < 0) {
            serverLog(LL_WARNING, "Failed to read from child: %s", strerror(errno));
            bug_found = -1;
        }

        /* Reap the child pid. */
        waitpid(pid, NULL, 0);
    }

exit:
    /* Cleanup */
    if (pipefd[0] != -1) close(pipefd[0]);
    if (pipefd[1] != -1) close(pipefd[1]);
    if (p != NULL) munmap(p, map_size);

    return bug_found;
}
#endif /* __arm64__ */
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
    fprintf(stderr,"Usage: ./redis-server [/path/to/redis.conf] [options] [-]\n");
    fprintf(stderr,"       ./redis-server - (read config from stdin)\n");
    fprintf(stderr,"       ./redis-server -v or --version\n");
    fprintf(stderr,"       ./redis-server -h or --help\n");
    fprintf(stderr,"       ./redis-server --test-memory <megabytes>\n\n");
    fprintf(stderr,"Examples:\n");
    fprintf(stderr,"       ./redis-server (run the server with default conf)\n");
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

int changeBindAddr(sds *addrlist, int addrlist_len) {
    int i;
    int result = C_OK;

    char *prev_bindaddr[CONFIG_BINDADDR_MAX];
    int prev_bindaddr_count;

    /* Close old TCP and TLS servers */
    closeSocketListeners(&server.ipfd);
    closeSocketListeners(&server.tlsfd);

    /* Keep previous settings */
    prev_bindaddr_count = server.bindaddr_count;
    memcpy(prev_bindaddr, server.bindaddr, sizeof(server.bindaddr));

    /* Copy new settings */
    memset(server.bindaddr, 0, sizeof(server.bindaddr));
    for (i = 0; i < addrlist_len; i++) {
        server.bindaddr[i] = zstrdup(addrlist[i]);
    }
    server.bindaddr_count = addrlist_len;

    /* Bind to the new port */
    if ((server.port != 0 && listenToPort(server.port, &server.ipfd) != C_OK) ||
        (server.tls_port != 0 && listenToPort(server.tls_port, &server.tlsfd) != C_OK)) {
        serverLog(LL_WARNING, "Failed to bind, trying to restore old listening sockets.");

        /* Restore old bind addresses */
        for (i = 0; i < addrlist_len; i++) {
            zfree(server.bindaddr[i]);
        }
        memcpy(server.bindaddr, prev_bindaddr, sizeof(server.bindaddr));
        server.bindaddr_count = prev_bindaddr_count;

        /* Re-Listen TCP and TLS */
        server.ipfd.count = 0;
        if (server.port != 0 && listenToPort(server.port, &server.ipfd) != C_OK) {
            serverPanic("Failed to restore old listening TCP socket.");
        }

        server.tlsfd.count = 0;
        if (server.tls_port != 0 && listenToPort(server.tls_port, &server.tlsfd) != C_OK) {
            serverPanic("Failed to restore old listening TLS socket.");
        }

        result = C_ERR;
    } else {
        /* Free old bind addresses */
        for (i = 0; i < prev_bindaddr_count; i++) {
            zfree(prev_bindaddr[i]);
        }
    }

    /* Create TCP and TLS event handlers */
    if (createSocketAcceptHandler(&server.ipfd, acceptTcpHandler) != C_OK) {
        serverPanic("Unrecoverable error creating TCP socket accept handler.");
    }
    if (createSocketAcceptHandler(&server.tlsfd, acceptTLSHandler) != C_OK) {
        serverPanic("Unrecoverable error creating TLS socket accept handler.");
    }

    if (server.set_proc_title) redisSetProcTitle(NULL);

    return result;
}

int changeListenPort(int port, socketFds *sfd, aeFileProc *accept_handler) {
    socketFds new_sfd = {{0}};

    /* Just close the server if port disabled */
    if (port == 0) {
        closeSocketListeners(sfd);
        if (server.set_proc_title) redisSetProcTitle(NULL);
        return C_OK;
    }

    /* Bind to the new port */
    if (listenToPort(port, &new_sfd) != C_OK) {
        return C_ERR;
    }

    /* Create event handlers */
    if (createSocketAcceptHandler(&new_sfd, accept_handler) != C_OK) {
        closeSocketListeners(&new_sfd);
        return C_ERR;
    }

    /* Close old servers */
    closeSocketListeners(sfd);

    /* Copy new descriptors */
    sfd->count = new_sfd.count;
    memcpy(sfd->fd, new_sfd.fd, sizeof(new_sfd.fd));

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
     * on disk. */
    if (server.shutdown_asap && sig == SIGINT) {
        serverLogFromHandler(LL_WARNING, "You insist... exiting now.");
        rdbRemoveTempFile(getpid(), 1);
        exit(1); /* Exit with an error since this was not a clean shutdown. */
    } else if (server.loading) {
        serverLogFromHandler(LL_WARNING, "Received shutdown signal during loading, exiting now.");
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

    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESETHAND | SA_SIGINFO;
    act.sa_sigaction = sigsegvHandler;
    if(server.crashlog_enabled) {
        sigaction(SIGSEGV, &act, NULL);
        sigaction(SIGBUS, &act, NULL);
        sigaction(SIGFPE, &act, NULL);
        sigaction(SIGILL, &act, NULL);
        sigaction(SIGABRT, &act, NULL);
    }
    return;
}

void removeSignalHandlers(void) {
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESETHAND;
    act.sa_handler = SIG_DFL;
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGFPE, &act, NULL);
    sigaction(SIGILL, &act, NULL);
    sigaction(SIGABRT, &act, NULL);
}

/* This is the signal handler for children process. It is currently useful
 * in order to track the SIGUSR1, that we send to a child in order to terminate
 * it in a clean way, without the parent detecting an error and stop
 * accepting writes because of a write error condition. */
static void sigKillChildHandler(int sig) {
    UNUSED(sig);
    int level = server.in_fork_child == CHILD_TYPE_MODULE? LL_VERBOSE: LL_WARNING;
    serverLogFromHandler(level, "Received SIGUSR1 in child, exiting now.");
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
    return;
}

/* After fork, the child process will inherit the resources
 * of the parent process, e.g. fd(socket or flock) etc.
 * should close the resources not used by the child process, so that if the
 * parent restarts it can bind/lock despite the child possibly still running. */
void closeChildUnusedResourceAfterFork() {
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
        if (hasActiveChildProcess())
            return -1;

        openChildInfoPipe();
    }

    int childpid;
    long long start = ustime();
    if ((childpid = fork()) == 0) {
        /* Child */
        server.in_fork_child = purpose;
        setOOMScoreAdj(CONFIG_OOM_BGCHILD);
        setupChildSignalHandlers();
        closeChildUnusedResourceAfterFork();
    } else {
        /* Parent */
        server.stat_total_forks++;
        server.stat_fork_time = ustime()-start;
        server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
        latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);
        if (childpid == -1) {
            if (isMutuallyExclusiveChildType(purpose)) closeChildInfoPipe();
            return -1;
        }

        /* The child_pid and child_type are only for mutual exclusive children.
         * other child types should handle and store their pid's in dedicated variables.
         *
         * Today, we allows CHILD_TYPE_LDB to run in parallel with the other fork types:
         * - it isn't used for production, so it will not make the server be less efficient
         * - used for debugging, and we don't want to block it from running while other
         *   forks are running (like RDB and AOF) */
        if (isMutuallyExclusiveChildType(purpose)) {
            server.child_pid = childpid;
            server.child_type = purpose;
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
        /* It's not a failure if the file is empty or doesn't exist (later we will create it) */
        int ret = loadAppendOnlyFile(server.aof_filename);
        if (ret == AOF_FAILED || ret == AOF_OPEN_ERR)
            exit(1);
        if (ret == AOF_OK)
            serverLog(LL_NOTICE,"DB loaded from append only file: %.3f seconds",(float)(ustime()-start)/1000000);
    } else {
        rdbSaveInfo rsi = RDB_SAVE_INFO_INIT;
        errno = 0; /* Prevent a stale value from affecting error checking */
        if (rdbLoad(server.rdb_filename,&rsi,RDBFLAGS_NONE) == C_OK) {
            serverLog(LL_NOTICE,"DB loaded from disk: %.3f seconds",
                (float)(ustime()-start)/1000000);

            /* Restore the replication ID / offset from the RDB file. */
            if ((server.masterhost ||
                (server.cluster_enabled &&
                nodeIsSlave(server.cluster->myself))) &&
                rsi.repl_id_is_set &&
                rsi.repl_offset != -1 &&
                /* Note that older implementations may save a repl_stream_db
                 * of -1 inside the RDB file in a wrong way, see more
                 * information in function rdbPopulateSaveInfo. */
                rsi.repl_stream_db != -1)
            {
                memcpy(server.replid,rsi.repl_id,sizeof(server.replid));
                server.master_repl_offset = rsi.repl_offset;
                /* If we are a slave, create a cached master from this
                 * information, in order to allow partial resynchronization
                 * with masters. */
                replicationCacheMasterUsingMyself();
                selectDb(server.cached_master,rsi.repl_stream_db);
            }
        } else if (errno != ENOENT) {
            serverLog(LL_WARNING,"Fatal error loading the DB: %s. Exiting.",strerror(errno));
            exit(1);
        }
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
            (server.cluster_enabled && nodeIsMaster(server.cluster->myself)));
}

#ifdef REDIS_TEST
typedef int redisTestProc(int argc, char **argv, int accurate);
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
    {"dict", dictTest},
    {"listpack", listpackTest}
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
    if (argc >= 3 && !strcasecmp(argv[1], "test")) {
        int accurate = 0;
        for (j = 3; j < argc; j++) {
            if (!strcasecmp(argv[j], "--accurate")) {
                accurate = 1;
            }
        }

        if (!strcasecmp(argv[2], "all")) {
            int numtests = sizeof(redisTests)/sizeof(struct redisTest);
            for (j = 0; j < numtests; j++) {
                redisTests[j].failed = (redisTests[j].proc(argc,argv,accurate) != 0);
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
            return proc(argc,argv,accurate);
        }

        return 0;
    }
#endif

    /* We need to initialize our libraries, and the server configuration. */
#ifdef INIT_SETPROCTITLE_REPLACEMENT
    spt_init(argc, argv);
#endif
    setlocale(LC_COLLATE,"");
    tzset(); /* Populates 'timezone' global. */
    zmalloc_set_oom_handler(redisOutOfMemoryHandler);
    srand(time(NULL)^getpid());
    srandom(time(NULL)^getpid());
    gettimeofday(&tv,NULL);
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
    tlsInit();

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
        while(j < argc) {
            /* Either first or last argument - Should we read config from stdin? */
            if (argv[j][0] == '-' && argv[j][1] == '\0' && (j == 1 || j == argc-1)) {
                config_from_stdin = 1;
            }
            /* All the other options are parsed and conceptually appended to the
             * configuration file. For instance --port 6380 will generate the
             * string "port 6380\n" to be parsed after the actual config file
             * and stdin input are parsed (if they exist). */
            else if (argv[j][0] == '-' && argv[j][1] == '-') {
                /* Option name */
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

        loadServerConfig(server.configfile, config_from_stdin, options);
        if (server.sentinel_mode) loadSentinelConfigFromQueue();
        sdsfree(options);
    }
    if (server.sentinel_mode) sentinelCheckConfigFile();
    server.supervised = redisIsSupervised(server.supervised_mode);
    int background = server.daemonize && !server.supervised;
    if (background) daemonize();

    serverLog(LL_WARNING, "oO0OoO0OoO0Oo Redis is starting oO0OoO0OoO0Oo");
    serverLog(LL_WARNING,
        "Redis version=%s, bits=%d, commit=%s, modified=%d, pid=%d, just started",
            REDIS_VERSION,
            (sizeof(long) == 8) ? 64 : 32,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            (int)getpid());

    if (argc == 1) {
        serverLog(LL_WARNING, "Warning: no config file specified, using the default config. In order to specify a config file use %s /path/to/redis.conf", argv[0]);
    } else {
        serverLog(LL_WARNING, "Configuration loaded");
    }

    readOOMScoreAdj();
    initServer();
    if (background || server.pidfile) createPidFile();
    if (server.set_proc_title) redisSetProcTitle(NULL);
    redisAsciiArt();
    checkTcpBacklogSettings();

    if (!server.sentinel_mode) {
        /* Things not needed when running in Sentinel mode. */
        serverLog(LL_WARNING,"Server initialized");
    #ifdef __linux__
        linuxMemoryWarnings();
    #if defined (__arm64__)
        int ret;
        if ((ret = linuxMadvFreeForkBugCheck())) {
            if (ret == 1)
                serverLog(LL_WARNING,"WARNING Your kernel has a bug that could lead to data corruption during background save. "
                                     "Please upgrade to the latest stable kernel.");
            else
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
        moduleInitModulesSystemLast();
        moduleLoadFromQueue();
        ACLLoadUsersAtStartup();
        InitServerLast();
        loadDataFromDisk();
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
        if (server.cluster_enabled) {
            if (verifyClusterConfigWithData() == C_ERR) {
                serverLog(LL_WARNING,
                    "You can't have keys in a DB different than DB 0 when in "
                    "Cluster mode. Exiting.");
                exit(1);
            }
        }
        if (server.ipfd.count > 0 || server.tlsfd.count > 0)
            serverLog(LL_NOTICE,"Ready to accept connections");
        if (server.sofd > 0)
            serverLog(LL_NOTICE,"The server is now ready to accept connections at %s", server.unixsocket);
        if (server.supervised_mode == SUPERVISED_SYSTEMD) {
            if (!server.masterhost) {
                redisCommunicateSystemd("STATUS=Ready to accept connections\n");
            } else {
                redisCommunicateSystemd("STATUS=Ready to accept connections in read-only mode. Waiting for MASTER <-> REPLICA sync\n");
            }
            redisCommunicateSystemd("READY=1\n");
        }
    } else {
        ACLLoadUsersAtStartup();
        InitServerLast();
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

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

#ifndef __REDIS_H
#define __REDIS_H

#include "fmacros.h"
#include "config.h"
#include "solarisfixes.h"
#include "rio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <syslog.h>
#include <netinet/in.h>
#include <lua.h>
#include <signal.h>

typedef long long mstime_t; /* millisecond time type. */

#include "ae.h"      /* Event driven programming library */
#include "sds.h"     /* Dynamic safe strings */
#include "dict.h"    /* Hash tables */
#include "adlist.h"  /* Linked lists */
#include "zmalloc.h" /* total memory usage aware version of malloc/free */
#include "anet.h"    /* Networking the easy way */
#include "ziplist.h" /* Compact list data structure */
#include "intset.h"  /* Compact integer set structure */
#include "version.h" /* Version macro */
#include "util.h"    /* Misc functions useful in many places */
#include "latency.h" /* Latency monitor API */
#include "sparkline.h" /* ASCII graphs API */
#include "quicklist.h"  /* Lists are encoded as linked lists of
                           N-elements flat arrays */
#include "rax.h"     /* Radix tree */

/* Following includes allow test functions to be called from Redis main() */
#include "zipmap.h"
#include "sha1.h"
#include "endianconv.h"
#include "crc64.h"

/* Error codes */
#define C_OK                    0
#define C_ERR                   -1

/* Static server configuration */
#define CONFIG_DEFAULT_DYNAMIC_HZ 1             /* Adapt hz to # of clients.*/
#define CONFIG_DEFAULT_HZ        10             /* Time interrupt calls/sec. */
#define CONFIG_MIN_HZ            1
#define CONFIG_MAX_HZ            500
#define MAX_CLIENTS_PER_CLOCK_TICK 200          /* HZ is adapted based on that. */
#define CONFIG_DEFAULT_SERVER_PORT        6379  /* TCP port. */
#define CONFIG_DEFAULT_TCP_BACKLOG       511    /* TCP listen backlog. */
#define CONFIG_DEFAULT_CLIENT_TIMEOUT       0   /* Default client timeout: infinite */
#define CONFIG_DEFAULT_DBNUM     16
#define CONFIG_DEFAULT_IO_THREADS_NUM 1         /* Single threaded by default */
#define CONFIG_DEFAULT_IO_THREADS_DO_READS 0    /* Read + parse from threads? */
#define CONFIG_MAX_LINE    1024
#define CRON_DBS_PER_CALL 16
#define NET_MAX_WRITES_PER_EVENT (1024*64)
#define PROTO_SHARED_SELECT_CMDS 10
#define OBJ_SHARED_INTEGERS 10000
#define OBJ_SHARED_BULKHDR_LEN 32
#define LOG_MAX_LEN    1024 /* Default maximum length of syslog messages.*/
#define AOF_REWRITE_PERC  100
#define AOF_REWRITE_MIN_SIZE (64*1024*1024)
#define AOF_REWRITE_ITEMS_PER_CMD 64
#define AOF_READ_DIFF_INTERVAL_BYTES (1024*10)
#define CONFIG_DEFAULT_SLOWLOG_LOG_SLOWER_THAN 10000
#define CONFIG_DEFAULT_SLOWLOG_MAX_LEN 128
#define CONFIG_DEFAULT_MAX_CLIENTS 10000
#define CONFIG_AUTHPASS_MAX_LEN 512
#define CONFIG_DEFAULT_SLAVE_PRIORITY 100
#define CONFIG_DEFAULT_REPL_TIMEOUT 60
#define CONFIG_DEFAULT_REPL_PING_SLAVE_PERIOD 10
#define CONFIG_RUN_ID_SIZE 40
#define RDB_EOF_MARK_SIZE 40
#define CONFIG_DEFAULT_REPL_BACKLOG_SIZE (1024*1024)    /* 1mb */
#define CONFIG_DEFAULT_REPL_BACKLOG_TIME_LIMIT (60*60)  /* 1 hour */
#define CONFIG_REPL_BACKLOG_MIN_SIZE (1024*16)          /* 16k */
#define CONFIG_BGSAVE_RETRY_DELAY 5 /* Wait a few secs before trying again. */
#define CONFIG_DEFAULT_PID_FILE "/var/run/redis.pid"
#define CONFIG_DEFAULT_SYSLOG_IDENT "redis"
#define CONFIG_DEFAULT_CLUSTER_CONFIG_FILE "nodes.conf"
#define CONFIG_DEFAULT_CLUSTER_ANNOUNCE_IP NULL         /* Auto detect. */
#define CONFIG_DEFAULT_CLUSTER_ANNOUNCE_PORT 0          /* Use server.port */
#define CONFIG_DEFAULT_CLUSTER_ANNOUNCE_BUS_PORT 0      /* Use +10000 offset. */
#define CONFIG_DEFAULT_DAEMONIZE 0
#define CONFIG_DEFAULT_UNIX_SOCKET_PERM 0
#define CONFIG_DEFAULT_TCP_KEEPALIVE 300
#define CONFIG_DEFAULT_PROTECTED_MODE 1
#define CONFIG_DEFAULT_GOPHER_ENABLED 0
#define CONFIG_DEFAULT_LOGFILE ""
#define CONFIG_DEFAULT_SYSLOG_ENABLED 0
#define CONFIG_DEFAULT_STOP_WRITES_ON_BGSAVE_ERROR 1
#define CONFIG_DEFAULT_RDB_COMPRESSION 1
#define CONFIG_DEFAULT_RDB_CHECKSUM 1
#define CONFIG_DEFAULT_RDB_FILENAME "dump.rdb"
#define CONFIG_DEFAULT_REPL_DISKLESS_SYNC 0
#define CONFIG_DEFAULT_REPL_DISKLESS_SYNC_DELAY 5
#define CONFIG_DEFAULT_RDB_KEY_SAVE_DELAY 0
#define CONFIG_DEFAULT_SLAVE_SERVE_STALE_DATA 1
#define CONFIG_DEFAULT_SLAVE_READ_ONLY 1
#define CONFIG_DEFAULT_SLAVE_IGNORE_MAXMEMORY 1
#define CONFIG_DEFAULT_SLAVE_ANNOUNCE_IP NULL
#define CONFIG_DEFAULT_SLAVE_ANNOUNCE_PORT 0
#define CONFIG_DEFAULT_REPL_DISABLE_TCP_NODELAY 0
#define CONFIG_DEFAULT_MAXMEMORY 0
#define CONFIG_DEFAULT_MAXMEMORY_SAMPLES 5
#define CONFIG_DEFAULT_LFU_LOG_FACTOR 10
#define CONFIG_DEFAULT_LFU_DECAY_TIME 1
#define CONFIG_DEFAULT_AOF_FILENAME "appendonly.aof"
#define CONFIG_DEFAULT_AOF_NO_FSYNC_ON_REWRITE 0
#define CONFIG_DEFAULT_AOF_LOAD_TRUNCATED 1
#define CONFIG_DEFAULT_AOF_USE_RDB_PREAMBLE 1
#define CONFIG_DEFAULT_ACTIVE_REHASHING 1
#define CONFIG_DEFAULT_AOF_REWRITE_INCREMENTAL_FSYNC 1
#define CONFIG_DEFAULT_RDB_SAVE_INCREMENTAL_FSYNC 1
#define CONFIG_DEFAULT_MIN_SLAVES_TO_WRITE 0
#define CONFIG_DEFAULT_MIN_SLAVES_MAX_LAG 10
#define CONFIG_DEFAULT_ACL_FILENAME ""
#define NET_IP_STR_LEN 46 /* INET6_ADDRSTRLEN is 46, but we need to be sure */
#define NET_PEER_ID_LEN (NET_IP_STR_LEN+32) /* Must be enough for ip:port */
#define CONFIG_BINDADDR_MAX 16
#define CONFIG_MIN_RESERVED_FDS 32
#define CONFIG_DEFAULT_LATENCY_MONITOR_THRESHOLD 0
#define CONFIG_DEFAULT_SLAVE_LAZY_FLUSH 0
#define CONFIG_DEFAULT_LAZYFREE_LAZY_EVICTION 0
#define CONFIG_DEFAULT_LAZYFREE_LAZY_EXPIRE 0
#define CONFIG_DEFAULT_LAZYFREE_LAZY_SERVER_DEL 0
#define CONFIG_DEFAULT_ALWAYS_SHOW_LOGO 0
#define CONFIG_DEFAULT_ACTIVE_DEFRAG 0
#define CONFIG_DEFAULT_DEFRAG_THRESHOLD_LOWER 10 /* don't defrag when fragmentation is below 10% */
#define CONFIG_DEFAULT_DEFRAG_THRESHOLD_UPPER 100 /* maximum defrag force at 100% fragmentation */
#define CONFIG_DEFAULT_DEFRAG_IGNORE_BYTES (100<<20) /* don't defrag if frag overhead is below 100mb */
#define CONFIG_DEFAULT_DEFRAG_CYCLE_MIN 5 /* 5% CPU min (at lower threshold) */
#define CONFIG_DEFAULT_DEFRAG_CYCLE_MAX 75 /* 75% CPU max (at upper threshold) */
#define CONFIG_DEFAULT_DEFRAG_MAX_SCAN_FIELDS 1000 /* keys with more than 1000 fields will be processed separately */
#define CONFIG_DEFAULT_PROTO_MAX_BULK_LEN (512ll*1024*1024) /* Bulk request max size */

#define ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP 20 /* Loopkups per loop. */
#define ACTIVE_EXPIRE_CYCLE_FAST_DURATION 1000 /* Microseconds */
#define ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC 25 /* CPU max % for keys collection */
#define ACTIVE_EXPIRE_CYCLE_SLOW 0
#define ACTIVE_EXPIRE_CYCLE_FAST 1

/* Instantaneous metrics tracking. */
#define STATS_METRIC_SAMPLES 16     /* Number of samples per metric. */
#define STATS_METRIC_COMMAND 0      /* Number of commands executed. */
#define STATS_METRIC_NET_INPUT 1    /* Bytes read to network .*/
#define STATS_METRIC_NET_OUTPUT 2   /* Bytes written to network. */
#define STATS_METRIC_COUNT 3

/* Protocol and I/O related defines */
#define PROTO_MAX_QUERYBUF_LEN  (1024*1024*1024) /* 1GB max query buffer. */
#define PROTO_IOBUF_LEN         (1024*16)  /* Generic I/O buffer size */
#define PROTO_REPLY_CHUNK_BYTES (16*1024) /* 16k output buffer */
#define PROTO_INLINE_MAX_SIZE   (1024*64) /* Max size of inline reads */
#define PROTO_MBULK_BIG_ARG     (1024*32)
#define LONG_STR_SIZE      21          /* Bytes needed for long -> str + '\0' */
#define REDIS_AUTOSYNC_BYTES (1024*1024*32) /* fdatasync every 32MB */

#define LIMIT_PENDING_QUERYBUF (4*1024*1024) /* 4mb */

/* When configuring the server eventloop, we setup it so that the total number
 * of file descriptors we can handle are server.maxclients + RESERVED_FDS +
 * a few more to stay safe. Since RESERVED_FDS defaults to 32, we add 96
 * in order to make sure of not over provisioning more than 128 fds. */
#define CONFIG_FDSET_INCR (CONFIG_MIN_RESERVED_FDS+96)

/* Hash table parameters */
#define HASHTABLE_MIN_FILL        10      /* Minimal hash table fill 10% */

/* Command flags. Please check the command table defined in the redis.c file
 * for more information about the meaning of every flag. */
#define CMD_WRITE (1ULL<<0)            /* "write" flag */
#define CMD_READONLY (1ULL<<1)         /* "read-only" flag */
#define CMD_DENYOOM (1ULL<<2)          /* "use-memory" flag */
#define CMD_MODULE (1ULL<<3)           /* Command exported by module. */
#define CMD_ADMIN (1ULL<<4)            /* "admin" flag */
#define CMD_PUBSUB (1ULL<<5)           /* "pub-sub" flag */
#define CMD_NOSCRIPT (1ULL<<6)         /* "no-script" flag */
#define CMD_RANDOM (1ULL<<7)           /* "random" flag */
#define CMD_SORT_FOR_SCRIPT (1ULL<<8)  /* "to-sort" flag */
#define CMD_LOADING (1ULL<<9)          /* "ok-loading" flag */
#define CMD_STALE (1ULL<<10)           /* "ok-stale" flag */
#define CMD_SKIP_MONITOR (1ULL<<11)    /* "no-monitor" flag */
#define CMD_ASKING (1ULL<<12)          /* "cluster-asking" flag */
#define CMD_FAST (1ULL<<13)            /* "fast" flag */

/* Command flags used by the module system. */
#define CMD_MODULE_GETKEYS (1ULL<<14)  /* Use the modules getkeys interface. */
#define CMD_MODULE_NO_CLUSTER (1ULL<<15) /* Deny on Redis Cluster. */

/* Command flags that describe ACLs categories. */
#define CMD_CATEGORY_KEYSPACE (1ULL<<16)
#define CMD_CATEGORY_READ (1ULL<<17)
#define CMD_CATEGORY_WRITE (1ULL<<18)
#define CMD_CATEGORY_SET (1ULL<<19)
#define CMD_CATEGORY_SORTEDSET (1ULL<<20)
#define CMD_CATEGORY_LIST (1ULL<<21)
#define CMD_CATEGORY_HASH (1ULL<<22)
#define CMD_CATEGORY_STRING (1ULL<<23)
#define CMD_CATEGORY_BITMAP (1ULL<<24)
#define CMD_CATEGORY_HYPERLOGLOG (1ULL<<25)
#define CMD_CATEGORY_GEO (1ULL<<26)
#define CMD_CATEGORY_STREAM (1ULL<<27)
#define CMD_CATEGORY_PUBSUB (1ULL<<28)
#define CMD_CATEGORY_ADMIN (1ULL<<29)
#define CMD_CATEGORY_FAST (1ULL<<30)
#define CMD_CATEGORY_SLOW (1ULL<<31)
#define CMD_CATEGORY_BLOCKING (1ULL<<32)
#define CMD_CATEGORY_DANGEROUS (1ULL<<33)
#define CMD_CATEGORY_CONNECTION (1ULL<<34)
#define CMD_CATEGORY_TRANSACTION (1ULL<<35)
#define CMD_CATEGORY_SCRIPTING (1ULL<<36)

/* AOF states */
#define AOF_OFF 0             /* AOF is off */
#define AOF_ON 1              /* AOF is on */
#define AOF_WAIT_REWRITE 2    /* AOF waits rewrite to start appending */

/* Client flags */
#define CLIENT_SLAVE (1<<0)   /* This client is a repliaca */
#define CLIENT_MASTER (1<<1)  /* This client is a master */
#define CLIENT_MONITOR (1<<2) /* This client is a slave monitor, see MONITOR */
#define CLIENT_MULTI (1<<3)   /* This client is in a MULTI context */
#define CLIENT_BLOCKED (1<<4) /* The client is waiting in a blocking operation */
#define CLIENT_DIRTY_CAS (1<<5) /* Watched keys modified. EXEC will fail. */
#define CLIENT_CLOSE_AFTER_REPLY (1<<6) /* Close after writing entire reply. */
#define CLIENT_UNBLOCKED (1<<7) /* This client was unblocked and is stored in
                                  server.unblocked_clients */
#define CLIENT_LUA (1<<8) /* This is a non connected client used by Lua */
#define CLIENT_ASKING (1<<9)     /* Client issued the ASKING command */
#define CLIENT_CLOSE_ASAP (1<<10)/* Close this client ASAP */
#define CLIENT_UNIX_SOCKET (1<<11) /* Client connected via Unix domain socket */
#define CLIENT_DIRTY_EXEC (1<<12)  /* EXEC will fail for errors while queueing */
#define CLIENT_MASTER_FORCE_REPLY (1<<13)  /* Queue replies even if is master */
#define CLIENT_FORCE_AOF (1<<14)   /* Force AOF propagation of current cmd. */
#define CLIENT_FORCE_REPL (1<<15)  /* Force replication of current cmd. */
#define CLIENT_PRE_PSYNC (1<<16)   /* Instance don't understand PSYNC. */
#define CLIENT_READONLY (1<<17)    /* Cluster client is in read-only state. */
#define CLIENT_PUBSUB (1<<18)      /* Client is in Pub/Sub mode. */
#define CLIENT_PREVENT_AOF_PROP (1<<19)  /* Don't propagate to AOF. */
#define CLIENT_PREVENT_REPL_PROP (1<<20)  /* Don't propagate to slaves. */
#define CLIENT_PREVENT_PROP (CLIENT_PREVENT_AOF_PROP|CLIENT_PREVENT_REPL_PROP)
#define CLIENT_PENDING_WRITE (1<<21) /* Client has output to send but a write
                                        handler is yet not installed. */
#define CLIENT_REPLY_OFF (1<<22)   /* Don't send replies to client. */
#define CLIENT_REPLY_SKIP_NEXT (1<<23)  /* Set CLIENT_REPLY_SKIP for next cmd */
#define CLIENT_REPLY_SKIP (1<<24)  /* Don't send just this reply. */
#define CLIENT_LUA_DEBUG (1<<25)  /* Run EVAL in debug mode. */
#define CLIENT_LUA_DEBUG_SYNC (1<<26)  /* EVAL debugging without fork() */
#define CLIENT_MODULE (1<<27) /* Non connected client used by some module. */
#define CLIENT_PROTECTED (1<<28) /* Client should not be freed for now. */
#define CLIENT_PENDING_READ (1<<29) /* The client has pending reads and was put
                                       in the list of clients we can read
                                       from. */
#define CLIENT_PENDING_COMMAND (1<<30) /* Used in threaded I/O to signal after
                                          we return single threaded that the
                                          client has already pending commands
                                          to be executed. */
#define CLIENT_TRACKING (1<<31) /* Client enabled keys tracking in order to
                                   perform client side caching. */
#define CLIENT_TRACKING_BROKEN_REDIR (1ULL<<32) /* Target client is invalid. */

/* Client block type (btype field in client structure)
 * if CLIENT_BLOCKED flag is set. */
#define BLOCKED_NONE 0    /* Not blocked, no CLIENT_BLOCKED flag set. */
#define BLOCKED_LIST 1    /* BLPOP & co. */
#define BLOCKED_WAIT 2    /* WAIT for synchronous replication. */
#define BLOCKED_MODULE 3  /* Blocked by a loadable module. */
#define BLOCKED_STREAM 4  /* XREAD. */
#define BLOCKED_ZSET 5    /* BZPOP et al. */
#define BLOCKED_NUM 6     /* Number of blocked states. */

/* Client request types */
#define PROTO_REQ_INLINE 1
#define PROTO_REQ_MULTIBULK 2

/* Client classes for client limits, currently used only for
 * the max-client-output-buffer limit implementation. */
#define CLIENT_TYPE_NORMAL 0 /* Normal req-reply clients + MONITORs */
#define CLIENT_TYPE_SLAVE 1  /* Slaves. */
#define CLIENT_TYPE_PUBSUB 2 /* Clients subscribed to PubSub channels. */
#define CLIENT_TYPE_MASTER 3 /* Master. */
#define CLIENT_TYPE_OBUF_COUNT 3 /* Number of clients to expose to output
                                    buffer configuration. Just the first
                                    three: normal, slave, pubsub. */

/* Slave replication state. Used in server.repl_state for slaves to remember
 * what to do next. */
#define REPL_STATE_NONE 0 /* No active replication */
#define REPL_STATE_CONNECT 1 /* Must connect to master */
#define REPL_STATE_CONNECTING 2 /* Connecting to master */
/* --- Handshake states, must be ordered --- */
#define REPL_STATE_RECEIVE_PONG 3 /* Wait for PING reply */
#define REPL_STATE_SEND_AUTH 4 /* Send AUTH to master */
#define REPL_STATE_RECEIVE_AUTH 5 /* Wait for AUTH reply */
#define REPL_STATE_SEND_PORT 6 /* Send REPLCONF listening-port */
#define REPL_STATE_RECEIVE_PORT 7 /* Wait for REPLCONF reply */
#define REPL_STATE_SEND_IP 8 /* Send REPLCONF ip-address */
#define REPL_STATE_RECEIVE_IP 9 /* Wait for REPLCONF reply */
#define REPL_STATE_SEND_CAPA 10 /* Send REPLCONF capa */
#define REPL_STATE_RECEIVE_CAPA 11 /* Wait for REPLCONF reply */
#define REPL_STATE_SEND_PSYNC 12 /* Send PSYNC */
#define REPL_STATE_RECEIVE_PSYNC 13 /* Wait for PSYNC reply */
/* --- End of handshake states --- */
#define REPL_STATE_TRANSFER 14 /* Receiving .rdb from master */
#define REPL_STATE_CONNECTED 15 /* Connected to master */

/* State of slaves from the POV of the master. Used in client->replstate.
 * In SEND_BULK and ONLINE state the slave receives new updates
 * in its output queue. In the WAIT_BGSAVE states instead the server is waiting
 * to start the next background saving in order to send updates to it. */
#define SLAVE_STATE_WAIT_BGSAVE_START 6 /* We need to produce a new RDB file. */
#define SLAVE_STATE_WAIT_BGSAVE_END 7 /* Waiting RDB file creation to finish. */
#define SLAVE_STATE_SEND_BULK 8 /* Sending RDB file to slave. */
#define SLAVE_STATE_ONLINE 9 /* RDB file transmitted, sending just updates. */

/* Slave capabilities. */
#define SLAVE_CAPA_NONE 0
#define SLAVE_CAPA_EOF (1<<0)    /* Can parse the RDB EOF streaming format. */
#define SLAVE_CAPA_PSYNC2 (1<<1) /* Supports PSYNC2 protocol. */

/* Synchronous read timeout - slave side */
#define CONFIG_REPL_SYNCIO_TIMEOUT 5

/* List related stuff */
#define LIST_HEAD 0
#define LIST_TAIL 1
#define ZSET_MIN 0
#define ZSET_MAX 1

/* Sort operations */
#define SORT_OP_GET 0

/* Log levels */
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define LL_RAW (1<<10) /* Modifier to log without timestamp */
#define CONFIG_DEFAULT_VERBOSITY LL_NOTICE

/* Supervision options */
#define SUPERVISED_NONE 0
#define SUPERVISED_AUTODETECT 1
#define SUPERVISED_SYSTEMD 2
#define SUPERVISED_UPSTART 3

/* Anti-warning macro... */
#define UNUSED(V) ((void) V)

#define ZSKIPLIST_MAXLEVEL 64 /* Should be enough for 2^64 elements */
#define ZSKIPLIST_P 0.25      /* Skiplist P = 1/4 */

/* Append only defines */
#define AOF_FSYNC_NO 0
#define AOF_FSYNC_ALWAYS 1
#define AOF_FSYNC_EVERYSEC 2
#define CONFIG_DEFAULT_AOF_FSYNC AOF_FSYNC_EVERYSEC

/* Replication diskless load defines */
#define REPL_DISKLESS_LOAD_DISABLED 0
#define REPL_DISKLESS_LOAD_WHEN_DB_EMPTY 1
#define REPL_DISKLESS_LOAD_SWAPDB 2
#define CONFIG_DEFAULT_REPL_DISKLESS_LOAD REPL_DISKLESS_LOAD_DISABLED

/* Zipped structures related defaults */
#define OBJ_HASH_MAX_ZIPLIST_ENTRIES 512
#define OBJ_HASH_MAX_ZIPLIST_VALUE 64
#define OBJ_SET_MAX_INTSET_ENTRIES 512
#define OBJ_ZSET_MAX_ZIPLIST_ENTRIES 128
#define OBJ_ZSET_MAX_ZIPLIST_VALUE 64
#define OBJ_STREAM_NODE_MAX_BYTES 4096
#define OBJ_STREAM_NODE_MAX_ENTRIES 100

/* List defaults */
#define OBJ_LIST_MAX_ZIPLIST_SIZE -2
#define OBJ_LIST_COMPRESS_DEPTH 0

/* HyperLogLog defines */
#define CONFIG_DEFAULT_HLL_SPARSE_MAX_BYTES 3000

/* Sets operations codes */
#define SET_OP_UNION 0
#define SET_OP_DIFF 1
#define SET_OP_INTER 2

/* Redis maxmemory strategies. Instead of using just incremental number
 * for this defines, we use a set of flags so that testing for certain
 * properties common to multiple policies is faster. */
#define MAXMEMORY_FLAG_LRU (1<<0)
#define MAXMEMORY_FLAG_LFU (1<<1)
#define MAXMEMORY_FLAG_ALLKEYS (1<<2)
#define MAXMEMORY_FLAG_NO_SHARED_INTEGERS \
    (MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_LFU)

#define MAXMEMORY_VOLATILE_LRU ((0<<8)|MAXMEMORY_FLAG_LRU)
#define MAXMEMORY_VOLATILE_LFU ((1<<8)|MAXMEMORY_FLAG_LFU)
#define MAXMEMORY_VOLATILE_TTL (2<<8)
#define MAXMEMORY_VOLATILE_RANDOM (3<<8)
#define MAXMEMORY_ALLKEYS_LRU ((4<<8)|MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_ALLKEYS_LFU ((5<<8)|MAXMEMORY_FLAG_LFU|MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_ALLKEYS_RANDOM ((6<<8)|MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_NO_EVICTION (7<<8)

#define CONFIG_DEFAULT_MAXMEMORY_POLICY MAXMEMORY_NO_EVICTION

/* Scripting */
#define LUA_SCRIPT_TIME_LIMIT 5000 /* milliseconds */

/* Units */
#define UNIT_SECONDS 0
#define UNIT_MILLISECONDS 1

/* SHUTDOWN flags */
#define SHUTDOWN_NOFLAGS 0      /* No flags. */
#define SHUTDOWN_SAVE 1         /* Force SAVE on SHUTDOWN even if no save
                                   points are configured. */
#define SHUTDOWN_NOSAVE 2       /* Don't SAVE on SHUTDOWN. */

/* Command call flags, see call() function */
#define CMD_CALL_NONE 0
#define CMD_CALL_SLOWLOG (1<<0)
#define CMD_CALL_STATS (1<<1)
#define CMD_CALL_PROPAGATE_AOF (1<<2)
#define CMD_CALL_PROPAGATE_REPL (1<<3)
#define CMD_CALL_PROPAGATE (CMD_CALL_PROPAGATE_AOF|CMD_CALL_PROPAGATE_REPL)
#define CMD_CALL_FULL (CMD_CALL_SLOWLOG | CMD_CALL_STATS | CMD_CALL_PROPAGATE)

/* Command propagation flags, see propagate() function */
#define PROPAGATE_NONE 0
#define PROPAGATE_AOF 1
#define PROPAGATE_REPL 2

/* RDB active child save type. */
#define RDB_CHILD_TYPE_NONE 0
#define RDB_CHILD_TYPE_DISK 1     /* RDB is written to disk. */
#define RDB_CHILD_TYPE_SOCKET 2   /* RDB is written to slave socket. */

/* Keyspace changes notification classes. Every class is associated with a
 * character for configuration purposes. */
#define NOTIFY_KEYSPACE (1<<0)    /* K */
#define NOTIFY_KEYEVENT (1<<1)    /* E */
#define NOTIFY_GENERIC (1<<2)     /* g */
#define NOTIFY_STRING (1<<3)      /* $ */
#define NOTIFY_LIST (1<<4)        /* l */
#define NOTIFY_SET (1<<5)         /* s */
#define NOTIFY_HASH (1<<6)        /* h */
#define NOTIFY_ZSET (1<<7)        /* z */
#define NOTIFY_EXPIRED (1<<8)     /* x */
#define NOTIFY_EVICTED (1<<9)     /* e */
#define NOTIFY_STREAM (1<<10)     /* t */
#define NOTIFY_KEY_MISS (1<<11)   /* m */
#define NOTIFY_ALL (NOTIFY_GENERIC | NOTIFY_STRING | NOTIFY_LIST | NOTIFY_SET | NOTIFY_HASH | NOTIFY_ZSET | NOTIFY_EXPIRED | NOTIFY_EVICTED | NOTIFY_STREAM | NOTIFY_KEY_MISS) /* A flag */

/* Get the first bind addr or NULL */
#define NET_FIRST_BIND_ADDR (server.bindaddr_count ? server.bindaddr[0] : NULL)

/* Using the following macro you can run code inside serverCron() with the
 * specified period, specified in milliseconds.
 * The actual resolution depends on server.hz. */
#define run_with_period(_ms_) if ((_ms_ <= 1000/server.hz) || !(server.cronloops%((_ms_)/(1000/server.hz))))

/* We can print the stacktrace, so our assert is defined this way: */
#define serverAssertWithInfo(_c,_o,_e) ((_e)?(void)0 : (_serverAssertWithInfo(_c,_o,#_e,__FILE__,__LINE__),_exit(1)))
#define serverAssert(_e) ((_e)?(void)0 : (_serverAssert(#_e,__FILE__,__LINE__),_exit(1)))
#define serverPanic(...) _serverPanic(__FILE__,__LINE__,__VA_ARGS__),_exit(1)

/*-----------------------------------------------------------------------------
 * Data types
 *----------------------------------------------------------------------------*/

/* A redis object, that is a type able to hold a string / list / set */

/* The actual Redis Object */
#define OBJ_STRING 0    /* String object. */
#define OBJ_LIST 1      /* List object. */
#define OBJ_SET 2       /* Set object. */
#define OBJ_ZSET 3      /* Sorted set object. */
#define OBJ_HASH 4      /* Hash object. */

/* The "module" object type is a special one that signals that the object
 * is one directly managed by a Redis module. In this case the value points
 * to a moduleValue struct, which contains the object value (which is only
 * handled by the module itself) and the RedisModuleType struct which lists
 * function pointers in order to serialize, deserialize, AOF-rewrite and
 * free the object.
 *
 * Inside the RDB file, module types are encoded as OBJ_MODULE followed
 * by a 64 bit module type ID, which has a 54 bits module-specific signature
 * in order to dispatch the loading to the right module, plus a 10 bits
 * encoding version. */
#define OBJ_MODULE 5    /* Module object. */
#define OBJ_STREAM 6    /* Stream object. */

/* Extract encver / signature from a module type ID. */
#define REDISMODULE_TYPE_ENCVER_BITS 10
#define REDISMODULE_TYPE_ENCVER_MASK ((1<<REDISMODULE_TYPE_ENCVER_BITS)-1)
#define REDISMODULE_TYPE_ENCVER(id) (id & REDISMODULE_TYPE_ENCVER_MASK)
#define REDISMODULE_TYPE_SIGN(id) ((id & ~((uint64_t)REDISMODULE_TYPE_ENCVER_MASK)) >>REDISMODULE_TYPE_ENCVER_BITS)

struct RedisModule;
struct RedisModuleIO;
struct RedisModuleDigest;
struct RedisModuleCtx;
struct redisObject;

/* Each module type implementation should export a set of methods in order
 * to serialize and deserialize the value in the RDB file, rewrite the AOF
 * log, create the digest for "DEBUG DIGEST", and free the value when a key
 * is deleted. */
typedef void *(*moduleTypeLoadFunc)(struct RedisModuleIO *io, int encver);
typedef void (*moduleTypeSaveFunc)(struct RedisModuleIO *io, void *value);
typedef void (*moduleTypeRewriteFunc)(struct RedisModuleIO *io, struct redisObject *key, void *value);
typedef void (*moduleTypeDigestFunc)(struct RedisModuleDigest *digest, void *value);
typedef size_t (*moduleTypeMemUsageFunc)(const void *value);
typedef void (*moduleTypeFreeFunc)(void *value);

/* The module type, which is referenced in each value of a given type, defines
 * the methods and links to the module exporting the type. */
typedef struct RedisModuleType {
    uint64_t id; /* Higher 54 bits of type ID + 10 lower bits of encoding ver. */
    struct RedisModule *module;
    moduleTypeLoadFunc rdb_load;
    moduleTypeSaveFunc rdb_save;
    moduleTypeRewriteFunc aof_rewrite;
    moduleTypeMemUsageFunc mem_usage;
    moduleTypeDigestFunc digest;
    moduleTypeFreeFunc free;
    char name[10]; /* 9 bytes name + null term. Charset: A-Z a-z 0-9 _- */
} moduleType;

/* In Redis objects 'robj' structures of type OBJ_MODULE, the value pointer
 * is set to the following structure, referencing the moduleType structure
 * in order to work with the value, and at the same time providing a raw
 * pointer to the value, as created by the module commands operating with
 * the module type.
 *
 * So for example in order to free such a value, it is possible to use
 * the following code:
 *
 *  if (robj->type == OBJ_MODULE) {
 *      moduleValue *mt = robj->ptr;
 *      mt->type->free(mt->value);
 *      zfree(mt); // We need to release this in-the-middle struct as well.
 *  }
 */
typedef struct moduleValue {
    moduleType *type;
    void *value;
} moduleValue;

/* This is a wrapper for the 'rio' streams used inside rdb.c in Redis, so that
 * the user does not have to take the total count of the written bytes nor
 * to care about error conditions. */
typedef struct RedisModuleIO {
    size_t bytes;       /* Bytes read / written so far. */
    rio *rio;           /* Rio stream. */
    moduleType *type;   /* Module type doing the operation. */
    int error;          /* True if error condition happened. */
    int ver;            /* Module serialization version: 1 (old),
                         * 2 (current version with opcodes annotation). */
    struct RedisModuleCtx *ctx; /* Optional context, see RM_GetContextFromIO()*/
    struct redisObject *key;    /* Optional name of key processed */
} RedisModuleIO;

/* Macro to initialize an IO context. Note that the 'ver' field is populated
 * inside rdb.c according to the version of the value to load. */
#define moduleInitIOContext(iovar,mtype,rioptr,keyptr) do { \
    iovar.rio = rioptr; \
    iovar.type = mtype; \
    iovar.bytes = 0; \
    iovar.error = 0; \
    iovar.ver = 0; \
    iovar.key = keyptr; \
    iovar.ctx = NULL; \
} while(0);

/* This is a structure used to export DEBUG DIGEST capabilities to Redis
 * modules. We want to capture both the ordered and unordered elements of
 * a data structure, so that a digest can be created in a way that correctly
 * reflects the values. See the DEBUG DIGEST command implementation for more
 * background. */
typedef struct RedisModuleDigest {
    unsigned char o[20];    /* Ordered elements. */
    unsigned char x[20];    /* Xored elements. */
} RedisModuleDigest;

/* Just start with a digest composed of all zero bytes. */
#define moduleInitDigestContext(mdvar) do { \
    memset(mdvar.o,0,sizeof(mdvar.o)); \
    memset(mdvar.x,0,sizeof(mdvar.x)); \
} while(0);

/* Objects encoding. Some kind of objects like Strings and Hashes can be
 * internally represented in multiple ways. The 'encoding' field of the object
 * is set to one of this fields for this object. */
#define OBJ_ENCODING_RAW 0     /* Raw representation */
#define OBJ_ENCODING_INT 1     /* Encoded as integer */
#define OBJ_ENCODING_HT 2      /* Encoded as hash table */
#define OBJ_ENCODING_ZIPMAP 3  /* Encoded as zipmap */
#define OBJ_ENCODING_LINKEDLIST 4 /* No longer used: old list encoding. */
#define OBJ_ENCODING_ZIPLIST 5 /* Encoded as ziplist */
#define OBJ_ENCODING_INTSET 6  /* Encoded as intset */
#define OBJ_ENCODING_SKIPLIST 7  /* Encoded as skiplist */
#define OBJ_ENCODING_EMBSTR 8  /* Embedded sds string encoding */
#define OBJ_ENCODING_QUICKLIST 9 /* Encoded as linked list of ziplists */
#define OBJ_ENCODING_STREAM 10 /* Encoded as a radix tree of listpacks */

#define LRU_BITS 24
#define LRU_CLOCK_MAX ((1<<LRU_BITS)-1) /* Max value of obj->lru */
#define LRU_CLOCK_RESOLUTION 1000 /* LRU clock resolution in ms */

#define OBJ_SHARED_REFCOUNT INT_MAX
typedef struct redisObject {
    unsigned type:4;
    unsigned encoding:4;
    unsigned lru:LRU_BITS; /* LRU time (relative to global lru_clock) or
                            * LFU data (least significant 8 bits frequency
                            * and most significant 16 bits access time). */
    int refcount;
    void *ptr;
} robj;

/* The a string name for an object's type as listed above
 * Native types are checked against the OBJ_STRING, OBJ_LIST, OBJ_* defines,
 * and Module types have their registered name returned. */
char *getObjectTypeName(robj*);

/* Macro used to initialize a Redis object allocated on the stack.
 * Note that this macro is taken near the structure definition to make sure
 * we'll update it when the structure is changed, to avoid bugs like
 * bug #85 introduced exactly in this way. */
#define initStaticStringObject(_var,_ptr) do { \
    _var.refcount = 1; \
    _var.type = OBJ_STRING; \
    _var.encoding = OBJ_ENCODING_RAW; \
    _var.ptr = _ptr; \
} while(0)

struct evictionPoolEntry; /* Defined in evict.c */

/* This structure is used in order to represent the output buffer of a client,
 * which is actually a linked list of blocks like that, that is: client->reply. */
typedef struct clientReplyBlock {
    size_t size, used;
    char buf[];
} clientReplyBlock;

/* Redis database representation. There are multiple databases identified
 * by integers from 0 (the default database) up to the max configured
 * database. The database number is the 'id' field in the structure. */
typedef struct redisDb {
    dict *dict;                 /* The keyspace for this DB */
    dict *expires;              /* Timeout of keys with a timeout set */
    dict *blocking_keys;        /* Keys with clients waiting for data (BLPOP)*/
    dict *ready_keys;           /* Blocked keys that received a PUSH */
    dict *watched_keys;         /* WATCHED keys for MULTI/EXEC CAS */
    int id;                     /* Database ID */
    long long avg_ttl;          /* Average TTL, just for stats */
    list *defrag_later;         /* List of key names to attempt to defrag one by one, gradually. */
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
    int cmd_flags;          /* The accumulated command flags OR-ed together.
                               So if at least a command has a given flag, it
                               will be set in this field. */
    int minreplicas;        /* MINREPLICAS for synchronous replication */
    time_t minreplicas_timeout; /* MINREPLICAS timeout as unixtime. */
} multiState;

/* This structure holds the blocking operation state for a client.
 * The fields used depend on client->btype. */
typedef struct blockingState {
    /* Generic fields. */
    mstime_t timeout;       /* Blocking operation timeout. If UNIX current time
                             * is > timeout then the operation timed out. */

    /* BLOCKED_LIST, BLOCKED_ZSET and BLOCKED_STREAM */
    dict *keys;             /* The keys we are waiting to terminate a blocking
                             * operation such as BLPOP or XREAD. Or NULL. */
    robj *target;           /* The key that should receive the element,
                             * for BRPOPLPUSH. */

    /* BLOCK_STREAM */
    size_t xread_count;     /* XREAD COUNT option. */
    robj *xread_group;      /* XREADGROUP group name. */
    robj *xread_consumer;   /* XREADGROUP consumer name. */
    mstime_t xread_retry_time, xread_retry_ttl;
    int xread_group_noack;

    /* BLOCKED_WAIT */
    int numreplicas;        /* Number of replicas we are waiting for ACK. */
    long long reploffset;   /* Replication offset to reach. */

    /* BLOCKED_MODULE */
    void *module_blocked_handle; /* RedisModuleBlockedClient structure.
                                    which is opaque for the Redis core, only
                                    handled in module.c. */
} blockingState;

/* The following structure represents a node in the server.ready_keys list,
 * where we accumulate all the keys that had clients blocked with a blocking
 * operation such as B[LR]POP, but received new data in the context of the
 * last executed command.
 *
 * After the execution of every command or script, we run this list to check
 * if as a result we should serve data to clients blocked, unblocking them.
 * Note that server.ready_keys will not have duplicates as there dictionary
 * also called ready_keys in every structure representing a Redis database,
 * where we make sure to remember if a given key was already added in the
 * server.ready_keys list. */
typedef struct readyList {
    redisDb *db;
    robj *key;
} readyList;

/* This structure represents a Redis user. This is useful for ACLs, the
 * user is associated to the connection after the connection is authenticated.
 * If there is no associated user, the connection uses the default user. */
#define USER_COMMAND_BITS_COUNT 1024    /* The total number of command bits
                                           in the user structure. The last valid
                                           command ID we can set in the user
                                           is USER_COMMAND_BITS_COUNT-1. */
#define USER_FLAG_ENABLED (1<<0)        /* The user is active. */
#define USER_FLAG_DISABLED (1<<1)       /* The user is disabled. */
#define USER_FLAG_ALLKEYS (1<<2)        /* The user can mention any key. */
#define USER_FLAG_ALLCOMMANDS (1<<3)    /* The user can run all commands. */
#define USER_FLAG_NOPASS      (1<<4)    /* The user requires no password, any
                                           provided password will work. For the
                                           default user, this also means that
                                           no AUTH is needed, and every
                                           connection is immediately
                                           authenticated. */
typedef struct user {
    sds name;       /* The username as an SDS string. */
    uint64_t flags; /* See USER_FLAG_* */

    /* The bit in allowed_commands is set if this user has the right to
     * execute this command. In commands having subcommands, if this bit is
     * set, then all the subcommands are also available.
     *
     * If the bit for a given command is NOT set and the command has
     * subcommands, Redis will also check allowed_subcommands in order to
     * understand if the command can be executed. */
    uint64_t allowed_commands[USER_COMMAND_BITS_COUNT/64];

    /* This array points, for each command ID (corresponding to the command
     * bit set in allowed_commands), to an array of SDS strings, terminated by
     * a NULL pointer, with all the sub commands that can be executed for
     * this command. When no subcommands matching is used, the field is just
     * set to NULL to avoid allocating USER_COMMAND_BITS_COUNT pointers. */
    sds **allowed_subcommands;
    list *passwords; /* A list of SDS valid passwords for this user. */
    list *patterns;  /* A list of allowed key patterns. If this field is NULL
                        the user cannot mention any key in a command, unless
                        the flag ALLKEYS is set in the user. */
} user;

/* With multiplexing we need to take per-client state.
 * Clients are taken in a linked list. */
typedef struct client {
    uint64_t id;            /* Client incremental unique ID. */
    int fd;                 /* Client socket. */
    int resp;               /* RESP protocol version. Can be 2 or 3. */
    redisDb *db;            /* Pointer to currently SELECTed DB. */
    robj *name;             /* As set by CLIENT SETNAME. */
    sds querybuf;           /* Buffer we use to accumulate client queries. */
    size_t qb_pos;          /* The position we have read in querybuf. */
    sds pending_querybuf;   /* If this client is flagged as master, this buffer
                               represents the yet not applied portion of the
                               replication stream that we are receiving from
                               the master. */
    size_t querybuf_peak;   /* Recent (100ms or more) peak of querybuf size. */
    int argc;               /* Num of arguments of current command. */
    robj **argv;            /* Arguments of current command. */
    struct redisCommand *cmd, *lastcmd;  /* Last command executed. */
    user *user;             /* User associated with this connection. If the
                               user is set to NULL the connection can do
                               anything (admin). */
    int reqtype;            /* Request protocol type: PROTO_REQ_* */
    int multibulklen;       /* Number of multi bulk arguments left to read. */
    long bulklen;           /* Length of bulk argument in multi bulk request. */
    list *reply;            /* List of reply objects to send to the client. */
    unsigned long long reply_bytes; /* Tot bytes of objects in reply list. */
    size_t sentlen;         /* Amount of bytes already sent in the current
                               buffer or object being sent. */
    time_t ctime;           /* Client creation time. */
    time_t lastinteraction; /* Time of the last interaction, used for timeout */
    time_t obuf_soft_limit_reached_time;
    uint64_t flags;         /* Client flags: CLIENT_* macros. */
    int authenticated;      /* Needed when the default user requires auth. */
    int replstate;          /* Replication state if this is a slave. */
    int repl_put_online_on_ack; /* Install slave write handler on ACK. */
    int repldbfd;           /* Replication DB file descriptor. */
    off_t repldboff;        /* Replication DB file offset. */
    off_t repldbsize;       /* Replication DB file size. */
    sds replpreamble;       /* Replication DB preamble. */
    long long read_reploff; /* Read replication offset if this is a master. */
    long long reploff;      /* Applied replication offset if this is a master. */
    long long repl_ack_off; /* Replication ack offset, if this is a slave. */
    long long repl_ack_time;/* Replication ack time, if this is a slave. */
    long long psync_initial_offset; /* FULLRESYNC reply offset other slaves
                                       copying this slave output buffer
                                       should use. */
    char replid[CONFIG_RUN_ID_SIZE+1]; /* Master replication ID (if master). */
    int slave_listening_port; /* As configured with: SLAVECONF listening-port */
    char slave_ip[NET_IP_STR_LEN]; /* Optionally given by REPLCONF ip-address */
    int slave_capa;         /* Slave capabilities: SLAVE_CAPA_* bitwise OR. */
    multiState mstate;      /* MULTI/EXEC state */
    int btype;              /* Type of blocking op if CLIENT_BLOCKED. */
    blockingState bpop;     /* blocking state */
    long long woff;         /* Last write global replication offset. */
    list *watched_keys;     /* Keys WATCHED for MULTI/EXEC CAS */
    dict *pubsub_channels;  /* channels a client is interested in (SUBSCRIBE) */
    list *pubsub_patterns;  /* patterns a client is interested in (SUBSCRIBE) */
    sds peerid;             /* Cached peer ID. */
    listNode *client_list_node; /* list node in client list */

    /* If this client is in tracking mode and this field is non zero,
     * invalidation messages for keys fetched by this client will be send to
     * the specified client ID. */
    uint64_t client_tracking_redirection;

    /* Response buffer */
    int bufpos;
    char buf[PROTO_REPLY_CHUNK_BYTES];
} client;

struct saveparam {
    time_t seconds;
    int changes;
};

struct moduleLoadQueueEntry {
    sds path;
    int argc;
    robj **argv;
};

struct sharedObjectsStruct {
    robj *crlf, *ok, *err, *emptybulk, *czero, *cone, *pong, *space,
    *colon, *queued, *null[4], *nullarray[4],
    *emptyarray, *wrongtypeerr, *nokeyerr, *syntaxerr, *sameobjecterr,
    *outofrangeerr, *noscripterr, *loadingerr, *slowscripterr, *bgsaveerr,
    *masterdownerr, *roslaveerr, *execaborterr, *noautherr, *noreplicaserr,
    *busykeyerr, *oomerr, *plus, *messagebulk, *pmessagebulk, *subscribebulk,
    *unsubscribebulk, *psubscribebulk, *punsubscribebulk, *del, *unlink,
    *rpop, *lpop, *lpush, *rpoplpush, *zpopmin, *zpopmax, *emptyscan,
    *select[PROTO_SHARED_SELECT_CMDS],
    *integers[OBJ_SHARED_INTEGERS],
    *mbulkhdr[OBJ_SHARED_BULKHDR_LEN], /* "*<value>\r\n" */
    *bulkhdr[OBJ_SHARED_BULKHDR_LEN];  /* "$<value>\r\n" */
    sds minstring, maxstring;
};

/* ZSETs use a specialized version of Skiplists */
typedef struct zskiplistNode {
    sds ele;
    double score;
    struct zskiplistNode *backward;
    struct zskiplistLevel {
        struct zskiplistNode *forward;
        unsigned long span;
    } level[];
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

typedef struct clientBufferLimitsConfig {
    unsigned long long hard_limit_bytes;
    unsigned long long soft_limit_bytes;
    time_t soft_limit_seconds;
} clientBufferLimitsConfig;

extern clientBufferLimitsConfig clientBufferLimitsDefaults[CLIENT_TYPE_OBUF_COUNT];

/* The redisOp structure defines a Redis Operation, that is an instance of
 * a command with an argument vector, database ID, propagation target
 * (PROPAGATE_*), and command pointer.
 *
 * Currently only used to additionally propagate more commands to AOF/Replication
 * after the propagation of the executed command. */
typedef struct redisOp {
    robj **argv;
    int argc, dbid, target;
    struct redisCommand *cmd;
} redisOp;

/* Defines an array of Redis operations. There is an API to add to this
 * structure in a easy way.
 *
 * redisOpArrayInit();
 * redisOpArrayAppend();
 * redisOpArrayFree();
 */
typedef struct redisOpArray {
    redisOp *ops;
    int numops;
} redisOpArray;

/* This structure is returned by the getMemoryOverheadData() function in
 * order to return memory overhead information. */
struct redisMemOverhead {
    size_t peak_allocated;
    size_t total_allocated;
    size_t startup_allocated;
    size_t repl_backlog;
    size_t clients_slaves;
    size_t clients_normal;
    size_t aof_buffer;
    size_t lua_caches;
    size_t overhead_total;
    size_t dataset;
    size_t total_keys;
    size_t bytes_per_key;
    float dataset_perc;
    float peak_perc;
    float total_frag;
    ssize_t total_frag_bytes;
    float allocator_frag;
    ssize_t allocator_frag_bytes;
    float allocator_rss;
    ssize_t allocator_rss_bytes;
    float rss_extra;
    size_t rss_extra_bytes;
    size_t num_dbs;
    struct {
        size_t dbid;
        size_t overhead_ht_main;
        size_t overhead_ht_expires;
    } *db;
};

/* This structure can be optionally passed to RDB save/load functions in
 * order to implement additional functionalities, by storing and loading
 * metadata to the RDB file.
 *
 * Currently the only use is to select a DB at load time, useful in
 * replication in order to make sure that chained slaves (slaves of slaves)
 * select the correct DB and are able to accept the stream coming from the
 * top-level master. */
typedef struct rdbSaveInfo {
    /* Used saving and loading. */
    int repl_stream_db;  /* DB to select in server.master client. */

    /* Used only loading. */
    int repl_id_is_set;  /* True if repl_id field is set. */
    char repl_id[CONFIG_RUN_ID_SIZE+1];     /* Replication ID. */
    long long repl_offset;                  /* Replication offset. */
} rdbSaveInfo;

#define RDB_SAVE_INFO_INIT {-1,0,"000000000000000000000000000000",-1}

struct malloc_stats {
    size_t zmalloc_used;
    size_t process_rss;
    size_t allocator_allocated;
    size_t allocator_active;
    size_t allocator_resident;
};

/*-----------------------------------------------------------------------------
 * Global server state
 *----------------------------------------------------------------------------*/

struct clusterState;

/* AIX defines hz to __hz, we don't use this define and in order to allow
 * Redis build on AIX we need to undef it. */
#ifdef _AIX
#undef hz
#endif

#define CHILD_INFO_MAGIC 0xC17DDA7A12345678LL
#define CHILD_INFO_TYPE_RDB 0
#define CHILD_INFO_TYPE_AOF 1

struct redisServer {
    /* General */
    pid_t pid;                  /* Main process pid. */
    char *configfile;           /* Absolute config file path, or NULL */
    char *executable;           /* Absolute executable file path. */
    char **exec_argv;           /* Executable argv vector (copy). */
    int dynamic_hz;             /* Change hz value depending on # of clients. */
    int config_hz;              /* Configured HZ value. May be different than
                                   the actual 'hz' field value if dynamic-hz
                                   is enabled. */
    int hz;                     /* serverCron() calls frequency in hertz */
    redisDb *db;
    dict *commands;             /* Command table */
    dict *orig_commands;        /* Command table before command renaming. */
    aeEventLoop *el;
    _Atomic unsigned int lruclock; /* Clock for LRU eviction */
    int shutdown_asap;          /* SHUTDOWN needed ASAP */
    int activerehashing;        /* Incremental rehash in serverCron() */
    int active_defrag_running;  /* Active defragmentation running (holds current scan aggressiveness) */
    char *pidfile;              /* PID file path */
    int arch_bits;              /* 32 or 64 depending on sizeof(long) */
    int cronloops;              /* Number of times the cron function run */
    char runid[CONFIG_RUN_ID_SIZE+1];  /* ID always different at every exec. */
    int sentinel_mode;          /* True if this instance is a Sentinel. */
    size_t initial_memory_usage; /* Bytes used after initialization. */
    int always_show_logo;       /* Show logo even for non-stdout logging. */
    /* Modules */
    dict *moduleapi;            /* Exported core APIs dictionary for modules. */
    dict *sharedapi;            /* Like moduleapi but containing the APIs that
                                   modules share with each other. */
    list *loadmodule_queue;     /* List of modules to load at startup. */
    int module_blocked_pipe[2]; /* Pipe used to awake the event loop if a
                                   client blocked on a module command needs
                                   to be processed. */
    /* Networking */
    int port;                   /* TCP listening port */
    int tcp_backlog;            /* TCP listen() backlog */
    char *bindaddr[CONFIG_BINDADDR_MAX]; /* Addresses we should bind to */
    int bindaddr_count;         /* Number of addresses in server.bindaddr[] */
    char *unixsocket;           /* UNIX socket path */
    mode_t unixsocketperm;      /* UNIX socket permission */
    int ipfd[CONFIG_BINDADDR_MAX]; /* TCP socket file descriptors */
    int ipfd_count;             /* Used slots in ipfd[] */
    int sofd;                   /* Unix socket file descriptor */
    int cfd[CONFIG_BINDADDR_MAX];/* Cluster bus listening socket */
    int cfd_count;              /* Used slots in cfd[] */
    list *clients;              /* List of active clients */
    list *clients_to_close;     /* Clients to close asynchronously */
    list *clients_pending_write; /* There is to write or install handler. */
    list *clients_pending_read;  /* Client has pending read socket buffers. */
    list *slaves, *monitors;    /* List of slaves and MONITORs */
    client *current_client; /* Current client, only used on crash report */
    rax *clients_index;         /* Active clients dictionary by client ID. */
    int clients_paused;         /* True if clients are currently paused */
    mstime_t clients_pause_end_time; /* Time when we undo clients_paused */
    char neterr[ANET_ERR_LEN];   /* Error buffer for anet.c */
    dict *migrate_cached_sockets;/* MIGRATE cached sockets */
    _Atomic uint64_t next_client_id; /* Next client unique ID. Incremental. */
    int protected_mode;         /* Don't accept external connections. */
    int gopher_enabled;         /* If true the server will reply to gopher
                                   queries. Will still serve RESP2 queries. */
    int io_threads_num;         /* Number of IO threads to use. */
    int io_threads_do_reads;    /* Read and parse from IO threads? */

    /* RDB / AOF loading information */
    int loading;                /* We are loading data from disk if true */
    off_t loading_total_bytes;
    off_t loading_loaded_bytes;
    time_t loading_start_time;
    off_t loading_process_events_interval_bytes;
    /* Fast pointers to often looked up command */
    struct redisCommand *delCommand, *multiCommand, *lpushCommand,
                        *lpopCommand, *rpopCommand, *zpopminCommand,
                        *zpopmaxCommand, *sremCommand, *execCommand,
                        *expireCommand, *pexpireCommand, *xclaimCommand,
                        *xgroupCommand;
    /* Fields used only for stats */
    time_t stat_starttime;          /* Server start time */
    long long stat_numcommands;     /* Number of processed commands */
    long long stat_numconnections;  /* Number of connections received */
    long long stat_expiredkeys;     /* Number of expired keys */
    double stat_expired_stale_perc; /* Percentage of keys probably expired */
    long long stat_expired_time_cap_reached_count; /* Early expire cylce stops.*/
    long long stat_evictedkeys;     /* Number of evicted keys (maxmemory) */
    long long stat_keyspace_hits;   /* Number of successful lookups of keys */
    long long stat_keyspace_misses; /* Number of failed lookups of keys */
    long long stat_active_defrag_hits;      /* number of allocations moved */
    long long stat_active_defrag_misses;    /* number of allocations scanned but not moved */
    long long stat_active_defrag_key_hits;  /* number of keys with moved allocations */
    long long stat_active_defrag_key_misses;/* number of keys scanned and not moved */
    long long stat_active_defrag_scanned;   /* number of dictEntries scanned */
    size_t stat_peak_memory;        /* Max used memory record */
    long long stat_fork_time;       /* Time needed to perform latest fork() */
    double stat_fork_rate;          /* Fork rate in GB/sec. */
    long long stat_rejected_conn;   /* Clients rejected because of maxclients */
    long long stat_sync_full;       /* Number of full resyncs with slaves. */
    long long stat_sync_partial_ok; /* Number of accepted PSYNC requests. */
    long long stat_sync_partial_err;/* Number of unaccepted PSYNC requests. */
    list *slowlog;                  /* SLOWLOG list of commands */
    long long slowlog_entry_id;     /* SLOWLOG current entry ID */
    long long slowlog_log_slower_than; /* SLOWLOG time limit (to get logged) */
    unsigned long slowlog_max_len;     /* SLOWLOG max number of items logged */
    struct malloc_stats cron_malloc_stats; /* sampled in serverCron(). */
    _Atomic long long stat_net_input_bytes; /* Bytes read from network. */
    _Atomic long long stat_net_output_bytes; /* Bytes written to network. */
    size_t stat_rdb_cow_bytes;      /* Copy on write bytes during RDB saving. */
    size_t stat_aof_cow_bytes;      /* Copy on write bytes during AOF rewrite. */
    /* The following two are used to track instantaneous metrics, like
     * number of operations per second, network traffic. */
    struct {
        long long last_sample_time; /* Timestamp of last sample in ms */
        long long last_sample_count;/* Count in last sample */
        long long samples[STATS_METRIC_SAMPLES];
        int idx;
    } inst_metric[STATS_METRIC_COUNT];
    /* Configuration */
    int verbosity;                  /* Loglevel in redis.conf */
    int maxidletime;                /* Client timeout in seconds */
    int tcpkeepalive;               /* Set SO_KEEPALIVE if non-zero. */
    int active_expire_enabled;      /* Can be disabled for testing purposes. */
    int active_defrag_enabled;
    size_t active_defrag_ignore_bytes; /* minimum amount of fragmentation waste to start active defrag */
    int active_defrag_threshold_lower; /* minimum percentage of fragmentation to start active defrag */
    int active_defrag_threshold_upper; /* maximum percentage of fragmentation at which we use maximum effort */
    int active_defrag_cycle_min;       /* minimal effort for defrag in CPU percentage */
    int active_defrag_cycle_max;       /* maximal effort for defrag in CPU percentage */
    unsigned long active_defrag_max_scan_fields; /* maximum number of fields of set/hash/zset/list to process from within the main dict scan */
    _Atomic size_t client_max_querybuf_len; /* Limit for client query buffer length */
    int dbnum;                      /* Total number of configured DBs */
    int supervised;                 /* 1 if supervised, 0 otherwise. */
    int supervised_mode;            /* See SUPERVISED_* */
    int daemonize;                  /* True if running as a daemon */
    clientBufferLimitsConfig client_obuf_limits[CLIENT_TYPE_OBUF_COUNT];
    /* AOF persistence */
    int aof_enabled;                /* AOF configuration */
    int aof_state;                  /* AOF_(ON|OFF|WAIT_REWRITE) */
    int aof_fsync;                  /* Kind of fsync() policy */
    char *aof_filename;             /* Name of the AOF file */
    int aof_no_fsync_on_rewrite;    /* Don't fsync if a rewrite is in prog. */
    int aof_rewrite_perc;           /* Rewrite AOF if % growth is > M and... */
    off_t aof_rewrite_min_size;     /* the AOF file is at least N bytes. */
    off_t aof_rewrite_base_size;    /* AOF size on latest startup or rewrite. */
    off_t aof_current_size;         /* AOF current size. */
    off_t aof_fsync_offset;         /* AOF offset which is already synced to disk. */
    int aof_rewrite_scheduled;      /* Rewrite once BGSAVE terminates. */
    pid_t aof_child_pid;            /* PID if rewriting process */
    list *aof_rewrite_buf_blocks;   /* Hold changes during an AOF rewrite. */
    sds aof_buf;      /* AOF buffer, written before entering the event loop */
    int aof_fd;       /* File descriptor of currently selected AOF file */
    int aof_selected_db; /* Currently selected DB in AOF */
    time_t aof_flush_postponed_start; /* UNIX time of postponed AOF flush */
    time_t aof_last_fsync;            /* UNIX time of last fsync() */
    time_t aof_rewrite_time_last;   /* Time used by last AOF rewrite run. */
    time_t aof_rewrite_time_start;  /* Current AOF rewrite start time. */
    int aof_lastbgrewrite_status;   /* C_OK or C_ERR */
    unsigned long aof_delayed_fsync;  /* delayed AOF fsync() counter */
    int aof_rewrite_incremental_fsync;/* fsync incrementally while aof rewriting? */
    int rdb_save_incremental_fsync;   /* fsync incrementally while rdb saving? */
    int aof_last_write_status;      /* C_OK or C_ERR */
    int aof_last_write_errno;       /* Valid if aof_last_write_status is ERR */
    int aof_load_truncated;         /* Don't stop on unexpected AOF EOF. */
    int aof_use_rdb_preamble;       /* Use RDB preamble on AOF rewrites. */
    /* AOF pipes used to communicate between parent and child during rewrite. */
    int aof_pipe_write_data_to_child;
    int aof_pipe_read_data_from_parent;
    int aof_pipe_write_ack_to_parent;
    int aof_pipe_read_ack_from_child;
    int aof_pipe_write_ack_to_child;
    int aof_pipe_read_ack_from_parent;
    int aof_stop_sending_diff;     /* If true stop sending accumulated diffs
                                      to child process. */
    sds aof_child_diff;             /* AOF diff accumulator child side. */
    /* RDB persistence */
    long long dirty;                /* Changes to DB from the last save */
    long long dirty_before_bgsave;  /* Used to restore dirty on failed BGSAVE */
    pid_t rdb_child_pid;            /* PID of RDB saving child */
    struct saveparam *saveparams;   /* Save points array for RDB */
    int saveparamslen;              /* Number of saving points */
    char *rdb_filename;             /* Name of RDB file */
    int rdb_compression;            /* Use compression in RDB? */
    int rdb_checksum;               /* Use RDB checksum? */
    time_t lastsave;                /* Unix time of last successful save */
    time_t lastbgsave_try;          /* Unix time of last attempted bgsave */
    time_t rdb_save_time_last;      /* Time used by last RDB save run. */
    time_t rdb_save_time_start;     /* Current RDB save start time. */
    int rdb_bgsave_scheduled;       /* BGSAVE when possible if true. */
    int rdb_child_type;             /* Type of save by active child. */
    int lastbgsave_status;          /* C_OK or C_ERR */
    int stop_writes_on_bgsave_err;  /* Don't allow writes if can't BGSAVE */
    int rdb_pipe_write_result_to_parent; /* RDB pipes used to return the state */
    int rdb_pipe_read_result_from_child; /* of each slave in diskless SYNC. */
    int rdb_key_save_delay;         /* Delay in microseconds between keys while
                                     * writing the RDB. (for testings) */
    /* Pipe and data structures for child -> parent info sharing. */
    int child_info_pipe[2];         /* Pipe used to write the child_info_data. */
    struct {
        int process_type;           /* AOF or RDB child? */
        size_t cow_size;            /* Copy on write size. */
        unsigned long long magic;   /* Magic value to make sure data is valid. */
    } child_info_data;
    /* Propagation of commands in AOF / replication */
    redisOpArray also_propagate;    /* Additional command to propagate. */
    /* Logging */
    char *logfile;                  /* Path of log file */
    int syslog_enabled;             /* Is syslog enabled? */
    char *syslog_ident;             /* Syslog ident */
    int syslog_facility;            /* Syslog facility */
    /* Replication (master) */
    char replid[CONFIG_RUN_ID_SIZE+1];  /* My current replication ID. */
    char replid2[CONFIG_RUN_ID_SIZE+1]; /* replid inherited from master*/
    long long master_repl_offset;   /* My current replication offset */
    long long second_replid_offset; /* Accept offsets up to this for replid2. */
    int slaveseldb;                 /* Last SELECTed DB in replication output */
    int repl_ping_slave_period;     /* Master pings the slave every N seconds */
    char *repl_backlog;             /* Replication backlog for partial syncs */
    long long repl_backlog_size;    /* Backlog circular buffer size */
    long long repl_backlog_histlen; /* Backlog actual data length */
    long long repl_backlog_idx;     /* Backlog circular buffer current offset,
                                       that is the next byte will'll write to.*/
    long long repl_backlog_off;     /* Replication "master offset" of first
                                       byte in the replication backlog buffer.*/
    time_t repl_backlog_time_limit; /* Time without slaves after the backlog
                                       gets released. */
    time_t repl_no_slaves_since;    /* We have no slaves since that time.
                                       Only valid if server.slaves len is 0. */
    int repl_min_slaves_to_write;   /* Min number of slaves to write. */
    int repl_min_slaves_max_lag;    /* Max lag of <count> slaves to write. */
    int repl_good_slaves_count;     /* Number of slaves with lag <= max_lag. */
    int repl_diskless_sync;         /* Master send RDB to slaves sockets directly. */
    int repl_diskless_load;         /* Slave parse RDB directly from the socket.
                                     * see REPL_DISKLESS_LOAD_* enum */
    int repl_diskless_sync_delay;   /* Delay to start a diskless repl BGSAVE. */
    /* Replication (slave) */
    char *masteruser;               /* AUTH with this user and masterauth with master */
    char *masterauth;               /* AUTH with this password with master */
    char *masterhost;               /* Hostname of master */
    int masterport;                 /* Port of master */
    int repl_timeout;               /* Timeout after N seconds of master idle */
    client *master;     /* Client that is master for this slave */
    client *cached_master; /* Cached master to be reused for PSYNC. */
    int repl_syncio_timeout; /* Timeout for synchronous I/O calls */
    int repl_state;          /* Replication status if the instance is a slave */
    off_t repl_transfer_size; /* Size of RDB to read from master during sync. */
    off_t repl_transfer_read; /* Amount of RDB read from master during sync. */
    off_t repl_transfer_last_fsync_off; /* Offset when we fsync-ed last time. */
    int repl_transfer_s;     /* Slave -> Master SYNC socket */
    int repl_transfer_fd;    /* Slave -> Master SYNC temp file descriptor */
    char *repl_transfer_tmpfile; /* Slave-> master SYNC temp file name */
    time_t repl_transfer_lastio; /* Unix time of the latest read, for timeout */
    int repl_serve_stale_data; /* Serve stale data when link is down? */
    int repl_slave_ro;          /* Slave is read only? */
    int repl_slave_ignore_maxmemory;    /* If true slaves do not evict. */
    time_t repl_down_since; /* Unix time at which link with master went down */
    int repl_disable_tcp_nodelay;   /* Disable TCP_NODELAY after SYNC? */
    int slave_priority;             /* Reported in INFO and used by Sentinel. */
    int slave_announce_port;        /* Give the master this listening port. */
    char *slave_announce_ip;        /* Give the master this ip address. */
    /* The following two fields is where we store master PSYNC replid/offset
     * while the PSYNC is in progress. At the end we'll copy the fields into
     * the server->master client structure. */
    char master_replid[CONFIG_RUN_ID_SIZE+1];  /* Master PSYNC runid. */
    long long master_initial_offset;           /* Master PSYNC offset. */
    int repl_slave_lazy_flush;          /* Lazy FLUSHALL before loading DB? */
    /* Replication script cache. */
    dict *repl_scriptcache_dict;        /* SHA1 all slaves are aware of. */
    list *repl_scriptcache_fifo;        /* First in, first out LRU eviction. */
    unsigned int repl_scriptcache_size; /* Max number of elements. */
    /* Synchronous replication. */
    list *clients_waiting_acks;         /* Clients waiting in WAIT command. */
    int get_ack_from_slaves;            /* If true we send REPLCONF GETACK. */
    /* Limits */
    unsigned int maxclients;            /* Max number of simultaneous clients */
    unsigned long long maxmemory;   /* Max number of memory bytes to use */
    int maxmemory_policy;           /* Policy for key eviction */
    int maxmemory_samples;          /* Pricision of random sampling */
    int lfu_log_factor;             /* LFU logarithmic counter factor. */
    int lfu_decay_time;             /* LFU counter decay factor. */
    long long proto_max_bulk_len;   /* Protocol bulk length maximum size. */
    /* Blocked clients */
    unsigned int blocked_clients;   /* # of clients executing a blocking cmd.*/
    unsigned int blocked_clients_by_type[BLOCKED_NUM];
    list *unblocked_clients; /* list of clients to unblock before next loop */
    list *ready_keys;        /* List of readyList structures for BLPOP & co */
    /* Client side caching. */
    unsigned int tracking_clients;  /* # of clients with tracking enabled.*/
    /* Sort parameters - qsort_r() is only available under BSD so we
     * have to take this state global, in order to pass it to sortCompare() */
    int sort_desc;
    int sort_alpha;
    int sort_bypattern;
    int sort_store;
    /* Zip structure config, see redis.conf for more information  */
    size_t hash_max_ziplist_entries;
    size_t hash_max_ziplist_value;
    size_t set_max_intset_entries;
    size_t zset_max_ziplist_entries;
    size_t zset_max_ziplist_value;
    size_t hll_sparse_max_bytes;
    size_t stream_node_max_bytes;
    int64_t stream_node_max_entries;
    /* List parameters */
    int list_max_ziplist_size;
    int list_compress_depth;
    /* time cache */
    _Atomic time_t unixtime;    /* Unix time sampled every cron cycle. */
    time_t timezone;            /* Cached timezone. As set by tzset(). */
    int daylight_active;        /* Currently in daylight saving time. */
    long long mstime;           /* 'unixtime' with milliseconds resolution. */
    /* Pubsub */
    dict *pubsub_channels;  /* Map channels to list of subscribed clients */
    list *pubsub_patterns;  /* A list of pubsub_patterns */
    int notify_keyspace_events; /* Events to propagate via Pub/Sub. This is an
                                   xor of NOTIFY_... flags. */
    /* Cluster */
    int cluster_enabled;      /* Is cluster enabled? */
    mstime_t cluster_node_timeout; /* Cluster node timeout. */
    char *cluster_configfile; /* Cluster auto-generated config file name. */
    struct clusterState *cluster;  /* State of the cluster */
    int cluster_migration_barrier; /* Cluster replicas migration barrier. */
    int cluster_slave_validity_factor; /* Slave max data age for failover. */
    int cluster_require_full_coverage; /* If true, put the cluster down if
                                          there is at least an uncovered slot.*/
    int cluster_slave_no_failover;  /* Prevent slave from starting a failover
                                       if the master is in failure state. */
    char *cluster_announce_ip;  /* IP address to announce on cluster bus. */
    int cluster_announce_port;     /* base port to announce on cluster bus. */
    int cluster_announce_bus_port; /* bus port to announce on cluster bus. */
    int cluster_module_flags;      /* Set of flags that Redis modules are able
                                      to set in order to suppress certain
                                      native Redis Cluster features. Check the
                                      REDISMODULE_CLUSTER_FLAG_*. */
    /* Scripting */
    lua_State *lua; /* The Lua interpreter. We use just one for all clients */
    client *lua_client;   /* The "fake client" to query Redis from Lua */
    client *lua_caller;   /* The client running EVAL right now, or NULL */
    dict *lua_scripts;         /* A dictionary of SHA1 -> Lua scripts */
    unsigned long long lua_scripts_mem;  /* Cached scripts' memory + oh */
    mstime_t lua_time_limit;  /* Script timeout in milliseconds */
    mstime_t lua_time_start;  /* Start time of script, milliseconds time */
    int lua_write_dirty;  /* True if a write command was called during the
                             execution of the current script. */
    int lua_random_dirty; /* True if a random command was called during the
                             execution of the current script. */
    int lua_replicate_commands; /* True if we are doing single commands repl. */
    int lua_multi_emitted;/* True if we already proagated MULTI. */
    int lua_repl;         /* Script replication flags for redis.set_repl(). */
    int lua_timedout;     /* True if we reached the time limit for script
                             execution. */
    int lua_kill;         /* Kill the script if true. */
    int lua_always_replicate_commands; /* Default replication type. */
    /* Lazy free */
    int lazyfree_lazy_eviction;
    int lazyfree_lazy_expire;
    int lazyfree_lazy_server_del;
    /* Latency monitor */
    long long latency_monitor_threshold;
    dict *latency_events;
    /* ACLs */
    char *acl_filename;     /* ACL Users file. NULL if not configured. */
    /* Assert & bug reporting */
    const char *assert_failed;
    const char *assert_file;
    int assert_line;
    int bug_report_start; /* True if bug report header was already logged. */
    int watchdog_period;  /* Software watchdog period in ms. 0 = off */
    /* System hardware info */
    size_t system_memory_size;  /* Total memory in system as reported by OS */
};

typedef struct pubsubPattern {
    client *client;
    robj *pattern;
} pubsubPattern;

typedef void redisCommandProc(client *c);
typedef int *redisGetKeysProc(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);
struct redisCommand {
    char *name;
    redisCommandProc *proc;
    int arity;
    char *sflags;   /* Flags as string representation, one char per flag. */
    uint64_t flags; /* The actual flags, obtained from the 'sflags' field. */
    /* Use a function to determine keys arguments in a command line.
     * Used for Redis Cluster redirect. */
    redisGetKeysProc *getkeys_proc;
    /* What keys should be loaded in background when calling this command? */
    int firstkey; /* The first argument that's a key (0 = no keys) */
    int lastkey;  /* The last argument that's a key */
    int keystep;  /* The step between first and last key */
    long long microseconds, calls;
    int id;     /* Command ID. This is a progressive ID starting from 0 that
                   is assigned at runtime, and is used in order to check
                   ACLs. A connection is able to execute a given command if
                   the user associated to the connection has this command
                   bit set in the bitmap of allowed commands. */
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

/* Structure to hold list iteration abstraction. */
typedef struct {
    robj *subject;
    unsigned char encoding;
    unsigned char direction; /* Iteration direction */
    quicklistIter *iter;
} listTypeIterator;

/* Structure for an entry while iterating over a list. */
typedef struct {
    listTypeIterator *li;
    quicklistEntry entry; /* Entry in quicklist */
} listTypeEntry;

/* Structure to hold set iteration abstraction. */
typedef struct {
    robj *subject;
    int encoding;
    int ii; /* intset iterator */
    dictIterator *di;
} setTypeIterator;

/* Structure to hold hash iteration abstraction. Note that iteration over
 * hashes involves both fields and values. Because it is possible that
 * not both are required, store pointers in the iterator to avoid
 * unnecessary memory allocation for fields/values. */
typedef struct {
    robj *subject;
    int encoding;

    unsigned char *fptr, *vptr;

    dictIterator *di;
    dictEntry *de;
} hashTypeIterator;

#include "stream.h"  /* Stream data type header file. */

#define OBJ_HASH_KEY 1
#define OBJ_HASH_VALUE 2

/*-----------------------------------------------------------------------------
 * Extern declarations
 *----------------------------------------------------------------------------*/

extern struct redisServer server;
extern struct sharedObjectsStruct shared;
extern dictType objectKeyPointerValueDictType;
extern dictType objectKeyHeapPointerValueDictType;
extern dictType setDictType;
extern dictType zsetDictType;
extern dictType clusterNodesDictType;
extern dictType clusterNodesBlackListDictType;
extern dictType dbDictType;
extern dictType shaScriptObjectDictType;
extern double R_Zero, R_PosInf, R_NegInf, R_Nan;
extern dictType hashDictType;
extern dictType replScriptCacheDictType;
extern dictType keyptrDictType;
extern dictType modulesDictType;

/*-----------------------------------------------------------------------------
 * Functions prototypes
 *----------------------------------------------------------------------------*/

/* Modules */
void moduleInitModulesSystem(void);
int moduleLoad(const char *path, void **argv, int argc);
void moduleLoadFromQueue(void);
int *moduleGetCommandKeysViaAPI(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);
moduleType *moduleTypeLookupModuleByID(uint64_t id);
void moduleTypeNameByID(char *name, uint64_t moduleid);
void moduleFreeContext(struct RedisModuleCtx *ctx);
void unblockClientFromModule(client *c);
void moduleHandleBlockedClients(void);
void moduleBlockedClientTimedOut(client *c);
void moduleBlockedClientPipeReadable(aeEventLoop *el, int fd, void *privdata, int mask);
size_t moduleCount(void);
void moduleAcquireGIL(void);
void moduleReleaseGIL(void);
void moduleNotifyKeyspaceEvent(int type, const char *event, robj *key, int dbid);
void moduleCallCommandFilters(client *c);

/* Utils */
long long ustime(void);
long long mstime(void);
void getRandomHexChars(char *p, size_t len);
void getRandomBytes(unsigned char *p, size_t len);
uint64_t crc64(uint64_t crc, const unsigned char *s, uint64_t l);
void exitFromChild(int retcode);
size_t redisPopcount(void *s, long count);
void redisSetProcTitle(char *title);

/* networking.c -- Networking and Client related operations */
client *createClient(int fd);
void closeTimedoutClients(void);
void freeClient(client *c);
void freeClientAsync(client *c);
void resetClient(client *c);
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask);
void *addReplyDeferredLen(client *c);
void setDeferredArrayLen(client *c, void *node, long length);
void setDeferredMapLen(client *c, void *node, long length);
void setDeferredSetLen(client *c, void *node, long length);
void setDeferredAttributeLen(client *c, void *node, long length);
void setDeferredPushLen(client *c, void *node, long length);
void processInputBuffer(client *c);
void processInputBufferAndReplicate(client *c);
void processGopherRequest(client *c);
void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask);
void addReplyNull(client *c);
void addReplyNullArray(client *c);
void addReplyBool(client *c, int b);
void addReplyVerbatim(client *c, const char *s, size_t len, const char *ext);
void addReplyProto(client *c, const char *s, size_t len);
void AddReplyFromClient(client *c, client *src);
void addReplyBulk(client *c, robj *obj);
void addReplyBulkCString(client *c, const char *s);
void addReplyBulkCBuffer(client *c, const void *p, size_t len);
void addReplyBulkLongLong(client *c, long long ll);
void addReply(client *c, robj *obj);
void addReplySds(client *c, sds s);
void addReplyBulkSds(client *c, sds s);
void addReplyError(client *c, const char *err);
void addReplyStatus(client *c, const char *status);
void addReplyDouble(client *c, double d);
void addReplyHumanLongDouble(client *c, long double d);
void addReplyLongLong(client *c, long long ll);
void addReplyArrayLen(client *c, long length);
void addReplyMapLen(client *c, long length);
void addReplySetLen(client *c, long length);
void addReplyAttributeLen(client *c, long length);
void addReplyPushLen(client *c, long length);
void addReplyHelp(client *c, const char **help);
void addReplySubcommandSyntaxError(client *c);
void addReplyLoadedModules(client *c);
void copyClientOutputBuffer(client *dst, client *src);
size_t sdsZmallocSize(sds s);
size_t getStringObjectSdsUsedMemory(robj *o);
void freeClientReplyValue(void *o);
void *dupClientReplyValue(void *o);
void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer);
char *getClientPeerId(client *client);
sds catClientInfoString(sds s, client *client);
sds getAllClientsInfoString(int type);
void rewriteClientCommandVector(client *c, int argc, ...);
void rewriteClientCommandArgument(client *c, int i, robj *newval);
void replaceClientCommandVector(client *c, int argc, robj **argv);
unsigned long getClientOutputBufferMemoryUsage(client *c);
void freeClientsInAsyncFreeQueue(void);
void asyncCloseClientOnOutputBufferLimitReached(client *c);
int getClientType(client *c);
int getClientTypeByName(char *name);
char *getClientTypeName(int class);
void flushSlavesOutputBuffers(void);
void disconnectSlaves(void);
int listenToPort(int port, int *fds, int *count);
void pauseClients(mstime_t duration);
int clientsArePaused(void);
int processEventsWhileBlocked(void);
int handleClientsWithPendingWrites(void);
int handleClientsWithPendingWritesUsingThreads(void);
int handleClientsWithPendingReadsUsingThreads(void);
int stopThreadedIOIfNeeded(void);
int clientHasPendingReplies(client *c);
void unlinkClient(client *c);
int writeToClient(int fd, client *c, int handler_installed);
void linkClient(client *c);
void protectClient(client *c);
void unprotectClient(client *c);
void initThreadedIO(void);
client *lookupClientByID(uint64_t id);

#ifdef __GNUC__
void addReplyErrorFormat(client *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void addReplyStatusFormat(client *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void addReplyErrorFormat(client *c, const char *fmt, ...);
void addReplyStatusFormat(client *c, const char *fmt, ...);
#endif

/* Client side caching (tracking mode) */
void enableTracking(client *c, uint64_t redirect_to);
void disableTracking(client *c);
void trackingRememberKeys(client *c);
void trackingInvalidateKey(robj *keyobj);
void trackingInvalidateKeysOnFlush(int dbid);

/* List data type */
void listTypeTryConversion(robj *subject, robj *value);
void listTypePush(robj *subject, robj *value, int where);
robj *listTypePop(robj *subject, int where);
unsigned long listTypeLength(const robj *subject);
listTypeIterator *listTypeInitIterator(robj *subject, long index, unsigned char direction);
void listTypeReleaseIterator(listTypeIterator *li);
int listTypeNext(listTypeIterator *li, listTypeEntry *entry);
robj *listTypeGet(listTypeEntry *entry);
void listTypeInsert(listTypeEntry *entry, robj *value, int where);
int listTypeEqual(listTypeEntry *entry, robj *o);
void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry);
void listTypeConvert(robj *subject, int enc);
void unblockClientWaitingData(client *c);
void popGenericCommand(client *c, int where);

/* MULTI/EXEC/WATCH... */
void unwatchAllKeys(client *c);
void initClientMultiState(client *c);
void freeClientMultiState(client *c);
void queueMultiCommand(client *c);
void touchWatchedKey(redisDb *db, robj *key);
void touchWatchedKeysOnFlush(int dbid);
void discardTransaction(client *c);
void flagTransaction(client *c);
void execCommandPropagateMulti(client *c);

/* Redis object implementation */
void decrRefCount(robj *o);
void decrRefCountVoid(void *o);
void incrRefCount(robj *o);
robj *makeObjectShared(robj *o);
robj *resetRefCount(robj *obj);
void freeStringObject(robj *o);
void freeListObject(robj *o);
void freeSetObject(robj *o);
void freeZsetObject(robj *o);
void freeHashObject(robj *o);
robj *createObject(int type, void *ptr);
robj *createStringObject(const char *ptr, size_t len);
robj *createRawStringObject(const char *ptr, size_t len);
robj *createEmbeddedStringObject(const char *ptr, size_t len);
robj *dupStringObject(const robj *o);
int isSdsRepresentableAsLongLong(sds s, long long *llval);
int isObjectRepresentableAsLongLong(robj *o, long long *llongval);
robj *tryObjectEncoding(robj *o);
robj *getDecodedObject(robj *o);
size_t stringObjectLen(robj *o);
robj *createStringObjectFromLongLong(long long value);
robj *createStringObjectFromLongLongForValue(long long value);
robj *createStringObjectFromLongDouble(long double value, int humanfriendly);
robj *createQuicklistObject(void);
robj *createZiplistObject(void);
robj *createSetObject(void);
robj *createIntsetObject(void);
robj *createHashObject(void);
robj *createZsetObject(void);
robj *createZsetZiplistObject(void);
robj *createStreamObject(void);
robj *createModuleObject(moduleType *mt, void *value);
int getLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg);
int checkType(client *c, robj *o, int type);
int getLongLongFromObjectOrReply(client *c, robj *o, long long *target, const char *msg);
int getDoubleFromObjectOrReply(client *c, robj *o, double *target, const char *msg);
int getDoubleFromObject(const robj *o, double *target);
int getLongLongFromObject(robj *o, long long *target);
int getLongDoubleFromObject(robj *o, long double *target);
int getLongDoubleFromObjectOrReply(client *c, robj *o, long double *target, const char *msg);
char *strEncoding(int encoding);
int compareStringObjects(robj *a, robj *b);
int collateStringObjects(robj *a, robj *b);
int equalStringObjects(robj *a, robj *b);
unsigned long long estimateObjectIdleTime(robj *o);
void trimStringObjectIfNeeded(robj *o);
#define sdsEncodedObject(objptr) (objptr->encoding == OBJ_ENCODING_RAW || objptr->encoding == OBJ_ENCODING_EMBSTR)

/* Synchronous I/O with timeout */
ssize_t syncWrite(int fd, char *ptr, ssize_t size, long long timeout);
ssize_t syncRead(int fd, char *ptr, ssize_t size, long long timeout);
ssize_t syncReadLine(int fd, char *ptr, ssize_t size, long long timeout);

/* Replication */
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc);
void replicationFeedSlavesFromMasterStream(list *slaves, char *buf, size_t buflen);
void replicationFeedMonitors(client *c, list *monitors, int dictid, robj **argv, int argc);
void updateSlavesWaitingBgsave(int bgsaveerr, int type);
void replicationCron(void);
void replicationHandleMasterDisconnection(void);
void replicationCacheMaster(client *c);
void resizeReplicationBacklog(long long newsize);
void replicationSetMaster(char *ip, int port);
void replicationUnsetMaster(void);
void refreshGoodSlavesCount(void);
void replicationScriptCacheInit(void);
void replicationScriptCacheFlush(void);
void replicationScriptCacheAdd(sds sha1);
int replicationScriptCacheExists(sds sha1);
void processClientsWaitingReplicas(void);
void unblockClientWaitingReplicas(client *c);
int replicationCountAcksByOffset(long long offset);
void replicationSendNewlineToMaster(void);
long long replicationGetSlaveOffset(void);
char *replicationGetSlaveName(client *c);
long long getPsyncInitialOffset(void);
int replicationSetupSlaveForFullResync(client *slave, long long offset);
void changeReplicationId(void);
void clearReplicationId2(void);
void chopReplicationBacklog(void);
void replicationCacheMasterUsingMyself(void);
void feedReplicationBacklog(void *ptr, size_t len);

/* Generic persistence functions */
void startLoadingFile(FILE* fp, char* filename);
void startLoading(size_t size);
void loadingProgress(off_t pos);
void stopLoading(void);

#define DISK_ERROR_TYPE_AOF 1       /* Don't accept writes: AOF errors. */
#define DISK_ERROR_TYPE_RDB 2       /* Don't accept writes: RDB errors. */
#define DISK_ERROR_TYPE_NONE 0      /* No problems, we can accept writes. */
int writeCommandsDeniedByDiskError(void);

/* RDB persistence */
#include "rdb.h"
int rdbSaveRio(rio *rdb, int *error, int flags, rdbSaveInfo *rsi);
void killRDBChild(void);

/* AOF persistence */
void flushAppendOnlyFile(int force);
void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc);
void aofRemoveTempFile(pid_t childpid);
int rewriteAppendOnlyFileBackground(void);
int loadAppendOnlyFile(char *filename);
void stopAppendOnly(void);
int startAppendOnly(void);
void backgroundRewriteDoneHandler(int exitcode, int bysignal);
void aofRewriteBufferReset(void);
unsigned long aofRewriteBufferSize(void);
ssize_t aofReadDiffFromParent(void);
void killAppendOnlyChild(void);

/* Child info */
void openChildInfoPipe(void);
void closeChildInfoPipe(void);
void sendChildInfo(int process_type);
void receiveChildInfo(void);

/* acl.c -- Authentication related prototypes. */
extern rax *Users;
extern user *DefaultUser;
void ACLInit(void);
/* Return values for ACLCheckUserCredentials(). */
#define ACL_OK 0
#define ACL_DENIED_CMD 1
#define ACL_DENIED_KEY 2
int ACLCheckUserCredentials(robj *username, robj *password);
int ACLAuthenticateUser(client *c, robj *username, robj *password);
unsigned long ACLGetCommandID(const char *cmdname);
user *ACLGetUserByName(const char *name, size_t namelen);
int ACLCheckCommandPerm(client *c);
int ACLSetUser(user *u, const char *op, ssize_t oplen);
sds ACLDefaultUserFirstPassword(void);
uint64_t ACLGetCommandCategoryFlagByName(const char *name);
int ACLAppendUserForLoading(sds *argv, int argc, int *argc_err);
char *ACLSetUserStringError(void);
int ACLLoadConfiguredUsers(void);
sds ACLDescribeUser(user *u);
void ACLLoadUsersAtStartup(void);
void addReplyCommandCategories(client *c, struct redisCommand *cmd);

/* Sorted sets data type */

/* Input flags. */
#define ZADD_NONE 0
#define ZADD_INCR (1<<0)    /* Increment the score instead of setting it. */
#define ZADD_NX (1<<1)      /* Don't touch elements not already existing. */
#define ZADD_XX (1<<2)      /* Only touch elements already existing. */

/* Output flags. */
#define ZADD_NOP (1<<3)     /* Operation not performed because of conditionals.*/
#define ZADD_NAN (1<<4)     /* Only touch elements already existing. */
#define ZADD_ADDED (1<<5)   /* The element was new and was added. */
#define ZADD_UPDATED (1<<6) /* The element already existed, score updated. */

/* Flags only used by the ZADD command but not by zsetAdd() API: */
#define ZADD_CH (1<<16)      /* Return num of elements added or updated. */

/* Struct to hold a inclusive/exclusive range spec by score comparison. */
typedef struct {
    double min, max;
    int minex, maxex; /* are min or max exclusive? */
} zrangespec;

/* Struct to hold an inclusive/exclusive range spec by lexicographic comparison. */
typedef struct {
    sds min, max;     /* May be set to shared.(minstring|maxstring) */
    int minex, maxex; /* are min or max exclusive? */
} zlexrangespec;

zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
zskiplistNode *zslInsert(zskiplist *zsl, double score, sds ele);
unsigned char *zzlInsert(unsigned char *zl, sds ele, double score);
int zslDelete(zskiplist *zsl, double score, sds ele, zskiplistNode **node);
zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range);
zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range);
double zzlGetScore(unsigned char *sptr);
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range);
unsigned char *zzlLastInRange(unsigned char *zl, zrangespec *range);
unsigned long zsetLength(const robj *zobj);
void zsetConvert(robj *zobj, int encoding);
void zsetConvertToZiplistIfNeeded(robj *zobj, size_t maxelelen);
int zsetScore(robj *zobj, sds member, double *score);
unsigned long zslGetRank(zskiplist *zsl, double score, sds o);
int zsetAdd(robj *zobj, double score, sds ele, int *flags, double *newscore);
long zsetRank(robj *zobj, sds ele, int reverse);
int zsetDel(robj *zobj, sds ele);
void genericZpopCommand(client *c, robj **keyv, int keyc, int where, int emitkey, robj *countarg);
sds ziplistGetObject(unsigned char *sptr);
int zslValueGteMin(double value, zrangespec *spec);
int zslValueLteMax(double value, zrangespec *spec);
void zslFreeLexRange(zlexrangespec *spec);
int zslParseLexRange(robj *min, robj *max, zlexrangespec *spec);
unsigned char *zzlFirstInLexRange(unsigned char *zl, zlexrangespec *range);
unsigned char *zzlLastInLexRange(unsigned char *zl, zlexrangespec *range);
zskiplistNode *zslFirstInLexRange(zskiplist *zsl, zlexrangespec *range);
zskiplistNode *zslLastInLexRange(zskiplist *zsl, zlexrangespec *range);
int zzlLexValueGteMin(unsigned char *p, zlexrangespec *spec);
int zzlLexValueLteMax(unsigned char *p, zlexrangespec *spec);
int zslLexValueGteMin(sds value, zlexrangespec *spec);
int zslLexValueLteMax(sds value, zlexrangespec *spec);

/* Core functions */
int getMaxmemoryState(size_t *total, size_t *logical, size_t *tofree, float *level);
size_t freeMemoryGetNotCountedMemory();
int freeMemoryIfNeeded(void);
int freeMemoryIfNeededAndSafe(void);
int processCommand(client *c);
void setupSignalHandlers(void);
struct redisCommand *lookupCommand(sds name);
struct redisCommand *lookupCommandByCString(char *s);
struct redisCommand *lookupCommandOrOriginal(sds name);
void call(client *c, int flags);
void propagate(struct redisCommand *cmd, int dbid, robj **argv, int argc, int flags);
void alsoPropagate(struct redisCommand *cmd, int dbid, robj **argv, int argc, int target);
void forceCommandPropagation(client *c, int flags);
void preventCommandPropagation(client *c);
void preventCommandAOF(client *c);
void preventCommandReplication(client *c);
int prepareForShutdown();
#ifdef __GNUC__
void serverLog(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void serverLog(int level, const char *fmt, ...);
#endif
void serverLogRaw(int level, const char *msg);
void serverLogFromHandler(int level, const char *msg);
void usage(void);
void updateDictResizePolicy(void);
int htNeedsResize(dict *dict);
void populateCommandTable(void);
void resetCommandTableStats(void);
void adjustOpenFilesLimit(void);
void closeListeningSockets(int unlink_unix_socket);
void updateCachedTime(void);
void resetServerStats(void);
void activeDefragCycle(void);
unsigned int getLRUClock(void);
unsigned int LRU_CLOCK(void);
const char *evictPolicyToString(void);
struct redisMemOverhead *getMemoryOverheadData(void);
void freeMemoryOverheadData(struct redisMemOverhead *mh);

#define RESTART_SERVER_NONE 0
#define RESTART_SERVER_GRACEFULLY (1<<0)     /* Do proper shutdown. */
#define RESTART_SERVER_CONFIG_REWRITE (1<<1) /* CONFIG REWRITE before restart.*/
int restartServer(int flags, mstime_t delay);

/* Set data type */
robj *setTypeCreate(sds value);
int setTypeAdd(robj *subject, sds value);
int setTypeRemove(robj *subject, sds value);
int setTypeIsMember(robj *subject, sds value);
setTypeIterator *setTypeInitIterator(robj *subject);
void setTypeReleaseIterator(setTypeIterator *si);
int setTypeNext(setTypeIterator *si, sds *sdsele, int64_t *llele);
sds setTypeNextObject(setTypeIterator *si);
int setTypeRandomElement(robj *setobj, sds *sdsele, int64_t *llele);
unsigned long setTypeRandomElements(robj *set, unsigned long count, robj *aux_set);
unsigned long setTypeSize(const robj *subject);
void setTypeConvert(robj *subject, int enc);

/* Hash data type */
#define HASH_SET_TAKE_FIELD (1<<0)
#define HASH_SET_TAKE_VALUE (1<<1)
#define HASH_SET_COPY 0

void hashTypeConvert(robj *o, int enc);
void hashTypeTryConversion(robj *subject, robj **argv, int start, int end);
int hashTypeExists(robj *o, sds key);
int hashTypeDelete(robj *o, sds key);
unsigned long hashTypeLength(const robj *o);
hashTypeIterator *hashTypeInitIterator(robj *subject);
void hashTypeReleaseIterator(hashTypeIterator *hi);
int hashTypeNext(hashTypeIterator *hi);
void hashTypeCurrentFromZiplist(hashTypeIterator *hi, int what,
                                unsigned char **vstr,
                                unsigned int *vlen,
                                long long *vll);
sds hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what);
void hashTypeCurrentObject(hashTypeIterator *hi, int what, unsigned char **vstr, unsigned int *vlen, long long *vll);
sds hashTypeCurrentObjectNewSds(hashTypeIterator *hi, int what);
robj *hashTypeLookupWriteOrCreate(client *c, robj *key);
robj *hashTypeGetValueObject(robj *o, sds field);
int hashTypeSet(robj *o, sds field, sds value, int flags);

/* Pub / Sub */
int pubsubUnsubscribeAllChannels(client *c, int notify);
int pubsubUnsubscribeAllPatterns(client *c, int notify);
void freePubsubPattern(void *p);
int listMatchPubsubPattern(void *a, void *b);
int pubsubPublishMessage(robj *channel, robj *message);
void addReplyPubsubMessage(client *c, robj *channel, robj *msg);

/* Keyspace events notification */
void notifyKeyspaceEvent(int type, char *event, robj *key, int dbid);
int keyspaceEventsStringToFlags(char *classes);
sds keyspaceEventsFlagsToString(int flags);

/* Configuration */
void loadServerConfig(char *filename, char *options);
void appendServerSaveParams(time_t seconds, int changes);
void resetServerSaveParams(void);
struct rewriteConfigState; /* Forward declaration to export API. */
void rewriteConfigRewriteLine(struct rewriteConfigState *state, const char *option, sds line, int force);
int rewriteConfig(char *path);

/* db.c -- Keyspace access API */
int removeExpire(redisDb *db, robj *key);
void propagateExpire(redisDb *db, robj *key, int lazy);
int expireIfNeeded(redisDb *db, robj *key);
long long getExpire(redisDb *db, robj *key);
void setExpire(client *c, redisDb *db, robj *key, long long when);
robj *lookupKey(redisDb *db, robj *key, int flags);
robj *lookupKeyRead(redisDb *db, robj *key);
robj *lookupKeyWrite(redisDb *db, robj *key);
robj *lookupKeyReadOrReply(client *c, robj *key, robj *reply);
robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply);
robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags);
robj *objectCommandLookup(client *c, robj *key);
robj *objectCommandLookupOrReply(client *c, robj *key, robj *reply);
void objectSetLRUOrLFU(robj *val, long long lfu_freq, long long lru_idle,
                       long long lru_clock);
#define LOOKUP_NONE 0
#define LOOKUP_NOTOUCH (1<<0)
void dbAdd(redisDb *db, robj *key, robj *val);
void dbOverwrite(redisDb *db, robj *key, robj *val);
void setKey(redisDb *db, robj *key, robj *val);
int dbExists(redisDb *db, robj *key);
robj *dbRandomKey(redisDb *db);
int dbSyncDelete(redisDb *db, robj *key);
int dbDelete(redisDb *db, robj *key);
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o);

#define EMPTYDB_NO_FLAGS 0      /* No flags. */
#define EMPTYDB_ASYNC (1<<0)    /* Reclaim memory in another thread. */
long long emptyDb(int dbnum, int flags, void(callback)(void*));
long long emptyDbGeneric(redisDb *dbarray, int dbnum, int flags, void(callback)(void*));
long long dbTotalServerKeyCount();

int selectDb(client *c, int id);
void signalModifiedKey(redisDb *db, robj *key);
void signalFlushedDb(int dbid);
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count);
unsigned int countKeysInSlot(unsigned int hashslot);
unsigned int delKeysInSlot(unsigned int hashslot);
int verifyClusterConfigWithData(void);
void scanGenericCommand(client *c, robj *o, unsigned long cursor);
int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor);
void slotToKeyAdd(robj *key);
void slotToKeyDel(robj *key);
void slotToKeyFlush(void);
int dbAsyncDelete(redisDb *db, robj *key);
void emptyDbAsync(redisDb *db);
void slotToKeyFlushAsync(void);
size_t lazyfreeGetPendingObjectsCount(void);
void freeObjAsync(robj *o);

/* API to get key arguments from commands */
int *getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);
void getKeysFreeResult(int *result);
int *zunionInterGetKeys(struct redisCommand *cmd,robj **argv, int argc, int *numkeys);
int *evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);
int *sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);
int *migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);
int *georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);
int *xreadGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);

/* Cluster */
void clusterInit(void);
unsigned short crc16(const char *buf, int len);
unsigned int keyHashSlot(char *key, int keylen);
void clusterCron(void);
void clusterPropagatePublish(robj *channel, robj *message);
void migrateCloseTimedoutSockets(void);
void clusterBeforeSleep(void);
int clusterSendModuleMessageToTarget(const char *target, uint64_t module_id, uint8_t type, unsigned char *payload, uint32_t len);

/* Sentinel */
void initSentinelConfig(void);
void initSentinel(void);
void sentinelTimer(void);
char *sentinelHandleConfiguration(char **argv, int argc);
void sentinelIsRunning(void);

/* redis-check-rdb & aof */
int redis_check_rdb(char *rdbfilename, FILE *fp);
int redis_check_rdb_main(int argc, char **argv, FILE *fp);
int redis_check_aof_main(int argc, char **argv);

/* Scripting */
void scriptingInit(int setup);
int ldbRemoveChild(pid_t pid);
void ldbKillForkedSessions(void);
int ldbPendingChildren(void);
sds luaCreateFunction(client *c, lua_State *lua, robj *body);

/* Blocked clients */
void processUnblockedClients(void);
void blockClient(client *c, int btype);
void unblockClient(client *c);
void queueClientForReprocessing(client *c);
void replyToBlockedClientTimedOut(client *c);
int getTimeoutFromObjectOrReply(client *c, robj *object, mstime_t *timeout, int unit);
void disconnectAllBlockedClients(void);
void handleClientsBlockedOnKeys(void);
void signalKeyAsReady(redisDb *db, robj *key);
void blockForKeys(client *c, int btype, robj **keys, int numkeys, mstime_t timeout, robj *target, streamID *ids);

/* expire.c -- Handling of expired keys */
void activeExpireCycle(int type);
void expireSlaveKeys(void);
void rememberSlaveKeyWithExpire(redisDb *db, robj *key);
void flushSlaveKeysWithExpireList(void);
size_t getSlaveKeyWithExpireCount(void);

/* evict.c -- maxmemory handling and LRU eviction. */
void evictionPoolAlloc(void);
#define LFU_INIT_VAL 5
unsigned long LFUGetTimeInMinutes(void);
uint8_t LFULogIncr(uint8_t value);
unsigned long LFUDecrAndReturn(robj *o);

/* Keys hashing / comparison functions for dict.c hash tables. */
uint64_t dictSdsHash(const void *key);
int dictSdsKeyCompare(void *privdata, const void *key1, const void *key2);
void dictSdsDestructor(void *privdata, void *val);

/* Git SHA1 */
char *redisGitSHA1(void);
char *redisGitDirty(void);
uint64_t redisBuildId(void);

/* Commands prototypes */
void authCommand(client *c);
void pingCommand(client *c);
void echoCommand(client *c);
void commandCommand(client *c);
void setCommand(client *c);
void setnxCommand(client *c);
void setexCommand(client *c);
void psetexCommand(client *c);
void getCommand(client *c);
void delCommand(client *c);
void unlinkCommand(client *c);
void existsCommand(client *c);
void setbitCommand(client *c);
void getbitCommand(client *c);
void bitfieldCommand(client *c);
void setrangeCommand(client *c);
void getrangeCommand(client *c);
void incrCommand(client *c);
void decrCommand(client *c);
void incrbyCommand(client *c);
void decrbyCommand(client *c);
void incrbyfloatCommand(client *c);
void selectCommand(client *c);
void swapdbCommand(client *c);
void randomkeyCommand(client *c);
void keysCommand(client *c);
void scanCommand(client *c);
void dbsizeCommand(client *c);
void lastsaveCommand(client *c);
void saveCommand(client *c);
void bgsaveCommand(client *c);
void bgrewriteaofCommand(client *c);
void shutdownCommand(client *c);
void moveCommand(client *c);
void renameCommand(client *c);
void renamenxCommand(client *c);
void lpushCommand(client *c);
void rpushCommand(client *c);
void lpushxCommand(client *c);
void rpushxCommand(client *c);
void linsertCommand(client *c);
void lpopCommand(client *c);
void rpopCommand(client *c);
void llenCommand(client *c);
void lindexCommand(client *c);
void lrangeCommand(client *c);
void ltrimCommand(client *c);
void typeCommand(client *c);
void lsetCommand(client *c);
void saddCommand(client *c);
void sremCommand(client *c);
void smoveCommand(client *c);
void sismemberCommand(client *c);
void scardCommand(client *c);
void spopCommand(client *c);
void srandmemberCommand(client *c);
void sinterCommand(client *c);
void sinterstoreCommand(client *c);
void sunionCommand(client *c);
void sunionstoreCommand(client *c);
void sdiffCommand(client *c);
void sdiffstoreCommand(client *c);
void sscanCommand(client *c);
void syncCommand(client *c);
void flushdbCommand(client *c);
void flushallCommand(client *c);
void sortCommand(client *c);
void lremCommand(client *c);
void rpoplpushCommand(client *c);
void infoCommand(client *c);
void mgetCommand(client *c);
void monitorCommand(client *c);
void expireCommand(client *c);
void expireatCommand(client *c);
void pexpireCommand(client *c);
void pexpireatCommand(client *c);
void getsetCommand(client *c);
void ttlCommand(client *c);
void touchCommand(client *c);
void pttlCommand(client *c);
void persistCommand(client *c);
void replicaofCommand(client *c);
void roleCommand(client *c);
void debugCommand(client *c);
void msetCommand(client *c);
void msetnxCommand(client *c);
void zaddCommand(client *c);
void zincrbyCommand(client *c);
void zrangeCommand(client *c);
void zrangebyscoreCommand(client *c);
void zrevrangebyscoreCommand(client *c);
void zrangebylexCommand(client *c);
void zrevrangebylexCommand(client *c);
void zcountCommand(client *c);
void zlexcountCommand(client *c);
void zrevrangeCommand(client *c);
void zcardCommand(client *c);
void zremCommand(client *c);
void zscoreCommand(client *c);
void zremrangebyscoreCommand(client *c);
void zremrangebylexCommand(client *c);
void zpopminCommand(client *c);
void zpopmaxCommand(client *c);
void bzpopminCommand(client *c);
void bzpopmaxCommand(client *c);
void multiCommand(client *c);
void execCommand(client *c);
void discardCommand(client *c);
void blpopCommand(client *c);
void brpopCommand(client *c);
void brpoplpushCommand(client *c);
void appendCommand(client *c);
void strlenCommand(client *c);
void zrankCommand(client *c);
void zrevrankCommand(client *c);
void hsetCommand(client *c);
void hsetnxCommand(client *c);
void hgetCommand(client *c);
void hmsetCommand(client *c);
void hmgetCommand(client *c);
void hdelCommand(client *c);
void hlenCommand(client *c);
void hstrlenCommand(client *c);
void zremrangebyrankCommand(client *c);
void zunionstoreCommand(client *c);
void zinterstoreCommand(client *c);
void zscanCommand(client *c);
void hkeysCommand(client *c);
void hvalsCommand(client *c);
void hgetallCommand(client *c);
void hexistsCommand(client *c);
void hscanCommand(client *c);
void configCommand(client *c);
void hincrbyCommand(client *c);
void hincrbyfloatCommand(client *c);
void subscribeCommand(client *c);
void unsubscribeCommand(client *c);
void psubscribeCommand(client *c);
void punsubscribeCommand(client *c);
void publishCommand(client *c);
void pubsubCommand(client *c);
void watchCommand(client *c);
void unwatchCommand(client *c);
void clusterCommand(client *c);
void restoreCommand(client *c);
void migrateCommand(client *c);
void askingCommand(client *c);
void readonlyCommand(client *c);
void readwriteCommand(client *c);
void dumpCommand(client *c);
void objectCommand(client *c);
void memoryCommand(client *c);
void clientCommand(client *c);
void helloCommand(client *c);
void evalCommand(client *c);
void evalShaCommand(client *c);
void scriptCommand(client *c);
void timeCommand(client *c);
void bitopCommand(client *c);
void bitcountCommand(client *c);
void bitposCommand(client *c);
void replconfCommand(client *c);
void waitCommand(client *c);
void geoencodeCommand(client *c);
void geodecodeCommand(client *c);
void georadiusbymemberCommand(client *c);
void georadiusbymemberroCommand(client *c);
void georadiusCommand(client *c);
void georadiusroCommand(client *c);
void geoaddCommand(client *c);
void geohashCommand(client *c);
void geoposCommand(client *c);
void geodistCommand(client *c);
void pfselftestCommand(client *c);
void pfaddCommand(client *c);
void pfcountCommand(client *c);
void pfmergeCommand(client *c);
void pfdebugCommand(client *c);
void latencyCommand(client *c);
void moduleCommand(client *c);
void securityWarningCommand(client *c);
void xaddCommand(client *c);
void xrangeCommand(client *c);
void xrevrangeCommand(client *c);
void xlenCommand(client *c);
void xreadCommand(client *c);
void xgroupCommand(client *c);
void xsetidCommand(client *c);
void xackCommand(client *c);
void xpendingCommand(client *c);
void xclaimCommand(client *c);
void xinfoCommand(client *c);
void xdelCommand(client *c);
void xtrimCommand(client *c);
void lolwutCommand(client *c);
void aclCommand(client *c);

#if defined(__GNUC__)
void *calloc(size_t count, size_t size) __attribute__ ((deprecated));
void free(void *ptr) __attribute__ ((deprecated));
void *malloc(size_t size) __attribute__ ((deprecated));
void *realloc(void *ptr, size_t size) __attribute__ ((deprecated));
#endif

/* Debugging stuff */
void _serverAssertWithInfo(const client *c, const robj *o, const char *estr, const char *file, int line);
void _serverAssert(const char *estr, const char *file, int line);
void _serverPanic(const char *file, int line, const char *msg, ...);
void bugReportStart(void);
void serverLogObjectDebugInfo(const robj *o);
void sigsegvHandler(int sig, siginfo_t *info, void *secret);
sds genRedisInfoString(char *section);
void enableWatchdog(int period);
void disableWatchdog(void);
void watchdogScheduleSignal(int period);
void serverLogHexDump(int level, char *descr, void *value, size_t len);
int memtest_preserving_test(unsigned long *m, size_t bytes, int passes);
void mixDigest(unsigned char *digest, void *ptr, size_t len);
void xorDigest(unsigned char *digest, void *ptr, size_t len);
int populateCommandTableParseFlags(struct redisCommand *c, char *strflags);

#define redisDebug(fmt, ...) \
    printf("DEBUG %s:%d > " fmt "\n", __FILE__, __LINE__, __VA_ARGS__)
#define redisDebugMark() \
    printf("-- MARK %s:%d --\n", __FILE__, __LINE__)

#endif

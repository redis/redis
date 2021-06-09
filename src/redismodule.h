#ifndef REDISMODULE_H
#define REDISMODULE_H

#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>

/* ---------------- Defines common between core and modules --------------- */

/* Error status return values. */
#define REDISMODULE_OK 0
#define REDISMODULE_ERR 1

/* API versions. */
#define REDISMODULE_APIVER_1 1

/* Version of the RedisModuleTypeMethods structure. Once the RedisModuleTypeMethods 
 * structure is changed, this version number needs to be changed synchronistically. */
#define REDISMODULE_TYPE_METHOD_VERSION 3

/* API flags and constants */
#define REDISMODULE_READ (1<<0)
#define REDISMODULE_WRITE (1<<1)

/* RedisModule_OpenKey extra flags for the 'mode' argument.
 * Avoid touching the LRU/LFU of the key when opened. */
#define REDISMODULE_OPEN_KEY_NOTOUCH (1<<16)

#define REDISMODULE_LIST_HEAD 0
#define REDISMODULE_LIST_TAIL 1

/* Key types. */
#define REDISMODULE_KEYTYPE_EMPTY 0
#define REDISMODULE_KEYTYPE_STRING 1
#define REDISMODULE_KEYTYPE_LIST 2
#define REDISMODULE_KEYTYPE_HASH 3
#define REDISMODULE_KEYTYPE_SET 4
#define REDISMODULE_KEYTYPE_ZSET 5
#define REDISMODULE_KEYTYPE_MODULE 6
#define REDISMODULE_KEYTYPE_STREAM 7

/* Reply types. */
#define REDISMODULE_REPLY_UNKNOWN -1
#define REDISMODULE_REPLY_STRING 0
#define REDISMODULE_REPLY_ERROR 1
#define REDISMODULE_REPLY_INTEGER 2
#define REDISMODULE_REPLY_ARRAY 3
#define REDISMODULE_REPLY_NULL 4

/* Postponed array length. */
#define REDISMODULE_POSTPONED_ARRAY_LEN -1

/* Expire */
#define REDISMODULE_NO_EXPIRE -1

/* Sorted set API flags. */
#define REDISMODULE_ZADD_XX      (1<<0)
#define REDISMODULE_ZADD_NX      (1<<1)
#define REDISMODULE_ZADD_ADDED   (1<<2)
#define REDISMODULE_ZADD_UPDATED (1<<3)
#define REDISMODULE_ZADD_NOP     (1<<4)
#define REDISMODULE_ZADD_GT      (1<<5)
#define REDISMODULE_ZADD_LT      (1<<6)

/* Hash API flags. */
#define REDISMODULE_HASH_NONE       0
#define REDISMODULE_HASH_NX         (1<<0)
#define REDISMODULE_HASH_XX         (1<<1)
#define REDISMODULE_HASH_CFIELDS    (1<<2)
#define REDISMODULE_HASH_EXISTS     (1<<3)
#define REDISMODULE_HASH_COUNT_ALL  (1<<4)

/* StreamID type. */
typedef struct RedisModuleStreamID {
    uint64_t ms;
    uint64_t seq;
} RedisModuleStreamID;

/* StreamAdd() flags. */
#define REDISMODULE_STREAM_ADD_AUTOID (1<<0)
/* StreamIteratorStart() flags. */
#define REDISMODULE_STREAM_ITERATOR_EXCLUSIVE (1<<0)
#define REDISMODULE_STREAM_ITERATOR_REVERSE (1<<1)
/* StreamIteratorTrim*() flags. */
#define REDISMODULE_STREAM_TRIM_APPROX (1<<0)

/* Context Flags: Info about the current context returned by
 * RM_GetContextFlags(). */

/* The command is running in the context of a Lua script */
#define REDISMODULE_CTX_FLAGS_LUA (1<<0)
/* The command is running inside a Redis transaction */
#define REDISMODULE_CTX_FLAGS_MULTI (1<<1)
/* The instance is a master */
#define REDISMODULE_CTX_FLAGS_MASTER (1<<2)
/* The instance is a slave */
#define REDISMODULE_CTX_FLAGS_SLAVE (1<<3)
/* The instance is read-only (usually meaning it's a slave as well) */
#define REDISMODULE_CTX_FLAGS_READONLY (1<<4)
/* The instance is running in cluster mode */
#define REDISMODULE_CTX_FLAGS_CLUSTER (1<<5)
/* The instance has AOF enabled */
#define REDISMODULE_CTX_FLAGS_AOF (1<<6)
/* The instance has RDB enabled */
#define REDISMODULE_CTX_FLAGS_RDB (1<<7)
/* The instance has Maxmemory set */
#define REDISMODULE_CTX_FLAGS_MAXMEMORY (1<<8)
/* Maxmemory is set and has an eviction policy that may delete keys */
#define REDISMODULE_CTX_FLAGS_EVICT (1<<9)
/* Redis is out of memory according to the maxmemory flag. */
#define REDISMODULE_CTX_FLAGS_OOM (1<<10)
/* Less than 25% of memory available according to maxmemory. */
#define REDISMODULE_CTX_FLAGS_OOM_WARNING (1<<11)
/* The command was sent over the replication link. */
#define REDISMODULE_CTX_FLAGS_REPLICATED (1<<12)
/* Redis is currently loading either from AOF or RDB. */
#define REDISMODULE_CTX_FLAGS_LOADING (1<<13)
/* The replica has no link with its master, note that
 * there is the inverse flag as well:
 *
 *  REDISMODULE_CTX_FLAGS_REPLICA_IS_ONLINE
 *
 * The two flags are exclusive, one or the other can be set. */
#define REDISMODULE_CTX_FLAGS_REPLICA_IS_STALE (1<<14)
/* The replica is trying to connect with the master.
 * (REPL_STATE_CONNECT and REPL_STATE_CONNECTING states) */
#define REDISMODULE_CTX_FLAGS_REPLICA_IS_CONNECTING (1<<15)
/* THe replica is receiving an RDB file from its master. */
#define REDISMODULE_CTX_FLAGS_REPLICA_IS_TRANSFERRING (1<<16)
/* The replica is online, receiving updates from its master. */
#define REDISMODULE_CTX_FLAGS_REPLICA_IS_ONLINE (1<<17)
/* There is currently some background process active. */
#define REDISMODULE_CTX_FLAGS_ACTIVE_CHILD (1<<18)
/* The next EXEC will fail due to dirty CAS (touched keys). */
#define REDISMODULE_CTX_FLAGS_MULTI_DIRTY (1<<19)
/* Redis is currently running inside background child process. */
#define REDISMODULE_CTX_FLAGS_IS_CHILD (1<<20)
/* The current client does not allow blocking, either called from
 * within multi, lua, or from another module using RM_Call */
#define REDISMODULE_CTX_FLAGS_DENY_BLOCKING (1<<21)

/* Next context flag, must be updated when adding new flags above!
This flag should not be used directly by the module.
 * Use RedisModule_GetContextFlagsAll instead. */
#define _REDISMODULE_CTX_FLAGS_NEXT (1<<22)

/* Keyspace changes notification classes. Every class is associated with a
 * character for configuration purposes.
 * NOTE: These have to be in sync with NOTIFY_* in server.h */
#define REDISMODULE_NOTIFY_KEYSPACE (1<<0)    /* K */
#define REDISMODULE_NOTIFY_KEYEVENT (1<<1)    /* E */
#define REDISMODULE_NOTIFY_GENERIC (1<<2)     /* g */
#define REDISMODULE_NOTIFY_STRING (1<<3)      /* $ */
#define REDISMODULE_NOTIFY_LIST (1<<4)        /* l */
#define REDISMODULE_NOTIFY_SET (1<<5)         /* s */
#define REDISMODULE_NOTIFY_HASH (1<<6)        /* h */
#define REDISMODULE_NOTIFY_ZSET (1<<7)        /* z */
#define REDISMODULE_NOTIFY_EXPIRED (1<<8)     /* x */
#define REDISMODULE_NOTIFY_EVICTED (1<<9)     /* e */
#define REDISMODULE_NOTIFY_STREAM (1<<10)     /* t */
#define REDISMODULE_NOTIFY_KEY_MISS (1<<11)   /* m (Note: This one is excluded from REDISMODULE_NOTIFY_ALL on purpose) */
#define REDISMODULE_NOTIFY_LOADED (1<<12)     /* module only key space notification, indicate a key loaded from rdb */
#define REDISMODULE_NOTIFY_MODULE (1<<13)     /* d, module key space notification */

/* Next notification flag, must be updated when adding new flags above!
This flag should not be used directly by the module.
 * Use RedisModule_GetKeyspaceNotificationFlagsAll instead. */
#define _REDISMODULE_NOTIFY_NEXT (1<<14)

#define REDISMODULE_NOTIFY_ALL (REDISMODULE_NOTIFY_GENERIC | REDISMODULE_NOTIFY_STRING | REDISMODULE_NOTIFY_LIST | REDISMODULE_NOTIFY_SET | REDISMODULE_NOTIFY_HASH | REDISMODULE_NOTIFY_ZSET | REDISMODULE_NOTIFY_EXPIRED | REDISMODULE_NOTIFY_EVICTED | REDISMODULE_NOTIFY_STREAM | REDISMODULE_NOTIFY_MODULE)      /* A */

/* A special pointer that we can use between the core and the module to signal
 * field deletion, and that is impossible to be a valid pointer. */
#define REDISMODULE_HASH_DELETE ((RedisModuleString*)(long)1)

/* Error messages. */
#define REDISMODULE_ERRORMSG_WRONGTYPE "WRONGTYPE Operation against a key holding the wrong kind of value"

#define REDISMODULE_POSITIVE_INFINITE (1.0/0.0)
#define REDISMODULE_NEGATIVE_INFINITE (-1.0/0.0)

/* Cluster API defines. */
#define REDISMODULE_NODE_ID_LEN 40
#define REDISMODULE_NODE_MYSELF     (1<<0)
#define REDISMODULE_NODE_MASTER     (1<<1)
#define REDISMODULE_NODE_SLAVE      (1<<2)
#define REDISMODULE_NODE_PFAIL      (1<<3)
#define REDISMODULE_NODE_FAIL       (1<<4)
#define REDISMODULE_NODE_NOFAILOVER (1<<5)

#define REDISMODULE_CLUSTER_FLAG_NONE 0
#define REDISMODULE_CLUSTER_FLAG_NO_FAILOVER (1<<1)
#define REDISMODULE_CLUSTER_FLAG_NO_REDIRECTION (1<<2)

#define REDISMODULE_NOT_USED(V) ((void) V)

/* Logging level strings */
#define REDISMODULE_LOGLEVEL_DEBUG "debug"
#define REDISMODULE_LOGLEVEL_VERBOSE "verbose"
#define REDISMODULE_LOGLEVEL_NOTICE "notice"
#define REDISMODULE_LOGLEVEL_WARNING "warning"

/* Bit flags for aux_save_triggers and the aux_load and aux_save callbacks */
#define REDISMODULE_AUX_BEFORE_RDB (1<<0)
#define REDISMODULE_AUX_AFTER_RDB (1<<1)

/* This type represents a timer handle, and is returned when a timer is
 * registered and used in order to invalidate a timer. It's just a 64 bit
 * number, because this is how each timer is represented inside the radix tree
 * of timers that are going to expire, sorted by expire time. */
typedef uint64_t RedisModuleTimerID;

/* CommandFilter Flags */

/* Do filter RedisModule_Call() commands initiated by module itself. */
#define REDISMODULE_CMDFILTER_NOSELF    (1<<0)

/* Declare that the module can handle errors with RedisModule_SetModuleOptions. */
#define REDISMODULE_OPTIONS_HANDLE_IO_ERRORS    (1<<0)
/* When set, Redis will not call RedisModule_SignalModifiedKey(), implicitly in
 * RedisModule_CloseKey, and the module needs to do that when manually when keys
 * are modified from the user's sperspective, to invalidate WATCH. */
#define REDISMODULE_OPTION_NO_IMPLICIT_SIGNAL_MODIFIED (1<<1)

/* Server events definitions.
 * Those flags should not be used directly by the module, instead
 * the module should use RedisModuleEvent_* variables */
#define REDISMODULE_EVENT_REPLICATION_ROLE_CHANGED 0
#define REDISMODULE_EVENT_PERSISTENCE 1
#define REDISMODULE_EVENT_FLUSHDB 2
#define REDISMODULE_EVENT_LOADING 3
#define REDISMODULE_EVENT_CLIENT_CHANGE 4
#define REDISMODULE_EVENT_SHUTDOWN 5
#define REDISMODULE_EVENT_REPLICA_CHANGE 6
#define REDISMODULE_EVENT_MASTER_LINK_CHANGE 7
#define REDISMODULE_EVENT_CRON_LOOP 8
#define REDISMODULE_EVENT_MODULE_CHANGE 9
#define REDISMODULE_EVENT_LOADING_PROGRESS 10
#define REDISMODULE_EVENT_SWAPDB 11
#define REDISMODULE_EVENT_REPL_BACKUP 12
#define REDISMODULE_EVENT_FORK_CHILD 13
#define _REDISMODULE_EVENT_NEXT 14 /* Next event flag, should be updated if a new event added. */

typedef struct RedisModuleEvent {
    uint64_t id;        /* REDISMODULE_EVENT_... defines. */
    uint64_t dataver;   /* Version of the structure we pass as 'data'. */
} RedisModuleEvent;

struct RedisModuleCtx;
struct RedisModuleDefragCtx;
typedef void (*RedisModuleEventCallback)(struct RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent, void *data);

static const RedisModuleEvent
    RedisModuleEvent_ReplicationRoleChanged = {
        REDISMODULE_EVENT_REPLICATION_ROLE_CHANGED,
        1
    },
    RedisModuleEvent_Persistence = {
        REDISMODULE_EVENT_PERSISTENCE,
        1
    },
    RedisModuleEvent_FlushDB = {
        REDISMODULE_EVENT_FLUSHDB,
        1
    },
    RedisModuleEvent_Loading = {
        REDISMODULE_EVENT_LOADING,
        1
    },
    RedisModuleEvent_ClientChange = {
        REDISMODULE_EVENT_CLIENT_CHANGE,
        1
    },
    RedisModuleEvent_Shutdown = {
        REDISMODULE_EVENT_SHUTDOWN,
        1
    },
    RedisModuleEvent_ReplicaChange = {
        REDISMODULE_EVENT_REPLICA_CHANGE,
        1
    },
    RedisModuleEvent_CronLoop = {
        REDISMODULE_EVENT_CRON_LOOP,
        1
    },
    RedisModuleEvent_MasterLinkChange = {
        REDISMODULE_EVENT_MASTER_LINK_CHANGE,
        1
    },
    RedisModuleEvent_ModuleChange = {
        REDISMODULE_EVENT_MODULE_CHANGE,
        1
    },
    RedisModuleEvent_LoadingProgress = {
        REDISMODULE_EVENT_LOADING_PROGRESS,
        1
    },
    RedisModuleEvent_SwapDB = {
        REDISMODULE_EVENT_SWAPDB,
        1
    },
    RedisModuleEvent_ReplBackup = {
        REDISMODULE_EVENT_REPL_BACKUP,
        1
    },
    RedisModuleEvent_ForkChild = {
        REDISMODULE_EVENT_FORK_CHILD,
        1
    };

/* Those are values that are used for the 'subevent' callback argument. */
#define REDISMODULE_SUBEVENT_PERSISTENCE_RDB_START 0
#define REDISMODULE_SUBEVENT_PERSISTENCE_AOF_START 1
#define REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_RDB_START 2
#define REDISMODULE_SUBEVENT_PERSISTENCE_ENDED 3
#define REDISMODULE_SUBEVENT_PERSISTENCE_FAILED 4
#define _REDISMODULE_SUBEVENT_PERSISTENCE_NEXT 5

#define REDISMODULE_SUBEVENT_LOADING_RDB_START 0
#define REDISMODULE_SUBEVENT_LOADING_AOF_START 1
#define REDISMODULE_SUBEVENT_LOADING_REPL_START 2
#define REDISMODULE_SUBEVENT_LOADING_ENDED 3
#define REDISMODULE_SUBEVENT_LOADING_FAILED 4
#define _REDISMODULE_SUBEVENT_LOADING_NEXT 5

#define REDISMODULE_SUBEVENT_CLIENT_CHANGE_CONNECTED 0
#define REDISMODULE_SUBEVENT_CLIENT_CHANGE_DISCONNECTED 1
#define _REDISMODULE_SUBEVENT_CLIENT_CHANGE_NEXT 2

#define REDISMODULE_SUBEVENT_MASTER_LINK_UP 0
#define REDISMODULE_SUBEVENT_MASTER_LINK_DOWN 1
#define _REDISMODULE_SUBEVENT_MASTER_NEXT 2

#define REDISMODULE_SUBEVENT_REPLICA_CHANGE_ONLINE 0
#define REDISMODULE_SUBEVENT_REPLICA_CHANGE_OFFLINE 1
#define _REDISMODULE_SUBEVENT_REPLICA_CHANGE_NEXT 2

#define REDISMODULE_EVENT_REPLROLECHANGED_NOW_MASTER 0
#define REDISMODULE_EVENT_REPLROLECHANGED_NOW_REPLICA 1
#define _REDISMODULE_EVENT_REPLROLECHANGED_NEXT 2

#define REDISMODULE_SUBEVENT_FLUSHDB_START 0
#define REDISMODULE_SUBEVENT_FLUSHDB_END 1
#define _REDISMODULE_SUBEVENT_FLUSHDB_NEXT 2

#define REDISMODULE_SUBEVENT_MODULE_LOADED 0
#define REDISMODULE_SUBEVENT_MODULE_UNLOADED 1
#define _REDISMODULE_SUBEVENT_MODULE_NEXT 2

#define REDISMODULE_SUBEVENT_LOADING_PROGRESS_RDB 0
#define REDISMODULE_SUBEVENT_LOADING_PROGRESS_AOF 1
#define _REDISMODULE_SUBEVENT_LOADING_PROGRESS_NEXT 2

#define REDISMODULE_SUBEVENT_REPL_BACKUP_CREATE 0
#define REDISMODULE_SUBEVENT_REPL_BACKUP_RESTORE 1
#define REDISMODULE_SUBEVENT_REPL_BACKUP_DISCARD 2
#define _REDISMODULE_SUBEVENT_REPL_BACKUP_NEXT 3

#define REDISMODULE_SUBEVENT_FORK_CHILD_BORN 0
#define REDISMODULE_SUBEVENT_FORK_CHILD_DIED 1
#define _REDISMODULE_SUBEVENT_FORK_CHILD_NEXT 2

#define _REDISMODULE_SUBEVENT_SHUTDOWN_NEXT 0
#define _REDISMODULE_SUBEVENT_CRON_LOOP_NEXT 0
#define _REDISMODULE_SUBEVENT_SWAPDB_NEXT 0

/* RedisModuleClientInfo flags. */
#define REDISMODULE_CLIENTINFO_FLAG_SSL (1<<0)
#define REDISMODULE_CLIENTINFO_FLAG_PUBSUB (1<<1)
#define REDISMODULE_CLIENTINFO_FLAG_BLOCKED (1<<2)
#define REDISMODULE_CLIENTINFO_FLAG_TRACKING (1<<3)
#define REDISMODULE_CLIENTINFO_FLAG_UNIXSOCKET (1<<4)
#define REDISMODULE_CLIENTINFO_FLAG_MULTI (1<<5)

/* Here we take all the structures that the module pass to the core
 * and the other way around. Notably the list here contains the structures
 * used by the hooks API RedisModule_RegisterToServerEvent().
 *
 * The structures always start with a 'version' field. This is useful
 * when we want to pass a reference to the structure to the core APIs,
 * for the APIs to fill the structure. In that case, the structure 'version'
 * field is initialized before passing it to the core, so that the core is
 * able to cast the pointer to the appropriate structure version. In this
 * way we obtain ABI compatibility.
 *
 * Here we'll list all the structure versions in case they evolve over time,
 * however using a define, we'll make sure to use the last version as the
 * public name for the module to use. */

#define REDISMODULE_CLIENTINFO_VERSION 1
typedef struct RedisModuleClientInfo {
    uint64_t version;       /* Version of this structure for ABI compat. */
    uint64_t flags;         /* REDISMODULE_CLIENTINFO_FLAG_* */
    uint64_t id;            /* Client ID. */
    char addr[46];          /* IPv4 or IPv6 address. */
    uint16_t port;          /* TCP port. */
    uint16_t db;            /* Selected DB. */
} RedisModuleClientInfoV1;

#define RedisModuleClientInfo RedisModuleClientInfoV1

#define REDISMODULE_REPLICATIONINFO_VERSION 1
typedef struct RedisModuleReplicationInfo {
    uint64_t version;       /* Not used since this structure is never passed
                               from the module to the core right now. Here
                               for future compatibility. */
    int master;             /* true if master, false if replica */
    char *masterhost;       /* master instance hostname for NOW_REPLICA */
    int masterport;         /* master instance port for NOW_REPLICA */
    char *replid1;          /* Main replication ID */
    char *replid2;          /* Secondary replication ID */
    uint64_t repl1_offset;  /* Main replication offset */
    uint64_t repl2_offset;  /* Offset of replid2 validity */
} RedisModuleReplicationInfoV1;

#define RedisModuleReplicationInfo RedisModuleReplicationInfoV1

#define REDISMODULE_FLUSHINFO_VERSION 1
typedef struct RedisModuleFlushInfo {
    uint64_t version;       /* Not used since this structure is never passed
                               from the module to the core right now. Here
                               for future compatibility. */
    int32_t sync;           /* Synchronous or threaded flush?. */
    int32_t dbnum;          /* Flushed database number, -1 for ALL. */
} RedisModuleFlushInfoV1;

#define RedisModuleFlushInfo RedisModuleFlushInfoV1

#define REDISMODULE_MODULE_CHANGE_VERSION 1
typedef struct RedisModuleModuleChange {
    uint64_t version;       /* Not used since this structure is never passed
                               from the module to the core right now. Here
                               for future compatibility. */
    const char* module_name;/* Name of module loaded or unloaded. */
    int32_t module_version; /* Module version. */
} RedisModuleModuleChangeV1;

#define RedisModuleModuleChange RedisModuleModuleChangeV1

#define REDISMODULE_CRON_LOOP_VERSION 1
typedef struct RedisModuleCronLoopInfo {
    uint64_t version;       /* Not used since this structure is never passed
                               from the module to the core right now. Here
                               for future compatibility. */
    int32_t hz;             /* Approximate number of events per second. */
} RedisModuleCronLoopV1;

#define RedisModuleCronLoop RedisModuleCronLoopV1

#define REDISMODULE_LOADING_PROGRESS_VERSION 1
typedef struct RedisModuleLoadingProgressInfo {
    uint64_t version;       /* Not used since this structure is never passed
                               from the module to the core right now. Here
                               for future compatibility. */
    int32_t hz;             /* Approximate number of events per second. */
    int32_t progress;       /* Approximate progress between 0 and 1024, or -1
                             * if unknown. */
} RedisModuleLoadingProgressV1;

#define RedisModuleLoadingProgress RedisModuleLoadingProgressV1

#define REDISMODULE_SWAPDBINFO_VERSION 1
typedef struct RedisModuleSwapDbInfo {
    uint64_t version;       /* Not used since this structure is never passed
                               from the module to the core right now. Here
                               for future compatibility. */
    int32_t dbnum_first;    /* Swap Db first dbnum */
    int32_t dbnum_second;   /* Swap Db second dbnum */
} RedisModuleSwapDbInfoV1;

#define RedisModuleSwapDbInfo RedisModuleSwapDbInfoV1

/* ------------------------- End of common defines ------------------------ */

#ifndef REDISMODULE_CORE

typedef long long mstime_t;

/* Macro definitions specific to individual compilers */
#ifndef REDISMODULE_ATTR_UNUSED
#    ifdef __GNUC__
#        define REDISMODULE_ATTR_UNUSED __attribute__((unused))
#    else
#        define REDISMODULE_ATTR_UNUSED
#    endif
#endif

#ifndef REDISMODULE_ATTR_PRINTF
#    ifdef __GNUC__
#        define REDISMODULE_ATTR_PRINTF(idx,cnt) __attribute__((format(printf,idx,cnt)))
#    else
#        define REDISMODULE_ATTR_PRINTF(idx,cnt)
#    endif
#endif

#ifndef REDISMODULE_ATTR_COMMON
#    if defined(__GNUC__) && !defined(__clang__)
#        define REDISMODULE_ATTR_COMMON __attribute__((__common__))
#    else
#        define REDISMODULE_ATTR_COMMON
#    endif
#endif

/* Incomplete structures for compiler checks but opaque access. */
typedef struct RedisModuleCtx RedisModuleCtx;
typedef struct RedisModuleKey RedisModuleKey;
typedef struct RedisModuleString RedisModuleString;
typedef struct RedisModuleCallReply RedisModuleCallReply;
typedef struct RedisModuleIO RedisModuleIO;
typedef struct RedisModuleType RedisModuleType;
typedef struct RedisModuleDigest RedisModuleDigest;
typedef struct RedisModuleBlockedClient RedisModuleBlockedClient;
typedef struct RedisModuleClusterInfo RedisModuleClusterInfo;
typedef struct RedisModuleDict RedisModuleDict;
typedef struct RedisModuleDictIter RedisModuleDictIter;
typedef struct RedisModuleCommandFilterCtx RedisModuleCommandFilterCtx;
typedef struct RedisModuleCommandFilter RedisModuleCommandFilter;
typedef struct RedisModuleInfoCtx RedisModuleInfoCtx;
typedef struct RedisModuleServerInfoData RedisModuleServerInfoData;
typedef struct RedisModuleScanCursor RedisModuleScanCursor;
typedef struct RedisModuleDefragCtx RedisModuleDefragCtx;
typedef struct RedisModuleUser RedisModuleUser;

typedef int (*RedisModuleCmdFunc)(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
typedef void (*RedisModuleDisconnectFunc)(RedisModuleCtx *ctx, RedisModuleBlockedClient *bc);
typedef int (*RedisModuleNotificationFunc)(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key);
typedef void *(*RedisModuleTypeLoadFunc)(RedisModuleIO *rdb, int encver);
typedef void (*RedisModuleTypeSaveFunc)(RedisModuleIO *rdb, void *value);
typedef int (*RedisModuleTypeAuxLoadFunc)(RedisModuleIO *rdb, int encver, int when);
typedef void (*RedisModuleTypeAuxSaveFunc)(RedisModuleIO *rdb, int when);
typedef void (*RedisModuleTypeRewriteFunc)(RedisModuleIO *aof, RedisModuleString *key, void *value);
typedef size_t (*RedisModuleTypeMemUsageFunc)(const void *value);
typedef void (*RedisModuleTypeDigestFunc)(RedisModuleDigest *digest, void *value);
typedef void (*RedisModuleTypeFreeFunc)(void *value);
typedef size_t (*RedisModuleTypeFreeEffortFunc)(RedisModuleString *key, const void *value);
typedef void (*RedisModuleTypeUnlinkFunc)(RedisModuleString *key, const void *value);
typedef void *(*RedisModuleTypeCopyFunc)(RedisModuleString *fromkey, RedisModuleString *tokey, const void *value);
typedef int (*RedisModuleTypeDefragFunc)(RedisModuleDefragCtx *ctx, RedisModuleString *key, void **value);
typedef void (*RedisModuleClusterMessageReceiver)(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len);
typedef void (*RedisModuleTimerProc)(RedisModuleCtx *ctx, void *data);
typedef void (*RedisModuleCommandFilterFunc) (RedisModuleCommandFilterCtx *filter);
typedef void (*RedisModuleForkDoneHandler) (int exitcode, int bysignal, void *user_data);
typedef void (*RedisModuleInfoFunc)(RedisModuleInfoCtx *ctx, int for_crash_report);
typedef void (*RedisModuleScanCB)(RedisModuleCtx *ctx, RedisModuleString *keyname, RedisModuleKey *key, void *privdata);
typedef void (*RedisModuleScanKeyCB)(RedisModuleKey *key, RedisModuleString *field, RedisModuleString *value, void *privdata);
typedef void (*RedisModuleUserChangedFunc) (uint64_t client_id, void *privdata);
typedef int (*RedisModuleDefragFunc)(RedisModuleDefragCtx *ctx);

typedef struct RedisModuleTypeMethods {
    uint64_t version;
    RedisModuleTypeLoadFunc rdb_load;
    RedisModuleTypeSaveFunc rdb_save;
    RedisModuleTypeRewriteFunc aof_rewrite;
    RedisModuleTypeMemUsageFunc mem_usage;
    RedisModuleTypeDigestFunc digest;
    RedisModuleTypeFreeFunc free;
    RedisModuleTypeAuxLoadFunc aux_load;
    RedisModuleTypeAuxSaveFunc aux_save;
    int aux_save_triggers;
    RedisModuleTypeFreeEffortFunc free_effort;
    RedisModuleTypeUnlinkFunc unlink;
    RedisModuleTypeCopyFunc copy;
    RedisModuleTypeDefragFunc defrag;
} RedisModuleTypeMethods;

#define REDISMODULE_GET_API(name) \
    RedisModule_GetApi("RedisModule_" #name, ((void **)&RedisModule_ ## name))

/* Default API declaration prefix (not 'extern' for backwards compatibility) */
#ifndef REDISMODULE_API
#define REDISMODULE_API
#endif

/* Default API declaration suffix (compiler attributes) */
#ifndef REDISMODULE_ATTR
#define REDISMODULE_ATTR REDISMODULE_ATTR_COMMON
#endif

REDISMODULE_API void * (*RedisModule_Alloc)(size_t bytes) REDISMODULE_ATTR;
REDISMODULE_API void * (*RedisModule_Realloc)(void *ptr, size_t bytes) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_Free)(void *ptr) REDISMODULE_ATTR;
REDISMODULE_API void * (*RedisModule_Calloc)(size_t nmemb, size_t size) REDISMODULE_ATTR;
REDISMODULE_API char * (*RedisModule_Strdup)(const char *str) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_GetApi)(const char *, void *) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_CreateCommand)(RedisModuleCtx *ctx, const char *name, RedisModuleCmdFunc cmdfunc, const char *strflags, int firstkey, int lastkey, int keystep) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_SetModuleAttribs)(RedisModuleCtx *ctx, const char *name, int ver, int apiver) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_IsModuleNameBusy)(const char *name) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_WrongArity)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ReplyWithLongLong)(RedisModuleCtx *ctx, long long ll) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_GetSelectedDb)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_SelectDb)(RedisModuleCtx *ctx, int newid) REDISMODULE_ATTR;
REDISMODULE_API void * (*RedisModule_OpenKey)(RedisModuleCtx *ctx, RedisModuleString *keyname, int mode) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_CloseKey)(RedisModuleKey *kp) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_KeyType)(RedisModuleKey *kp) REDISMODULE_ATTR;
REDISMODULE_API size_t (*RedisModule_ValueLength)(RedisModuleKey *kp) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ListPush)(RedisModuleKey *kp, int where, RedisModuleString *ele) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_ListPop)(RedisModuleKey *key, int where) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleCallReply * (*RedisModule_Call)(RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...) REDISMODULE_ATTR;
REDISMODULE_API const char * (*RedisModule_CallReplyProto)(RedisModuleCallReply *reply, size_t *len) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_FreeCallReply)(RedisModuleCallReply *reply) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_CallReplyType)(RedisModuleCallReply *reply) REDISMODULE_ATTR;
REDISMODULE_API long long (*RedisModule_CallReplyInteger)(RedisModuleCallReply *reply) REDISMODULE_ATTR;
REDISMODULE_API size_t (*RedisModule_CallReplyLength)(RedisModuleCallReply *reply) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleCallReply * (*RedisModule_CallReplyArrayElement)(RedisModuleCallReply *reply, size_t idx) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_CreateString)(RedisModuleCtx *ctx, const char *ptr, size_t len) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_CreateStringFromLongLong)(RedisModuleCtx *ctx, long long ll) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_CreateStringFromDouble)(RedisModuleCtx *ctx, double d) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_CreateStringFromLongDouble)(RedisModuleCtx *ctx, long double ld, int humanfriendly) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_CreateStringFromString)(RedisModuleCtx *ctx, const RedisModuleString *str) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_CreateStringFromStreamID)(RedisModuleCtx *ctx, const RedisModuleStreamID *id) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_CreateStringPrintf)(RedisModuleCtx *ctx, const char *fmt, ...) REDISMODULE_ATTR_PRINTF(2,3) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_FreeString)(RedisModuleCtx *ctx, RedisModuleString *str) REDISMODULE_ATTR;
REDISMODULE_API const char * (*RedisModule_StringPtrLen)(const RedisModuleString *str, size_t *len) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ReplyWithError)(RedisModuleCtx *ctx, const char *err) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ReplyWithSimpleString)(RedisModuleCtx *ctx, const char *msg) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ReplyWithArray)(RedisModuleCtx *ctx, long len) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ReplyWithNullArray)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ReplyWithEmptyArray)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_ReplySetArrayLength)(RedisModuleCtx *ctx, long len) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ReplyWithStringBuffer)(RedisModuleCtx *ctx, const char *buf, size_t len) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ReplyWithCString)(RedisModuleCtx *ctx, const char *buf) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ReplyWithString)(RedisModuleCtx *ctx, RedisModuleString *str) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ReplyWithEmptyString)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ReplyWithVerbatimString)(RedisModuleCtx *ctx, const char *buf, size_t len) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ReplyWithNull)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ReplyWithDouble)(RedisModuleCtx *ctx, double d) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ReplyWithLongDouble)(RedisModuleCtx *ctx, long double d) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ReplyWithCallReply)(RedisModuleCtx *ctx, RedisModuleCallReply *reply) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_StringToLongLong)(const RedisModuleString *str, long long *ll) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_StringToDouble)(const RedisModuleString *str, double *d) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_StringToLongDouble)(const RedisModuleString *str, long double *d) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_StringToStreamID)(const RedisModuleString *str, RedisModuleStreamID *id) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_AutoMemory)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_Replicate)(RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ReplicateVerbatim)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API const char * (*RedisModule_CallReplyStringPtr)(RedisModuleCallReply *reply, size_t *len) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_CreateStringFromCallReply)(RedisModuleCallReply *reply) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_DeleteKey)(RedisModuleKey *key) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_UnlinkKey)(RedisModuleKey *key) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_StringSet)(RedisModuleKey *key, RedisModuleString *str) REDISMODULE_ATTR;
REDISMODULE_API char * (*RedisModule_StringDMA)(RedisModuleKey *key, size_t *len, int mode) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_StringTruncate)(RedisModuleKey *key, size_t newlen) REDISMODULE_ATTR;
REDISMODULE_API mstime_t (*RedisModule_GetExpire)(RedisModuleKey *key) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_SetExpire)(RedisModuleKey *key, mstime_t expire) REDISMODULE_ATTR;
REDISMODULE_API mstime_t (*RedisModule_GetAbsExpire)(RedisModuleKey *key) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_SetAbsExpire)(RedisModuleKey *key, mstime_t expire) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_ResetDataset)(int restart_aof, int async) REDISMODULE_ATTR;
REDISMODULE_API unsigned long long (*RedisModule_DbSize)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_RandomKey)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ZsetAdd)(RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ZsetIncrby)(RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr, double *newscore) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ZsetScore)(RedisModuleKey *key, RedisModuleString *ele, double *score) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ZsetRem)(RedisModuleKey *key, RedisModuleString *ele, int *deleted) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_ZsetRangeStop)(RedisModuleKey *key) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ZsetFirstInScoreRange)(RedisModuleKey *key, double min, double max, int minex, int maxex) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ZsetLastInScoreRange)(RedisModuleKey *key, double min, double max, int minex, int maxex) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ZsetFirstInLexRange)(RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ZsetLastInLexRange)(RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_ZsetRangeCurrentElement)(RedisModuleKey *key, double *score) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ZsetRangeNext)(RedisModuleKey *key) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ZsetRangePrev)(RedisModuleKey *key) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ZsetRangeEndReached)(RedisModuleKey *key) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_HashSet)(RedisModuleKey *key, int flags, ...) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_HashGet)(RedisModuleKey *key, int flags, ...) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_StreamAdd)(RedisModuleKey *key, int flags, RedisModuleStreamID *id, RedisModuleString **argv, int64_t numfields) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_StreamDelete)(RedisModuleKey *key, RedisModuleStreamID *id) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_StreamIteratorStart)(RedisModuleKey *key, int flags, RedisModuleStreamID *startid, RedisModuleStreamID *endid) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_StreamIteratorStop)(RedisModuleKey *key) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_StreamIteratorNextID)(RedisModuleKey *key, RedisModuleStreamID *id, long *numfields) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_StreamIteratorNextField)(RedisModuleKey *key, RedisModuleString **field_ptr, RedisModuleString **value_ptr) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_StreamIteratorDelete)(RedisModuleKey *key) REDISMODULE_ATTR;
REDISMODULE_API long long (*RedisModule_StreamTrimByLength)(RedisModuleKey *key, int flags, long long length) REDISMODULE_ATTR;
REDISMODULE_API long long (*RedisModule_StreamTrimByID)(RedisModuleKey *key, int flags, RedisModuleStreamID *id) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_IsKeysPositionRequest)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_KeyAtPos)(RedisModuleCtx *ctx, int pos) REDISMODULE_ATTR;
REDISMODULE_API unsigned long long (*RedisModule_GetClientId)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_GetClientUserNameById)(RedisModuleCtx *ctx, uint64_t id) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_GetClientInfoById)(void *ci, uint64_t id) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_PublishMessage)(RedisModuleCtx *ctx, RedisModuleString *channel, RedisModuleString *message) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_GetContextFlags)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_AvoidReplicaTraffic)() REDISMODULE_ATTR;
REDISMODULE_API void * (*RedisModule_PoolAlloc)(RedisModuleCtx *ctx, size_t bytes) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleType * (*RedisModule_CreateDataType)(RedisModuleCtx *ctx, const char *name, int encver, RedisModuleTypeMethods *typemethods) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ModuleTypeSetValue)(RedisModuleKey *key, RedisModuleType *mt, void *value) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ModuleTypeReplaceValue)(RedisModuleKey *key, RedisModuleType *mt, void *new_value, void **old_value) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleType * (*RedisModule_ModuleTypeGetType)(RedisModuleKey *key) REDISMODULE_ATTR;
REDISMODULE_API void * (*RedisModule_ModuleTypeGetValue)(RedisModuleKey *key) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_IsIOError)(RedisModuleIO *io) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_SetModuleOptions)(RedisModuleCtx *ctx, int options) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_SignalModifiedKey)(RedisModuleCtx *ctx, RedisModuleString *keyname) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_SaveUnsigned)(RedisModuleIO *io, uint64_t value) REDISMODULE_ATTR;
REDISMODULE_API uint64_t (*RedisModule_LoadUnsigned)(RedisModuleIO *io) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_SaveSigned)(RedisModuleIO *io, int64_t value) REDISMODULE_ATTR;
REDISMODULE_API int64_t (*RedisModule_LoadSigned)(RedisModuleIO *io) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_EmitAOF)(RedisModuleIO *io, const char *cmdname, const char *fmt, ...) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_SaveString)(RedisModuleIO *io, RedisModuleString *s) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_SaveStringBuffer)(RedisModuleIO *io, const char *str, size_t len) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_LoadString)(RedisModuleIO *io) REDISMODULE_ATTR;
REDISMODULE_API char * (*RedisModule_LoadStringBuffer)(RedisModuleIO *io, size_t *lenptr) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_SaveDouble)(RedisModuleIO *io, double value) REDISMODULE_ATTR;
REDISMODULE_API double (*RedisModule_LoadDouble)(RedisModuleIO *io) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_SaveFloat)(RedisModuleIO *io, float value) REDISMODULE_ATTR;
REDISMODULE_API float (*RedisModule_LoadFloat)(RedisModuleIO *io) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_SaveLongDouble)(RedisModuleIO *io, long double value) REDISMODULE_ATTR;
REDISMODULE_API long double (*RedisModule_LoadLongDouble)(RedisModuleIO *io) REDISMODULE_ATTR;
REDISMODULE_API void * (*RedisModule_LoadDataTypeFromString)(const RedisModuleString *str, const RedisModuleType *mt) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_SaveDataTypeToString)(RedisModuleCtx *ctx, void *data, const RedisModuleType *mt) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_Log)(RedisModuleCtx *ctx, const char *level, const char *fmt, ...) REDISMODULE_ATTR REDISMODULE_ATTR_PRINTF(3,4);
REDISMODULE_API void (*RedisModule_LogIOError)(RedisModuleIO *io, const char *levelstr, const char *fmt, ...) REDISMODULE_ATTR REDISMODULE_ATTR_PRINTF(3,4);
REDISMODULE_API void (*RedisModule__Assert)(const char *estr, const char *file, int line) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_LatencyAddSample)(const char *event, mstime_t latency) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_StringAppendBuffer)(RedisModuleCtx *ctx, RedisModuleString *str, const char *buf, size_t len) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_RetainString)(RedisModuleCtx *ctx, RedisModuleString *str) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_HoldString)(RedisModuleCtx *ctx, RedisModuleString *str) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_StringCompare)(RedisModuleString *a, RedisModuleString *b) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleCtx * (*RedisModule_GetContextFromIO)(RedisModuleIO *io) REDISMODULE_ATTR;
REDISMODULE_API const RedisModuleString * (*RedisModule_GetKeyNameFromIO)(RedisModuleIO *io) REDISMODULE_ATTR;
REDISMODULE_API const RedisModuleString * (*RedisModule_GetKeyNameFromModuleKey)(RedisModuleKey *key) REDISMODULE_ATTR;
REDISMODULE_API long long (*RedisModule_Milliseconds)(void) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_DigestAddStringBuffer)(RedisModuleDigest *md, unsigned char *ele, size_t len) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_DigestAddLongLong)(RedisModuleDigest *md, long long ele) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_DigestEndSequence)(RedisModuleDigest *md) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleDict * (*RedisModule_CreateDict)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_FreeDict)(RedisModuleCtx *ctx, RedisModuleDict *d) REDISMODULE_ATTR;
REDISMODULE_API uint64_t (*RedisModule_DictSize)(RedisModuleDict *d) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_DictSetC)(RedisModuleDict *d, void *key, size_t keylen, void *ptr) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_DictReplaceC)(RedisModuleDict *d, void *key, size_t keylen, void *ptr) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_DictSet)(RedisModuleDict *d, RedisModuleString *key, void *ptr) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_DictReplace)(RedisModuleDict *d, RedisModuleString *key, void *ptr) REDISMODULE_ATTR;
REDISMODULE_API void * (*RedisModule_DictGetC)(RedisModuleDict *d, void *key, size_t keylen, int *nokey) REDISMODULE_ATTR;
REDISMODULE_API void * (*RedisModule_DictGet)(RedisModuleDict *d, RedisModuleString *key, int *nokey) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_DictDelC)(RedisModuleDict *d, void *key, size_t keylen, void *oldval) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_DictDel)(RedisModuleDict *d, RedisModuleString *key, void *oldval) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleDictIter * (*RedisModule_DictIteratorStartC)(RedisModuleDict *d, const char *op, void *key, size_t keylen) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleDictIter * (*RedisModule_DictIteratorStart)(RedisModuleDict *d, const char *op, RedisModuleString *key) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_DictIteratorStop)(RedisModuleDictIter *di) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_DictIteratorReseekC)(RedisModuleDictIter *di, const char *op, void *key, size_t keylen) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_DictIteratorReseek)(RedisModuleDictIter *di, const char *op, RedisModuleString *key) REDISMODULE_ATTR;
REDISMODULE_API void * (*RedisModule_DictNextC)(RedisModuleDictIter *di, size_t *keylen, void **dataptr) REDISMODULE_ATTR;
REDISMODULE_API void * (*RedisModule_DictPrevC)(RedisModuleDictIter *di, size_t *keylen, void **dataptr) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_DictNext)(RedisModuleCtx *ctx, RedisModuleDictIter *di, void **dataptr) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_DictPrev)(RedisModuleCtx *ctx, RedisModuleDictIter *di, void **dataptr) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_DictCompareC)(RedisModuleDictIter *di, const char *op, void *key, size_t keylen) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_DictCompare)(RedisModuleDictIter *di, const char *op, RedisModuleString *key) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_RegisterInfoFunc)(RedisModuleCtx *ctx, RedisModuleInfoFunc cb) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_InfoAddSection)(RedisModuleInfoCtx *ctx, char *name) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_InfoBeginDictField)(RedisModuleInfoCtx *ctx, char *name) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_InfoEndDictField)(RedisModuleInfoCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_InfoAddFieldString)(RedisModuleInfoCtx *ctx, char *field, RedisModuleString *value) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_InfoAddFieldCString)(RedisModuleInfoCtx *ctx, char *field, char *value) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_InfoAddFieldDouble)(RedisModuleInfoCtx *ctx, char *field, double value) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_InfoAddFieldLongLong)(RedisModuleInfoCtx *ctx, char *field, long long value) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_InfoAddFieldULongLong)(RedisModuleInfoCtx *ctx, char *field, unsigned long long value) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleServerInfoData * (*RedisModule_GetServerInfo)(RedisModuleCtx *ctx, const char *section) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_FreeServerInfo)(RedisModuleCtx *ctx, RedisModuleServerInfoData *data) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_ServerInfoGetField)(RedisModuleCtx *ctx, RedisModuleServerInfoData *data, const char* field) REDISMODULE_ATTR;
REDISMODULE_API const char * (*RedisModule_ServerInfoGetFieldC)(RedisModuleServerInfoData *data, const char* field) REDISMODULE_ATTR;
REDISMODULE_API long long (*RedisModule_ServerInfoGetFieldSigned)(RedisModuleServerInfoData *data, const char* field, int *out_err) REDISMODULE_ATTR;
REDISMODULE_API unsigned long long (*RedisModule_ServerInfoGetFieldUnsigned)(RedisModuleServerInfoData *data, const char* field, int *out_err) REDISMODULE_ATTR;
REDISMODULE_API double (*RedisModule_ServerInfoGetFieldDouble)(RedisModuleServerInfoData *data, const char* field, int *out_err) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_SubscribeToServerEvent)(RedisModuleCtx *ctx, RedisModuleEvent event, RedisModuleEventCallback callback) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_SetLRU)(RedisModuleKey *key, mstime_t lru_idle) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_GetLRU)(RedisModuleKey *key, mstime_t *lru_idle) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_SetLFU)(RedisModuleKey *key, long long lfu_freq) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_GetLFU)(RedisModuleKey *key, long long *lfu_freq) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleBlockedClient * (*RedisModule_BlockClientOnKeys)(RedisModuleCtx *ctx, RedisModuleCmdFunc reply_callback, RedisModuleCmdFunc timeout_callback, void (*free_privdata)(RedisModuleCtx*,void*), long long timeout_ms, RedisModuleString **keys, int numkeys, void *privdata) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_SignalKeyAsReady)(RedisModuleCtx *ctx, RedisModuleString *key) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_GetBlockedClientReadyKey)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleScanCursor * (*RedisModule_ScanCursorCreate)() REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_ScanCursorRestart)(RedisModuleScanCursor *cursor) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_ScanCursorDestroy)(RedisModuleScanCursor *cursor) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_Scan)(RedisModuleCtx *ctx, RedisModuleScanCursor *cursor, RedisModuleScanCB fn, void *privdata) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ScanKey)(RedisModuleKey *key, RedisModuleScanCursor *cursor, RedisModuleScanKeyCB fn, void *privdata) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_GetContextFlagsAll)() REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_GetKeyspaceNotificationFlagsAll)() REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_IsSubEventSupported)(RedisModuleEvent event, uint64_t subevent) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_GetServerVersion)() REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_GetTypeMethodVersion)() REDISMODULE_ATTR;

/* Experimental APIs */
#ifdef REDISMODULE_EXPERIMENTAL_API
#define REDISMODULE_EXPERIMENTAL_API_VERSION 3
REDISMODULE_API RedisModuleBlockedClient * (*RedisModule_BlockClient)(RedisModuleCtx *ctx, RedisModuleCmdFunc reply_callback, RedisModuleCmdFunc timeout_callback, void (*free_privdata)(RedisModuleCtx*,void*), long long timeout_ms) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_UnblockClient)(RedisModuleBlockedClient *bc, void *privdata) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_IsBlockedReplyRequest)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_IsBlockedTimeoutRequest)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API void * (*RedisModule_GetBlockedClientPrivateData)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleBlockedClient * (*RedisModule_GetBlockedClientHandle)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_AbortBlock)(RedisModuleBlockedClient *bc) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_BlockedClientMeasureTimeStart)(RedisModuleBlockedClient *bc) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_BlockedClientMeasureTimeEnd)(RedisModuleBlockedClient *bc) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleCtx * (*RedisModule_GetThreadSafeContext)(RedisModuleBlockedClient *bc) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleCtx * (*RedisModule_GetDetachedThreadSafeContext)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_FreeThreadSafeContext)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_ThreadSafeContextLock)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ThreadSafeContextTryLock)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_ThreadSafeContextUnlock)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_SubscribeToKeyspaceEvents)(RedisModuleCtx *ctx, int types, RedisModuleNotificationFunc cb) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_NotifyKeyspaceEvent)(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_GetNotifyKeyspaceEvents)() REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_BlockedClientDisconnected)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_RegisterClusterMessageReceiver)(RedisModuleCtx *ctx, uint8_t type, RedisModuleClusterMessageReceiver callback) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_SendClusterMessage)(RedisModuleCtx *ctx, char *target_id, uint8_t type, unsigned char *msg, uint32_t len) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_GetClusterNodeInfo)(RedisModuleCtx *ctx, const char *id, char *ip, char *master_id, int *port, int *flags) REDISMODULE_ATTR;
REDISMODULE_API char ** (*RedisModule_GetClusterNodesList)(RedisModuleCtx *ctx, size_t *numnodes) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_FreeClusterNodesList)(char **ids) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleTimerID (*RedisModule_CreateTimer)(RedisModuleCtx *ctx, mstime_t period, RedisModuleTimerProc callback, void *data) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_StopTimer)(RedisModuleCtx *ctx, RedisModuleTimerID id, void **data) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_GetTimerInfo)(RedisModuleCtx *ctx, RedisModuleTimerID id, uint64_t *remaining, void **data) REDISMODULE_ATTR;
REDISMODULE_API const char * (*RedisModule_GetMyClusterID)(void) REDISMODULE_ATTR;
REDISMODULE_API size_t (*RedisModule_GetClusterSize)(void) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_GetRandomBytes)(unsigned char *dst, size_t len) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_GetRandomHexChars)(char *dst, size_t len) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_SetDisconnectCallback)(RedisModuleBlockedClient *bc, RedisModuleDisconnectFunc callback) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_SetClusterFlags)(RedisModuleCtx *ctx, uint64_t flags) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ExportSharedAPI)(RedisModuleCtx *ctx, const char *apiname, void *func) REDISMODULE_ATTR;
REDISMODULE_API void * (*RedisModule_GetSharedAPI)(RedisModuleCtx *ctx, const char *apiname) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleCommandFilter * (*RedisModule_RegisterCommandFilter)(RedisModuleCtx *ctx, RedisModuleCommandFilterFunc cb, int flags) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_UnregisterCommandFilter)(RedisModuleCtx *ctx, RedisModuleCommandFilter *filter) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_CommandFilterArgsCount)(RedisModuleCommandFilterCtx *fctx) REDISMODULE_ATTR;
REDISMODULE_API const RedisModuleString * (*RedisModule_CommandFilterArgGet)(RedisModuleCommandFilterCtx *fctx, int pos) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_CommandFilterArgInsert)(RedisModuleCommandFilterCtx *fctx, int pos, RedisModuleString *arg) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_CommandFilterArgReplace)(RedisModuleCommandFilterCtx *fctx, int pos, RedisModuleString *arg) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_CommandFilterArgDelete)(RedisModuleCommandFilterCtx *fctx, int pos) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_Fork)(RedisModuleForkDoneHandler cb, void *user_data) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_SendChildHeartbeat)(double progress) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_ExitFromChild)(int retcode) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_KillForkChild)(int child_pid) REDISMODULE_ATTR;
REDISMODULE_API float (*RedisModule_GetUsedMemoryRatio)() REDISMODULE_ATTR;
REDISMODULE_API size_t (*RedisModule_MallocSize)(void* ptr) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleUser * (*RedisModule_CreateModuleUser)(const char *name) REDISMODULE_ATTR;
REDISMODULE_API void (*RedisModule_FreeModuleUser)(RedisModuleUser *user) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_SetModuleUserACL)(RedisModuleUser *user, const char* acl) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_AuthenticateClientWithACLUser)(RedisModuleCtx *ctx, const char *name, size_t len, RedisModuleUserChangedFunc callback, void *privdata, uint64_t *client_id) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_AuthenticateClientWithUser)(RedisModuleCtx *ctx, RedisModuleUser *user, RedisModuleUserChangedFunc callback, void *privdata, uint64_t *client_id) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_DeauthenticateAndCloseClient)(RedisModuleCtx *ctx, uint64_t client_id) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString * (*RedisModule_GetClientCertificate)(RedisModuleCtx *ctx, uint64_t id) REDISMODULE_ATTR;
REDISMODULE_API int *(*RedisModule_GetCommandKeys)(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, int *num_keys) REDISMODULE_ATTR;
REDISMODULE_API const char *(*RedisModule_GetCurrentCommandName)(RedisModuleCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_RegisterDefragFunc)(RedisModuleCtx *ctx, RedisModuleDefragFunc func) REDISMODULE_ATTR;
REDISMODULE_API void *(*RedisModule_DefragAlloc)(RedisModuleDefragCtx *ctx, void *ptr) REDISMODULE_ATTR;
REDISMODULE_API RedisModuleString *(*RedisModule_DefragRedisModuleString)(RedisModuleDefragCtx *ctx, RedisModuleString *str) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_DefragShouldStop)(RedisModuleDefragCtx *ctx) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_DefragCursorSet)(RedisModuleDefragCtx *ctx, unsigned long cursor) REDISMODULE_ATTR;
REDISMODULE_API int (*RedisModule_DefragCursorGet)(RedisModuleDefragCtx *ctx, unsigned long *cursor) REDISMODULE_ATTR;
#endif

#define RedisModule_IsAOFClient(id) ((id) == UINT64_MAX)

/* This is included inline inside each Redis module. */
static int RedisModule_Init(RedisModuleCtx *ctx, const char *name, int ver, int apiver) REDISMODULE_ATTR_UNUSED;
static int RedisModule_Init(RedisModuleCtx *ctx, const char *name, int ver, int apiver) {
    void *getapifuncptr = ((void**)ctx)[0];
    RedisModule_GetApi = (int (*)(const char *, void *)) (unsigned long)getapifuncptr;
    REDISMODULE_GET_API(Alloc);
    REDISMODULE_GET_API(Calloc);
    REDISMODULE_GET_API(Free);
    REDISMODULE_GET_API(Realloc);
    REDISMODULE_GET_API(Strdup);
    REDISMODULE_GET_API(CreateCommand);
    REDISMODULE_GET_API(SetModuleAttribs);
    REDISMODULE_GET_API(IsModuleNameBusy);
    REDISMODULE_GET_API(WrongArity);
    REDISMODULE_GET_API(ReplyWithLongLong);
    REDISMODULE_GET_API(ReplyWithError);
    REDISMODULE_GET_API(ReplyWithSimpleString);
    REDISMODULE_GET_API(ReplyWithArray);
    REDISMODULE_GET_API(ReplyWithNullArray);
    REDISMODULE_GET_API(ReplyWithEmptyArray);
    REDISMODULE_GET_API(ReplySetArrayLength);
    REDISMODULE_GET_API(ReplyWithStringBuffer);
    REDISMODULE_GET_API(ReplyWithCString);
    REDISMODULE_GET_API(ReplyWithString);
    REDISMODULE_GET_API(ReplyWithEmptyString);
    REDISMODULE_GET_API(ReplyWithVerbatimString);
    REDISMODULE_GET_API(ReplyWithNull);
    REDISMODULE_GET_API(ReplyWithCallReply);
    REDISMODULE_GET_API(ReplyWithDouble);
    REDISMODULE_GET_API(ReplyWithLongDouble);
    REDISMODULE_GET_API(GetSelectedDb);
    REDISMODULE_GET_API(SelectDb);
    REDISMODULE_GET_API(OpenKey);
    REDISMODULE_GET_API(CloseKey);
    REDISMODULE_GET_API(KeyType);
    REDISMODULE_GET_API(ValueLength);
    REDISMODULE_GET_API(ListPush);
    REDISMODULE_GET_API(ListPop);
    REDISMODULE_GET_API(StringToLongLong);
    REDISMODULE_GET_API(StringToDouble);
    REDISMODULE_GET_API(StringToLongDouble);
    REDISMODULE_GET_API(StringToStreamID);
    REDISMODULE_GET_API(Call);
    REDISMODULE_GET_API(CallReplyProto);
    REDISMODULE_GET_API(FreeCallReply);
    REDISMODULE_GET_API(CallReplyInteger);
    REDISMODULE_GET_API(CallReplyType);
    REDISMODULE_GET_API(CallReplyLength);
    REDISMODULE_GET_API(CallReplyArrayElement);
    REDISMODULE_GET_API(CallReplyStringPtr);
    REDISMODULE_GET_API(CreateStringFromCallReply);
    REDISMODULE_GET_API(CreateString);
    REDISMODULE_GET_API(CreateStringFromLongLong);
    REDISMODULE_GET_API(CreateStringFromDouble);
    REDISMODULE_GET_API(CreateStringFromLongDouble);
    REDISMODULE_GET_API(CreateStringFromString);
    REDISMODULE_GET_API(CreateStringFromStreamID);
    REDISMODULE_GET_API(CreateStringPrintf);
    REDISMODULE_GET_API(FreeString);
    REDISMODULE_GET_API(StringPtrLen);
    REDISMODULE_GET_API(AutoMemory);
    REDISMODULE_GET_API(Replicate);
    REDISMODULE_GET_API(ReplicateVerbatim);
    REDISMODULE_GET_API(DeleteKey);
    REDISMODULE_GET_API(UnlinkKey);
    REDISMODULE_GET_API(StringSet);
    REDISMODULE_GET_API(StringDMA);
    REDISMODULE_GET_API(StringTruncate);
    REDISMODULE_GET_API(GetExpire);
    REDISMODULE_GET_API(SetExpire);
    REDISMODULE_GET_API(GetAbsExpire);
    REDISMODULE_GET_API(SetAbsExpire);
    REDISMODULE_GET_API(ResetDataset);
    REDISMODULE_GET_API(DbSize);
    REDISMODULE_GET_API(RandomKey);
    REDISMODULE_GET_API(ZsetAdd);
    REDISMODULE_GET_API(ZsetIncrby);
    REDISMODULE_GET_API(ZsetScore);
    REDISMODULE_GET_API(ZsetRem);
    REDISMODULE_GET_API(ZsetRangeStop);
    REDISMODULE_GET_API(ZsetFirstInScoreRange);
    REDISMODULE_GET_API(ZsetLastInScoreRange);
    REDISMODULE_GET_API(ZsetFirstInLexRange);
    REDISMODULE_GET_API(ZsetLastInLexRange);
    REDISMODULE_GET_API(ZsetRangeCurrentElement);
    REDISMODULE_GET_API(ZsetRangeNext);
    REDISMODULE_GET_API(ZsetRangePrev);
    REDISMODULE_GET_API(ZsetRangeEndReached);
    REDISMODULE_GET_API(HashSet);
    REDISMODULE_GET_API(HashGet);
    REDISMODULE_GET_API(StreamAdd);
    REDISMODULE_GET_API(StreamDelete);
    REDISMODULE_GET_API(StreamIteratorStart);
    REDISMODULE_GET_API(StreamIteratorStop);
    REDISMODULE_GET_API(StreamIteratorNextID);
    REDISMODULE_GET_API(StreamIteratorNextField);
    REDISMODULE_GET_API(StreamIteratorDelete);
    REDISMODULE_GET_API(StreamTrimByLength);
    REDISMODULE_GET_API(StreamTrimByID);
    REDISMODULE_GET_API(IsKeysPositionRequest);
    REDISMODULE_GET_API(KeyAtPos);
    REDISMODULE_GET_API(GetClientId);
    REDISMODULE_GET_API(GetClientUserNameById);
    REDISMODULE_GET_API(GetContextFlags);
    REDISMODULE_GET_API(AvoidReplicaTraffic);
    REDISMODULE_GET_API(PoolAlloc);
    REDISMODULE_GET_API(CreateDataType);
    REDISMODULE_GET_API(ModuleTypeSetValue);
    REDISMODULE_GET_API(ModuleTypeReplaceValue);
    REDISMODULE_GET_API(ModuleTypeGetType);
    REDISMODULE_GET_API(ModuleTypeGetValue);
    REDISMODULE_GET_API(IsIOError);
    REDISMODULE_GET_API(SetModuleOptions);
    REDISMODULE_GET_API(SignalModifiedKey);
    REDISMODULE_GET_API(SaveUnsigned);
    REDISMODULE_GET_API(LoadUnsigned);
    REDISMODULE_GET_API(SaveSigned);
    REDISMODULE_GET_API(LoadSigned);
    REDISMODULE_GET_API(SaveString);
    REDISMODULE_GET_API(SaveStringBuffer);
    REDISMODULE_GET_API(LoadString);
    REDISMODULE_GET_API(LoadStringBuffer);
    REDISMODULE_GET_API(SaveDouble);
    REDISMODULE_GET_API(LoadDouble);
    REDISMODULE_GET_API(SaveFloat);
    REDISMODULE_GET_API(LoadFloat);
    REDISMODULE_GET_API(SaveLongDouble);
    REDISMODULE_GET_API(LoadLongDouble);
    REDISMODULE_GET_API(SaveDataTypeToString);
    REDISMODULE_GET_API(LoadDataTypeFromString);
    REDISMODULE_GET_API(EmitAOF);
    REDISMODULE_GET_API(Log);
    REDISMODULE_GET_API(LogIOError);
    REDISMODULE_GET_API(_Assert);
    REDISMODULE_GET_API(LatencyAddSample);
    REDISMODULE_GET_API(StringAppendBuffer);
    REDISMODULE_GET_API(RetainString);
    REDISMODULE_GET_API(HoldString);
    REDISMODULE_GET_API(StringCompare);
    REDISMODULE_GET_API(GetContextFromIO);
    REDISMODULE_GET_API(GetKeyNameFromIO);
    REDISMODULE_GET_API(GetKeyNameFromModuleKey);
    REDISMODULE_GET_API(Milliseconds);
    REDISMODULE_GET_API(DigestAddStringBuffer);
    REDISMODULE_GET_API(DigestAddLongLong);
    REDISMODULE_GET_API(DigestEndSequence);
    REDISMODULE_GET_API(CreateDict);
    REDISMODULE_GET_API(FreeDict);
    REDISMODULE_GET_API(DictSize);
    REDISMODULE_GET_API(DictSetC);
    REDISMODULE_GET_API(DictReplaceC);
    REDISMODULE_GET_API(DictSet);
    REDISMODULE_GET_API(DictReplace);
    REDISMODULE_GET_API(DictGetC);
    REDISMODULE_GET_API(DictGet);
    REDISMODULE_GET_API(DictDelC);
    REDISMODULE_GET_API(DictDel);
    REDISMODULE_GET_API(DictIteratorStartC);
    REDISMODULE_GET_API(DictIteratorStart);
    REDISMODULE_GET_API(DictIteratorStop);
    REDISMODULE_GET_API(DictIteratorReseekC);
    REDISMODULE_GET_API(DictIteratorReseek);
    REDISMODULE_GET_API(DictNextC);
    REDISMODULE_GET_API(DictPrevC);
    REDISMODULE_GET_API(DictNext);
    REDISMODULE_GET_API(DictPrev);
    REDISMODULE_GET_API(DictCompare);
    REDISMODULE_GET_API(DictCompareC);
    REDISMODULE_GET_API(RegisterInfoFunc);
    REDISMODULE_GET_API(InfoAddSection);
    REDISMODULE_GET_API(InfoBeginDictField);
    REDISMODULE_GET_API(InfoEndDictField);
    REDISMODULE_GET_API(InfoAddFieldString);
    REDISMODULE_GET_API(InfoAddFieldCString);
    REDISMODULE_GET_API(InfoAddFieldDouble);
    REDISMODULE_GET_API(InfoAddFieldLongLong);
    REDISMODULE_GET_API(InfoAddFieldULongLong);
    REDISMODULE_GET_API(GetServerInfo);
    REDISMODULE_GET_API(FreeServerInfo);
    REDISMODULE_GET_API(ServerInfoGetField);
    REDISMODULE_GET_API(ServerInfoGetFieldC);
    REDISMODULE_GET_API(ServerInfoGetFieldSigned);
    REDISMODULE_GET_API(ServerInfoGetFieldUnsigned);
    REDISMODULE_GET_API(ServerInfoGetFieldDouble);
    REDISMODULE_GET_API(GetClientInfoById);
    REDISMODULE_GET_API(PublishMessage);
    REDISMODULE_GET_API(SubscribeToServerEvent);
    REDISMODULE_GET_API(SetLRU);
    REDISMODULE_GET_API(GetLRU);
    REDISMODULE_GET_API(SetLFU);
    REDISMODULE_GET_API(GetLFU);
    REDISMODULE_GET_API(BlockClientOnKeys);
    REDISMODULE_GET_API(SignalKeyAsReady);
    REDISMODULE_GET_API(GetBlockedClientReadyKey);
    REDISMODULE_GET_API(ScanCursorCreate);
    REDISMODULE_GET_API(ScanCursorRestart);
    REDISMODULE_GET_API(ScanCursorDestroy);
    REDISMODULE_GET_API(Scan);
    REDISMODULE_GET_API(ScanKey);
    REDISMODULE_GET_API(GetContextFlagsAll);
    REDISMODULE_GET_API(GetKeyspaceNotificationFlagsAll);
    REDISMODULE_GET_API(IsSubEventSupported);
    REDISMODULE_GET_API(GetServerVersion);
    REDISMODULE_GET_API(GetTypeMethodVersion);

#ifdef REDISMODULE_EXPERIMENTAL_API
    REDISMODULE_GET_API(GetThreadSafeContext);
    REDISMODULE_GET_API(GetDetachedThreadSafeContext);
    REDISMODULE_GET_API(FreeThreadSafeContext);
    REDISMODULE_GET_API(ThreadSafeContextLock);
    REDISMODULE_GET_API(ThreadSafeContextTryLock);
    REDISMODULE_GET_API(ThreadSafeContextUnlock);
    REDISMODULE_GET_API(BlockClient);
    REDISMODULE_GET_API(UnblockClient);
    REDISMODULE_GET_API(IsBlockedReplyRequest);
    REDISMODULE_GET_API(IsBlockedTimeoutRequest);
    REDISMODULE_GET_API(GetBlockedClientPrivateData);
    REDISMODULE_GET_API(GetBlockedClientHandle);
    REDISMODULE_GET_API(AbortBlock);
    REDISMODULE_GET_API(BlockedClientMeasureTimeStart);
    REDISMODULE_GET_API(BlockedClientMeasureTimeEnd);
    REDISMODULE_GET_API(SetDisconnectCallback);
    REDISMODULE_GET_API(SubscribeToKeyspaceEvents);
    REDISMODULE_GET_API(NotifyKeyspaceEvent);
    REDISMODULE_GET_API(GetNotifyKeyspaceEvents);
    REDISMODULE_GET_API(BlockedClientDisconnected);
    REDISMODULE_GET_API(RegisterClusterMessageReceiver);
    REDISMODULE_GET_API(SendClusterMessage);
    REDISMODULE_GET_API(GetClusterNodeInfo);
    REDISMODULE_GET_API(GetClusterNodesList);
    REDISMODULE_GET_API(FreeClusterNodesList);
    REDISMODULE_GET_API(CreateTimer);
    REDISMODULE_GET_API(StopTimer);
    REDISMODULE_GET_API(GetTimerInfo);
    REDISMODULE_GET_API(GetMyClusterID);
    REDISMODULE_GET_API(GetClusterSize);
    REDISMODULE_GET_API(GetRandomBytes);
    REDISMODULE_GET_API(GetRandomHexChars);
    REDISMODULE_GET_API(SetClusterFlags);
    REDISMODULE_GET_API(ExportSharedAPI);
    REDISMODULE_GET_API(GetSharedAPI);
    REDISMODULE_GET_API(RegisterCommandFilter);
    REDISMODULE_GET_API(UnregisterCommandFilter);
    REDISMODULE_GET_API(CommandFilterArgsCount);
    REDISMODULE_GET_API(CommandFilterArgGet);
    REDISMODULE_GET_API(CommandFilterArgInsert);
    REDISMODULE_GET_API(CommandFilterArgReplace);
    REDISMODULE_GET_API(CommandFilterArgDelete);
    REDISMODULE_GET_API(Fork);
    REDISMODULE_GET_API(SendChildHeartbeat);
    REDISMODULE_GET_API(ExitFromChild);
    REDISMODULE_GET_API(KillForkChild);
    REDISMODULE_GET_API(GetUsedMemoryRatio);
    REDISMODULE_GET_API(MallocSize);
    REDISMODULE_GET_API(CreateModuleUser);
    REDISMODULE_GET_API(FreeModuleUser);
    REDISMODULE_GET_API(SetModuleUserACL);
    REDISMODULE_GET_API(DeauthenticateAndCloseClient);
    REDISMODULE_GET_API(AuthenticateClientWithACLUser);
    REDISMODULE_GET_API(AuthenticateClientWithUser);
    REDISMODULE_GET_API(GetClientCertificate);
    REDISMODULE_GET_API(GetCommandKeys);
    REDISMODULE_GET_API(GetCurrentCommandName);
    REDISMODULE_GET_API(RegisterDefragFunc);
    REDISMODULE_GET_API(DefragAlloc);
    REDISMODULE_GET_API(DefragRedisModuleString);
    REDISMODULE_GET_API(DefragShouldStop);
    REDISMODULE_GET_API(DefragCursorSet);
    REDISMODULE_GET_API(DefragCursorGet);
#endif

    if (RedisModule_IsModuleNameBusy && RedisModule_IsModuleNameBusy(name)) return REDISMODULE_ERR;
    RedisModule_SetModuleAttribs(ctx,name,ver,apiver);
    return REDISMODULE_OK;
}

#define RedisModule_Assert(_e) ((_e)?(void)0 : (RedisModule__Assert(#_e,__FILE__,__LINE__),exit(1)))

#define RMAPI_FUNC_SUPPORTED(func) (func != NULL)

#else

/* Things only defined for the modules core, not exported to modules
 * including this file. */
#define RedisModuleString robj

#endif /* REDISMODULE_CORE */
#endif /* REDISMODULE_H */

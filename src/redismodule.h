
#ifndef REDISMODULE_H
#define REDISMODULE_H

#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- Defines common between core and modules --------------- */

/* Error status return values. */
#define REDISMODULE_OK 0
#define REDISMODULE_ERR 1

/* API versions. */
#define REDISMODULE_APIVER_1 1

/* API flags and constants */
#define REDISMODULE_READ  (1<<0)
#define REDISMODULE_WRITE (1<<1)

/* RedisModule_OpenKey extra flags for the 'mode' argument.
 * Avoid touching the LRU/LFU of the key when opened. */
#define REDISMODULE_OPEN_KEY_NOTOUCH (1<<16)

#define REDISMODULE_LIST_HEAD 0
#define REDISMODULE_LIST_TAIL 1

/* Key types. */
#define REDISMODULE_KEYTYPE_EMPTY  0
#define REDISMODULE_KEYTYPE_STRING 1
#define REDISMODULE_KEYTYPE_LIST   2
#define REDISMODULE_KEYTYPE_HASH   3
#define REDISMODULE_KEYTYPE_SET    4
#define REDISMODULE_KEYTYPE_ZSET   5
#define REDISMODULE_KEYTYPE_MODULE 6

/* Reply types. */
#define REDISMODULE_REPLY_UNKNOWN   (-1)
#define REDISMODULE_REPLY_STRING    0
#define REDISMODULE_REPLY_ERROR     1
#define REDISMODULE_REPLY_INTEGER   2
#define REDISMODULE_REPLY_ARRAY     3
#define REDISMODULE_REPLY_NULL      4

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

/* Hash API flags. */
#define REDISMODULE_HASH_NONE       0
#define REDISMODULE_HASH_NX         (1<<0)
#define REDISMODULE_HASH_XX         (1<<1)
#define REDISMODULE_HASH_CFIELDS    (1<<2)
#define REDISMODULE_HASH_EXISTS     (1<<3)

/* Context Flags: Info about the current context returned by
 * RM_GetContextFlags(). */

/* The command is running in the context of a Lua script */
#define REDISMODULE_CTX_FLAGS_LUA         (1<<0)
/* The command is running inside a Redis transaction */
#define REDISMODULE_CTX_FLAGS_MULTI       (1<<1)
/* The instance is a master */
#define REDISMODULE_CTX_FLAGS_MASTER      (1<<2)
/* The instance is a slave */
#define REDISMODULE_CTX_FLAGS_SLAVE       (1<<3)
/* The instance is read-only (usually meaning it's a slave as well) */
#define REDISMODULE_CTX_FLAGS_READONLY    (1<<4)
/* The instance is running in cluster mode */
#define REDISMODULE_CTX_FLAGS_CLUSTER     (1<<5)
/* The instance has AOF enabled */
#define REDISMODULE_CTX_FLAGS_AOF         (1<<6)
/* The instance has RDB enabled */
#define REDISMODULE_CTX_FLAGS_RDB         (1<<7)
/* The instance has Maxmemory set */
#define REDISMODULE_CTX_FLAGS_MAXMEMORY   (1<<8)
/* Maxmemory is set and has an eviction policy that may delete keys */
#define REDISMODULE_CTX_FLAGS_EVICT       (1<<9)
/* Redis is out of memory according to the maxmemory flag. */
#define REDISMODULE_CTX_FLAGS_OOM         (1<<10)
/* Less than 25% of memory available according to maxmemory. */
#define REDISMODULE_CTX_FLAGS_OOM_WARNING (1<<11)
/* The command was sent over the replication link. */
#define REDISMODULE_CTX_FLAGS_REPLICATED  (1<<12)
/* Redis is currently loading either from AOF or RDB. */
#define REDISMODULE_CTX_FLAGS_LOADING     (1<<13)
/* The replica has no link with its master, note that
 * there is the inverse flag as well:
 *
 *  REDISMODULE_CTX_FLAGS_REPLICA_IS_ONLINE
 *
 * The two flags are exclusive, one or the other can be set. */
#define REDISMODULE_CTX_FLAGS_REPLICA_IS_STALE        (1<<14)
/* The replica is trying to connect with the master.
 * (REPL_STATE_CONNECT and REPL_STATE_CONNECTING states) */
#define REDISMODULE_CTX_FLAGS_REPLICA_IS_CONNECTING   (1<<15)
/* THe replica is receiving an RDB file from its master. */
#define REDISMODULE_CTX_FLAGS_REPLICA_IS_TRANSFERRING (1<<16)
/* The replica is online, receiving updates from its master. */
#define REDISMODULE_CTX_FLAGS_REPLICA_IS_ONLINE       (1<<17)
/* There is currently some background process active. */
#define REDISMODULE_CTX_FLAGS_ACTIVE_CHILD            (1<<18)

/* Keyspace changes notification classes. Every class is associated with a
 * character for configuration purposes.
 * NOTE: These have to be in sync with NOTIFY_* in server.h */
#define REDISMODULE_NOTIFY_KEYSPACE (1<<0)    /* K */
#define REDISMODULE_NOTIFY_KEYEVENT (1<<1)    /* E */
#define REDISMODULE_NOTIFY_GENERIC  (1<<2)    /* g */
#define REDISMODULE_NOTIFY_STRING   (1<<3)    /* $ */
#define REDISMODULE_NOTIFY_LIST     (1<<4)    /* l */
#define REDISMODULE_NOTIFY_SET      (1<<5)    /* s */
#define REDISMODULE_NOTIFY_HASH     (1<<6)    /* h */
#define REDISMODULE_NOTIFY_ZSET     (1<<7)    /* z */
#define REDISMODULE_NOTIFY_EXPIRED  (1<<8)    /* x */
#define REDISMODULE_NOTIFY_EVICTED  (1<<9)    /* e */
#define REDISMODULE_NOTIFY_STREAM   (1<<10)   /* t */
#define REDISMODULE_NOTIFY_KEY_MISS (1<<11)   /* m */
#define REDISMODULE_NOTIFY_ALL (REDISMODULE_NOTIFY_GENERIC | REDISMODULE_NOTIFY_STRING | REDISMODULE_NOTIFY_LIST | REDISMODULE_NOTIFY_SET | REDISMODULE_NOTIFY_HASH | REDISMODULE_NOTIFY_ZSET | REDISMODULE_NOTIFY_EXPIRED | REDISMODULE_NOTIFY_EVICTED | REDISMODULE_NOTIFY_STREAM | REDISMODULE_NOTIFY_KEY_MISS)      /* A */


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

#define REDISMODULE_CLUSTER_FLAG_NONE           0
#define REDISMODULE_CLUSTER_FLAG_NO_FAILOVER    (1<<1)
#define REDISMODULE_CLUSTER_FLAG_NO_REDIRECTION (1<<2)

#define REDISMODULE_NOT_USED(V) ((void) V)

/* Bit flags for aux_save_triggers and the aux_load and aux_save callbacks */
#define REDISMODULE_AUX_BEFORE_RDB (1<<0)
#define REDISMODULE_AUX_AFTER_RDB  (1<<1)

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

/* Server events definitions. */
#define REDISMODULE_EVENT_REPLICATION_ROLE_CHANGED  0
#define REDISMODULE_EVENT_PERSISTENCE               1
#define REDISMODULE_EVENT_FLUSHDB                   2
#define REDISMODULE_EVENT_LOADING                   3
#define REDISMODULE_EVENT_CLIENT_CHANGE             4
#define REDISMODULE_EVENT_SHUTDOWN                  5
#define REDISMODULE_EVENT_REPLICA_CHANGE            6
#define REDISMODULE_EVENT_MASTER_LINK_CHANGE        7
#define REDISMODULE_EVENT_CRON_LOOP                 8
#define REDISMODULE_EVENT_MODULE_CHANGE             9
#define REDISMODULE_EVENT_LOADING_PROGRESS         10

typedef struct RedisModuleEvent {
    uint64_t id;        /* REDISMODULE_EVENT_... defines. */
    uint64_t dataver;   /* Version of the structure we pass as 'data'. */
} RedisModuleEvent;

struct RedisModuleCtx;
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
    };

/* Those are values that are used for the 'subevent' callback argument. */
#define REDISMODULE_SUBEVENT_PERSISTENCE_RDB_START      0
#define REDISMODULE_SUBEVENT_PERSISTENCE_AOF_START      1
#define REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_RDB_START 2
#define REDISMODULE_SUBEVENT_PERSISTENCE_ENDED          3
#define REDISMODULE_SUBEVENT_PERSISTENCE_FAILED         4

#define REDISMODULE_SUBEVENT_LOADING_RDB_START  0
#define REDISMODULE_SUBEVENT_LOADING_AOF_START  1
#define REDISMODULE_SUBEVENT_LOADING_REPL_START 2
#define REDISMODULE_SUBEVENT_LOADING_ENDED      3
#define REDISMODULE_SUBEVENT_LOADING_FAILED     4

#define REDISMODULE_SUBEVENT_CLIENT_CHANGE_CONNECTED    0
#define REDISMODULE_SUBEVENT_CLIENT_CHANGE_DISCONNECTED 1

#define REDISMODULE_SUBEVENT_MASTER_LINK_UP   0
#define REDISMODULE_SUBEVENT_MASTER_LINK_DOWN 1

#define REDISMODULE_SUBEVENT_REPLICA_CHANGE_ONLINE  0
#define REDISMODULE_SUBEVENT_REPLICA_CHANGE_OFFLINE 1

#define REDISMODULE_EVENT_REPLROLECHANGED_NOW_MASTER  0
#define REDISMODULE_EVENT_REPLROLECHANGED_NOW_REPLICA 1

#define REDISMODULE_SUBEVENT_FLUSHDB_START 0
#define REDISMODULE_SUBEVENT_FLUSHDB_END   1

#define REDISMODULE_SUBEVENT_MODULE_LOADED   0
#define REDISMODULE_SUBEVENT_MODULE_UNLOADED 1

#define REDISMODULE_SUBEVENT_LOADING_PROGRESS_RDB 0
#define REDISMODULE_SUBEVENT_LOADING_PROGRESS_AOF 1

/* RedisModuleClientInfo flags. */
#define REDISMODULE_CLIENTINFO_FLAG_SSL        (1<<0)
#define REDISMODULE_CLIENTINFO_FLAG_PUBSUB     (1<<1)
#define REDISMODULE_CLIENTINFO_FLAG_BLOCKED    (1<<2)
#define REDISMODULE_CLIENTINFO_FLAG_TRACKING   (1<<3)
#define REDISMODULE_CLIENTINFO_FLAG_UNIXSOCKET (1<<4)
#define REDISMODULE_CLIENTINFO_FLAG_MULTI      (1<<5)

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

/* ------------------------- End of common defines ------------------------ */

#ifndef REDISMODULE_CORE

typedef long long mstime_t;

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
typedef struct RedisModuleScanCursor RedisModuleScanCursor;

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
typedef void (*RedisModuleClusterMessageReceiver)(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len);
typedef void (*RedisModuleTimerProc)(RedisModuleCtx *ctx, void *data);
typedef void (*RedisModuleCommandFilterFunc)(RedisModuleCommandFilterCtx *filter);
typedef void (*RedisModuleForkDoneHandler) (int exitcode, int bysignal, void *user_data);
typedef void (*RedisModuleInfoFunc)(RedisModuleInfoCtx *ctx, int for_crash_report);
typedef void (*RedisModuleScanCB)(RedisModuleCtx *ctx, RedisModuleString *keyname, RedisModuleKey *key, void *privdata);
typedef void (*RedisModuleScanKeyCB)(RedisModuleKey *key, RedisModuleString *field, RedisModuleString *value, void *privdata);

#define REDISMODULE_TYPE_METHOD_VERSION 2
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
} RedisModuleTypeMethods;

#define REDISMODULE_XAPI_STABLE(X) \
    X(void *, Alloc, (size_t bytes)) \
    X(void *, Realloc, (void *ptr, size_t bytes)) \
    X(void,Free, (void *ptr)) \
    X(void *, Calloc, (size_t nmemb, size_t size)) \
    X(char *, Strdup, (const char *str)) \
    X(int, GetApi, (const char *, void *)) \
    X(int, CreateCommand, (RedisModuleCtx *ctx, const char *name, RedisModuleCmdFunc cmdfunc, const char *strflags, int firstkey, int lastkey, int keystep)) \
    X(void, SetModuleAttribs, (RedisModuleCtx *ctx, const char *name, int ver, int apiver)) \
    X(int, IsModuleNameBusy, (const char *name)) \
    X(int, WrongArity, (RedisModuleCtx *ctx)) \
    X(int, ReplyWithLongLong, (RedisModuleCtx *ctx, long long ll)) \
    X(int, GetSelectedDb, (RedisModuleCtx *ctx)) \
    X(int, SelectDb, (RedisModuleCtx *ctx, int newid)) \
    X(RedisModuleKey *, OpenKey, (RedisModuleCtx *ctx, RedisModuleString *keyname, int mode)) \
    X(void, CloseKey, (RedisModuleKey *kp)) \
    X(int, KeyType, (RedisModuleKey *kp)) \
    X(size_t, ValueLength, (RedisModuleKey *kp)) \
    X(int, ListPush, (RedisModuleKey *kp, int where, RedisModuleString *ele)) \
    X(RedisModuleString *, ListPop, (RedisModuleKey *key, int where)) \
    X(RedisModuleCallReply *, Call, (RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...)) \
    X(const char *, CallReplyProto, (RedisModuleCallReply *reply, size_t *len)) \
    X(void, FreeCallReply, (RedisModuleCallReply *reply)) \
    X(int, CallReplyType, (RedisModuleCallReply *reply)) \
    X(long long, CallReplyInteger, (RedisModuleCallReply *reply)) \
    X(size_t, CallReplyLength, (RedisModuleCallReply *reply)) \
    X(RedisModuleCallReply *, CallReplyArrayElement, (RedisModuleCallReply *reply, size_t idx)) \
    X(RedisModuleString *, CreateString, (RedisModuleCtx *ctx, const char *ptr, size_t len)) \
    X(RedisModuleString *, CreateStringFromLongLong, (RedisModuleCtx *ctx, long long ll)) \
	X(RedisModuleString *, CreateStringFromLongDouble, (RedisModuleCtx *ctx, long double ld, int humanfriendly)) \
    X(RedisModuleString *, CreateStringFromString, (RedisModuleCtx *ctx, const RedisModuleString *str)) \
    X(RedisModuleString *, CreateStringPrintf, (RedisModuleCtx *ctx, const char *fmt, ...)) \
    X(void, FreeString, (RedisModuleCtx *ctx, RedisModuleString *str)) \
    X(const char *, StringPtrLen, (const RedisModuleString *str, size_t *len)) \
    X(int, ReplyWithError, (RedisModuleCtx *ctx, const char *err)) \
    X(int, ReplyWithSimpleString, (RedisModuleCtx *ctx, const char *msg)) \
    X(int, ReplyWithArray, (RedisModuleCtx *ctx, long len)) \
	X(int, ReplyWithNullArray, (RedisModuleCtx *ctx)) \
	X(int, ReplyWithEmptyArray, (RedisModuleCtx *ctx)) \
    X(void, ReplySetArrayLength, (RedisModuleCtx *ctx, long len)) \
    X(int, ReplyWithStringBuffer, (RedisModuleCtx *ctx, const char *buf, size_t len)) \
    X(int, ReplyWithCString, (RedisModuleCtx *ctx, const char *buf)) \
    X(int, ReplyWithString, (RedisModuleCtx *ctx, RedisModuleString *str)) \
	X(int, ReplyWithEmptyString, (RedisModuleCtx *ctx)) \
	X(int, ReplyWithVerbatimString)(RedisModuleCtx *ctx, const char *buf, size_t len)) \
    X(int, ReplyWithNull, (RedisModuleCtx *ctx)) \
    X(int, ReplyWithDouble, (RedisModuleCtx *ctx, double d)) \
	X(int, ReplyWithLongDouble, (RedisModuleCtx *ctx, long double d)) \
    X(int, ReplyWithCallReply, (RedisModuleCtx *ctx, RedisModuleCallReply *reply)) \
    X(int, StringToLongLong, (const RedisModuleString *str, long long *ll)) \
    X(int, StringToDouble, (const RedisModuleString *str, double *d)) \
	X(int, StringToLongDouble, (const RedisModuleString *str, long double *d)) \
    X(void, AutoMemory, (RedisModuleCtx *ctx)) \
    X(int, Replicate, (RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...)) \
    X(int, ReplicateVerbatim, (RedisModuleCtx *ctx)) \
    X(const char *, CallReplyStringPtr, (RedisModuleCallReply *reply, size_t *len)) \
    X(RedisModuleString *, CreateStringFromCallReply, (RedisModuleCallReply *reply)) \
    X(int, DeleteKey, (RedisModuleKey *key)) \
    X(int, UnlinkKey, (RedisModuleKey *key)) \
    X(int, StringSet, (RedisModuleKey *key, RedisModuleString *str)) \
    X(char *, StringDMA, (RedisModuleKey *key, size_t *len, int mode)) \
    X(int, StringTruncate, (RedisModuleKey *key, size_t newlen)) \
    X(mstime_t, GetExpire, (RedisModuleKey *key)) \
    X(int, SetExpire, (RedisModuleKey *key, mstime_t expire)) \
	X(void, ResetDataset, (int restart_aof, int async)) \
	X(unsigned long long, DbSize, (RedisModuleCtx *ctx)) \
	X(RedisModuleString *, RandomKey, (RedisModuleCtx *ctx)) \
    X(int, ZsetAdd, (RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr)) \
    X(int, ZsetIncrby, (RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr, double *newscore)) \
    X(int, ZsetScore, (RedisModuleKey *key, RedisModuleString *ele, double *score)) \
    X(int, ZsetRem, (RedisModuleKey *key, RedisModuleString *ele, int *deleted)) \
    X(void ,ZsetRangeStop, (RedisModuleKey *key)) \
    X(int, ZsetFirstInScoreRange, (RedisModuleKey *key, double min, double max, int minex, int maxex)) \
    X(int, ZsetLastInScoreRange, (RedisModuleKey *key, double min, double max, int minex, int maxex)) \
    X(int, ZsetFirstInLexRange, (RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max)) \
    X(int, ZsetLastInLexRange, (RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max)) \
    X(RedisModuleString *, ZsetRangeCurrentElement, (RedisModuleKey *key, double *score)) \
    X(int, ZsetRangeNext, (RedisModuleKey *key)) \
    X(int, ZsetRangePrev, (RedisModuleKey *key)) \
    X(int, ZsetRangeEndReached, (RedisModuleKey *key)) \
    X(int, HashSet, (RedisModuleKey *key, int flags, ...)) \
    X(int, HashGet, (RedisModuleKey *key, int flags, ...)) \
    X(int, IsKeysPositionRequest, (RedisModuleCtx *ctx)) \
    X(void, KeyAtPos, (RedisModuleCtx *ctx, int pos)) \
    X(unsigned long long, GetClientId, (RedisModuleCtx *ctx)) \
	X(int, GetClientInfoById, (void *ci, uint64_t id)) \
	X(int, PublishMessage, (RedisModuleCtx *ctx, RedisModuleString *channel, RedisModuleString *message)) \
    X(int, GetContextFlags, (RedisModuleCtx *ctx)) \
    X(void *, PoolAlloc, (RedisModuleCtx *ctx, size_t bytes)) \
    X(RedisModuleType *, CreateDataType, (RedisModuleCtx *ctx, const char *name, int encver, RedisModuleTypeMethods *typemethods)) \
    X(int,ModuleTypeSetValue, (RedisModuleKey *key, RedisModuleType *mt, void *value)) \
	X(void *, ModuleTypeReplaceValue, (RedisModuleKey *key, RedisModuleType *mt, void *new_value)) \
    X(RedisModuleType *, ModuleTypeGetType, (RedisModuleKey *key)) \
    X(void *, ModuleTypeGetValue, (RedisModuleKey *key)) \
	X(int, IsIOError, (RedisModuleIO *io)) \
	X(void, SetModuleOptions, (RedisModuleCtx *ctx, int options)) \
	X(int, SignalModifiedKey, (RedisModuleCtx *ctx, RedisModuleString *keyname)) \
    X(void, SaveUnsigned, (RedisModuleIO *io, uint64_t value)) \
    X(uint64_t, LoadUnsigned, (RedisModuleIO *io)) \
    X(void, SaveSigned, (RedisModuleIO *io, int64_t value)) \
    X(int64_t, LoadSigned, (RedisModuleIO *io)) \
    X(void, EmitAOF, (RedisModuleIO *io, const char *cmdname, const char *fmt, ...)) \
    X(void, SaveString, (RedisModuleIO *io, RedisModuleString *s)) \
    X(void, SaveStringBuffer, (RedisModuleIO *io, const char *str, size_t len)) \
    X(RedisModuleString *, LoadString, (RedisModuleIO *io)) \
    X(char *, LoadStringBuffer, (RedisModuleIO *io, size_t *lenptr)) \
    X(void, SaveDouble, (RedisModuleIO *io, double value)) \
    X(double, LoadDouble, (RedisModuleIO *io)) \
    X(void, SaveFloat, (RedisModuleIO *io, float value)) \
    X(float, LoadFloat, (RedisModuleIO *io)) \
	X(void, SaveLongDouble, (RedisModuleIO *io, long double value)) \
	X(long double, LoadLongDouble, (RedisModuleIO *io)) \
	X(void *, LoadDataTypeFromString, (const RedisModuleString *str, const RedisModuleType *mt)) \
	X(RedisModuleString *, SaveDataTypeToString, (RedisModuleCtx *ctx, void *data, const RedisModuleType *mt)) \
    X(void, Log, (RedisModuleCtx *ctx, const char *level, const char *fmt, ...)) \
    X(void, LogIOError, (RedisModuleIO *io, const char *levelstr, const char *fmt, ...)) \
	X(void, _Assert, (const char *estr, const char *file, int line)) \
	X(void, LatencyAddSample, (const char *event, mstime_t latency)) \
    X(int, StringAppendBuffer, (RedisModuleCtx *ctx, RedisModuleString *str, const char *buf, size_t len)) \
    X(void, RetainString, (RedisModuleCtx *ctx, RedisModuleString *str)) \
    X(int, StringCompare, (RedisModuleString *a, RedisModuleString *b)) \
    X(RedisModuleCtx *, GetContextFromIO, (RedisModuleIO *io)) \
    X(const RedisModuleString *, GetKeyNameFromIO, (RedisModuleIO *io)) \
	X(const RedisModuleString *, GetKeyNameFromModuleKey, (RedisModuleKey *key)) \
    X(long long, Milliseconds, (void)) \
    X(void, DigestAddStringBuffer, (RedisModuleDigest *md, unsigned char *ele, size_t len)) \
    X(void, DigestAddLongLong, (RedisModuleDigest *md, long long ele)) \
    X(void, DigestEndSequence, (RedisModuleDigest *md)) \
    X(RedisModuleDict *, CreateDict, (RedisModuleCtx *ctx)) \
    X(void, FreeDict, (RedisModuleCtx *ctx, RedisModuleDict *d)) \
    X(uint64_t, DictSize, (RedisModuleDict *d)) \
    X(int, DictSetC, (RedisModuleDict *d, void *key, size_t keylen, void *ptr)) \
    X(int, DictReplaceC, (RedisModuleDict *d, void *key, size_t keylen, void *ptr)) \
    X(int, DictSet, (RedisModuleDict *d, RedisModuleString *key, void *ptr)) \
    X(int, DictReplace, (RedisModuleDict *d, RedisModuleString *key, void *ptr)) \
    X(void *, DictGetC, (RedisModuleDict *d, void *key, size_t keylen, int *nokey)) \
    X(void *, DictGet, (RedisModuleDict *d, RedisModuleString *key, int *nokey)) \
    X(int, DictDelC, (RedisModuleDict *d, void *key, size_t keylen, void *oldval)) \
    X(int, DictDel, (RedisModuleDict *d, RedisModuleString *key, void *oldval)) \
    X(RedisModuleDictIter *, DictIteratorStartC, (RedisModuleDict *d, const char *op, void *key, size_t keylen)) \
    X(RedisModuleDictIter *, DictIteratorStart, (RedisModuleDict *d, const char *op, RedisModuleString *key)) \
    X(void, DictIteratorStop, (RedisModuleDictIter *di)) \
    X(int, DictIteratorReseekC, (RedisModuleDictIter *di, const char *op, void *key, size_t keylen)) \
    X(int, DictIteratorReseek, (RedisModuleDictIter *di, const char *op, RedisModuleString *key)) \
    X(void *, DictNextC, (RedisModuleDictIter *di, size_t *keylen, void **dataptr)) \
    X(void *, DictPrevC, (RedisModuleDictIter *di, size_t *keylen, void **dataptr)) \
    X(RedisModuleString *, DictNext, (RedisModuleCtx *ctx, RedisModuleDictIter *di, void **dataptr)) \
    X(RedisModuleString *, DictPrev, (RedisModuleCtx *ctx, RedisModuleDictIter *di, void **dataptr)) \
    X(int, DictCompareC, (RedisModuleDictIter *di, const char *op, void *key, size_t keylen)) \
    X(int, DictCompare, (RedisModuleDictIter *di, const char *op, RedisModuleString *key)) \
    X(int, RegisterInfoFunc, (RedisModuleCtx *ctx, RedisModuleInfoFunc cb)) \
    X(int, InfoAddSection, (RedisModuleInfoCtx *ctx, char *name)) \
    X(int, InfoBeginDictField, (RedisModuleInfoCtx *ctx, char *name)) \
    X(int, InfoEndDictField, (RedisModuleInfoCtx *ctx)) \
    X(int, InfoAddFieldString, (RedisModuleInfoCtx *ctx, char *field, RedisModuleString *value)) \
    X(int, InfoAddFieldCString, (RedisModuleInfoCtx *ctx, char *field, char *value)) \
    X(int, InfoAddFieldDouble, (RedisModuleInfoCtx *ctx, char *field, double value)) \
    X(int, InfoAddFieldLongLong, (RedisModuleInfoCtx *ctx, char *field, long long value)) \
    X(int, InfoAddFieldULongLong, (RedisModuleInfoCtx *ctx, char *field, unsigned long long value)) \
    X(int, SubscribeToServerEvent, (RedisModuleCtx *ctx, RedisModuleEvent event, RedisModuleEventCallback callback)) \
    X(int, SetLRU, (RedisModuleKey *key, mstime_t lru_idle)) \
    X(int, GetLRU, (RedisModuleKey *key, mstime_t *lru_idle)) \
    X(int, SetLFU, (RedisModuleKey *key, long long lfu_freq)) \
    X(int, GetLFU, (RedisModuleKey *key, long long *lfu_freq)) \
    X(RedisModuleBlockedClient *, BlockClientOnKeys, (RedisModuleCtx *ctx, RedisModuleCmdFunc reply_callback, RedisModuleCmdFunc timeout_callback, void (*free_privdata)(RedisModuleCtx*,void*), long long timeout_ms, RedisModuleString **keys, int numkeys, void *privdata)) \
    X(void, SignalKeyAsReady, (RedisModuleCtx *ctx, RedisModuleString *key)) \
    X(RedisModuleString *, GetBlockedClientReadyKey, (RedisModuleCtx *ctx)) \
    X(RedisModuleScanCursor *, ScanCursorCreate, ()) \
    X(void, ScanCursorRestart, (RedisModuleScanCursor *cursor)) \
    X(void, ScanCursorDestroy, (RedisModuleScanCursor *cursor)) \
    X(int, Scan, (RedisModuleCtx *ctx, RedisModuleScanCursor *cursor, RedisModuleScanCB fn, void *privdata)) \
    X(int, ScanKey, (RedisModuleKey *key, RedisModuleScanCursor *cursor, RedisModuleScanKeyCB fn, void *privdata))

/* Experimental APIs */

#define REDISMODULE_EXPERIMENTAL_API_VERSION 3

#define REDISMODULE_XAPI_EXPERIMENTAL(X) \
    X(RedisModuleBlockedClient *, BlockClient, (RedisModuleCtx *ctx, RedisModuleCmdFunc reply_callback, RedisModuleCmdFunc timeout_callback, void (*free_privdata)(RedisModuleCtx*,void*), long long timeout_ms)) \
    X(int, UnblockClient, (RedisModuleBlockedClient *bc, void *privdata)) \
    X(int, IsBlockedReplyRequest, (RedisModuleCtx *ctx)) \
    X(int, IsBlockedTimeoutRequest, (RedisModuleCtx *ctx)) \
    X(void *, GetBlockedClientPrivateData, (RedisModuleCtx *ctx)) \
    X(RedisModuleBlockedClient *, GetBlockedClientHandle, (RedisModuleCtx *ctx)) \
    X(int, AbortBlock, (RedisModuleBlockedClient *bc)) \
    X(RedisModuleCtx *, GetThreadSafeContext, (RedisModuleBlockedClient *bc)) \
    X(void, FreeThreadSafeContext, (RedisModuleCtx *ctx)) \
    X(void, ThreadSafeContextLock, (RedisModuleCtx *ctx)) \
    X(void, ThreadSafeContextUnlock, (RedisModuleCtx *ctx)) \
    X(int, SubscribeToKeyspaceEvents, (RedisModuleCtx *ctx, int types, RedisModuleNotificationFunc cb)) \
	X(int, NotifyKeyspaceEvent, (RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key)) \
	X(int, GetNotifyKeyspaceEvents)()) \
    X(int, BlockedClientDisconnected, (RedisModuleCtx *ctx)) \
    X(void, RegisterClusterMessageReceiver, (RedisModuleCtx *ctx, uint8_t type, RedisModuleClusterMessageReceiver callback)) \
    X(int, SendClusterMessage, (RedisModuleCtx *ctx, char *target_id, uint8_t type, unsigned char *msg, uint32_t len)) \
    X(int, GetClusterNodeInfo, (RedisModuleCtx *ctx, const char *id, char *ip, char *master_id, int *port, int *flags)) \
    X(char **, GetClusterNodesList, (RedisModuleCtx *ctx, size_t *numnodes)) \
    X(void, FreeClusterNodesList, (char **ids)) \
    X(RedisModuleTimerID, CreateTimer, (RedisModuleCtx *ctx, mstime_t period, RedisModuleTimerProc callback, void *data)) \
    X(int, StopTimer, (RedisModuleCtx *ctx, RedisModuleTimerID id, void **data)) \
    X(int, GetTimerInfo, (RedisModuleCtx *ctx, RedisModuleTimerID id, uint64_t *remaining, void **data)) \
    X(const char *, GetMyClusterID, (void)) \
    X(size_t, GetClusterSize, (void)) \
    X(void, GetRandomBytes, (unsigned char *dst, size_t len)) \
    X(void, GetRandomHexChars, (char *dst, size_t len)) \
    X(void, SetDisconnectCallback, (RedisModuleBlockedClient *bc, RedisModuleDisconnectFunc callback)) \
    X(void, SetClusterFlags, (RedisModuleCtx *ctx, uint64_t flags)) \
    X(int, ExportSharedAPI, (RedisModuleCtx *ctx, const char *apiname, void *func)) \
    X(void *, GetSharedAPI, (RedisModuleCtx *ctx, const char *apiname)) \
    X(RedisModuleCommandFilter *, RegisterCommandFilter, (RedisModuleCtx *ctx, RedisModuleCommandFilterFunc cb, int flags)) \
    X(int, UnregisterCommandFilter, (RedisModuleCtx *ctx, RedisModuleCommandFilter *filter)) \
    X(int, CommandFilterArgsCount, (RedisModuleCommandFilterCtx *fctx)) \
    X(const RedisModuleString *, CommandFilterArgGet, (RedisModuleCommandFilterCtx *fctx, int pos)) \
    X(int, CommandFilterArgInsert, (RedisModuleCommandFilterCtx *fctx, int pos, RedisModuleString *arg)) \
    X(int, CommandFilterArgReplace, (RedisModuleCommandFilterCtx *fctx, int pos, RedisModuleString *arg)) \
    X(int, CommandFilterArgDelete, (RedisModuleCommandFilterCtx *fctx, int pos))
	X(int, Fork, (RedisModuleForkDoneHandler cb, void *user_data)) \
	X(int, ExitFromChild, (int retcode)) \
	X(int, KillForkChild, (int child_pid))

#ifndef REDISMODULE_XAPI_EXTENSIONS
#define REDISMODULE_XAPI_EXTENSIONS(X)
#endif

#ifdef REDISMODULE_EXPERIMENTAL_API
#define REDISMODULE_XAPI(X) REDISMODULE_XAPI_STABLE(X) REDISMODULE_XAPI_EXPERIMENTAL(X) REDISMODULE_XAPI_EXTENSIONS(X)
#else
#define REDISMODULE_XAPI(X) REDISMODULE_XAPI_STABLE(X) REDISMODULE_XAPI_EXTENSIONS(X)
#endif

#define RedisModule_IsAOFClient(id) ((id) == UINT64_MAX)

typedef int (*RedisModule_GetApiFunctionType)(const char *name, void *pp);

#pragma push_macro("X")
#define X(TYPE, NAME, ARGS) \
    extern TYPE (*RedisModule_##NAME) ARGS;
REDISMODULE_XAPI(X)
#undef X
#pragma pop_macro("X")

/* This is included inline inside each Redis module. */

static int RedisModule_Init(RedisModuleCtx *ctx, const char *name, int ver, int apiver) __attribute__((unused));

static int RedisModule_Init(RedisModuleCtx *ctx, const char *name, int ver, int apiver) {
    RedisModule_GetApiFunctionType getapifuncptr = (RedisModule_GetApiFunctionType)((void **)ctx)[0];

#pragma push_macro("X")
#define X(TYPE, NAME, ARGS) getapifuncptr("RedisModule_" #NAME, (void *)&RedisModule_##NAME);
    REDISMODULE_XAPI(X)
#undef X
#pragma pop_macro("X")

    if (RedisModule_IsModuleNameBusy && RedisModule_IsModuleNameBusy(name)) {
        return REDISMODULE_ERR;
    }
    RedisModule_SetModuleAttribs(ctx,name,ver,apiver);
    return REDISMODULE_OK;
}

#define REDISMODULE__INIT_WITH_NULL(TYPE, NAME, ARGS) \
    TYPE (*RedisModule_##NAME)ARGS = NULL;
#define REDISMODULE_INIT_SYMBOLS() REDISMODULE_XAPI(REDISMODULE__INIT_WITH_NULL)

#define RedisModule_Assert(_e) ((_e)?(void)0 : (RedisModule__Assert(#_e,__FILE__,__LINE__),exit(1)))

#else

/* Things only defined for the modules core, not exported to modules
 * including this file. */
#define RedisModuleString robj

#endif /* REDISMODULE_CORE */

#ifdef __cplusplus
}
#endif

#endif /* REDISMOUDLE_H */

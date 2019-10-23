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

/* Hash API flags. */
#define REDISMODULE_HASH_NONE       0
#define REDISMODULE_HASH_NX         (1<<0)
#define REDISMODULE_HASH_XX         (1<<1)
#define REDISMODULE_HASH_CFIELDS    (1<<2)
#define REDISMODULE_HASH_EXISTS     (1<<3)

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

#define REDISMODULE_CLUSTER_FLAG_NONE 0
#define REDISMODULE_CLUSTER_FLAG_NO_FAILOVER (1<<1)
#define REDISMODULE_CLUSTER_FLAG_NO_REDIRECTION (1<<2)

#define REDISMODULE_NOT_USED(V) ((void) V)

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

/* Server events definitions. */
#define REDISMODULE_EVENT_REPLICATION_ROLE_CHANGED 0
#define REDISMODULE_EVENT_PERSISTENCE 1
#define REDISMODULE_EVENT_FLUSHDB 2
#define REDISMODULE_EVENT_LOADING 3
#define REDISMODULE_EVENT_CLIENT_CHANGE 4
#define REDISMODULE_EVENT_SHUTDOWN 5
#define REDISMODULE_EVENT_REPLICA_CHANGE 6
#define REDISMODULE_EVENT_MASTER_LINK_CHANGE 7
#define REDISMODULE_EVENT_CRON_LOOP 8

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
    };

/* Those are values that are used for the 'subevent' callback argument. */
#define REDISMODULE_SUBEVENT_PERSISTENCE_RDB_START 0
#define REDISMODULE_SUBEVENT_PERSISTENCE_RDB_END 1
#define REDISMODULE_SUBEVENT_PERSISTENCE_AOF_START 2
#define REDISMODULE_SUBEVENT_PERSISTENCE_AOF_END 3

#define REDISMODULE_SUBEVENT_LOADING_RDB_START 0
#define REDISMODULE_SUBEVENT_LOADING_RDB_END 1
#define REDISMODULE_SUBEVENT_LOADING_AOF_START 2
#define REDISMODULE_SUBEVENT_LOADING_AOF_END 3

#define REDISMODULE_SUBEVENT_CLIENT_CHANGE_CONNECTED 0
#define REDISMODULE_SUBEVENT_CLIENT_CHANGE_DISCONNECTED 1

#define REDISMODULE_SUBEVENT_MASTER_LINK_UP 0
#define REDISMODULE_SUBEVENT_MASTER_LINK_DOWN 1

#define REDISMODULE_SUBEVENT_REPLICA_CHANGE_CONNECTED 0
#define REDISMODULE_SUBEVENT_REPLICA_CHANGE_DISCONNECTED 1

#define REDISMODULE_SUBEVENT_FLUSHDB_START 0
#define REDISMODULE_SUBEVENT_FLUSHDB_END 1

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

#define REDISMODULE_FLUSHINFO_VERSION 1
typedef struct RedisModuleFlushInfo {
    uint64_t version;       /* Not used since this structure is never passed
                               from the module to the core right now. Here
                               for future compatibility. */
    int32_t sync;           /* Synchronous or threaded flush?. */
    int32_t dbnum;          /* Flushed database number, -1 for ALL. */
} RedisModuleFlushInfoV1;

#define RedisModuleFlushInfo RedisModuleFlushInfoV1

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
typedef void (*RedisModuleCommandFilterFunc) (RedisModuleCommandFilterCtx *filter);
typedef void (*RedisModuleForkDoneHandler) (int exitcode, int bysignal, void *user_data);
typedef void (*RedisModuleInfoFunc)(RedisModuleInfoCtx *ctx, int for_crash_report);

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

#define REDISMODULE_GET_API(name) \
    RedisModule_GetApi("RedisModule_" #name, ((void **)&RedisModule_ ## name))

#define REDISMODULE_API_FUNC(x) (*x)


void *REDISMODULE_API_FUNC(RedisModule_Alloc)(size_t bytes);
void *REDISMODULE_API_FUNC(RedisModule_Realloc)(void *ptr, size_t bytes);
void REDISMODULE_API_FUNC(RedisModule_Free)(void *ptr);
void *REDISMODULE_API_FUNC(RedisModule_Calloc)(size_t nmemb, size_t size);
char *REDISMODULE_API_FUNC(RedisModule_Strdup)(const char *str);
int REDISMODULE_API_FUNC(RedisModule_GetApi)(const char *, void *);
int REDISMODULE_API_FUNC(RedisModule_CreateCommand)(RedisModuleCtx *ctx, const char *name, RedisModuleCmdFunc cmdfunc, const char *strflags, int firstkey, int lastkey, int keystep);
void REDISMODULE_API_FUNC(RedisModule_SetModuleAttribs)(RedisModuleCtx *ctx, const char *name, int ver, int apiver);
int REDISMODULE_API_FUNC(RedisModule_IsModuleNameBusy)(const char *name);
int REDISMODULE_API_FUNC(RedisModule_WrongArity)(RedisModuleCtx *ctx);
int REDISMODULE_API_FUNC(RedisModule_ReplyWithLongLong)(RedisModuleCtx *ctx, long long ll);
int REDISMODULE_API_FUNC(RedisModule_GetSelectedDb)(RedisModuleCtx *ctx);
int REDISMODULE_API_FUNC(RedisModule_SelectDb)(RedisModuleCtx *ctx, int newid);
void *REDISMODULE_API_FUNC(RedisModule_OpenKey)(RedisModuleCtx *ctx, RedisModuleString *keyname, int mode);
void REDISMODULE_API_FUNC(RedisModule_CloseKey)(RedisModuleKey *kp);
int REDISMODULE_API_FUNC(RedisModule_KeyType)(RedisModuleKey *kp);
size_t REDISMODULE_API_FUNC(RedisModule_ValueLength)(RedisModuleKey *kp);
int REDISMODULE_API_FUNC(RedisModule_ListPush)(RedisModuleKey *kp, int where, RedisModuleString *ele);
RedisModuleString *REDISMODULE_API_FUNC(RedisModule_ListPop)(RedisModuleKey *key, int where);
RedisModuleCallReply *REDISMODULE_API_FUNC(RedisModule_Call)(RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...);
const char *REDISMODULE_API_FUNC(RedisModule_CallReplyProto)(RedisModuleCallReply *reply, size_t *len);
void REDISMODULE_API_FUNC(RedisModule_FreeCallReply)(RedisModuleCallReply *reply);
int REDISMODULE_API_FUNC(RedisModule_CallReplyType)(RedisModuleCallReply *reply);
long long REDISMODULE_API_FUNC(RedisModule_CallReplyInteger)(RedisModuleCallReply *reply);
size_t REDISMODULE_API_FUNC(RedisModule_CallReplyLength)(RedisModuleCallReply *reply);
RedisModuleCallReply *REDISMODULE_API_FUNC(RedisModule_CallReplyArrayElement)(RedisModuleCallReply *reply, size_t idx);
RedisModuleString *REDISMODULE_API_FUNC(RedisModule_CreateString)(RedisModuleCtx *ctx, const char *ptr, size_t len);
RedisModuleString *REDISMODULE_API_FUNC(RedisModule_CreateStringFromLongLong)(RedisModuleCtx *ctx, long long ll);
RedisModuleString *REDISMODULE_API_FUNC(RedisModule_CreateStringFromString)(RedisModuleCtx *ctx, const RedisModuleString *str);
RedisModuleString *REDISMODULE_API_FUNC(RedisModule_CreateStringPrintf)(RedisModuleCtx *ctx, const char *fmt, ...);
void REDISMODULE_API_FUNC(RedisModule_FreeString)(RedisModuleCtx *ctx, RedisModuleString *str);
const char *REDISMODULE_API_FUNC(RedisModule_StringPtrLen)(const RedisModuleString *str, size_t *len);
int REDISMODULE_API_FUNC(RedisModule_ReplyWithError)(RedisModuleCtx *ctx, const char *err);
int REDISMODULE_API_FUNC(RedisModule_ReplyWithSimpleString)(RedisModuleCtx *ctx, const char *msg);
int REDISMODULE_API_FUNC(RedisModule_ReplyWithArray)(RedisModuleCtx *ctx, long len);
int REDISMODULE_API_FUNC(RedisModule_ReplyWithNullArray)(RedisModuleCtx *ctx);
int REDISMODULE_API_FUNC(RedisModule_ReplyWithEmptyArray)(RedisModuleCtx *ctx);
void REDISMODULE_API_FUNC(RedisModule_ReplySetArrayLength)(RedisModuleCtx *ctx, long len);
int REDISMODULE_API_FUNC(RedisModule_ReplyWithStringBuffer)(RedisModuleCtx *ctx, const char *buf, size_t len);
int REDISMODULE_API_FUNC(RedisModule_ReplyWithCString)(RedisModuleCtx *ctx, const char *buf);
int REDISMODULE_API_FUNC(RedisModule_ReplyWithString)(RedisModuleCtx *ctx, RedisModuleString *str);
int REDISMODULE_API_FUNC(RedisModule_ReplyWithEmptyString)(RedisModuleCtx *ctx);
int REDISMODULE_API_FUNC(RedisModule_ReplyWithVerbatimString)(RedisModuleCtx *ctx, const char *buf, size_t len);
int REDISMODULE_API_FUNC(RedisModule_ReplyWithNull)(RedisModuleCtx *ctx);
int REDISMODULE_API_FUNC(RedisModule_ReplyWithDouble)(RedisModuleCtx *ctx, double d);
int REDISMODULE_API_FUNC(RedisModule_ReplyWithCallReply)(RedisModuleCtx *ctx, RedisModuleCallReply *reply);
int REDISMODULE_API_FUNC(RedisModule_StringToLongLong)(const RedisModuleString *str, long long *ll);
int REDISMODULE_API_FUNC(RedisModule_StringToDouble)(const RedisModuleString *str, double *d);
void REDISMODULE_API_FUNC(RedisModule_AutoMemory)(RedisModuleCtx *ctx);
int REDISMODULE_API_FUNC(RedisModule_Replicate)(RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...);
int REDISMODULE_API_FUNC(RedisModule_ReplicateVerbatim)(RedisModuleCtx *ctx);
const char *REDISMODULE_API_FUNC(RedisModule_CallReplyStringPtr)(RedisModuleCallReply *reply, size_t *len);
RedisModuleString *REDISMODULE_API_FUNC(RedisModule_CreateStringFromCallReply)(RedisModuleCallReply *reply);
int REDISMODULE_API_FUNC(RedisModule_DeleteKey)(RedisModuleKey *key);
int REDISMODULE_API_FUNC(RedisModule_UnlinkKey)(RedisModuleKey *key);
int REDISMODULE_API_FUNC(RedisModule_StringSet)(RedisModuleKey *key, RedisModuleString *str);
char *REDISMODULE_API_FUNC(RedisModule_StringDMA)(RedisModuleKey *key, size_t *len, int mode);
int REDISMODULE_API_FUNC(RedisModule_StringTruncate)(RedisModuleKey *key, size_t newlen);
mstime_t REDISMODULE_API_FUNC(RedisModule_GetExpire)(RedisModuleKey *key);
int REDISMODULE_API_FUNC(RedisModule_SetExpire)(RedisModuleKey *key, mstime_t expire);
int REDISMODULE_API_FUNC(RedisModule_ZsetAdd)(RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr);
int REDISMODULE_API_FUNC(RedisModule_ZsetIncrby)(RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr, double *newscore);
int REDISMODULE_API_FUNC(RedisModule_ZsetScore)(RedisModuleKey *key, RedisModuleString *ele, double *score);
int REDISMODULE_API_FUNC(RedisModule_ZsetRem)(RedisModuleKey *key, RedisModuleString *ele, int *deleted);
void REDISMODULE_API_FUNC(RedisModule_ZsetRangeStop)(RedisModuleKey *key);
int REDISMODULE_API_FUNC(RedisModule_ZsetFirstInScoreRange)(RedisModuleKey *key, double min, double max, int minex, int maxex);
int REDISMODULE_API_FUNC(RedisModule_ZsetLastInScoreRange)(RedisModuleKey *key, double min, double max, int minex, int maxex);
int REDISMODULE_API_FUNC(RedisModule_ZsetFirstInLexRange)(RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max);
int REDISMODULE_API_FUNC(RedisModule_ZsetLastInLexRange)(RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max);
RedisModuleString *REDISMODULE_API_FUNC(RedisModule_ZsetRangeCurrentElement)(RedisModuleKey *key, double *score);
int REDISMODULE_API_FUNC(RedisModule_ZsetRangeNext)(RedisModuleKey *key);
int REDISMODULE_API_FUNC(RedisModule_ZsetRangePrev)(RedisModuleKey *key);
int REDISMODULE_API_FUNC(RedisModule_ZsetRangeEndReached)(RedisModuleKey *key);
int REDISMODULE_API_FUNC(RedisModule_HashSet)(RedisModuleKey *key, int flags, ...);
int REDISMODULE_API_FUNC(RedisModule_HashGet)(RedisModuleKey *key, int flags, ...);
int REDISMODULE_API_FUNC(RedisModule_IsKeysPositionRequest)(RedisModuleCtx *ctx);
void REDISMODULE_API_FUNC(RedisModule_KeyAtPos)(RedisModuleCtx *ctx, int pos);
unsigned long long REDISMODULE_API_FUNC(RedisModule_GetClientId)(RedisModuleCtx *ctx);
int REDISMODULE_API_FUNC(RedisModule_GetClientInfoById)(void *ci, uint64_t id);
int REDISMODULE_API_FUNC(RedisModule_GetContextFlags)(RedisModuleCtx *ctx);
void *REDISMODULE_API_FUNC(RedisModule_PoolAlloc)(RedisModuleCtx *ctx, size_t bytes);
RedisModuleType *REDISMODULE_API_FUNC(RedisModule_CreateDataType)(RedisModuleCtx *ctx, const char *name, int encver, RedisModuleTypeMethods *typemethods);
int REDISMODULE_API_FUNC(RedisModule_ModuleTypeSetValue)(RedisModuleKey *key, RedisModuleType *mt, void *value);
RedisModuleType *REDISMODULE_API_FUNC(RedisModule_ModuleTypeGetType)(RedisModuleKey *key);
void *REDISMODULE_API_FUNC(RedisModule_ModuleTypeGetValue)(RedisModuleKey *key);
int REDISMODULE_API_FUNC(RedisModule_IsIOError)(RedisModuleIO *io);
void REDISMODULE_API_FUNC(RedisModule_SetModuleOptions)(RedisModuleCtx *ctx, int options);
int REDISMODULE_API_FUNC(RedisModule_SignalModifiedKey)(RedisModuleCtx *ctx, RedisModuleString *keyname);
void REDISMODULE_API_FUNC(RedisModule_SaveUnsigned)(RedisModuleIO *io, uint64_t value);
uint64_t REDISMODULE_API_FUNC(RedisModule_LoadUnsigned)(RedisModuleIO *io);
void REDISMODULE_API_FUNC(RedisModule_SaveSigned)(RedisModuleIO *io, int64_t value);
int64_t REDISMODULE_API_FUNC(RedisModule_LoadSigned)(RedisModuleIO *io);
void REDISMODULE_API_FUNC(RedisModule_EmitAOF)(RedisModuleIO *io, const char *cmdname, const char *fmt, ...);
void REDISMODULE_API_FUNC(RedisModule_SaveString)(RedisModuleIO *io, RedisModuleString *s);
void REDISMODULE_API_FUNC(RedisModule_SaveStringBuffer)(RedisModuleIO *io, const char *str, size_t len);
RedisModuleString *REDISMODULE_API_FUNC(RedisModule_LoadString)(RedisModuleIO *io);
char *REDISMODULE_API_FUNC(RedisModule_LoadStringBuffer)(RedisModuleIO *io, size_t *lenptr);
void REDISMODULE_API_FUNC(RedisModule_SaveDouble)(RedisModuleIO *io, double value);
double REDISMODULE_API_FUNC(RedisModule_LoadDouble)(RedisModuleIO *io);
void REDISMODULE_API_FUNC(RedisModule_SaveFloat)(RedisModuleIO *io, float value);
float REDISMODULE_API_FUNC(RedisModule_LoadFloat)(RedisModuleIO *io);
void REDISMODULE_API_FUNC(RedisModule_Log)(RedisModuleCtx *ctx, const char *level, const char *fmt, ...);
void REDISMODULE_API_FUNC(RedisModule_LogIOError)(RedisModuleIO *io, const char *levelstr, const char *fmt, ...);
void REDISMODULE_API_FUNC(RedisModule__Assert)(const char *estr, const char *file, int line);
void REDISMODULE_API_FUNC(RedisModule_LatencyAddSample)(const char *event, mstime_t latency);
int REDISMODULE_API_FUNC(RedisModule_StringAppendBuffer)(RedisModuleCtx *ctx, RedisModuleString *str, const char *buf, size_t len);
void REDISMODULE_API_FUNC(RedisModule_RetainString)(RedisModuleCtx *ctx, RedisModuleString *str);
int REDISMODULE_API_FUNC(RedisModule_StringCompare)(RedisModuleString *a, RedisModuleString *b);
RedisModuleCtx *REDISMODULE_API_FUNC(RedisModule_GetContextFromIO)(RedisModuleIO *io);
const RedisModuleString *REDISMODULE_API_FUNC(RedisModule_GetKeyNameFromIO)(RedisModuleIO *io);
const RedisModuleString *REDISMODULE_API_FUNC(RedisModule_GetKeyNameFromModuleKey)(RedisModuleKey *key);
long long REDISMODULE_API_FUNC(RedisModule_Milliseconds)(void);
void REDISMODULE_API_FUNC(RedisModule_DigestAddStringBuffer)(RedisModuleDigest *md, unsigned char *ele, size_t len);
void REDISMODULE_API_FUNC(RedisModule_DigestAddLongLong)(RedisModuleDigest *md, long long ele);
void REDISMODULE_API_FUNC(RedisModule_DigestEndSequence)(RedisModuleDigest *md);
RedisModuleDict *REDISMODULE_API_FUNC(RedisModule_CreateDict)(RedisModuleCtx *ctx);
void REDISMODULE_API_FUNC(RedisModule_FreeDict)(RedisModuleCtx *ctx, RedisModuleDict *d);
uint64_t REDISMODULE_API_FUNC(RedisModule_DictSize)(RedisModuleDict *d);
int REDISMODULE_API_FUNC(RedisModule_DictSetC)(RedisModuleDict *d, void *key, size_t keylen, void *ptr);
int REDISMODULE_API_FUNC(RedisModule_DictReplaceC)(RedisModuleDict *d, void *key, size_t keylen, void *ptr);
int REDISMODULE_API_FUNC(RedisModule_DictSet)(RedisModuleDict *d, RedisModuleString *key, void *ptr);
int REDISMODULE_API_FUNC(RedisModule_DictReplace)(RedisModuleDict *d, RedisModuleString *key, void *ptr);
void *REDISMODULE_API_FUNC(RedisModule_DictGetC)(RedisModuleDict *d, void *key, size_t keylen, int *nokey);
void *REDISMODULE_API_FUNC(RedisModule_DictGet)(RedisModuleDict *d, RedisModuleString *key, int *nokey);
int REDISMODULE_API_FUNC(RedisModule_DictDelC)(RedisModuleDict *d, void *key, size_t keylen, void *oldval);
int REDISMODULE_API_FUNC(RedisModule_DictDel)(RedisModuleDict *d, RedisModuleString *key, void *oldval);
RedisModuleDictIter *REDISMODULE_API_FUNC(RedisModule_DictIteratorStartC)(RedisModuleDict *d, const char *op, void *key, size_t keylen);
RedisModuleDictIter *REDISMODULE_API_FUNC(RedisModule_DictIteratorStart)(RedisModuleDict *d, const char *op, RedisModuleString *key);
void REDISMODULE_API_FUNC(RedisModule_DictIteratorStop)(RedisModuleDictIter *di);
int REDISMODULE_API_FUNC(RedisModule_DictIteratorReseekC)(RedisModuleDictIter *di, const char *op, void *key, size_t keylen);
int REDISMODULE_API_FUNC(RedisModule_DictIteratorReseek)(RedisModuleDictIter *di, const char *op, RedisModuleString *key);
void *REDISMODULE_API_FUNC(RedisModule_DictNextC)(RedisModuleDictIter *di, size_t *keylen, void **dataptr);
void *REDISMODULE_API_FUNC(RedisModule_DictPrevC)(RedisModuleDictIter *di, size_t *keylen, void **dataptr);
RedisModuleString *REDISMODULE_API_FUNC(RedisModule_DictNext)(RedisModuleCtx *ctx, RedisModuleDictIter *di, void **dataptr);
RedisModuleString *REDISMODULE_API_FUNC(RedisModule_DictPrev)(RedisModuleCtx *ctx, RedisModuleDictIter *di, void **dataptr);
int REDISMODULE_API_FUNC(RedisModule_DictCompareC)(RedisModuleDictIter *di, const char *op, void *key, size_t keylen);
int REDISMODULE_API_FUNC(RedisModule_DictCompare)(RedisModuleDictIter *di, const char *op, RedisModuleString *key);
int REDISMODULE_API_FUNC(RedisModule_RegisterInfoFunc)(RedisModuleCtx *ctx, RedisModuleInfoFunc cb);
int REDISMODULE_API_FUNC(RedisModule_InfoAddSection)(RedisModuleInfoCtx *ctx, char *name);
int REDISMODULE_API_FUNC(RedisModule_InfoBeginDictField)(RedisModuleInfoCtx *ctx, char *name);
int REDISMODULE_API_FUNC(RedisModule_InfoEndDictField)(RedisModuleInfoCtx *ctx);
int REDISMODULE_API_FUNC(RedisModule_InfoAddFieldString)(RedisModuleInfoCtx *ctx, char *field, RedisModuleString *value);
int REDISMODULE_API_FUNC(RedisModule_InfoAddFieldCString)(RedisModuleInfoCtx *ctx, char *field, char *value);
int REDISMODULE_API_FUNC(RedisModule_InfoAddFieldDouble)(RedisModuleInfoCtx *ctx, char *field, double value);
int REDISMODULE_API_FUNC(RedisModule_InfoAddFieldLongLong)(RedisModuleInfoCtx *ctx, char *field, long long value);
int REDISMODULE_API_FUNC(RedisModule_InfoAddFieldULongLong)(RedisModuleInfoCtx *ctx, char *field, unsigned long long value);
int REDISMODULE_API_FUNC(RedisModule_SubscribeToServerEvent)(RedisModuleCtx *ctx, RedisModuleEvent event, RedisModuleEventCallback callback);
int REDISMODULE_API_FUNC(RedisModule_SetLRUOrLFU)(RedisModuleKey *key, long long lfu_freq, long long lru_idle);
int REDISMODULE_API_FUNC(RedisModule_GetLRUOrLFU)(RedisModuleKey *key, long long *lfu_freq, long long *lru_idle);

/* Experimental APIs */
#ifdef REDISMODULE_EXPERIMENTAL_API
#define REDISMODULE_EXPERIMENTAL_API_VERSION 3
RedisModuleBlockedClient *REDISMODULE_API_FUNC(RedisModule_BlockClient)(RedisModuleCtx *ctx, RedisModuleCmdFunc reply_callback, RedisModuleCmdFunc timeout_callback, void (*free_privdata)(RedisModuleCtx*,void*), long long timeout_ms);
int REDISMODULE_API_FUNC(RedisModule_UnblockClient)(RedisModuleBlockedClient *bc, void *privdata);
int REDISMODULE_API_FUNC(RedisModule_IsBlockedReplyRequest)(RedisModuleCtx *ctx);
int REDISMODULE_API_FUNC(RedisModule_IsBlockedTimeoutRequest)(RedisModuleCtx *ctx);
void *REDISMODULE_API_FUNC(RedisModule_GetBlockedClientPrivateData)(RedisModuleCtx *ctx);
RedisModuleBlockedClient *REDISMODULE_API_FUNC(RedisModule_GetBlockedClientHandle)(RedisModuleCtx *ctx);
int REDISMODULE_API_FUNC(RedisModule_AbortBlock)(RedisModuleBlockedClient *bc);
RedisModuleCtx *REDISMODULE_API_FUNC(RedisModule_GetThreadSafeContext)(RedisModuleBlockedClient *bc);
void REDISMODULE_API_FUNC(RedisModule_FreeThreadSafeContext)(RedisModuleCtx *ctx);
void REDISMODULE_API_FUNC(RedisModule_ThreadSafeContextLock)(RedisModuleCtx *ctx);
void REDISMODULE_API_FUNC(RedisModule_ThreadSafeContextUnlock)(RedisModuleCtx *ctx);
int REDISMODULE_API_FUNC(RedisModule_SubscribeToKeyspaceEvents)(RedisModuleCtx *ctx, int types, RedisModuleNotificationFunc cb);
int REDISMODULE_API_FUNC(RedisModule_NotifyKeyspaceEvent)(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key);
int REDISMODULE_API_FUNC(RedisModule_GetNotifyKeyspaceEvents)();
int REDISMODULE_API_FUNC(RedisModule_BlockedClientDisconnected)(RedisModuleCtx *ctx);
void REDISMODULE_API_FUNC(RedisModule_RegisterClusterMessageReceiver)(RedisModuleCtx *ctx, uint8_t type, RedisModuleClusterMessageReceiver callback);
int REDISMODULE_API_FUNC(RedisModule_SendClusterMessage)(RedisModuleCtx *ctx, char *target_id, uint8_t type, unsigned char *msg, uint32_t len);
int REDISMODULE_API_FUNC(RedisModule_GetClusterNodeInfo)(RedisModuleCtx *ctx, const char *id, char *ip, char *master_id, int *port, int *flags);
char **REDISMODULE_API_FUNC(RedisModule_GetClusterNodesList)(RedisModuleCtx *ctx, size_t *numnodes);
void REDISMODULE_API_FUNC(RedisModule_FreeClusterNodesList)(char **ids);
RedisModuleTimerID REDISMODULE_API_FUNC(RedisModule_CreateTimer)(RedisModuleCtx *ctx, mstime_t period, RedisModuleTimerProc callback, void *data);
int REDISMODULE_API_FUNC(RedisModule_StopTimer)(RedisModuleCtx *ctx, RedisModuleTimerID id, void **data);
int REDISMODULE_API_FUNC(RedisModule_GetTimerInfo)(RedisModuleCtx *ctx, RedisModuleTimerID id, uint64_t *remaining, void **data);
const char *REDISMODULE_API_FUNC(RedisModule_GetMyClusterID)(void);
size_t REDISMODULE_API_FUNC(RedisModule_GetClusterSize)(void);
void REDISMODULE_API_FUNC(RedisModule_GetRandomBytes)(unsigned char *dst, size_t len);
void REDISMODULE_API_FUNC(RedisModule_GetRandomHexChars)(char *dst, size_t len);
void REDISMODULE_API_FUNC(RedisModule_SetDisconnectCallback)(RedisModuleBlockedClient *bc, RedisModuleDisconnectFunc callback);
void REDISMODULE_API_FUNC(RedisModule_SetClusterFlags)(RedisModuleCtx *ctx, uint64_t flags);
int REDISMODULE_API_FUNC(RedisModule_ExportSharedAPI)(RedisModuleCtx *ctx, const char *apiname, void *func);
void *REDISMODULE_API_FUNC(RedisModule_GetSharedAPI)(RedisModuleCtx *ctx, const char *apiname);
RedisModuleCommandFilter *REDISMODULE_API_FUNC(RedisModule_RegisterCommandFilter)(RedisModuleCtx *ctx, RedisModuleCommandFilterFunc cb, int flags);
int REDISMODULE_API_FUNC(RedisModule_UnregisterCommandFilter)(RedisModuleCtx *ctx, RedisModuleCommandFilter *filter);
int REDISMODULE_API_FUNC(RedisModule_CommandFilterArgsCount)(RedisModuleCommandFilterCtx *fctx);
const RedisModuleString *REDISMODULE_API_FUNC(RedisModule_CommandFilterArgGet)(RedisModuleCommandFilterCtx *fctx, int pos);
int REDISMODULE_API_FUNC(RedisModule_CommandFilterArgInsert)(RedisModuleCommandFilterCtx *fctx, int pos, RedisModuleString *arg);
int REDISMODULE_API_FUNC(RedisModule_CommandFilterArgReplace)(RedisModuleCommandFilterCtx *fctx, int pos, RedisModuleString *arg);
int REDISMODULE_API_FUNC(RedisModule_CommandFilterArgDelete)(RedisModuleCommandFilterCtx *fctx, int pos);
int REDISMODULE_API_FUNC(RedisModule_Fork)(RedisModuleForkDoneHandler cb, void *user_data);
int REDISMODULE_API_FUNC(RedisModule_ExitFromChild)(int retcode);
int REDISMODULE_API_FUNC(RedisModule_KillForkChild)(int child_pid);
#endif

#define RedisModule_IsAOFClient(id) ((id) == UINT64_MAX)

/* This is included inline inside each Redis module. */
static int RedisModule_Init(RedisModuleCtx *ctx, const char *name, int ver, int apiver) __attribute__((unused));
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
    REDISMODULE_GET_API(CreateStringFromString);
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
    REDISMODULE_GET_API(IsKeysPositionRequest);
    REDISMODULE_GET_API(KeyAtPos);
    REDISMODULE_GET_API(GetClientId);
    REDISMODULE_GET_API(GetContextFlags);
    REDISMODULE_GET_API(PoolAlloc);
    REDISMODULE_GET_API(CreateDataType);
    REDISMODULE_GET_API(ModuleTypeSetValue);
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
    REDISMODULE_GET_API(EmitAOF);
    REDISMODULE_GET_API(Log);
    REDISMODULE_GET_API(LogIOError);
    REDISMODULE_GET_API(_Assert);
    REDISMODULE_GET_API(LatencyAddSample);
    REDISMODULE_GET_API(StringAppendBuffer);
    REDISMODULE_GET_API(RetainString);
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
    REDISMODULE_GET_API(GetClientInfoById);
    REDISMODULE_GET_API(SubscribeToServerEvent);
    REDISMODULE_GET_API(SetLRUOrLFU);
    REDISMODULE_GET_API(GetLRUOrLFU);

#ifdef REDISMODULE_EXPERIMENTAL_API
    REDISMODULE_GET_API(GetThreadSafeContext);
    REDISMODULE_GET_API(FreeThreadSafeContext);
    REDISMODULE_GET_API(ThreadSafeContextLock);
    REDISMODULE_GET_API(ThreadSafeContextUnlock);
    REDISMODULE_GET_API(BlockClient);
    REDISMODULE_GET_API(UnblockClient);
    REDISMODULE_GET_API(IsBlockedReplyRequest);
    REDISMODULE_GET_API(IsBlockedTimeoutRequest);
    REDISMODULE_GET_API(GetBlockedClientPrivateData);
    REDISMODULE_GET_API(GetBlockedClientHandle);
    REDISMODULE_GET_API(AbortBlock);
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
    REDISMODULE_GET_API(ExitFromChild);
    REDISMODULE_GET_API(KillForkChild);
#endif

    if (RedisModule_IsModuleNameBusy && RedisModule_IsModuleNameBusy(name)) return REDISMODULE_ERR;
    RedisModule_SetModuleAttribs(ctx,name,ver,apiver);
    return REDISMODULE_OK;
}

#define RedisModule_Assert(_e) ((_e)?(void)0 : (RedisModule__Assert(#_e,__FILE__,__LINE__),exit(1)))

#else

/* Things only defined for the modules core, not exported to modules
 * including this file. */
#define RedisModuleString robj

#endif /* REDISMODULE_CORE */
#endif /* REDISMOUDLE_H */

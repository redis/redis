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

#ifdef REDISMODULE_USE_EXTERN
#define REDISMODULE_API_FUNC(T, N) extern T(*N)
#else
#define REDISMODULE_API_FUNC(T, N) T (*N)
#endif

REDISMODULE_API_FUNC(void *,RedisModule_Alloc)(size_t bytes);
REDISMODULE_API_FUNC(void *,RedisModule_Realloc)(void *ptr, size_t bytes);
REDISMODULE_API_FUNC(void ,RedisModule_Free)(void *ptr);
REDISMODULE_API_FUNC(void *,RedisModule_Calloc)(size_t nmemb, size_t size);
REDISMODULE_API_FUNC(char *,RedisModule_Strdup)(const char *str);
REDISMODULE_API_FUNC(int ,RedisModule_GetApi)(const char *, void *);
REDISMODULE_API_FUNC(int ,RedisModule_CreateCommand)(RedisModuleCtx *ctx, const char *name, RedisModuleCmdFunc cmdfunc, const char *strflags, int firstkey, int lastkey, int keystep);
REDISMODULE_API_FUNC(void ,RedisModule_SetModuleAttribs)(RedisModuleCtx *ctx, const char *name, int ver, int apiver);
REDISMODULE_API_FUNC(int ,RedisModule_IsModuleNameBusy)(const char *name);
REDISMODULE_API_FUNC(int ,RedisModule_WrongArity)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(int ,RedisModule_ReplyWithLongLong)(RedisModuleCtx *ctx, long long ll);
REDISMODULE_API_FUNC(int ,RedisModule_GetSelectedDb)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(int ,RedisModule_SelectDb)(RedisModuleCtx *ctx, int newid);
REDISMODULE_API_FUNC(RedisModuleKey *,RedisModule_OpenKey)(RedisModuleCtx *ctx, RedisModuleString *keyname, int mode);
REDISMODULE_API_FUNC(void ,RedisModule_CloseKey)(RedisModuleKey *kp);
REDISMODULE_API_FUNC(int ,RedisModule_KeyType)(RedisModuleKey *kp);
REDISMODULE_API_FUNC(size_t ,RedisModule_ValueLength)(RedisModuleKey *kp);
REDISMODULE_API_FUNC(int ,RedisModule_ListPush)(RedisModuleKey *kp, int where, RedisModuleString *ele);
REDISMODULE_API_FUNC(RedisModuleString *,RedisModule_ListPop)(RedisModuleKey *key, int where);
REDISMODULE_API_FUNC(RedisModuleCallReply *,RedisModule_Call)(RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...);
REDISMODULE_API_FUNC(const char *,RedisModule_CallReplyProto)(RedisModuleCallReply *reply, size_t *len);
REDISMODULE_API_FUNC(void ,RedisModule_FreeCallReply)(RedisModuleCallReply *reply);
REDISMODULE_API_FUNC(int ,RedisModule_CallReplyType)(RedisModuleCallReply *reply);
REDISMODULE_API_FUNC(long long ,RedisModule_CallReplyInteger)(RedisModuleCallReply *reply);
REDISMODULE_API_FUNC(size_t ,RedisModule_CallReplyLength)(RedisModuleCallReply *reply);
REDISMODULE_API_FUNC(RedisModuleCallReply *,RedisModule_CallReplyArrayElement)(RedisModuleCallReply *reply, size_t idx);
REDISMODULE_API_FUNC(RedisModuleString *,RedisModule_CreateString)(RedisModuleCtx *ctx, const char *ptr, size_t len);
REDISMODULE_API_FUNC(RedisModuleString *,RedisModule_CreateStringFromLongLong)(RedisModuleCtx *ctx, long long ll);
REDISMODULE_API_FUNC(RedisModuleString *,RedisModule_CreateStringFromString)(RedisModuleCtx *ctx, const RedisModuleString *str);
REDISMODULE_API_FUNC(RedisModuleString *,RedisModule_CreateStringPrintf)(RedisModuleCtx *ctx, const char *fmt, ...);
REDISMODULE_API_FUNC(void ,RedisModule_FreeString)(RedisModuleCtx *ctx, RedisModuleString *str);
REDISMODULE_API_FUNC(const char *,RedisModule_StringPtrLen)(const RedisModuleString *str, size_t *len);
REDISMODULE_API_FUNC(int ,RedisModule_ReplyWithError)(RedisModuleCtx *ctx, const char *err);
REDISMODULE_API_FUNC(int ,RedisModule_ReplyWithSimpleString)(RedisModuleCtx *ctx, const char *msg);
REDISMODULE_API_FUNC(int ,RedisModule_ReplyWithArray)(RedisModuleCtx *ctx, long len);
REDISMODULE_API_FUNC(void ,RedisModule_ReplySetArrayLength)(RedisModuleCtx *ctx, long len);
REDISMODULE_API_FUNC(int ,RedisModule_ReplyWithStringBuffer)(RedisModuleCtx *ctx, const char *buf, size_t len);
REDISMODULE_API_FUNC(int ,RedisModule_ReplyWithCString)(RedisModuleCtx *ctx, const char *buf);
REDISMODULE_API_FUNC(int ,RedisModule_ReplyWithString)(RedisModuleCtx *ctx, RedisModuleString *str);
REDISMODULE_API_FUNC(int ,RedisModule_ReplyWithNull)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(int ,RedisModule_ReplyWithDouble)(RedisModuleCtx *ctx, double d);
REDISMODULE_API_FUNC(int ,RedisModule_ReplyWithCallReply)(RedisModuleCtx *ctx, RedisModuleCallReply *reply);
REDISMODULE_API_FUNC(int ,RedisModule_StringToLongLong)(const RedisModuleString *str, long long *ll);
REDISMODULE_API_FUNC(int ,RedisModule_StringToDouble)(const RedisModuleString *str, double *d);
REDISMODULE_API_FUNC(void ,RedisModule_AutoMemory)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(int ,RedisModule_Replicate)(RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...);
REDISMODULE_API_FUNC(int ,RedisModule_ReplicateVerbatim)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(const char *,RedisModule_CallReplyStringPtr)(RedisModuleCallReply *reply, size_t *len);
REDISMODULE_API_FUNC(RedisModuleString *,RedisModule_CreateStringFromCallReply)(RedisModuleCallReply *reply);
REDISMODULE_API_FUNC(int ,RedisModule_DeleteKey)(RedisModuleKey *key);
REDISMODULE_API_FUNC(int ,RedisModule_UnlinkKey)(RedisModuleKey *key);
REDISMODULE_API_FUNC(int ,RedisModule_StringSet)(RedisModuleKey *key, RedisModuleString *str);
REDISMODULE_API_FUNC(char *,RedisModule_StringDMA)(RedisModuleKey *key, size_t *len, int mode);
REDISMODULE_API_FUNC(int ,RedisModule_StringTruncate)(RedisModuleKey *key, size_t newlen);
REDISMODULE_API_FUNC(mstime_t ,RedisModule_GetExpire)(RedisModuleKey *key);
REDISMODULE_API_FUNC(int ,RedisModule_SetExpire)(RedisModuleKey *key, mstime_t expire);
REDISMODULE_API_FUNC(int ,RedisModule_ZsetAdd)(RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr);
REDISMODULE_API_FUNC(int ,RedisModule_ZsetIncrby)(RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr, double *newscore);
REDISMODULE_API_FUNC(int ,RedisModule_ZsetScore)(RedisModuleKey *key, RedisModuleString *ele, double *score);
REDISMODULE_API_FUNC(int ,RedisModule_ZsetRem)(RedisModuleKey *key, RedisModuleString *ele, int *deleted);
REDISMODULE_API_FUNC(void ,RedisModule_ZsetRangeStop)(RedisModuleKey *key);
REDISMODULE_API_FUNC(int ,RedisModule_ZsetFirstInScoreRange)(RedisModuleKey *key, double min, double max, int minex, int maxex);
REDISMODULE_API_FUNC(int ,RedisModule_ZsetLastInScoreRange)(RedisModuleKey *key, double min, double max, int minex, int maxex);
REDISMODULE_API_FUNC(int ,RedisModule_ZsetFirstInLexRange)(RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max);
REDISMODULE_API_FUNC(int ,RedisModule_ZsetLastInLexRange)(RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max);
REDISMODULE_API_FUNC(RedisModuleString *,RedisModule_ZsetRangeCurrentElement)(RedisModuleKey *key, double *score);
REDISMODULE_API_FUNC(int ,RedisModule_ZsetRangeNext)(RedisModuleKey *key);
REDISMODULE_API_FUNC(int ,RedisModule_ZsetRangePrev)(RedisModuleKey *key);
REDISMODULE_API_FUNC(int ,RedisModule_ZsetRangeEndReached)(RedisModuleKey *key);
REDISMODULE_API_FUNC(int ,RedisModule_HashSet)(RedisModuleKey *key, int flags, ...);
REDISMODULE_API_FUNC(int ,RedisModule_HashGet)(RedisModuleKey *key, int flags, ...);
REDISMODULE_API_FUNC(int ,RedisModule_IsKeysPositionRequest)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(void ,RedisModule_KeyAtPos)(RedisModuleCtx *ctx, int pos);
REDISMODULE_API_FUNC(unsigned long long ,RedisModule_GetClientId)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(int ,RedisModule_GetContextFlags)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(void *,RedisModule_PoolAlloc)(RedisModuleCtx *ctx, size_t bytes);
REDISMODULE_API_FUNC(RedisModuleType *,RedisModule_CreateDataType)(RedisModuleCtx *ctx, const char *name, int encver, RedisModuleTypeMethods *typemethods);
REDISMODULE_API_FUNC(int ,RedisModule_ModuleTypeSetValue)(RedisModuleKey *key, RedisModuleType *mt, void *value);
REDISMODULE_API_FUNC(RedisModuleType *,RedisModule_ModuleTypeGetType)(RedisModuleKey *key);
REDISMODULE_API_FUNC(void *,RedisModule_ModuleTypeGetValue)(RedisModuleKey *key);
REDISMODULE_API_FUNC(void ,RedisModule_SaveUnsigned)(RedisModuleIO *io, uint64_t value);
REDISMODULE_API_FUNC(uint64_t ,RedisModule_LoadUnsigned)(RedisModuleIO *io);
REDISMODULE_API_FUNC(void ,RedisModule_SaveSigned)(RedisModuleIO *io, int64_t value);
REDISMODULE_API_FUNC(int64_t ,RedisModule_LoadSigned)(RedisModuleIO *io);
REDISMODULE_API_FUNC(void ,RedisModule_EmitAOF)(RedisModuleIO *io, const char *cmdname, const char *fmt, ...);
REDISMODULE_API_FUNC(void ,RedisModule_SaveString)(RedisModuleIO *io, RedisModuleString *s);
REDISMODULE_API_FUNC(void ,RedisModule_SaveStringBuffer)(RedisModuleIO *io, const char *str, size_t len);
REDISMODULE_API_FUNC(RedisModuleString *,RedisModule_LoadString)(RedisModuleIO *io);
REDISMODULE_API_FUNC(char *,RedisModule_LoadStringBuffer)(RedisModuleIO *io, size_t *lenptr);
REDISMODULE_API_FUNC(void ,RedisModule_SaveDouble)(RedisModuleIO *io, double value);
REDISMODULE_API_FUNC(double ,RedisModule_LoadDouble)(RedisModuleIO *io);
REDISMODULE_API_FUNC(void ,RedisModule_SaveFloat)(RedisModuleIO *io, float value);
REDISMODULE_API_FUNC(float ,RedisModule_LoadFloat)(RedisModuleIO *io);
REDISMODULE_API_FUNC(void ,RedisModule_Log)(RedisModuleCtx *ctx, const char *level, const char *fmt, ...);
REDISMODULE_API_FUNC(void ,RedisModule_LogIOError)(RedisModuleIO *io, const char *levelstr, const char *fmt, ...);
REDISMODULE_API_FUNC(int ,RedisModule_StringAppendBuffer)(RedisModuleCtx *ctx, RedisModuleString *str, const char *buf, size_t len);
REDISMODULE_API_FUNC(void ,RedisModule_RetainString)(RedisModuleCtx *ctx, RedisModuleString *str);
REDISMODULE_API_FUNC(int ,RedisModule_StringCompare)(RedisModuleString *a, RedisModuleString *b);
REDISMODULE_API_FUNC(RedisModuleCtx *,RedisModule_GetContextFromIO)(RedisModuleIO *io);
REDISMODULE_API_FUNC(const RedisModuleString *,RedisModule_GetKeyNameFromIO)(RedisModuleIO *io);
REDISMODULE_API_FUNC(long long ,RedisModule_Milliseconds)(void);
REDISMODULE_API_FUNC(void ,RedisModule_DigestAddStringBuffer)(RedisModuleDigest *md, unsigned char *ele, size_t len);
REDISMODULE_API_FUNC(void ,RedisModule_DigestAddLongLong)(RedisModuleDigest *md, long long ele);
REDISMODULE_API_FUNC(void ,RedisModule_DigestEndSequence)(RedisModuleDigest *md);
REDISMODULE_API_FUNC(RedisModuleDict *,RedisModule_CreateDict)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(void ,RedisModule_FreeDict)(RedisModuleCtx *ctx, RedisModuleDict *d);
REDISMODULE_API_FUNC(uint64_t ,RedisModule_DictSize)(RedisModuleDict *d);
REDISMODULE_API_FUNC(int ,RedisModule_DictSetC)(RedisModuleDict *d, void *key, size_t keylen, void *ptr);
REDISMODULE_API_FUNC(int ,RedisModule_DictReplaceC)(RedisModuleDict *d, void *key, size_t keylen, void *ptr);
REDISMODULE_API_FUNC(int ,RedisModule_DictSet)(RedisModuleDict *d, RedisModuleString *key, void *ptr);
REDISMODULE_API_FUNC(int ,RedisModule_DictReplace)(RedisModuleDict *d, RedisModuleString *key, void *ptr);
REDISMODULE_API_FUNC(void *,RedisModule_DictGetC)(RedisModuleDict *d, void *key, size_t keylen, int *nokey);
REDISMODULE_API_FUNC(void *,RedisModule_DictGet)(RedisModuleDict *d, RedisModuleString *key, int *nokey);
REDISMODULE_API_FUNC(int ,RedisModule_DictDelC)(RedisModuleDict *d, void *key, size_t keylen, void *oldval);
REDISMODULE_API_FUNC(int ,RedisModule_DictDel)(RedisModuleDict *d, RedisModuleString *key, void *oldval);
REDISMODULE_API_FUNC(RedisModuleDictIter *,RedisModule_DictIteratorStartC)(RedisModuleDict *d, const char *op, void *key, size_t keylen);
REDISMODULE_API_FUNC(RedisModuleDictIter *,RedisModule_DictIteratorStart)(RedisModuleDict *d, const char *op, RedisModuleString *key);
REDISMODULE_API_FUNC(void ,RedisModule_DictIteratorStop)(RedisModuleDictIter *di);
REDISMODULE_API_FUNC(int ,RedisModule_DictIteratorReseekC)(RedisModuleDictIter *di, const char *op, void *key, size_t keylen);
REDISMODULE_API_FUNC(int ,RedisModule_DictIteratorReseek)(RedisModuleDictIter *di, const char *op, RedisModuleString *key);
REDISMODULE_API_FUNC(void *,RedisModule_DictNextC)(RedisModuleDictIter *di, size_t *keylen, void **dataptr);
REDISMODULE_API_FUNC(void *,RedisModule_DictPrevC)(RedisModuleDictIter *di, size_t *keylen, void **dataptr);
REDISMODULE_API_FUNC(RedisModuleString *,RedisModule_DictNext)(RedisModuleCtx *ctx, RedisModuleDictIter *di, void **dataptr);
REDISMODULE_API_FUNC(RedisModuleString *,RedisModule_DictPrev)(RedisModuleCtx *ctx, RedisModuleDictIter *di, void **dataptr);
REDISMODULE_API_FUNC(int ,RedisModule_DictCompareC)(RedisModuleDictIter *di, const char *op, void *key, size_t keylen);
REDISMODULE_API_FUNC(int ,RedisModule_DictCompare)(RedisModuleDictIter *di, const char *op, RedisModuleString *key);

/* Experimental APIs */
#ifdef REDISMODULE_EXPERIMENTAL_API
#define REDISMODULE_EXPERIMENTAL_API_VERSION 3
REDISMODULE_API_FUNC(RedisModuleBlockedClient *,RedisModule_BlockClient)(RedisModuleCtx *ctx, RedisModuleCmdFunc reply_callback, RedisModuleCmdFunc timeout_callback, void (*free_privdata)(RedisModuleCtx*,void*), long long timeout_ms);
REDISMODULE_API_FUNC(int ,RedisModule_UnblockClient)(RedisModuleBlockedClient *bc, void *privdata);
REDISMODULE_API_FUNC(int ,RedisModule_IsBlockedReplyRequest)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(int ,RedisModule_IsBlockedTimeoutRequest)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(void *,RedisModule_GetBlockedClientPrivateData)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(RedisModuleBlockedClient *,RedisModule_GetBlockedClientHandle)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(int ,RedisModule_AbortBlock)(RedisModuleBlockedClient *bc);
REDISMODULE_API_FUNC(RedisModuleCtx *,RedisModule_GetThreadSafeContext)(RedisModuleBlockedClient *bc);
REDISMODULE_API_FUNC(void ,RedisModule_FreeThreadSafeContext)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(void ,RedisModule_ThreadSafeContextLock)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(void ,RedisModule_ThreadSafeContextUnlock)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(int ,RedisModule_SubscribeToKeyspaceEvents)(RedisModuleCtx *ctx, int types, RedisModuleNotificationFunc cb);
REDISMODULE_API_FUNC(int ,RedisModule_BlockedClientDisconnected)(RedisModuleCtx *ctx);
REDISMODULE_API_FUNC(void ,RedisModule_RegisterClusterMessageReceiver)(RedisModuleCtx *ctx, uint8_t type, RedisModuleClusterMessageReceiver callback);
REDISMODULE_API_FUNC(int ,RedisModule_SendClusterMessage)(RedisModuleCtx *ctx, char *target_id, uint8_t type, unsigned char *msg, uint32_t len);
REDISMODULE_API_FUNC(int ,RedisModule_GetClusterNodeInfo)(RedisModuleCtx *ctx, const char *id, char *ip, char *master_id, int *port, int *flags);
REDISMODULE_API_FUNC(char **,RedisModule_GetClusterNodesList)(RedisModuleCtx *ctx, size_t *numnodes);
REDISMODULE_API_FUNC(void ,RedisModule_FreeClusterNodesList)(char **ids);
REDISMODULE_API_FUNC(RedisModuleTimerID ,RedisModule_CreateTimer)(RedisModuleCtx *ctx, mstime_t period, RedisModuleTimerProc callback, void *data);
REDISMODULE_API_FUNC(int ,RedisModule_StopTimer)(RedisModuleCtx *ctx, RedisModuleTimerID id, void **data);
REDISMODULE_API_FUNC(int ,RedisModule_GetTimerInfo)(RedisModuleCtx *ctx, RedisModuleTimerID id, uint64_t *remaining, void **data);
REDISMODULE_API_FUNC(const char *,RedisModule_GetMyClusterID)(void);
REDISMODULE_API_FUNC(size_t ,RedisModule_GetClusterSize)(void);
REDISMODULE_API_FUNC(void ,RedisModule_GetRandomBytes)(unsigned char *dst, size_t len);
REDISMODULE_API_FUNC(void ,RedisModule_GetRandomHexChars)(char *dst, size_t len);
REDISMODULE_API_FUNC(void ,RedisModule_SetDisconnectCallback)(RedisModuleBlockedClient *bc, RedisModuleDisconnectFunc callback);
REDISMODULE_API_FUNC(void ,RedisModule_SetClusterFlags)(RedisModuleCtx *ctx, uint64_t flags);
REDISMODULE_API_FUNC(int ,RedisModule_ExportSharedAPI)(RedisModuleCtx *ctx, const char *apiname, void *func);
REDISMODULE_API_FUNC(void *,RedisModule_GetSharedAPI)(RedisModuleCtx *ctx, const char *apiname);
REDISMODULE_API_FUNC(RedisModuleCommandFilter *,RedisModule_RegisterCommandFilter)(RedisModuleCtx *ctx, RedisModuleCommandFilterFunc cb, int flags);
REDISMODULE_API_FUNC(int ,RedisModule_UnregisterCommandFilter)(RedisModuleCtx *ctx, RedisModuleCommandFilter *filter);
REDISMODULE_API_FUNC(int ,RedisModule_CommandFilterArgsCount)(RedisModuleCommandFilterCtx *fctx);
REDISMODULE_API_FUNC(const RedisModuleString *,RedisModule_CommandFilterArgGet)(RedisModuleCommandFilterCtx *fctx, int pos);
REDISMODULE_API_FUNC(int ,RedisModule_CommandFilterArgInsert)(RedisModuleCommandFilterCtx *fctx, int pos, RedisModuleString *arg);
REDISMODULE_API_FUNC(int ,RedisModule_CommandFilterArgReplace)(RedisModuleCommandFilterCtx *fctx, int pos, RedisModuleString *arg);
REDISMODULE_API_FUNC(int ,RedisModule_CommandFilterArgDelete)(RedisModuleCommandFilterCtx *fctx, int pos);
#endif

#define REDISMODULE_XAPI_STABLE(X) \
    X(Alloc)                         \
    X(Calloc)                        \
    X(Free)                          \
    X(Realloc)                       \
    X(Strdup)                        \
    X(CreateCommand)                 \
    X(SetModuleAttribs)              \
    X(IsModuleNameBusy)              \
    X(WrongArity)                    \
    X(ReplyWithLongLong)             \
    X(ReplyWithError)                \
    X(ReplyWithSimpleString)         \
    X(ReplyWithArray)                \
    X(ReplySetArrayLength)           \
    X(ReplyWithStringBuffer)         \
    X(ReplyWithCString)              \
    X(ReplyWithString)               \
    X(ReplyWithNull)                 \
    X(ReplyWithCallReply)            \
    X(ReplyWithDouble)               \
    X(ReplySetArrayLength)           \
    X(GetSelectedDb)                 \
    X(SelectDb)                      \
    X(OpenKey)                       \
    X(CloseKey)                      \
    X(KeyType)                       \
    X(ValueLength)                   \
    X(ListPush)                      \
    X(ListPop)                       \
    X(StringToLongLong)              \
    X(StringToDouble)                \
    X(Call)                          \
    X(CallReplyProto)                \
    X(FreeCallReply)                 \
    X(CallReplyInteger)              \
    X(CallReplyType)                 \
    X(CallReplyLength)               \
    X(CallReplyArrayElement)         \
    X(CallReplyStringPtr)            \
    X(CreateStringFromCallReply)     \
    X(CreateString)                  \
    X(CreateStringFromLongLong)      \
    X(CreateStringFromString)        \
    X(CreateStringPrintf)            \
    X(FreeString)                    \
    X(StringPtrLen)                  \
    X(AutoMemory)                    \
    X(Replicate)                     \
    X(ReplicateVerbatim)             \
    X(DeleteKey)                     \
    X(UnlinkKey)                     \
    X(StringSet)                     \
    X(StringDMA)                     \
    X(StringTruncate)                \
    X(GetExpire)                     \
    X(SetExpire)                     \
    X(ZsetAdd)                       \
    X(ZsetIncrby)                    \
    X(ZsetScore)                     \
    X(ZsetRem)                       \
    X(ZsetRangeStop)                 \
    X(ZsetFirstInScoreRange)         \
    X(ZsetLastInScoreRange)          \
    X(ZsetFirstInLexRange)           \
    X(ZsetLastInLexRange)            \
    X(ZsetRangeCurrentElement)       \
    X(ZsetRangeNext)                 \
    X(ZsetRangePrev)                 \
    X(ZsetRangeEndReached)           \
    X(HashSet)                       \
    X(HashGet)                       \
    X(IsKeysPositionRequest)         \
    X(KeyAtPos)                      \
    X(GetClientId)                   \
    X(GetContextFlags)               \
    X(PoolAlloc)                     \
    X(CreateDataType)                \
    X(ModuleTypeSetValue)            \
    X(ModuleTypeGetType)             \
    X(ModuleTypeGetValue)            \
    X(SaveUnsigned)                  \
    X(LoadUnsigned)                  \
    X(SaveSigned)                    \
    X(LoadSigned)                    \
    X(SaveString)                    \
    X(SaveStringBuffer)              \
    X(LoadString)                    \
    X(LoadStringBuffer)              \
    X(SaveDouble)                    \
    X(LoadDouble)                    \
    X(SaveFloat)                     \
    X(LoadFloat)                     \
    X(EmitAOF)                       \
    X(Log)                           \
    X(LogIOError)                    \
    X(StringAppendBuffer)            \
    X(RetainString)                  \
    X(StringCompare)                 \
    X(GetContextFromIO)              \
    X(GetKeyNameFromIO)              \
    X(Milliseconds)                  \
    X(DigestAddStringBuffer)         \
    X(DigestAddLongLong)             \
    X(DigestEndSequence)             \
    X(CreateDict)                    \
    X(FreeDict)                      \
    X(DictSize)                      \
    X(DictSetC)                      \
    X(DictReplaceC)                  \
    X(DictSet)                       \
    X(DictReplace)                   \
    X(DictGetC)                      \
    X(DictGet)                       \
    X(DictDelC)                      \
    X(DictDel)                       \
    X(DictIteratorStartC)            \
    X(DictIteratorStart)             \
    X(DictIteratorStop)              \
    X(DictIteratorReseekC)           \
    X(DictIteratorReseek)            \
    X(DictNextC)                     \
    X(DictPrevC)                     \
    X(DictNext)                      \
    X(DictPrev)                      \
    X(DictCompare)                   \
    X(DictCompareC)

#define REDISMODULE_XAPI_EXPERIMENTAL(X) \
    X(GetThreadSafeContext)                \
    X(FreeThreadSafeContext)               \
    X(ThreadSafeContextLock)               \
    X(ThreadSafeContextUnlock)             \
    X(BlockClient)                         \
    X(UnblockClient)                       \
    X(IsBlockedReplyRequest)               \
    X(IsBlockedTimeoutRequest)             \
    X(GetBlockedClientPrivateData)         \
    X(GetBlockedClientHandle)              \
    X(AbortBlock)                          \
    X(SetDisconnectCallback)               \
    X(SubscribeToKeyspaceEvents)           \
    X(BlockedClientDisconnected)           \
    X(RegisterClusterMessageReceiver)      \
    X(SendClusterMessage)                  \
    X(GetClusterNodeInfo)                  \
    X(GetClusterNodesList)                 \
    X(FreeClusterNodesList)                \
    X(CreateTimer)                         \
    X(StopTimer)                           \
    X(GetTimerInfo)                        \
    X(GetMyClusterID)                      \
    X(GetClusterSize)                      \
    X(GetRandomBytes)                      \
    X(GetRandomHexChars)                   \
    X(SetClusterFlags)                     \
    X(ExportSharedAPI)                     \
    X(GetSharedAPI)                        \
    X(RegisterCommandFilter)               \
    X(UnregisterCommandFilter)             \
    X(CommandFilterArgsCount)              \
    X(CommandFilterArgGet)                 \
    X(CommandFilterArgInsert)              \
    X(CommandFilterArgReplace)             \
    X(CommandFilterArgDelete)

#ifdef REDISMODULE_EXPERIMENTAL_API
#define REDISMODULE_XAPI(X) REDISMODULE_XAPI_STABLE(X) REDISMODULE_XAPI_EXPERIMENTAL(X)
#else
#define REDISMODULE_XAPI(X) REDISMODULE_XAPI_STABLE(X)
#endif

/* handy typedef */
typedef int (*RedisModule_GetApiFunctionType)(const char *name, void *pp);

/* This is included inline inside each Redis module. */
static int RedisModule_Init(RedisModuleCtx *ctx, const char *name, int ver, int apiver) __attribute__((unused));
static int RedisModule_Init(RedisModuleCtx *ctx, const char *name, int ver, int apiver) {
    RedisModule_GetApiFunctionType getapifuncptr = (RedisModule_GetApiFunctionType)((void **)ctx)[0];
    RedisModule_GetApi = (RedisModule_GetApiFunctionType) (unsigned long)getapifuncptr;

#define X(name) RedisModule_GetApi("RedisModule_" #name, (void *)&RedisModule_##name);
    REDISMODULE_XAPI(X)
#undef X
    if (RedisModule_IsModuleNameBusy && RedisModule_IsModuleNameBusy(name)) return REDISMODULE_ERR;
    RedisModule_SetModuleAttribs(ctx,name,ver,apiver);
    return REDISMODULE_OK;
}

#ifdef REDISMODULE_USE_EXTERN
#define REDISMODULE__INIT_WITH_NULL(name) \
  __typeof__(RedisModule_##name) RedisModule_##name = NULL;

/**
 * If redismodule API symbols are declared as extern, then they must be defined
 * once (not more, not less) in the executable or library. Use the
 * REDISMODULE_INIT_SYMBOLS() (at file scope) to define them.
 */
#define REDISMODULE_INIT_SYMBOLS() REDISMODULE_XAPI(REDISMODULE__INIT_WITH_NULL)
#endif

#else

/* Things only defined for the modules core, not exported to modules
 * including this file. */
#define RedisModuleString robj

#endif /* REDISMODULE_CORE */
#endif /* REDISMOUDLE_H */

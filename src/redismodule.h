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

/* A special pointer that we can use between the core and the module to signal
 * field deletion, and that is impossible to be a valid pointer. */
#define REDISMODULE_HASH_DELETE ((RedisModuleString*)(long)1)

/* Error messages. */
#define REDISMODULE_ERRORMSG_WRONGTYPE "WRONGTYPE Operation against a key holding the wrong kind of value"

#define REDISMODULE_POSITIVE_INFINITE (1.0/0.0)
#define REDISMODULE_NEGATIVE_INFINITE (-1.0/0.0)

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

typedef int (*RedisModuleCmdFunc) (RedisModuleCtx *ctx, RedisModuleString **argv, int argc);

typedef void *(*RedisModuleTypeLoadFunc)(RedisModuleIO *rdb, int encver);
typedef void (*RedisModuleTypeSaveFunc)(RedisModuleIO *rdb, void *value);
typedef void (*RedisModuleTypeRewriteFunc)(RedisModuleIO *aof, RedisModuleString *key, void *value);
typedef void (*RedisModuleTypeDigestFunc)(RedisModuleDigest *digest, void *value);
typedef void (*RedisModuleTypeFreeFunc)(void *value);

#define REDISMODULE_GET_API(name) \
    RedisModule_GetApi("RedisModule_" #name, ((void **)&RedisModule_ ## name))

#define REDISMODULE_API_FUNC(x) (*x)


extern void *REDISMODULE_API_FUNC(RedisModule_Alloc)(size_t bytes);
extern void *REDISMODULE_API_FUNC(RedisModule_Realloc)(void *ptr, size_t bytes);
extern void REDISMODULE_API_FUNC(RedisModule_Free)(void *ptr);
extern void *REDISMODULE_API_FUNC(RedisModule_Calloc)(size_t nmemb, size_t size);
extern char *REDISMODULE_API_FUNC(RedisModule_Strdup)(const char *str);
extern int REDISMODULE_API_FUNC(RedisModule_GetApi)(const char *, void *);
extern int REDISMODULE_API_FUNC(RedisModule_CreateCommand)(RedisModuleCtx *ctx, const char *name, RedisModuleCmdFunc cmdfunc, const char *strflags, int firstkey, int lastkey, int keystep);
extern int REDISMODULE_API_FUNC(RedisModule_SetModuleAttribs)(RedisModuleCtx *ctx, const char *name, int ver, int apiver);
extern int REDISMODULE_API_FUNC(RedisModule_WrongArity)(RedisModuleCtx *ctx);
extern int REDISMODULE_API_FUNC(RedisModule_ReplyWithLongLong)(RedisModuleCtx *ctx, long long ll);
extern int REDISMODULE_API_FUNC(RedisModule_GetSelectedDb)(RedisModuleCtx *ctx);
extern int REDISMODULE_API_FUNC(RedisModule_SelectDb)(RedisModuleCtx *ctx, int newid);
extern void *REDISMODULE_API_FUNC(RedisModule_OpenKey)(RedisModuleCtx *ctx, RedisModuleString *keyname, int mode);
extern void REDISMODULE_API_FUNC(RedisModule_CloseKey)(RedisModuleKey *kp);
extern int REDISMODULE_API_FUNC(RedisModule_KeyType)(RedisModuleKey *kp);
extern size_t REDISMODULE_API_FUNC(RedisModule_ValueLength)(RedisModuleKey *kp);
extern int REDISMODULE_API_FUNC(RedisModule_ListPush)(RedisModuleKey *kp, int where, RedisModuleString *ele);
extern RedisModuleString *REDISMODULE_API_FUNC(RedisModule_ListPop)(RedisModuleKey *key, int where);
extern RedisModuleCallReply *REDISMODULE_API_FUNC(RedisModule_Call)(RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...);
extern const char *REDISMODULE_API_FUNC(RedisModule_CallReplyProto)(RedisModuleCallReply *reply, size_t *len);
extern void REDISMODULE_API_FUNC(RedisModule_FreeCallReply)(RedisModuleCallReply *reply);
extern int REDISMODULE_API_FUNC(RedisModule_CallReplyType)(RedisModuleCallReply *reply);
extern long long REDISMODULE_API_FUNC(RedisModule_CallReplyInteger)(RedisModuleCallReply *reply);
extern size_t REDISMODULE_API_FUNC(RedisModule_CallReplyLength)(RedisModuleCallReply *reply);
extern RedisModuleCallReply *REDISMODULE_API_FUNC(RedisModule_CallReplyArrayElement)(RedisModuleCallReply *reply, size_t idx);
extern RedisModuleString *REDISMODULE_API_FUNC(RedisModule_CreateString)(RedisModuleCtx *ctx, const char *ptr, size_t len);
extern RedisModuleString *REDISMODULE_API_FUNC(RedisModule_CreateStringFromLongLong)(RedisModuleCtx *ctx, long long ll);
extern RedisModuleString *REDISMODULE_API_FUNC(RedisModule_CreateStringFromString)(RedisModuleCtx *ctx, const RedisModuleString *str);
extern void REDISMODULE_API_FUNC(RedisModule_FreeString)(RedisModuleCtx *ctx, RedisModuleString *str);
extern const char *REDISMODULE_API_FUNC(RedisModule_StringPtrLen)(const RedisModuleString *str, size_t *len);
extern int REDISMODULE_API_FUNC(RedisModule_ReplyWithError)(RedisModuleCtx *ctx, const char *err);
extern int REDISMODULE_API_FUNC(RedisModule_ReplyWithSimpleString)(RedisModuleCtx *ctx, const char *msg);
extern int REDISMODULE_API_FUNC(RedisModule_ReplyWithArray)(RedisModuleCtx *ctx, long len);
extern void REDISMODULE_API_FUNC(RedisModule_ReplySetArrayLength)(RedisModuleCtx *ctx, long len);
extern int REDISMODULE_API_FUNC(RedisModule_ReplyWithStringBuffer)(RedisModuleCtx *ctx, const char *buf, size_t len);
extern int REDISMODULE_API_FUNC(RedisModule_ReplyWithString)(RedisModuleCtx *ctx, RedisModuleString *str);
extern int REDISMODULE_API_FUNC(RedisModule_ReplyWithNull)(RedisModuleCtx *ctx);
extern int REDISMODULE_API_FUNC(RedisModule_ReplyWithDouble)(RedisModuleCtx *ctx, double d);
extern int REDISMODULE_API_FUNC(RedisModule_ReplyWithCallReply)(RedisModuleCtx *ctx, RedisModuleCallReply *reply);
extern int REDISMODULE_API_FUNC(RedisModule_StringToLongLong)(const RedisModuleString *str, long long *ll);
extern int REDISMODULE_API_FUNC(RedisModule_StringToDouble)(const RedisModuleString *str, double *d);
extern void REDISMODULE_API_FUNC(RedisModule_AutoMemory)(RedisModuleCtx *ctx);
extern int REDISMODULE_API_FUNC(RedisModule_Replicate)(RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...);
extern int REDISMODULE_API_FUNC(RedisModule_ReplicateVerbatim)(RedisModuleCtx *ctx);
extern const char *REDISMODULE_API_FUNC(RedisModule_CallReplyStringPtr)(RedisModuleCallReply *reply, size_t *len);
extern RedisModuleString *REDISMODULE_API_FUNC(RedisModule_CreateStringFromCallReply)(RedisModuleCallReply *reply);
extern int REDISMODULE_API_FUNC(RedisModule_DeleteKey)(RedisModuleKey *key);
extern int REDISMODULE_API_FUNC(RedisModule_StringSet)(RedisModuleKey *key, RedisModuleString *str);
extern char *REDISMODULE_API_FUNC(RedisModule_StringDMA)(RedisModuleKey *key, size_t *len, int mode);
extern int REDISMODULE_API_FUNC(RedisModule_StringTruncate)(RedisModuleKey *key, size_t newlen);
extern mstime_t REDISMODULE_API_FUNC(RedisModule_GetExpire)(RedisModuleKey *key);
extern int REDISMODULE_API_FUNC(RedisModule_SetExpire)(RedisModuleKey *key, mstime_t expire);
extern int REDISMODULE_API_FUNC(RedisModule_ZsetAdd)(RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr);
extern int REDISMODULE_API_FUNC(RedisModule_ZsetIncrby)(RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr, double *newscore);
extern int REDISMODULE_API_FUNC(RedisModule_ZsetScore)(RedisModuleKey *key, RedisModuleString *ele, double *score);
extern int REDISMODULE_API_FUNC(RedisModule_ZsetRem)(RedisModuleKey *key, RedisModuleString *ele, int *deleted);
extern void REDISMODULE_API_FUNC(RedisModule_ZsetRangeStop)(RedisModuleKey *key);
extern int REDISMODULE_API_FUNC(RedisModule_ZsetFirstInScoreRange)(RedisModuleKey *key, double min, double max, int minex, int maxex);
extern int REDISMODULE_API_FUNC(RedisModule_ZsetLastInScoreRange)(RedisModuleKey *key, double min, double max, int minex, int maxex);
extern int REDISMODULE_API_FUNC(RedisModule_ZsetFirstInLexRange)(RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max);
extern int REDISMODULE_API_FUNC(RedisModule_ZsetLastInLexRange)(RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max);
extern RedisModuleString *REDISMODULE_API_FUNC(RedisModule_ZsetRangeCurrentElement)(RedisModuleKey *key, double *score);
extern int REDISMODULE_API_FUNC(RedisModule_ZsetRangeNext)(RedisModuleKey *key);
extern int REDISMODULE_API_FUNC(RedisModule_ZsetRangePrev)(RedisModuleKey *key);
extern int REDISMODULE_API_FUNC(RedisModule_ZsetRangeEndReached)(RedisModuleKey *key);
extern int REDISMODULE_API_FUNC(RedisModule_HashSet)(RedisModuleKey *key, int flags, ...);
extern int REDISMODULE_API_FUNC(RedisModule_HashGet)(RedisModuleKey *key, int flags, ...);
extern int REDISMODULE_API_FUNC(RedisModule_IsKeysPositionRequest)(RedisModuleCtx *ctx);
extern void REDISMODULE_API_FUNC(RedisModule_KeyAtPos)(RedisModuleCtx *ctx, int pos);
extern unsigned long long REDISMODULE_API_FUNC(RedisModule_GetClientId)(RedisModuleCtx *ctx);
extern void *REDISMODULE_API_FUNC(RedisModule_PoolAlloc)(RedisModuleCtx *ctx, size_t bytes);
extern RedisModuleType *REDISMODULE_API_FUNC(RedisModule_CreateDataType)(RedisModuleCtx *ctx, const char *name, int encver, RedisModuleTypeLoadFunc rdb_load, RedisModuleTypeSaveFunc rdb_save, RedisModuleTypeRewriteFunc aof_rewrite, RedisModuleTypeDigestFunc digest, RedisModuleTypeFreeFunc free);
extern int REDISMODULE_API_FUNC(RedisModule_ModuleTypeSetValue)(RedisModuleKey *key, RedisModuleType *mt, void *value);
extern RedisModuleType *REDISMODULE_API_FUNC(RedisModule_ModuleTypeGetType)(RedisModuleKey *key);
extern void *REDISMODULE_API_FUNC(RedisModule_ModuleTypeGetValue)(RedisModuleKey *key);
extern void REDISMODULE_API_FUNC(RedisModule_SaveUnsigned)(RedisModuleIO *io, uint64_t value);
extern uint64_t REDISMODULE_API_FUNC(RedisModule_LoadUnsigned)(RedisModuleIO *io);
extern void REDISMODULE_API_FUNC(RedisModule_SaveSigned)(RedisModuleIO *io, int64_t value);
extern int64_t REDISMODULE_API_FUNC(RedisModule_LoadSigned)(RedisModuleIO *io);
extern void REDISMODULE_API_FUNC(RedisModule_EmitAOF)(RedisModuleIO *io, const char *cmdname, const char *fmt, ...);
extern void REDISMODULE_API_FUNC(RedisModule_SaveString)(RedisModuleIO *io, RedisModuleString *s);
extern void REDISMODULE_API_FUNC(RedisModule_SaveStringBuffer)(RedisModuleIO *io, const char *str, size_t len);
extern RedisModuleString *REDISMODULE_API_FUNC(RedisModule_LoadString)(RedisModuleIO *io);
extern char *REDISMODULE_API_FUNC(RedisModule_LoadStringBuffer)(RedisModuleIO *io, size_t *lenptr);
extern void REDISMODULE_API_FUNC(RedisModule_SaveDouble)(RedisModuleIO *io, double value);
extern double REDISMODULE_API_FUNC(RedisModule_LoadDouble)(RedisModuleIO *io);
extern void REDISMODULE_API_FUNC(RedisModule_Log)(RedisModuleCtx *ctx, const char *level, const char *fmt, ...);
extern int REDISMODULE_API_FUNC(RedisModule_StringAppendBuffer)(RedisModuleCtx *ctx, RedisModuleString *str, const char *buf, size_t len);
extern void REDISMODULE_API_FUNC(RedisModule_RetainString)(RedisModuleCtx *ctx, RedisModuleString *str);
extern int REDISMODULE_API_FUNC(RedisModule_StringCompare)(RedisModuleString *a, RedisModuleString *b);

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
    REDISMODULE_GET_API(WrongArity);
    REDISMODULE_GET_API(ReplyWithLongLong);
    REDISMODULE_GET_API(ReplyWithError);
    REDISMODULE_GET_API(ReplyWithSimpleString);
    REDISMODULE_GET_API(ReplyWithArray);
    REDISMODULE_GET_API(ReplySetArrayLength);
    REDISMODULE_GET_API(ReplyWithStringBuffer);
    REDISMODULE_GET_API(ReplyWithString);
    REDISMODULE_GET_API(ReplyWithNull);
    REDISMODULE_GET_API(ReplyWithCallReply);
    REDISMODULE_GET_API(ReplyWithDouble);
    REDISMODULE_GET_API(ReplySetArrayLength);
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
    REDISMODULE_GET_API(FreeString);
    REDISMODULE_GET_API(StringPtrLen);
    REDISMODULE_GET_API(AutoMemory);
    REDISMODULE_GET_API(Replicate);
    REDISMODULE_GET_API(ReplicateVerbatim);
    REDISMODULE_GET_API(DeleteKey);
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
    REDISMODULE_GET_API(PoolAlloc);
    REDISMODULE_GET_API(CreateDataType);
    REDISMODULE_GET_API(ModuleTypeSetValue);
    REDISMODULE_GET_API(ModuleTypeGetType);
    REDISMODULE_GET_API(ModuleTypeGetValue);
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
    REDISMODULE_GET_API(EmitAOF);
    REDISMODULE_GET_API(Log);
    REDISMODULE_GET_API(StringAppendBuffer);
    REDISMODULE_GET_API(RetainString);
    REDISMODULE_GET_API(StringCompare);

    RedisModule_SetModuleAttribs(ctx,name,ver,apiver);
    return REDISMODULE_OK;
}

#else

/* Things only defined for the modules core, not exported to modules
 * including this file. */
#define RedisModuleString robj

#endif /* REDISMODULE_CORE */
#endif /* REDISMOUDLE_H */

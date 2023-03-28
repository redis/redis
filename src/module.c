/*
 * Copyright (c) 2016, Salvatore Sanfilippo <antirez at gmail dot com>
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

/* --------------------------------------------------------------------------
 * Modules API documentation information
 *
 * The comments in this file are used to generate the API documentation on the
 * Redis website.
 *
 * Each function starting with RM_ and preceded by a block comment is included
 * in the API documentation. To hide an RM_ function, put a blank line between
 * the comment and the function definition or put the comment inside the
 * function body.
 *
 * The functions are divided into sections. Each section is preceded by a
 * documentation block, which is comment block starting with a markdown level 2
 * heading, i.e. a line starting with ##, on the first line of the comment block
 * (with the exception of a ----- line which can appear first). Other comment
 * blocks, which are not intended for the modules API user, such as this comment
 * block, do NOT start with a markdown level 2 heading, so they are included in
 * the generated a API documentation.
 *
 * The documentation comments may contain markdown formatting. Some automatic
 * replacements are done, such as the replacement of RM with RedisModule in
 * function names. For details, see the script src/modules/gendoc.rb.
 * -------------------------------------------------------------------------- */

#include "server.h"
#include "cluster.h"
#include "slowlog.h"
#include "rdb.h"
#include "monotonic.h"
#include "script.h"
#include "call_reply.h"
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Private data structures used by the modules system. Those are data
 * structures that are never exposed to Redis Modules, if not as void
 * pointers that have an API the module can call with them)
 * -------------------------------------------------------------------------- */

struct RedisModuleInfoCtx {
    struct RedisModule *module;
    dict *requested_sections;
    sds info;           /* info string we collected so far */
    int sections;       /* number of sections we collected so far */
    int in_section;     /* indication if we're in an active section or not */
    int in_dict_field;  /* indication that we're currently appending to a dict */
};

/* This represents a shared API. Shared APIs will be used to populate
 * the server.sharedapi dictionary, mapping names of APIs exported by
 * modules for other modules to use, to their structure specifying the
 * function pointer that can be called. */
struct RedisModuleSharedAPI {
    void *func;
    RedisModule *module;
};
typedef struct RedisModuleSharedAPI RedisModuleSharedAPI;

dict *modules; /* Hash table of modules. SDS -> RedisModule ptr.*/

/* Entries in the context->amqueue array, representing objects to free
 * when the callback returns. */
struct AutoMemEntry {
    void *ptr;
    int type;
};

/* AutoMemEntry type field values. */
#define REDISMODULE_AM_KEY 0
#define REDISMODULE_AM_STRING 1
#define REDISMODULE_AM_REPLY 2
#define REDISMODULE_AM_FREED 3 /* Explicitly freed by user already. */
#define REDISMODULE_AM_DICT 4
#define REDISMODULE_AM_INFO 5

/* The pool allocator block. Redis Modules can allocate memory via this special
 * allocator that will automatically release it all once the callback returns.
 * This means that it can only be used for ephemeral allocations. However
 * there are two advantages for modules to use this API:
 *
 * 1) The memory is automatically released when the callback returns.
 * 2) This allocator is faster for many small allocations since whole blocks
 *    are allocated, and small pieces returned to the caller just advancing
 *    the index of the allocation.
 *
 * Allocations are always rounded to the size of the void pointer in order
 * to always return aligned memory chunks. */

#define REDISMODULE_POOL_ALLOC_MIN_SIZE (1024*8)
#define REDISMODULE_POOL_ALLOC_ALIGN (sizeof(void*))

typedef struct RedisModulePoolAllocBlock {
    uint32_t size;
    uint32_t used;
    struct RedisModulePoolAllocBlock *next;
    char memory[];
} RedisModulePoolAllocBlock;

/* This structure represents the context in which Redis modules operate.
 * Most APIs module can access, get a pointer to the context, so that the API
 * implementation can hold state across calls, or remember what to free after
 * the call and so forth.
 *
 * Note that not all the context structure is always filled with actual values
 * but only the fields needed in a given context. */

struct RedisModuleBlockedClient;
struct RedisModuleUser;

struct RedisModuleCtx {
    void *getapifuncptr;            /* NOTE: Must be the first field. */
    struct RedisModule *module;     /* Module reference. */
    client *client;                 /* Client calling a command. */
    struct RedisModuleBlockedClient *blocked_client; /* Blocked client for
                                                        thread safe context. */
    struct AutoMemEntry *amqueue;   /* Auto memory queue of objects to free. */
    int amqueue_len;                /* Number of slots in amqueue. */
    int amqueue_used;               /* Number of used slots in amqueue. */
    int flags;                      /* REDISMODULE_CTX_... flags. */
    void **postponed_arrays;        /* To set with RM_ReplySetArrayLength(). */
    int postponed_arrays_count;     /* Number of entries in postponed_arrays. */
    void *blocked_privdata;         /* Privdata set when unblocking a client. */
    RedisModuleString *blocked_ready_key; /* Key ready when the reply callback
                                             gets called for clients blocked
                                             on keys. */

    /* Used if there is the REDISMODULE_CTX_KEYS_POS_REQUEST or 
     * REDISMODULE_CTX_CHANNEL_POS_REQUEST flag set. */
    getKeysResult *keys_result;

    struct RedisModulePoolAllocBlock *pa_head;
    long long next_yield_time;

    const struct RedisModuleUser *user;  /* RedisModuleUser commands executed via
                                            RM_Call should be executed as, if set */
};
typedef struct RedisModuleCtx RedisModuleCtx;

#define REDISMODULE_CTX_NONE (0)
#define REDISMODULE_CTX_AUTO_MEMORY (1<<0)
#define REDISMODULE_CTX_KEYS_POS_REQUEST (1<<1)
#define REDISMODULE_CTX_BLOCKED_REPLY (1<<2)
#define REDISMODULE_CTX_BLOCKED_TIMEOUT (1<<3)
#define REDISMODULE_CTX_THREAD_SAFE (1<<4)
#define REDISMODULE_CTX_BLOCKED_DISCONNECTED (1<<5)
#define REDISMODULE_CTX_TEMP_CLIENT (1<<6) /* Return client object to the pool
                                              when the context is destroyed */
#define REDISMODULE_CTX_NEW_CLIENT (1<<7)  /* Free client object when the
                                              context is destroyed */
#define REDISMODULE_CTX_CHANNELS_POS_REQUEST (1<<8)
#define REDISMODULE_CTX_COMMAND (1<<9) /* Context created to serve a command from call() or AOF (which calls cmd->proc directly) */


/* This represents a Redis key opened with RM_OpenKey(). */
struct RedisModuleKey {
    RedisModuleCtx *ctx;
    redisDb *db;
    robj *key;      /* Key name object. */
    robj *value;    /* Value object, or NULL if the key was not found. */
    void *iter;     /* Iterator. */
    int mode;       /* Opening mode. */

    union {
        struct {
            /* List, use only if value->type == OBJ_LIST */
            listTypeEntry entry;   /* Current entry in iteration. */
            long index;            /* Current 0-based index in iteration. */
        } list;
        struct {
            /* Zset iterator, use only if value->type == OBJ_ZSET */
            uint32_t type;         /* REDISMODULE_ZSET_RANGE_* */
            zrangespec rs;         /* Score range. */
            zlexrangespec lrs;     /* Lex range. */
            uint32_t start;        /* Start pos for positional ranges. */
            uint32_t end;          /* End pos for positional ranges. */
            void *current;         /* Zset iterator current node. */
            int er;                /* Zset iterator end reached flag
                                       (true if end was reached). */
        } zset;
        struct {
            /* Stream, use only if value->type == OBJ_STREAM */
            streamID currentid;    /* Current entry while iterating. */
            int64_t numfieldsleft; /* Fields left to fetch for current entry. */
            int signalready;       /* Flag that signalKeyAsReady() is needed. */
        } stream;
    } u;
};

/* RedisModuleKey 'ztype' values. */
#define REDISMODULE_ZSET_RANGE_NONE 0       /* This must always be 0. */
#define REDISMODULE_ZSET_RANGE_LEX 1
#define REDISMODULE_ZSET_RANGE_SCORE 2
#define REDISMODULE_ZSET_RANGE_POS 3

/* Function pointer type of a function representing a command inside
 * a Redis module. */
struct RedisModuleBlockedClient;
typedef int (*RedisModuleCmdFunc) (RedisModuleCtx *ctx, void **argv, int argc);
typedef int (*RedisModuleAuthCallback)(RedisModuleCtx *ctx, void *username, void *password, RedisModuleString **err);
typedef void (*RedisModuleDisconnectFunc) (RedisModuleCtx *ctx, struct RedisModuleBlockedClient *bc);

/* This struct holds the information about a command registered by a module.*/
struct RedisModuleCommand {
    struct RedisModule *module;
    RedisModuleCmdFunc func;
    struct redisCommand *rediscmd;
};
typedef struct RedisModuleCommand RedisModuleCommand;

#define REDISMODULE_REPLYFLAG_NONE 0
#define REDISMODULE_REPLYFLAG_TOPARSE (1<<0) /* Protocol must be parsed. */
#define REDISMODULE_REPLYFLAG_NESTED (1<<1)  /* Nested reply object. No proto
                                                or struct free. */

/* Reply of RM_Call() function. The function is filled in a lazy
 * way depending on the function called on the reply structure. By default
 * only the type, proto and protolen are filled. */
typedef struct CallReply RedisModuleCallReply;

/* Structure to hold the module auth callback & the Module implementing it. */
typedef struct RedisModuleAuthCtx {
    struct RedisModule *module;
    RedisModuleAuthCallback auth_cb;
} RedisModuleAuthCtx;

/* Structure representing a blocked client. We get a pointer to such
 * an object when blocking from modules. */
typedef struct RedisModuleBlockedClient {
    client *client;  /* Pointer to the blocked client. or NULL if the client
                        was destroyed during the life of this object. */
    RedisModule *module;    /* Module blocking the client. */
    RedisModuleCmdFunc reply_callback; /* Reply callback on normal completion.*/
    RedisModuleAuthCallback auth_reply_cb; /* Reply callback on completing blocking
                                                    module authentication. */
    RedisModuleCmdFunc timeout_callback; /* Reply callback on timeout. */
    RedisModuleDisconnectFunc disconnect_callback; /* Called on disconnection.*/
    void (*free_privdata)(RedisModuleCtx*,void*);/* privdata cleanup callback.*/
    void *privdata;     /* Module private data that may be used by the reply
                           or timeout callback. It is set via the
                           RedisModule_UnblockClient() API. */
    client *thread_safe_ctx_client; /* Fake client to be used for thread safe
                                       context so that no lock is required. */
    client *reply_client;           /* Fake client used to accumulate replies
                                       in thread safe contexts. */
    int dbid;           /* Database number selected by the original client. */
    int blocked_on_keys;    /* If blocked via RM_BlockClientOnKeys(). */
    int unblocked;          /* Already on the moduleUnblocked list. */
    monotime background_timer; /* Timer tracking the start of background work */
    uint64_t background_duration; /* Current command background time duration.
                                     Used for measuring latency of blocking cmds */
} RedisModuleBlockedClient;

/* This is a list of Module Auth Contexts. Each time a Module registers a callback, a new ctx is
 * added to this list. Multiple modules can register auth callbacks and the same Module can have
 * multiple auth callbacks. */
static list *moduleAuthCallbacks;

static pthread_mutex_t moduleUnblockedClientsMutex = PTHREAD_MUTEX_INITIALIZER;
static list *moduleUnblockedClients;

/* Pool for temporary client objects. Creating and destroying a client object is
 * costly. We manage a pool of clients to avoid this cost. Pool expands when
 * more clients are needed and shrinks when unused. Please see modulesCron()
 * for more details. */
static client **moduleTempClients;
static size_t moduleTempClientCap = 0;
static size_t moduleTempClientCount = 0;    /* Client count in pool */
static size_t moduleTempClientMinCount = 0; /* Min client count in pool since
                                               the last cron. */

/* We need a mutex that is unlocked / relocked in beforeSleep() in order to
 * allow thread safe contexts to execute commands at a safe moment. */
static pthread_mutex_t moduleGIL = PTHREAD_MUTEX_INITIALIZER;


/* Function pointer type for keyspace event notification subscriptions from modules. */
typedef int (*RedisModuleNotificationFunc) (RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key);

/* Function pointer type for post jobs */
typedef void (*RedisModulePostNotificationJobFunc) (RedisModuleCtx *ctx, void *pd);

/* Keyspace notification subscriber information.
 * See RM_SubscribeToKeyspaceEvents() for more information. */
typedef struct RedisModuleKeyspaceSubscriber {
    /* The module subscribed to the event */
    RedisModule *module;
    /* Notification callback in the module*/
    RedisModuleNotificationFunc notify_callback;
    /* A bit mask of the events the module is interested in */
    int event_mask;
    /* Active flag set on entry, to avoid reentrant subscribers
     * calling themselves */
    int active;
} RedisModuleKeyspaceSubscriber;

typedef struct RedisModulePostExecUnitJob {
    /* The module subscribed to the event */
    RedisModule *module;
    RedisModulePostNotificationJobFunc callback;
    void *pd;
    void (*free_pd)(void*);
    int dbid;
} RedisModulePostExecUnitJob;

/* The module keyspace notification subscribers list */
static list *moduleKeyspaceSubscribers;

/* The module post keyspace jobs list */
static list *modulePostExecUnitJobs;

/* Data structures related to the exported dictionary data structure. */
typedef struct RedisModuleDict {
    rax *rax;                       /* The radix tree. */
} RedisModuleDict;

typedef struct RedisModuleDictIter {
    RedisModuleDict *dict;
    raxIterator ri;
} RedisModuleDictIter;

typedef struct RedisModuleCommandFilterCtx {
    RedisModuleString **argv;
    int argv_len;
    int argc;
} RedisModuleCommandFilterCtx;

typedef void (*RedisModuleCommandFilterFunc) (RedisModuleCommandFilterCtx *filter);

typedef struct RedisModuleCommandFilter {
    /* The module that registered the filter */
    RedisModule *module;
    /* Filter callback function */
    RedisModuleCommandFilterFunc callback;
    /* REDISMODULE_CMDFILTER_* flags */
    int flags;
} RedisModuleCommandFilter;

/* Registered filters */
static list *moduleCommandFilters;

typedef void (*RedisModuleForkDoneHandler) (int exitcode, int bysignal, void *user_data);

static struct RedisModuleForkInfo {
    RedisModuleForkDoneHandler done_handler;
    void* done_handler_user_data;
} moduleForkInfo = {0};

typedef struct RedisModuleServerInfoData {
    rax *rax;                       /* parsed info data. */
} RedisModuleServerInfoData;

/* Flags for moduleCreateArgvFromUserFormat(). */
#define REDISMODULE_ARGV_REPLICATE (1<<0)
#define REDISMODULE_ARGV_NO_AOF (1<<1)
#define REDISMODULE_ARGV_NO_REPLICAS (1<<2)
#define REDISMODULE_ARGV_RESP_3 (1<<3)
#define REDISMODULE_ARGV_RESP_AUTO (1<<4)
#define REDISMODULE_ARGV_RUN_AS_USER (1<<5)
#define REDISMODULE_ARGV_SCRIPT_MODE (1<<6)
#define REDISMODULE_ARGV_NO_WRITES (1<<7)
#define REDISMODULE_ARGV_CALL_REPLIES_AS_ERRORS (1<<8)
#define REDISMODULE_ARGV_RESPECT_DENY_OOM (1<<9)
#define REDISMODULE_ARGV_DRY_RUN (1<<10)
#define REDISMODULE_ARGV_ALLOW_BLOCK (1<<11)

/* Determine whether Redis should signalModifiedKey implicitly.
 * In case 'ctx' has no 'module' member (and therefore no module->options),
 * we assume default behavior, that is, Redis signals.
 * (see RM_GetThreadSafeContext) */
#define SHOULD_SIGNAL_MODIFIED_KEYS(ctx) \
    ((ctx)->module? !((ctx)->module->options & REDISMODULE_OPTION_NO_IMPLICIT_SIGNAL_MODIFIED) : 1)

/* Server events hooks data structures and defines: this modules API
 * allow modules to subscribe to certain events in Redis, such as
 * the start and end of an RDB or AOF save, the change of role in replication,
 * and similar other events. */

typedef struct RedisModuleEventListener {
    RedisModule *module;
    RedisModuleEvent event;
    RedisModuleEventCallback callback;
} RedisModuleEventListener;

list *RedisModule_EventListeners; /* Global list of all the active events. */

/* Data structures related to the redis module users */

/* This is the object returned by RM_CreateModuleUser(). The module API is
 * able to create users, set ACLs to such users, and later authenticate
 * clients using such newly created users. */
typedef struct RedisModuleUser {
    user *user; /* Reference to the real redis user */
    int free_user; /* Indicates that user should also be freed when this object is freed */
} RedisModuleUser;

/* This is a structure used to export some meta-information such as dbid to the module. */
typedef struct RedisModuleKeyOptCtx {
    struct redisObject *from_key, *to_key; /* Optional name of key processed, NULL when unknown. 
                                              In most cases, only 'from_key' is valid, but in callbacks 
                                              such as `copy2`, both 'from_key' and 'to_key' are valid. */
    int from_dbid, to_dbid;                /* The dbid of the key being processed, -1 when unknown.
                                              In most cases, only 'from_dbid' is valid, but in callbacks such 
                                              as `copy2`, 'from_dbid' and 'to_dbid' are both valid. */
} RedisModuleKeyOptCtx;

/* Data structures related to redis module configurations */
/* The function signatures for module config get callbacks. These are identical to the ones exposed in redismodule.h. */
typedef RedisModuleString * (*RedisModuleConfigGetStringFunc)(const char *name, void *privdata);
typedef long long (*RedisModuleConfigGetNumericFunc)(const char *name, void *privdata);
typedef int (*RedisModuleConfigGetBoolFunc)(const char *name, void *privdata);
typedef int (*RedisModuleConfigGetEnumFunc)(const char *name, void *privdata);
/* The function signatures for module config set callbacks. These are identical to the ones exposed in redismodule.h. */
typedef int (*RedisModuleConfigSetStringFunc)(const char *name, RedisModuleString *val, void *privdata, RedisModuleString **err);
typedef int (*RedisModuleConfigSetNumericFunc)(const char *name, long long val, void *privdata, RedisModuleString **err);
typedef int (*RedisModuleConfigSetBoolFunc)(const char *name, int val, void *privdata, RedisModuleString **err);
typedef int (*RedisModuleConfigSetEnumFunc)(const char *name, int val, void *privdata, RedisModuleString **err);
/* Apply signature, identical to redismodule.h */
typedef int (*RedisModuleConfigApplyFunc)(RedisModuleCtx *ctx, void *privdata, RedisModuleString **err);

/* Struct representing a module config. These are stored in a list in the module struct */
struct ModuleConfig {
    sds name; /* Name of config without the module name appended to the front */
    void *privdata; /* Optional data passed into the module config callbacks */
    union get_fn { /* The get callback specified by the module */
        RedisModuleConfigGetStringFunc get_string;
        RedisModuleConfigGetNumericFunc get_numeric;
        RedisModuleConfigGetBoolFunc get_bool;
        RedisModuleConfigGetEnumFunc get_enum;
    } get_fn;
    union set_fn { /* The set callback specified by the module */
        RedisModuleConfigSetStringFunc set_string;
        RedisModuleConfigSetNumericFunc set_numeric;
        RedisModuleConfigSetBoolFunc set_bool;
        RedisModuleConfigSetEnumFunc set_enum;
    } set_fn;
    RedisModuleConfigApplyFunc apply_fn;
    RedisModule *module;
};

typedef struct RedisModuleAsyncRMCallPromise{
    size_t ref_count;
    void *private_data;
    RedisModule *module;
    RedisModuleOnUnblocked on_unblocked;
    client *c;
    RedisModuleCtx *ctx;
} RedisModuleAsyncRMCallPromise;

/* --------------------------------------------------------------------------
 * Prototypes
 * -------------------------------------------------------------------------- */

void RM_FreeCallReply(RedisModuleCallReply *reply);
void RM_CloseKey(RedisModuleKey *key);
void autoMemoryCollect(RedisModuleCtx *ctx);
robj **moduleCreateArgvFromUserFormat(const char *cmdname, const char *fmt, int *argcp, int *flags, va_list ap);
void RM_ZsetRangeStop(RedisModuleKey *kp);
static void zsetKeyReset(RedisModuleKey *key);
static void moduleInitKeyTypeSpecific(RedisModuleKey *key);
void RM_FreeDict(RedisModuleCtx *ctx, RedisModuleDict *d);
void RM_FreeServerInfo(RedisModuleCtx *ctx, RedisModuleServerInfoData *data);

/* Helpers for RM_SetCommandInfo. */
static int moduleValidateCommandInfo(const RedisModuleCommandInfo *info);
static int64_t moduleConvertKeySpecsFlags(int64_t flags, int from_api);
static int moduleValidateCommandArgs(RedisModuleCommandArg *args,
                                     const RedisModuleCommandInfoVersion *version);
static struct redisCommandArg *moduleCopyCommandArgs(RedisModuleCommandArg *args,
                                                     const RedisModuleCommandInfoVersion *version);
static redisCommandArgType moduleConvertArgType(RedisModuleCommandArgType type, int *error);
static int moduleConvertArgFlags(int flags);
void moduleCreateContext(RedisModuleCtx *out_ctx, RedisModule *module, int ctx_flags);
/* --------------------------------------------------------------------------
 * ## Heap allocation raw functions
 *
 * Memory allocated with these functions are taken into account by Redis key
 * eviction algorithms and are reported in Redis memory usage information.
 * -------------------------------------------------------------------------- */

/* Use like malloc(). Memory allocated with this function is reported in
 * Redis INFO memory, used for keys eviction according to maxmemory settings
 * and in general is taken into account as memory allocated by Redis.
 * You should avoid using malloc().
 * This function panics if unable to allocate enough memory. */
void *RM_Alloc(size_t bytes) {
    return zmalloc(bytes);
}

/* Similar to RM_Alloc, but returns NULL in case of allocation failure, instead
 * of panicking. */
void *RM_TryAlloc(size_t bytes) {
    return ztrymalloc(bytes);
}

/* Use like calloc(). Memory allocated with this function is reported in
 * Redis INFO memory, used for keys eviction according to maxmemory settings
 * and in general is taken into account as memory allocated by Redis.
 * You should avoid using calloc() directly. */
void *RM_Calloc(size_t nmemb, size_t size) {
    return zcalloc(nmemb*size);
}

/* Use like realloc() for memory obtained with RedisModule_Alloc(). */
void* RM_Realloc(void *ptr, size_t bytes) {
    return zrealloc(ptr,bytes);
}

/* Use like free() for memory obtained by RedisModule_Alloc() and
 * RedisModule_Realloc(). However you should never try to free with
 * RedisModule_Free() memory allocated with malloc() inside your module. */
void RM_Free(void *ptr) {
    zfree(ptr);
}

/* Like strdup() but returns memory allocated with RedisModule_Alloc(). */
char *RM_Strdup(const char *str) {
    return zstrdup(str);
}

/* --------------------------------------------------------------------------
 * Pool allocator
 * -------------------------------------------------------------------------- */

/* Release the chain of blocks used for pool allocations. */
void poolAllocRelease(RedisModuleCtx *ctx) {
    RedisModulePoolAllocBlock *head = ctx->pa_head, *next;

    while(head != NULL) {
        next = head->next;
        zfree(head);
        head = next;
    }
    ctx->pa_head = NULL;
}

/* Return heap allocated memory that will be freed automatically when the
 * module callback function returns. Mostly suitable for small allocations
 * that are short living and must be released when the callback returns
 * anyway. The returned memory is aligned to the architecture word size
 * if at least word size bytes are requested, otherwise it is just
 * aligned to the next power of two, so for example a 3 bytes request is
 * 4 bytes aligned while a 2 bytes request is 2 bytes aligned.
 *
 * There is no realloc style function since when this is needed to use the
 * pool allocator is not a good idea.
 *
 * The function returns NULL if `bytes` is 0. */
void *RM_PoolAlloc(RedisModuleCtx *ctx, size_t bytes) {
    if (bytes == 0) return NULL;
    RedisModulePoolAllocBlock *b = ctx->pa_head;
    size_t left = b ? b->size - b->used : 0;

    /* Fix alignment. */
    if (left >= bytes) {
        size_t alignment = REDISMODULE_POOL_ALLOC_ALIGN;
        while (bytes < alignment && alignment/2 >= bytes) alignment /= 2;
        if (b->used % alignment)
            b->used += alignment - (b->used % alignment);
        left = (b->used > b->size) ? 0 : b->size - b->used;
    }

    /* Create a new block if needed. */
    if (left < bytes) {
        size_t blocksize = REDISMODULE_POOL_ALLOC_MIN_SIZE;
        if (blocksize < bytes) blocksize = bytes;
        b = zmalloc(sizeof(*b) + blocksize);
        b->size = blocksize;
        b->used = 0;
        b->next = ctx->pa_head;
        ctx->pa_head = b;
    }

    char *retval = b->memory + b->used;
    b->used += bytes;
    return retval;
}

/* --------------------------------------------------------------------------
 * Helpers for modules API implementation
 * -------------------------------------------------------------------------- */

client *moduleAllocTempClient(user *user) {
    client *c = NULL;

    if (moduleTempClientCount > 0) {
        c = moduleTempClients[--moduleTempClientCount];
        if (moduleTempClientCount < moduleTempClientMinCount)
            moduleTempClientMinCount = moduleTempClientCount;
    } else {
        c = createClient(NULL);
        c->flags |= CLIENT_MODULE;
    }

    c->user = user;

    return c;
}

static void freeRedisModuleAsyncRMCallPromise(RedisModuleAsyncRMCallPromise *promise) {
    if (--promise->ref_count > 0) {
        return;
    }
    /* When the promise is finally freed it can not have a client attached to it.
     * Either releasing the client or RM_CallReplyPromiseAbort would have removed it. */
    serverAssert(!promise->c);
    zfree(promise);
}

void moduleReleaseTempClient(client *c) {
    if (moduleTempClientCount == moduleTempClientCap) {
        moduleTempClientCap = moduleTempClientCap ? moduleTempClientCap*2 : 32;
        moduleTempClients = zrealloc(moduleTempClients, sizeof(c)*moduleTempClientCap);
    }
    clearClientConnectionState(c);
    listEmpty(c->reply);
    c->reply_bytes = 0;
    resetClient(c);
    c->bufpos = 0;
    c->flags = CLIENT_MODULE;
    c->user = NULL; /* Root user */
    c->cmd = c->lastcmd = c->realcmd = NULL;
    if (c->bstate.async_rm_call_handle) {
        RedisModuleAsyncRMCallPromise *promise = c->bstate.async_rm_call_handle;
        promise->c = NULL; /* Remove the client from the promise so it will no longer be possible to abort it. */
        freeRedisModuleAsyncRMCallPromise(promise);
        c->bstate.async_rm_call_handle = NULL;
    }
    moduleTempClients[moduleTempClientCount++] = c;
}

/* Create an empty key of the specified type. `key` must point to a key object
 * opened for writing where the `.value` member is set to NULL because the
 * key was found to be non existing.
 *
 * On success REDISMODULE_OK is returned and the key is populated with
 * the value of the specified type. The function fails and returns
 * REDISMODULE_ERR if:
 *
 * 1. The key is not open for writing.
 * 2. The key is not empty.
 * 3. The specified type is unknown.
 */
int moduleCreateEmptyKey(RedisModuleKey *key, int type) {
    robj *obj;

    /* The key must be open for writing and non existing to proceed. */
    if (!(key->mode & REDISMODULE_WRITE) || key->value)
        return REDISMODULE_ERR;

    switch(type) {
    case REDISMODULE_KEYTYPE_LIST:
        obj = createListListpackObject();
        break;
    case REDISMODULE_KEYTYPE_ZSET:
        obj = createZsetListpackObject();
        break;
    case REDISMODULE_KEYTYPE_HASH:
        obj = createHashObject();
        break;
    case REDISMODULE_KEYTYPE_STREAM:
        obj = createStreamObject();
        break;
    default: return REDISMODULE_ERR;
    }
    dbAdd(key->db,key->key,obj);
    key->value = obj;
    moduleInitKeyTypeSpecific(key);
    return REDISMODULE_OK;
}

/* Frees key->iter and sets it to NULL. */
static void moduleFreeKeyIterator(RedisModuleKey *key) {
    serverAssert(key->iter != NULL);
    switch (key->value->type) {
    case OBJ_LIST: listTypeReleaseIterator(key->iter); break;
    case OBJ_STREAM:
        streamIteratorStop(key->iter);
        zfree(key->iter);
        break;
    default: serverAssert(0); /* No key->iter for other types. */
    }
    key->iter = NULL;
}

/* Callback for listTypeTryConversion().
 * Frees list iterator and sets it to NULL. */
static void moduleFreeListIterator(void *data) {
    RedisModuleKey *key = (RedisModuleKey*)data;
    serverAssert(key->value->type == OBJ_LIST);
    if (key->iter) moduleFreeKeyIterator(key);
}

/* This function is called in low-level API implementation functions in order
 * to check if the value associated with the key remained empty after an
 * operation that removed elements from an aggregate data type.
 *
 * If this happens, the key is deleted from the DB and the key object state
 * is set to the right one in order to be targeted again by write operations
 * possibly recreating the key if needed.
 *
 * The function returns 1 if the key value object is found empty and is
 * deleted, otherwise 0 is returned. */
int moduleDelKeyIfEmpty(RedisModuleKey *key) {
    if (!(key->mode & REDISMODULE_WRITE) || key->value == NULL) return 0;
    int isempty;
    robj *o = key->value;

    switch(o->type) {
    case OBJ_LIST: isempty = listTypeLength(o) == 0; break;
    case OBJ_SET: isempty = setTypeSize(o) == 0; break;
    case OBJ_ZSET: isempty = zsetLength(o) == 0; break;
    case OBJ_HASH: isempty = hashTypeLength(o) == 0; break;
    case OBJ_STREAM: isempty = streamLength(o) == 0; break;
    default: isempty = 0;
    }

    if (isempty) {
        if (key->iter) moduleFreeKeyIterator(key);
        dbDelete(key->db,key->key);
        key->value = NULL;
        return 1;
    } else {
        return 0;
    }
}

/* --------------------------------------------------------------------------
 * Service API exported to modules
 *
 * Note that all the exported APIs are called RM_<funcname> in the core
 * and RedisModule_<funcname> in the module side (defined as function
 * pointers in redismodule.h). In this way the dynamic linker does not
 * mess with our global function pointers, overriding it with the symbols
 * defined in the main executable having the same names.
 * -------------------------------------------------------------------------- */

int RM_GetApi(const char *funcname, void **targetPtrPtr) {
    /* Lookup the requested module API and store the function pointer into the
     * target pointer. The function returns REDISMODULE_ERR if there is no such
     * named API, otherwise REDISMODULE_OK.
     *
     * This function is not meant to be used by modules developer, it is only
     * used implicitly by including redismodule.h. */
    dictEntry *he = dictFind(server.moduleapi, funcname);
    if (!he) return REDISMODULE_ERR;
    *targetPtrPtr = dictGetVal(he);
    return REDISMODULE_OK;
}

void modulePostExecutionUnitOperations() {
    if (server.execution_nesting)
        return;

    if (server.busy_module_yield_flags) {
        blockingOperationEnds();
        server.busy_module_yield_flags = BUSY_MODULE_YIELD_NONE;
        if (server.current_client)
            unprotectClient(server.current_client);
        unblockPostponedClients();
    }
}

/* Free the context after the user function was called. */
void moduleFreeContext(RedisModuleCtx *ctx) {
    /* See comment in moduleCreateContext */
    if (!(ctx->flags & (REDISMODULE_CTX_THREAD_SAFE|REDISMODULE_CTX_COMMAND))) {
        exitExecutionUnit();
        postExecutionUnitOperations();
    }
    autoMemoryCollect(ctx);
    poolAllocRelease(ctx);
    if (ctx->postponed_arrays) {
        zfree(ctx->postponed_arrays);
        ctx->postponed_arrays_count = 0;
        serverLog(LL_WARNING,
            "API misuse detected in module %s: "
            "RedisModule_ReplyWith*(REDISMODULE_POSTPONED_LEN) "
            "not matched by the same number of RedisModule_SetReply*Len() "
            "calls.",
            ctx->module->name);
    }
    /* If this context has a temp client, we return it back to the pool.
     * If this context created a new client (e.g detached context), we free it.
     * If the client is assigned manually, e.g ctx->client = someClientInstance,
     * none of these flags will be set and we do not attempt to free it. */
    if (ctx->flags & REDISMODULE_CTX_TEMP_CLIENT)
        moduleReleaseTempClient(ctx->client);
    else if (ctx->flags & REDISMODULE_CTX_NEW_CLIENT)
        freeClient(ctx->client);
}

static CallReply *moduleParseReply(client *c, RedisModuleCtx *ctx) {
    /* Convert the result of the Redis command into a module reply. */
    sds proto = sdsnewlen(c->buf,c->bufpos);
    c->bufpos = 0;
    while(listLength(c->reply)) {
        clientReplyBlock *o = listNodeValue(listFirst(c->reply));

        proto = sdscatlen(proto,o->buf,o->used);
        listDelNode(c->reply,listFirst(c->reply));
    }
    CallReply *reply = callReplyCreate(proto, c->deferred_reply_errors, ctx);
    c->deferred_reply_errors = NULL; /* now the responsibility of the reply object. */
    return reply;
}

void moduleCallCommandUnblockedHandler(client *c) {
    RedisModuleCtx ctx;
    RedisModuleAsyncRMCallPromise *promise = c->bstate.async_rm_call_handle;
    serverAssert(promise);
    RedisModule *module = promise->module;
    if (!promise->on_unblocked) {
        moduleReleaseTempClient(c);
        return; /* module did not set any unblock callback. */
    }
    moduleCreateContext(&ctx, module, REDISMODULE_CTX_TEMP_CLIENT);
    selectDb(ctx.client, c->db->id);

    CallReply *reply = moduleParseReply(c, &ctx);
    module->in_call++;
    promise->on_unblocked(&ctx, reply, promise->private_data);
    module->in_call--;

    moduleFreeContext(&ctx);
    moduleReleaseTempClient(c);
}

/* Create a module ctx and keep track of the nesting level.
 *
 * Note: When creating ctx for threads (RM_GetThreadSafeContext and
 * RM_GetDetachedThreadSafeContext) we do not bump up the nesting level
 * because we only need to track of nesting level in the main thread
 * (only the main thread uses propagatePendingCommands) */
void moduleCreateContext(RedisModuleCtx *out_ctx, RedisModule *module, int ctx_flags) {
    memset(out_ctx, 0 ,sizeof(RedisModuleCtx));
    out_ctx->getapifuncptr = (void*)(unsigned long)&RM_GetApi;
    out_ctx->module = module;
    out_ctx->flags = ctx_flags;
    if (ctx_flags & REDISMODULE_CTX_TEMP_CLIENT)
        out_ctx->client = moduleAllocTempClient(NULL);
    else if (ctx_flags & REDISMODULE_CTX_NEW_CLIENT)
        out_ctx->client = createClient(NULL);

    /* Calculate the initial yield time for long blocked contexts.
     * in loading we depend on the server hz, but in other cases we also wait
     * for busy_reply_threshold.
     * Note that in theory we could have started processing BUSY_MODULE_YIELD_EVENTS
     * sooner, and only delay the processing for clients till the busy_reply_threshold,
     * but this carries some overheads of frequently marking clients with BLOCKED_POSTPONE
     * and releasing them, i.e. if modules only block for short periods. */
    if (server.loading)
        out_ctx->next_yield_time = getMonotonicUs() + 1000000 / server.hz;
    else
        out_ctx->next_yield_time = getMonotonicUs() + server.busy_reply_threshold * 1000;

    /* Increment the execution_nesting counter (module is about to execute some code),
     * except in the following cases:
     * 1. We came here from cmd->proc (either call() or AOF load).
     *    In the former, the counter has been already incremented from within
     *    call() and in the latter we don't care about execution_nesting
     * 2. If we are running in a thread (execution_nesting will be dealt with
     *    when locking/unlocking the GIL) */
    if (!(ctx_flags & (REDISMODULE_CTX_THREAD_SAFE|REDISMODULE_CTX_COMMAND))) {
        enterExecutionUnit(1, 0);
    }
}

/* This Redis command binds the normal Redis command invocation with commands
 * exported by modules. */
void RedisModuleCommandDispatcher(client *c) {
    RedisModuleCommand *cp = c->cmd->module_cmd;
    RedisModuleCtx ctx;
    moduleCreateContext(&ctx, cp->module, REDISMODULE_CTX_COMMAND);

    ctx.client = c;
    cp->func(&ctx,(void**)c->argv,c->argc);
    moduleFreeContext(&ctx);

    /* In some cases processMultibulkBuffer uses sdsMakeRoomFor to
     * expand the query buffer, and in order to avoid a big object copy
     * the query buffer SDS may be used directly as the SDS string backing
     * the client argument vectors: sometimes this will result in the SDS
     * string having unused space at the end. Later if a module takes ownership
     * of the RedisString, such space will be wasted forever. Inside the
     * Redis core this is not a problem because tryObjectEncoding() is called
     * before storing strings in the key space. Here we need to do it
     * for the module. */
    for (int i = 0; i < c->argc; i++) {
        /* Only do the work if the module took ownership of the object:
         * in that case the refcount is no longer 1. */
        if (c->argv[i]->refcount > 1)
            trimStringObjectIfNeeded(c->argv[i], 0);
    }
}

/* This function returns the list of keys, with the same interface as the
 * 'getkeys' function of the native commands, for module commands that exported
 * the "getkeys-api" flag during the registration. This is done when the
 * list of keys are not at fixed positions, so that first/last/step cannot
 * be used.
 *
 * In order to accomplish its work, the module command is called, flagging
 * the context in a way that the command can recognize this is a special
 * "get keys" call by calling RedisModule_IsKeysPositionRequest(ctx). */
int moduleGetCommandKeysViaAPI(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    RedisModuleCommand *cp = cmd->module_cmd;
    RedisModuleCtx ctx;
    moduleCreateContext(&ctx, cp->module, REDISMODULE_CTX_KEYS_POS_REQUEST);

    /* Initialize getKeysResult */
    getKeysPrepareResult(result, MAX_KEYS_BUFFER);
    ctx.keys_result = result;

    cp->func(&ctx,(void**)argv,argc);
    /* We currently always use the array allocated by RM_KeyAtPos() and don't try
     * to optimize for the pre-allocated buffer.
     */
    moduleFreeContext(&ctx);
    return result->numkeys;
}

/* This function returns the list of channels, with the same interface as
 * moduleGetCommandKeysViaAPI, for modules that declare "getchannels-api"
 * during registration. Unlike keys, this is the only way to declare channels. */
int moduleGetCommandChannelsViaAPI(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    RedisModuleCommand *cp = cmd->module_cmd;
    RedisModuleCtx ctx;
    moduleCreateContext(&ctx, cp->module, REDISMODULE_CTX_CHANNELS_POS_REQUEST);

    /* Initialize getKeysResult */
    getKeysPrepareResult(result, MAX_KEYS_BUFFER);
    ctx.keys_result = result;

    cp->func(&ctx,(void**)argv,argc);
    /* We currently always use the array allocated by RM_RM_ChannelAtPosWithFlags() and don't try
     * to optimize for the pre-allocated buffer. */
    moduleFreeContext(&ctx);
    return result->numkeys;
}

/* --------------------------------------------------------------------------
 * ## Commands API
 *
 * These functions are used to implement custom Redis commands.
 *
 * For examples, see https://redis.io/topics/modules-intro.
 * -------------------------------------------------------------------------- */

/* Return non-zero if a module command, that was declared with the
 * flag "getkeys-api", is called in a special way to get the keys positions
 * and not to get executed. Otherwise zero is returned. */
int RM_IsKeysPositionRequest(RedisModuleCtx *ctx) {
    return (ctx->flags & REDISMODULE_CTX_KEYS_POS_REQUEST) != 0;
}

/* When a module command is called in order to obtain the position of
 * keys, since it was flagged as "getkeys-api" during the registration,
 * the command implementation checks for this special call using the
 * RedisModule_IsKeysPositionRequest() API and uses this function in
 * order to report keys.
 *
 * The supported flags are the ones used by RM_SetCommandInfo, see REDISMODULE_CMD_KEY_*.
 *
 *
 * The following is an example of how it could be used:
 *
 *     if (RedisModule_IsKeysPositionRequest(ctx)) {
 *         RedisModule_KeyAtPosWithFlags(ctx, 2, REDISMODULE_CMD_KEY_RO | REDISMODULE_CMD_KEY_ACCESS);
 *         RedisModule_KeyAtPosWithFlags(ctx, 1, REDISMODULE_CMD_KEY_RW | REDISMODULE_CMD_KEY_UPDATE | REDISMODULE_CMD_KEY_ACCESS);
 *     }
 *
 *  Note: in the example above the get keys API could have been handled by key-specs (preferred).
 *  Implementing the getkeys-api is required only when is it not possible to declare key-specs that cover all keys.
 *
 */
void RM_KeyAtPosWithFlags(RedisModuleCtx *ctx, int pos, int flags) {
    if (!(ctx->flags & REDISMODULE_CTX_KEYS_POS_REQUEST) || !ctx->keys_result) return;
    if (pos <= 0) return;

    getKeysResult *res = ctx->keys_result;

    /* Check overflow */
    if (res->numkeys == res->size) {
        int newsize = res->size + (res->size > 8192 ? 8192 : res->size);
        getKeysPrepareResult(res, newsize);
    }

    res->keys[res->numkeys].pos = pos;
    res->keys[res->numkeys].flags = moduleConvertKeySpecsFlags(flags, 1);
    res->numkeys++;
}

/* This API existed before RM_KeyAtPosWithFlags was added, now deprecated and
 * can be used for compatibility with older versions, before key-specs and flags
 * were introduced. */
void RM_KeyAtPos(RedisModuleCtx *ctx, int pos) {
    /* Default flags require full access */
    int flags = moduleConvertKeySpecsFlags(CMD_KEY_FULL_ACCESS, 0);
    RM_KeyAtPosWithFlags(ctx, pos, flags);
}

/* Return non-zero if a module command, that was declared with the
 * flag "getchannels-api", is called in a special way to get the channel positions
 * and not to get executed. Otherwise zero is returned. */
int RM_IsChannelsPositionRequest(RedisModuleCtx *ctx) {
    return (ctx->flags & REDISMODULE_CTX_CHANNELS_POS_REQUEST) != 0;
}

/* When a module command is called in order to obtain the position of
 * channels, since it was flagged as "getchannels-api" during the
 * registration, the command implementation checks for this special call
 * using the RedisModule_IsChannelsPositionRequest() API and uses this
 * function in order to report the channels.
 * 
 * The supported flags are:
 * * REDISMODULE_CMD_CHANNEL_SUBSCRIBE: This command will subscribe to the channel.
 * * REDISMODULE_CMD_CHANNEL_UNSUBSCRIBE: This command will unsubscribe from this channel.
 * * REDISMODULE_CMD_CHANNEL_PUBLISH: This command will publish to this channel.
 * * REDISMODULE_CMD_CHANNEL_PATTERN: Instead of acting on a specific channel, will act on any 
 *                                    channel specified by the pattern. This is the same access
 *                                    used by the PSUBSCRIBE and PUNSUBSCRIBE commands available 
 *                                    in Redis. Not intended to be used with PUBLISH permissions.
 *
 * The following is an example of how it could be used:
 *
 *     if (RedisModule_IsChannelsPositionRequest(ctx)) {
 *         RedisModule_ChannelAtPosWithFlags(ctx, 1, REDISMODULE_CMD_CHANNEL_SUBSCRIBE | REDISMODULE_CMD_CHANNEL_PATTERN);
 *         RedisModule_ChannelAtPosWithFlags(ctx, 1, REDISMODULE_CMD_CHANNEL_PUBLISH);
 *     }
 *
 * Note: One usage of declaring channels is for evaluating ACL permissions. In this context,
 * unsubscribing is always allowed, so commands will only be checked against subscribe and
 * publish permissions. This is preferred over using RM_ACLCheckChannelPermissions, since
 * it allows the ACLs to be checked before the command is executed. */
void RM_ChannelAtPosWithFlags(RedisModuleCtx *ctx, int pos, int flags) {
    if (!(ctx->flags & REDISMODULE_CTX_CHANNELS_POS_REQUEST) || !ctx->keys_result) return;
    if (pos <= 0) return;

    getKeysResult *res = ctx->keys_result;

    /* Check overflow */
    if (res->numkeys == res->size) {
        int newsize = res->size + (res->size > 8192 ? 8192 : res->size);
        getKeysPrepareResult(res, newsize);
    }

    int new_flags = 0;
    if (flags & REDISMODULE_CMD_CHANNEL_SUBSCRIBE) new_flags |= CMD_CHANNEL_SUBSCRIBE;
    if (flags & REDISMODULE_CMD_CHANNEL_UNSUBSCRIBE) new_flags |= CMD_CHANNEL_UNSUBSCRIBE;
    if (flags & REDISMODULE_CMD_CHANNEL_PUBLISH) new_flags |= CMD_CHANNEL_PUBLISH;
    if (flags & REDISMODULE_CMD_CHANNEL_PATTERN) new_flags |= CMD_CHANNEL_PATTERN;

    res->keys[res->numkeys].pos = pos;
    res->keys[res->numkeys].flags = new_flags;
    res->numkeys++;
}

/* Returns 1 if name is valid, otherwise returns 0.
 *
 * We want to block some chars in module command names that we know can
 * mess things up.
 *
 * There are these characters:
 * ' ' (space) - issues with old inline protocol.
 * '\r', '\n' (newline) - can mess up the protocol on acl error replies.
 * '|' - sub-commands.
 * '@' - ACL categories.
 * '=', ',' - info and client list fields (':' handled by getSafeInfoString).
 * */
int isCommandNameValid(const char *name) {
    const char *block_chars = " \r\n|@=,";

    if (strpbrk(name, block_chars))
        return 0;
    return 1;
}

/* Helper for RM_CreateCommand(). Turns a string representing command
 * flags into the command flags used by the Redis core.
 *
 * It returns the set of flags, or -1 if unknown flags are found. */
int64_t commandFlagsFromString(char *s) {
    int count, j;
    int64_t flags = 0;
    sds *tokens = sdssplitlen(s,strlen(s)," ",1,&count);
    for (j = 0; j < count; j++) {
        char *t = tokens[j];
        if (!strcasecmp(t,"write")) flags |= CMD_WRITE;
        else if (!strcasecmp(t,"readonly")) flags |= CMD_READONLY;
        else if (!strcasecmp(t,"admin")) flags |= CMD_ADMIN;
        else if (!strcasecmp(t,"deny-oom")) flags |= CMD_DENYOOM;
        else if (!strcasecmp(t,"deny-script")) flags |= CMD_NOSCRIPT;
        else if (!strcasecmp(t,"allow-loading")) flags |= CMD_LOADING;
        else if (!strcasecmp(t,"pubsub")) flags |= CMD_PUBSUB;
        else if (!strcasecmp(t,"random")) { /* Deprecated. Silently ignore. */ }
        else if (!strcasecmp(t,"blocking")) flags |= CMD_BLOCKING;
        else if (!strcasecmp(t,"allow-stale")) flags |= CMD_STALE;
        else if (!strcasecmp(t,"no-monitor")) flags |= CMD_SKIP_MONITOR;
        else if (!strcasecmp(t,"no-slowlog")) flags |= CMD_SKIP_SLOWLOG;
        else if (!strcasecmp(t,"fast")) flags |= CMD_FAST;
        else if (!strcasecmp(t,"no-auth")) flags |= CMD_NO_AUTH;
        else if (!strcasecmp(t,"may-replicate")) flags |= CMD_MAY_REPLICATE;
        else if (!strcasecmp(t,"getkeys-api")) flags |= CMD_MODULE_GETKEYS;
        else if (!strcasecmp(t,"getchannels-api")) flags |= CMD_MODULE_GETCHANNELS;
        else if (!strcasecmp(t,"no-cluster")) flags |= CMD_MODULE_NO_CLUSTER;
        else if (!strcasecmp(t,"no-mandatory-keys")) flags |= CMD_NO_MANDATORY_KEYS;
        else if (!strcasecmp(t,"allow-busy")) flags |= CMD_ALLOW_BUSY;
        else break;
    }
    sdsfreesplitres(tokens,count);
    if (j != count) return -1; /* Some token not processed correctly. */
    return flags;
}

RedisModuleCommand *moduleCreateCommandProxy(struct RedisModule *module, sds declared_name, sds fullname, RedisModuleCmdFunc cmdfunc, int64_t flags, int firstkey, int lastkey, int keystep);

/* Register a new command in the Redis server, that will be handled by
 * calling the function pointer 'cmdfunc' using the RedisModule calling
 * convention.
 *
 * The function returns REDISMODULE_ERR in these cases:
 * - If creation of module command is called outside the RedisModule_OnLoad.
 * - The specified command is already busy.
 * - The command name contains some chars that are not allowed.
 * - A set of invalid flags were passed.
 *
 * Otherwise REDISMODULE_OK is returned and the new command is registered.
 *
 * This function must be called during the initialization of the module
 * inside the RedisModule_OnLoad() function. Calling this function outside
 * of the initialization function is not defined.
 *
 * The command function type is the following:
 *
 *      int MyCommand_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
 *
 * And is supposed to always return REDISMODULE_OK.
 *
 * The set of flags 'strflags' specify the behavior of the command, and should
 * be passed as a C string composed of space separated words, like for
 * example "write deny-oom". The set of flags are:
 *
 * * **"write"**:     The command may modify the data set (it may also read
 *                    from it).
 * * **"readonly"**:  The command returns data from keys but never writes.
 * * **"admin"**:     The command is an administrative command (may change
 *                    replication or perform similar tasks).
 * * **"deny-oom"**:  The command may use additional memory and should be
 *                    denied during out of memory conditions.
 * * **"deny-script"**:   Don't allow this command in Lua scripts.
 * * **"allow-loading"**: Allow this command while the server is loading data.
 *                        Only commands not interacting with the data set
 *                        should be allowed to run in this mode. If not sure
 *                        don't use this flag.
 * * **"pubsub"**:    The command publishes things on Pub/Sub channels.
 * * **"random"**:    The command may have different outputs even starting
 *                    from the same input arguments and key values.
 *                    Starting from Redis 7.0 this flag has been deprecated.
 *                    Declaring a command as "random" can be done using
 *                    command tips, see https://redis.io/topics/command-tips.
 * * **"allow-stale"**: The command is allowed to run on slaves that don't
 *                      serve stale data. Don't use if you don't know what
 *                      this means.
 * * **"no-monitor"**: Don't propagate the command on monitor. Use this if
 *                     the command has sensitive data among the arguments.
 * * **"no-slowlog"**: Don't log this command in the slowlog. Use this if
 *                     the command has sensitive data among the arguments.
 * * **"fast"**:      The command time complexity is not greater
 *                    than O(log(N)) where N is the size of the collection or
 *                    anything else representing the normal scalability
 *                    issue with the command.
 * * **"getkeys-api"**: The command implements the interface to return
 *                      the arguments that are keys. Used when start/stop/step
 *                      is not enough because of the command syntax.
 * * **"no-cluster"**: The command should not register in Redis Cluster
 *                     since is not designed to work with it because, for
 *                     example, is unable to report the position of the
 *                     keys, programmatically creates key names, or any
 *                     other reason.
 * * **"no-auth"**:    This command can be run by an un-authenticated client.
 *                     Normally this is used by a command that is used
 *                     to authenticate a client.
 * * **"may-replicate"**: This command may generate replication traffic, even
 *                        though it's not a write command.
 * * **"no-mandatory-keys"**: All the keys this command may take are optional
 * * **"blocking"**: The command has the potential to block the client.
 * * **"allow-busy"**: Permit the command while the server is blocked either by
 *                     a script or by a slow module command, see
 *                     RM_Yield.
 * * **"getchannels-api"**: The command implements the interface to return
 *                          the arguments that are channels.
 *
 * The last three parameters specify which arguments of the new command are
 * Redis keys. See https://redis.io/commands/command for more information.
 *
 * * `firstkey`: One-based index of the first argument that's a key.
 *               Position 0 is always the command name itself.
 *               0 for commands with no keys.
 * * `lastkey`:  One-based index of the last argument that's a key.
 *               Negative numbers refer to counting backwards from the last
 *               argument (-1 means the last argument provided)
 *               0 for commands with no keys.
 * * `keystep`:  Step between first and last key indexes.
 *               0 for commands with no keys.
 *
 * This information is used by ACL, Cluster and the `COMMAND` command.
 *
 * NOTE: The scheme described above serves a limited purpose and can
 * only be used to find keys that exist at constant indices.
 * For non-trivial key arguments, you may pass 0,0,0 and use
 * RedisModule_SetCommandInfo to set key specs using a more advanced scheme and use
 * RedisModule_SetCommandACLCategories to set Redis ACL categories of the commands. */
int RM_CreateCommand(RedisModuleCtx *ctx, const char *name, RedisModuleCmdFunc cmdfunc, const char *strflags, int firstkey, int lastkey, int keystep) {
    if (!ctx->module->onload)
        return REDISMODULE_ERR;
    int64_t flags = strflags ? commandFlagsFromString((char*)strflags) : 0;
    if (flags == -1) return REDISMODULE_ERR;
    if ((flags & CMD_MODULE_NO_CLUSTER) && server.cluster_enabled)
        return REDISMODULE_ERR;

    /* Check if the command name is valid. */
    if (!isCommandNameValid(name))
        return REDISMODULE_ERR;

    /* Check if the command name is busy. */
    if (lookupCommandByCString(name) != NULL)
        return REDISMODULE_ERR;

    sds declared_name = sdsnew(name);
    RedisModuleCommand *cp = moduleCreateCommandProxy(ctx->module, declared_name, sdsdup(declared_name), cmdfunc, flags, firstkey, lastkey, keystep);
    cp->rediscmd->arity = cmdfunc ? -1 : -2; /* Default value, can be changed later via dedicated API */

    serverAssert(dictAdd(server.commands, sdsdup(declared_name), cp->rediscmd) == DICT_OK);
    serverAssert(dictAdd(server.orig_commands, sdsdup(declared_name), cp->rediscmd) == DICT_OK);
    cp->rediscmd->id = ACLGetCommandID(declared_name); /* ID used for ACL. */
    return REDISMODULE_OK;
}

/* A proxy that help create a module command / subcommand.
 *
 * 'declared_name': it contains the sub_name, which is just the fullname for non-subcommands.
 * 'fullname': sds string representing the command fullname.
 *
 * Function will take the ownership of both 'declared_name' and 'fullname' SDS.
 */
RedisModuleCommand *moduleCreateCommandProxy(struct RedisModule *module, sds declared_name, sds fullname, RedisModuleCmdFunc cmdfunc, int64_t flags, int firstkey, int lastkey, int keystep) {
    struct redisCommand *rediscmd;
    RedisModuleCommand *cp;

    /* Create a command "proxy", which is a structure that is referenced
     * in the command table, so that the generic command that works as
     * binding between modules and Redis, can know what function to call
     * and what the module is. */
    cp = zcalloc(sizeof(*cp));
    cp->module = module;
    cp->func = cmdfunc;
    cp->rediscmd = zcalloc(sizeof(*rediscmd));
    cp->rediscmd->declared_name = declared_name; /* SDS for module commands */
    cp->rediscmd->fullname = fullname;
    cp->rediscmd->group = COMMAND_GROUP_MODULE;
    cp->rediscmd->proc = RedisModuleCommandDispatcher;
    cp->rediscmd->flags = flags | CMD_MODULE;
    cp->rediscmd->module_cmd = cp;
    cp->rediscmd->key_specs_max = STATIC_KEY_SPECS_NUM;
    cp->rediscmd->key_specs = cp->rediscmd->key_specs_static;
    if (firstkey != 0) {
        cp->rediscmd->key_specs_num = 1;
        cp->rediscmd->key_specs[0].flags = CMD_KEY_FULL_ACCESS;
        if (flags & CMD_MODULE_GETKEYS)
            cp->rediscmd->key_specs[0].flags |= CMD_KEY_VARIABLE_FLAGS;
        cp->rediscmd->key_specs[0].begin_search_type = KSPEC_BS_INDEX;
        cp->rediscmd->key_specs[0].bs.index.pos = firstkey;
        cp->rediscmd->key_specs[0].find_keys_type = KSPEC_FK_RANGE;
        cp->rediscmd->key_specs[0].fk.range.lastkey = lastkey < 0 ? lastkey : (lastkey-firstkey);
        cp->rediscmd->key_specs[0].fk.range.keystep = keystep;
        cp->rediscmd->key_specs[0].fk.range.limit = 0;
    } else {
        cp->rediscmd->key_specs_num = 0;
    }
    populateCommandLegacyRangeSpec(cp->rediscmd);
    cp->rediscmd->microseconds = 0;
    cp->rediscmd->calls = 0;
    cp->rediscmd->rejected_calls = 0;
    cp->rediscmd->failed_calls = 0;
    return cp;
}

/* Get an opaque structure, representing a module command, by command name.
 * This structure is used in some of the command-related APIs.
 *
 * NULL is returned in case of the following errors:
 *
 * * Command not found
 * * The command is not a module command
 * * The command doesn't belong to the calling module
 */
RedisModuleCommand *RM_GetCommand(RedisModuleCtx *ctx, const char *name) {
    struct redisCommand *cmd = lookupCommandByCString(name);

    if (!cmd || !(cmd->flags & CMD_MODULE))
        return NULL;

    RedisModuleCommand *cp = cmd->module_cmd;
    if (cp->module != ctx->module)
        return NULL;

    return cp;
}

/* Very similar to RedisModule_CreateCommand except that it is used to create
 * a subcommand, associated with another, container, command.
 *
 * Example: If a module has a configuration command, MODULE.CONFIG, then
 * GET and SET should be individual subcommands, while MODULE.CONFIG is
 * a command, but should not be registered with a valid `funcptr`:
 *
 *      if (RedisModule_CreateCommand(ctx,"module.config",NULL,"",0,0,0) == REDISMODULE_ERR)
 *          return REDISMODULE_ERR;
 *
 *      RedisModuleCommand *parent = RedisModule_GetCommand(ctx,,"module.config");
 *
 *      if (RedisModule_CreateSubcommand(parent,"set",cmd_config_set,"",0,0,0) == REDISMODULE_ERR)
 *         return REDISMODULE_ERR;
 *
 *      if (RedisModule_CreateSubcommand(parent,"get",cmd_config_get,"",0,0,0) == REDISMODULE_ERR)
 *         return REDISMODULE_ERR;
 *
 * Returns REDISMODULE_OK on success and REDISMODULE_ERR in case of the following errors:
 *
 * * Error while parsing `strflags`
 * * Command is marked as `no-cluster` but cluster mode is enabled
 * * `parent` is already a subcommand (we do not allow more than one level of command nesting)
 * * `parent` is a command with an implementation (RedisModuleCmdFunc) (A parent command should be a pure container of subcommands)
 * * `parent` already has a subcommand called `name`
 * * Creating a subcommand is called outside of RedisModule_OnLoad.
 */
int RM_CreateSubcommand(RedisModuleCommand *parent, const char *name, RedisModuleCmdFunc cmdfunc, const char *strflags, int firstkey, int lastkey, int keystep) {
    if (!parent->module->onload)
        return REDISMODULE_ERR;
    int64_t flags = strflags ? commandFlagsFromString((char*)strflags) : 0;
    if (flags == -1) return REDISMODULE_ERR;
    if ((flags & CMD_MODULE_NO_CLUSTER) && server.cluster_enabled)
        return REDISMODULE_ERR;

    struct redisCommand *parent_cmd = parent->rediscmd;

    if (parent_cmd->parent)
        return REDISMODULE_ERR; /* We don't allow more than one level of subcommands */

    RedisModuleCommand *parent_cp = parent_cmd->module_cmd;
    if (parent_cp->func)
        return REDISMODULE_ERR; /* A parent command should be a pure container of subcommands */

    /* Check if the command name is valid. */
    if (!isCommandNameValid(name))
        return REDISMODULE_ERR;

    /* Check if the command name is busy within the parent command. */
    sds declared_name = sdsnew(name);
    if (parent_cmd->subcommands_dict && lookupSubcommand(parent_cmd, declared_name) != NULL) {
        sdsfree(declared_name);
        return REDISMODULE_ERR;
    }

    sds fullname = catSubCommandFullname(parent_cmd->fullname, name);
    RedisModuleCommand *cp = moduleCreateCommandProxy(parent->module, declared_name, fullname, cmdfunc, flags, firstkey, lastkey, keystep);
    cp->rediscmd->arity = -2;

    commandAddSubcommand(parent_cmd, cp->rediscmd, name);
    return REDISMODULE_OK;
}

/* Accessors of array elements of structs where the element size is stored
 * separately in the version struct. */
static RedisModuleCommandHistoryEntry *
moduleCmdHistoryEntryAt(const RedisModuleCommandInfoVersion *version,
                        RedisModuleCommandHistoryEntry *entries, int index) {
    off_t offset = index * version->sizeof_historyentry;
    return (RedisModuleCommandHistoryEntry *)((char *)(entries) + offset);
}
static RedisModuleCommandKeySpec *
moduleCmdKeySpecAt(const RedisModuleCommandInfoVersion *version,
                   RedisModuleCommandKeySpec *keyspecs, int index) {
    off_t offset = index * version->sizeof_keyspec;
    return (RedisModuleCommandKeySpec *)((char *)(keyspecs) + offset);
}
static RedisModuleCommandArg *
moduleCmdArgAt(const RedisModuleCommandInfoVersion *version,
               const RedisModuleCommandArg *args, int index) {
    off_t offset = index * version->sizeof_arg;
    return (RedisModuleCommandArg *)((char *)(args) + offset);
}

/* Helper for categoryFlagsFromString(). Attempts to find an acl flag representing the provided flag string
 * and adds that flag to acl_categories_flags if a match is found.
 *
 * Returns '1' if acl category flag is recognized or
 * returns '0' if not recognized  */
int matchAclCategoryFlag(char *flag, int64_t *acl_categories_flags) {
    uint64_t this_flag = ACLGetCommandCategoryFlagByName(flag);
    if (this_flag) {
        *acl_categories_flags |= (int64_t) this_flag;
        return 1;
    }
    return 0; /* Unrecognized */
}

/* Helper for RM_SetCommandACLCategories(). Turns a string representing acl category
 * flags into the acl category flags used by Redis ACL which allows users to access 
 * the module commands by acl categories.
 * 
 * It returns the set of acl flags, or -1 if unknown flags are found. */
int64_t categoryFlagsFromString(char *aclflags) {
    int count, j;
    int64_t acl_categories_flags = 0;
    sds *tokens = sdssplitlen(aclflags,strlen(aclflags)," ",1,&count);
    for (j = 0; j < count; j++) {
        char *t = tokens[j];
        if (!matchAclCategoryFlag(t, &acl_categories_flags)) {
            serverLog(LL_WARNING,"Unrecognized categories flag %s on module load", t);
            break;
        }
    }
    sdsfreesplitres(tokens,count);
    if (j != count) return -1; /* Some token not processed correctly. */
    return acl_categories_flags;
}

/* RedisModule_SetCommandACLCategories can be used to set ACL categories to module
 * commands and subcommands. The set of ACL categories should be passed as
 * a space separated C string 'aclflags'.
 * 
 * Example, the acl flags 'write slow' marks the command as part of the write and 
 * slow ACL categories.
 * 
 * On success REDISMODULE_OK is returned. On error REDISMODULE_ERR is returned.
 * 
 * This function can only be called during the RedisModule_OnLoad function. If called
 * outside of this function, an error is returned.
 */
int RM_SetCommandACLCategories(RedisModuleCommand *command, const char *aclflags) {
    if (!command || !command->module || !command->module->onload) return REDISMODULE_ERR;
    int64_t categories_flags = aclflags ? categoryFlagsFromString((char*)aclflags) : 0;
    if (categories_flags == -1) return REDISMODULE_ERR;
    struct redisCommand *rcmd = command->rediscmd;
    rcmd->acl_categories = categories_flags; /* ACL categories flags for module command */
    command->module->num_commands_with_acl_categories++;
    return REDISMODULE_OK;
}

/* Set additional command information.
 *
 * Affects the output of `COMMAND`, `COMMAND INFO` and `COMMAND DOCS`, Cluster,
 * ACL and is used to filter commands with the wrong number of arguments before
 * the call reaches the module code.
 *
 * This function can be called after creating a command using RM_CreateCommand
 * and fetching the command pointer using RM_GetCommand. The information can
 * only be set once for each command and has the following structure:
 *
 *     typedef struct RedisModuleCommandInfo {
 *         const RedisModuleCommandInfoVersion *version;
 *         const char *summary;
 *         const char *complexity;
 *         const char *since;
 *         RedisModuleCommandHistoryEntry *history;
 *         const char *tips;
 *         int arity;
 *         RedisModuleCommandKeySpec *key_specs;
 *         RedisModuleCommandArg *args;
 *     } RedisModuleCommandInfo;
 *
 * All fields except `version` are optional. Explanation of the fields:
 *
 * - `version`: This field enables compatibility with different Redis versions.
 *   Always set this field to REDISMODULE_COMMAND_INFO_VERSION.
 *
 * - `summary`: A short description of the command (optional).
 *
 * - `complexity`: Complexity description (optional).
 *
 * - `since`: The version where the command was introduced (optional).
 *   Note: The version specified should be the module's, not Redis version.
 *
 * - `history`: An array of RedisModuleCommandHistoryEntry (optional), which is
 *   a struct with the following fields:
 *
 *         const char *since;
 *         const char *changes;
 *
 *     `since` is a version string and `changes` is a string describing the
 *     changes. The array is terminated by a zeroed entry, i.e. an entry with
 *     both strings set to NULL.
 *
 * - `tips`: A string of space-separated tips regarding this command, meant for
 *   clients and proxies. See https://redis.io/topics/command-tips.
 *
 * - `arity`: Number of arguments, including the command name itself. A positive
 *   number specifies an exact number of arguments and a negative number
 *   specifies a minimum number of arguments, so use -N to say >= N. Redis
 *   validates a call before passing it to a module, so this can replace an
 *   arity check inside the module command implementation. A value of 0 (or an
 *   omitted arity field) is equivalent to -2 if the command has sub commands
 *   and -1 otherwise.
 *
 * - `key_specs`: An array of RedisModuleCommandKeySpec, terminated by an
 *   element memset to zero. This is a scheme that tries to describe the
 *   positions of key arguments better than the old RM_CreateCommand arguments
 *   `firstkey`, `lastkey`, `keystep` and is needed if those three are not
 *   enough to describe the key positions. There are two steps to retrieve key
 *   positions: *begin search* (BS) in which index should find the first key and
 *   *find keys* (FK) which, relative to the output of BS, describes how can we
 *   will which arguments are keys. Additionally, there are key specific flags.
 *
 *     Key-specs cause the triplet (firstkey, lastkey, keystep) given in
 *     RM_CreateCommand to be recomputed, but it is still useful to provide
 *     these three parameters in RM_CreateCommand, to better support old Redis
 *     versions where RM_SetCommandInfo is not available.
 *
 *     Note that key-specs don't fully replace the "getkeys-api" (see
 *     RM_CreateCommand, RM_IsKeysPositionRequest and RM_KeyAtPosWithFlags) so
 *     it may be a good idea to supply both key-specs and implement the
 *     getkeys-api.
 *
 *     A key-spec has the following structure:
 *
 *         typedef struct RedisModuleCommandKeySpec {
 *             const char *notes;
 *             uint64_t flags;
 *             RedisModuleKeySpecBeginSearchType begin_search_type;
 *             union {
 *                 struct {
 *                     int pos;
 *                 } index;
 *                 struct {
 *                     const char *keyword;
 *                     int startfrom;
 *                 } keyword;
 *             } bs;
 *             RedisModuleKeySpecFindKeysType find_keys_type;
 *             union {
 *                 struct {
 *                     int lastkey;
 *                     int keystep;
 *                     int limit;
 *                 } range;
 *                 struct {
 *                     int keynumidx;
 *                     int firstkey;
 *                     int keystep;
 *                 } keynum;
 *             } fk;
 *         } RedisModuleCommandKeySpec;
 *
 *     Explanation of the fields of RedisModuleCommandKeySpec:
 *
 *     * `notes`: Optional notes or clarifications about this key spec.
 *
 *     * `flags`: A bitwise or of key-spec flags described below.
 *
 *     * `begin_search_type`: This describes how the first key is discovered.
 *       There are two ways to determine the first key:
 *
 *         * `REDISMODULE_KSPEC_BS_UNKNOWN`: There is no way to tell where the
 *           key args start.
 *         * `REDISMODULE_KSPEC_BS_INDEX`: Key args start at a constant index.
 *         * `REDISMODULE_KSPEC_BS_KEYWORD`: Key args start just after a
 *           specific keyword.
 *
 *     * `bs`: This is a union in which the `index` or `keyword` branch is used
 *       depending on the value of the `begin_search_type` field.
 *
 *         * `bs.index.pos`: The index from which we start the search for keys.
 *           (`REDISMODULE_KSPEC_BS_INDEX` only.)
 *
 *         * `bs.keyword.keyword`: The keyword (string) that indicates the
 *           beginning of key arguments. (`REDISMODULE_KSPEC_BS_KEYWORD` only.)
 *
 *         * `bs.keyword.startfrom`: An index in argv from which to start
 *           searching. Can be negative, which means start search from the end,
 *           in reverse. Example: -2 means to start in reverse from the
 *           penultimate argument. (`REDISMODULE_KSPEC_BS_KEYWORD` only.)
 *
 *     * `find_keys_type`: After the "begin search", this describes which
 *       arguments are keys. The strategies are:
 *
 *         * `REDISMODULE_KSPEC_BS_UNKNOWN`: There is no way to tell where the
 *           key args are located.
 *         * `REDISMODULE_KSPEC_FK_RANGE`: Keys end at a specific index (or
 *           relative to the last argument).
 *         * `REDISMODULE_KSPEC_FK_KEYNUM`: There's an argument that contains
 *           the number of key args somewhere before the keys themselves.
 *
 *       `find_keys_type` and `fk` can be omitted if this keyspec describes
 *       exactly one key.
 *
 *     * `fk`: This is a union in which the `range` or `keynum` branch is used
 *       depending on the value of the `find_keys_type` field.
 *
 *         * `fk.range` (for `REDISMODULE_KSPEC_FK_RANGE`): A struct with the
 *           following fields:
 *
 *             * `lastkey`: Index of the last key relative to the result of the
 *               begin search step. Can be negative, in which case it's not
 *               relative. -1 indicates the last argument, -2 one before the
 *               last and so on.
 *
 *             * `keystep`: How many arguments should we skip after finding a
 *               key, in order to find the next one?
 *
 *             * `limit`: If `lastkey` is -1, we use `limit` to stop the search
 *               by a factor. 0 and 1 mean no limit. 2 means 1/2 of the
 *               remaining args, 3 means 1/3, and so on.
 *
 *         * `fk.keynum` (for `REDISMODULE_KSPEC_FK_KEYNUM`): A struct with the
 *           following fields:
 *
 *             * `keynumidx`: Index of the argument containing the number of
 *               keys to come, relative to the result of the begin search step.
 *
 *             * `firstkey`: Index of the fist key relative to the result of the
 *               begin search step. (Usually it's just after `keynumidx`, in
 *               which case it should be set to `keynumidx + 1`.)
 *
 *             * `keystep`: How many arguments should we skip after finding a
 *               key, in order to find the next one?
 *
 *     Key-spec flags:
 *
 *     The first four refer to what the command actually does with the *value or
 *     metadata of the key*, and not necessarily the user data or how it affects
 *     it. Each key-spec may must have exactly one of these. Any operation
 *     that's not distinctly deletion, overwrite or read-only would be marked as
 *     RW.
 *
 *     * `REDISMODULE_CMD_KEY_RO`: Read-Only. Reads the value of the key, but
 *       doesn't necessarily return it.
 *
 *     * `REDISMODULE_CMD_KEY_RW`: Read-Write. Modifies the data stored in the
 *       value of the key or its metadata.
 *
 *     * `REDISMODULE_CMD_KEY_OW`: Overwrite. Overwrites the data stored in the
 *       value of the key.
 *
 *     * `REDISMODULE_CMD_KEY_RM`: Deletes the key.
 *
 *     The next four refer to *user data inside the value of the key*, not the
 *     metadata like LRU, type, cardinality. It refers to the logical operation
 *     on the user's data (actual input strings or TTL), being
 *     used/returned/copied/changed. It doesn't refer to modification or
 *     returning of metadata (like type, count, presence of data). ACCESS can be
 *     combined with one of the write operations INSERT, DELETE or UPDATE. Any
 *     write that's not an INSERT or a DELETE would be UPDATE.
 *
 *     * `REDISMODULE_CMD_KEY_ACCESS`: Returns, copies or uses the user data
 *       from the value of the key.
 *
 *     * `REDISMODULE_CMD_KEY_UPDATE`: Updates data to the value, new value may
 *       depend on the old value.
 *
 *     * `REDISMODULE_CMD_KEY_INSERT`: Adds data to the value with no chance of
 *       modification or deletion of existing data.
 *
 *     * `REDISMODULE_CMD_KEY_DELETE`: Explicitly deletes some content from the
 *       value of the key.
 *
 *     Other flags:
 *
 *     * `REDISMODULE_CMD_KEY_NOT_KEY`: The key is not actually a key, but 
 *       should be routed in cluster mode as if it was a key.
 *
 *     * `REDISMODULE_CMD_KEY_INCOMPLETE`: The keyspec might not point out all
 *       the keys it should cover.
 *
 *     * `REDISMODULE_CMD_KEY_VARIABLE_FLAGS`: Some keys might have different
 *       flags depending on arguments.
 *
 * - `args`: An array of RedisModuleCommandArg, terminated by an element memset
 *   to zero. RedisModuleCommandArg is a structure with at the fields described
 *   below.
 *
 *         typedef struct RedisModuleCommandArg {
 *             const char *name;
 *             RedisModuleCommandArgType type;
 *             int key_spec_index;
 *             const char *token;
 *             const char *summary;
 *             const char *since;
 *             int flags;
 *             struct RedisModuleCommandArg *subargs;
 *         } RedisModuleCommandArg;
 *
 *     Explanation of the fields:
 *
 *     * `name`: Name of the argument.
 *
 *     * `type`: The type of the argument. See below for details. The types
 *       `REDISMODULE_ARG_TYPE_ONEOF` and `REDISMODULE_ARG_TYPE_BLOCK` require
 *       an argument to have sub-arguments, i.e. `subargs`.
 *
 *     * `key_spec_index`: If the `type` is `REDISMODULE_ARG_TYPE_KEY` you must
 *       provide the index of the key-spec associated with this argument. See
 *       `key_specs` above. If the argument is not a key, you may specify -1.
 *
 *     * `token`: The token preceding the argument (optional). Example: the
 *       argument `seconds` in `SET` has a token `EX`. If the argument consists
 *       of only a token (for example `NX` in `SET`) the type should be
 *       `REDISMODULE_ARG_TYPE_PURE_TOKEN` and `value` should be NULL.
 *
 *     * `summary`: A short description of the argument (optional).
 *
 *     * `since`: The first version which included this argument (optional).
 *
 *     * `flags`: A bitwise or of the macros `REDISMODULE_CMD_ARG_*`. See below.
 *
 *     * `value`: The display-value of the argument. This string is what should
 *       be displayed when creating the command syntax from the output of
 *       `COMMAND`. If `token` is not NULL, it should also be displayed.
 *
 *     Explanation of `RedisModuleCommandArgType`:
 *
 *     * `REDISMODULE_ARG_TYPE_STRING`: String argument.
 *     * `REDISMODULE_ARG_TYPE_INTEGER`: Integer argument.
 *     * `REDISMODULE_ARG_TYPE_DOUBLE`: Double-precision float argument.
 *     * `REDISMODULE_ARG_TYPE_KEY`: String argument representing a keyname.
 *     * `REDISMODULE_ARG_TYPE_PATTERN`: String, but regex pattern.
 *     * `REDISMODULE_ARG_TYPE_UNIX_TIME`: Integer, but Unix timestamp.
 *     * `REDISMODULE_ARG_TYPE_PURE_TOKEN`: Argument doesn't have a placeholder.
 *       It's just a token without a value. Example: the `KEEPTTL` option of the
 *       `SET` command.
 *     * `REDISMODULE_ARG_TYPE_ONEOF`: Used when the user can choose only one of
 *       a few sub-arguments. Requires `subargs`. Example: the `NX` and `XX`
 *       options of `SET`.
 *     * `REDISMODULE_ARG_TYPE_BLOCK`: Used when one wants to group together
 *       several sub-arguments, usually to apply something on all of them, like
 *       making the entire group "optional". Requires `subargs`. Example: the
 *       `LIMIT offset count` parameters in `ZRANGE`.
 *
 *     Explanation of the command argument flags:
 *
 *     * `REDISMODULE_CMD_ARG_OPTIONAL`: The argument is optional (like GET in
 *       the SET command).
 *     * `REDISMODULE_CMD_ARG_MULTIPLE`: The argument may repeat itself (like
 *       key in DEL).
 *     * `REDISMODULE_CMD_ARG_MULTIPLE_TOKEN`: The argument may repeat itself,
 *       and so does its token (like `GET pattern` in SORT).
 *
 * On success REDISMODULE_OK is returned. On error REDISMODULE_ERR is returned
 * and `errno` is set to EINVAL if invalid info was provided or EEXIST if info
 * has already been set. If the info is invalid, a warning is logged explaining
 * which part of the info is invalid and why. */
int RM_SetCommandInfo(RedisModuleCommand *command, const RedisModuleCommandInfo *info) {
    if (!moduleValidateCommandInfo(info)) {
        errno = EINVAL;
        return REDISMODULE_ERR;
    }

    struct redisCommand *cmd = command->rediscmd;

    /* Check if any info has already been set. Overwriting info involves freeing
     * the old info, which is not implemented. */
    if (cmd->summary || cmd->complexity || cmd->since || cmd->history ||
        cmd->tips || cmd->args ||
        !(cmd->key_specs_num == 0 ||
          /* Allow key spec populated from legacy (first,last,step) to exist. */
          (cmd->key_specs_num == 1 && cmd->key_specs == cmd->key_specs_static &&
           cmd->key_specs[0].begin_search_type == KSPEC_BS_INDEX &&
           cmd->key_specs[0].find_keys_type == KSPEC_FK_RANGE))) {
        errno = EEXIST;
        return REDISMODULE_ERR;
    }

    if (info->summary) cmd->summary = zstrdup(info->summary);
    if (info->complexity) cmd->complexity = zstrdup(info->complexity);
    if (info->since) cmd->since = zstrdup(info->since);

    const RedisModuleCommandInfoVersion *version = info->version;
    if (info->history) {
        size_t count = 0;
        while (moduleCmdHistoryEntryAt(version, info->history, count)->since)
            count++;
        serverAssert(count < SIZE_MAX / sizeof(commandHistory));
        cmd->history = zmalloc(sizeof(commandHistory) * (count + 1));
        for (size_t j = 0; j < count; j++) {
            RedisModuleCommandHistoryEntry *entry =
                moduleCmdHistoryEntryAt(version, info->history, j);
            cmd->history[j].since = zstrdup(entry->since);
            cmd->history[j].changes = zstrdup(entry->changes);
        }
        cmd->history[count].since = NULL;
        cmd->history[count].changes = NULL;
        cmd->num_history = count;
    }

    if (info->tips) {
        int count;
        sds *tokens = sdssplitlen(info->tips, strlen(info->tips), " ", 1, &count);
        if (tokens) {
            cmd->tips = zmalloc(sizeof(char *) * (count + 1));
            for (int j = 0; j < count; j++) {
                cmd->tips[j] = zstrdup(tokens[j]);
            }
            cmd->tips[count] = NULL;
            cmd->num_tips = count;
            sdsfreesplitres(tokens, count);
        }
    }

    if (info->arity) cmd->arity = info->arity;

    if (info->key_specs) {
        /* Count and allocate the key specs. */
        size_t count = 0;
        while (moduleCmdKeySpecAt(version, info->key_specs, count)->begin_search_type)
            count++;
        serverAssert(count < INT_MAX);
        if (count <= STATIC_KEY_SPECS_NUM) {
            cmd->key_specs_max = STATIC_KEY_SPECS_NUM;
            cmd->key_specs = cmd->key_specs_static;
        } else {
            cmd->key_specs_max = count;
            cmd->key_specs = zmalloc(sizeof(keySpec) * count);
        }

        /* Copy the contents of the RedisModuleCommandKeySpec array. */
        cmd->key_specs_num = count;
        for (size_t j = 0; j < count; j++) {
            RedisModuleCommandKeySpec *spec =
                moduleCmdKeySpecAt(version, info->key_specs, j);
            cmd->key_specs[j].notes = spec->notes ? zstrdup(spec->notes) : NULL;
            cmd->key_specs[j].flags = moduleConvertKeySpecsFlags(spec->flags, 1);
            switch (spec->begin_search_type) {
            case REDISMODULE_KSPEC_BS_UNKNOWN:
                cmd->key_specs[j].begin_search_type = KSPEC_BS_UNKNOWN;
                break;
            case REDISMODULE_KSPEC_BS_INDEX:
                cmd->key_specs[j].begin_search_type = KSPEC_BS_INDEX;
                cmd->key_specs[j].bs.index.pos = spec->bs.index.pos;
                break;
            case REDISMODULE_KSPEC_BS_KEYWORD:
                cmd->key_specs[j].begin_search_type = KSPEC_BS_KEYWORD;
                cmd->key_specs[j].bs.keyword.keyword = zstrdup(spec->bs.keyword.keyword);
                cmd->key_specs[j].bs.keyword.startfrom = spec->bs.keyword.startfrom;
                break;
            default:
                /* Can't happen; stopped in moduleValidateCommandInfo(). */
                serverPanic("Unknown begin_search_type");
            }

            switch (spec->find_keys_type) {
            case REDISMODULE_KSPEC_FK_OMITTED:
                /* Omitted field is shorthand to say that it's a single key. */
                cmd->key_specs[j].find_keys_type = KSPEC_FK_RANGE;
                cmd->key_specs[j].fk.range.lastkey = 0;
                cmd->key_specs[j].fk.range.keystep = 1;
                cmd->key_specs[j].fk.range.limit = 0;
                break;
            case REDISMODULE_KSPEC_FK_UNKNOWN:
                cmd->key_specs[j].find_keys_type = KSPEC_FK_UNKNOWN;
                break;
            case REDISMODULE_KSPEC_FK_RANGE:
                cmd->key_specs[j].find_keys_type = KSPEC_FK_RANGE;
                cmd->key_specs[j].fk.range.lastkey = spec->fk.range.lastkey;
                cmd->key_specs[j].fk.range.keystep = spec->fk.range.keystep;
                cmd->key_specs[j].fk.range.limit = spec->fk.range.limit;
                break;
            case REDISMODULE_KSPEC_FK_KEYNUM:
                cmd->key_specs[j].find_keys_type = KSPEC_FK_KEYNUM;
                cmd->key_specs[j].fk.keynum.keynumidx = spec->fk.keynum.keynumidx;
                cmd->key_specs[j].fk.keynum.firstkey = spec->fk.keynum.firstkey;
                cmd->key_specs[j].fk.keynum.keystep = spec->fk.keynum.keystep;
                break;
            default:
                /* Can't happen; stopped in moduleValidateCommandInfo(). */
                serverPanic("Unknown find_keys_type");
            }
        }

        /* Update the legacy (first,last,step) spec and "movablekeys" flag used by the COMMAND command,
         * by trying to "glue" consecutive range key specs. */
        populateCommandLegacyRangeSpec(cmd);
    }

    if (info->args) {
        cmd->args = moduleCopyCommandArgs(info->args, version);
        /* Populate arg.num_args with the number of subargs, recursively */
        cmd->num_args = populateArgsStructure(cmd->args);
    }

    /* Fields added in future versions to be added here, under conditions like
     * `if (info->version >= 2) { access version 2 fields here }` */

    return REDISMODULE_OK;
}

/* Returns 1 if v is a power of two, 0 otherwise. */
static inline int isPowerOfTwo(uint64_t v) {
    return v && !(v & (v - 1));
}

/* Returns 1 if the command info is valid and 0 otherwise. */
static int moduleValidateCommandInfo(const RedisModuleCommandInfo *info) {
    const RedisModuleCommandInfoVersion *version = info->version;
    if (!version) {
        serverLog(LL_WARNING, "Invalid command info: version missing");
        return 0;
    }

    /* No validation for the fields summary, complexity, since, tips (strings or
     * NULL) and arity (any integer). */

    /* History: If since is set, changes must also be set. */
    if (info->history) {
        for (size_t j = 0;
             moduleCmdHistoryEntryAt(version, info->history, j)->since;
             j++)
        {
            if (!moduleCmdHistoryEntryAt(version, info->history, j)->changes) {
                serverLog(LL_WARNING, "Invalid command info: history[%zd].changes missing", j);
                return 0;
            }
        }
    }

    /* Key specs. */
    if (info->key_specs) {
        for (size_t j = 0;
             moduleCmdKeySpecAt(version, info->key_specs, j)->begin_search_type;
             j++)
        {
            RedisModuleCommandKeySpec *spec =
                moduleCmdKeySpecAt(version, info->key_specs, j);
            if (j >= INT_MAX) {
                serverLog(LL_WARNING, "Invalid command info: Too many key specs");
                return 0; /* redisCommand.key_specs_num is an int. */
            }

            /* Flags. Exactly one flag in a group is set if and only if the
             * masked bits is a power of two. */
            uint64_t key_flags =
                REDISMODULE_CMD_KEY_RO | REDISMODULE_CMD_KEY_RW |
                REDISMODULE_CMD_KEY_OW | REDISMODULE_CMD_KEY_RM;
            uint64_t write_flags =
                REDISMODULE_CMD_KEY_INSERT | REDISMODULE_CMD_KEY_DELETE |
                REDISMODULE_CMD_KEY_UPDATE;
            if (!isPowerOfTwo(spec->flags & key_flags)) {
                serverLog(LL_WARNING,
                          "Invalid command info: key_specs[%zd].flags: "
                          "Exactly one of the flags RO, RW, OW, RM required", j);
                return 0;
            }
            if ((spec->flags & write_flags) != 0 &&
                !isPowerOfTwo(spec->flags & write_flags))
            {
                serverLog(LL_WARNING,
                          "Invalid command info: key_specs[%zd].flags: "
                          "INSERT, DELETE and UPDATE are mutually exclusive", j);
                return 0;
            }

            switch (spec->begin_search_type) {
            case REDISMODULE_KSPEC_BS_UNKNOWN: break;
            case REDISMODULE_KSPEC_BS_INDEX: break;
            case REDISMODULE_KSPEC_BS_KEYWORD:
                if (spec->bs.keyword.keyword == NULL) {
                    serverLog(LL_WARNING,
                              "Invalid command info: key_specs[%zd].bs.keyword.keyword "
                              "required when begin_search_type is KEYWORD", j);
                    return 0;
                }
                break;
            default:
                serverLog(LL_WARNING,
                          "Invalid command info: key_specs[%zd].begin_search_type: "
                          "Invalid value %d", j, spec->begin_search_type);
                return 0;
            }

            /* Validate find_keys_type. */
            switch (spec->find_keys_type) {
            case REDISMODULE_KSPEC_FK_OMITTED: break; /* short for RANGE {0,1,0} */
            case REDISMODULE_KSPEC_FK_UNKNOWN: break;
            case REDISMODULE_KSPEC_FK_RANGE: break;
            case REDISMODULE_KSPEC_FK_KEYNUM: break;
            default:
                serverLog(LL_WARNING,
                          "Invalid command info: key_specs[%zd].find_keys_type: "
                          "Invalid value %d", j, spec->find_keys_type);
                return 0;
            }
        }
    }

    /* Args, subargs (recursive) */
    return moduleValidateCommandArgs(info->args, version);
}

/* When from_api is true, converts from REDISMODULE_CMD_KEY_* flags to CMD_KEY_* flags.
 * When from_api is false, converts from CMD_KEY_* flags to REDISMODULE_CMD_KEY_* flags. */
static int64_t moduleConvertKeySpecsFlags(int64_t flags, int from_api) {
    int64_t out = 0;
    int64_t map[][2] = {
        {REDISMODULE_CMD_KEY_RO, CMD_KEY_RO},
        {REDISMODULE_CMD_KEY_RW, CMD_KEY_RW},
        {REDISMODULE_CMD_KEY_OW, CMD_KEY_OW},
        {REDISMODULE_CMD_KEY_RM, CMD_KEY_RM},
        {REDISMODULE_CMD_KEY_ACCESS, CMD_KEY_ACCESS},
        {REDISMODULE_CMD_KEY_INSERT, CMD_KEY_INSERT},
        {REDISMODULE_CMD_KEY_UPDATE, CMD_KEY_UPDATE},
        {REDISMODULE_CMD_KEY_DELETE, CMD_KEY_DELETE},
        {REDISMODULE_CMD_KEY_NOT_KEY, CMD_KEY_NOT_KEY},
        {REDISMODULE_CMD_KEY_INCOMPLETE, CMD_KEY_INCOMPLETE},
        {REDISMODULE_CMD_KEY_VARIABLE_FLAGS, CMD_KEY_VARIABLE_FLAGS},
        {0,0}};

    int from_idx = from_api ? 0 : 1, to_idx = !from_idx;
    for (int i=0; map[i][0]; i++)
        if (flags & map[i][from_idx]) out |= map[i][to_idx];
    return out;
}

/* Validates an array of RedisModuleCommandArg. Returns 1 if it's valid and 0 if
 * it's invalid. */
static int moduleValidateCommandArgs(RedisModuleCommandArg *args,
                                     const RedisModuleCommandInfoVersion *version) {
    if (args == NULL) return 1; /* Missing args is OK. */
    for (size_t j = 0; moduleCmdArgAt(version, args, j)->name != NULL; j++) {
        RedisModuleCommandArg *arg = moduleCmdArgAt(version, args, j);
        int arg_type_error = 0;
        moduleConvertArgType(arg->type, &arg_type_error);
        if (arg_type_error) {
            serverLog(LL_WARNING,
                      "Invalid command info: Argument \"%s\": Undefined type %d",
                      arg->name, arg->type);
            return 0;
        }
        if (arg->type == REDISMODULE_ARG_TYPE_PURE_TOKEN && !arg->token) {
            serverLog(LL_WARNING,
                      "Invalid command info: Argument \"%s\": "
                      "token required when type is PURE_TOKEN", args[j].name);
            return 0;
        }

        if (arg->type == REDISMODULE_ARG_TYPE_KEY) {
            if (arg->key_spec_index < 0) {
                serverLog(LL_WARNING,
                          "Invalid command info: Argument \"%s\": "
                          "key_spec_index required when type is KEY",
                          arg->name);
                return 0;
            }
        } else if (arg->key_spec_index != -1 && arg->key_spec_index != 0) {
            /* 0 is allowed for convenience, to allow it to be omitted in
             * compound struct literals on the form `.field = value`. */
            serverLog(LL_WARNING,
                      "Invalid command info: Argument \"%s\": "
                      "key_spec_index specified but type isn't KEY",
                      arg->name);
            return 0;
        }

        if (arg->flags & ~(_REDISMODULE_CMD_ARG_NEXT - 1)) {
            serverLog(LL_WARNING,
                      "Invalid command info: Argument \"%s\": Invalid flags",
                      arg->name);
            return 0;
        }

        if (arg->type == REDISMODULE_ARG_TYPE_ONEOF ||
            arg->type == REDISMODULE_ARG_TYPE_BLOCK)
        {
            if (arg->subargs == NULL) {
                serverLog(LL_WARNING,
                          "Invalid command info: Argument \"%s\": "
                          "subargs required when type is ONEOF or BLOCK",
                          arg->name);
                return 0;
            }
            if (!moduleValidateCommandArgs(arg->subargs, version)) return 0;
        } else {
            if (arg->subargs != NULL) {
                serverLog(LL_WARNING,
                          "Invalid command info: Argument \"%s\": "
                          "subargs specified but type isn't ONEOF nor BLOCK",
                          arg->name);
                return 0;
            }
        }
    }
    return 1;
}

/* Converts an array of RedisModuleCommandArg into a freshly allocated array of
 * struct redisCommandArg. */
static struct redisCommandArg *moduleCopyCommandArgs(RedisModuleCommandArg *args,
                                                     const RedisModuleCommandInfoVersion *version) {
    size_t count = 0;
    while (moduleCmdArgAt(version, args, count)->name) count++;
    serverAssert(count < SIZE_MAX / sizeof(struct redisCommandArg));
    struct redisCommandArg *realargs = zcalloc((count+1) * sizeof(redisCommandArg));

    for (size_t j = 0; j < count; j++) {
        RedisModuleCommandArg *arg = moduleCmdArgAt(version, args, j);
        realargs[j].name = zstrdup(arg->name);
        realargs[j].type = moduleConvertArgType(arg->type, NULL);
        if (arg->type == REDISMODULE_ARG_TYPE_KEY)
            realargs[j].key_spec_index = arg->key_spec_index;
        else
            realargs[j].key_spec_index = -1;
        if (arg->token) realargs[j].token = zstrdup(arg->token);
        if (arg->summary) realargs[j].summary = zstrdup(arg->summary);
        if (arg->since) realargs[j].since = zstrdup(arg->since);
        if (arg->deprecated_since) realargs[j].deprecated_since = zstrdup(arg->deprecated_since);
        if (arg->display_text) realargs[j].display_text = zstrdup(arg->display_text);
        realargs[j].flags = moduleConvertArgFlags(arg->flags);
        if (arg->subargs) realargs[j].subargs = moduleCopyCommandArgs(arg->subargs, version);
    }
    return realargs;
}

static redisCommandArgType moduleConvertArgType(RedisModuleCommandArgType type, int *error) {
    if (error) *error = 0;
    switch (type) {
    case REDISMODULE_ARG_TYPE_STRING: return ARG_TYPE_STRING;
    case REDISMODULE_ARG_TYPE_INTEGER: return ARG_TYPE_INTEGER;
    case REDISMODULE_ARG_TYPE_DOUBLE: return ARG_TYPE_DOUBLE;
    case REDISMODULE_ARG_TYPE_KEY: return ARG_TYPE_KEY;
    case REDISMODULE_ARG_TYPE_PATTERN: return ARG_TYPE_PATTERN;
    case REDISMODULE_ARG_TYPE_UNIX_TIME: return ARG_TYPE_UNIX_TIME;
    case REDISMODULE_ARG_TYPE_PURE_TOKEN: return ARG_TYPE_PURE_TOKEN;
    case REDISMODULE_ARG_TYPE_ONEOF: return ARG_TYPE_ONEOF;
    case REDISMODULE_ARG_TYPE_BLOCK: return ARG_TYPE_BLOCK;
    default:
        if (error) *error = 1;
        return -1;
    }
}

static int moduleConvertArgFlags(int flags) {
    int realflags = 0;
    if (flags & REDISMODULE_CMD_ARG_OPTIONAL) realflags |= CMD_ARG_OPTIONAL;
    if (flags & REDISMODULE_CMD_ARG_MULTIPLE) realflags |= CMD_ARG_MULTIPLE;
    if (flags & REDISMODULE_CMD_ARG_MULTIPLE_TOKEN) realflags |= CMD_ARG_MULTIPLE_TOKEN;
    return realflags;
}

/* Return `struct RedisModule *` as `void *` to avoid exposing it outside of module.c. */
void *moduleGetHandleByName(char *modulename) {
    return dictFetchValue(modules,modulename);
}

/* Returns 1 if `cmd` is a command of the module `modulename`. 0 otherwise. */
int moduleIsModuleCommand(void *module_handle, struct redisCommand *cmd) {
    if (cmd->proc != RedisModuleCommandDispatcher)
        return 0;
    if (module_handle == NULL)
        return 0;
    RedisModuleCommand *cp = cmd->module_cmd;
    return (cp->module == module_handle);
}

/* --------------------------------------------------------------------------
 * ## Module information and time measurement
 * -------------------------------------------------------------------------- */

int moduleListConfigMatch(void *config, void *name) {
    return strcasecmp(((ModuleConfig *) config)->name, (char *) name) == 0;
}

void moduleListFree(void *config) {
    ModuleConfig *module_config = (ModuleConfig *) config;
    sdsfree(module_config->name);
    zfree(config);
}

void RM_SetModuleAttribs(RedisModuleCtx *ctx, const char *name, int ver, int apiver) {
    /* Called by RM_Init() to setup the `ctx->module` structure.
     *
     * This is an internal function, Redis modules developers don't need
     * to use it. */
    RedisModule *module;

    if (ctx->module != NULL) return;
    module = zmalloc(sizeof(*module));
    module->name = sdsnew(name);
    module->ver = ver;
    module->apiver = apiver;
    module->types = listCreate();
    module->usedby = listCreate();
    module->using = listCreate();
    module->filters = listCreate();
    module->module_configs = listCreate();
    listSetMatchMethod(module->module_configs, moduleListConfigMatch);
    listSetFreeMethod(module->module_configs, moduleListFree);
    module->in_call = 0;
    module->configs_initialized = 0;
    module->in_hook = 0;
    module->options = 0;
    module->info_cb = 0;
    module->defrag_cb = 0;
    module->loadmod = NULL;
    module->num_commands_with_acl_categories = 0;
    module->onload = 1;
    ctx->module = module;
}

/* Return non-zero if the module name is busy.
 * Otherwise zero is returned. */
int RM_IsModuleNameBusy(const char *name) {
    sds modulename = sdsnew(name);
    dictEntry *de = dictFind(modules,modulename);
    sdsfree(modulename);
    return de != NULL;
}

/* Return the current UNIX time in milliseconds. */
mstime_t RM_Milliseconds(void) {
    return mstime();
}

/* Return counter of micro-seconds relative to an arbitrary point in time. */
uint64_t RM_MonotonicMicroseconds(void) {
    return getMonotonicUs();
}

/* Return the current UNIX time in microseconds */
ustime_t RM_Microseconds() {
    return ustime();
}

/* Return the cached UNIX time in microseconds.
 * It is updated in the server cron job and before executing a command.
 * It is useful for complex call stacks, such as a command causing a
 * key space notification, causing a module to execute a RedisModule_Call,
 * causing another notification, etc.
 * It makes sense that all this callbacks would use the same clock. */
ustime_t RM_CachedMicroseconds() {
    return server.ustime;
}

/* Mark a point in time that will be used as the start time to calculate
 * the elapsed execution time when RM_BlockedClientMeasureTimeEnd() is called.
 * Within the same command, you can call multiple times
 * RM_BlockedClientMeasureTimeStart() and RM_BlockedClientMeasureTimeEnd()
 * to accumulate independent time intervals to the background duration.
 * This method always return REDISMODULE_OK. */
int RM_BlockedClientMeasureTimeStart(RedisModuleBlockedClient *bc) {
    elapsedStart(&(bc->background_timer));
    return REDISMODULE_OK;
}

/* Mark a point in time that will be used as the end time
 * to calculate the elapsed execution time.
 * On success REDISMODULE_OK is returned.
 * This method only returns REDISMODULE_ERR if no start time was
 * previously defined ( meaning RM_BlockedClientMeasureTimeStart was not called ). */
int RM_BlockedClientMeasureTimeEnd(RedisModuleBlockedClient *bc) {
    // If the counter is 0 then we haven't called RM_BlockedClientMeasureTimeStart
    if (!bc->background_timer)
        return REDISMODULE_ERR;
    bc->background_duration += elapsedUs(bc->background_timer);
    return REDISMODULE_OK;
}

/* This API allows modules to let Redis process background tasks, and some
 * commands during long blocking execution of a module command.
 * The module can call this API periodically.
 * The flags is a bit mask of these:
 *
 * - `REDISMODULE_YIELD_FLAG_NONE`: No special flags, can perform some background
 *                                  operations, but not process client commands.
 * - `REDISMODULE_YIELD_FLAG_CLIENTS`: Redis can also process client commands.
 *
 * The `busy_reply` argument is optional, and can be used to control the verbose
 * error string after the `-BUSY` error code.
 *
 * When the `REDISMODULE_YIELD_FLAG_CLIENTS` is used, Redis will only start
 * processing client commands after the time defined by the
 * `busy-reply-threshold` config, in which case Redis will start rejecting most
 * commands with `-BUSY` error, but allow the ones marked with the `allow-busy`
 * flag to be executed.
 * This API can also be used in thread safe context (while locked), and during
 * loading (in the `rdb_load` callback, in which case it'll reject commands with
 * the -LOADING error)
 */
void RM_Yield(RedisModuleCtx *ctx, int flags, const char *busy_reply) {
    static int yield_nesting = 0;
    /* Avoid nested calls to RM_Yield */
    if (yield_nesting)
        return;
    yield_nesting++;

    long long now = getMonotonicUs();
    if (now >= ctx->next_yield_time) {
        /* In loading mode, there's no need to handle busy_module_yield_reply,
         * and busy_module_yield_flags, since redis is anyway rejecting all
         * commands with -LOADING. */
        if (server.loading) {
            /* Let redis process events */
            processEventsWhileBlocked();
        } else {
            const char *prev_busy_module_yield_reply = server.busy_module_yield_reply;
            server.busy_module_yield_reply = busy_reply;
            /* start the blocking operation if not already started. */
            if (!server.busy_module_yield_flags) {
                server.busy_module_yield_flags = BUSY_MODULE_YIELD_EVENTS;
                blockingOperationStarts();
                if (server.current_client)
                    protectClient(server.current_client);
            }
            if (flags & REDISMODULE_YIELD_FLAG_CLIENTS)
                server.busy_module_yield_flags |= BUSY_MODULE_YIELD_CLIENTS;

            /* Let redis process events */
            processEventsWhileBlocked();

            server.busy_module_yield_reply = prev_busy_module_yield_reply;
            /* Possibly restore the previous flags in case of two nested contexts
             * that use this API with different flags, but keep the first bit
             * (PROCESS_EVENTS) set, so we know to call blockingOperationEnds on time. */
            server.busy_module_yield_flags &= ~BUSY_MODULE_YIELD_CLIENTS;
        }

        /* decide when the next event should fire. */
        ctx->next_yield_time = now + 1000000 / server.hz;
    }
    yield_nesting--;
}

/* Set flags defining capabilities or behavior bit flags.
 *
 * REDISMODULE_OPTIONS_HANDLE_IO_ERRORS:
 * Generally, modules don't need to bother with this, as the process will just
 * terminate if a read error happens, however, setting this flag would allow
 * repl-diskless-load to work if enabled.
 * The module should use RedisModule_IsIOError after reads, before using the
 * data that was read, and in case of error, propagate it upwards, and also be
 * able to release the partially populated value and all it's allocations.
 *
 * REDISMODULE_OPTION_NO_IMPLICIT_SIGNAL_MODIFIED:
 * See RM_SignalModifiedKey().
 * 
 * REDISMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD:
 * Setting this flag indicates module awareness of diskless async replication (repl-diskless-load=swapdb)
 * and that redis could be serving reads during replication instead of blocking with LOADING status.
 *
 * REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS:
 * Declare that the module wants to get nested key-space notifications.
 * By default, Redis will not fire key-space notifications that happened inside
 * a key-space notification callback. This flag allows to change this behavior
 * and fire nested key-space notifications. Notice: if enabled, the module
 * should protected itself from infinite recursion. */
void RM_SetModuleOptions(RedisModuleCtx *ctx, int options) {
    ctx->module->options = options;
}

/* Signals that the key is modified from user's perspective (i.e. invalidate WATCH
 * and client side caching).
 *
 * This is done automatically when a key opened for writing is closed, unless
 * the option REDISMODULE_OPTION_NO_IMPLICIT_SIGNAL_MODIFIED has been set using
 * RM_SetModuleOptions().
*/
int RM_SignalModifiedKey(RedisModuleCtx *ctx, RedisModuleString *keyname) {
    signalModifiedKey(ctx->client,ctx->client->db,keyname);
    return REDISMODULE_OK;
}

/* --------------------------------------------------------------------------
 * ## Automatic memory management for modules
 * -------------------------------------------------------------------------- */

/* Enable automatic memory management.
 *
 * The function must be called as the first function of a command implementation
 * that wants to use automatic memory.
 *
 * When enabled, automatic memory management tracks and automatically frees
 * keys, call replies and Redis string objects once the command returns. In most
 * cases this eliminates the need of calling the following functions:
 *
 * 1. RedisModule_CloseKey()
 * 2. RedisModule_FreeCallReply()
 * 3. RedisModule_FreeString()
 *
 * These functions can still be used with automatic memory management enabled,
 * to optimize loops that make numerous allocations for example. */
void RM_AutoMemory(RedisModuleCtx *ctx) {
    ctx->flags |= REDISMODULE_CTX_AUTO_MEMORY;
}

/* Add a new object to release automatically when the callback returns. */
void autoMemoryAdd(RedisModuleCtx *ctx, int type, void *ptr) {
    if (!(ctx->flags & REDISMODULE_CTX_AUTO_MEMORY)) return;
    if (ctx->amqueue_used == ctx->amqueue_len) {
        ctx->amqueue_len *= 2;
        if (ctx->amqueue_len < 16) ctx->amqueue_len = 16;
        ctx->amqueue = zrealloc(ctx->amqueue,sizeof(struct AutoMemEntry)*ctx->amqueue_len);
    }
    ctx->amqueue[ctx->amqueue_used].type = type;
    ctx->amqueue[ctx->amqueue_used].ptr = ptr;
    ctx->amqueue_used++;
}

/* Mark an object as freed in the auto release queue, so that users can still
 * free things manually if they want.
 *
 * The function returns 1 if the object was actually found in the auto memory
 * pool, otherwise 0 is returned. */
int autoMemoryFreed(RedisModuleCtx *ctx, int type, void *ptr) {
    if (!(ctx->flags & REDISMODULE_CTX_AUTO_MEMORY)) return 0;

    int count = (ctx->amqueue_used+1)/2;
    for (int j = 0; j < count; j++) {
        for (int side = 0; side < 2; side++) {
            /* For side = 0 check right side of the array, for
             * side = 1 check the left side instead (zig-zag scanning). */
            int i = (side == 0) ? (ctx->amqueue_used - 1 - j) : j;
            if (ctx->amqueue[i].type == type &&
                ctx->amqueue[i].ptr == ptr)
            {
                ctx->amqueue[i].type = REDISMODULE_AM_FREED;

                /* Switch the freed element and the last element, to avoid growing
                 * the queue unnecessarily if we allocate/free in a loop */
                if (i != ctx->amqueue_used-1) {
                    ctx->amqueue[i] = ctx->amqueue[ctx->amqueue_used-1];
                }

                /* Reduce the size of the queue because we either moved the top
                 * element elsewhere or freed it */
                ctx->amqueue_used--;
                return 1;
            }
        }
    }
    return 0;
}

/* Release all the objects in queue. */
void autoMemoryCollect(RedisModuleCtx *ctx) {
    if (!(ctx->flags & REDISMODULE_CTX_AUTO_MEMORY)) return;
    /* Clear the AUTO_MEMORY flag from the context, otherwise the functions
     * we call to free the resources, will try to scan the auto release
     * queue to mark the entries as freed. */
    ctx->flags &= ~REDISMODULE_CTX_AUTO_MEMORY;
    int j;
    for (j = 0; j < ctx->amqueue_used; j++) {
        void *ptr = ctx->amqueue[j].ptr;
        switch(ctx->amqueue[j].type) {
        case REDISMODULE_AM_STRING: decrRefCount(ptr); break;
        case REDISMODULE_AM_REPLY: RM_FreeCallReply(ptr); break;
        case REDISMODULE_AM_KEY: RM_CloseKey(ptr); break;
        case REDISMODULE_AM_DICT: RM_FreeDict(NULL,ptr); break;
        case REDISMODULE_AM_INFO: RM_FreeServerInfo(NULL,ptr); break;
        }
    }
    ctx->flags |= REDISMODULE_CTX_AUTO_MEMORY;
    zfree(ctx->amqueue);
    ctx->amqueue = NULL;
    ctx->amqueue_len = 0;
    ctx->amqueue_used = 0;
}

/* --------------------------------------------------------------------------
 * ## String objects APIs
 * -------------------------------------------------------------------------- */

/* Create a new module string object. The returned string must be freed
 * with RedisModule_FreeString(), unless automatic memory is enabled.
 *
 * The string is created by copying the `len` bytes starting
 * at `ptr`. No reference is retained to the passed buffer.
 *
 * The module context 'ctx' is optional and may be NULL if you want to create
 * a string out of the context scope. However in that case, the automatic
 * memory management will not be available, and the string memory must be
 * managed manually. */
RedisModuleString *RM_CreateString(RedisModuleCtx *ctx, const char *ptr, size_t len) {
    RedisModuleString *o = createStringObject(ptr,len);
    if (ctx != NULL) autoMemoryAdd(ctx,REDISMODULE_AM_STRING,o);
    return o;
}

/* Create a new module string object from a printf format and arguments.
 * The returned string must be freed with RedisModule_FreeString(), unless
 * automatic memory is enabled.
 *
 * The string is created using the sds formatter function sdscatvprintf().
 *
 * The passed context 'ctx' may be NULL if necessary, see the
 * RedisModule_CreateString() documentation for more info. */
RedisModuleString *RM_CreateStringPrintf(RedisModuleCtx *ctx, const char *fmt, ...) {
    sds s = sdsempty();

    va_list ap;
    va_start(ap, fmt);
    s = sdscatvprintf(s, fmt, ap);
    va_end(ap);

    RedisModuleString *o = createObject(OBJ_STRING, s);
    if (ctx != NULL) autoMemoryAdd(ctx,REDISMODULE_AM_STRING,o);

    return o;
}


/* Like RedisModule_CreateString(), but creates a string starting from a `long long`
 * integer instead of taking a buffer and its length.
 *
 * The returned string must be released with RedisModule_FreeString() or by
 * enabling automatic memory management.
 *
 * The passed context 'ctx' may be NULL if necessary, see the
 * RedisModule_CreateString() documentation for more info. */
RedisModuleString *RM_CreateStringFromLongLong(RedisModuleCtx *ctx, long long ll) {
    char buf[LONG_STR_SIZE];
    size_t len = ll2string(buf,sizeof(buf),ll);
    return RM_CreateString(ctx,buf,len);
}

/* Like RedisModule_CreateString(), but creates a string starting from a `unsigned long long`
 * integer instead of taking a buffer and its length.
 *
 * The returned string must be released with RedisModule_FreeString() or by
 * enabling automatic memory management.
 *
 * The passed context 'ctx' may be NULL if necessary, see the
 * RedisModule_CreateString() documentation for more info. */
RedisModuleString *RM_CreateStringFromULongLong(RedisModuleCtx *ctx, unsigned long long ull) {
    char buf[LONG_STR_SIZE];
    size_t len = ull2string(buf,sizeof(buf),ull);
    return RM_CreateString(ctx,buf,len);
}

/* Like RedisModule_CreateString(), but creates a string starting from a double
 * instead of taking a buffer and its length.
 *
 * The returned string must be released with RedisModule_FreeString() or by
 * enabling automatic memory management. */
RedisModuleString *RM_CreateStringFromDouble(RedisModuleCtx *ctx, double d) {
    char buf[MAX_D2STRING_CHARS];
    size_t len = d2string(buf,sizeof(buf),d);
    return RM_CreateString(ctx,buf,len);
}

/* Like RedisModule_CreateString(), but creates a string starting from a long
 * double.
 *
 * The returned string must be released with RedisModule_FreeString() or by
 * enabling automatic memory management.
 *
 * The passed context 'ctx' may be NULL if necessary, see the
 * RedisModule_CreateString() documentation for more info. */
RedisModuleString *RM_CreateStringFromLongDouble(RedisModuleCtx *ctx, long double ld, int humanfriendly) {
    char buf[MAX_LONG_DOUBLE_CHARS];
    size_t len = ld2string(buf,sizeof(buf),ld,
        (humanfriendly ? LD_STR_HUMAN : LD_STR_AUTO));
    return RM_CreateString(ctx,buf,len);
}

/* Like RedisModule_CreateString(), but creates a string starting from another
 * RedisModuleString.
 *
 * The returned string must be released with RedisModule_FreeString() or by
 * enabling automatic memory management.
 *
 * The passed context 'ctx' may be NULL if necessary, see the
 * RedisModule_CreateString() documentation for more info. */
RedisModuleString *RM_CreateStringFromString(RedisModuleCtx *ctx, const RedisModuleString *str) {
    RedisModuleString *o = dupStringObject(str);
    if (ctx != NULL) autoMemoryAdd(ctx,REDISMODULE_AM_STRING,o);
    return o;
}

/* Creates a string from a stream ID. The returned string must be released with
 * RedisModule_FreeString(), unless automatic memory is enabled.
 *
 * The passed context `ctx` may be NULL if necessary. See the
 * RedisModule_CreateString() documentation for more info. */
RedisModuleString *RM_CreateStringFromStreamID(RedisModuleCtx *ctx, const RedisModuleStreamID *id) {
    streamID streamid = {id->ms, id->seq};
    RedisModuleString *o = createObjectFromStreamID(&streamid);
    if (ctx != NULL) autoMemoryAdd(ctx, REDISMODULE_AM_STRING, o);
    return o;
}

/* Free a module string object obtained with one of the Redis modules API calls
 * that return new string objects.
 *
 * It is possible to call this function even when automatic memory management
 * is enabled. In that case the string will be released ASAP and removed
 * from the pool of string to release at the end.
 *
 * If the string was created with a NULL context 'ctx', it is also possible to
 * pass ctx as NULL when releasing the string (but passing a context will not
 * create any issue). Strings created with a context should be freed also passing
 * the context, so if you want to free a string out of context later, make sure
 * to create it using a NULL context. */
void RM_FreeString(RedisModuleCtx *ctx, RedisModuleString *str) {
    decrRefCount(str);
    if (ctx != NULL) autoMemoryFreed(ctx,REDISMODULE_AM_STRING,str);
}

/* Every call to this function, will make the string 'str' requiring
 * an additional call to RedisModule_FreeString() in order to really
 * free the string. Note that the automatic freeing of the string obtained
 * enabling modules automatic memory management counts for one
 * RedisModule_FreeString() call (it is just executed automatically).
 *
 * Normally you want to call this function when, at the same time
 * the following conditions are true:
 *
 * 1. You have automatic memory management enabled.
 * 2. You want to create string objects.
 * 3. Those string objects you create need to live *after* the callback
 *    function(for example a command implementation) creating them returns.
 *
 * Usually you want this in order to store the created string object
 * into your own data structure, for example when implementing a new data
 * type.
 *
 * Note that when memory management is turned off, you don't need
 * any call to RetainString() since creating a string will always result
 * into a string that lives after the callback function returns, if
 * no FreeString() call is performed.
 *
 * It is possible to call this function with a NULL context.
 *
 * When strings are going to be retained for an extended duration, it is good
 * practice to also call RedisModule_TrimStringAllocation() in order to
 * optimize memory usage.
 *
 * Threaded modules that reference retained strings from other threads *must*
 * explicitly trim the allocation as soon as the string is retained. Not doing
 * so may result with automatic trimming which is not thread safe. */
void RM_RetainString(RedisModuleCtx *ctx, RedisModuleString *str) {
    if (ctx == NULL || !autoMemoryFreed(ctx,REDISMODULE_AM_STRING,str)) {
        /* Increment the string reference counting only if we can't
         * just remove the object from the list of objects that should
         * be reclaimed. Why we do that, instead of just incrementing
         * the refcount in any case, and let the automatic FreeString()
         * call at the end to bring the refcount back at the desired
         * value? Because this way we ensure that the object refcount
         * value is 1 (instead of going to 2 to be dropped later to 1)
         * after the call to this function. This is needed for functions
         * like RedisModule_StringAppendBuffer() to work. */
        incrRefCount(str);
    }
}

/**
* This function can be used instead of RedisModule_RetainString().
* The main difference between the two is that this function will always
* succeed, whereas RedisModule_RetainString() may fail because of an
* assertion.
*
* The function returns a pointer to RedisModuleString, which is owned
* by the caller. It requires a call to RedisModule_FreeString() to free
* the string when automatic memory management is disabled for the context.
* When automatic memory management is enabled, you can either call
* RedisModule_FreeString() or let the automation free it.
*
* This function is more efficient than RedisModule_CreateStringFromString()
* because whenever possible, it avoids copying the underlying
* RedisModuleString. The disadvantage of using this function is that it
* might not be possible to use RedisModule_StringAppendBuffer() on the
* returned RedisModuleString.
*
* It is possible to call this function with a NULL context.
*
 * When strings are going to be held for an extended duration, it is good
 * practice to also call RedisModule_TrimStringAllocation() in order to
 * optimize memory usage.
 *
 * Threaded modules that reference held strings from other threads *must*
 * explicitly trim the allocation as soon as the string is held. Not doing
 * so may result with automatic trimming which is not thread safe. */
RedisModuleString* RM_HoldString(RedisModuleCtx *ctx, RedisModuleString *str) {
    if (str->refcount == OBJ_STATIC_REFCOUNT) {
        return RM_CreateStringFromString(ctx, str);
    }

    incrRefCount(str);
    if (ctx != NULL) {
        /*
         * Put the str in the auto memory management of the ctx.
         * It might already be there, in this case, the ref count will
         * be 2 and we will decrease the ref count twice and free the
         * object in the auto memory free function.
         *
         * Why we can not do the same trick of just remove the object
         * from the auto memory (like in RM_RetainString)?
         * This code shows the issue:
         *
         * RM_AutoMemory(ctx);
         * str1 = RM_CreateString(ctx, "test", 4);
         * str2 = RM_HoldString(ctx, str1);
         * RM_FreeString(str1);
         * RM_FreeString(str2);
         *
         * If after the RM_HoldString we would just remove the string from
         * the auto memory, this example will cause access to a freed memory
         * on 'RM_FreeString(str2);' because the String will be free
         * on 'RM_FreeString(str1);'.
         *
         * So it's safer to just increase the ref count
         * and add the String to auto memory again.
         *
         * The limitation is that it is not possible to use RedisModule_StringAppendBuffer
         * on the String.
         */
        autoMemoryAdd(ctx,REDISMODULE_AM_STRING,str);
    }
    return str;
}

/* Given a string module object, this function returns the string pointer
 * and length of the string. The returned pointer and length should only
 * be used for read only accesses and never modified. */
const char *RM_StringPtrLen(const RedisModuleString *str, size_t *len) {
    if (str == NULL) {
        const char *errmsg = "(NULL string reply referenced in module)";
        if (len) *len = strlen(errmsg);
        return errmsg;
    }
    if (len) *len = sdslen(str->ptr);
    return str->ptr;
}

/* --------------------------------------------------------------------------
 * Higher level string operations
 * ------------------------------------------------------------------------- */

/* Convert the string into a `long long` integer, storing it at `*ll`.
 * Returns REDISMODULE_OK on success. If the string can't be parsed
 * as a valid, strict `long long` (no spaces before/after), REDISMODULE_ERR
 * is returned. */
int RM_StringToLongLong(const RedisModuleString *str, long long *ll) {
    return string2ll(str->ptr,sdslen(str->ptr),ll) ? REDISMODULE_OK :
                                                     REDISMODULE_ERR;
}

/* Convert the string into a `unsigned long long` integer, storing it at `*ull`.
 * Returns REDISMODULE_OK on success. If the string can't be parsed
 * as a valid, strict `unsigned long long` (no spaces before/after), REDISMODULE_ERR
 * is returned. */
int RM_StringToULongLong(const RedisModuleString *str, unsigned long long *ull) {
    return string2ull(str->ptr,ull) ? REDISMODULE_OK : REDISMODULE_ERR;
}

/* Convert the string into a double, storing it at `*d`.
 * Returns REDISMODULE_OK on success or REDISMODULE_ERR if the string is
 * not a valid string representation of a double value. */
int RM_StringToDouble(const RedisModuleString *str, double *d) {
    int retval = getDoubleFromObject(str,d);
    return (retval == C_OK) ? REDISMODULE_OK : REDISMODULE_ERR;
}

/* Convert the string into a long double, storing it at `*ld`.
 * Returns REDISMODULE_OK on success or REDISMODULE_ERR if the string is
 * not a valid string representation of a double value. */
int RM_StringToLongDouble(const RedisModuleString *str, long double *ld) {
    int retval = string2ld(str->ptr,sdslen(str->ptr),ld);
    return retval ? REDISMODULE_OK : REDISMODULE_ERR;
}

/* Convert the string into a stream ID, storing it at `*id`.
 * Returns REDISMODULE_OK on success and returns REDISMODULE_ERR if the string
 * is not a valid string representation of a stream ID. The special IDs "+" and
 * "-" are allowed.
 */
int RM_StringToStreamID(const RedisModuleString *str, RedisModuleStreamID *id) {
    streamID streamid;
    if (streamParseID(str, &streamid) == C_OK) {
        id->ms = streamid.ms;
        id->seq = streamid.seq;
        return REDISMODULE_OK;
    } else {
        return REDISMODULE_ERR;
    }
}

/* Compare two string objects, returning -1, 0 or 1 respectively if
 * a < b, a == b, a > b. Strings are compared byte by byte as two
 * binary blobs without any encoding care / collation attempt. */
int RM_StringCompare(const RedisModuleString *a, const RedisModuleString *b) {
    return compareStringObjects(a,b);
}

/* Return the (possibly modified in encoding) input 'str' object if
 * the string is unshared, otherwise NULL is returned. */
RedisModuleString *moduleAssertUnsharedString(RedisModuleString *str) {
    if (str->refcount != 1) {
        serverLog(LL_WARNING,
            "Module attempted to use an in-place string modify operation "
            "with a string referenced multiple times. Please check the code "
            "for API usage correctness.");
        return NULL;
    }
    if (str->encoding == OBJ_ENCODING_EMBSTR) {
        /* Note: here we "leak" the additional allocation that was
         * used in order to store the embedded string in the object. */
        str->ptr = sdsnewlen(str->ptr,sdslen(str->ptr));
        str->encoding = OBJ_ENCODING_RAW;
    } else if (str->encoding == OBJ_ENCODING_INT) {
        /* Convert the string from integer to raw encoding. */
        str->ptr = sdsfromlonglong((long)str->ptr);
        str->encoding = OBJ_ENCODING_RAW;
    }
    return str;
}

/* Append the specified buffer to the string 'str'. The string must be a
 * string created by the user that is referenced only a single time, otherwise
 * REDISMODULE_ERR is returned and the operation is not performed. */
int RM_StringAppendBuffer(RedisModuleCtx *ctx, RedisModuleString *str, const char *buf, size_t len) {
    UNUSED(ctx);
    str = moduleAssertUnsharedString(str);
    if (str == NULL) return REDISMODULE_ERR;
    str->ptr = sdscatlen(str->ptr,buf,len);
    return REDISMODULE_OK;
}

/* Trim possible excess memory allocated for a RedisModuleString.
 *
 * Sometimes a RedisModuleString may have more memory allocated for
 * it than required, typically for argv arguments that were constructed
 * from network buffers. This function optimizes such strings by reallocating
 * their memory, which is useful for strings that are not short lived but
 * retained for an extended duration.
 *
 * This operation is *not thread safe* and should only be called when
 * no concurrent access to the string is guaranteed. Using it for an argv
 * string in a module command before the string is potentially available
 * to other threads is generally safe.
 *
 * Currently, Redis may also automatically trim retained strings when a
 * module command returns. However, doing this explicitly should still be
 * a preferred option:
 *
 * 1. Future versions of Redis may abandon auto-trimming.
 * 2. Auto-trimming as currently implemented is *not thread safe*.
 *    A background thread manipulating a recently retained string may end up
 *    in a race condition with the auto-trim, which could result with
 *    data corruption.
 */
void RM_TrimStringAllocation(RedisModuleString *str) {
    if (!str) return;
    trimStringObjectIfNeeded(str, 1);
}

/* --------------------------------------------------------------------------
 * ## Reply APIs
 *
 * These functions are used for sending replies to the client.
 *
 * Most functions always return REDISMODULE_OK so you can use it with
 * 'return' in order to return from the command implementation with:
 *
 *     if (... some condition ...)
 *         return RedisModule_ReplyWithLongLong(ctx,mycount);
 *
 * ### Reply with collection functions
 *
 * After starting a collection reply, the module must make calls to other
 * `ReplyWith*` style functions in order to emit the elements of the collection.
 * Collection types include: Array, Map, Set and Attribute.
 *
 * When producing collections with a number of elements that is not known
 * beforehand, the function can be called with a special flag
 * REDISMODULE_POSTPONED_LEN (REDISMODULE_POSTPONED_ARRAY_LEN in the past),
 * and the actual number of elements can be later set with RM_ReplySet*Length()
 * call (which will set the latest "open" count if there are multiple ones).
 * -------------------------------------------------------------------------- */

/* Send an error about the number of arguments given to the command,
 * citing the command name in the error message. Returns REDISMODULE_OK.
 *
 * Example:
 *
 *     if (argc != 3) return RedisModule_WrongArity(ctx);
 */
int RM_WrongArity(RedisModuleCtx *ctx) {
    addReplyErrorArity(ctx->client);
    return REDISMODULE_OK;
}

/* Return the client object the `RM_Reply*` functions should target.
 * Normally this is just `ctx->client`, that is the client that called
 * the module command, however in the case of thread safe contexts there
 * is no directly associated client (since it would not be safe to access
 * the client from a thread), so instead the blocked client object referenced
 * in the thread safe context, has a fake client that we just use to accumulate
 * the replies. Later, when the client is unblocked, the accumulated replies
 * are appended to the actual client.
 *
 * The function returns the client pointer depending on the context, or
 * NULL if there is no potential client. This happens when we are in the
 * context of a thread safe context that was not initialized with a blocked
 * client object. Other contexts without associated clients are the ones
 * initialized to run the timers callbacks. */
client *moduleGetReplyClient(RedisModuleCtx *ctx) {
    if (ctx->flags & REDISMODULE_CTX_THREAD_SAFE) {
        if (ctx->blocked_client)
            return ctx->blocked_client->reply_client;
        else
            return NULL;
    } else {
        /* If this is a non thread safe context, just return the client
         * that is running the command if any. This may be NULL as well
         * in the case of contexts that are not executed with associated
         * clients, like timer contexts. */
        return ctx->client;
    }
}

/* Send an integer reply to the client, with the specified `long long` value.
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithLongLong(RedisModuleCtx *ctx, long long ll) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyLongLong(c,ll);
    return REDISMODULE_OK;
}

/* Reply with the error 'err'.
 *
 * Note that 'err' must contain all the error, including
 * the initial error code. The function only provides the initial "-", so
 * the usage is, for example:
 *
 *     RedisModule_ReplyWithError(ctx,"ERR Wrong Type");
 *
 * and not just:
 *
 *     RedisModule_ReplyWithError(ctx,"Wrong Type");
 *
 * The function always returns REDISMODULE_OK.
 */
int RM_ReplyWithError(RedisModuleCtx *ctx, const char *err) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyErrorFormat(c,"-%s",err);
    return REDISMODULE_OK;
}

/* Reply with a simple string (`+... \r\n` in RESP protocol). This replies
 * are suitable only when sending a small non-binary string with small
 * overhead, like "OK" or similar replies.
 *
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithSimpleString(RedisModuleCtx *ctx, const char *msg) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyProto(c,"+",1);
    addReplyProto(c,msg,strlen(msg));
    addReplyProto(c,"\r\n",2);
    return REDISMODULE_OK;
}

#define COLLECTION_REPLY_ARRAY      1
#define COLLECTION_REPLY_MAP        2
#define COLLECTION_REPLY_SET        3
#define COLLECTION_REPLY_ATTRIBUTE  4

int moduleReplyWithCollection(RedisModuleCtx *ctx, long len, int type) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    if (len == REDISMODULE_POSTPONED_LEN) {
        ctx->postponed_arrays = zrealloc(ctx->postponed_arrays,sizeof(void*)*
                (ctx->postponed_arrays_count+1));
        ctx->postponed_arrays[ctx->postponed_arrays_count] =
            addReplyDeferredLen(c);
        ctx->postponed_arrays_count++;
    } else if (len == 0) {
        switch (type) {
        case COLLECTION_REPLY_ARRAY:
            addReply(c, shared.emptyarray);
            break;
        case COLLECTION_REPLY_MAP:
            addReply(c, shared.emptymap[c->resp]);
            break;
        case COLLECTION_REPLY_SET:
            addReply(c, shared.emptyset[c->resp]);
            break;
        case COLLECTION_REPLY_ATTRIBUTE:
            addReplyAttributeLen(c,len);
            break;
        default:
            serverPanic("Invalid module empty reply type %d", type);        }
    } else {
        switch (type) {
        case COLLECTION_REPLY_ARRAY:
            addReplyArrayLen(c,len);
            break;
        case COLLECTION_REPLY_MAP:
            addReplyMapLen(c,len);
            break;
        case COLLECTION_REPLY_SET:
            addReplySetLen(c,len);
            break;
        case COLLECTION_REPLY_ATTRIBUTE:
            addReplyAttributeLen(c,len);
            break;
        default:
            serverPanic("Invalid module reply type %d", type);
        }
    }
    return REDISMODULE_OK;
}

/* Reply with an array type of 'len' elements.
 *
 * After starting an array reply, the module must make `len` calls to other
 * `ReplyWith*` style functions in order to emit the elements of the array.
 * See Reply APIs section for more details.
 *
 * Use RM_ReplySetArrayLength() to set deferred length.
 *
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithArray(RedisModuleCtx *ctx, long len) {
    return moduleReplyWithCollection(ctx, len, COLLECTION_REPLY_ARRAY);
}

/* Reply with a RESP3 Map type of 'len' pairs.
 * Visit https://github.com/antirez/RESP3/blob/master/spec.md for more info about RESP3.
 *
 * After starting a map reply, the module must make `len*2` calls to other
 * `ReplyWith*` style functions in order to emit the elements of the map.
 * See Reply APIs section for more details.
 *
 * If the connected client is using RESP2, the reply will be converted to a flat
 * array.
 * 
 * Use RM_ReplySetMapLength() to set deferred length.
 * 
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithMap(RedisModuleCtx *ctx, long len) {
    return moduleReplyWithCollection(ctx, len, COLLECTION_REPLY_MAP);
}

/* Reply with a RESP3 Set type of 'len' elements.
 * Visit https://github.com/antirez/RESP3/blob/master/spec.md for more info about RESP3.
 *
 * After starting a set reply, the module must make `len` calls to other
 * `ReplyWith*` style functions in order to emit the elements of the set.
 * See Reply APIs section for more details.
 *
 * If the connected client is using RESP2, the reply will be converted to an
 * array type.
 *
 * Use RM_ReplySetSetLength() to set deferred length.
 * 
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithSet(RedisModuleCtx *ctx, long len) {
    return moduleReplyWithCollection(ctx, len, COLLECTION_REPLY_SET);
}


/* Add attributes (metadata) to the reply. Should be done before adding the
 * actual reply. see https://github.com/antirez/RESP3/blob/master/spec.md#attribute-type
 *
 * After starting an attribute's reply, the module must make `len*2` calls to other
 * `ReplyWith*` style functions in order to emit the elements of the attribute map.
 * See Reply APIs section for more details.
 *
 * Use RM_ReplySetAttributeLength() to set deferred length.
 * 
 * Not supported by RESP2 and will return REDISMODULE_ERR, otherwise
 * the function always returns REDISMODULE_OK. */
int RM_ReplyWithAttribute(RedisModuleCtx *ctx, long len) {
    if (ctx->client->resp == 2) return REDISMODULE_ERR;
 
    return moduleReplyWithCollection(ctx, len, COLLECTION_REPLY_ATTRIBUTE);
}

/* Reply to the client with a null array, simply null in RESP3,
 * null array in RESP2.
 *
 * Note: In RESP3 there's no difference between Null reply and
 * NullArray reply, so to prevent ambiguity it's better to avoid
 * using this API and use RedisModule_ReplyWithNull instead.
 *
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithNullArray(RedisModuleCtx *ctx) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyNullArray(c);
    return REDISMODULE_OK;
}

/* Reply to the client with an empty array.
 *
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithEmptyArray(RedisModuleCtx *ctx) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReply(c,shared.emptyarray);
    return REDISMODULE_OK;
}

void moduleReplySetCollectionLength(RedisModuleCtx *ctx, long len, int type) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return;
    if (ctx->postponed_arrays_count == 0) {
        serverLog(LL_WARNING,
            "API misuse detected in module %s: "
            "RedisModule_ReplySet*Length() called without previous "
            "RedisModule_ReplyWith*(ctx,REDISMODULE_POSTPONED_LEN) "
            "call.", ctx->module->name);
            return;
    }
    ctx->postponed_arrays_count--;
    switch(type) {
    case COLLECTION_REPLY_ARRAY:
        setDeferredArrayLen(c,ctx->postponed_arrays[ctx->postponed_arrays_count],len);
        break;
    case COLLECTION_REPLY_MAP:
        setDeferredMapLen(c,ctx->postponed_arrays[ctx->postponed_arrays_count],len);
        break;
    case COLLECTION_REPLY_SET:
        setDeferredSetLen(c,ctx->postponed_arrays[ctx->postponed_arrays_count],len);
        break;
    case COLLECTION_REPLY_ATTRIBUTE:
        setDeferredAttributeLen(c,ctx->postponed_arrays[ctx->postponed_arrays_count],len);
        break;
    default:
        serverPanic("Invalid module reply type %d", type);
    }
    if (ctx->postponed_arrays_count == 0) {
        zfree(ctx->postponed_arrays);
        ctx->postponed_arrays = NULL;
    }
}

/* When RedisModule_ReplyWithArray() is used with the argument
 * REDISMODULE_POSTPONED_LEN, because we don't know beforehand the number
 * of items we are going to output as elements of the array, this function
 * will take care to set the array length.
 *
 * Since it is possible to have multiple array replies pending with unknown
 * length, this function guarantees to always set the latest array length
 * that was created in a postponed way.
 *
 * For example in order to output an array like [1,[10,20,30]] we
 * could write:
 *
 *      RedisModule_ReplyWithArray(ctx,REDISMODULE_POSTPONED_LEN);
 *      RedisModule_ReplyWithLongLong(ctx,1);
 *      RedisModule_ReplyWithArray(ctx,REDISMODULE_POSTPONED_LEN);
 *      RedisModule_ReplyWithLongLong(ctx,10);
 *      RedisModule_ReplyWithLongLong(ctx,20);
 *      RedisModule_ReplyWithLongLong(ctx,30);
 *      RedisModule_ReplySetArrayLength(ctx,3); // Set len of 10,20,30 array.
 *      RedisModule_ReplySetArrayLength(ctx,2); // Set len of top array
 *
 * Note that in the above example there is no reason to postpone the array
 * length, since we produce a fixed number of elements, but in the practice
 * the code may use an iterator or other ways of creating the output so
 * that is not easy to calculate in advance the number of elements.
 */
void RM_ReplySetArrayLength(RedisModuleCtx *ctx, long len) {
    moduleReplySetCollectionLength(ctx, len, COLLECTION_REPLY_ARRAY);
}

/* Very similar to RedisModule_ReplySetArrayLength except `len` should
 * exactly half of the number of `ReplyWith*` functions called in the
 * context of the map.
 * Visit https://github.com/antirez/RESP3/blob/master/spec.md for more info about RESP3. */
void RM_ReplySetMapLength(RedisModuleCtx *ctx, long len) {
    moduleReplySetCollectionLength(ctx, len, COLLECTION_REPLY_MAP);
}

/* Very similar to RedisModule_ReplySetArrayLength
 * Visit https://github.com/antirez/RESP3/blob/master/spec.md for more info about RESP3. */
void RM_ReplySetSetLength(RedisModuleCtx *ctx, long len) {
    moduleReplySetCollectionLength(ctx, len, COLLECTION_REPLY_SET);
}

/* Very similar to RedisModule_ReplySetMapLength
 * Visit https://github.com/antirez/RESP3/blob/master/spec.md for more info about RESP3.
 *
 * Must not be called if RM_ReplyWithAttribute returned an error. */
void RM_ReplySetAttributeLength(RedisModuleCtx *ctx, long len) {
    if (ctx->client->resp == 2) return;
    moduleReplySetCollectionLength(ctx, len, COLLECTION_REPLY_ATTRIBUTE);
}

/* Reply with a bulk string, taking in input a C buffer pointer and length.
 *
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithStringBuffer(RedisModuleCtx *ctx, const char *buf, size_t len) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyBulkCBuffer(c,(char*)buf,len);
    return REDISMODULE_OK;
}

/* Reply with a bulk string, taking in input a C buffer pointer that is
 * assumed to be null-terminated.
 *
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithCString(RedisModuleCtx *ctx, const char *buf) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyBulkCString(c,(char*)buf);
    return REDISMODULE_OK;
}

/* Reply with a bulk string, taking in input a RedisModuleString object.
 *
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithString(RedisModuleCtx *ctx, RedisModuleString *str) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyBulk(c,str);
    return REDISMODULE_OK;
}

/* Reply with an empty string.
 *
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithEmptyString(RedisModuleCtx *ctx) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReply(c,shared.emptybulk);
    return REDISMODULE_OK;
}

/* Reply with a binary safe string, which should not be escaped or filtered
 * taking in input a C buffer pointer, length and a 3 character type/extension.
 *
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithVerbatimStringType(RedisModuleCtx *ctx, const char *buf, size_t len, const char *ext) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyVerbatim(c, buf, len, ext);
    return REDISMODULE_OK;
}

/* Reply with a binary safe string, which should not be escaped or filtered
 * taking in input a C buffer pointer and length.
 *
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithVerbatimString(RedisModuleCtx *ctx, const char *buf, size_t len) {
	return RM_ReplyWithVerbatimStringType(ctx, buf, len, "txt");
}

/* Reply to the client with a NULL.
 *
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithNull(RedisModuleCtx *ctx) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyNull(c);
    return REDISMODULE_OK;
}

/* Reply with a RESP3 Boolean type.
 * Visit https://github.com/antirez/RESP3/blob/master/spec.md for more info about RESP3.
 *
 * In RESP3, this is boolean type
 * In RESP2, it's a string response of "1" and "0" for true and false respectively.
 *
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithBool(RedisModuleCtx *ctx, int b) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyBool(c,b);
    return REDISMODULE_OK;
}

/* Reply exactly what a Redis command returned us with RedisModule_Call().
 * This function is useful when we use RedisModule_Call() in order to
 * execute some command, as we want to reply to the client exactly the
 * same reply we obtained by the command.
 *
 * Return:
 * - REDISMODULE_OK on success.
 * - REDISMODULE_ERR if the given reply is in RESP3 format but the client expects RESP2.
 *   In case of an error, it's the module writer responsibility to translate the reply
 *   to RESP2 (or handle it differently by returning an error). Notice that for
 *   module writer convenience, it is possible to pass `0` as a parameter to the fmt
 *   argument of `RM_Call` so that the RedisModuleCallReply will return in the same
 *   protocol (RESP2 or RESP3) as set in the current client's context. */
int RM_ReplyWithCallReply(RedisModuleCtx *ctx, RedisModuleCallReply *reply) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    if (c->resp == 2 && callReplyIsResp3(reply)) {
        /* The reply is in RESP3 format and the client is RESP2,
         * so it isn't possible to send this reply to the client. */
        return REDISMODULE_ERR;
    }
    size_t proto_len;
    const char *proto = callReplyGetProto(reply, &proto_len);
    addReplyProto(c, proto, proto_len);
    /* Propagate the error list from that reply to the other client, to do some
     * post error reply handling, like statistics.
     * Note that if the original reply had an array with errors, and the module
     * replied with just a portion of the original reply, and not the entire
     * reply, the errors are currently not propagated and the errors stats
     * will not get propagated. */
    list *errors = callReplyDeferredErrorList(reply);
    if (errors)
        deferredAfterErrorReply(c, errors);
    return REDISMODULE_OK;
}

/* Reply with a RESP3 Double type.
 * Visit https://github.com/antirez/RESP3/blob/master/spec.md for more info about RESP3.
 *
 * Send a string reply obtained converting the double 'd' into a bulk string.
 * This function is basically equivalent to converting a double into
 * a string into a C buffer, and then calling the function
 * RedisModule_ReplyWithStringBuffer() with the buffer and length.
 *
 * In RESP3 the string is tagged as a double, while in RESP2 it's just a plain string 
 * that the user will have to parse.
 *
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithDouble(RedisModuleCtx *ctx, double d) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyDouble(c,d);
    return REDISMODULE_OK;
}

/* Reply with a RESP3 BigNumber type.
 * Visit https://github.com/antirez/RESP3/blob/master/spec.md for more info about RESP3.
 *
 * In RESP3, this is a string of length `len` that is tagged as a BigNumber, 
 * however, it's up to the caller to ensure that it's a valid BigNumber.
 * In RESP2, this is just a plain bulk string response.
 *
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithBigNumber(RedisModuleCtx *ctx, const char *bignum, size_t len) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyBigNum(c, bignum, len);
    return REDISMODULE_OK;
}

/* Send a string reply obtained converting the long double 'ld' into a bulk
 * string. This function is basically equivalent to converting a long double
 * into a string into a C buffer, and then calling the function
 * RedisModule_ReplyWithStringBuffer() with the buffer and length.
 * The double string uses human readable formatting (see
 * `addReplyHumanLongDouble` in networking.c).
 *
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithLongDouble(RedisModuleCtx *ctx, long double ld) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyHumanLongDouble(c, ld);
    return REDISMODULE_OK;
}

/* --------------------------------------------------------------------------
 * ## Commands replication API
 * -------------------------------------------------------------------------- */

/* Replicate the specified command and arguments to slaves and AOF, as effect
 * of execution of the calling command implementation.
 *
 * The replicated commands are always wrapped into the MULTI/EXEC that
 * contains all the commands replicated in a given module command
 * execution. However the commands replicated with RedisModule_Call()
 * are the first items, the ones replicated with RedisModule_Replicate()
 * will all follow before the EXEC.
 *
 * Modules should try to use one interface or the other.
 *
 * This command follows exactly the same interface of RedisModule_Call(),
 * so a set of format specifiers must be passed, followed by arguments
 * matching the provided format specifiers.
 *
 * Please refer to RedisModule_Call() for more information.
 *
 * Using the special "A" and "R" modifiers, the caller can exclude either
 * the AOF or the replicas from the propagation of the specified command.
 * Otherwise, by default, the command will be propagated in both channels.
 *
 * #### Note about calling this function from a thread safe context:
 *
 * Normally when you call this function from the callback implementing a
 * module command, or any other callback provided by the Redis Module API,
 * Redis will accumulate all the calls to this function in the context of
 * the callback, and will propagate all the commands wrapped in a MULTI/EXEC
 * transaction. However when calling this function from a threaded safe context
 * that can live an undefined amount of time, and can be locked/unlocked in
 * at will, the behavior is different: MULTI/EXEC wrapper is not emitted
 * and the command specified is inserted in the AOF and replication stream
 * immediately.
 *
 * #### Return value
 *
 * The command returns REDISMODULE_ERR if the format specifiers are invalid
 * or the command name does not belong to a known command. */
int RM_Replicate(RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...) {
    struct redisCommand *cmd;
    robj **argv = NULL;
    int argc = 0, flags = 0, j;
    va_list ap;

    cmd = lookupCommandByCString((char*)cmdname);
    if (!cmd) return REDISMODULE_ERR;

    /* Create the client and dispatch the command. */
    va_start(ap, fmt);
    argv = moduleCreateArgvFromUserFormat(cmdname,fmt,&argc,&flags,ap);
    va_end(ap);
    if (argv == NULL) return REDISMODULE_ERR;

    /* Select the propagation target. Usually is AOF + replicas, however
     * the caller can exclude one or the other using the "A" or "R"
     * modifiers. */
    int target = 0;
    if (!(flags & REDISMODULE_ARGV_NO_AOF)) target |= PROPAGATE_AOF;
    if (!(flags & REDISMODULE_ARGV_NO_REPLICAS)) target |= PROPAGATE_REPL;

    alsoPropagate(ctx->client->db->id,argv,argc,target);

    /* Release the argv. */
    for (j = 0; j < argc; j++) decrRefCount(argv[j]);
    zfree(argv);
    server.dirty++;
    return REDISMODULE_OK;
}

/* This function will replicate the command exactly as it was invoked
 * by the client. Note that this function will not wrap the command into
 * a MULTI/EXEC stanza, so it should not be mixed with other replication
 * commands.
 *
 * Basically this form of replication is useful when you want to propagate
 * the command to the slaves and AOF file exactly as it was called, since
 * the command can just be re-executed to deterministically re-create the
 * new state starting from the old one.
 *
 * The function always returns REDISMODULE_OK. */
int RM_ReplicateVerbatim(RedisModuleCtx *ctx) {
    alsoPropagate(ctx->client->db->id,
        ctx->client->argv,ctx->client->argc,
        PROPAGATE_AOF|PROPAGATE_REPL);
    server.dirty++;
    return REDISMODULE_OK;
}

/* --------------------------------------------------------------------------
 * ## DB and Key APIs -- Generic API
 * -------------------------------------------------------------------------- */

/* Return the ID of the current client calling the currently active module
 * command. The returned ID has a few guarantees:
 *
 * 1. The ID is different for each different client, so if the same client
 *    executes a module command multiple times, it can be recognized as
 *    having the same ID, otherwise the ID will be different.
 * 2. The ID increases monotonically. Clients connecting to the server later
 *    are guaranteed to get IDs greater than any past ID previously seen.
 *
 * Valid IDs are from 1 to 2^64 - 1. If 0 is returned it means there is no way
 * to fetch the ID in the context the function was currently called.
 *
 * After obtaining the ID, it is possible to check if the command execution
 * is actually happening in the context of AOF loading, using this macro:
 *
 *      if (RedisModule_IsAOFClient(RedisModule_GetClientId(ctx)) {
 *          // Handle it differently.
 *      }
 */
unsigned long long RM_GetClientId(RedisModuleCtx *ctx) {
    if (ctx->client == NULL) return 0;
    return ctx->client->id;
}

/* Return the ACL user name used by the client with the specified client ID.
 * Client ID can be obtained with RM_GetClientId() API. If the client does not
 * exist, NULL is returned and errno is set to ENOENT. If the client isn't
 * using an ACL user, NULL is returned and errno is set to ENOTSUP */
RedisModuleString *RM_GetClientUserNameById(RedisModuleCtx *ctx, uint64_t id) {
    client *client = lookupClientByID(id);
    if (client == NULL) {
        errno = ENOENT;
        return NULL;
    }
    
    if (client->user == NULL) {
        errno = ENOTSUP;
        return NULL;
    }

    sds name = sdsnew(client->user->name);
    robj *str = createObject(OBJ_STRING, name);
    autoMemoryAdd(ctx, REDISMODULE_AM_STRING, str);
    return str;
}

/* This is a helper for RM_GetClientInfoById() and other functions: given
 * a client, it populates the client info structure with the appropriate
 * fields depending on the version provided. If the version is not valid
 * then REDISMODULE_ERR is returned. Otherwise the function returns
 * REDISMODULE_OK and the structure pointed by 'ci' gets populated. */

int modulePopulateClientInfoStructure(void *ci, client *client, int structver) {
    if (structver != 1) return REDISMODULE_ERR;

    RedisModuleClientInfoV1 *ci1 = ci;
    memset(ci1,0,sizeof(*ci1));
    ci1->version = structver;
    if (client->flags & CLIENT_MULTI)
        ci1->flags |= REDISMODULE_CLIENTINFO_FLAG_MULTI;
    if (client->flags & CLIENT_PUBSUB)
        ci1->flags |= REDISMODULE_CLIENTINFO_FLAG_PUBSUB;
    if (client->flags & CLIENT_UNIX_SOCKET)
        ci1->flags |= REDISMODULE_CLIENTINFO_FLAG_UNIXSOCKET;
    if (client->flags & CLIENT_TRACKING)
        ci1->flags |= REDISMODULE_CLIENTINFO_FLAG_TRACKING;
    if (client->flags & CLIENT_BLOCKED)
        ci1->flags |= REDISMODULE_CLIENTINFO_FLAG_BLOCKED;
    if (client->conn->type == connectionTypeTls())
        ci1->flags |= REDISMODULE_CLIENTINFO_FLAG_SSL;

    int port;
    connAddrPeerName(client->conn,ci1->addr,sizeof(ci1->addr),&port);
    ci1->port = port;
    ci1->db = client->db->id;
    ci1->id = client->id;
    return REDISMODULE_OK;
}

/* This is a helper for moduleFireServerEvent() and other functions:
 * It populates the replication info structure with the appropriate
 * fields depending on the version provided. If the version is not valid
 * then REDISMODULE_ERR is returned. Otherwise the function returns
 * REDISMODULE_OK and the structure pointed by 'ri' gets populated. */
int modulePopulateReplicationInfoStructure(void *ri, int structver) {
    if (structver != 1) return REDISMODULE_ERR;

    RedisModuleReplicationInfoV1 *ri1 = ri;
    memset(ri1,0,sizeof(*ri1));
    ri1->version = structver;
    ri1->master = server.masterhost==NULL;
    ri1->masterhost = server.masterhost? server.masterhost: "";
    ri1->masterport = server.masterport;
    ri1->replid1 = server.replid;
    ri1->replid2 = server.replid2;
    ri1->repl1_offset = server.master_repl_offset;
    ri1->repl2_offset = server.second_replid_offset;
    return REDISMODULE_OK;
}

/* Return information about the client with the specified ID (that was
 * previously obtained via the RedisModule_GetClientId() API). If the
 * client exists, REDISMODULE_OK is returned, otherwise REDISMODULE_ERR
 * is returned.
 *
 * When the client exist and the `ci` pointer is not NULL, but points to
 * a structure of type RedisModuleClientInfoV1, previously initialized with
 * the correct REDISMODULE_CLIENTINFO_INITIALIZER_V1, the structure is populated
 * with the following fields:
 *
 *      uint64_t flags;         // REDISMODULE_CLIENTINFO_FLAG_*
 *      uint64_t id;            // Client ID
 *      char addr[46];          // IPv4 or IPv6 address.
 *      uint16_t port;          // TCP port.
 *      uint16_t db;            // Selected DB.
 *
 * Note: the client ID is useless in the context of this call, since we
 *       already know, however the same structure could be used in other
 *       contexts where we don't know the client ID, yet the same structure
 *       is returned.
 *
 * With flags having the following meaning:
 *
 *     REDISMODULE_CLIENTINFO_FLAG_SSL          Client using SSL connection.
 *     REDISMODULE_CLIENTINFO_FLAG_PUBSUB       Client in Pub/Sub mode.
 *     REDISMODULE_CLIENTINFO_FLAG_BLOCKED      Client blocked in command.
 *     REDISMODULE_CLIENTINFO_FLAG_TRACKING     Client with keys tracking on.
 *     REDISMODULE_CLIENTINFO_FLAG_UNIXSOCKET   Client using unix domain socket.
 *     REDISMODULE_CLIENTINFO_FLAG_MULTI        Client in MULTI state.
 *
 * However passing NULL is a way to just check if the client exists in case
 * we are not interested in any additional information.
 *
 * This is the correct usage when we want the client info structure
 * returned:
 *
 *      RedisModuleClientInfo ci = REDISMODULE_CLIENTINFO_INITIALIZER;
 *      int retval = RedisModule_GetClientInfoById(&ci,client_id);
 *      if (retval == REDISMODULE_OK) {
 *          printf("Address: %s\n", ci.addr);
 *      }
 */
int RM_GetClientInfoById(void *ci, uint64_t id) {
    client *client = lookupClientByID(id);
    if (client == NULL) return REDISMODULE_ERR;
    if (ci == NULL) return REDISMODULE_OK;

    /* Fill the info structure if passed. */
    uint64_t structver = ((uint64_t*)ci)[0];
    return modulePopulateClientInfoStructure(ci,client,structver);
}

/* Returns the name of the client connection with the given ID.
 *
 * If the client ID does not exist or if the client has no name associated with
 * it, NULL is returned. */
RedisModuleString *RM_GetClientNameById(RedisModuleCtx *ctx, uint64_t id) {
    client *client = lookupClientByID(id);
    if (client == NULL || client->name == NULL) return NULL;
    robj *name = client->name;
    incrRefCount(name);
    autoMemoryAdd(ctx, REDISMODULE_AM_STRING, name);
    return name;
}

/* Sets the name of the client with the given ID. This is equivalent to the client calling
 * `CLIENT SETNAME name`.
 *
 * Returns REDISMODULE_OK on success. On failure, REDISMODULE_ERR is returned
 * and errno is set as follows:
 *
 * - ENOENT if the client does not exist
 * - EINVAL if the name contains invalid characters */
int RM_SetClientNameById(uint64_t id, RedisModuleString *name) {
    client *client = lookupClientByID(id);
    if (client == NULL) {
        errno = ENOENT;
        return REDISMODULE_ERR;
    }
    if (clientSetName(client, name, NULL) == C_ERR) {
        errno = EINVAL;
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}

/* Publish a message to subscribers (see PUBLISH command). */
int RM_PublishMessage(RedisModuleCtx *ctx, RedisModuleString *channel, RedisModuleString *message) {
    UNUSED(ctx);
    return pubsubPublishMessageAndPropagateToCluster(channel, message, 0);
}

/* Publish a message to shard-subscribers (see SPUBLISH command). */
int RM_PublishMessageShard(RedisModuleCtx *ctx, RedisModuleString *channel, RedisModuleString *message) {
    UNUSED(ctx);
    return pubsubPublishMessageAndPropagateToCluster(channel, message, 1);
}

/* Return the currently selected DB. */
int RM_GetSelectedDb(RedisModuleCtx *ctx) {
    return ctx->client->db->id;
}


/* Return the current context's flags. The flags provide information on the
 * current request context (whether the client is a Lua script or in a MULTI),
 * and about the Redis instance in general, i.e replication and persistence.
 *
 * It is possible to call this function even with a NULL context, however
 * in this case the following flags will not be reported:
 *
 *  * LUA, MULTI, REPLICATED, DIRTY (see below for more info).
 *
 * Available flags and their meaning:
 *
 *  * REDISMODULE_CTX_FLAGS_LUA: The command is running in a Lua script
 *
 *  * REDISMODULE_CTX_FLAGS_MULTI: The command is running inside a transaction
 *
 *  * REDISMODULE_CTX_FLAGS_REPLICATED: The command was sent over the replication
 *    link by the MASTER
 *
 *  * REDISMODULE_CTX_FLAGS_MASTER: The Redis instance is a master
 *
 *  * REDISMODULE_CTX_FLAGS_SLAVE: The Redis instance is a slave
 *
 *  * REDISMODULE_CTX_FLAGS_READONLY: The Redis instance is read-only
 *
 *  * REDISMODULE_CTX_FLAGS_CLUSTER: The Redis instance is in cluster mode
 *
 *  * REDISMODULE_CTX_FLAGS_AOF: The Redis instance has AOF enabled
 *
 *  * REDISMODULE_CTX_FLAGS_RDB: The instance has RDB enabled
 *
 *  * REDISMODULE_CTX_FLAGS_MAXMEMORY:  The instance has Maxmemory set
 *
 *  * REDISMODULE_CTX_FLAGS_EVICT:  Maxmemory is set and has an eviction
 *    policy that may delete keys
 *
 *  * REDISMODULE_CTX_FLAGS_OOM: Redis is out of memory according to the
 *    maxmemory setting.
 *
 *  * REDISMODULE_CTX_FLAGS_OOM_WARNING: Less than 25% of memory remains before
 *                                       reaching the maxmemory level.
 *
 *  * REDISMODULE_CTX_FLAGS_LOADING: Server is loading RDB/AOF
 *
 *  * REDISMODULE_CTX_FLAGS_REPLICA_IS_STALE: No active link with the master.
 *
 *  * REDISMODULE_CTX_FLAGS_REPLICA_IS_CONNECTING: The replica is trying to
 *                                                 connect with the master.
 *
 *  * REDISMODULE_CTX_FLAGS_REPLICA_IS_TRANSFERRING: Master -> Replica RDB
 *                                                   transfer is in progress.
 *
 *  * REDISMODULE_CTX_FLAGS_REPLICA_IS_ONLINE: The replica has an active link
 *                                             with its master. This is the
 *                                             contrary of STALE state.
 *
 *  * REDISMODULE_CTX_FLAGS_ACTIVE_CHILD: There is currently some background
 *                                        process active (RDB, AUX or module).
 *
 *  * REDISMODULE_CTX_FLAGS_MULTI_DIRTY: The next EXEC will fail due to dirty
 *                                       CAS (touched keys).
 *
 *  * REDISMODULE_CTX_FLAGS_IS_CHILD: Redis is currently running inside
 *                                    background child process.
 *
 *  * REDISMODULE_CTX_FLAGS_RESP3: Indicate the that client attached to this
 *                                 context is using RESP3.
 *
 *  * REDISMODULE_CTX_FLAGS_SERVER_STARTUP: The Redis instance is starting
 */
int RM_GetContextFlags(RedisModuleCtx *ctx) {
    int flags = 0;

    /* Client specific flags */
    if (ctx) {
        if (ctx->client) {
            if (ctx->client->flags & CLIENT_DENY_BLOCKING)
                flags |= REDISMODULE_CTX_FLAGS_DENY_BLOCKING;
            /* Module command received from MASTER, is replicated. */
            if (ctx->client->flags & CLIENT_MASTER)
                flags |= REDISMODULE_CTX_FLAGS_REPLICATED;
            if (ctx->client->resp == 3) {
                flags |= REDISMODULE_CTX_FLAGS_RESP3;
            }
        }

        /* For DIRTY flags, we need the blocked client if used */
        client *c = ctx->blocked_client ? ctx->blocked_client->client : ctx->client;
        if (c && (c->flags & (CLIENT_DIRTY_CAS|CLIENT_DIRTY_EXEC))) {
            flags |= REDISMODULE_CTX_FLAGS_MULTI_DIRTY;
        }
    }

    if (scriptIsRunning())
        flags |= REDISMODULE_CTX_FLAGS_LUA;

    if (server.in_exec)
        flags |= REDISMODULE_CTX_FLAGS_MULTI;

    if (server.cluster_enabled)
        flags |= REDISMODULE_CTX_FLAGS_CLUSTER;

    if (server.async_loading)
        flags |= REDISMODULE_CTX_FLAGS_ASYNC_LOADING;
    else if (server.loading)
        flags |= REDISMODULE_CTX_FLAGS_LOADING;

    /* Maxmemory and eviction policy */
    if (server.maxmemory > 0 && (!server.masterhost || !server.repl_slave_ignore_maxmemory)) {
        flags |= REDISMODULE_CTX_FLAGS_MAXMEMORY;

        if (server.maxmemory_policy != MAXMEMORY_NO_EVICTION)
            flags |= REDISMODULE_CTX_FLAGS_EVICT;
    }

    /* Persistence flags */
    if (server.aof_state != AOF_OFF)
        flags |= REDISMODULE_CTX_FLAGS_AOF;
    if (server.saveparamslen > 0)
        flags |= REDISMODULE_CTX_FLAGS_RDB;

    /* Replication flags */
    if (server.masterhost == NULL) {
        flags |= REDISMODULE_CTX_FLAGS_MASTER;
    } else {
        flags |= REDISMODULE_CTX_FLAGS_SLAVE;
        if (server.repl_slave_ro)
            flags |= REDISMODULE_CTX_FLAGS_READONLY;

        /* Replica state flags. */
        if (server.repl_state == REPL_STATE_CONNECT ||
            server.repl_state == REPL_STATE_CONNECTING)
        {
            flags |= REDISMODULE_CTX_FLAGS_REPLICA_IS_CONNECTING;
        } else if (server.repl_state == REPL_STATE_TRANSFER) {
            flags |= REDISMODULE_CTX_FLAGS_REPLICA_IS_TRANSFERRING;
        } else if (server.repl_state == REPL_STATE_CONNECTED) {
            flags |= REDISMODULE_CTX_FLAGS_REPLICA_IS_ONLINE;
        }

        if (server.repl_state != REPL_STATE_CONNECTED)
            flags |= REDISMODULE_CTX_FLAGS_REPLICA_IS_STALE;
    }

    /* OOM flag. */
    float level;
    int retval = getMaxmemoryState(NULL,NULL,NULL,&level);
    if (retval == C_ERR) flags |= REDISMODULE_CTX_FLAGS_OOM;
    if (level > 0.75) flags |= REDISMODULE_CTX_FLAGS_OOM_WARNING;

    /* Presence of children processes. */
    if (hasActiveChildProcess()) flags |= REDISMODULE_CTX_FLAGS_ACTIVE_CHILD;
    if (server.in_fork_child) flags |= REDISMODULE_CTX_FLAGS_IS_CHILD;

    /* Non-empty server.loadmodule_queue means that Redis is starting. */
    if (listLength(server.loadmodule_queue) > 0)
        flags |= REDISMODULE_CTX_FLAGS_SERVER_STARTUP;

    return flags;
}

/* Returns true if a client sent the CLIENT PAUSE command to the server or
 * if Redis Cluster does a manual failover, pausing the clients.
 * This is needed when we have a master with replicas, and want to write,
 * without adding further data to the replication channel, that the replicas
 * replication offset, match the one of the master. When this happens, it is
 * safe to failover the master without data loss.
 *
 * However modules may generate traffic by calling RedisModule_Call() with
 * the "!" flag, or by calling RedisModule_Replicate(), in a context outside
 * commands execution, for instance in timeout callbacks, threads safe
 * contexts, and so forth. When modules will generate too much traffic, it
 * will be hard for the master and replicas offset to match, because there
 * is more data to send in the replication channel.
 *
 * So modules may want to try to avoid very heavy background work that has
 * the effect of creating data to the replication channel, when this function
 * returns true. This is mostly useful for modules that have background
 * garbage collection tasks, or that do writes and replicate such writes
 * periodically in timer callbacks or other periodic callbacks.
 */
int RM_AvoidReplicaTraffic() {
    return !!(isPausedActionsWithUpdate(PAUSE_ACTION_REPLICA));
}

/* Change the currently selected DB. Returns an error if the id
 * is out of range.
 *
 * Note that the client will retain the currently selected DB even after
 * the Redis command implemented by the module calling this function
 * returns.
 *
 * If the module command wishes to change something in a different DB and
 * returns back to the original one, it should call RedisModule_GetSelectedDb()
 * before in order to restore the old DB number before returning. */
int RM_SelectDb(RedisModuleCtx *ctx, int newid) {
    int retval = selectDb(ctx->client,newid);
    return (retval == C_OK) ? REDISMODULE_OK : REDISMODULE_ERR;
}

/* Check if a key exists, without affecting its last access time.
 *
 * This is equivalent to calling RM_OpenKey with the mode REDISMODULE_READ |
 * REDISMODULE_OPEN_KEY_NOTOUCH, then checking if NULL was returned and, if not,
 * calling RM_CloseKey on the opened key.
 */
int RM_KeyExists(RedisModuleCtx *ctx, robj *keyname) {
    robj *value = lookupKeyReadWithFlags(ctx->client->db, keyname, LOOKUP_NOTOUCH);
    return (value != NULL);
}

/* Initialize a RedisModuleKey struct */
static void moduleInitKey(RedisModuleKey *kp, RedisModuleCtx *ctx, robj *keyname, robj *value, int mode){
    kp->ctx = ctx;
    kp->db = ctx->client->db;
    kp->key = keyname;
    incrRefCount(keyname);
    kp->value = value;
    kp->iter = NULL;
    kp->mode = mode;
    if (kp->value) moduleInitKeyTypeSpecific(kp);
}

/* Initialize the type-specific part of the key. Only when key has a value. */
static void moduleInitKeyTypeSpecific(RedisModuleKey *key) {
    switch (key->value->type) {
    case OBJ_ZSET: zsetKeyReset(key); break;
    case OBJ_STREAM: key->u.stream.signalready = 0; break;
    }
}

/* Return a handle representing a Redis key, so that it is possible
 * to call other APIs with the key handle as argument to perform
 * operations on the key.
 *
 * The return value is the handle representing the key, that must be
 * closed with RM_CloseKey().
 *
 * If the key does not exist and REDISMODULE_WRITE mode is requested, the handle
 * is still returned, since it is possible to perform operations on
 * a yet not existing key (that will be created, for example, after
 * a list push operation). If the mode is just REDISMODULE_READ instead, and the
 * key does not exist, NULL is returned. However it is still safe to
 * call RedisModule_CloseKey() and RedisModule_KeyType() on a NULL
 * value.
 *
 * Extra flags that can be pass to the API under the mode argument:
 * * REDISMODULE_OPEN_KEY_NOTOUCH - Avoid touching the LRU/LFU of the key when opened.
 * * REDISMODULE_OPEN_KEY_NONOTIFY - Don't trigger keyspace event on key misses.
 * * REDISMODULE_OPEN_KEY_NOSTATS - Don't update keyspace hits/misses counters.
 * * REDISMODULE_OPEN_KEY_NOEXPIRE - Avoid deleting lazy expired keys.
 * * REDISMODULE_OPEN_KEY_NOEFFECTS - Avoid any effects from fetching the key. */
RedisModuleKey *RM_OpenKey(RedisModuleCtx *ctx, robj *keyname, int mode) {
    RedisModuleKey *kp;
    robj *value;
    int flags = 0;
    flags |= (mode & REDISMODULE_OPEN_KEY_NOTOUCH? LOOKUP_NOTOUCH: 0);
    flags |= (mode & REDISMODULE_OPEN_KEY_NONOTIFY? LOOKUP_NONOTIFY: 0);
    flags |= (mode & REDISMODULE_OPEN_KEY_NOSTATS? LOOKUP_NOSTATS: 0);
    flags |= (mode & REDISMODULE_OPEN_KEY_NOEXPIRE? LOOKUP_NOEXPIRE: 0);
    flags |= (mode & REDISMODULE_OPEN_KEY_NOEFFECTS? LOOKUP_NOEFFECTS: 0);

    if (mode & REDISMODULE_WRITE) {
        value = lookupKeyWriteWithFlags(ctx->client->db,keyname, flags);
    } else {
        value = lookupKeyReadWithFlags(ctx->client->db,keyname, flags);
        if (value == NULL) {
            return NULL;
        }
    }

    /* Setup the key handle. */
    kp = zmalloc(sizeof(*kp));
    moduleInitKey(kp, ctx, keyname, value, mode);
    autoMemoryAdd(ctx,REDISMODULE_AM_KEY,kp);
    return kp;
}

/**
 * Returns the full OpenKey modes mask, using the return value
 * the module can check if a certain set of OpenKey modes are supported
 * by the redis server version in use.
 * Example:
 *
 *        int supportedMode = RM_GetOpenKeyModesAll();
 *        if (supportedMode & REDISMODULE_OPEN_KEY_NOTOUCH) {
 *              // REDISMODULE_OPEN_KEY_NOTOUCH is supported
 *        } else{
 *              // REDISMODULE_OPEN_KEY_NOTOUCH is not supported
 *        }
 */
int RM_GetOpenKeyModesAll() {
    return _REDISMODULE_OPEN_KEY_ALL;
}

/* Destroy a RedisModuleKey struct (freeing is the responsibility of the caller). */
static void moduleCloseKey(RedisModuleKey *key) {
    int signal = SHOULD_SIGNAL_MODIFIED_KEYS(key->ctx);
    if ((key->mode & REDISMODULE_WRITE) && signal)
        signalModifiedKey(key->ctx->client,key->db,key->key);
    if (key->value) {
        if (key->iter) moduleFreeKeyIterator(key);
        switch (key->value->type) {
        case OBJ_ZSET:
            RM_ZsetRangeStop(key);
            break;
        case OBJ_STREAM:
            if (key->u.stream.signalready)
                /* One or more RM_StreamAdd() have been done. */
                signalKeyAsReady(key->db, key->key, OBJ_STREAM);
            break;
        }
    }
    serverAssert(key->iter == NULL);
    decrRefCount(key->key);
}

/* Close a key handle. */
void RM_CloseKey(RedisModuleKey *key) {
    if (key == NULL) return;
    moduleCloseKey(key);
    autoMemoryFreed(key->ctx,REDISMODULE_AM_KEY,key);
    zfree(key);
}

/* Return the type of the key. If the key pointer is NULL then
 * REDISMODULE_KEYTYPE_EMPTY is returned. */
int RM_KeyType(RedisModuleKey *key) {
    if (key == NULL || key->value ==  NULL) return REDISMODULE_KEYTYPE_EMPTY;
    /* We map between defines so that we are free to change the internal
     * defines as desired. */
    switch(key->value->type) {
    case OBJ_STRING: return REDISMODULE_KEYTYPE_STRING;
    case OBJ_LIST: return REDISMODULE_KEYTYPE_LIST;
    case OBJ_SET: return REDISMODULE_KEYTYPE_SET;
    case OBJ_ZSET: return REDISMODULE_KEYTYPE_ZSET;
    case OBJ_HASH: return REDISMODULE_KEYTYPE_HASH;
    case OBJ_MODULE: return REDISMODULE_KEYTYPE_MODULE;
    case OBJ_STREAM: return REDISMODULE_KEYTYPE_STREAM;
    default: return REDISMODULE_KEYTYPE_EMPTY;
    }
}

/* Return the length of the value associated with the key.
 * For strings this is the length of the string. For all the other types
 * is the number of elements (just counting keys for hashes).
 *
 * If the key pointer is NULL or the key is empty, zero is returned. */
size_t RM_ValueLength(RedisModuleKey *key) {
    if (key == NULL || key->value == NULL) return 0;
    switch(key->value->type) {
    case OBJ_STRING: return stringObjectLen(key->value);
    case OBJ_LIST: return listTypeLength(key->value);
    case OBJ_SET: return setTypeSize(key->value);
    case OBJ_ZSET: return zsetLength(key->value);
    case OBJ_HASH: return hashTypeLength(key->value);
    case OBJ_STREAM: return streamLength(key->value);
    default: return 0;
    }
}

/* If the key is open for writing, remove it, and setup the key to
 * accept new writes as an empty key (that will be created on demand).
 * On success REDISMODULE_OK is returned. If the key is not open for
 * writing REDISMODULE_ERR is returned. */
int RM_DeleteKey(RedisModuleKey *key) {
    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value) {
        dbDelete(key->db,key->key);
        key->value = NULL;
    }
    return REDISMODULE_OK;
}

/* If the key is open for writing, unlink it (that is delete it in a
 * non-blocking way, not reclaiming memory immediately) and setup the key to
 * accept new writes as an empty key (that will be created on demand).
 * On success REDISMODULE_OK is returned. If the key is not open for
 * writing REDISMODULE_ERR is returned. */
int RM_UnlinkKey(RedisModuleKey *key) {
    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value) {
        dbAsyncDelete(key->db,key->key);
        key->value = NULL;
    }
    return REDISMODULE_OK;
}

/* Return the key expire value, as milliseconds of remaining TTL.
 * If no TTL is associated with the key or if the key is empty,
 * REDISMODULE_NO_EXPIRE is returned. */
mstime_t RM_GetExpire(RedisModuleKey *key) {
    mstime_t expire = getExpire(key->db,key->key);
    if (expire == -1 || key->value == NULL)
        return REDISMODULE_NO_EXPIRE;
    expire -= commandTimeSnapshot();
    return expire >= 0 ? expire : 0;
}

/* Set a new expire for the key. If the special expire
 * REDISMODULE_NO_EXPIRE is set, the expire is cancelled if there was
 * one (the same as the PERSIST command).
 *
 * Note that the expire must be provided as a positive integer representing
 * the number of milliseconds of TTL the key should have.
 *
 * The function returns REDISMODULE_OK on success or REDISMODULE_ERR if
 * the key was not open for writing or is an empty key. */
int RM_SetExpire(RedisModuleKey *key, mstime_t expire) {
    if (!(key->mode & REDISMODULE_WRITE) || key->value == NULL || (expire < 0 && expire != REDISMODULE_NO_EXPIRE))
        return REDISMODULE_ERR;
    if (expire != REDISMODULE_NO_EXPIRE) {
        expire += commandTimeSnapshot();
        setExpire(key->ctx->client,key->db,key->key,expire);
    } else {
        removeExpire(key->db,key->key);
    }
    return REDISMODULE_OK;
}

/* Return the key expire value, as absolute Unix timestamp.
 * If no TTL is associated with the key or if the key is empty,
 * REDISMODULE_NO_EXPIRE is returned. */
mstime_t RM_GetAbsExpire(RedisModuleKey *key) {
    mstime_t expire = getExpire(key->db,key->key);
    if (expire == -1 || key->value == NULL)
        return REDISMODULE_NO_EXPIRE;
    return expire;
}

/* Set a new expire for the key. If the special expire
 * REDISMODULE_NO_EXPIRE is set, the expire is cancelled if there was
 * one (the same as the PERSIST command).
 * 
 * Note that the expire must be provided as a positive integer representing
 * the absolute Unix timestamp the key should have.
 *
 * The function returns REDISMODULE_OK on success or REDISMODULE_ERR if
 * the key was not open for writing or is an empty key. */
int RM_SetAbsExpire(RedisModuleKey *key, mstime_t expire) {
    if (!(key->mode & REDISMODULE_WRITE) || key->value == NULL || (expire < 0 && expire != REDISMODULE_NO_EXPIRE))
        return REDISMODULE_ERR;
    if (expire != REDISMODULE_NO_EXPIRE) {
        setExpire(key->ctx->client,key->db,key->key,expire);
    } else {
        removeExpire(key->db,key->key);
    }
    return REDISMODULE_OK;
}

/* Performs similar operation to FLUSHALL, and optionally start a new AOF file (if enabled)
 * If restart_aof is true, you must make sure the command that triggered this call is not
 * propagated to the AOF file.
 * When async is set to true, db contents will be freed by a background thread. */
void RM_ResetDataset(int restart_aof, int async) {
    if (restart_aof && server.aof_state != AOF_OFF) stopAppendOnly();
    flushAllDataAndResetRDB((async? EMPTYDB_ASYNC: EMPTYDB_NO_FLAGS) | EMPTYDB_NOFUNCTIONS);
    if (server.aof_enabled && restart_aof) restartAOFAfterSYNC();
}

/* Returns the number of keys in the current db. */
unsigned long long RM_DbSize(RedisModuleCtx *ctx) {
    return dictSize(ctx->client->db->dict);
}

/* Returns a name of a random key, or NULL if current db is empty. */
RedisModuleString *RM_RandomKey(RedisModuleCtx *ctx) {
    robj *key = dbRandomKey(ctx->client->db);
    autoMemoryAdd(ctx,REDISMODULE_AM_STRING,key);
    return key;
}

/* Returns the name of the key currently being processed. */
const RedisModuleString *RM_GetKeyNameFromOptCtx(RedisModuleKeyOptCtx *ctx) {
    return ctx->from_key;
}

/* Returns the name of the target key currently being processed. */
const RedisModuleString *RM_GetToKeyNameFromOptCtx(RedisModuleKeyOptCtx *ctx) {
    return ctx->to_key;
}

/* Returns the dbid currently being processed. */
int RM_GetDbIdFromOptCtx(RedisModuleKeyOptCtx *ctx) {
    return ctx->from_dbid;
}

/* Returns the target dbid currently being processed. */
int RM_GetToDbIdFromOptCtx(RedisModuleKeyOptCtx *ctx) {
    return ctx->to_dbid;
}
/* --------------------------------------------------------------------------
 * ## Key API for String type
 *
 * See also RM_ValueLength(), which returns the length of a string.
 * -------------------------------------------------------------------------- */

/* If the key is open for writing, set the specified string 'str' as the
 * value of the key, deleting the old value if any.
 * On success REDISMODULE_OK is returned. If the key is not open for
 * writing or there is an active iterator, REDISMODULE_ERR is returned. */
int RM_StringSet(RedisModuleKey *key, RedisModuleString *str) {
    if (!(key->mode & REDISMODULE_WRITE) || key->iter) return REDISMODULE_ERR;
    RM_DeleteKey(key);
    setKey(key->ctx->client,key->db,key->key,str,SETKEY_NO_SIGNAL);
    key->value = str;
    return REDISMODULE_OK;
}

/* Prepare the key associated string value for DMA access, and returns
 * a pointer and size (by reference), that the user can use to read or
 * modify the string in-place accessing it directly via pointer.
 *
 * The 'mode' is composed by bitwise OR-ing the following flags:
 *
 *     REDISMODULE_READ -- Read access
 *     REDISMODULE_WRITE -- Write access
 *
 * If the DMA is not requested for writing, the pointer returned should
 * only be accessed in a read-only fashion.
 *
 * On error (wrong type) NULL is returned.
 *
 * DMA access rules:
 *
 * 1. No other key writing function should be called since the moment
 * the pointer is obtained, for all the time we want to use DMA access
 * to read or modify the string.
 *
 * 2. Each time RM_StringTruncate() is called, to continue with the DMA
 * access, RM_StringDMA() should be called again to re-obtain
 * a new pointer and length.
 *
 * 3. If the returned pointer is not NULL, but the length is zero, no
 * byte can be touched (the string is empty, or the key itself is empty)
 * so a RM_StringTruncate() call should be used if there is to enlarge
 * the string, and later call StringDMA() again to get the pointer.
 */
char *RM_StringDMA(RedisModuleKey *key, size_t *len, int mode) {
    /* We need to return *some* pointer for empty keys, we just return
     * a string literal pointer, that is the advantage to be mapped into
     * a read only memory page, so the module will segfault if a write
     * attempt is performed. */
    char *emptystring = "<dma-empty-string>";
    if (key->value == NULL) {
        *len = 0;
        return emptystring;
    }

    if (key->value->type != OBJ_STRING) return NULL;

    /* For write access, and even for read access if the object is encoded,
     * we unshare the string (that has the side effect of decoding it). */
    if ((mode & REDISMODULE_WRITE) || key->value->encoding != OBJ_ENCODING_RAW)
        key->value = dbUnshareStringValue(key->db, key->key, key->value);

    *len = sdslen(key->value->ptr);
    return key->value->ptr;
}

/* If the key is open for writing and is of string type, resize it, padding
 * with zero bytes if the new length is greater than the old one.
 *
 * After this call, RM_StringDMA() must be called again to continue
 * DMA access with the new pointer.
 *
 * The function returns REDISMODULE_OK on success, and REDISMODULE_ERR on
 * error, that is, the key is not open for writing, is not a string
 * or resizing for more than 512 MB is requested.
 *
 * If the key is empty, a string key is created with the new string value
 * unless the new length value requested is zero. */
int RM_StringTruncate(RedisModuleKey *key, size_t newlen) {
    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value && key->value->type != OBJ_STRING) return REDISMODULE_ERR;
    if (newlen > 512*1024*1024) return REDISMODULE_ERR;

    /* Empty key and new len set to 0. Just return REDISMODULE_OK without
     * doing anything. */
    if (key->value == NULL && newlen == 0) return REDISMODULE_OK;

    if (key->value == NULL) {
        /* Empty key: create it with the new size. */
        robj *o = createObject(OBJ_STRING,sdsnewlen(NULL, newlen));
        setKey(key->ctx->client,key->db,key->key,o,SETKEY_NO_SIGNAL);
        key->value = o;
        decrRefCount(o);
    } else {
        /* Unshare and resize. */
        key->value = dbUnshareStringValue(key->db, key->key, key->value);
        size_t curlen = sdslen(key->value->ptr);
        if (newlen > curlen) {
            key->value->ptr = sdsgrowzero(key->value->ptr,newlen);
        } else if (newlen < curlen) {
            sdssubstr(key->value->ptr,0,newlen);
            /* If the string is too wasteful, reallocate it. */
            if (sdslen(key->value->ptr) < sdsavail(key->value->ptr))
                key->value->ptr = sdsRemoveFreeSpace(key->value->ptr, 0);
        }
    }
    return REDISMODULE_OK;
}

/* --------------------------------------------------------------------------
 * ## Key API for List type
 *
 * Many of the list functions access elements by index. Since a list is in
 * essence a doubly-linked list, accessing elements by index is generally an
 * O(N) operation. However, if elements are accessed sequentially or with
 * indices close together, the functions are optimized to seek the index from
 * the previous index, rather than seeking from the ends of the list.
 *
 * This enables iteration to be done efficiently using a simple for loop:
 *
 *     long n = RM_ValueLength(key);
 *     for (long i = 0; i < n; i++) {
 *         RedisModuleString *elem = RedisModule_ListGet(key, i);
 *         // Do stuff...
 *     }
 *
 * Note that after modifying a list using RM_ListPop, RM_ListSet or
 * RM_ListInsert, the internal iterator is invalidated so the next operation
 * will require a linear seek.
 *
 * Modifying a list in any another way, for example using RM_Call(), while a key
 * is open will confuse the internal iterator and may cause trouble if the key
 * is used after such modifications. The key must be reopened in this case.
 *
 * See also RM_ValueLength(), which returns the length of a list.
 * -------------------------------------------------------------------------- */

/* Seeks the key's internal list iterator to the given index. On success, 1 is
 * returned and key->iter, key->u.list.entry and key->u.list.index are set. On
 * failure, 0 is returned and errno is set as required by the list API
 * functions. */
int moduleListIteratorSeek(RedisModuleKey *key, long index, int mode) {
    if (!key) {
        errno = EINVAL;
        return 0;
    } else if (!key->value || key->value->type != OBJ_LIST) {
        errno = ENOTSUP;
        return 0;
    } if (!(key->mode & mode)) {
        errno = EBADF;
        return 0;
    }

    long length = listTypeLength(key->value);
    if (index < -length || index >= length) {
        errno = EDOM; /* Invalid index */
        return 0;
    }

    if (key->iter == NULL) {
        /* No existing iterator. Create one. */
        key->iter = listTypeInitIterator(key->value, index, LIST_TAIL);
        serverAssert(key->iter != NULL);
        serverAssert(listTypeNext(key->iter, &key->u.list.entry));
        key->u.list.index = index;
        return 1;
    }

    /* There's an existing iterator. Make sure the requested index has the same
     * sign as the iterator's index. */
    if      (index < 0 && key->u.list.index >= 0) index += length;
    else if (index >= 0 && key->u.list.index < 0) index -= length;

    if (index == key->u.list.index) return 1; /* We're done. */

    /* Seek the iterator to the requested index. */
    unsigned char dir = key->u.list.index < index ? LIST_TAIL : LIST_HEAD;
    listTypeSetIteratorDirection(key->iter, &key->u.list.entry, dir);
    while (key->u.list.index != index) {
        serverAssert(listTypeNext(key->iter, &key->u.list.entry));
        key->u.list.index += dir == LIST_HEAD ? -1 : 1;
    }
    return 1;
}

/* Push an element into a list, on head or tail depending on 'where' argument
 * (REDISMODULE_LIST_HEAD or REDISMODULE_LIST_TAIL). If the key refers to an
 * empty key opened for writing, the key is created. On success, REDISMODULE_OK
 * is returned. On failure, REDISMODULE_ERR is returned and `errno` is set as
 * follows:
 *
 * - EINVAL if key or ele is NULL.
 * - ENOTSUP if the key is of another type than list.
 * - EBADF if the key is not opened for writing.
 *
 * Note: Before Redis 7.0, `errno` was not set by this function. */
int RM_ListPush(RedisModuleKey *key, int where, RedisModuleString *ele) {
    if (!key || !ele) {
        errno = EINVAL;
        return REDISMODULE_ERR;
    } else if (key->value != NULL && key->value->type != OBJ_LIST) {
        errno = ENOTSUP;
        return REDISMODULE_ERR;
    } if (!(key->mode & REDISMODULE_WRITE)) {
        errno = EBADF;
        return REDISMODULE_ERR;
    }

    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value && key->value->type != OBJ_LIST) return REDISMODULE_ERR;
    if (key->iter) moduleFreeKeyIterator(key);
    if (key->value == NULL) moduleCreateEmptyKey(key,REDISMODULE_KEYTYPE_LIST);
    listTypeTryConversionAppend(key->value, &ele, 0, 0, moduleFreeListIterator, key);
    listTypePush(key->value, ele,
        (where == REDISMODULE_LIST_HEAD) ? LIST_HEAD : LIST_TAIL);
    return REDISMODULE_OK;
}

/* Pop an element from the list, and returns it as a module string object
 * that the user should be free with RM_FreeString() or by enabling
 * automatic memory. The `where` argument specifies if the element should be
 * popped from the beginning or the end of the list (REDISMODULE_LIST_HEAD or
 * REDISMODULE_LIST_TAIL). On failure, the command returns NULL and sets
 * `errno` as follows:
 *
 * - EINVAL if key is NULL.
 * - ENOTSUP if the key is empty or of another type than list.
 * - EBADF if the key is not opened for writing.
 *
 * Note: Before Redis 7.0, `errno` was not set by this function. */
RedisModuleString *RM_ListPop(RedisModuleKey *key, int where) {
    if (!key) {
        errno = EINVAL;
        return NULL;
    } else if (key->value == NULL || key->value->type != OBJ_LIST) {
        errno = ENOTSUP;
        return NULL;
    } else if (!(key->mode & REDISMODULE_WRITE)) {
        errno = EBADF;
        return NULL;
    }
    if (key->iter) moduleFreeKeyIterator(key);
    robj *ele = listTypePop(key->value,
        (where == REDISMODULE_LIST_HEAD) ? LIST_HEAD : LIST_TAIL);
    robj *decoded = getDecodedObject(ele);
    decrRefCount(ele);
    if (!moduleDelKeyIfEmpty(key))
        listTypeTryConversion(key->value, LIST_CONV_SHRINKING, moduleFreeListIterator, key);
    autoMemoryAdd(key->ctx,REDISMODULE_AM_STRING,decoded);
    return decoded;
}

/* Returns the element at index `index` in the list stored at `key`, like the
 * LINDEX command. The element should be free'd using RM_FreeString() or using
 * automatic memory management.
 *
 * The index is zero-based, so 0 means the first element, 1 the second element
 * and so on. Negative indices can be used to designate elements starting at the
 * tail of the list. Here, -1 means the last element, -2 means the penultimate
 * and so forth.
 *
 * When no value is found at the given key and index, NULL is returned and
 * `errno` is set as follows:
 *
 * - EINVAL if key is NULL.
 * - ENOTSUP if the key is not a list.
 * - EBADF if the key is not opened for reading.
 * - EDOM if the index is not a valid index in the list.
 */
RedisModuleString *RM_ListGet(RedisModuleKey *key, long index) {
    if (moduleListIteratorSeek(key, index, REDISMODULE_READ)) {
        robj *elem = listTypeGet(&key->u.list.entry);
        robj *decoded = getDecodedObject(elem);
        decrRefCount(elem);
        autoMemoryAdd(key->ctx, REDISMODULE_AM_STRING, decoded);
        return decoded;
    } else {
        return NULL;
    }
}

/* Replaces the element at index `index` in the list stored at `key`.
 *
 * The index is zero-based, so 0 means the first element, 1 the second element
 * and so on. Negative indices can be used to designate elements starting at the
 * tail of the list. Here, -1 means the last element, -2 means the penultimate
 * and so forth.
 *
 * On success, REDISMODULE_OK is returned. On failure, REDISMODULE_ERR is
 * returned and `errno` is set as follows:
 *
 * - EINVAL if key or value is NULL.
 * - ENOTSUP if the key is not a list.
 * - EBADF if the key is not opened for writing.
 * - EDOM if the index is not a valid index in the list.
 */
int RM_ListSet(RedisModuleKey *key, long index, RedisModuleString *value) {
    if (!value) {
        errno = EINVAL;
        return REDISMODULE_ERR;
    }
    if (!key->value || key->value->type != OBJ_LIST) {
        errno = ENOTSUP;
        return REDISMODULE_ERR;
    }
    listTypeTryConversionAppend(key->value, &value, 0, 0, moduleFreeListIterator, key);
    if (moduleListIteratorSeek(key, index, REDISMODULE_WRITE)) {
        listTypeReplace(&key->u.list.entry, value);
        /* A note in quicklist.c forbids use of iterator after insert, so
         * probably also after replace. */
        moduleFreeKeyIterator(key);
        return REDISMODULE_OK;
    } else {
        return REDISMODULE_ERR;
    }
}

/* Inserts an element at the given index.
 *
 * The index is zero-based, so 0 means the first element, 1 the second element
 * and so on. Negative indices can be used to designate elements starting at the
 * tail of the list. Here, -1 means the last element, -2 means the penultimate
 * and so forth. The index is the element's index after inserting it.
 *
 * On success, REDISMODULE_OK is returned. On failure, REDISMODULE_ERR is
 * returned and `errno` is set as follows:
 *
 * - EINVAL if key or value is NULL.
 * - ENOTSUP if the key of another type than list.
 * - EBADF if the key is not opened for writing.
 * - EDOM if the index is not a valid index in the list.
 */
int RM_ListInsert(RedisModuleKey *key, long index, RedisModuleString *value) {
    if (!value) {
        errno = EINVAL;
        return REDISMODULE_ERR;
    } else if (key != NULL && key->value == NULL &&
               (index == 0 || index == -1)) {
        /* Insert in empty key => push. */
        return RM_ListPush(key, REDISMODULE_LIST_TAIL, value);
    } else if (key != NULL && key->value != NULL &&
               key->value->type == OBJ_LIST &&
               (index == (long)listTypeLength(key->value) || index == -1)) {
        /* Insert after the last element => push tail. */
        return RM_ListPush(key, REDISMODULE_LIST_TAIL, value);
    } else if (key != NULL && key->value != NULL &&
               key->value->type == OBJ_LIST &&
               (index == 0 || index == -(long)listTypeLength(key->value) - 1)) {
        /* Insert before the first element => push head. */
        return RM_ListPush(key, REDISMODULE_LIST_HEAD, value);
    }
    listTypeTryConversionAppend(key->value, &value, 0, 0, moduleFreeListIterator, key);
    if (moduleListIteratorSeek(key, index, REDISMODULE_WRITE)) {
        int where = index < 0 ? LIST_TAIL : LIST_HEAD;
        listTypeInsert(&key->u.list.entry, value, where);
        /* A note in quicklist.c forbids use of iterator after insert. */
        moduleFreeKeyIterator(key);
        return REDISMODULE_OK;
    } else {
        return REDISMODULE_ERR;
    }
}

/* Removes an element at the given index. The index is 0-based. A negative index
 * can also be used, counting from the end of the list.
 *
 * On success, REDISMODULE_OK is returned. On failure, REDISMODULE_ERR is
 * returned and `errno` is set as follows:
 *
 * - EINVAL if key or value is NULL.
 * - ENOTSUP if the key is not a list.
 * - EBADF if the key is not opened for writing.
 * - EDOM if the index is not a valid index in the list.
 */
int RM_ListDelete(RedisModuleKey *key, long index) {
    if (moduleListIteratorSeek(key, index, REDISMODULE_WRITE)) {
        listTypeDelete(key->iter, &key->u.list.entry);
        if (moduleDelKeyIfEmpty(key)) return REDISMODULE_OK;
        listTypeTryConversion(key->value, LIST_CONV_SHRINKING, moduleFreeListIterator, key);
        if (!key->iter) return REDISMODULE_OK; /* Return ASAP if iterator has been freed */
        if (listTypeNext(key->iter, &key->u.list.entry)) {
            /* After delete entry at position 'index', we need to update
             * 'key->u.list.index' according to the following cases:
             * 1) [1, 2, 3] => dir: forward, index: 0  => [2, 3] => index: still 0
             * 2) [1, 2, 3] => dir: forward, index: -3 => [2, 3] => index: -2
             * 3) [1, 2, 3] => dir: reverse, index: 2  => [1, 2] => index: 1
             * 4) [1, 2, 3] => dir: reverse, index: -1 => [1, 2] => index: still -1 */
            listTypeIterator *li = key->iter;
            int reverse = li->direction == LIST_HEAD;
            if (key->u.list.index < 0)
                key->u.list.index += reverse ? 0 : 1;
            else
                key->u.list.index += reverse ? -1 : 0;
        } else {
            /* Reset list iterator if the next entry doesn't exist. */
            moduleFreeKeyIterator(key);
        }
        return REDISMODULE_OK;
    } else {
        return REDISMODULE_ERR;
    }
}

/* --------------------------------------------------------------------------
 * ## Key API for Sorted Set type
 *
 * See also RM_ValueLength(), which returns the length of a sorted set.
 * -------------------------------------------------------------------------- */

/* Conversion from/to public flags of the Modules API and our private flags,
 * so that we have everything decoupled. */
int moduleZsetAddFlagsToCoreFlags(int flags) {
    int retflags = 0;
    if (flags & REDISMODULE_ZADD_XX) retflags |= ZADD_IN_XX;
    if (flags & REDISMODULE_ZADD_NX) retflags |= ZADD_IN_NX;
    if (flags & REDISMODULE_ZADD_GT) retflags |= ZADD_IN_GT;
    if (flags & REDISMODULE_ZADD_LT) retflags |= ZADD_IN_LT;
    return retflags;
}

/* See previous function comment. */
int moduleZsetAddFlagsFromCoreFlags(int flags) {
    int retflags = 0;
    if (flags & ZADD_OUT_ADDED) retflags |= REDISMODULE_ZADD_ADDED;
    if (flags & ZADD_OUT_UPDATED) retflags |= REDISMODULE_ZADD_UPDATED;
    if (flags & ZADD_OUT_NOP) retflags |= REDISMODULE_ZADD_NOP;
    return retflags;
}

/* Add a new element into a sorted set, with the specified 'score'.
 * If the element already exists, the score is updated.
 *
 * A new sorted set is created at value if the key is an empty open key
 * setup for writing.
 *
 * Additional flags can be passed to the function via a pointer, the flags
 * are both used to receive input and to communicate state when the function
 * returns. 'flagsptr' can be NULL if no special flags are used.
 *
 * The input flags are:
 *
 *     REDISMODULE_ZADD_XX: Element must already exist. Do nothing otherwise.
 *     REDISMODULE_ZADD_NX: Element must not exist. Do nothing otherwise.
 *     REDISMODULE_ZADD_GT: If element exists, new score must be greater than the current score. 
 *                          Do nothing otherwise. Can optionally be combined with XX.
 *     REDISMODULE_ZADD_LT: If element exists, new score must be less than the current score.
 *                          Do nothing otherwise. Can optionally be combined with XX.
 *
 * The output flags are:
 *
 *     REDISMODULE_ZADD_ADDED: The new element was added to the sorted set.
 *     REDISMODULE_ZADD_UPDATED: The score of the element was updated.
 *     REDISMODULE_ZADD_NOP: No operation was performed because XX or NX flags.
 *
 * On success the function returns REDISMODULE_OK. On the following errors
 * REDISMODULE_ERR is returned:
 *
 * * The key was not opened for writing.
 * * The key is of the wrong type.
 * * 'score' double value is not a number (NaN).
 */
int RM_ZsetAdd(RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr) {
    int in_flags = 0, out_flags = 0;
    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value && key->value->type != OBJ_ZSET) return REDISMODULE_ERR;
    if (key->value == NULL) moduleCreateEmptyKey(key,REDISMODULE_KEYTYPE_ZSET);
    if (flagsptr) in_flags = moduleZsetAddFlagsToCoreFlags(*flagsptr);
    if (zsetAdd(key->value,score,ele->ptr,in_flags,&out_flags,NULL) == 0) {
        if (flagsptr) *flagsptr = 0;
        return REDISMODULE_ERR;
    }
    if (flagsptr) *flagsptr = moduleZsetAddFlagsFromCoreFlags(out_flags);
    return REDISMODULE_OK;
}

/* This function works exactly like RM_ZsetAdd(), but instead of setting
 * a new score, the score of the existing element is incremented, or if the
 * element does not already exist, it is added assuming the old score was
 * zero.
 *
 * The input and output flags, and the return value, have the same exact
 * meaning, with the only difference that this function will return
 * REDISMODULE_ERR even when 'score' is a valid double number, but adding it
 * to the existing score results into a NaN (not a number) condition.
 *
 * This function has an additional field 'newscore', if not NULL is filled
 * with the new score of the element after the increment, if no error
 * is returned. */
int RM_ZsetIncrby(RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr, double *newscore) {
    int in_flags = 0, out_flags = 0;
    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value && key->value->type != OBJ_ZSET) return REDISMODULE_ERR;
    if (key->value == NULL) moduleCreateEmptyKey(key,REDISMODULE_KEYTYPE_ZSET);
    if (flagsptr) in_flags = moduleZsetAddFlagsToCoreFlags(*flagsptr);
    in_flags |= ZADD_IN_INCR;
    if (zsetAdd(key->value,score,ele->ptr,in_flags,&out_flags,newscore) == 0) {
        if (flagsptr) *flagsptr = 0;
        return REDISMODULE_ERR;
    }
    if (flagsptr) *flagsptr = moduleZsetAddFlagsFromCoreFlags(out_flags);
    return REDISMODULE_OK;
}

/* Remove the specified element from the sorted set.
 * The function returns REDISMODULE_OK on success, and REDISMODULE_ERR
 * on one of the following conditions:
 *
 * * The key was not opened for writing.
 * * The key is of the wrong type.
 *
 * The return value does NOT indicate the fact the element was really
 * removed (since it existed) or not, just if the function was executed
 * with success.
 *
 * In order to know if the element was removed, the additional argument
 * 'deleted' must be passed, that populates the integer by reference
 * setting it to 1 or 0 depending on the outcome of the operation.
 * The 'deleted' argument can be NULL if the caller is not interested
 * to know if the element was really removed.
 *
 * Empty keys will be handled correctly by doing nothing. */
int RM_ZsetRem(RedisModuleKey *key, RedisModuleString *ele, int *deleted) {
    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value && key->value->type != OBJ_ZSET) return REDISMODULE_ERR;
    if (key->value != NULL && zsetDel(key->value,ele->ptr)) {
        if (deleted) *deleted = 1;
        moduleDelKeyIfEmpty(key);
    } else {
        if (deleted) *deleted = 0;
    }
    return REDISMODULE_OK;
}

/* On success retrieve the double score associated at the sorted set element
 * 'ele' and returns REDISMODULE_OK. Otherwise REDISMODULE_ERR is returned
 * to signal one of the following conditions:
 *
 * * There is no such element 'ele' in the sorted set.
 * * The key is not a sorted set.
 * * The key is an open empty key.
 */
int RM_ZsetScore(RedisModuleKey *key, RedisModuleString *ele, double *score) {
    if (key->value == NULL) return REDISMODULE_ERR;
    if (key->value->type != OBJ_ZSET) return REDISMODULE_ERR;
    if (zsetScore(key->value,ele->ptr,score) == C_ERR) return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

/* --------------------------------------------------------------------------
 * ## Key API for Sorted Set iterator
 * -------------------------------------------------------------------------- */

void zsetKeyReset(RedisModuleKey *key) {
    key->u.zset.type = REDISMODULE_ZSET_RANGE_NONE;
    key->u.zset.current = NULL;
    key->u.zset.er = 1;
}

/* Stop a sorted set iteration. */
void RM_ZsetRangeStop(RedisModuleKey *key) {
    if (!key->value || key->value->type != OBJ_ZSET) return;
    /* Free resources if needed. */
    if (key->u.zset.type == REDISMODULE_ZSET_RANGE_LEX)
        zslFreeLexRange(&key->u.zset.lrs);
    /* Setup sensible values so that misused iteration API calls when an
     * iterator is not active will result into something more sensible
     * than crashing. */
    zsetKeyReset(key);
}

/* Return the "End of range" flag value to signal the end of the iteration. */
int RM_ZsetRangeEndReached(RedisModuleKey *key) {
    if (!key->value || key->value->type != OBJ_ZSET) return 1;
    return key->u.zset.er;
}

/* Helper function for RM_ZsetFirstInScoreRange() and RM_ZsetLastInScoreRange().
 * Setup the sorted set iteration according to the specified score range
 * (see the functions calling it for more info). If 'first' is true the
 * first element in the range is used as a starting point for the iterator
 * otherwise the last. Return REDISMODULE_OK on success otherwise
 * REDISMODULE_ERR. */
int zsetInitScoreRange(RedisModuleKey *key, double min, double max, int minex, int maxex, int first) {
    if (!key->value || key->value->type != OBJ_ZSET) return REDISMODULE_ERR;

    RM_ZsetRangeStop(key);
    key->u.zset.type = REDISMODULE_ZSET_RANGE_SCORE;
    key->u.zset.er = 0;

    /* Setup the range structure used by the sorted set core implementation
     * in order to seek at the specified element. */
    zrangespec *zrs = &key->u.zset.rs;
    zrs->min = min;
    zrs->max = max;
    zrs->minex = minex;
    zrs->maxex = maxex;

    if (key->value->encoding == OBJ_ENCODING_LISTPACK) {
        key->u.zset.current = first ? zzlFirstInRange(key->value->ptr,zrs) :
                                      zzlLastInRange(key->value->ptr,zrs);
    } else if (key->value->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = key->value->ptr;
        zskiplist *zsl = zs->zsl;
        key->u.zset.current = first ? zslFirstInRange(zsl,zrs) :
                                      zslLastInRange(zsl,zrs);
    } else {
        serverPanic("Unsupported zset encoding");
    }
    if (key->u.zset.current == NULL) key->u.zset.er = 1;
    return REDISMODULE_OK;
}

/* Setup a sorted set iterator seeking the first element in the specified
 * range. Returns REDISMODULE_OK if the iterator was correctly initialized
 * otherwise REDISMODULE_ERR is returned in the following conditions:
 *
 * 1. The value stored at key is not a sorted set or the key is empty.
 *
 * The range is specified according to the two double values 'min' and 'max'.
 * Both can be infinite using the following two macros:
 *
 * * REDISMODULE_POSITIVE_INFINITE for positive infinite value
 * * REDISMODULE_NEGATIVE_INFINITE for negative infinite value
 *
 * 'minex' and 'maxex' parameters, if true, respectively setup a range
 * where the min and max value are exclusive (not included) instead of
 * inclusive. */
int RM_ZsetFirstInScoreRange(RedisModuleKey *key, double min, double max, int minex, int maxex) {
    return zsetInitScoreRange(key,min,max,minex,maxex,1);
}

/* Exactly like RedisModule_ZsetFirstInScoreRange() but the last element of
 * the range is selected for the start of the iteration instead. */
int RM_ZsetLastInScoreRange(RedisModuleKey *key, double min, double max, int minex, int maxex) {
    return zsetInitScoreRange(key,min,max,minex,maxex,0);
}

/* Helper function for RM_ZsetFirstInLexRange() and RM_ZsetLastInLexRange().
 * Setup the sorted set iteration according to the specified lexicographical
 * range (see the functions calling it for more info). If 'first' is true the
 * first element in the range is used as a starting point for the iterator
 * otherwise the last. Return REDISMODULE_OK on success otherwise
 * REDISMODULE_ERR.
 *
 * Note that this function takes 'min' and 'max' in the same form of the
 * Redis ZRANGEBYLEX command. */
int zsetInitLexRange(RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max, int first) {
    if (!key->value || key->value->type != OBJ_ZSET) return REDISMODULE_ERR;

    RM_ZsetRangeStop(key);
    key->u.zset.er = 0;

    /* Setup the range structure used by the sorted set core implementation
     * in order to seek at the specified element. */
    zlexrangespec *zlrs = &key->u.zset.lrs;
    if (zslParseLexRange(min, max, zlrs) == C_ERR) return REDISMODULE_ERR;

    /* Set the range type to lex only after successfully parsing the range,
     * otherwise we don't want the zlexrangespec to be freed. */
    key->u.zset.type = REDISMODULE_ZSET_RANGE_LEX;

    if (key->value->encoding == OBJ_ENCODING_LISTPACK) {
        key->u.zset.current = first ? zzlFirstInLexRange(key->value->ptr,zlrs) :
                                      zzlLastInLexRange(key->value->ptr,zlrs);
    } else if (key->value->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = key->value->ptr;
        zskiplist *zsl = zs->zsl;
        key->u.zset.current = first ? zslFirstInLexRange(zsl,zlrs) :
                                      zslLastInLexRange(zsl,zlrs);
    } else {
        serverPanic("Unsupported zset encoding");
    }
    if (key->u.zset.current == NULL) key->u.zset.er = 1;

    return REDISMODULE_OK;
}

/* Setup a sorted set iterator seeking the first element in the specified
 * lexicographical range. Returns REDISMODULE_OK if the iterator was correctly
 * initialized otherwise REDISMODULE_ERR is returned in the
 * following conditions:
 *
 * 1. The value stored at key is not a sorted set or the key is empty.
 * 2. The lexicographical range 'min' and 'max' format is invalid.
 *
 * 'min' and 'max' should be provided as two RedisModuleString objects
 * in the same format as the parameters passed to the ZRANGEBYLEX command.
 * The function does not take ownership of the objects, so they can be released
 * ASAP after the iterator is setup. */
int RM_ZsetFirstInLexRange(RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max) {
    return zsetInitLexRange(key,min,max,1);
}

/* Exactly like RedisModule_ZsetFirstInLexRange() but the last element of
 * the range is selected for the start of the iteration instead. */
int RM_ZsetLastInLexRange(RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max) {
    return zsetInitLexRange(key,min,max,0);
}

/* Return the current sorted set element of an active sorted set iterator
 * or NULL if the range specified in the iterator does not include any
 * element. */
RedisModuleString *RM_ZsetRangeCurrentElement(RedisModuleKey *key, double *score) {
    RedisModuleString *str;

    if (!key->value || key->value->type != OBJ_ZSET) return NULL;
    if (key->u.zset.current == NULL) return NULL;
    if (key->value->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *eptr, *sptr;
        eptr = key->u.zset.current;
        sds ele = lpGetObject(eptr);
        if (score) {
            sptr = lpNext(key->value->ptr,eptr);
            *score = zzlGetScore(sptr);
        }
        str = createObject(OBJ_STRING,ele);
    } else if (key->value->encoding == OBJ_ENCODING_SKIPLIST) {
        zskiplistNode *ln = key->u.zset.current;
        if (score) *score = ln->score;
        str = createStringObject(ln->ele,sdslen(ln->ele));
    } else {
        serverPanic("Unsupported zset encoding");
    }
    autoMemoryAdd(key->ctx,REDISMODULE_AM_STRING,str);
    return str;
}

/* Go to the next element of the sorted set iterator. Returns 1 if there was
 * a next element, 0 if we are already at the latest element or the range
 * does not include any item at all. */
int RM_ZsetRangeNext(RedisModuleKey *key) {
    if (!key->value || key->value->type != OBJ_ZSET) return 0;
    if (!key->u.zset.type || !key->u.zset.current) return 0; /* No active iterator. */

    if (key->value->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = key->value->ptr;
        unsigned char *eptr = key->u.zset.current;
        unsigned char *next;
        next = lpNext(zl,eptr); /* Skip element. */
        if (next) next = lpNext(zl,next); /* Skip score. */
        if (next == NULL) {
            key->u.zset.er = 1;
            return 0;
        } else {
            /* Are we still within the range? */
            if (key->u.zset.type == REDISMODULE_ZSET_RANGE_SCORE) {
                /* Fetch the next element score for the
                 * range check. */
                unsigned char *saved_next = next;
                next = lpNext(zl,next); /* Skip next element. */
                double score = zzlGetScore(next); /* Obtain the next score. */
                if (!zslValueLteMax(score,&key->u.zset.rs)) {
                    key->u.zset.er = 1;
                    return 0;
                }
                next = saved_next;
            } else if (key->u.zset.type == REDISMODULE_ZSET_RANGE_LEX) {
                if (!zzlLexValueLteMax(next,&key->u.zset.lrs)) {
                    key->u.zset.er = 1;
                    return 0;
                }
            }
            key->u.zset.current = next;
            return 1;
        }
    } else if (key->value->encoding == OBJ_ENCODING_SKIPLIST) {
        zskiplistNode *ln = key->u.zset.current, *next = ln->level[0].forward;
        if (next == NULL) {
            key->u.zset.er = 1;
            return 0;
        } else {
            /* Are we still within the range? */
            if (key->u.zset.type == REDISMODULE_ZSET_RANGE_SCORE &&
                !zslValueLteMax(next->score,&key->u.zset.rs))
            {
                key->u.zset.er = 1;
                return 0;
            } else if (key->u.zset.type == REDISMODULE_ZSET_RANGE_LEX) {
                if (!zslLexValueLteMax(next->ele,&key->u.zset.lrs)) {
                    key->u.zset.er = 1;
                    return 0;
                }
            }
            key->u.zset.current = next;
            return 1;
        }
    } else {
        serverPanic("Unsupported zset encoding");
    }
}

/* Go to the previous element of the sorted set iterator. Returns 1 if there was
 * a previous element, 0 if we are already at the first element or the range
 * does not include any item at all. */
int RM_ZsetRangePrev(RedisModuleKey *key) {
    if (!key->value || key->value->type != OBJ_ZSET) return 0;
    if (!key->u.zset.type || !key->u.zset.current) return 0; /* No active iterator. */

    if (key->value->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = key->value->ptr;
        unsigned char *eptr = key->u.zset.current;
        unsigned char *prev;
        prev = lpPrev(zl,eptr); /* Go back to previous score. */
        if (prev) prev = lpPrev(zl,prev); /* Back to previous ele. */
        if (prev == NULL) {
            key->u.zset.er = 1;
            return 0;
        } else {
            /* Are we still within the range? */
            if (key->u.zset.type == REDISMODULE_ZSET_RANGE_SCORE) {
                /* Fetch the previous element score for the
                 * range check. */
                unsigned char *saved_prev = prev;
                prev = lpNext(zl,prev); /* Skip element to get the score.*/
                double score = zzlGetScore(prev); /* Obtain the prev score. */
                if (!zslValueGteMin(score,&key->u.zset.rs)) {
                    key->u.zset.er = 1;
                    return 0;
                }
                prev = saved_prev;
            } else if (key->u.zset.type == REDISMODULE_ZSET_RANGE_LEX) {
                if (!zzlLexValueGteMin(prev,&key->u.zset.lrs)) {
                    key->u.zset.er = 1;
                    return 0;
                }
            }
            key->u.zset.current = prev;
            return 1;
        }
    } else if (key->value->encoding == OBJ_ENCODING_SKIPLIST) {
        zskiplistNode *ln = key->u.zset.current, *prev = ln->backward;
        if (prev == NULL) {
            key->u.zset.er = 1;
            return 0;
        } else {
            /* Are we still within the range? */
            if (key->u.zset.type == REDISMODULE_ZSET_RANGE_SCORE &&
                !zslValueGteMin(prev->score,&key->u.zset.rs))
            {
                key->u.zset.er = 1;
                return 0;
            } else if (key->u.zset.type == REDISMODULE_ZSET_RANGE_LEX) {
                if (!zslLexValueGteMin(prev->ele,&key->u.zset.lrs)) {
                    key->u.zset.er = 1;
                    return 0;
                }
            }
            key->u.zset.current = prev;
            return 1;
        }
    } else {
        serverPanic("Unsupported zset encoding");
    }
}

/* --------------------------------------------------------------------------
 * ## Key API for Hash type
 *
 * See also RM_ValueLength(), which returns the number of fields in a hash.
 * -------------------------------------------------------------------------- */

/* Set the field of the specified hash field to the specified value.
 * If the key is an empty key open for writing, it is created with an empty
 * hash value, in order to set the specified field.
 *
 * The function is variadic and the user must specify pairs of field
 * names and values, both as RedisModuleString pointers (unless the
 * CFIELD option is set, see later). At the end of the field/value-ptr pairs,
 * NULL must be specified as last argument to signal the end of the arguments
 * in the variadic function.
 *
 * Example to set the hash argv[1] to the value argv[2]:
 *
 *      RedisModule_HashSet(key,REDISMODULE_HASH_NONE,argv[1],argv[2],NULL);
 *
 * The function can also be used in order to delete fields (if they exist)
 * by setting them to the specified value of REDISMODULE_HASH_DELETE:
 *
 *      RedisModule_HashSet(key,REDISMODULE_HASH_NONE,argv[1],
 *                          REDISMODULE_HASH_DELETE,NULL);
 *
 * The behavior of the command changes with the specified flags, that can be
 * set to REDISMODULE_HASH_NONE if no special behavior is needed.
 *
 *     REDISMODULE_HASH_NX: The operation is performed only if the field was not
 *                          already existing in the hash.
 *     REDISMODULE_HASH_XX: The operation is performed only if the field was
 *                          already existing, so that a new value could be
 *                          associated to an existing filed, but no new fields
 *                          are created.
 *     REDISMODULE_HASH_CFIELDS: The field names passed are null terminated C
 *                               strings instead of RedisModuleString objects.
 *     REDISMODULE_HASH_COUNT_ALL: Include the number of inserted fields in the
 *                                 returned number, in addition to the number of
 *                                 updated and deleted fields. (Added in Redis
 *                                 6.2.)
 *
 * Unless NX is specified, the command overwrites the old field value with
 * the new one.
 *
 * When using REDISMODULE_HASH_CFIELDS, field names are reported using
 * normal C strings, so for example to delete the field "foo" the following
 * code can be used:
 *
 *      RedisModule_HashSet(key,REDISMODULE_HASH_CFIELDS,"foo",
 *                          REDISMODULE_HASH_DELETE,NULL);
 *
 * Return value:
 *
 * The number of fields existing in the hash prior to the call, which have been
 * updated (its old value has been replaced by a new value) or deleted. If the
 * flag REDISMODULE_HASH_COUNT_ALL is set, inserted fields not previously
 * existing in the hash are also counted.
 *
 * If the return value is zero, `errno` is set (since Redis 6.2) as follows:
 *
 * - EINVAL if any unknown flags are set or if key is NULL.
 * - ENOTSUP if the key is associated with a non Hash value.
 * - EBADF if the key was not opened for writing.
 * - ENOENT if no fields were counted as described under Return value above.
 *   This is not actually an error. The return value can be zero if all fields
 *   were just created and the COUNT_ALL flag was unset, or if changes were held
 *   back due to the NX and XX flags.
 *
 * NOTICE: The return value semantics of this function are very different
 * between Redis 6.2 and older versions. Modules that use it should determine
 * the Redis version and handle it accordingly.
 */
int RM_HashSet(RedisModuleKey *key, int flags, ...) {
    va_list ap;
    if (!key || (flags & ~(REDISMODULE_HASH_NX |
                           REDISMODULE_HASH_XX |
                           REDISMODULE_HASH_CFIELDS |
                           REDISMODULE_HASH_COUNT_ALL))) {
        errno = EINVAL;
        return 0;
    } else if (key->value && key->value->type != OBJ_HASH) {
        errno = ENOTSUP;
        return 0;
    } else if (!(key->mode & REDISMODULE_WRITE)) {
        errno = EBADF;
        return 0;
    }
    if (key->value == NULL) moduleCreateEmptyKey(key,REDISMODULE_KEYTYPE_HASH);

    int count = 0;
    va_start(ap, flags);
    while(1) {
        RedisModuleString *field, *value;
        /* Get the field and value objects. */
        if (flags & REDISMODULE_HASH_CFIELDS) {
            char *cfield = va_arg(ap,char*);
            if (cfield == NULL) break;
            field = createRawStringObject(cfield,strlen(cfield));
        } else {
            field = va_arg(ap,RedisModuleString*);
            if (field == NULL) break;
        }
        value = va_arg(ap,RedisModuleString*);

        /* Handle XX and NX */
        if (flags & (REDISMODULE_HASH_XX|REDISMODULE_HASH_NX)) {
            int exists = hashTypeExists(key->value, field->ptr);
            if (((flags & REDISMODULE_HASH_XX) && !exists) ||
                ((flags & REDISMODULE_HASH_NX) && exists))
            {
                if (flags & REDISMODULE_HASH_CFIELDS) decrRefCount(field);
                continue;
            }
        }

        /* Handle deletion if value is REDISMODULE_HASH_DELETE. */
        if (value == REDISMODULE_HASH_DELETE) {
            count += hashTypeDelete(key->value, field->ptr);
            if (flags & REDISMODULE_HASH_CFIELDS) decrRefCount(field);
            continue;
        }

        int low_flags = HASH_SET_COPY;
        /* If CFIELDS is active, we can pass the ownership of the
         * SDS object to the low level function that sets the field
         * to avoid a useless copy. */
        if (flags & REDISMODULE_HASH_CFIELDS)
            low_flags |= HASH_SET_TAKE_FIELD;

        robj *argv[2] = {field,value};
        hashTypeTryConversion(key->value,argv,0,1);
        int updated = hashTypeSet(key->value, field->ptr, value->ptr, low_flags);
        count += (flags & REDISMODULE_HASH_COUNT_ALL) ? 1 : updated;

        /* If CFIELDS is active, SDS string ownership is now of hashTypeSet(),
         * however we still have to release the 'field' object shell. */
        if (flags & REDISMODULE_HASH_CFIELDS) {
           field->ptr = NULL; /* Prevent the SDS string from being freed. */
           decrRefCount(field);
        }
    }
    va_end(ap);
    moduleDelKeyIfEmpty(key);
    if (count == 0) errno = ENOENT;
    return count;
}

/* Get fields from a hash value. This function is called using a variable
 * number of arguments, alternating a field name (as a RedisModuleString
 * pointer) with a pointer to a RedisModuleString pointer, that is set to the
 * value of the field if the field exists, or NULL if the field does not exist.
 * At the end of the field/value-ptr pairs, NULL must be specified as last
 * argument to signal the end of the arguments in the variadic function.
 *
 * This is an example usage:
 *
 *      RedisModuleString *first, *second;
 *      RedisModule_HashGet(mykey,REDISMODULE_HASH_NONE,argv[1],&first,
 *                          argv[2],&second,NULL);
 *
 * As with RedisModule_HashSet() the behavior of the command can be specified
 * passing flags different than REDISMODULE_HASH_NONE:
 *
 * REDISMODULE_HASH_CFIELDS: field names as null terminated C strings.
 *
 * REDISMODULE_HASH_EXISTS: instead of setting the value of the field
 * expecting a RedisModuleString pointer to pointer, the function just
 * reports if the field exists or not and expects an integer pointer
 * as the second element of each pair.
 *
 * Example of REDISMODULE_HASH_CFIELDS:
 *
 *      RedisModuleString *username, *hashedpass;
 *      RedisModule_HashGet(mykey,REDISMODULE_HASH_CFIELDS,"username",&username,"hp",&hashedpass, NULL);
 *
 * Example of REDISMODULE_HASH_EXISTS:
 *
 *      int exists;
 *      RedisModule_HashGet(mykey,REDISMODULE_HASH_EXISTS,argv[1],&exists,NULL);
 *
 * The function returns REDISMODULE_OK on success and REDISMODULE_ERR if
 * the key is not a hash value.
 *
 * Memory management:
 *
 * The returned RedisModuleString objects should be released with
 * RedisModule_FreeString(), or by enabling automatic memory management.
 */
int RM_HashGet(RedisModuleKey *key, int flags, ...) {
    va_list ap;
    if (key->value && key->value->type != OBJ_HASH) return REDISMODULE_ERR;

    va_start(ap, flags);
    while(1) {
        RedisModuleString *field, **valueptr;
        int *existsptr;
        /* Get the field object and the value pointer to pointer. */
        if (flags & REDISMODULE_HASH_CFIELDS) {
            char *cfield = va_arg(ap,char*);
            if (cfield == NULL) break;
            field = createRawStringObject(cfield,strlen(cfield));
        } else {
            field = va_arg(ap,RedisModuleString*);
            if (field == NULL) break;
        }

        /* Query the hash for existence or value object. */
        if (flags & REDISMODULE_HASH_EXISTS) {
            existsptr = va_arg(ap,int*);
            if (key->value)
                *existsptr = hashTypeExists(key->value,field->ptr);
            else
                *existsptr = 0;
        } else {
            valueptr = va_arg(ap,RedisModuleString**);
            if (key->value) {
                *valueptr = hashTypeGetValueObject(key->value,field->ptr);
                if (*valueptr) {
                    robj *decoded = getDecodedObject(*valueptr);
                    decrRefCount(*valueptr);
                    *valueptr = decoded;
                }
                if (*valueptr)
                    autoMemoryAdd(key->ctx,REDISMODULE_AM_STRING,*valueptr);
            } else {
                *valueptr = NULL;
            }
        }

        /* Cleanup */
        if (flags & REDISMODULE_HASH_CFIELDS) decrRefCount(field);
    }
    va_end(ap);
    return REDISMODULE_OK;
}

/* --------------------------------------------------------------------------
 * ## Key API for Stream type
 *
 * For an introduction to streams, see https://redis.io/topics/streams-intro.
 *
 * The type RedisModuleStreamID, which is used in stream functions, is a struct
 * with two 64-bit fields and is defined as
 *
 *     typedef struct RedisModuleStreamID {
 *         uint64_t ms;
 *         uint64_t seq;
 *     } RedisModuleStreamID;
 *
 * See also RM_ValueLength(), which returns the length of a stream, and the
 * conversion functions RM_StringToStreamID() and RM_CreateStringFromStreamID().
 * -------------------------------------------------------------------------- */

/* Adds an entry to a stream. Like XADD without trimming.
 *
 * - `key`: The key where the stream is (or will be) stored
 * - `flags`: A bit field of
 *   - `REDISMODULE_STREAM_ADD_AUTOID`: Assign a stream ID automatically, like
 *     `*` in the XADD command.
 * - `id`: If the `AUTOID` flag is set, this is where the assigned ID is
 *   returned. Can be NULL if `AUTOID` is set, if you don't care to receive the
 *   ID. If `AUTOID` is not set, this is the requested ID.
 * - `argv`: A pointer to an array of size `numfields * 2` containing the
 *   fields and values.
 * - `numfields`: The number of field-value pairs in `argv`.
 *
 * Returns REDISMODULE_OK if an entry has been added. On failure,
 * REDISMODULE_ERR is returned and `errno` is set as follows:
 *
 * - EINVAL if called with invalid arguments
 * - ENOTSUP if the key refers to a value of a type other than stream
 * - EBADF if the key was not opened for writing
 * - EDOM if the given ID was 0-0 or not greater than all other IDs in the
 *   stream (only if the AUTOID flag is unset)
 * - EFBIG if the stream has reached the last possible ID
 * - ERANGE if the elements are too large to be stored.
 */
int RM_StreamAdd(RedisModuleKey *key, int flags, RedisModuleStreamID *id, RedisModuleString **argv, long numfields) {
    /* Validate args */
    if (!key || (numfields != 0 && !argv) || /* invalid key or argv */
        (flags & ~(REDISMODULE_STREAM_ADD_AUTOID)) || /* invalid flags */
        (!(flags & REDISMODULE_STREAM_ADD_AUTOID) && !id)) { /* id required */
        errno = EINVAL;
        return REDISMODULE_ERR;
    } else if (key->value && key->value->type != OBJ_STREAM) {
        errno = ENOTSUP; /* wrong type */
        return REDISMODULE_ERR;
    } else if (!(key->mode & REDISMODULE_WRITE)) {
        errno = EBADF; /* key not open for writing */
        return REDISMODULE_ERR;
    } else if (!(flags & REDISMODULE_STREAM_ADD_AUTOID) &&
               id->ms == 0 && id->seq == 0) {
        errno = EDOM; /* ID out of range */
        return REDISMODULE_ERR;
    }

    /* Create key if necessary */
    int created = 0;
    if (key->value == NULL) {
        moduleCreateEmptyKey(key, REDISMODULE_KEYTYPE_STREAM);
        created = 1;
    }

    stream *s = key->value->ptr;
    if (s->last_id.ms == UINT64_MAX && s->last_id.seq == UINT64_MAX) {
        /* The stream has reached the last possible ID */
        errno = EFBIG;
        return REDISMODULE_ERR;
    }

    streamID added_id;
    streamID use_id;
    streamID *use_id_ptr = NULL;
    if (!(flags & REDISMODULE_STREAM_ADD_AUTOID)) {
        use_id.ms = id->ms;
        use_id.seq = id->seq;
        use_id_ptr = &use_id;
    }

    if (streamAppendItem(s,argv,numfields,&added_id,use_id_ptr,1) == C_ERR) {
        /* Either the ID not greater than all existing IDs in the stream, or
         * the elements are too large to be stored. either way, errno is already
         * set by streamAppendItem. */
        return REDISMODULE_ERR;
    }
    /* Postponed signalKeyAsReady(). Done implicitly by moduleCreateEmptyKey()
     * so not needed if the stream has just been created. */
    if (!created) key->u.stream.signalready = 1;

    if (id != NULL) {
        id->ms = added_id.ms;
        id->seq = added_id.seq;
    }

    return REDISMODULE_OK;
}

/* Deletes an entry from a stream.
 *
 * - `key`: A key opened for writing, with no stream iterator started.
 * - `id`: The stream ID of the entry to delete.
 *
 * Returns REDISMODULE_OK on success. On failure, REDISMODULE_ERR is returned
 * and `errno` is set as follows:
 *
 * - EINVAL if called with invalid arguments
 * - ENOTSUP if the key refers to a value of a type other than stream or if the
 *   key is empty
 * - EBADF if the key was not opened for writing or if a stream iterator is
 *   associated with the key
 * - ENOENT if no entry with the given stream ID exists
 *
 * See also RM_StreamIteratorDelete() for deleting the current entry while
 * iterating using a stream iterator.
 */
int RM_StreamDelete(RedisModuleKey *key, RedisModuleStreamID *id) {
    if (!key || !id) {
        errno = EINVAL;
        return REDISMODULE_ERR;
    } else if (!key->value || key->value->type != OBJ_STREAM) {
        errno = ENOTSUP; /* wrong type */
        return REDISMODULE_ERR;
    } else if (!(key->mode & REDISMODULE_WRITE) ||
               key->iter != NULL) {
        errno = EBADF; /* key not opened for writing or iterator started */
        return REDISMODULE_ERR;
    }
    stream *s = key->value->ptr;
    streamID streamid = {id->ms, id->seq};
    if (streamDeleteItem(s, &streamid)) {
        return REDISMODULE_OK;
    } else {
        errno = ENOENT; /* no entry with this id */
        return REDISMODULE_ERR;
    }
}

/* Sets up a stream iterator.
 *
 * - `key`: The stream key opened for reading using RedisModule_OpenKey().
 * - `flags`:
 *   - `REDISMODULE_STREAM_ITERATOR_EXCLUSIVE`: Don't include `start` and `end`
 *     in the iterated range.
 *   - `REDISMODULE_STREAM_ITERATOR_REVERSE`: Iterate in reverse order, starting
 *     from the `end` of the range.
 * - `start`: The lower bound of the range. Use NULL for the beginning of the
 *   stream.
 * - `end`: The upper bound of the range. Use NULL for the end of the stream.
 *
 * Returns REDISMODULE_OK on success. On failure, REDISMODULE_ERR is returned
 * and `errno` is set as follows:
 *
 * - EINVAL if called with invalid arguments
 * - ENOTSUP if the key refers to a value of a type other than stream or if the
 *   key is empty
 * - EBADF if the key was not opened for writing or if a stream iterator is
 *   already associated with the key
 * - EDOM if `start` or `end` is outside the valid range
 *
 * Returns REDISMODULE_OK on success and REDISMODULE_ERR if the key doesn't
 * refer to a stream or if invalid arguments were given.
 *
 * The stream IDs are retrieved using RedisModule_StreamIteratorNextID() and
 * for each stream ID, the fields and values are retrieved using
 * RedisModule_StreamIteratorNextField(). The iterator is freed by calling
 * RedisModule_StreamIteratorStop().
 *
 * Example (error handling omitted):
 *
 *     RedisModule_StreamIteratorStart(key, 0, startid_ptr, endid_ptr);
 *     RedisModuleStreamID id;
 *     long numfields;
 *     while (RedisModule_StreamIteratorNextID(key, &id, &numfields) ==
 *            REDISMODULE_OK) {
 *         RedisModuleString *field, *value;
 *         while (RedisModule_StreamIteratorNextField(key, &field, &value) ==
 *                REDISMODULE_OK) {
 *             //
 *             // ... Do stuff ...
 *             //
 *             RedisModule_FreeString(ctx, field);
 *             RedisModule_FreeString(ctx, value);
 *         }
 *     }
 *     RedisModule_StreamIteratorStop(key);
 */
int RM_StreamIteratorStart(RedisModuleKey *key, int flags, RedisModuleStreamID *start, RedisModuleStreamID *end) {
    /* check args */
    if (!key ||
        (flags & ~(REDISMODULE_STREAM_ITERATOR_EXCLUSIVE |
                   REDISMODULE_STREAM_ITERATOR_REVERSE))) {
        errno = EINVAL; /* key missing or invalid flags */
        return REDISMODULE_ERR;
    } else if (!key->value || key->value->type != OBJ_STREAM) {
        errno = ENOTSUP;
        return REDISMODULE_ERR; /* not a stream */
    } else if (key->iter) {
        errno = EBADF; /* iterator already started */
        return REDISMODULE_ERR;
    }

    /* define range for streamIteratorStart() */
    streamID lower, upper;
    if (start) lower = (streamID){start->ms, start->seq};
    if (end)   upper = (streamID){end->ms,   end->seq};
    if (flags & REDISMODULE_STREAM_ITERATOR_EXCLUSIVE) {
        if ((start && streamIncrID(&lower) != C_OK) ||
            (end   && streamDecrID(&upper) != C_OK)) {
            errno = EDOM; /* end is 0-0 or start is MAX-MAX? */
            return REDISMODULE_ERR;
        }
    }

    /* create iterator */
    stream *s = key->value->ptr;
    int rev = flags & REDISMODULE_STREAM_ITERATOR_REVERSE;
    streamIterator *si = zmalloc(sizeof(*si));
    streamIteratorStart(si, s, start ? &lower : NULL, end ? &upper : NULL, rev);
    key->iter = si;
    key->u.stream.currentid.ms = 0; /* for RM_StreamIteratorDelete() */
    key->u.stream.currentid.seq = 0;
    key->u.stream.numfieldsleft = 0; /* for RM_StreamIteratorNextField() */
    return REDISMODULE_OK;
}

/* Stops a stream iterator created using RedisModule_StreamIteratorStart() and
 * reclaims its memory.
 *
 * Returns REDISMODULE_OK on success. On failure, REDISMODULE_ERR is returned
 * and `errno` is set as follows:
 *
 * - EINVAL if called with a NULL key
 * - ENOTSUP if the key refers to a value of a type other than stream or if the
 *   key is empty
 * - EBADF if the key was not opened for writing or if no stream iterator is
 *   associated with the key
 */
int RM_StreamIteratorStop(RedisModuleKey *key) {
    if (!key) {
        errno = EINVAL;
        return REDISMODULE_ERR;
    } else if (!key->value || key->value->type != OBJ_STREAM) {
        errno = ENOTSUP;
        return REDISMODULE_ERR;
    } else if (!key->iter) {
        errno = EBADF;
        return REDISMODULE_ERR;
    }
    streamIteratorStop(key->iter);
    zfree(key->iter);
    key->iter = NULL;
    return REDISMODULE_OK;
}

/* Finds the next stream entry and returns its stream ID and the number of
 * fields.
 *
 * - `key`: Key for which a stream iterator has been started using
 *   RedisModule_StreamIteratorStart().
 * - `id`: The stream ID returned. NULL if you don't care.
 * - `numfields`: The number of fields in the found stream entry. NULL if you
 *   don't care.
 *
 * Returns REDISMODULE_OK and sets `*id` and `*numfields` if an entry was found.
 * On failure, REDISMODULE_ERR is returned and `errno` is set as follows:
 *
 * - EINVAL if called with a NULL key
 * - ENOTSUP if the key refers to a value of a type other than stream or if the
 *   key is empty
 * - EBADF if no stream iterator is associated with the key
 * - ENOENT if there are no more entries in the range of the iterator
 *
 * In practice, if RM_StreamIteratorNextID() is called after a successful call
 * to RM_StreamIteratorStart() and with the same key, it is safe to assume that
 * an REDISMODULE_ERR return value means that there are no more entries.
 *
 * Use RedisModule_StreamIteratorNextField() to retrieve the fields and values.
 * See the example at RedisModule_StreamIteratorStart().
 */
int RM_StreamIteratorNextID(RedisModuleKey *key, RedisModuleStreamID *id, long *numfields) {
    if (!key) {
        errno = EINVAL;
        return REDISMODULE_ERR;
    } else if (!key->value || key->value->type != OBJ_STREAM) {
        errno = ENOTSUP;
        return REDISMODULE_ERR;
    } else if (!key->iter) {
        errno = EBADF;
        return REDISMODULE_ERR;
    }
    streamIterator *si = key->iter;
    int64_t *num_ptr = &key->u.stream.numfieldsleft;
    streamID *streamid_ptr = &key->u.stream.currentid;
    if (streamIteratorGetID(si, streamid_ptr, num_ptr)) {
        if (id) {
            id->ms = streamid_ptr->ms;
            id->seq = streamid_ptr->seq;
        }
        if (numfields) *numfields = *num_ptr;
        return REDISMODULE_OK;
    } else {
        /* No entry found. */
        key->u.stream.currentid.ms = 0; /* for RM_StreamIteratorDelete() */
        key->u.stream.currentid.seq = 0;
        key->u.stream.numfieldsleft = 0; /* for RM_StreamIteratorNextField() */
        errno = ENOENT;
        return REDISMODULE_ERR;
    }
}

/* Retrieves the next field of the current stream ID and its corresponding value
 * in a stream iteration. This function should be called repeatedly after calling
 * RedisModule_StreamIteratorNextID() to fetch each field-value pair.
 *
 * - `key`: Key where a stream iterator has been started.
 * - `field_ptr`: This is where the field is returned.
 * - `value_ptr`: This is where the value is returned.
 *
 * Returns REDISMODULE_OK and points `*field_ptr` and `*value_ptr` to freshly
 * allocated RedisModuleString objects. The string objects are freed
 * automatically when the callback finishes if automatic memory is enabled. On
 * failure, REDISMODULE_ERR is returned and `errno` is set as follows:
 *
 * - EINVAL if called with a NULL key
 * - ENOTSUP if the key refers to a value of a type other than stream or if the
 *   key is empty
 * - EBADF if no stream iterator is associated with the key
 * - ENOENT if there are no more fields in the current stream entry
 *
 * In practice, if RM_StreamIteratorNextField() is called after a successful
 * call to RM_StreamIteratorNextID() and with the same key, it is safe to assume
 * that an REDISMODULE_ERR return value means that there are no more fields.
 *
 * See the example at RedisModule_StreamIteratorStart().
 */
int RM_StreamIteratorNextField(RedisModuleKey *key, RedisModuleString **field_ptr, RedisModuleString **value_ptr) {
    if (!key) {
        errno = EINVAL;
        return REDISMODULE_ERR;
    } else if (!key->value || key->value->type != OBJ_STREAM) {
        errno = ENOTSUP;
        return REDISMODULE_ERR;
    } else if (!key->iter) {
        errno = EBADF;
        return REDISMODULE_ERR;
    } else if (key->u.stream.numfieldsleft <= 0) {
        errno = ENOENT;
        return REDISMODULE_ERR;
    }
    streamIterator *si = key->iter;
    unsigned char *field, *value;
    int64_t field_len, value_len;
    streamIteratorGetField(si, &field, &value, &field_len, &value_len);
    if (field_ptr) {
        *field_ptr = createRawStringObject((char *)field, field_len);
        autoMemoryAdd(key->ctx, REDISMODULE_AM_STRING, *field_ptr);
    }
    if (value_ptr) {
        *value_ptr = createRawStringObject((char *)value, value_len);
        autoMemoryAdd(key->ctx, REDISMODULE_AM_STRING, *value_ptr);
    }
    key->u.stream.numfieldsleft--;
    return REDISMODULE_OK;
}

/* Deletes the current stream entry while iterating.
 *
 * This function can be called after RM_StreamIteratorNextID() or after any
 * calls to RM_StreamIteratorNextField().
 *
 * Returns REDISMODULE_OK on success. On failure, REDISMODULE_ERR is returned
 * and `errno` is set as follows:
 *
 * - EINVAL if key is NULL
 * - ENOTSUP if the key is empty or is of another type than stream
 * - EBADF if the key is not opened for writing, if no iterator has been started
 * - ENOENT if the iterator has no current stream entry
 */
int RM_StreamIteratorDelete(RedisModuleKey *key) {
    if (!key) {
        errno = EINVAL;
        return REDISMODULE_ERR;
    } else if (!key->value || key->value->type != OBJ_STREAM) {
        errno = ENOTSUP;
        return REDISMODULE_ERR;
    } else if (!(key->mode & REDISMODULE_WRITE) || !key->iter) {
        errno = EBADF;
        return REDISMODULE_ERR;
    } else if (key->u.stream.currentid.ms == 0 &&
               key->u.stream.currentid.seq == 0) {
        errno = ENOENT;
        return REDISMODULE_ERR;
    }
    streamIterator *si = key->iter;
    streamIteratorRemoveEntry(si, &key->u.stream.currentid);
    key->u.stream.currentid.ms = 0; /* Make sure repeated Delete() fails */
    key->u.stream.currentid.seq = 0;
    key->u.stream.numfieldsleft = 0; /* Make sure NextField() fails */
    return REDISMODULE_OK;
}

/* Trim a stream by length, similar to XTRIM with MAXLEN.
 *
 * - `key`: Key opened for writing.
 * - `flags`: A bitfield of
 *   - `REDISMODULE_STREAM_TRIM_APPROX`: Trim less if it improves performance,
 *     like XTRIM with `~`.
 * - `length`: The number of stream entries to keep after trimming.
 *
 * Returns the number of entries deleted. On failure, a negative value is
 * returned and `errno` is set as follows:
 *
 * - EINVAL if called with invalid arguments
 * - ENOTSUP if the key is empty or of a type other than stream
 * - EBADF if the key is not opened for writing
 */
long long RM_StreamTrimByLength(RedisModuleKey *key, int flags, long long length) {
    if (!key || (flags & ~(REDISMODULE_STREAM_TRIM_APPROX)) || length < 0) {
        errno = EINVAL;
        return -1;
    } else if (!key->value || key->value->type != OBJ_STREAM) {
        errno = ENOTSUP;
        return -1;
    } else if (!(key->mode & REDISMODULE_WRITE)) {
        errno = EBADF;
        return -1;
    }
    int approx = flags & REDISMODULE_STREAM_TRIM_APPROX ? 1 : 0;
    return streamTrimByLength((stream *)key->value->ptr, length, approx);
}

/* Trim a stream by ID, similar to XTRIM with MINID.
 *
 * - `key`: Key opened for writing.
 * - `flags`: A bitfield of
 *   - `REDISMODULE_STREAM_TRIM_APPROX`: Trim less if it improves performance,
 *     like XTRIM with `~`.
 * - `id`: The smallest stream ID to keep after trimming.
 *
 * Returns the number of entries deleted. On failure, a negative value is
 * returned and `errno` is set as follows:
 *
 * - EINVAL if called with invalid arguments
 * - ENOTSUP if the key is empty or of a type other than stream
 * - EBADF if the key is not opened for writing
 */
long long RM_StreamTrimByID(RedisModuleKey *key, int flags, RedisModuleStreamID *id) {
    if (!key || (flags & ~(REDISMODULE_STREAM_TRIM_APPROX)) || !id) {
        errno = EINVAL;
        return -1;
    } else if (!key->value || key->value->type != OBJ_STREAM) {
        errno = ENOTSUP;
        return -1;
    } else if (!(key->mode & REDISMODULE_WRITE)) {
        errno = EBADF;
        return -1;
    }
    int approx = flags & REDISMODULE_STREAM_TRIM_APPROX ? 1 : 0;
    streamID minid = (streamID){id->ms, id->seq};
    return streamTrimByID((stream *)key->value->ptr, minid, approx);
}

/* --------------------------------------------------------------------------
 * ## Calling Redis commands from modules
 *
 * RM_Call() sends a command to Redis. The remaining functions handle the reply.
 * -------------------------------------------------------------------------- */


void moduleParseCallReply_Int(RedisModuleCallReply *reply);
void moduleParseCallReply_BulkString(RedisModuleCallReply *reply);
void moduleParseCallReply_SimpleString(RedisModuleCallReply *reply);
void moduleParseCallReply_Array(RedisModuleCallReply *reply);




/* Free a Call reply and all the nested replies it contains if it's an
 * array. */
void RM_FreeCallReply(RedisModuleCallReply *reply) {
    /* This is a wrapper for the recursive free reply function. This is needed
     * in order to have the first level function to return on nested replies,
     * but only if called by the module API. */

    RedisModuleCtx *ctx = NULL;
    if(callReplyType(reply) == REDISMODULE_REPLY_PROMISE) {
        RedisModuleAsyncRMCallPromise *promise = callReplyGetPrivateData(reply);
        ctx = promise->ctx;
        freeRedisModuleAsyncRMCallPromise(promise);
    } else {
        ctx = callReplyGetPrivateData(reply);
    }

    freeCallReply(reply);
    if (ctx) {
        autoMemoryFreed(ctx,REDISMODULE_AM_REPLY,reply);
    }
}

/* Return the reply type as one of the following:
 *
 * - REDISMODULE_REPLY_UNKNOWN
 * - REDISMODULE_REPLY_STRING
 * - REDISMODULE_REPLY_ERROR
 * - REDISMODULE_REPLY_INTEGER
 * - REDISMODULE_REPLY_ARRAY
 * - REDISMODULE_REPLY_NULL
 * - REDISMODULE_REPLY_MAP
 * - REDISMODULE_REPLY_SET
 * - REDISMODULE_REPLY_BOOL
 * - REDISMODULE_REPLY_DOUBLE
 * - REDISMODULE_REPLY_BIG_NUMBER
 * - REDISMODULE_REPLY_VERBATIM_STRING
 * - REDISMODULE_REPLY_ATTRIBUTE
 * - REDISMODULE_REPLY_PROMISE */
int RM_CallReplyType(RedisModuleCallReply *reply) {
    return callReplyType(reply);
}

/* Return the reply type length, where applicable. */
size_t RM_CallReplyLength(RedisModuleCallReply *reply) {
    return callReplyGetLen(reply);
}

/* Return the 'idx'-th nested call reply element of an array reply, or NULL
 * if the reply type is wrong or the index is out of range. */
RedisModuleCallReply *RM_CallReplyArrayElement(RedisModuleCallReply *reply, size_t idx) {
    return callReplyGetArrayElement(reply, idx);
}

/* Return the `long long` of an integer reply. */
long long RM_CallReplyInteger(RedisModuleCallReply *reply) {
    return callReplyGetLongLong(reply);
}

/* Return the double value of a double reply. */
double RM_CallReplyDouble(RedisModuleCallReply *reply) {
    return callReplyGetDouble(reply);
}

/* Return the big number value of a big number reply. */
const char *RM_CallReplyBigNumber(RedisModuleCallReply *reply, size_t *len) {
    return callReplyGetBigNumber(reply, len);
}

/* Return the value of a verbatim string reply,
 * An optional output argument can be given to get verbatim reply format. */
const char *RM_CallReplyVerbatim(RedisModuleCallReply *reply, size_t *len, const char **format) {
    return callReplyGetVerbatim(reply, len, format);
}

/* Return the Boolean value of a Boolean reply. */
int RM_CallReplyBool(RedisModuleCallReply *reply) {
    return callReplyGetBool(reply);
}

/* Return the 'idx'-th nested call reply element of a set reply, or NULL
 * if the reply type is wrong or the index is out of range. */
RedisModuleCallReply *RM_CallReplySetElement(RedisModuleCallReply *reply, size_t idx) {
    return callReplyGetSetElement(reply, idx);
}

/* Retrieve the 'idx'-th key and value of a map reply.
 *
 * Returns:
 * - REDISMODULE_OK on success.
 * - REDISMODULE_ERR if idx out of range or if the reply type is wrong.
 *
 * The `key` and `value` arguments are used to return by reference, and may be
 * NULL if not required. */
int RM_CallReplyMapElement(RedisModuleCallReply *reply, size_t idx, RedisModuleCallReply **key, RedisModuleCallReply **val) {
    if (callReplyGetMapElement(reply, idx, key, val) == C_OK){
        return REDISMODULE_OK;
    }
    return REDISMODULE_ERR;
}

/* Return the attribute of the given reply, or NULL if no attribute exists. */
RedisModuleCallReply *RM_CallReplyAttribute(RedisModuleCallReply *reply) {
    return callReplyGetAttribute(reply);
}

/* Retrieve the 'idx'-th key and value of an attribute reply.
 *
 * Returns:
 * - REDISMODULE_OK on success.
 * - REDISMODULE_ERR if idx out of range or if the reply type is wrong.
 *
 * The `key` and `value` arguments are used to return by reference, and may be
 * NULL if not required. */
int RM_CallReplyAttributeElement(RedisModuleCallReply *reply, size_t idx, RedisModuleCallReply **key, RedisModuleCallReply **val) {
    if (callReplyGetAttributeElement(reply, idx, key, val) == C_OK){
        return REDISMODULE_OK;
    }
    return REDISMODULE_ERR;
}

/* Set unblock handler (callback and private data) on the given promise RedisModuleCallReply.
 * The given reply must be of promise type (REDISMODULE_REPLY_PROMISE). */
void RM_CallReplyPromiseSetUnblockHandler(RedisModuleCallReply *reply, RedisModuleOnUnblocked on_unblock, void *private_data) {
    RedisModuleAsyncRMCallPromise *promise = callReplyGetPrivateData(reply);
    promise->on_unblocked = on_unblock;
    promise->private_data = private_data;
}

/* Abort the execution of a given promise RedisModuleCallReply.
 * return REDMODULE_OK in case the abort was done successfully and REDISMODULE_ERR
 * if its not possible to abort the execution (execution already finished).
 * In case the execution was aborted (REDMODULE_OK was returned), the private_data out parameter
 * will be set with the value of the private data that was given on 'RM_CallReplyPromiseSetUnblockHandler'
 * so the caller will be able to release the private data.
 *
 * If the execution was aborted successfully, it is promised that the unblock handler will not be called.
 * That said, it is possible that the abort operation will successes but the operation will still continue.
 * This can happened if, for example, a module implements some blocking command and does not respect the
 * disconnect callback. For pure Redis commands this can not happened.*/
int RM_CallReplyPromiseAbort(RedisModuleCallReply *reply, void **private_data) {
    RedisModuleAsyncRMCallPromise *promise = callReplyGetPrivateData(reply);
    if (!promise->c) return REDISMODULE_ERR; /* Promise can not be aborted, either already aborted or already finished. */
    if (!(promise->c->flags & CLIENT_BLOCKED)) return REDISMODULE_ERR; /* Client is not blocked anymore, can not abort it. */

    /* Client is still blocked, remove it from any blocking state and release it. */
    if (private_data) *private_data = promise->private_data;
    promise->private_data = NULL;
    promise->on_unblocked = NULL;
    unblockClient(promise->c, 0);
    moduleReleaseTempClient(promise->c);
    return REDISMODULE_OK;
}

/* Return the pointer and length of a string or error reply. */
const char *RM_CallReplyStringPtr(RedisModuleCallReply *reply, size_t *len) {
    size_t private_len;
    if (!len) len = &private_len;
    return callReplyGetString(reply, len);
}

/* Return a new string object from a call reply of type string, error or
 * integer. Otherwise (wrong reply type) return NULL. */
RedisModuleString *RM_CreateStringFromCallReply(RedisModuleCallReply *reply) {
    RedisModuleCtx* ctx = callReplyGetPrivateData(reply);
    size_t len;
    const char *str;
    switch(callReplyType(reply)) {
        case REDISMODULE_REPLY_STRING:
        case REDISMODULE_REPLY_ERROR:
            str = callReplyGetString(reply, &len);
            return RM_CreateString(ctx, str, len);
        case REDISMODULE_REPLY_INTEGER: {
            char buf[64];
            int len = ll2string(buf,sizeof(buf),callReplyGetLongLong(reply));
            return RM_CreateString(ctx ,buf,len);
            }
        default:
            return NULL;
    }
}

/* Modifies the user that RM_Call will use (e.g. for ACL checks) */
void RM_SetContextUser(RedisModuleCtx *ctx, const RedisModuleUser *user) {
    ctx->user = user;
}

/* Returns an array of robj pointers, by parsing the format specifier "fmt" as described for
 * the RM_Call(), RM_Replicate() and other module APIs. Populates *argcp with the number of
 * items (which equals to the length of the allocated argv).
 *
 * The integer pointed by 'flags' is populated with flags according
 * to special modifiers in "fmt".
 *
 *     "!" -> REDISMODULE_ARGV_REPLICATE
 *     "A" -> REDISMODULE_ARGV_NO_AOF
 *     "R" -> REDISMODULE_ARGV_NO_REPLICAS
 *     "3" -> REDISMODULE_ARGV_RESP_3
 *     "0" -> REDISMODULE_ARGV_RESP_AUTO
 *     "C" -> REDISMODULE_ARGV_RUN_AS_USER
 *     "M" -> REDISMODULE_ARGV_RESPECT_DENY_OOM
 *     "K" -> REDISMODULE_ARGV_ALLOW_BLOCK
 *
 * On error (format specifier error) NULL is returned and nothing is
 * allocated. On success the argument vector is returned. */
robj **moduleCreateArgvFromUserFormat(const char *cmdname, const char *fmt, int *argcp, int *flags, va_list ap) {
    int argc = 0, argv_size, j;
    robj **argv = NULL;

    /* As a first guess to avoid useless reallocations, size argv to
     * hold one argument for each char specifier in 'fmt'. */
    argv_size = strlen(fmt)+1; /* +1 because of the command name. */
    argv = zrealloc(argv,sizeof(robj*)*argv_size);

    /* Build the arguments vector based on the format specifier. */
    argv[0] = createStringObject(cmdname,strlen(cmdname));
    argc++;

    /* Create the client and dispatch the command. */
    const char *p = fmt;
    while(*p) {
        if (*p == 'c') {
            char *cstr = va_arg(ap,char*);
            argv[argc++] = createStringObject(cstr,strlen(cstr));
        } else if (*p == 's') {
            robj *obj = va_arg(ap,void*);
            if (obj->refcount == OBJ_STATIC_REFCOUNT)
                obj = createStringObject(obj->ptr,sdslen(obj->ptr));
            else
                incrRefCount(obj);
            argv[argc++] = obj;
        } else if (*p == 'b') {
            char *buf = va_arg(ap,char*);
            size_t len = va_arg(ap,size_t);
            argv[argc++] = createStringObject(buf,len);
        } else if (*p == 'l') {
            long long ll = va_arg(ap,long long);
            argv[argc++] = createObject(OBJ_STRING,sdsfromlonglong(ll));
        } else if (*p == 'v') {
             /* A vector of strings */
             robj **v = va_arg(ap, void*);
             size_t vlen = va_arg(ap, size_t);

             /* We need to grow argv to hold the vector's elements.
              * We resize by vector_len-1 elements, because we held
              * one element in argv for the vector already */
             argv_size += vlen-1;
             argv = zrealloc(argv,sizeof(robj*)*argv_size);

             size_t i = 0;
             for (i = 0; i < vlen; i++) {
                 incrRefCount(v[i]);
                 argv[argc++] = v[i];
             }
        } else if (*p == '!') {
            if (flags) (*flags) |= REDISMODULE_ARGV_REPLICATE;
        } else if (*p == 'A') {
            if (flags) (*flags) |= REDISMODULE_ARGV_NO_AOF;
        } else if (*p == 'R') {
            if (flags) (*flags) |= REDISMODULE_ARGV_NO_REPLICAS;
        } else if (*p == '3') {
            if (flags) (*flags) |= REDISMODULE_ARGV_RESP_3;
        } else if (*p == '0') {
            if (flags) (*flags) |= REDISMODULE_ARGV_RESP_AUTO;
        } else if (*p == 'C') {
            if (flags) (*flags) |= REDISMODULE_ARGV_RUN_AS_USER;
        } else if (*p == 'S') {
            if (flags) (*flags) |= REDISMODULE_ARGV_SCRIPT_MODE;
        } else if (*p == 'W') {
            if (flags) (*flags) |= REDISMODULE_ARGV_NO_WRITES;
        } else if (*p == 'M') {
            if (flags) (*flags) |= REDISMODULE_ARGV_RESPECT_DENY_OOM;
        } else if (*p == 'E') {
            if (flags) (*flags) |= REDISMODULE_ARGV_CALL_REPLIES_AS_ERRORS;
        } else if (*p == 'D') {
            if (flags) (*flags) |= (REDISMODULE_ARGV_DRY_RUN | REDISMODULE_ARGV_CALL_REPLIES_AS_ERRORS);
        } else if (*p == 'K') {
            if (flags) (*flags) |= REDISMODULE_ARGV_ALLOW_BLOCK;
        } else {
            goto fmterr;
        }
        p++;
    }
    if (argcp) *argcp = argc;
    return argv;

fmterr:
    for (j = 0; j < argc; j++)
        decrRefCount(argv[j]);
    zfree(argv);
    return NULL;
}

/* Exported API to call any Redis command from modules.
 *
 * * **cmdname**: The Redis command to call.
 * * **fmt**: A format specifier string for the command's arguments. Each
 *   of the arguments should be specified by a valid type specification. The
 *   format specifier can also contain the modifiers `!`, `A`, `3` and `R` which
 *   don't have a corresponding argument.
 *
 *     * `b` -- The argument is a buffer and is immediately followed by another
 *              argument that is the buffer's length.
 *     * `c` -- The argument is a pointer to a plain C string (null-terminated).
 *     * `l` -- The argument is a `long long` integer.
 *     * `s` -- The argument is a RedisModuleString.
 *     * `v` -- The argument(s) is a vector of RedisModuleString.
 *     * `!` -- Sends the Redis command and its arguments to replicas and AOF.
 *     * `A` -- Suppress AOF propagation, send only to replicas (requires `!`).
 *     * `R` -- Suppress replicas propagation, send only to AOF (requires `!`).
 *     * `3` -- Return a RESP3 reply. This will change the command reply.
 *              e.g., HGETALL returns a map instead of a flat array.
 *     * `0` -- Return the reply in auto mode, i.e. the reply format will be the
 *              same as the client attached to the given RedisModuleCtx. This will
 *              probably used when you want to pass the reply directly to the client.
 *     * `C` -- Run a command as the user attached to the context.
 *              User is either attached automatically via the client that directly
 *              issued the command and created the context or via RM_SetContextUser.
 *              If the context is not directly created by an issued command (such as a
 *              background context and no user was set on it via RM_SetContextUser,
 *              RM_Call will fail.
 *              Checks if the command can be executed according to ACL rules and causes
 *              the command to run as the determined user, so that any future user
 *              dependent activity, such as ACL checks within scripts will proceed as
 *              expected.
 *              Otherwise, the command will run as the Redis unrestricted user.
 *     * `S` -- Run the command in a script mode, this means that it will raise
 *              an error if a command which are not allowed inside a script
 *              (flagged with the `deny-script` flag) is invoked (like SHUTDOWN).
 *              In addition, on script mode, write commands are not allowed if there are
 *              not enough good replicas (as configured with `min-replicas-to-write`)
 *              or when the server is unable to persist to the disk.
 *     * `W` -- Do not allow to run any write command (flagged with the `write` flag).
 *     * `M` -- Do not allow `deny-oom` flagged commands when over the memory limit.
 *     * `E` -- Return error as RedisModuleCallReply. If there is an error before
 *              invoking the command, the error is returned using errno mechanism.
 *              This flag allows to get the error also as an error CallReply with
 *              relevant error message.
 *     * 'D' -- A "Dry Run" mode. Return before executing the underlying call().
 *              If everything succeeded, it will return with a NULL, otherwise it will
 *              return with a CallReply object denoting the error, as if it was called with
 *              the 'E' code.
 *     * 'K' -- Allow running blocking commands. If enabled and the command gets blocked, a
 *              special REDISMODULE_REPLY_PROMISE will be returned. This reply type
 *              indicates that the command was blocked and the reply will be given asynchronously.
 *              The module can use this reply object to set a handler which will be called when
 *              the command gets unblocked using RedisModule_CallReplyPromiseSetUnblockHandler.
 *              The handler must be set immediately after the command invocation (without releasing
 *              the Redis lock in between). If the handler is not set, the blocking command will
 *              still continue its execution but the reply will be ignored (fire and forget),
 *              notice that this is dangerous in case of role change, as explained below.
 *              The module can use RedisModule_CallReplyPromiseAbort to abort the command invocation
 *              if it was not yet finished (see RedisModule_CallReplyPromiseAbort documentation for more
 *              details). It is also the module's responsibility to abort the execution on role change, either by using
 *              server event (to get notified when the instance becomes a replica) or relying on the disconnect
 *              callback of the original client. Failing to do so can result in a write operation on a replica.
 *              Unlike other call replies, promise call reply **must** be freed while the Redis GIL is locked.
 *              Notice that on unblocking, the only promise is that the unblock handler will be called,
 *              If the blocking RM_Call caused the module to also block some real client (using RM_BlockClient),
 *              it is the module responsibility to unblock this client on the unblock handler.
 *              On the unblock handler it is only allowed to perform the following:
 *              * Calling additional Redis commands using RM_Call
 *              * Open keys using RM_OpenKey
 *              * Replicate data to the replica or AOF
 *
 *              Specifically, it is not allowed to call any Redis module API which are client related such as:
 *              * RM_Reply* API's
 *              * RM_BlockClient
 *              * RM_GetCurrentUserName
 *
 * * **...**: The actual arguments to the Redis command.
 *
 * On success a RedisModuleCallReply object is returned, otherwise
 * NULL is returned and errno is set to the following values:
 *
 * * EBADF: wrong format specifier.
 * * EINVAL: wrong command arity.
 * * ENOENT: command does not exist.
 * * EPERM: operation in Cluster instance with key in non local slot.
 * * EROFS: operation in Cluster instance when a write command is sent
 *          in a readonly state.
 * * ENETDOWN: operation in Cluster instance when cluster is down.
 * * ENOTSUP: No ACL user for the specified module context
 * * EACCES: Command cannot be executed, according to ACL rules
 * * ENOSPC: Write or deny-oom command is not allowed
 * * ESPIPE: Command not allowed on script mode
 *
 * Example code fragment:
 *
 *      reply = RedisModule_Call(ctx,"INCRBY","sc",argv[1],"10");
 *      if (RedisModule_CallReplyType(reply) == REDISMODULE_REPLY_INTEGER) {
 *        long long myval = RedisModule_CallReplyInteger(reply);
 *        // Do something with myval.
 *      }
 *
 * This API is documented here: https://redis.io/topics/modules-intro
 */
RedisModuleCallReply *RM_Call(RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...) {
    client *c = NULL;
    robj **argv = NULL;
    int argc = 0, flags = 0;
    va_list ap;
    RedisModuleCallReply *reply = NULL;
    int replicate = 0; /* Replicate this command? */
    int error_as_call_replies = 0; /* return errors as RedisModuleCallReply object */
    uint64_t cmd_flags;

    /* Handle arguments. */
    va_start(ap, fmt);
    argv = moduleCreateArgvFromUserFormat(cmdname,fmt,&argc,&flags,ap);
    replicate = flags & REDISMODULE_ARGV_REPLICATE;
    error_as_call_replies = flags & REDISMODULE_ARGV_CALL_REPLIES_AS_ERRORS;
    va_end(ap);

    user *user = NULL;
    if (flags & REDISMODULE_ARGV_RUN_AS_USER) {
        user = ctx->user ? ctx->user->user : ctx->client->user;
        if (!user) {
            errno = ENOTSUP;
            if (error_as_call_replies) {
                sds msg = sdsnew("cannot run as user, no user directly attached to context or context's client");
                reply = callReplyCreateError(msg, ctx);
            }
            return reply;
        }
    }

    c = moduleAllocTempClient(user);

    if (!(flags & REDISMODULE_ARGV_ALLOW_BLOCK)) {
        /* We do not want to allow block, the module do not expect it */
        c->flags |= CLIENT_DENY_BLOCKING;
    }
    c->db = ctx->client->db;
    c->argv = argv;
    /* We have to assign argv_len, which is equal to argc in that case (RM_Call)
     * because we may be calling a command that uses rewriteClientCommandArgument */
    c->argc = c->argv_len = argc;
    c->resp = 2;
    if (flags & REDISMODULE_ARGV_RESP_3) {
        c->resp = 3;
    } else if (flags & REDISMODULE_ARGV_RESP_AUTO) {
        /* Auto mode means to take the same protocol as the ctx client. */
        c->resp = ctx->client->resp;
    }
    if (ctx->module) ctx->module->in_call++;

    /* We handle the above format error only when the client is setup so that
     * we can free it normally. */
    if (argv == NULL) {
        /* We do not return a call reply here this is an error that should only
         * be catch by the module indicating wrong fmt was given, the module should
         * handle this error and decide how to continue. It is not an error that
         * should be propagated to the user. */
        errno = EBADF;
        goto cleanup;
    }

    /* Call command filters */
    moduleCallCommandFilters(c);

    /* Lookup command now, after filters had a chance to make modifications
     * if necessary.
     */
    c->cmd = c->lastcmd = c->realcmd = lookupCommand(c->argv,c->argc);
    sds err;
    if (!commandCheckExistence(c, error_as_call_replies? &err : NULL)) {
        errno = ENOENT;
        if (error_as_call_replies)
            reply = callReplyCreateError(err, ctx);
        goto cleanup;
    }
    if (!commandCheckArity(c, error_as_call_replies? &err : NULL)) {
        errno = EINVAL;
        if (error_as_call_replies)
            reply = callReplyCreateError(err, ctx);
        goto cleanup;
    }

    cmd_flags = getCommandFlags(c);

    if (flags & REDISMODULE_ARGV_SCRIPT_MODE) {
        /* Basically on script mode we want to only allow commands that can
         * be executed on scripts (CMD_NOSCRIPT is not set on the command flags) */
        if (cmd_flags & CMD_NOSCRIPT) {
            errno = ESPIPE;
            if (error_as_call_replies) {
                sds msg = sdscatfmt(sdsempty(), "command '%S' is not allowed on script mode", c->cmd->fullname);
                reply = callReplyCreateError(msg, ctx);
            }
            goto cleanup;
        }
    }

    if (flags & REDISMODULE_ARGV_RESPECT_DENY_OOM && server.maxmemory) {
        if (cmd_flags & CMD_DENYOOM) {
            int oom_state;
            if (ctx->flags & REDISMODULE_CTX_THREAD_SAFE) {
                /* On background thread we can not count on server.pre_command_oom_state.
                 * Because it is only set on the main thread, in such case we will check
                 * the actual memory usage. */
                oom_state = (getMaxmemoryState(NULL,NULL,NULL,NULL) == C_ERR);
            } else {
                oom_state = server.pre_command_oom_state;
            }
            if (oom_state) {
                errno = ENOSPC;
                if (error_as_call_replies) {
                    sds msg = sdsdup(shared.oomerr->ptr);
                    reply = callReplyCreateError(msg, ctx);
                }
                goto cleanup;
            }
        }
    } else {
        /* if we aren't OOM checking in RM_Call, we want further executions from this client to also not fail on OOM */
        c->flags |= CLIENT_ALLOW_OOM;
    }

    if (flags & REDISMODULE_ARGV_NO_WRITES) {
        if (cmd_flags & CMD_WRITE) {
            errno = ENOSPC;
            if (error_as_call_replies) {
                sds msg = sdscatfmt(sdsempty(), "Write command '%S' was "
                                                "called while write is not allowed.", c->cmd->fullname);
                reply = callReplyCreateError(msg, ctx);
            }
            goto cleanup;
        }
    }

    /* Script mode tests */
    if (flags & REDISMODULE_ARGV_SCRIPT_MODE) {
        if (cmd_flags & CMD_WRITE) {
            /* on script mode, if a command is a write command,
             * We will not run it if we encounter disk error
             * or we do not have enough replicas */

            if (!checkGoodReplicasStatus()) {
                errno = ESPIPE;
                if (error_as_call_replies) {
                    sds msg = sdsdup(shared.noreplicaserr->ptr);
                    reply = callReplyCreateError(msg, ctx);
                }
                goto cleanup;
            }

            int deny_write_type = writeCommandsDeniedByDiskError();
            int obey_client = (server.current_client && mustObeyClient(server.current_client));

            if (deny_write_type != DISK_ERROR_TYPE_NONE && !obey_client) {
                errno = ESPIPE;
                if (error_as_call_replies) {
                    sds msg = writeCommandsGetDiskErrorMessage(deny_write_type);
                    reply = callReplyCreateError(msg, ctx);
                }
                goto cleanup;
            }

            if (server.masterhost && server.repl_slave_ro && !obey_client) {
                errno = ESPIPE;
                if (error_as_call_replies) {
                    sds msg = sdsdup(shared.roslaveerr->ptr);
                    reply = callReplyCreateError(msg, ctx);
                }
                goto cleanup;
            }
        }

        if (server.masterhost && server.repl_state != REPL_STATE_CONNECTED &&
            server.repl_serve_stale_data == 0 && !(cmd_flags & CMD_STALE)) {
            errno = ESPIPE;
            if (error_as_call_replies) {
                sds msg = sdsdup(shared.masterdownerr->ptr);
                reply = callReplyCreateError(msg, ctx);
            }
            goto cleanup;
        }
    }

    /* Check if the user can run this command according to the current
     * ACLs.
     *
     * If RM_SetContextUser has set a user, that user is used, otherwise
     * use the attached client's user. If there is no attached client user and no manually
     * set user, an error will be returned */
    if (flags & REDISMODULE_ARGV_RUN_AS_USER) {
        int acl_errpos;
        int acl_retval;

        acl_retval = ACLCheckAllUserCommandPerm(user,c->cmd,c->argv,c->argc,&acl_errpos);
        if (acl_retval != ACL_OK) {
            sds object = (acl_retval == ACL_DENIED_CMD) ? sdsdup(c->cmd->fullname) : sdsdup(c->argv[acl_errpos]->ptr);
            addACLLogEntry(ctx->client, acl_retval, ACL_LOG_CTX_MODULE, -1, c->user->name, object);
            if (error_as_call_replies) {
                /* verbosity should be same as processCommand() in server.c */
                sds acl_msg = getAclErrorMessage(acl_retval, c->user, c->cmd, c->argv[acl_errpos]->ptr, 0);
                sds msg = sdscatfmt(sdsempty(), "-NOPERM %S\r\n", acl_msg);
                sdsfree(acl_msg);
                reply = callReplyCreateError(msg, ctx);
            }
            errno = EACCES;
            goto cleanup;
        }
    }

    /* If this is a Redis Cluster node, we need to make sure the module is not
     * trying to access non-local keys, with the exception of commands
     * received from our master. */
    if (server.cluster_enabled && !mustObeyClient(ctx->client)) {
        int error_code;
        /* Duplicate relevant flags in the module client. */
        c->flags &= ~(CLIENT_READONLY|CLIENT_ASKING);
        c->flags |= ctx->client->flags & (CLIENT_READONLY|CLIENT_ASKING);
        if (getNodeByQuery(c,c->cmd,c->argv,c->argc,NULL,&error_code) !=
                           server.cluster->myself)
        {
            sds msg = NULL;
            if (error_code == CLUSTER_REDIR_DOWN_RO_STATE) {
                if (error_as_call_replies) {
                    msg = sdscatfmt(sdsempty(), "Can not execute a write command '%S' while the cluster is down and readonly", c->cmd->fullname);
                }
                errno = EROFS;
            } else if (error_code == CLUSTER_REDIR_DOWN_STATE) {
                if (error_as_call_replies) {
                    msg = sdscatfmt(sdsempty(), "Can not execute a command '%S' while the cluster is down", c->cmd->fullname);
                }
                errno = ENETDOWN;
            } else {
                if (error_as_call_replies) {
                    msg = sdsnew("Attempted to access a non local key in a cluster node");
                }
                errno = EPERM;
            }
            if (msg) {
                reply = callReplyCreateError(msg, ctx);
            }
            goto cleanup;
        }
    }

    if (flags & REDISMODULE_ARGV_DRY_RUN) {
        goto cleanup;
    }

    /* We need to use a global replication_allowed flag in order to prevent
     * replication of nested RM_Calls. Example:
     * 1. module1.foo does RM_Call of module2.bar without replication (i.e. no '!')
     * 2. module2.bar internally calls RM_Call of INCR with '!'
     * 3. at the end of module1.foo we call RM_ReplicateVerbatim
     * We want the replica/AOF to see only module1.foo and not the INCR from module2.bar */
    int prev_replication_allowed = server.replication_allowed;
    server.replication_allowed = replicate && server.replication_allowed;

    /* Run the command */
    int call_flags = CMD_CALL_FROM_MODULE;
    if (replicate) {
        if (!(flags & REDISMODULE_ARGV_NO_AOF))
            call_flags |= CMD_CALL_PROPAGATE_AOF;
        if (!(flags & REDISMODULE_ARGV_NO_REPLICAS))
            call_flags |= CMD_CALL_PROPAGATE_REPL;
    }
    call(c,call_flags);
    server.replication_allowed = prev_replication_allowed;

    if (c->flags & CLIENT_BLOCKED) {
        serverAssert(flags & REDISMODULE_ARGV_ALLOW_BLOCK);
        serverAssert(ctx->module);
        RedisModuleAsyncRMCallPromise *promise = zmalloc(sizeof(RedisModuleAsyncRMCallPromise));
        *promise = (RedisModuleAsyncRMCallPromise) {
                /* We start with ref_count value of 2 because this object is held
                 * by the promise CallReply and the fake client that was used to execute the command. */
                .ref_count = 2,
                .module = ctx->module,
                .on_unblocked = NULL,
                .private_data = NULL,
                .c = c,
                .ctx = (ctx->flags & REDISMODULE_CTX_AUTO_MEMORY) ? ctx : NULL,
        };
        reply = callReplyCreatePromise(promise);
        c->bstate.async_rm_call_handle = promise;
        if (!(call_flags & CMD_CALL_PROPAGATE_AOF)) {
            /* No need for AOF propagation, set the relevant flags of the client */
            c->flags |= CLIENT_MODULE_PREVENT_AOF_PROP;
        }
        if (!(call_flags & CMD_CALL_PROPAGATE_REPL)) {
            /* No need for replication propagation, set the relevant flags of the client */
            c->flags |= CLIENT_MODULE_PREVENT_REPL_PROP;
        }
        c = NULL; /* Make sure not to free the client */
    } else {
        reply = moduleParseReply(c, (ctx->flags & REDISMODULE_CTX_AUTO_MEMORY) ? ctx : NULL);
    }

cleanup:
    if (reply) autoMemoryAdd(ctx,REDISMODULE_AM_REPLY,reply);
    if (ctx->module) ctx->module->in_call--;
    if (c) moduleReleaseTempClient(c);
    return reply;
}

/* Return a pointer, and a length, to the protocol returned by the command
 * that returned the reply object. */
const char *RM_CallReplyProto(RedisModuleCallReply *reply, size_t *len) {
    return callReplyGetProto(reply, len);
}

/* --------------------------------------------------------------------------
 * ## Modules data types
 *
 * When String DMA or using existing data structures is not enough, it is
 * possible to create new data types from scratch and export them to
 * Redis. The module must provide a set of callbacks for handling the
 * new values exported (for example in order to provide RDB saving/loading,
 * AOF rewrite, and so forth). In this section we define this API.
 * -------------------------------------------------------------------------- */

/* Turn a 9 chars name in the specified charset and a 10 bit encver into
 * a single 64 bit unsigned integer that represents this exact module name
 * and version. This final number is called a "type ID" and is used when
 * writing module exported values to RDB files, in order to re-associate the
 * value to the right module to load them during RDB loading.
 *
 * If the string is not of the right length or the charset is wrong, or
 * if encver is outside the unsigned 10 bit integer range, 0 is returned,
 * otherwise the function returns the right type ID.
 *
 * The resulting 64 bit integer is composed as follows:
 *
 *     (high order bits) 6|6|6|6|6|6|6|6|6|10 (low order bits)
 *
 * The first 6 bits value is the first character, name[0], while the last
 * 6 bits value, immediately before the 10 bits integer, is name[8].
 * The last 10 bits are the encoding version.
 *
 * Note that a name and encver combo of "AAAAAAAAA" and 0, will produce
 * zero as return value, that is the same we use to signal errors, thus
 * this combination is invalid, and also useless since type names should
 * try to be vary to avoid collisions. */

const char *ModuleTypeNameCharSet =
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrstuvwxyz"
             "0123456789-_";

uint64_t moduleTypeEncodeId(const char *name, int encver) {
    /* We use 64 symbols so that we can map each character into 6 bits
     * of the final output. */
    const char *cset = ModuleTypeNameCharSet;
    if (strlen(name) != 9) return 0;
    if (encver < 0 || encver > 1023) return 0;

    uint64_t id = 0;
    for (int j = 0; j < 9; j++) {
        char *p = strchr(cset,name[j]);
        if (!p) return 0;
        unsigned long pos = p-cset;
        id = (id << 6) | pos;
    }
    id = (id << 10) | encver;
    return id;
}

/* Search, in the list of exported data types of all the modules registered,
 * a type with the same name as the one given. Returns the moduleType
 * structure pointer if such a module is found, or NULL otherwise. */
moduleType *moduleTypeLookupModuleByName(const char *name) {
    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;

    while ((de = dictNext(di)) != NULL) {
        struct RedisModule *module = dictGetVal(de);
        listIter li;
        listNode *ln;

        listRewind(module->types,&li);
        while((ln = listNext(&li))) {
            moduleType *mt = ln->value;
            if (memcmp(name,mt->name,sizeof(mt->name)) == 0) {
                dictReleaseIterator(di);
                return mt;
            }
        }
    }
    dictReleaseIterator(di);
    return NULL;
}

/* Lookup a module by ID, with caching. This function is used during RDB
 * loading. Modules exporting data types should never be able to unload, so
 * our cache does not need to expire. */
#define MODULE_LOOKUP_CACHE_SIZE 3

moduleType *moduleTypeLookupModuleByID(uint64_t id) {
    static struct {
        uint64_t id;
        moduleType *mt;
    } cache[MODULE_LOOKUP_CACHE_SIZE];

    /* Search in cache to start. */
    int j;
    for (j = 0; j < MODULE_LOOKUP_CACHE_SIZE && cache[j].mt != NULL; j++)
        if (cache[j].id == id) return cache[j].mt;

    /* Slow module by module lookup. */
    moduleType *mt = NULL;
    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;

    while ((de = dictNext(di)) != NULL && mt == NULL) {
        struct RedisModule *module = dictGetVal(de);
        listIter li;
        listNode *ln;

        listRewind(module->types,&li);
        while((ln = listNext(&li))) {
            moduleType *this_mt = ln->value;
            /* Compare only the 54 bit module identifier and not the
             * encoding version. */
            if (this_mt->id >> 10 == id >> 10) {
                mt = this_mt;
                break;
            }
        }
    }
    dictReleaseIterator(di);

    /* Add to cache if possible. */
    if (mt && j < MODULE_LOOKUP_CACHE_SIZE) {
        cache[j].id = id;
        cache[j].mt = mt;
    }
    return mt;
}

/* Turn an (unresolved) module ID into a type name, to show the user an
 * error when RDB files contain module data we can't load.
 * The buffer pointed by 'name' must be 10 bytes at least. The function will
 * fill it with a null terminated module name. */
void moduleTypeNameByID(char *name, uint64_t moduleid) {
    const char *cset = ModuleTypeNameCharSet;

    name[9] = '\0';
    char *p = name+8;
    moduleid >>= 10;
    for (int j = 0; j < 9; j++) {
        *p-- = cset[moduleid & 63];
        moduleid >>= 6;
    }
}

/* Return the name of the module that owns the specified moduleType. */
const char *moduleTypeModuleName(moduleType *mt) {
    if (!mt || !mt->module) return NULL;
    return mt->module->name;
}

/* Return the module name from a module command */
const char *moduleNameFromCommand(struct redisCommand *cmd) {
    serverAssert(cmd->proc == RedisModuleCommandDispatcher);

    RedisModuleCommand *cp = cmd->module_cmd;
    return cp->module->name;
}

/* Create a copy of a module type value using the copy callback. If failed
 * or not supported, produce an error reply and return NULL.
 */
robj *moduleTypeDupOrReply(client *c, robj *fromkey, robj *tokey, int todb, robj *value) {
    moduleValue *mv = value->ptr;
    moduleType *mt = mv->type;
    if (!mt->copy && !mt->copy2) {
        addReplyError(c, "not supported for this module key");
        return NULL;
    }
    void *newval = NULL;
    if (mt->copy2 != NULL) {
        RedisModuleKeyOptCtx ctx = {fromkey, tokey, c->db->id, todb};
        newval = mt->copy2(&ctx, mv->value);
    } else {
        newval = mt->copy(fromkey, tokey, mv->value);
    }
     
    if (!newval) {
        addReplyError(c, "module key failed to copy");
        return NULL;
    }
    return createModuleObject(mt, newval);
}

/* Register a new data type exported by the module. The parameters are the
 * following. Please for in depth documentation check the modules API
 * documentation, especially https://redis.io/topics/modules-native-types.
 *
 * * **name**: A 9 characters data type name that MUST be unique in the Redis
 *   Modules ecosystem. Be creative... and there will be no collisions. Use
 *   the charset A-Z a-z 9-0, plus the two "-_" characters. A good
 *   idea is to use, for example `<typename>-<vendor>`. For example
 *   "tree-AntZ" may mean "Tree data structure by @antirez". To use both
 *   lower case and upper case letters helps in order to prevent collisions.
 * * **encver**: Encoding version, which is, the version of the serialization
 *   that a module used in order to persist data. As long as the "name"
 *   matches, the RDB loading will be dispatched to the type callbacks
 *   whatever 'encver' is used, however the module can understand if
 *   the encoding it must load are of an older version of the module.
 *   For example the module "tree-AntZ" initially used encver=0. Later
 *   after an upgrade, it started to serialize data in a different format
 *   and to register the type with encver=1. However this module may
 *   still load old data produced by an older version if the rdb_load
 *   callback is able to check the encver value and act accordingly.
 *   The encver must be a positive value between 0 and 1023.
 *
 * * **typemethods_ptr** is a pointer to a RedisModuleTypeMethods structure
 *   that should be populated with the methods callbacks and structure
 *   version, like in the following example:
 *
 *         RedisModuleTypeMethods tm = {
 *             .version = REDISMODULE_TYPE_METHOD_VERSION,
 *             .rdb_load = myType_RDBLoadCallBack,
 *             .rdb_save = myType_RDBSaveCallBack,
 *             .aof_rewrite = myType_AOFRewriteCallBack,
 *             .free = myType_FreeCallBack,
 *
 *             // Optional fields
 *             .digest = myType_DigestCallBack,
 *             .mem_usage = myType_MemUsageCallBack,
 *             .aux_load = myType_AuxRDBLoadCallBack,
 *             .aux_save = myType_AuxRDBSaveCallBack,
 *             .free_effort = myType_FreeEffortCallBack,
 *             .unlink = myType_UnlinkCallBack,
 *             .copy = myType_CopyCallback,
 *             .defrag = myType_DefragCallback
 * 
 *             // Enhanced optional fields
 *             .mem_usage2 = myType_MemUsageCallBack2,
 *             .free_effort2 = myType_FreeEffortCallBack2,
 *             .unlink2 = myType_UnlinkCallBack2,
 *             .copy2 = myType_CopyCallback2,
 *         }
 *
 * * **rdb_load**: A callback function pointer that loads data from RDB files.
 * * **rdb_save**: A callback function pointer that saves data to RDB files.
 * * **aof_rewrite**: A callback function pointer that rewrites data as commands.
 * * **digest**: A callback function pointer that is used for `DEBUG DIGEST`.
 * * **free**: A callback function pointer that can free a type value.
 * * **aux_save**: A callback function pointer that saves out of keyspace data to RDB files.
 *   'when' argument is either REDISMODULE_AUX_BEFORE_RDB or REDISMODULE_AUX_AFTER_RDB.
 * * **aux_load**: A callback function pointer that loads out of keyspace data from RDB files.
 *   Similar to aux_save, returns REDISMODULE_OK on success, and ERR otherwise.
 * * **free_effort**: A callback function pointer that used to determine whether the module's
 *   memory needs to be lazy reclaimed. The module should return the complexity involved by
 *   freeing the value. for example: how many pointers are gonna be freed. Note that if it 
 *   returns 0, we'll always do an async free.
 * * **unlink**: A callback function pointer that used to notifies the module that the key has 
 *   been removed from the DB by redis, and may soon be freed by a background thread. Note that 
 *   it won't be called on FLUSHALL/FLUSHDB (both sync and async), and the module can use the 
 *   RedisModuleEvent_FlushDB to hook into that.
 * * **copy**: A callback function pointer that is used to make a copy of the specified key.
 *   The module is expected to perform a deep copy of the specified value and return it.
 *   In addition, hints about the names of the source and destination keys is provided.
 *   A NULL return value is considered an error and the copy operation fails.
 *   Note: if the target key exists and is being overwritten, the copy callback will be
 *   called first, followed by a free callback to the value that is being replaced.
 * 
 * * **defrag**: A callback function pointer that is used to request the module to defrag
 *   a key. The module should then iterate pointers and call the relevant RM_Defrag*()
 *   functions to defragment pointers or complex types. The module should continue
 *   iterating as long as RM_DefragShouldStop() returns a zero value, and return a
 *   zero value if finished or non-zero value if more work is left to be done. If more work
 *   needs to be done, RM_DefragCursorSet() and RM_DefragCursorGet() can be used to track
 *   this work across different calls.
 *   Normally, the defrag mechanism invokes the callback without a time limit, so
 *   RM_DefragShouldStop() always returns zero. The "late defrag" mechanism which has
 *   a time limit and provides cursor support is used only for keys that are determined
 *   to have significant internal complexity. To determine this, the defrag mechanism
 *   uses the free_effort callback and the 'active-defrag-max-scan-fields' config directive.
 *   NOTE: The value is passed as a `void**` and the function is expected to update the
 *   pointer if the top-level value pointer is defragmented and consequently changes.
 *
 * * **mem_usage2**: Similar to `mem_usage`, but provides the `RedisModuleKeyOptCtx` parameter
 *   so that meta information such as key name and db id can be obtained, and
 *   the `sample_size` for size estimation (see MEMORY USAGE command).
 * * **free_effort2**: Similar to `free_effort`, but provides the `RedisModuleKeyOptCtx` parameter
 *   so that meta information such as key name and db id can be obtained.
 * * **unlink2**: Similar to `unlink`, but provides the `RedisModuleKeyOptCtx` parameter
 *   so that meta information such as key name and db id can be obtained.
 * * **copy2**: Similar to `copy`, but provides the `RedisModuleKeyOptCtx` parameter
 *   so that meta information such as key names and db ids can be obtained.
 * * **aux_save2**: Similar to `aux_save`, but with small semantic change, if the module
 *   saves nothing on this callback then no data about this aux field will be written to the
 *   RDB and it will be possible to load the RDB even if the module is not loaded.
 * 
 * Note: the module name "AAAAAAAAA" is reserved and produces an error, it
 * happens to be pretty lame as well.
 *
 * If RedisModule_CreateDataType() is called outside of RedisModule_OnLoad() function,
 * there is already a module registering a type with the same name,
 * or if the module name or encver is invalid, NULL is returned.
 * Otherwise the new type is registered into Redis, and a reference of
 * type RedisModuleType is returned: the caller of the function should store
 * this reference into a global variable to make future use of it in the
 * modules type API, since a single module may register multiple types.
 * Example code fragment:
 *
 *      static RedisModuleType *BalancedTreeType;
 *
 *      int RedisModule_OnLoad(RedisModuleCtx *ctx) {
 *          // some code here ...
 *          BalancedTreeType = RM_CreateDataType(...);
 *      }
 */
moduleType *RM_CreateDataType(RedisModuleCtx *ctx, const char *name, int encver, void *typemethods_ptr) {
    if (!ctx->module->onload)
        return NULL;
    uint64_t id = moduleTypeEncodeId(name,encver);
    if (id == 0) return NULL;
    if (moduleTypeLookupModuleByName(name) != NULL) return NULL;

    long typemethods_version = ((long*)typemethods_ptr)[0];
    if (typemethods_version == 0) return NULL;

    struct typemethods {
        uint64_t version;
        moduleTypeLoadFunc rdb_load;
        moduleTypeSaveFunc rdb_save;
        moduleTypeRewriteFunc aof_rewrite;
        moduleTypeMemUsageFunc mem_usage;
        moduleTypeDigestFunc digest;
        moduleTypeFreeFunc free;
        struct {
            moduleTypeAuxLoadFunc aux_load;
            moduleTypeAuxSaveFunc aux_save;
            int aux_save_triggers;
        } v2;
        struct {
            moduleTypeFreeEffortFunc free_effort;
            moduleTypeUnlinkFunc unlink;
            moduleTypeCopyFunc copy;
            moduleTypeDefragFunc defrag;
        } v3;
        struct {
            moduleTypeMemUsageFunc2 mem_usage2;
            moduleTypeFreeEffortFunc2 free_effort2;
            moduleTypeUnlinkFunc2 unlink2;
            moduleTypeCopyFunc2 copy2;
        } v4;
        struct {
            moduleTypeAuxSaveFunc aux_save2;
        } v5;
    } *tms = (struct typemethods*) typemethods_ptr;

    moduleType *mt = zcalloc(sizeof(*mt));
    mt->id = id;
    mt->module = ctx->module;
    mt->rdb_load = tms->rdb_load;
    mt->rdb_save = tms->rdb_save;
    mt->aof_rewrite = tms->aof_rewrite;
    mt->mem_usage = tms->mem_usage;
    mt->digest = tms->digest;
    mt->free = tms->free;
    if (tms->version >= 2) {
        mt->aux_load = tms->v2.aux_load;
        mt->aux_save = tms->v2.aux_save;
        mt->aux_save_triggers = tms->v2.aux_save_triggers;
    }
    if (tms->version >= 3) {
        mt->free_effort = tms->v3.free_effort;
        mt->unlink = tms->v3.unlink;
        mt->copy = tms->v3.copy;
        mt->defrag = tms->v3.defrag;
    }
    if (tms->version >= 4) {
        mt->mem_usage2 = tms->v4.mem_usage2;
        mt->unlink2 = tms->v4.unlink2;
        mt->free_effort2 = tms->v4.free_effort2;
        mt->copy2 = tms->v4.copy2;
    }
    if (tms->version >= 5) {
        mt->aux_save2 = tms->v5.aux_save2;
    }
    memcpy(mt->name,name,sizeof(mt->name));
    listAddNodeTail(ctx->module->types,mt);
    return mt;
}

/* If the key is open for writing, set the specified module type object
 * as the value of the key, deleting the old value if any.
 * On success REDISMODULE_OK is returned. If the key is not open for
 * writing or there is an active iterator, REDISMODULE_ERR is returned. */
int RM_ModuleTypeSetValue(RedisModuleKey *key, moduleType *mt, void *value) {
    if (!(key->mode & REDISMODULE_WRITE) || key->iter) return REDISMODULE_ERR;
    RM_DeleteKey(key);
    robj *o = createModuleObject(mt,value);
    setKey(key->ctx->client,key->db,key->key,o,SETKEY_NO_SIGNAL);
    decrRefCount(o);
    key->value = o;
    return REDISMODULE_OK;
}

/* Assuming RedisModule_KeyType() returned REDISMODULE_KEYTYPE_MODULE on
 * the key, returns the module type pointer of the value stored at key.
 *
 * If the key is NULL, is not associated with a module type, or is empty,
 * then NULL is returned instead. */
moduleType *RM_ModuleTypeGetType(RedisModuleKey *key) {
    if (key == NULL ||
        key->value == NULL ||
        RM_KeyType(key) != REDISMODULE_KEYTYPE_MODULE) return NULL;
    moduleValue *mv = key->value->ptr;
    return mv->type;
}

/* Assuming RedisModule_KeyType() returned REDISMODULE_KEYTYPE_MODULE on
 * the key, returns the module type low-level value stored at key, as
 * it was set by the user via RedisModule_ModuleTypeSetValue().
 *
 * If the key is NULL, is not associated with a module type, or is empty,
 * then NULL is returned instead. */
void *RM_ModuleTypeGetValue(RedisModuleKey *key) {
    if (key == NULL ||
        key->value == NULL ||
        RM_KeyType(key) != REDISMODULE_KEYTYPE_MODULE) return NULL;
    moduleValue *mv = key->value->ptr;
    return mv->value;
}

/* --------------------------------------------------------------------------
 * ## RDB loading and saving functions
 * -------------------------------------------------------------------------- */

/* Called when there is a load error in the context of a module. On some
 * modules this cannot be recovered, but if the module declared capability
 * to handle errors, we'll raise a flag rather than exiting. */
void moduleRDBLoadError(RedisModuleIO *io) {
    if (io->type->module->options & REDISMODULE_OPTIONS_HANDLE_IO_ERRORS) {
        io->error = 1;
        return;
    }
    serverPanic(
        "Error loading data from RDB (short read or EOF). "
        "Read performed by module '%s' about type '%s' "
        "after reading '%llu' bytes of a value "
        "for key named: '%s'.",
        io->type->module->name,
        io->type->name,
        (unsigned long long)io->bytes,
        io->key? (char*)io->key->ptr: "(null)");
}

/* Returns 0 if there's at least one registered data type that did not declare
 * REDISMODULE_OPTIONS_HANDLE_IO_ERRORS, in which case diskless loading should
 * be avoided since it could cause data loss. */
int moduleAllDatatypesHandleErrors() {
    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;

    while ((de = dictNext(di)) != NULL) {
        struct RedisModule *module = dictGetVal(de);
        if (listLength(module->types) &&
            !(module->options & REDISMODULE_OPTIONS_HANDLE_IO_ERRORS))
        {
            dictReleaseIterator(di);
            return 0;
        }
    }
    dictReleaseIterator(di);
    return 1;
}

/* Returns 0 if module did not declare REDISMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD, in which case
 * diskless async loading should be avoided because module doesn't know there can be traffic during
 * database full resynchronization. */
int moduleAllModulesHandleReplAsyncLoad() {
    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;

    while ((de = dictNext(di)) != NULL) {
        struct RedisModule *module = dictGetVal(de);
        if (!(module->options & REDISMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD)) {
            dictReleaseIterator(di);
            return 0;
        }
    }
    dictReleaseIterator(di);
    return 1;
}

/* Returns true if any previous IO API failed.
 * for `Load*` APIs the REDISMODULE_OPTIONS_HANDLE_IO_ERRORS flag must be set with
 * RedisModule_SetModuleOptions first. */
int RM_IsIOError(RedisModuleIO *io) {
    return io->error;
}

static int flushRedisModuleIOBuffer(RedisModuleIO *io) {
    if (!io->pre_flush_buffer) return 0;

    /* We have data that must be flushed before saving the current data.
     * Lets flush it. */
    sds pre_flush_buffer = io->pre_flush_buffer;
    io->pre_flush_buffer = NULL;
    ssize_t retval = rdbWriteRaw(io->rio, pre_flush_buffer, sdslen(pre_flush_buffer));
    sdsfree(pre_flush_buffer);
    if (retval >= 0) io->bytes += retval;
    return retval;
}

/* Save an unsigned 64 bit value into the RDB file. This function should only
 * be called in the context of the rdb_save method of modules implementing new
 * data types. */
void RM_SaveUnsigned(RedisModuleIO *io, uint64_t value) {
    if (io->error) return;
    if (flushRedisModuleIOBuffer(io) == -1) goto saveerr;
    /* Save opcode. */
    int retval = rdbSaveLen(io->rio, RDB_MODULE_OPCODE_UINT);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    /* Save value. */
    retval = rdbSaveLen(io->rio, value);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    return;

saveerr:
    io->error = 1;
}

/* Load an unsigned 64 bit value from the RDB file. This function should only
 * be called in the context of the `rdb_load` method of modules implementing
 * new data types. */
uint64_t RM_LoadUnsigned(RedisModuleIO *io) {
    if (io->error) return 0;
    uint64_t opcode = rdbLoadLen(io->rio,NULL);
    if (opcode != RDB_MODULE_OPCODE_UINT) goto loaderr;
    uint64_t value;
    int retval = rdbLoadLenByRef(io->rio, NULL, &value);
    if (retval == -1) goto loaderr;
    return value;

loaderr:
    moduleRDBLoadError(io);
    return 0;
}

/* Like RedisModule_SaveUnsigned() but for signed 64 bit values. */
void RM_SaveSigned(RedisModuleIO *io, int64_t value) {
    union {uint64_t u; int64_t i;} conv;
    conv.i = value;
    RM_SaveUnsigned(io,conv.u);
}

/* Like RedisModule_LoadUnsigned() but for signed 64 bit values. */
int64_t RM_LoadSigned(RedisModuleIO *io) {
    union {uint64_t u; int64_t i;} conv;
    conv.u = RM_LoadUnsigned(io);
    return conv.i;
}

/* In the context of the rdb_save method of a module type, saves a
 * string into the RDB file taking as input a RedisModuleString.
 *
 * The string can be later loaded with RedisModule_LoadString() or
 * other Load family functions expecting a serialized string inside
 * the RDB file. */
void RM_SaveString(RedisModuleIO *io, RedisModuleString *s) {
    if (io->error) return;
    if (flushRedisModuleIOBuffer(io) == -1) goto saveerr;
    /* Save opcode. */
    ssize_t retval = rdbSaveLen(io->rio, RDB_MODULE_OPCODE_STRING);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    /* Save value. */
    retval = rdbSaveStringObject(io->rio, s);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    return;

saveerr:
    io->error = 1;
}

/* Like RedisModule_SaveString() but takes a raw C pointer and length
 * as input. */
void RM_SaveStringBuffer(RedisModuleIO *io, const char *str, size_t len) {
    if (io->error) return;
    if (flushRedisModuleIOBuffer(io) == -1) goto saveerr;
    /* Save opcode. */
    ssize_t retval = rdbSaveLen(io->rio, RDB_MODULE_OPCODE_STRING);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    /* Save value. */
    retval = rdbSaveRawString(io->rio, (unsigned char*)str,len);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    return;

saveerr:
    io->error = 1;
}

/* Implements RM_LoadString() and RM_LoadStringBuffer() */
void *moduleLoadString(RedisModuleIO *io, int plain, size_t *lenptr) {
    if (io->error) return NULL;
    uint64_t opcode = rdbLoadLen(io->rio,NULL);
    if (opcode != RDB_MODULE_OPCODE_STRING) goto loaderr;
    void *s = rdbGenericLoadStringObject(io->rio,
              plain ? RDB_LOAD_PLAIN : RDB_LOAD_NONE, lenptr);
    if (s == NULL) goto loaderr;
    return s;

loaderr:
    moduleRDBLoadError(io);
    return NULL;
}

/* In the context of the rdb_load method of a module data type, loads a string
 * from the RDB file, that was previously saved with RedisModule_SaveString()
 * functions family.
 *
 * The returned string is a newly allocated RedisModuleString object, and
 * the user should at some point free it with a call to RedisModule_FreeString().
 *
 * If the data structure does not store strings as RedisModuleString objects,
 * the similar function RedisModule_LoadStringBuffer() could be used instead. */
RedisModuleString *RM_LoadString(RedisModuleIO *io) {
    return moduleLoadString(io,0,NULL);
}

/* Like RedisModule_LoadString() but returns a heap allocated string that
 * was allocated with RedisModule_Alloc(), and can be resized or freed with
 * RedisModule_Realloc() or RedisModule_Free().
 *
 * The size of the string is stored at '*lenptr' if not NULL.
 * The returned string is not automatically NULL terminated, it is loaded
 * exactly as it was stored inside the RDB file. */
char *RM_LoadStringBuffer(RedisModuleIO *io, size_t *lenptr) {
    return moduleLoadString(io,1,lenptr);
}

/* In the context of the rdb_save method of a module data type, saves a double
 * value to the RDB file. The double can be a valid number, a NaN or infinity.
 * It is possible to load back the value with RedisModule_LoadDouble(). */
void RM_SaveDouble(RedisModuleIO *io, double value) {
    if (io->error) return;
    if (flushRedisModuleIOBuffer(io) == -1) goto saveerr;
    /* Save opcode. */
    int retval = rdbSaveLen(io->rio, RDB_MODULE_OPCODE_DOUBLE);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    /* Save value. */
    retval = rdbSaveBinaryDoubleValue(io->rio, value);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    return;

saveerr:
    io->error = 1;
}

/* In the context of the rdb_save method of a module data type, loads back the
 * double value saved by RedisModule_SaveDouble(). */
double RM_LoadDouble(RedisModuleIO *io) {
    if (io->error) return 0;
    uint64_t opcode = rdbLoadLen(io->rio,NULL);
    if (opcode != RDB_MODULE_OPCODE_DOUBLE) goto loaderr;
    double value;
    int retval = rdbLoadBinaryDoubleValue(io->rio, &value);
    if (retval == -1) goto loaderr;
    return value;

loaderr:
    moduleRDBLoadError(io);
    return 0;
}

/* In the context of the rdb_save method of a module data type, saves a float
 * value to the RDB file. The float can be a valid number, a NaN or infinity.
 * It is possible to load back the value with RedisModule_LoadFloat(). */
void RM_SaveFloat(RedisModuleIO *io, float value) {
    if (io->error) return;
    if (flushRedisModuleIOBuffer(io) == -1) goto saveerr;
    /* Save opcode. */
    int retval = rdbSaveLen(io->rio, RDB_MODULE_OPCODE_FLOAT);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    /* Save value. */
    retval = rdbSaveBinaryFloatValue(io->rio, value);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    return;

saveerr:
    io->error = 1;
}

/* In the context of the rdb_save method of a module data type, loads back the
 * float value saved by RedisModule_SaveFloat(). */
float RM_LoadFloat(RedisModuleIO *io) {
    if (io->error) return 0;
    uint64_t opcode = rdbLoadLen(io->rio,NULL);
    if (opcode != RDB_MODULE_OPCODE_FLOAT) goto loaderr;
    float value;
    int retval = rdbLoadBinaryFloatValue(io->rio, &value);
    if (retval == -1) goto loaderr;
    return value;

loaderr:
    moduleRDBLoadError(io);
    return 0;
}

/* In the context of the rdb_save method of a module data type, saves a long double
 * value to the RDB file. The double can be a valid number, a NaN or infinity.
 * It is possible to load back the value with RedisModule_LoadLongDouble(). */
void RM_SaveLongDouble(RedisModuleIO *io, long double value) {
    if (io->error) return;
    char buf[MAX_LONG_DOUBLE_CHARS];
    /* Long double has different number of bits in different platforms, so we
     * save it as a string type. */
    size_t len = ld2string(buf,sizeof(buf),value,LD_STR_HEX);
    RM_SaveStringBuffer(io,buf,len);
}

/* In the context of the rdb_save method of a module data type, loads back the
 * long double value saved by RedisModule_SaveLongDouble(). */
long double RM_LoadLongDouble(RedisModuleIO *io) {
    if (io->error) return 0;
    long double value;
    size_t len;
    char* str = RM_LoadStringBuffer(io,&len);
    if (!str) return 0;
    string2ld(str,len,&value);
    RM_Free(str);
    return value;
}

/* Iterate over modules, and trigger rdb aux saving for the ones modules types
 * who asked for it. */
ssize_t rdbSaveModulesAux(rio *rdb, int when) {
    size_t total_written = 0;
    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;

    while ((de = dictNext(di)) != NULL) {
        struct RedisModule *module = dictGetVal(de);
        listIter li;
        listNode *ln;

        listRewind(module->types,&li);
        while((ln = listNext(&li))) {
            moduleType *mt = ln->value;
            if ((!mt->aux_save && !mt->aux_save2) || !(mt->aux_save_triggers & when))
                continue;
            ssize_t ret = rdbSaveSingleModuleAux(rdb, when, mt);
            if (ret==-1) {
                dictReleaseIterator(di);
                return -1;
            }
            total_written += ret;
        }
    }

    dictReleaseIterator(di);
    return total_written;
}

/* --------------------------------------------------------------------------
 * ## Key digest API (DEBUG DIGEST interface for modules types)
 * -------------------------------------------------------------------------- */

/* Add a new element to the digest. This function can be called multiple times
 * one element after the other, for all the elements that constitute a given
 * data structure. The function call must be followed by the call to
 * `RedisModule_DigestEndSequence` eventually, when all the elements that are
 * always in a given order are added. See the Redis Modules data types
 * documentation for more info. However this is a quick example that uses Redis
 * data types as an example.
 *
 * To add a sequence of unordered elements (for example in the case of a Redis
 * Set), the pattern to use is:
 *
 *     foreach element {
 *         AddElement(element);
 *         EndSequence();
 *     }
 *
 * Because Sets are not ordered, so every element added has a position that
 * does not depend from the other. However if instead our elements are
 * ordered in pairs, like field-value pairs of a Hash, then one should
 * use:
 *
 *     foreach key,value {
 *         AddElement(key);
 *         AddElement(value);
 *         EndSequence();
 *     }
 *
 * Because the key and value will be always in the above order, while instead
 * the single key-value pairs, can appear in any position into a Redis hash.
 *
 * A list of ordered elements would be implemented with:
 *
 *     foreach element {
 *         AddElement(element);
 *     }
 *     EndSequence();
 *
 */
void RM_DigestAddStringBuffer(RedisModuleDigest *md, const char *ele, size_t len) {
    mixDigest(md->o,ele,len);
}

/* Like `RedisModule_DigestAddStringBuffer()` but takes a `long long` as input
 * that gets converted into a string before adding it to the digest. */
void RM_DigestAddLongLong(RedisModuleDigest *md, long long ll) {
    char buf[LONG_STR_SIZE];
    size_t len = ll2string(buf,sizeof(buf),ll);
    mixDigest(md->o,buf,len);
}

/* See the documentation for `RedisModule_DigestAddElement()`. */
void RM_DigestEndSequence(RedisModuleDigest *md) {
    xorDigest(md->x,md->o,sizeof(md->o));
    memset(md->o,0,sizeof(md->o));
}

/* Decode a serialized representation of a module data type 'mt', in a specific encoding version 'encver'
 * from string 'str' and return a newly allocated value, or NULL if decoding failed.
 *
 * This call basically reuses the 'rdb_load' callback which module data types
 * implement in order to allow a module to arbitrarily serialize/de-serialize
 * keys, similar to how the Redis 'DUMP' and 'RESTORE' commands are implemented.
 *
 * Modules should generally use the REDISMODULE_OPTIONS_HANDLE_IO_ERRORS flag and
 * make sure the de-serialization code properly checks and handles IO errors
 * (freeing allocated buffers and returning a NULL).
 *
 * If this is NOT done, Redis will handle corrupted (or just truncated) serialized
 * data by producing an error message and terminating the process.
 */
void *RM_LoadDataTypeFromStringEncver(const RedisModuleString *str, const moduleType *mt, int encver) {
    rio payload;
    RedisModuleIO io;
    void *ret;

    rioInitWithBuffer(&payload, str->ptr);
    moduleInitIOContext(io,(moduleType *)mt,&payload,NULL,-1);

    /* All RM_Save*() calls always write a version 2 compatible format, so we
     * need to make sure we read the same.
     */
    ret = mt->rdb_load(&io,encver);
    if (io.ctx) {
        moduleFreeContext(io.ctx);
        zfree(io.ctx);
    }
    return ret;
}

/* Similar to RM_LoadDataTypeFromStringEncver, original version of the API, kept
 * for backward compatibility. 
 */
void *RM_LoadDataTypeFromString(const RedisModuleString *str, const moduleType *mt) {
    return RM_LoadDataTypeFromStringEncver(str, mt, 0);
}

/* Encode a module data type 'mt' value 'data' into serialized form, and return it
 * as a newly allocated RedisModuleString.
 *
 * This call basically reuses the 'rdb_save' callback which module data types
 * implement in order to allow a module to arbitrarily serialize/de-serialize
 * keys, similar to how the Redis 'DUMP' and 'RESTORE' commands are implemented.
 */
RedisModuleString *RM_SaveDataTypeToString(RedisModuleCtx *ctx, void *data, const moduleType *mt) {
    rio payload;
    RedisModuleIO io;

    rioInitWithBuffer(&payload,sdsempty());
    moduleInitIOContext(io,(moduleType *)mt,&payload,NULL,-1);
    mt->rdb_save(&io,data);
    if (io.ctx) {
        moduleFreeContext(io.ctx);
        zfree(io.ctx);
    }
    if (io.error) {
        return NULL;
    } else {
        robj *str = createObject(OBJ_STRING,payload.io.buffer.ptr);
        if (ctx != NULL) autoMemoryAdd(ctx,REDISMODULE_AM_STRING,str);
        return str;
    }
}

/* Returns the name of the key currently being processed. */
const RedisModuleString *RM_GetKeyNameFromDigest(RedisModuleDigest *dig) {
    return dig->key;
}

/* Returns the database id of the key currently being processed. */
int RM_GetDbIdFromDigest(RedisModuleDigest *dig) {
    return dig->dbid;
}
/* --------------------------------------------------------------------------
 * ## AOF API for modules data types
 * -------------------------------------------------------------------------- */

/* Emits a command into the AOF during the AOF rewriting process. This function
 * is only called in the context of the aof_rewrite method of data types exported
 * by a module. The command works exactly like RedisModule_Call() in the way
 * the parameters are passed, but it does not return anything as the error
 * handling is performed by Redis itself. */
void RM_EmitAOF(RedisModuleIO *io, const char *cmdname, const char *fmt, ...) {
    if (io->error) return;
    struct redisCommand *cmd;
    robj **argv = NULL;
    int argc = 0, flags = 0, j;
    va_list ap;

    cmd = lookupCommandByCString((char*)cmdname);
    if (!cmd) {
        serverLog(LL_WARNING,
            "Fatal: AOF method for module data type '%s' tried to "
            "emit unknown command '%s'",
            io->type->name, cmdname);
        io->error = 1;
        errno = EINVAL;
        return;
    }

    /* Emit the arguments into the AOF in Redis protocol format. */
    va_start(ap, fmt);
    argv = moduleCreateArgvFromUserFormat(cmdname,fmt,&argc,&flags,ap);
    va_end(ap);
    if (argv == NULL) {
        serverLog(LL_WARNING,
            "Fatal: AOF method for module data type '%s' tried to "
            "call RedisModule_EmitAOF() with wrong format specifiers '%s'",
            io->type->name, fmt);
        io->error = 1;
        errno = EINVAL;
        return;
    }

    /* Bulk count. */
    if (!io->error && rioWriteBulkCount(io->rio,'*',argc) == 0)
        io->error = 1;

    /* Arguments. */
    for (j = 0; j < argc; j++) {
        if (!io->error && rioWriteBulkObject(io->rio,argv[j]) == 0)
            io->error = 1;
        decrRefCount(argv[j]);
    }
    zfree(argv);
    return;
}

/* --------------------------------------------------------------------------
 * ## IO context handling
 * -------------------------------------------------------------------------- */

RedisModuleCtx *RM_GetContextFromIO(RedisModuleIO *io) {
    if (io->ctx) return io->ctx; /* Can't have more than one... */
    io->ctx = zmalloc(sizeof(RedisModuleCtx));
    moduleCreateContext(io->ctx, io->type->module, REDISMODULE_CTX_NONE);
    return io->ctx;
}

/* Returns the name of the key currently being processed.
 * There is no guarantee that the key name is always available, so this may return NULL.
 */
const RedisModuleString *RM_GetKeyNameFromIO(RedisModuleIO *io) {
    return io->key;
}

/* Returns a RedisModuleString with the name of the key from RedisModuleKey. */
const RedisModuleString *RM_GetKeyNameFromModuleKey(RedisModuleKey *key) {
    return key ? key->key : NULL;
}

/* Returns a database id of the key from RedisModuleKey. */
int RM_GetDbIdFromModuleKey(RedisModuleKey *key) {
    return key ? key->db->id : -1;
}

/* Returns the database id of the key currently being processed.
 * There is no guarantee that this info is always available, so this may return -1.
 */
int RM_GetDbIdFromIO(RedisModuleIO *io) {
    return io->dbid;
}

/* --------------------------------------------------------------------------
 * ## Logging
 * -------------------------------------------------------------------------- */

/* This is the low level function implementing both:
 *
 *      RM_Log()
 *      RM_LogIOError()
 *
 */
void moduleLogRaw(RedisModule *module, const char *levelstr, const char *fmt, va_list ap) {
    char msg[LOG_MAX_LEN];
    size_t name_len;
    int level;

    if (!strcasecmp(levelstr,"debug")) level = LL_DEBUG;
    else if (!strcasecmp(levelstr,"verbose")) level = LL_VERBOSE;
    else if (!strcasecmp(levelstr,"notice")) level = LL_NOTICE;
    else if (!strcasecmp(levelstr,"warning")) level = LL_WARNING;
    else level = LL_VERBOSE; /* Default. */

    if (level < server.verbosity) return;

    name_len = snprintf(msg, sizeof(msg),"<%s> ", module? module->name: "module");
    vsnprintf(msg + name_len, sizeof(msg) - name_len, fmt, ap);
    serverLogRaw(level,msg);
}

/* Produces a log message to the standard Redis log, the format accepts
 * printf-alike specifiers, while level is a string describing the log
 * level to use when emitting the log, and must be one of the following:
 *
 * * "debug" (`REDISMODULE_LOGLEVEL_DEBUG`)
 * * "verbose" (`REDISMODULE_LOGLEVEL_VERBOSE`)
 * * "notice" (`REDISMODULE_LOGLEVEL_NOTICE`)
 * * "warning" (`REDISMODULE_LOGLEVEL_WARNING`)
 *
 * If the specified log level is invalid, verbose is used by default.
 * There is a fixed limit to the length of the log line this function is able
 * to emit, this limit is not specified but is guaranteed to be more than
 * a few lines of text.
 *
 * The ctx argument may be NULL if cannot be provided in the context of the
 * caller for instance threads or callbacks, in which case a generic "module"
 * will be used instead of the module name.
 */
void RM_Log(RedisModuleCtx *ctx, const char *levelstr, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    moduleLogRaw(ctx? ctx->module: NULL,levelstr,fmt,ap);
    va_end(ap);
}

/* Log errors from RDB / AOF serialization callbacks.
 *
 * This function should be used when a callback is returning a critical
 * error to the caller since cannot load or save the data for some
 * critical reason. */
void RM_LogIOError(RedisModuleIO *io, const char *levelstr, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    moduleLogRaw(io->type->module,levelstr,fmt,ap);
    va_end(ap);
}

/* Redis-like assert function.
 *
 * The macro `RedisModule_Assert(expression)` is recommended, rather than
 * calling this function directly.
 *
 * A failed assertion will shut down the server and produce logging information
 * that looks identical to information generated by Redis itself.
 */
void RM__Assert(const char *estr, const char *file, int line) {
    _serverAssert(estr, file, line);
}

/* Allows adding event to the latency monitor to be observed by the LATENCY
 * command. The call is skipped if the latency is smaller than the configured
 * latency-monitor-threshold. */
void RM_LatencyAddSample(const char *event, mstime_t latency) {
    if (latency >= server.latency_monitor_threshold)
        latencyAddSample(event, latency);
}

/* --------------------------------------------------------------------------
 * ## Blocking clients from modules
 *
 * For a guide about blocking commands in modules, see
 * https://redis.io/topics/modules-blocking-ops.
 * -------------------------------------------------------------------------- */

/* This is called from blocked.c in order to unblock a client: may be called
 * for multiple reasons while the client is in the middle of being blocked
 * because the client is terminated, but is also called for cleanup when a
 * client is unblocked in a clean way after replaying.
 *
 * What we do here is just to set the client to NULL in the redis module
 * blocked client handle. This way if the client is terminated while there
 * is a pending threaded operation involving the blocked client, we'll know
 * that the client no longer exists and no reply callback should be called.
 *
 * The structure RedisModuleBlockedClient will be always deallocated when
 * running the list of clients blocked by a module that need to be unblocked. */
void unblockClientFromModule(client *c) {
    RedisModuleBlockedClient *bc = c->bstate.module_blocked_handle;

    /* Call the disconnection callback if any. Note that
     * bc->disconnect_callback is set to NULL if the client gets disconnected
     * by the module itself or because of a timeout, so the callback will NOT
     * get called if this is not an actual disconnection event. */
    if (bc->disconnect_callback) {
        RedisModuleCtx ctx;
        moduleCreateContext(&ctx, bc->module, REDISMODULE_CTX_NONE);
        ctx.blocked_privdata = bc->privdata;
        ctx.client = bc->client;
        bc->disconnect_callback(&ctx,bc);
        moduleFreeContext(&ctx);
    }

    /* If we made it here and client is still blocked it means that the command
     * timed-out, client was killed or disconnected and disconnect_callback was
     * not implemented (or it was, but RM_UnblockClient was not called from
     * within it, as it should).
     * We must call moduleUnblockClient in order to free privdata and
     * RedisModuleBlockedClient.
     *
     * Note that we only do that for clients that are blocked on keys, for which
     * the contract is that the module should not call RM_UnblockClient under
     * normal circumstances.
     * Clients implementing threads and working with private data should be
     * aware that calling RM_UnblockClient for every blocked client is their
     * responsibility, and if they fail to do so memory may leak. Ideally they
     * should implement the disconnect and timeout callbacks and call
     * RM_UnblockClient, but any other way is also acceptable. */
    if (bc->blocked_on_keys && !bc->unblocked)
        moduleUnblockClient(c);

    bc->client = NULL;
}

/* Block a client in the context of a module: this function implements both
 * RM_BlockClient() and RM_BlockClientOnKeys() depending on the fact the
 * keys are passed or not.
 *
 * When not blocking for keys, the keys, numkeys, and privdata parameters are
 * not needed. The privdata in that case must be NULL, since later is
 * RM_UnblockClient() that will provide some private data that the reply
 * callback will receive.
 *
 * Instead when blocking for keys, normally RM_UnblockClient() will not be
 * called (because the client will unblock when the key is modified), so
 * 'privdata' should be provided in that case, so that once the client is
 * unlocked and the reply callback is called, it will receive its associated
 * private data.
 *
 * Even when blocking on keys, RM_UnblockClient() can be called however, but
 * in that case the privdata argument is disregarded, because we pass the
 * reply callback the privdata that is set here while blocking.
 *
 */
RedisModuleBlockedClient *moduleBlockClient(RedisModuleCtx *ctx, RedisModuleCmdFunc reply_callback,
                                            RedisModuleAuthCallback auth_reply_callback,
                                            RedisModuleCmdFunc timeout_callback, void (*free_privdata)(RedisModuleCtx*,void*),
                                            long long timeout_ms, RedisModuleString **keys, int numkeys, void *privdata,
                                            int flags) {
    client *c = ctx->client;
    int islua = scriptIsRunning();
    int ismulti = server.in_exec;

    c->bstate.module_blocked_handle = zmalloc(sizeof(RedisModuleBlockedClient));
    RedisModuleBlockedClient *bc = c->bstate.module_blocked_handle;
    ctx->module->blocked_clients++;

    /* We need to handle the invalid operation of calling modules blocking
     * commands from Lua or MULTI. We actually create an already aborted
     * (client set to NULL) blocked client handle, and actually reply with
     * an error. */
    mstime_t timeout = timeout_ms ? (mstime()+timeout_ms) : 0;
    bc->client = (islua || ismulti) ? NULL : c;
    bc->module = ctx->module;
    bc->reply_callback = reply_callback;
    bc->auth_reply_cb = auth_reply_callback;
    bc->timeout_callback = timeout_callback;
    bc->disconnect_callback = NULL; /* Set by RM_SetDisconnectCallback() */
    bc->free_privdata = free_privdata;
    bc->privdata = privdata;
    bc->reply_client = moduleAllocTempClient(NULL);
    bc->thread_safe_ctx_client = moduleAllocTempClient(NULL);
    if (bc->client)
        bc->reply_client->resp = bc->client->resp;
    bc->dbid = c->db->id;
    bc->blocked_on_keys = keys != NULL;
    bc->unblocked = 0;
    bc->background_timer = 0;
    bc->background_duration = 0;
    c->bstate.timeout = timeout;

    if (islua || ismulti) {
        c->bstate.module_blocked_handle = NULL;
        addReplyError(c, islua ?
            "Blocking module command called from Lua script" :
            "Blocking module command called from transaction");
    } else if (ctx->flags & REDISMODULE_CTX_BLOCKED_REPLY) {
        c->bstate.module_blocked_handle = NULL;
        addReplyError(c, "Blocking module command called from a Reply callback context");
    }
    else if (!auth_reply_callback && clientHasModuleAuthInProgress(c)) {
        c->bstate.module_blocked_handle = NULL;
        addReplyError(c, "Clients undergoing module based authentication can only be blocked on auth");
    } else {
        if (keys) {
            blockForKeys(c,BLOCKED_MODULE,keys,numkeys,timeout,flags&REDISMODULE_BLOCK_UNBLOCK_DELETED);
        } else {
            blockClient(c,BLOCKED_MODULE);
        }
    }
    return bc;
}

/* This API registers a callback to execute in addition to normal password based authentication.
 * Multiple callbacks can be registered across different modules. When a Module is unloaded, all the
 * auth callbacks registered by it are unregistered.
 * The callbacks are attempted (in the order of most recently registered first) when the AUTH/HELLO
 * (with AUTH field provided) commands are called.
 * The callbacks will be called with a module context along with a username and a password, and are
 * expected to take one of the following actions:
 * (1) Authenticate - Use the RM_AuthenticateClient* API and return REDISMODULE_AUTH_HANDLED.
 * This will immediately end the auth chain as successful and add the OK reply.
 * (2) Deny Authentication - Return REDISMODULE_AUTH_HANDLED without authenticating or blocking the
 * client. Optionally, `err` can be set to a custom error message and `err` will be automatically
 * freed by the server.
 * This will immediately end the auth chain as unsuccessful and add the ERR reply.
 * (3) Block a client on authentication - Use the RM_BlockClientOnAuth API and return
 * REDISMODULE_AUTH_HANDLED. Here, the client will be blocked until the RM_UnblockClient API is used
 * which will trigger the auth reply callback (provided through the RM_BlockClientOnAuth).
 * In this reply callback, the Module should authenticate, deny or skip handling authentication.
 * (4) Skip handling Authentication - Return REDISMODULE_AUTH_NOT_HANDLED without blocking the
 * client. This will allow the engine to attempt the next module auth callback.
 * If none of the callbacks authenticate or deny auth, then password based auth is attempted and
 * will authenticate or add failure logs and reply to the clients accordingly.
 *
 * Note: If a client is disconnected while it was in the middle of blocking module auth, that
 * occurrence of the AUTH or HELLO command will not be tracked in the INFO command stats.
 *
 * The following is an example of how non-blocking module based authentication can be used:
 *
 *      int auth_cb(RedisModuleCtx *ctx, RedisModuleString *username, RedisModuleString *password, RedisModuleString **err) {
 *          const char *user = RedisModule_StringPtrLen(username, NULL);
 *          const char *pwd = RedisModule_StringPtrLen(password, NULL);
 *          if (!strcmp(user,"foo") && !strcmp(pwd,"valid_password")) {
 *              RedisModule_AuthenticateClientWithACLUser(ctx, "foo", 3, NULL, NULL, NULL);
 *              return REDISMODULE_AUTH_HANDLED;
 *          }
 *
 *          else if (!strcmp(user,"foo") && !strcmp(pwd,"wrong_password")) {
 *              RedisModuleString *log = RedisModule_CreateString(ctx, "Module Auth", 11);
 *              RedisModule_ACLAddLogEntryByUserName(ctx, username, log, REDISMODULE_ACL_LOG_AUTH);
 *              RedisModule_FreeString(ctx, log);
 *              const char *err_msg = "Auth denied by Misc Module.";
 *              *err = RedisModule_CreateString(ctx, err_msg, strlen(err_msg));
 *              return REDISMODULE_AUTH_HANDLED;
 *          }
 *          return REDISMODULE_AUTH_NOT_HANDLED;
 *       }
 *
 *      int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
 *          if (RedisModule_Init(ctx,"authmodule",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
 *              return REDISMODULE_ERR;
 *          RedisModule_RegisterAuthCallback(ctx, auth_cb);
 *          return REDISMODULE_OK;
 *      }
 */
void RM_RegisterAuthCallback(RedisModuleCtx *ctx, RedisModuleAuthCallback cb) {
    RedisModuleAuthCtx *auth_ctx = zmalloc(sizeof(RedisModuleAuthCtx));
    auth_ctx->module = ctx->module;
    auth_ctx->auth_cb = cb;
    listAddNodeHead(moduleAuthCallbacks, auth_ctx);
}

/* Helper function to invoke the free private data callback of a Module blocked client. */
void moduleInvokeFreePrivDataCallback(client *c, RedisModuleBlockedClient *bc) {
    if (bc->privdata && bc->free_privdata) {
        RedisModuleCtx ctx;
        int ctx_flags = c == NULL ? REDISMODULE_CTX_BLOCKED_DISCONNECTED : REDISMODULE_CTX_NONE;
        moduleCreateContext(&ctx, bc->module, ctx_flags);
        ctx.blocked_privdata = bc->privdata;
        ctx.client = bc->client;
        bc->free_privdata(&ctx,bc->privdata);
        moduleFreeContext(&ctx);
    }
}

/* Unregisters all the module auth callbacks that have been registered by this Module. */
void moduleUnregisterAuthCBs(RedisModule *module) {
    listIter li;
    listNode *ln;
    listRewind(moduleAuthCallbacks, &li);
    while ((ln = listNext(&li))) {
        RedisModuleAuthCtx *ctx = listNodeValue(ln);
        if (ctx->module == module) {
            listDelNode(moduleAuthCallbacks, ln);
            zfree(ctx);
        }
    }
}

/* Search for & attempt next module auth callback after skipping the ones already attempted.
 * Returns the result of the module auth callback. */
int attemptNextAuthCb(client *c, robj *username, robj *password, robj **err) {
    int handle_next_callback = c->module_auth_ctx == NULL;
    RedisModuleAuthCtx *cur_auth_ctx = NULL;
    listNode *ln;
    listIter li;
    listRewind(moduleAuthCallbacks, &li);
    int result = REDISMODULE_AUTH_NOT_HANDLED;
    while((ln = listNext(&li))) {
        cur_auth_ctx = listNodeValue(ln);
        /* Skip over the previously attempted auth contexts. */
        if (!handle_next_callback) {
            handle_next_callback = cur_auth_ctx == c->module_auth_ctx;
            continue;
        }
        /* Remove the module auth complete flag before we attempt the next cb. */
        c->flags &= ~CLIENT_MODULE_AUTH_HAS_RESULT;
        RedisModuleCtx ctx;
        moduleCreateContext(&ctx, cur_auth_ctx->module, REDISMODULE_CTX_NONE);
        ctx.client = c;
        *err = NULL;
        c->module_auth_ctx = cur_auth_ctx;
        result = cur_auth_ctx->auth_cb(&ctx, username, password, err);
        moduleFreeContext(&ctx);
        if (result == REDISMODULE_AUTH_HANDLED) break;
        /* If Auth was not handled (allowed/denied/blocked) by the Module, try the next auth cb. */
    }
    return result;
}

/* Helper function to handle a reprocessed unblocked auth client.
 * Returns REDISMODULE_AUTH_NOT_HANDLED if the client was not reprocessed after a blocking module
 * auth operation.
 * Otherwise, we attempt the auth reply callback & the free priv data callback, update fields and
 * return the result of the reply callback. */
int attemptBlockedAuthReplyCallback(client *c, robj *username, robj *password, robj **err) {
    int result = REDISMODULE_AUTH_NOT_HANDLED;
    if (!c->module_blocked_client) return result;
    RedisModuleBlockedClient *bc = (RedisModuleBlockedClient *) c->module_blocked_client;
    bc->client = c;
    if (bc->auth_reply_cb) {
        RedisModuleCtx ctx;
        moduleCreateContext(&ctx, bc->module, REDISMODULE_CTX_BLOCKED_REPLY);
        ctx.blocked_privdata = bc->privdata;
        ctx.blocked_ready_key = NULL;
        ctx.client = bc->client;
        ctx.blocked_client = bc;
        result = bc->auth_reply_cb(&ctx, username, password, err);
        moduleFreeContext(&ctx);
    }
    moduleInvokeFreePrivDataCallback(c, bc);
    c->module_blocked_client = NULL;
    c->lastcmd->microseconds += bc->background_duration;
    bc->module->blocked_clients--;
    zfree(bc);
    return result;
}

/* Helper function to attempt Module based authentication through module auth callbacks.
 * Here, the Module is expected to authenticate the client using the RedisModule APIs and to add ACL
 * logs in case of errors.
 * Returns one of the following codes:
 * AUTH_OK - Indicates that a module handled and authenticated the client.
 * AUTH_ERR - Indicates that a module handled and denied authentication for this client.
 * AUTH_NOT_HANDLED - Indicates that authentication was not handled by any Module and that
 * normal password based authentication can be attempted next.
 * AUTH_BLOCKED - Indicates module authentication is in progress through a blocking implementation.
 * In this case, authentication is handled here again after the client is unblocked / reprocessed. */
int checkModuleAuthentication(client *c, robj *username, robj *password, robj **err) {
    if (!listLength(moduleAuthCallbacks)) return AUTH_NOT_HANDLED;
    int result = attemptBlockedAuthReplyCallback(c, username, password, err);
    if (result == REDISMODULE_AUTH_NOT_HANDLED) {
        result = attemptNextAuthCb(c, username, password, err);
    }
    if (c->flags & CLIENT_BLOCKED) {
        /* Modules are expected to return REDISMODULE_AUTH_HANDLED when blocking clients. */
        serverAssert(result == REDISMODULE_AUTH_HANDLED);
        return AUTH_BLOCKED;
    }
    c->module_auth_ctx = NULL;
    if (result == REDISMODULE_AUTH_NOT_HANDLED) {
        c->flags &= ~CLIENT_MODULE_AUTH_HAS_RESULT;
        return AUTH_NOT_HANDLED;
    }
    if (c->flags & CLIENT_MODULE_AUTH_HAS_RESULT) {
        c->flags &= ~CLIENT_MODULE_AUTH_HAS_RESULT;
        if (c->authenticated) return AUTH_OK;
    }
    return AUTH_ERR;
}

/* This function is called from module.c in order to check if a module
 * blocked for BLOCKED_MODULE and subtype 'on keys' (bc->blocked_on_keys true)
 * can really be unblocked, since the module was able to serve the client.
 * If the callback returns REDISMODULE_OK, then the client can be unblocked,
 * otherwise the client remains blocked and we'll retry again when one of
 * the keys it blocked for becomes "ready" again.
 * This function returns 1 if client was served (and should be unblocked) */
int moduleTryServeClientBlockedOnKey(client *c, robj *key) {
    int served = 0;
    RedisModuleBlockedClient *bc = c->bstate.module_blocked_handle;

    /* Protect against re-processing: don't serve clients that are already
     * in the unblocking list for any reason (including RM_UnblockClient()
     * explicit call). See #6798. */
    if (bc->unblocked) return 0;

    RedisModuleCtx ctx;
    moduleCreateContext(&ctx, bc->module, REDISMODULE_CTX_BLOCKED_REPLY);
    ctx.blocked_ready_key = key;
    ctx.blocked_privdata = bc->privdata;
    ctx.client = bc->client;
    ctx.blocked_client = bc;
    if (bc->reply_callback(&ctx,(void**)c->argv,c->argc) == REDISMODULE_OK)
        served = 1;
    moduleFreeContext(&ctx);
    return served;
}

/* Block a client in the context of a blocking command, returning a handle
 * which will be used, later, in order to unblock the client with a call to
 * RedisModule_UnblockClient(). The arguments specify callback functions
 * and a timeout after which the client is unblocked.
 *
 * The callbacks are called in the following contexts:
 *
 *     reply_callback:   called after a successful RedisModule_UnblockClient()
 *                       call in order to reply to the client and unblock it.
 *
 *     timeout_callback: called when the timeout is reached or if `CLIENT UNBLOCK`
 *                       is invoked, in order to send an error to the client.
 *
 *     free_privdata:    called in order to free the private data that is passed
 *                       by RedisModule_UnblockClient() call.
 *
 * Note: RedisModule_UnblockClient should be called for every blocked client,
 *       even if client was killed, timed-out or disconnected. Failing to do so
 *       will result in memory leaks.
 *
 * There are some cases where RedisModule_BlockClient() cannot be used:
 *
 * 1. If the client is a Lua script.
 * 2. If the client is executing a MULTI block.
 *
 * In these cases, a call to RedisModule_BlockClient() will **not** block the
 * client, but instead produce a specific error reply.
 *
 * A module that registers a timeout_callback function can also be unblocked
 * using the `CLIENT UNBLOCK` command, which will trigger the timeout callback.
 * If a callback function is not registered, then the blocked client will be
 * treated as if it is not in a blocked state and `CLIENT UNBLOCK` will return
 * a zero value.
 *
 * Measuring background time: By default the time spent in the blocked command
 * is not account for the total command duration. To include such time you should
 * use RM_BlockedClientMeasureTimeStart() and RM_BlockedClientMeasureTimeEnd() one,
 * or multiple times within the blocking command background work.
 */
RedisModuleBlockedClient *RM_BlockClient(RedisModuleCtx *ctx, RedisModuleCmdFunc reply_callback,
                                         RedisModuleCmdFunc timeout_callback, void (*free_privdata)(RedisModuleCtx*,void*),
                                         long long timeout_ms) {
    return moduleBlockClient(ctx,reply_callback,NULL,timeout_callback,free_privdata,timeout_ms, NULL,0,NULL,0);
}

/* Block the current client for module authentication in the background. If module auth is not in
 * progress on the client, the API returns NULL. Otherwise, the client is blocked and the RM_BlockedClient
 * is returned similar to the RM_BlockClient API.
 * Note: Only use this API from the context of a module auth callback. */
RedisModuleBlockedClient *RM_BlockClientOnAuth(RedisModuleCtx *ctx, RedisModuleAuthCallback reply_callback,
                                               void (*free_privdata)(RedisModuleCtx*,void*)) {
    if (!clientHasModuleAuthInProgress(ctx->client)) {
        addReplyError(ctx->client, "Module blocking client on auth when not currently undergoing module authentication");
        return NULL;
    }
    RedisModuleBlockedClient *bc = moduleBlockClient(ctx,NULL,reply_callback,NULL,free_privdata,0, NULL,0,NULL,0);
    if (ctx->client->flags & CLIENT_BLOCKED) {
        ctx->client->flags |= CLIENT_PENDING_COMMAND;
    }
    return bc;
}

/* Get the private data that was previusely set on a blocked client */
void *RM_BlockClientGetPrivateData(RedisModuleBlockedClient *blocked_client) {
    return blocked_client->privdata;
}

/* Set private data on a blocked client */
void RM_BlockClientSetPrivateData(RedisModuleBlockedClient *blocked_client, void *private_data) {
    blocked_client->privdata = private_data;
}

/* This call is similar to RedisModule_BlockClient(), however in this case we
 * don't just block the client, but also ask Redis to unblock it automatically
 * once certain keys become "ready", that is, contain more data.
 *
 * Basically this is similar to what a typical Redis command usually does,
 * like BLPOP or BZPOPMAX: the client blocks if it cannot be served ASAP,
 * and later when the key receives new data (a list push for instance), the
 * client is unblocked and served.
 *
 * However in the case of this module API, when the client is unblocked?
 *
 * 1. If you block on a key of a type that has blocking operations associated,
 *    like a list, a sorted set, a stream, and so forth, the client may be
 *    unblocked once the relevant key is targeted by an operation that normally
 *    unblocks the native blocking operations for that type. So if we block
 *    on a list key, an RPUSH command may unblock our client and so forth.
 * 2. If you are implementing your native data type, or if you want to add new
 *    unblocking conditions in addition to "1", you can call the modules API
 *    RedisModule_SignalKeyAsReady().
 *
 * Anyway we can't be sure if the client should be unblocked just because the
 * key is signaled as ready: for instance a successive operation may change the
 * key, or a client in queue before this one can be served, modifying the key
 * as well and making it empty again. So when a client is blocked with
 * RedisModule_BlockClientOnKeys() the reply callback is not called after
 * RM_UnblockClient() is called, but every time a key is signaled as ready:
 * if the reply callback can serve the client, it returns REDISMODULE_OK
 * and the client is unblocked, otherwise it will return REDISMODULE_ERR
 * and we'll try again later.
 *
 * The reply callback can access the key that was signaled as ready by
 * calling the API RedisModule_GetBlockedClientReadyKey(), that returns
 * just the string name of the key as a RedisModuleString object.
 *
 * Thanks to this system we can setup complex blocking scenarios, like
 * unblocking a client only if a list contains at least 5 items or other
 * more fancy logics.
 *
 * Note that another difference with RedisModule_BlockClient(), is that here
 * we pass the private data directly when blocking the client: it will
 * be accessible later in the reply callback. Normally when blocking with
 * RedisModule_BlockClient() the private data to reply to the client is
 * passed when calling RedisModule_UnblockClient() but here the unblocking
 * is performed by Redis itself, so we need to have some private data before
 * hand. The private data is used to store any information about the specific
 * unblocking operation that you are implementing. Such information will be
 * freed using the free_privdata callback provided by the user.
 *
 * However the reply callback will be able to access the argument vector of
 * the command, so the private data is often not needed.
 *
 * Note: Under normal circumstances RedisModule_UnblockClient should not be
 *       called for clients that are blocked on keys (Either the key will
 *       become ready or a timeout will occur). If for some reason you do want
 *       to call RedisModule_UnblockClient it is possible: Client will be
 *       handled as if it were timed-out (You must implement the timeout
 *       callback in that case).
 */
RedisModuleBlockedClient *RM_BlockClientOnKeys(RedisModuleCtx *ctx, RedisModuleCmdFunc reply_callback,
                                               RedisModuleCmdFunc timeout_callback, void (*free_privdata)(RedisModuleCtx*,void*),
                                               long long timeout_ms, RedisModuleString **keys, int numkeys, void *privdata) {
    return moduleBlockClient(ctx,reply_callback,NULL,timeout_callback,free_privdata,timeout_ms, keys,numkeys,privdata,0);
}

/* Same as RedisModule_BlockClientOnKeys, but can take REDISMODULE_BLOCK_* flags
 * Can be either REDISMODULE_BLOCK_UNBLOCK_DEFAULT, which means default behavior (same
 * as calling RedisModule_BlockClientOnKeys)
 *
 * The flags is a bit mask of these:
 *
 * - `REDISMODULE_BLOCK_UNBLOCK_DELETED`: The clients should to be awakened in case any of `keys` are deleted.
 *                                        Mostly useful for commands that require the key to exist (like XREADGROUP)
 */
RedisModuleBlockedClient *RM_BlockClientOnKeysWithFlags(RedisModuleCtx *ctx, RedisModuleCmdFunc reply_callback,
                                                        RedisModuleCmdFunc timeout_callback, void (*free_privdata)(RedisModuleCtx*,void*),
                                                        long long timeout_ms, RedisModuleString **keys, int numkeys, void *privdata,
                                                        int flags) {
    return moduleBlockClient(ctx,reply_callback,NULL,timeout_callback,free_privdata,timeout_ms, keys,numkeys,privdata,flags);
}

/* This function is used in order to potentially unblock a client blocked
 * on keys with RedisModule_BlockClientOnKeys(). When this function is called,
 * all the clients blocked for this key will get their reply_callback called. */
void RM_SignalKeyAsReady(RedisModuleCtx *ctx, RedisModuleString *key) {
    signalKeyAsReady(ctx->client->db, key, OBJ_MODULE);
}

/* Implements RM_UnblockClient() and moduleUnblockClient(). */
int moduleUnblockClientByHandle(RedisModuleBlockedClient *bc, void *privdata) {
    pthread_mutex_lock(&moduleUnblockedClientsMutex);
    if (!bc->blocked_on_keys) bc->privdata = privdata;
    bc->unblocked = 1;
    if (listLength(moduleUnblockedClients) == 0) {
        if (write(server.module_pipe[1],"A",1) != 1) {
            /* Ignore the error, this is best-effort. */
        }
    }
    listAddNodeTail(moduleUnblockedClients,bc);
    pthread_mutex_unlock(&moduleUnblockedClientsMutex);
    return REDISMODULE_OK;
}

/* This API is used by the Redis core to unblock a client that was blocked
 * by a module. */
void moduleUnblockClient(client *c) {
    RedisModuleBlockedClient *bc = c->bstate.module_blocked_handle;
    moduleUnblockClientByHandle(bc,NULL);
}

/* Return true if the client 'c' was blocked by a module using
 * RM_BlockClientOnKeys(). */
int moduleClientIsBlockedOnKeys(client *c) {
    RedisModuleBlockedClient *bc = c->bstate.module_blocked_handle;
    return bc->blocked_on_keys;
}

/* Unblock a client blocked by `RedisModule_BlockedClient`. This will trigger
 * the reply callbacks to be called in order to reply to the client.
 * The 'privdata' argument will be accessible by the reply callback, so
 * the caller of this function can pass any value that is needed in order to
 * actually reply to the client.
 *
 * A common usage for 'privdata' is a thread that computes something that
 * needs to be passed to the client, included but not limited some slow
 * to compute reply or some reply obtained via networking.
 *
 * Note 1: this function can be called from threads spawned by the module.
 *
 * Note 2: when we unblock a client that is blocked for keys using the API
 * RedisModule_BlockClientOnKeys(), the privdata argument here is not used.
 * Unblocking a client that was blocked for keys using this API will still
 * require the client to get some reply, so the function will use the
 * "timeout" handler in order to do so (The privdata provided in
 * RedisModule_BlockClientOnKeys() is accessible from the timeout
 * callback via RM_GetBlockedClientPrivateData). */
int RM_UnblockClient(RedisModuleBlockedClient *bc, void *privdata) {
    if (bc->blocked_on_keys) {
        /* In theory the user should always pass the timeout handler as an
         * argument, but better to be safe than sorry. */
        if (bc->timeout_callback == NULL) return REDISMODULE_ERR;
        if (bc->unblocked) return REDISMODULE_OK;
        if (bc->client) moduleBlockedClientTimedOut(bc->client);
    }
    moduleUnblockClientByHandle(bc,privdata);
    return REDISMODULE_OK;
}

/* Abort a blocked client blocking operation: the client will be unblocked
 * without firing any callback. */
int RM_AbortBlock(RedisModuleBlockedClient *bc) {
    bc->reply_callback = NULL;
    bc->disconnect_callback = NULL;
    bc->auth_reply_cb = NULL;
    return RM_UnblockClient(bc,NULL);
}

/* Set a callback that will be called if a blocked client disconnects
 * before the module has a chance to call RedisModule_UnblockClient()
 *
 * Usually what you want to do there, is to cleanup your module state
 * so that you can call RedisModule_UnblockClient() safely, otherwise
 * the client will remain blocked forever if the timeout is large.
 *
 * Notes:
 *
 * 1. It is not safe to call Reply* family functions here, it is also
 *    useless since the client is gone.
 *
 * 2. This callback is not called if the client disconnects because of
 *    a timeout. In such a case, the client is unblocked automatically
 *    and the timeout callback is called.
 */
void RM_SetDisconnectCallback(RedisModuleBlockedClient *bc, RedisModuleDisconnectFunc callback) {
    bc->disconnect_callback = callback;
}

/* This function will check the moduleUnblockedClients queue in order to
 * call the reply callback and really unblock the client.
 *
 * Clients end into this list because of calls to RM_UnblockClient(),
 * however it is possible that while the module was doing work for the
 * blocked client, it was terminated by Redis (for timeout or other reasons).
 * When this happens the RedisModuleBlockedClient structure in the queue
 * will have the 'client' field set to NULL. */
void moduleHandleBlockedClients(void) {
    listNode *ln;
    RedisModuleBlockedClient *bc;

    pthread_mutex_lock(&moduleUnblockedClientsMutex);
    while (listLength(moduleUnblockedClients)) {
        ln = listFirst(moduleUnblockedClients);
        bc = ln->value;
        client *c = bc->client;
        listDelNode(moduleUnblockedClients,ln);
        pthread_mutex_unlock(&moduleUnblockedClientsMutex);

        /* Release the lock during the loop, as long as we don't
         * touch the shared list. */

        /* Call the reply callback if the client is valid and we have
         * any callback. However the callback is not called if the client
         * was blocked on keys (RM_BlockClientOnKeys()), because we already
         * called such callback in moduleTryServeClientBlockedOnKey() when
         * the key was signaled as ready. */
        long long prev_error_replies = server.stat_total_error_replies;
        uint64_t reply_us = 0;
        if (c && !bc->blocked_on_keys && bc->reply_callback) {
            RedisModuleCtx ctx;
            moduleCreateContext(&ctx, bc->module, REDISMODULE_CTX_BLOCKED_REPLY);
            ctx.blocked_privdata = bc->privdata;
            ctx.blocked_ready_key = NULL;
            ctx.client = bc->client;
            ctx.blocked_client = bc;
            monotime replyTimer;
            elapsedStart(&replyTimer);
            bc->reply_callback(&ctx,(void**)c->argv,c->argc);
            reply_us = elapsedUs(replyTimer);
            moduleFreeContext(&ctx);
        }
        /* Hold onto the blocked client if module auth is in progress. The reply callback is invoked
         * when the client is reprocessed. */
        if (c && clientHasModuleAuthInProgress(c)) {
            c->module_blocked_client = bc;
        } else {
            /* Free privdata if any. */
            moduleInvokeFreePrivDataCallback(c, bc);
        }

        /* It is possible that this blocked client object accumulated
         * replies to send to the client in a thread safe context.
         * We need to glue such replies to the client output buffer and
         * free the temporary client we just used for the replies. */
        if (c) AddReplyFromClient(c, bc->reply_client);
        moduleReleaseTempClient(bc->reply_client);
        moduleReleaseTempClient(bc->thread_safe_ctx_client);

        /* Update stats now that we've finished the blocking operation.
         * This needs to be out of the reply callback above given that a
         * module might not define any callback and still do blocking ops.
         */
        if (c && !clientHasModuleAuthInProgress(c) && !bc->blocked_on_keys) {
            updateStatsOnUnblock(c, bc->background_duration, reply_us, server.stat_total_error_replies != prev_error_replies);
        }

        if (c != NULL) {
            /* Before unblocking the client, set the disconnect callback
             * to NULL, because if we reached this point, the client was
             * properly unblocked by the module. */
            bc->disconnect_callback = NULL;
            unblockClient(c, 1);
            /* Put the client in the list of clients that need to write
             * if there are pending replies here. This is needed since
             * during a non blocking command the client may receive output. */
            if (!clientHasModuleAuthInProgress(c) && clientHasPendingReplies(c) &&
                !(c->flags & CLIENT_PENDING_WRITE))
            {
                c->flags |= CLIENT_PENDING_WRITE;
                listLinkNodeHead(server.clients_pending_write, &c->clients_pending_write_node);
            }
        }

        /* Free 'bc' only after unblocking the client, since it is
         * referenced in the client blocking context, and must be valid
         * when calling unblockClient(). */
        if (!(c && clientHasModuleAuthInProgress(c))) {
            bc->module->blocked_clients--;
            zfree(bc);
        }

        /* Lock again before to iterate the loop. */
        pthread_mutex_lock(&moduleUnblockedClientsMutex);
    }
    pthread_mutex_unlock(&moduleUnblockedClientsMutex);
}

/* Check if the specified client can be safely timed out using
 * moduleBlockedClientTimedOut().
 */
int moduleBlockedClientMayTimeout(client *c) {
    if (c->bstate.btype != BLOCKED_MODULE)
        return 1;

    RedisModuleBlockedClient *bc = c->bstate.module_blocked_handle;
    return (bc && bc->timeout_callback != NULL);
}

/* Called when our client timed out. After this function unblockClient()
 * is called, and it will invalidate the blocked client. So this function
 * does not need to do any cleanup. Eventually the module will call the
 * API to unblock the client and the memory will be released. */
void moduleBlockedClientTimedOut(client *c) {
    RedisModuleBlockedClient *bc = c->bstate.module_blocked_handle;

    /* Protect against re-processing: don't serve clients that are already
     * in the unblocking list for any reason (including RM_UnblockClient()
     * explicit call). See #6798. */
    if (bc->unblocked) return;

    RedisModuleCtx ctx;
    moduleCreateContext(&ctx, bc->module, REDISMODULE_CTX_BLOCKED_TIMEOUT);
    ctx.client = bc->client;
    ctx.blocked_client = bc;
    ctx.blocked_privdata = bc->privdata;
    long long prev_error_replies = server.stat_total_error_replies;
    bc->timeout_callback(&ctx,(void**)c->argv,c->argc);
    moduleFreeContext(&ctx);
    updateStatsOnUnblock(c, bc->background_duration, 0, server.stat_total_error_replies != prev_error_replies);

    /* For timeout events, we do not want to call the disconnect callback,
     * because the blocked client will be automatically disconnected in
     * this case, and the user can still hook using the timeout callback. */
    bc->disconnect_callback = NULL;
}

/* Return non-zero if a module command was called in order to fill the
 * reply for a blocked client. */
int RM_IsBlockedReplyRequest(RedisModuleCtx *ctx) {
    return (ctx->flags & REDISMODULE_CTX_BLOCKED_REPLY) != 0;
}

/* Return non-zero if a module command was called in order to fill the
 * reply for a blocked client that timed out. */
int RM_IsBlockedTimeoutRequest(RedisModuleCtx *ctx) {
    return (ctx->flags & REDISMODULE_CTX_BLOCKED_TIMEOUT) != 0;
}

/* Get the private data set by RedisModule_UnblockClient() */
void *RM_GetBlockedClientPrivateData(RedisModuleCtx *ctx) {
    return ctx->blocked_privdata;
}

/* Get the key that is ready when the reply callback is called in the context
 * of a client blocked by RedisModule_BlockClientOnKeys(). */
RedisModuleString *RM_GetBlockedClientReadyKey(RedisModuleCtx *ctx) {
    return ctx->blocked_ready_key;
}

/* Get the blocked client associated with a given context.
 * This is useful in the reply and timeout callbacks of blocked clients,
 * before sometimes the module has the blocked client handle references
 * around, and wants to cleanup it. */
RedisModuleBlockedClient *RM_GetBlockedClientHandle(RedisModuleCtx *ctx) {
    return ctx->blocked_client;
}

/* Return true if when the free callback of a blocked client is called,
 * the reason for the client to be unblocked is that it disconnected
 * while it was blocked. */
int RM_BlockedClientDisconnected(RedisModuleCtx *ctx) {
    return (ctx->flags & REDISMODULE_CTX_BLOCKED_DISCONNECTED) != 0;
}

/* --------------------------------------------------------------------------
 * ## Thread Safe Contexts
 * -------------------------------------------------------------------------- */

/* Return a context which can be used inside threads to make Redis context
 * calls with certain modules APIs. If 'bc' is not NULL then the module will
 * be bound to a blocked client, and it will be possible to use the
 * `RedisModule_Reply*` family of functions to accumulate a reply for when the
 * client will be unblocked. Otherwise the thread safe context will be
 * detached by a specific client.
 *
 * To call non-reply APIs, the thread safe context must be prepared with:
 *
 *     RedisModule_ThreadSafeContextLock(ctx);
 *     ... make your call here ...
 *     RedisModule_ThreadSafeContextUnlock(ctx);
 *
 * This is not needed when using `RedisModule_Reply*` functions, assuming
 * that a blocked client was used when the context was created, otherwise
 * no RedisModule_Reply* call should be made at all.
 *
 * NOTE: If you're creating a detached thread safe context (bc is NULL),
 * consider using `RM_GetDetachedThreadSafeContext` which will also retain
 * the module ID and thus be more useful for logging. */
RedisModuleCtx *RM_GetThreadSafeContext(RedisModuleBlockedClient *bc) {
    RedisModuleCtx *ctx = zmalloc(sizeof(*ctx));
    RedisModule *module = bc ? bc->module : NULL;
    int flags = REDISMODULE_CTX_THREAD_SAFE;

    /* Creating a new client object is costly. To avoid that, we have an
     * internal pool of client objects. In blockClient(), a client object is
     * assigned to bc->thread_safe_ctx_client to be used for the thread safe
     * context.
     * For detached thread safe contexts, we create a new client object.
     * Otherwise, as this function can be called from different threads, we
     * would need to synchronize access to internal pool of client objects.
     * Assuming creating detached context is rare and not that performance
     * critical, we avoid synchronizing access to the client pool by creating
     * a new client */
    if (!bc) flags |= REDISMODULE_CTX_NEW_CLIENT;
    moduleCreateContext(ctx, module, flags);
    /* Even when the context is associated with a blocked client, we can't
     * access it safely from another thread, so we use a fake client here
     * in order to keep things like the currently selected database and similar
     * things. */
    if (bc) {
        ctx->blocked_client = bc;
        ctx->client = bc->thread_safe_ctx_client;
        selectDb(ctx->client,bc->dbid);
        if (bc->client) {
            ctx->client->id = bc->client->id;
            ctx->client->resp = bc->client->resp;
        }
    }
    return ctx;
}

/* Return a detached thread safe context that is not associated with any
 * specific blocked client, but is associated with the module's context.
 *
 * This is useful for modules that wish to hold a global context over
 * a long term, for purposes such as logging. */
RedisModuleCtx *RM_GetDetachedThreadSafeContext(RedisModuleCtx *ctx) {
    RedisModuleCtx *new_ctx = zmalloc(sizeof(*new_ctx));
    /* We create a new client object for the detached context.
     * See RM_GetThreadSafeContext() for more information */
    moduleCreateContext(new_ctx, ctx->module,
                        REDISMODULE_CTX_THREAD_SAFE|REDISMODULE_CTX_NEW_CLIENT);
    return new_ctx;
}

/* Release a thread safe context. */
void RM_FreeThreadSafeContext(RedisModuleCtx *ctx) {
    moduleFreeContext(ctx);
    zfree(ctx);
}

void moduleGILAfterLock() {
    /* We should never get here if we already inside a module
     * code block which already opened a context. */
    serverAssert(server.execution_nesting == 0);
    /* Bump up the nesting level to prevent immediate propagation
     * of possible RM_Call from th thread */
    enterExecutionUnit(1, 0);
}

/* Acquire the server lock before executing a thread safe API call.
 * This is not needed for `RedisModule_Reply*` calls when there is
 * a blocked client connected to the thread safe context. */
void RM_ThreadSafeContextLock(RedisModuleCtx *ctx) {
    UNUSED(ctx);
    moduleAcquireGIL();
    moduleGILAfterLock();
}

/* Similar to RM_ThreadSafeContextLock but this function
 * would not block if the server lock is already acquired.
 *
 * If successful (lock acquired) REDISMODULE_OK is returned,
 * otherwise REDISMODULE_ERR is returned and errno is set
 * accordingly. */
int RM_ThreadSafeContextTryLock(RedisModuleCtx *ctx) {
    UNUSED(ctx);

    int res = moduleTryAcquireGIL();
    if(res != 0) {
        errno = res;
        return REDISMODULE_ERR;
    }
    moduleGILAfterLock();
    return REDISMODULE_OK;
}

void moduleGILBeforeUnlock() {
    /* We should never get here if we already inside a module
     * code block which already opened a context, except
     * the bump-up from moduleGILAcquired. */
    serverAssert(server.execution_nesting == 1);
    /* Restore nesting level and propagate pending commands
     * (because it's unclear when thread safe contexts are
     * released we have to propagate here). */
    exitExecutionUnit();
    postExecutionUnitOperations();
}

/* Release the server lock after a thread safe API call was executed. */
void RM_ThreadSafeContextUnlock(RedisModuleCtx *ctx) {
    UNUSED(ctx);
    moduleGILBeforeUnlock();
    moduleReleaseGIL();
}

void moduleAcquireGIL(void) {
    pthread_mutex_lock(&moduleGIL);
}

int moduleTryAcquireGIL(void) {
    return pthread_mutex_trylock(&moduleGIL);
}

void moduleReleaseGIL(void) {
    pthread_mutex_unlock(&moduleGIL);
}


/* --------------------------------------------------------------------------
 * ## Module Keyspace Notifications API
 * -------------------------------------------------------------------------- */

/* Subscribe to keyspace notifications. This is a low-level version of the
 * keyspace-notifications API. A module can register callbacks to be notified
 * when keyspace events occur.
 *
 * Notification events are filtered by their type (string events, set events,
 * etc), and the subscriber callback receives only events that match a specific
 * mask of event types.
 *
 * When subscribing to notifications with RedisModule_SubscribeToKeyspaceEvents
 * the module must provide an event type-mask, denoting the events the subscriber
 * is interested in. This can be an ORed mask of any of the following flags:
 *
 *  - REDISMODULE_NOTIFY_GENERIC: Generic commands like DEL, EXPIRE, RENAME
 *  - REDISMODULE_NOTIFY_STRING: String events
 *  - REDISMODULE_NOTIFY_LIST: List events
 *  - REDISMODULE_NOTIFY_SET: Set events
 *  - REDISMODULE_NOTIFY_HASH: Hash events
 *  - REDISMODULE_NOTIFY_ZSET: Sorted Set events
 *  - REDISMODULE_NOTIFY_EXPIRED: Expiration events
 *  - REDISMODULE_NOTIFY_EVICTED: Eviction events
 *  - REDISMODULE_NOTIFY_STREAM: Stream events
 *  - REDISMODULE_NOTIFY_MODULE: Module types events
 *  - REDISMODULE_NOTIFY_KEYMISS: Key-miss events
 *                                Notice, key-miss event is the only type
 *                                of event that is fired from within a read command.
 *                                Performing RM_Call with a write command from within
 *                                this notification is wrong and discourage. It will
 *                                cause the read command that trigger the event to be
 *                                replicated to the AOF/Replica.
 *  - REDISMODULE_NOTIFY_ALL: All events (Excluding REDISMODULE_NOTIFY_KEYMISS)
 *  - REDISMODULE_NOTIFY_LOADED: A special notification available only for modules,
 *                               indicates that the key was loaded from persistence.
 *                               Notice, when this event fires, the given key
 *                               can not be retained, use RM_CreateStringFromString
 *                               instead.
 *
 * We do not distinguish between key events and keyspace events, and it is up
 * to the module to filter the actions taken based on the key.
 *
 * The subscriber signature is:
 *
 *     int (*RedisModuleNotificationFunc) (RedisModuleCtx *ctx, int type,
 *                                         const char *event,
 *                                         RedisModuleString *key);
 *
 * `type` is the event type bit, that must match the mask given at registration
 * time. The event string is the actual command being executed, and key is the
 * relevant Redis key.
 *
 * Notification callback gets executed with a redis context that can not be
 * used to send anything to the client, and has the db number where the event
 * occurred as its selected db number.
 *
 * Notice that it is not necessary to enable notifications in redis.conf for
 * module notifications to work.
 *
 * Warning: the notification callbacks are performed in a synchronous manner,
 * so notification callbacks must to be fast, or they would slow Redis down.
 * If you need to take long actions, use threads to offload them.
 *
 * Moreover, the fact that the notification is executed synchronously means
 * that the notification code will be executed in the middle on Redis logic
 * (commands logic, eviction, expire). Changing the key space while the logic
 * runs is dangerous and discouraged. In order to react to key space events with
 * write actions, please refer to `RM_AddPostExecutionUnitJob`.
 *
 * See https://redis.io/topics/notifications for more information.
 */
int RM_SubscribeToKeyspaceEvents(RedisModuleCtx *ctx, int types, RedisModuleNotificationFunc callback) {
    RedisModuleKeyspaceSubscriber *sub = zmalloc(sizeof(*sub));
    sub->module = ctx->module;
    sub->event_mask = types;
    sub->notify_callback = callback;
    sub->active = 0;

    listAddNodeTail(moduleKeyspaceSubscribers, sub);
    return REDISMODULE_OK;
}

void firePostExecutionUnitJobs() {
    /* Avoid propagation of commands.
     * In that way, postExecutionUnitOperations will prevent
     * recursive calls to firePostExecutionUnitJobs.
     * This is a special case where we need to increase 'execution_nesting'
     * but we do not want to update the cached time */
    enterExecutionUnit(0, 0);
    while (listLength(modulePostExecUnitJobs) > 0) {
        listNode *ln = listFirst(modulePostExecUnitJobs);
        RedisModulePostExecUnitJob *job = listNodeValue(ln);
        listDelNode(modulePostExecUnitJobs, ln);

        RedisModuleCtx ctx;
        moduleCreateContext(&ctx, job->module, REDISMODULE_CTX_TEMP_CLIENT);
        selectDb(ctx.client, job->dbid);

        job->callback(&ctx, job->pd);
        if (job->free_pd) job->free_pd(job->pd);

        moduleFreeContext(&ctx);
        zfree(job);
    }
    exitExecutionUnit();
}

/* When running inside a key space notification callback, it is dangerous and highly discouraged to perform any write
 * operation (See `RM_SubscribeToKeyspaceEvents`). In order to still perform write actions in this scenario,
 * Redis provides `RM_AddPostNotificationJob` API. The API allows to register a job callback which Redis will call
 * when the following condition are promised to be fulfilled:
 * 1. It is safe to perform any write operation.
 * 2. The job will be called atomically along side the key space notification.
 *
 * Notice, one job might trigger key space notifications that will trigger more jobs.
 * This raises a concerns of entering an infinite loops, we consider infinite loops
 * as a logical bug that need to be fixed in the module, an attempt to protect against
 * infinite loops by halting the execution could result in violation of the feature correctness
 * and so Redis will make no attempt to protect the module from infinite loops.
 *
 * 'free_pd' can be NULL and in such case will not be used. */
int RM_AddPostNotificationJob(RedisModuleCtx *ctx, RedisModulePostNotificationJobFunc callback, void *privdata, void (*free_privdata)(void*)) {
    RedisModulePostExecUnitJob *job = zmalloc(sizeof(*job));
    job->module = ctx->module;
    job->callback = callback;
    job->pd = privdata;
    job->free_pd = free_privdata;
    job->dbid = ctx->client->db->id;

    listAddNodeTail(modulePostExecUnitJobs, job);
    return REDISMODULE_OK;
}

/* Get the configured bitmap of notify-keyspace-events (Could be used
 * for additional filtering in RedisModuleNotificationFunc) */
int RM_GetNotifyKeyspaceEvents() {
    return server.notify_keyspace_events;
}

/* Expose notifyKeyspaceEvent to modules */
int RM_NotifyKeyspaceEvent(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    if (!ctx || !ctx->client)
        return REDISMODULE_ERR;
    notifyKeyspaceEvent(type, (char *)event, key, ctx->client->db->id);
    return REDISMODULE_OK;
}

/* Dispatcher for keyspace notifications to module subscriber functions.
 * This gets called  only if at least one module requested to be notified on
 * keyspace notifications */
void moduleNotifyKeyspaceEvent(int type, const char *event, robj *key, int dbid) {
    /* Don't do anything if there aren't any subscribers */
    if (listLength(moduleKeyspaceSubscribers) == 0) return;

    /* Ugly hack to handle modules which use write commands from within
     * notify_callback, which they should NOT do!
     * Modules should use RedisModules_AddPostNotificationJob instead.
     *
     * Anyway, we want any propagated commands from within notify_callback
     * to be propagated inside a MULTI/EXEC together with the original
     * command that caused the KSN.
     * Note that it's only relevant for KSNs which are not generated from within
     * call(), for example active-expiry and eviction (because anyway
     * execution_nesting is incremented from within call())
     *
     * In order to do that we increment the execution_nesting counter, thus
     * preventing postExecutionUnitOperations (from within moduleFreeContext)
     * from propagating commands from CB.
     *
     * This is a special case where we need to increase 'execution_nesting'
     * but we do not want to update the cached time */
    enterExecutionUnit(0, 0);

    listIter li;
    listNode *ln;
    listRewind(moduleKeyspaceSubscribers,&li);

    /* Remove irrelevant flags from the type mask */
    type &= ~(NOTIFY_KEYEVENT | NOTIFY_KEYSPACE);

    while((ln = listNext(&li))) {
        RedisModuleKeyspaceSubscriber *sub = ln->value;
        /* Only notify subscribers on events matching the registration,
         * and avoid subscribers triggering themselves */
        if ((sub->event_mask & type) &&
            (sub->active == 0 || (sub->module->options & REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS))) {
            RedisModuleCtx ctx;
            moduleCreateContext(&ctx, sub->module, REDISMODULE_CTX_TEMP_CLIENT);
            selectDb(ctx.client, dbid);

            /* mark the handler as active to avoid reentrant loops.
             * If the subscriber performs an action triggering itself,
             * it will not be notified about it. */
            sub->active = 1;
            server.lazy_expire_disabled++;
            sub->notify_callback(&ctx, type, event, key);
            server.lazy_expire_disabled--;
            sub->active = 0;
            moduleFreeContext(&ctx);
        }
    }

    exitExecutionUnit();
}

/* Unsubscribe any notification subscribers this module has upon unloading */
void moduleUnsubscribeNotifications(RedisModule *module) {
    listIter li;
    listNode *ln;
    listRewind(moduleKeyspaceSubscribers,&li);
    while((ln = listNext(&li))) {
        RedisModuleKeyspaceSubscriber *sub = ln->value;
        if (sub->module == module) {
            listDelNode(moduleKeyspaceSubscribers, ln);
            zfree(sub);
        }
    }
}

/* --------------------------------------------------------------------------
 * ## Modules Cluster API
 * -------------------------------------------------------------------------- */

/* The Cluster message callback function pointer type. */
typedef void (*RedisModuleClusterMessageReceiver)(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len);

/* This structure identifies a registered caller: it must match a given module
 * ID, for a given message type. The callback function is just the function
 * that was registered as receiver. */
typedef struct moduleClusterReceiver {
    uint64_t module_id;
    RedisModuleClusterMessageReceiver callback;
    struct RedisModule *module;
    struct moduleClusterReceiver *next;
} moduleClusterReceiver;

typedef struct moduleClusterNodeInfo {
    int flags;
    char ip[NET_IP_STR_LEN];
    int port;
    char master_id[40]; /* Only if flags & REDISMODULE_NODE_MASTER is true. */
} mdouleClusterNodeInfo;

/* We have an array of message types: each bucket is a linked list of
 * configured receivers. */
static moduleClusterReceiver *clusterReceivers[UINT8_MAX];

/* Dispatch the message to the right module receiver. */
void moduleCallClusterReceivers(const char *sender_id, uint64_t module_id, uint8_t type, const unsigned char *payload, uint32_t len) {
    moduleClusterReceiver *r = clusterReceivers[type];
    while(r) {
        if (r->module_id == module_id) {
            RedisModuleCtx ctx;
            moduleCreateContext(&ctx, r->module, REDISMODULE_CTX_TEMP_CLIENT);
            r->callback(&ctx,sender_id,type,payload,len);
            moduleFreeContext(&ctx);
            return;
        }
        r = r->next;
    }
}

/* Register a callback receiver for cluster messages of type 'type'. If there
 * was already a registered callback, this will replace the callback function
 * with the one provided, otherwise if the callback is set to NULL and there
 * is already a callback for this function, the callback is unregistered
 * (so this API call is also used in order to delete the receiver). */
void RM_RegisterClusterMessageReceiver(RedisModuleCtx *ctx, uint8_t type, RedisModuleClusterMessageReceiver callback) {
    if (!server.cluster_enabled) return;

    uint64_t module_id = moduleTypeEncodeId(ctx->module->name,0);
    moduleClusterReceiver *r = clusterReceivers[type], *prev = NULL;
    while(r) {
        if (r->module_id == module_id) {
            /* Found! Set or delete. */
            if (callback) {
                r->callback = callback;
            } else {
                /* Delete the receiver entry if the user is setting
                 * it to NULL. Just unlink the receiver node from the
                 * linked list. */
                if (prev)
                    prev->next = r->next;
                else
                    clusterReceivers[type]->next = r->next;
                zfree(r);
            }
            return;
        }
        prev = r;
        r = r->next;
    }

    /* Not found, let's add it. */
    if (callback) {
        r = zmalloc(sizeof(*r));
        r->module_id = module_id;
        r->module = ctx->module;
        r->callback = callback;
        r->next = clusterReceivers[type];
        clusterReceivers[type] = r;
    }
}

/* Send a message to all the nodes in the cluster if `target` is NULL, otherwise
 * at the specified target, which is a REDISMODULE_NODE_ID_LEN bytes node ID, as
 * returned by the receiver callback or by the nodes iteration functions.
 *
 * The function returns REDISMODULE_OK if the message was successfully sent,
 * otherwise if the node is not connected or such node ID does not map to any
 * known cluster node, REDISMODULE_ERR is returned. */
int RM_SendClusterMessage(RedisModuleCtx *ctx, const char *target_id, uint8_t type, const char *msg, uint32_t len) {
    if (!server.cluster_enabled) return REDISMODULE_ERR;
    uint64_t module_id = moduleTypeEncodeId(ctx->module->name,0);
    if (clusterSendModuleMessageToTarget(target_id,module_id,type,msg,len) == C_OK)
        return REDISMODULE_OK;
    else
        return REDISMODULE_ERR;
}

/* Return an array of string pointers, each string pointer points to a cluster
 * node ID of exactly REDISMODULE_NODE_ID_LEN bytes (without any null term).
 * The number of returned node IDs is stored into `*numnodes`.
 * However if this function is called by a module not running an a Redis
 * instance with Redis Cluster enabled, NULL is returned instead.
 *
 * The IDs returned can be used with RedisModule_GetClusterNodeInfo() in order
 * to get more information about single node.
 *
 * The array returned by this function must be freed using the function
 * RedisModule_FreeClusterNodesList().
 *
 * Example:
 *
 *     size_t count, j;
 *     char **ids = RedisModule_GetClusterNodesList(ctx,&count);
 *     for (j = 0; j < count; j++) {
 *         RedisModule_Log(ctx,"notice","Node %.*s",
 *             REDISMODULE_NODE_ID_LEN,ids[j]);
 *     }
 *     RedisModule_FreeClusterNodesList(ids);
 */
char **RM_GetClusterNodesList(RedisModuleCtx *ctx, size_t *numnodes) {
    UNUSED(ctx);

    if (!server.cluster_enabled) return NULL;
    size_t count = dictSize(server.cluster->nodes);
    char **ids = zmalloc((count+1)*REDISMODULE_NODE_ID_LEN);
    dictIterator *di = dictGetIterator(server.cluster->nodes);
    dictEntry *de;
    int j = 0;
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node->flags & (CLUSTER_NODE_NOADDR|CLUSTER_NODE_HANDSHAKE)) continue;
        ids[j] = zmalloc(REDISMODULE_NODE_ID_LEN);
        memcpy(ids[j],node->name,REDISMODULE_NODE_ID_LEN);
        j++;
    }
    *numnodes = j;
    ids[j] = NULL; /* Null term so that FreeClusterNodesList does not need
                    * to also get the count argument. */
    dictReleaseIterator(di);
    return ids;
}

/* Free the node list obtained with RedisModule_GetClusterNodesList. */
void RM_FreeClusterNodesList(char **ids) {
    if (ids == NULL) return;
    for (int j = 0; ids[j]; j++) zfree(ids[j]);
    zfree(ids);
}

/* Return this node ID (REDISMODULE_CLUSTER_ID_LEN bytes) or NULL if the cluster
 * is disabled. */
const char *RM_GetMyClusterID(void) {
    if (!server.cluster_enabled) return NULL;
    return server.cluster->myself->name;
}

/* Return the number of nodes in the cluster, regardless of their state
 * (handshake, noaddress, ...) so that the number of active nodes may actually
 * be smaller, but not greater than this number. If the instance is not in
 * cluster mode, zero is returned. */
size_t RM_GetClusterSize(void) {
    if (!server.cluster_enabled) return 0;
    return dictSize(server.cluster->nodes);
}

/* Populate the specified info for the node having as ID the specified 'id',
 * then returns REDISMODULE_OK. Otherwise if the format of node ID is invalid
 * or the node ID does not exist from the POV of this local node, REDISMODULE_ERR
 * is returned.
 *
 * The arguments `ip`, `master_id`, `port` and `flags` can be NULL in case we don't
 * need to populate back certain info. If an `ip` and `master_id` (only populated
 * if the instance is a slave) are specified, they point to buffers holding
 * at least REDISMODULE_NODE_ID_LEN bytes. The strings written back as `ip`
 * and `master_id` are not null terminated.
 *
 * The list of flags reported is the following:
 *
 * * REDISMODULE_NODE_MYSELF:       This node
 * * REDISMODULE_NODE_MASTER:       The node is a master
 * * REDISMODULE_NODE_SLAVE:        The node is a replica
 * * REDISMODULE_NODE_PFAIL:        We see the node as failing
 * * REDISMODULE_NODE_FAIL:         The cluster agrees the node is failing
 * * REDISMODULE_NODE_NOFAILOVER:   The slave is configured to never failover
 */
int RM_GetClusterNodeInfo(RedisModuleCtx *ctx, const char *id, char *ip, char *master_id, int *port, int *flags) {
    UNUSED(ctx);

    clusterNode *node = clusterLookupNode(id, strlen(id));
    if (node == NULL ||
        node->flags & (CLUSTER_NODE_NOADDR|CLUSTER_NODE_HANDSHAKE))
    {
        return REDISMODULE_ERR;
    }

    if (ip) redis_strlcpy(ip,node->ip,NET_IP_STR_LEN);

    if (master_id) {
        /* If the information is not available, the function will set the
         * field to zero bytes, so that when the field can't be populated the
         * function kinda remains predictable. */
        if (node->flags & CLUSTER_NODE_SLAVE && node->slaveof)
            memcpy(master_id,node->slaveof->name,REDISMODULE_NODE_ID_LEN);
        else
            memset(master_id,0,REDISMODULE_NODE_ID_LEN);
    }
    if (port) *port = node->port;

    /* As usually we have to remap flags for modules, in order to ensure
     * we can provide binary compatibility. */
    if (flags) {
        *flags = 0;
        if (node->flags & CLUSTER_NODE_MYSELF) *flags |= REDISMODULE_NODE_MYSELF;
        if (node->flags & CLUSTER_NODE_MASTER) *flags |= REDISMODULE_NODE_MASTER;
        if (node->flags & CLUSTER_NODE_SLAVE) *flags |= REDISMODULE_NODE_SLAVE;
        if (node->flags & CLUSTER_NODE_PFAIL) *flags |= REDISMODULE_NODE_PFAIL;
        if (node->flags & CLUSTER_NODE_FAIL) *flags |= REDISMODULE_NODE_FAIL;
        if (node->flags & CLUSTER_NODE_NOFAILOVER) *flags |= REDISMODULE_NODE_NOFAILOVER;
    }
    return REDISMODULE_OK;
}

/* Set Redis Cluster flags in order to change the normal behavior of
 * Redis Cluster, especially with the goal of disabling certain functions.
 * This is useful for modules that use the Cluster API in order to create
 * a different distributed system, but still want to use the Redis Cluster
 * message bus. Flags that can be set:
 *
 * * CLUSTER_MODULE_FLAG_NO_FAILOVER
 * * CLUSTER_MODULE_FLAG_NO_REDIRECTION
 *
 * With the following effects:
 *
 * * NO_FAILOVER: prevent Redis Cluster slaves from failing over a dead master.
 *                Also disables the replica migration feature.
 *
 * * NO_REDIRECTION: Every node will accept any key, without trying to perform
 *                   partitioning according to the Redis Cluster algorithm.
 *                   Slots information will still be propagated across the
 *                   cluster, but without effect. */
void RM_SetClusterFlags(RedisModuleCtx *ctx, uint64_t flags) {
    UNUSED(ctx);
    if (flags & REDISMODULE_CLUSTER_FLAG_NO_FAILOVER)
        server.cluster_module_flags |= CLUSTER_MODULE_FLAG_NO_FAILOVER;
    if (flags & REDISMODULE_CLUSTER_FLAG_NO_REDIRECTION)
        server.cluster_module_flags |= CLUSTER_MODULE_FLAG_NO_REDIRECTION;
}

/* --------------------------------------------------------------------------
 * ## Modules Timers API
 *
 * Module timers are a high precision "green timers" abstraction where
 * every module can register even millions of timers without problems, even if
 * the actual event loop will just have a single timer that is used to awake the
 * module timers subsystem in order to process the next event.
 *
 * All the timers are stored into a radix tree, ordered by expire time, when
 * the main Redis event loop timer callback is called, we try to process all
 * the timers already expired one after the other. Then we re-enter the event
 * loop registering a timer that will expire when the next to process module
 * timer will expire.
 *
 * Every time the list of active timers drops to zero, we unregister the
 * main event loop timer, so that there is no overhead when such feature is
 * not used.
 * -------------------------------------------------------------------------- */

static rax *Timers;     /* The radix tree of all the timers sorted by expire. */
long long aeTimer = -1; /* Main event loop (ae.c) timer identifier. */

typedef void (*RedisModuleTimerProc)(RedisModuleCtx *ctx, void *data);

/* The timer descriptor, stored as value in the radix tree. */
typedef struct RedisModuleTimer {
    RedisModule *module;                /* Module reference. */
    RedisModuleTimerProc callback;      /* The callback to invoke on expire. */
    void *data;                         /* Private data for the callback. */
    int dbid;                           /* Database number selected by the original client. */
} RedisModuleTimer;

/* This is the timer handler that is called by the main event loop. We schedule
 * this timer to be called when the nearest of our module timers will expire. */
int moduleTimerHandler(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    /* To start let's try to fire all the timers already expired. */
    raxIterator ri;
    raxStart(&ri,Timers);
    uint64_t now = ustime();
    long long next_period = 0;
    while(1) {
        raxSeek(&ri,"^",NULL,0);
        if (!raxNext(&ri)) break;
        uint64_t expiretime;
        memcpy(&expiretime,ri.key,sizeof(expiretime));
        expiretime = ntohu64(expiretime);
        if (now >= expiretime) {
            RedisModuleTimer *timer = ri.data;
            RedisModuleCtx ctx;
            moduleCreateContext(&ctx,timer->module,REDISMODULE_CTX_TEMP_CLIENT);
            selectDb(ctx.client, timer->dbid);
            timer->callback(&ctx,timer->data);
            moduleFreeContext(&ctx);
            raxRemove(Timers,(unsigned char*)ri.key,ri.key_len,NULL);
            zfree(timer);
        } else {
            /* We call ustime() again instead of using the cached 'now' so that
             * 'next_period' isn't affected by the time it took to execute
             * previous calls to 'callback.
             * We need to cast 'expiretime' so that the compiler will not treat
             * the difference as unsigned (Causing next_period to be huge) in
             * case expiretime < ustime() */
            next_period = ((long long)expiretime-ustime())/1000; /* Scale to milliseconds. */
            break;
        }
    }
    raxStop(&ri);

    /* Reschedule the next timer or cancel it. */
    if (next_period <= 0) next_period = 1;
    if (raxSize(Timers) > 0) {
        return next_period;
    } else {
        aeTimer = -1;
        return AE_NOMORE;
    }
}

/* Create a new timer that will fire after `period` milliseconds, and will call
 * the specified function using `data` as argument. The returned timer ID can be
 * used to get information from the timer or to stop it before it fires.
 * Note that for the common use case of a repeating timer (Re-registration
 * of the timer inside the RedisModuleTimerProc callback) it matters when
 * this API is called:
 * If it is called at the beginning of 'callback' it means
 * the event will triggered every 'period'.
 * If it is called at the end of 'callback' it means
 * there will 'period' milliseconds gaps between events.
 * (If the time it takes to execute 'callback' is negligible the two
 * statements above mean the same) */
RedisModuleTimerID RM_CreateTimer(RedisModuleCtx *ctx, mstime_t period, RedisModuleTimerProc callback, void *data) {
    RedisModuleTimer *timer = zmalloc(sizeof(*timer));
    timer->module = ctx->module;
    timer->callback = callback;
    timer->data = data;
    timer->dbid = ctx->client ? ctx->client->db->id : 0;
    uint64_t expiretime = ustime()+period*1000;
    uint64_t key;

    while(1) {
        key = htonu64(expiretime);
        if (raxFind(Timers, (unsigned char*)&key,sizeof(key)) == raxNotFound) {
            raxInsert(Timers,(unsigned char*)&key,sizeof(key),timer,NULL);
            break;
        } else {
            expiretime++;
        }
    }

    /* We need to install the main event loop timer if it's not already
     * installed, or we may need to refresh its period if we just installed
     * a timer that will expire sooner than any other else (i.e. the timer
     * we just installed is the first timer in the Timers rax). */
    if (aeTimer != -1) {
        raxIterator ri;
        raxStart(&ri,Timers);
        raxSeek(&ri,"^",NULL,0);
        raxNext(&ri);
        if (memcmp(ri.key,&key,sizeof(key)) == 0) {
            /* This is the first key, we need to re-install the timer according
             * to the just added event. */
            aeDeleteTimeEvent(server.el,aeTimer);
            aeTimer = -1;
        }
        raxStop(&ri);
    }

    /* If we have no main timer (the old one was invalidated, or this is the
     * first module timer we have), install one. */
    if (aeTimer == -1)
        aeTimer = aeCreateTimeEvent(server.el,period,moduleTimerHandler,NULL,NULL);

    return key;
}

/* Stop a timer, returns REDISMODULE_OK if the timer was found, belonged to the
 * calling module, and was stopped, otherwise REDISMODULE_ERR is returned.
 * If not NULL, the data pointer is set to the value of the data argument when
 * the timer was created. */
int RM_StopTimer(RedisModuleCtx *ctx, RedisModuleTimerID id, void **data) {
    RedisModuleTimer *timer = raxFind(Timers,(unsigned char*)&id,sizeof(id));
    if (timer == raxNotFound || timer->module != ctx->module)
        return REDISMODULE_ERR;
    if (data) *data = timer->data;
    raxRemove(Timers,(unsigned char*)&id,sizeof(id),NULL);
    zfree(timer);
    return REDISMODULE_OK;
}

/* Obtain information about a timer: its remaining time before firing
 * (in milliseconds), and the private data pointer associated with the timer.
 * If the timer specified does not exist or belongs to a different module
 * no information is returned and the function returns REDISMODULE_ERR, otherwise
 * REDISMODULE_OK is returned. The arguments remaining or data can be NULL if
 * the caller does not need certain information. */
int RM_GetTimerInfo(RedisModuleCtx *ctx, RedisModuleTimerID id, uint64_t *remaining, void **data) {
    RedisModuleTimer *timer = raxFind(Timers,(unsigned char*)&id,sizeof(id));
    if (timer == raxNotFound || timer->module != ctx->module)
        return REDISMODULE_ERR;
    if (remaining) {
        int64_t rem = ntohu64(id)-ustime();
        if (rem < 0) rem = 0;
        *remaining = rem/1000; /* Scale to milliseconds. */
    }
    if (data) *data = timer->data;
    return REDISMODULE_OK;
}

/* Query timers to see if any timer belongs to the module.
 * Return 1 if any timer was found, otherwise 0 would be returned. */
int moduleHoldsTimer(struct RedisModule *module) {
    raxIterator iter;
    int found = 0;
    raxStart(&iter,Timers);
    raxSeek(&iter,"^",NULL,0);
    while (raxNext(&iter)) {
        RedisModuleTimer *timer = iter.data;
        if (timer->module == module) {
            found = 1;
            break;
        }
    }
    raxStop(&iter);
    return found;
}

/* --------------------------------------------------------------------------
 * ## Modules EventLoop API
 * --------------------------------------------------------------------------*/

typedef struct EventLoopData {
    RedisModuleEventLoopFunc rFunc;
    RedisModuleEventLoopFunc wFunc;
    void *user_data;
} EventLoopData;

typedef struct EventLoopOneShot {
    RedisModuleEventLoopOneShotFunc func;
    void *user_data;
} EventLoopOneShot;

list *moduleEventLoopOneShots;
static pthread_mutex_t moduleEventLoopMutex = PTHREAD_MUTEX_INITIALIZER;

static int eventLoopToAeMask(int mask) {
    int aeMask = 0;
    if (mask & REDISMODULE_EVENTLOOP_READABLE)
        aeMask |= AE_READABLE;
    if (mask & REDISMODULE_EVENTLOOP_WRITABLE)
        aeMask |= AE_WRITABLE;
    return aeMask;
}

static int eventLoopFromAeMask(int ae_mask) {
    int mask = 0;
    if (ae_mask & AE_READABLE)
        mask |= REDISMODULE_EVENTLOOP_READABLE;
    if (ae_mask & AE_WRITABLE)
        mask |= REDISMODULE_EVENTLOOP_WRITABLE;
    return mask;
}

static void eventLoopCbReadable(struct aeEventLoop *ae, int fd, void *user_data, int ae_mask) {
    UNUSED(ae);
    EventLoopData *data = user_data;
    data->rFunc(fd, data->user_data, eventLoopFromAeMask(ae_mask));
}

static void eventLoopCbWritable(struct aeEventLoop *ae, int fd, void *user_data, int ae_mask) {
    UNUSED(ae);
    EventLoopData *data = user_data;
    data->wFunc(fd, data->user_data, eventLoopFromAeMask(ae_mask));
}

/* Add a pipe / socket event to the event loop.
 *
 * * `mask` must be one of the following values:
 *
 *     * `REDISMODULE_EVENTLOOP_READABLE`
 *     * `REDISMODULE_EVENTLOOP_WRITABLE`
 *     * `REDISMODULE_EVENTLOOP_READABLE | REDISMODULE_EVENTLOOP_WRITABLE`
 *
 * On success REDISMODULE_OK is returned, otherwise
 * REDISMODULE_ERR is returned and errno is set to the following values:
 *
 * * ERANGE: `fd` is negative or higher than `maxclients` Redis config.
 * * EINVAL: `callback` is NULL or `mask` value is invalid.
 *
 * `errno` might take other values in case of an internal error.
 *
 * Example:
 *
 *     void onReadable(int fd, void *user_data, int mask) {
 *         char buf[32];
 *         int bytes = read(fd,buf,sizeof(buf));
 *         printf("Read %d bytes \n", bytes);
 *     }
 *     RM_EventLoopAdd(fd, REDISMODULE_EVENTLOOP_READABLE, onReadable, NULL);
 */
int RM_EventLoopAdd(int fd, int mask, RedisModuleEventLoopFunc func, void *user_data) {
    if (fd < 0 || fd >= aeGetSetSize(server.el)) {
        errno = ERANGE;
        return REDISMODULE_ERR;
    }

    if (!func || mask & ~(REDISMODULE_EVENTLOOP_READABLE |
                          REDISMODULE_EVENTLOOP_WRITABLE)) {
        errno = EINVAL;
        return REDISMODULE_ERR;
    }

    /* We are going to register stub callbacks to 'ae' for two reasons:
     *
     * - "ae" callback signature is different from RedisModuleEventLoopCallback,
     *   that will be handled it in our stub callbacks.
     * - We need to remap 'mask' value to provide binary compatibility.
     *
     * For the stub callbacks, saving user 'callback' and 'user_data' in an
     * EventLoopData object and passing it to ae, later, we'll extract
     * 'callback' and 'user_data' from that.
     */
    EventLoopData *data = aeGetFileClientData(server.el, fd);
    if (!data)
        data = zcalloc(sizeof(*data));

    aeFileProc *aeProc;
    if (mask & REDISMODULE_EVENTLOOP_READABLE)
        aeProc = eventLoopCbReadable;
    else
        aeProc = eventLoopCbWritable;

    int aeMask = eventLoopToAeMask(mask);

    if (aeCreateFileEvent(server.el, fd, aeMask, aeProc, data) != AE_OK) {
        if (aeGetFileEvents(server.el, fd) == AE_NONE)
            zfree(data);
        return REDISMODULE_ERR;
    }

    data->user_data = user_data;
    if (mask & REDISMODULE_EVENTLOOP_READABLE)
        data->rFunc = func;
    if (mask & REDISMODULE_EVENTLOOP_WRITABLE)
        data->wFunc = func;

    errno = 0;
    return REDISMODULE_OK;
}

/* Delete a pipe / socket event from the event loop.
 *
 * * `mask` must be one of the following values:
 *
 *     * `REDISMODULE_EVENTLOOP_READABLE`
 *     * `REDISMODULE_EVENTLOOP_WRITABLE`
 *     * `REDISMODULE_EVENTLOOP_READABLE | REDISMODULE_EVENTLOOP_WRITABLE`
 *
 * On success REDISMODULE_OK is returned, otherwise
 * REDISMODULE_ERR is returned and errno is set to the following values:
 *
 * * ERANGE: `fd` is negative or higher than `maxclients` Redis config.
 * * EINVAL: `mask` value is invalid.
 */
int RM_EventLoopDel(int fd, int mask) {
    if (fd < 0 || fd >= aeGetSetSize(server.el)) {
        errno = ERANGE;
        return REDISMODULE_ERR;
    }

    if (mask & ~(REDISMODULE_EVENTLOOP_READABLE |
                 REDISMODULE_EVENTLOOP_WRITABLE)) {
        errno = EINVAL;
        return REDISMODULE_ERR;
    }

    /* After deleting the event, if fd does not have any registered event
     * anymore, we can free the EventLoopData object. */
    EventLoopData *data = aeGetFileClientData(server.el, fd);
    aeDeleteFileEvent(server.el, fd, eventLoopToAeMask(mask));
    if (aeGetFileEvents(server.el, fd) == AE_NONE)
        zfree(data);

    errno = 0;
    return REDISMODULE_OK;
}

/* This function can be called from other threads to trigger callback on Redis
 * main thread. On success REDISMODULE_OK is returned. If `func` is NULL
 * REDISMODULE_ERR is returned and errno is set to EINVAL.
 */
int RM_EventLoopAddOneShot(RedisModuleEventLoopOneShotFunc func, void *user_data) {
    if (!func) {
        errno = EINVAL;
        return REDISMODULE_ERR;
    }

    EventLoopOneShot *oneshot = zmalloc(sizeof(*oneshot));
    oneshot->func = func;
    oneshot->user_data = user_data;

    pthread_mutex_lock(&moduleEventLoopMutex);
    if (!moduleEventLoopOneShots) moduleEventLoopOneShots = listCreate();
    listAddNodeTail(moduleEventLoopOneShots, oneshot);
    pthread_mutex_unlock(&moduleEventLoopMutex);

    if (write(server.module_pipe[1],"A",1) != 1) {
        /* Pipe is non-blocking, write() may fail if it's full. */
    }

    errno = 0;
    return REDISMODULE_OK;
}

/* This function will check the moduleEventLoopOneShots queue in order to
 * call the callback for the registered oneshot events. */
static void eventLoopHandleOneShotEvents() {
    pthread_mutex_lock(&moduleEventLoopMutex);
    if (moduleEventLoopOneShots) {
        while (listLength(moduleEventLoopOneShots)) {
            listNode *ln = listFirst(moduleEventLoopOneShots);
            EventLoopOneShot *oneshot = ln->value;
            listDelNode(moduleEventLoopOneShots, ln);
            /* Unlock mutex before the callback. Another oneshot event can be
             * added in the callback, it will need to lock the mutex. */
            pthread_mutex_unlock(&moduleEventLoopMutex);
            oneshot->func(oneshot->user_data);
            zfree(oneshot);
            /* Lock again for the next iteration */
            pthread_mutex_lock(&moduleEventLoopMutex);
        }
    }
    pthread_mutex_unlock(&moduleEventLoopMutex);
}

/* --------------------------------------------------------------------------
 * ## Modules ACL API
 *
 * Implements a hook into the authentication and authorization within Redis.
 * --------------------------------------------------------------------------*/

/* This function is called when a client's user has changed and invokes the
 * client's user changed callback if it was set. This callback should
 * cleanup any state the module was tracking about this client.
 *
 * A client's user can be changed through the AUTH command, module
 * authentication, and when a client is freed. */
void moduleNotifyUserChanged(client *c) {
    if (c->auth_callback) {
        c->auth_callback(c->id, c->auth_callback_privdata);

        /* The callback will fire exactly once, even if the user remains
         * the same. It is expected to completely clean up the state
         * so all references are cleared here. */
        c->auth_callback = NULL;
        c->auth_callback_privdata = NULL;
        c->auth_module = NULL;
    }
}

void revokeClientAuthentication(client *c) {
    /* Freeing the client would result in moduleNotifyUserChanged() to be
     * called later, however since we use revokeClientAuthentication() also
     * in moduleFreeAuthenticatedClients() to implement module unloading, we
     * do this action ASAP: this way if the module is unloaded, when the client
     * is eventually freed we don't rely on the module to still exist. */
    moduleNotifyUserChanged(c);

    c->user = DefaultUser;
    c->authenticated = 0;
    /* We will write replies to this client later, so we can't close it
     * directly even if async. */
    if (c == server.current_client) {
        c->flags |= CLIENT_CLOSE_AFTER_COMMAND;
    } else {
        freeClientAsync(c);
    }
}

/* Cleanup all clients that have been authenticated with this module. This
 * is called from onUnload() to give the module a chance to cleanup any
 * resources associated with clients it has authenticated. */
static void moduleFreeAuthenticatedClients(RedisModule *module) {
    listIter li;
    listNode *ln;
    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        if (!c->auth_module) continue;

        RedisModule *auth_module = (RedisModule *) c->auth_module;
        if (auth_module == module) {
            revokeClientAuthentication(c);
        }
    }
}

/* Creates a Redis ACL user that the module can use to authenticate a client.
 * After obtaining the user, the module should set what such user can do
 * using the RM_SetUserACL() function. Once configured, the user
 * can be used in order to authenticate a connection, with the specified
 * ACL rules, using the RedisModule_AuthClientWithUser() function.
 *
 * Note that:
 *
 * * Users created here are not listed by the ACL command.
 * * Users created here are not checked for duplicated name, so it's up to
 *   the module calling this function to take care of not creating users
 *   with the same name.
 * * The created user can be used to authenticate multiple Redis connections.
 *
 * The caller can later free the user using the function
 * RM_FreeModuleUser(). When this function is called, if there are
 * still clients authenticated with this user, they are disconnected.
 * The function to free the user should only be used when the caller really
 * wants to invalidate the user to define a new one with different
 * capabilities. */
RedisModuleUser *RM_CreateModuleUser(const char *name) {
    RedisModuleUser *new_user = zmalloc(sizeof(RedisModuleUser));
    new_user->user = ACLCreateUnlinkedUser();
    new_user->free_user = 1;

    /* Free the previous temporarily assigned name to assign the new one */
    sdsfree(new_user->user->name);
    new_user->user->name = sdsnew(name);
    return new_user;
}

/* Frees a given user and disconnects all of the clients that have been
 * authenticated with it. See RM_CreateModuleUser for detailed usage.*/
int RM_FreeModuleUser(RedisModuleUser *user) {
    if (user->free_user)
        ACLFreeUserAndKillClients(user->user);
    zfree(user);
    return REDISMODULE_OK;
}

/* Sets the permissions of a user created through the redis module
 * interface. The syntax is the same as ACL SETUSER, so refer to the
 * documentation in acl.c for more information. See RM_CreateModuleUser
 * for detailed usage.
 *
 * Returns REDISMODULE_OK on success and REDISMODULE_ERR on failure
 * and will set an errno describing why the operation failed. */
int RM_SetModuleUserACL(RedisModuleUser *user, const char* acl) {
    return ACLSetUser(user->user, acl, -1);
}

/* Sets the permission of a user with a complete ACL string, such as one
 * would use on the redis ACL SETUSER command line API. This differs from
 * RM_SetModuleUserACL, which only takes single ACL operations at a time.
 *
 * Returns REDISMODULE_OK on success and REDISMODULE_ERR on failure
 * if a RedisModuleString is provided in error, a string describing the error
 * will be returned */
int RM_SetModuleUserACLString(RedisModuleCtx *ctx, RedisModuleUser *user, const char *acl, RedisModuleString **error) {
    serverAssert(user != NULL);

    int argc;
    sds *argv = sdssplitargs(acl, &argc);

    sds err = ACLStringSetUser(user->user, NULL, argv, argc);

    sdsfreesplitres(argv, argc);

    if (err) {
        if (error) {
            *error = createObject(OBJ_STRING, err);
            if (ctx != NULL) autoMemoryAdd(ctx, REDISMODULE_AM_STRING, *error);
        } else {
            sdsfree(err);
        }

        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

/* Get the ACL string for a given user
 * Returns a RedisModuleString
 */
RedisModuleString *RM_GetModuleUserACLString(RedisModuleUser *user) {
    serverAssert(user != NULL);

    return ACLDescribeUser(user->user);
}

/* Retrieve the user name of the client connection behind the current context.
 * The user name can be used later, in order to get a RedisModuleUser.
 * See more information in RM_GetModuleUserFromUserName.
 *
 * The returned string must be released with RedisModule_FreeString() or by
 * enabling automatic memory management. */
RedisModuleString *RM_GetCurrentUserName(RedisModuleCtx *ctx) {
    return RM_CreateString(ctx,ctx->client->user->name,sdslen(ctx->client->user->name));
}

/* A RedisModuleUser can be used to check if command, key or channel can be executed or
 * accessed according to the ACLs rules associated with that user.
 * When a Module wants to do ACL checks on a general ACL user (not created by RM_CreateModuleUser),
 * it can get the RedisModuleUser from this API, based on the user name retrieved by RM_GetCurrentUserName.
 *
 * Since a general ACL user can be deleted at any time, this RedisModuleUser should be used only in the context
 * where this function was called. In order to do ACL checks out of that context, the Module can store the user name,
 * and call this API at any other context.
 *
 * Returns NULL if the user is disabled or the user does not exist.
 * The caller should later free the user using the function RM_FreeModuleUser().*/
RedisModuleUser *RM_GetModuleUserFromUserName(RedisModuleString *name) {
    /* First, verify that the user exist */
    user *acl_user = ACLGetUserByName(name->ptr, sdslen(name->ptr));
    if (acl_user == NULL) {
        return NULL;
    }

    RedisModuleUser *new_user = zmalloc(sizeof(RedisModuleUser));
    new_user->user = acl_user;
    new_user->free_user = 0;
    return new_user;
}

/* Checks if the command can be executed by the user, according to the ACLs associated with it.
 *
 * On success a REDISMODULE_OK is returned, otherwise
 * REDISMODULE_ERR is returned and errno is set to the following values:
 *
 * * ENOENT: Specified command does not exist.
 * * EACCES: Command cannot be executed, according to ACL rules
 */
int RM_ACLCheckCommandPermissions(RedisModuleUser *user, RedisModuleString **argv, int argc) {
    int keyidxptr;
    struct redisCommand *cmd;

    /* Find command */
    if ((cmd = lookupCommand(argv, argc)) == NULL) {
        errno = ENOENT;
        return REDISMODULE_ERR;
    }

    if (ACLCheckAllUserCommandPerm(user->user, cmd, argv, argc, &keyidxptr) != ACL_OK) {
        errno = EACCES;
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

/* Check if the key can be accessed by the user according to the ACLs attached to the user
 * and the flags representing the key access. The flags are the same that are used in the
 * keyspec for logical operations. These flags are documented in RedisModule_SetCommandInfo as
 * the REDISMODULE_CMD_KEY_ACCESS, REDISMODULE_CMD_KEY_UPDATE, REDISMODULE_CMD_KEY_INSERT,
 * and REDISMODULE_CMD_KEY_DELETE flags.
 * 
 * If no flags are supplied, the user is still required to have some access to the key for
 * this command to return successfully.
 *
 * If the user is able to access the key then REDISMODULE_OK is returned, otherwise
 * REDISMODULE_ERR is returned and errno is set to one of the following values:
 * 
 * * EINVAL: The provided flags are invalid.
 * * EACCESS: The user does not have permission to access the key.
 */
int RM_ACLCheckKeyPermissions(RedisModuleUser *user, RedisModuleString *key, int flags) {
    const int allow_mask = (REDISMODULE_CMD_KEY_ACCESS
        | REDISMODULE_CMD_KEY_INSERT
        | REDISMODULE_CMD_KEY_DELETE
        | REDISMODULE_CMD_KEY_UPDATE);

    if ((flags & allow_mask) != flags) {
        errno = EINVAL;
        return REDISMODULE_ERR;
    }

    int keyspec_flags = moduleConvertKeySpecsFlags(flags, 0);
    if (ACLUserCheckKeyPerm(user->user, key->ptr, sdslen(key->ptr), keyspec_flags) != ACL_OK) {
        errno = EACCES;
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

/* Check if the pubsub channel can be accessed by the user based off of the given
 * access flags. See RM_ChannelAtPosWithFlags for more information about the
 * possible flags that can be passed in.
 *
 * If the user is able to access the pubsub channel then REDISMODULE_OK is returned, otherwise
 * REDISMODULE_ERR is returned and errno is set to one of the following values:
 * 
 * * EINVAL: The provided flags are invalid.
 * * EACCESS: The user does not have permission to access the pubsub channel. 
 */
int RM_ACLCheckChannelPermissions(RedisModuleUser *user, RedisModuleString *ch, int flags) {
    const int allow_mask = (REDISMODULE_CMD_CHANNEL_PUBLISH
        | REDISMODULE_CMD_CHANNEL_SUBSCRIBE
        | REDISMODULE_CMD_CHANNEL_UNSUBSCRIBE
        | REDISMODULE_CMD_CHANNEL_PATTERN);

    if ((flags & allow_mask) != flags) {
        errno = EINVAL;
        return REDISMODULE_ERR;
    }

    /* Unsubscribe permissions are currently always allowed. */
    if (flags & REDISMODULE_CMD_CHANNEL_UNSUBSCRIBE){
        return REDISMODULE_OK;
    }

    int is_pattern = flags & REDISMODULE_CMD_CHANNEL_PATTERN;
    if (ACLUserCheckChannelPerm(user->user, ch->ptr, is_pattern) != ACL_OK)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

/* Helper function to map a RedisModuleACLLogEntryReason to ACL Log entry reason. */
int moduleGetACLLogEntryReason(RedisModuleACLLogEntryReason reason) {
    int acl_reason = 0;
    switch (reason) {
        case REDISMODULE_ACL_LOG_AUTH: acl_reason = ACL_DENIED_AUTH; break;
        case REDISMODULE_ACL_LOG_KEY: acl_reason = ACL_DENIED_KEY; break;
        case REDISMODULE_ACL_LOG_CHANNEL: acl_reason = ACL_DENIED_CHANNEL; break;
        case REDISMODULE_ACL_LOG_CMD: acl_reason = ACL_DENIED_CMD; break;
        default: break;
    }
    return acl_reason;
}

/* Adds a new entry in the ACL log.
 * Returns REDISMODULE_OK on success and REDISMODULE_ERR on error.
 *
 * For more information about ACL log, please refer to https://redis.io/commands/acl-log */
int RM_ACLAddLogEntry(RedisModuleCtx *ctx, RedisModuleUser *user, RedisModuleString *object, RedisModuleACLLogEntryReason reason) {
    int acl_reason = moduleGetACLLogEntryReason(reason);
    if (!acl_reason) return REDISMODULE_ERR;
    addACLLogEntry(ctx->client, acl_reason, ACL_LOG_CTX_MODULE, -1, user->user->name, sdsdup(object->ptr));
    return REDISMODULE_OK;
}

/* Adds a new entry in the ACL log with the `username` RedisModuleString provided.
 * Returns REDISMODULE_OK on success and REDISMODULE_ERR on error.
 *
 * For more information about ACL log, please refer to https://redis.io/commands/acl-log */
int RM_ACLAddLogEntryByUserName(RedisModuleCtx *ctx, RedisModuleString *username, RedisModuleString *object, RedisModuleACLLogEntryReason reason) {
    int acl_reason = moduleGetACLLogEntryReason(reason);
    if (!acl_reason) return REDISMODULE_ERR;
    addACLLogEntry(ctx->client, acl_reason, ACL_LOG_CTX_MODULE, -1, username->ptr, sdsdup(object->ptr));
    return REDISMODULE_OK;
}

/* Authenticate the client associated with the context with
 * the provided user. Returns REDISMODULE_OK on success and
 * REDISMODULE_ERR on error.
 *
 * This authentication can be tracked with the optional callback and private
 * data fields. The callback will be called whenever the user of the client
 * changes. This callback should be used to cleanup any state that is being
 * kept in the module related to the client authentication. It will only be
 * called once, even when the user hasn't changed, in order to allow for a
 * new callback to be specified. If this authentication does not need to be
 * tracked, pass in NULL for the callback and privdata.
 *
 * If client_id is not NULL, it will be filled with the id of the client
 * that was authenticated. This can be used with the
 * RM_DeauthenticateAndCloseClient() API in order to deauthenticate a
 * previously authenticated client if the authentication is no longer valid.
 *
 * For expensive authentication operations, it is recommended to block the
 * client and do the authentication in the background and then attach the user
 * to the client in a threadsafe context. */
static int authenticateClientWithUser(RedisModuleCtx *ctx, user *user, RedisModuleUserChangedFunc callback, void *privdata, uint64_t *client_id) {
    if (user->flags & USER_FLAG_DISABLED) {
        return REDISMODULE_ERR;
    }

    /* Avoid settings which are meaningless and will be lost */
    if (!ctx->client || (ctx->client->flags & CLIENT_MODULE)) {
        return REDISMODULE_ERR;
    }

    moduleNotifyUserChanged(ctx->client);

    ctx->client->user = user;
    ctx->client->authenticated = 1;

    if (clientHasModuleAuthInProgress(ctx->client)) {
        ctx->client->flags |= CLIENT_MODULE_AUTH_HAS_RESULT;
    }

    if (callback) {
        ctx->client->auth_callback = callback;
        ctx->client->auth_callback_privdata = privdata;
        ctx->client->auth_module = ctx->module;
    }

    if (client_id) {
        *client_id = ctx->client->id;
    }

    return REDISMODULE_OK;
}


/* Authenticate the current context's user with the provided redis acl user.
 * Returns REDISMODULE_ERR if the user is disabled.
 *
 * See authenticateClientWithUser for information about callback, client_id,
 * and general usage for authentication. */
int RM_AuthenticateClientWithUser(RedisModuleCtx *ctx, RedisModuleUser *module_user, RedisModuleUserChangedFunc callback, void *privdata, uint64_t *client_id) {
    return authenticateClientWithUser(ctx, module_user->user, callback, privdata, client_id);
}

/* Authenticate the current context's user with the provided redis acl user.
 * Returns REDISMODULE_ERR if the user is disabled or the user does not exist.
 *
 * See authenticateClientWithUser for information about callback, client_id,
 * and general usage for authentication. */
int RM_AuthenticateClientWithACLUser(RedisModuleCtx *ctx, const char *name, size_t len, RedisModuleUserChangedFunc callback, void *privdata, uint64_t *client_id) {
    user *acl_user = ACLGetUserByName(name, len);

    if (!acl_user) {
        return REDISMODULE_ERR;
    }
    return authenticateClientWithUser(ctx, acl_user, callback, privdata, client_id);
}

/* Deauthenticate and close the client. The client resources will not be
 * immediately freed, but will be cleaned up in a background job. This is
 * the recommended way to deauthenticate a client since most clients can't
 * handle users becoming deauthenticated. Returns REDISMODULE_ERR when the
 * client doesn't exist and REDISMODULE_OK when the operation was successful.
 *
 * The client ID is returned from the RM_AuthenticateClientWithUser and
 * RM_AuthenticateClientWithACLUser APIs, but can be obtained through
 * the CLIENT api or through server events.
 *
 * This function is not thread safe, and must be executed within the context
 * of a command or thread safe context. */
int RM_DeauthenticateAndCloseClient(RedisModuleCtx *ctx, uint64_t client_id) {
    UNUSED(ctx);
    client *c = lookupClientByID(client_id);
    if (c == NULL) return REDISMODULE_ERR;

    /* Revoke also marks client to be closed ASAP */
    revokeClientAuthentication(c);
    return REDISMODULE_OK;
}

/* Redact the client command argument specified at the given position. Redacted arguments 
 * are obfuscated in user facing commands such as SLOWLOG or MONITOR, as well as
 * never being written to server logs. This command may be called multiple times on the
 * same position.
 * 
 * Note that the command name, position 0, can not be redacted. 
 * 
 * Returns REDISMODULE_OK if the argument was redacted and REDISMODULE_ERR if there 
 * was an invalid parameter passed in or the position is outside the client 
 * argument range. */
int RM_RedactClientCommandArgument(RedisModuleCtx *ctx, int pos) {
    if (!ctx || !ctx->client || pos <= 0 || ctx->client->argc <= pos) {
        return REDISMODULE_ERR;
    }
    redactClientCommandArgument(ctx->client, pos);
    return REDISMODULE_OK;
}

/* Return the X.509 client-side certificate used by the client to authenticate
 * this connection.
 *
 * The return value is an allocated RedisModuleString that is a X.509 certificate
 * encoded in PEM (Base64) format. It should be freed (or auto-freed) by the caller.
 *
 * A NULL value is returned in the following conditions:
 *
 * - Connection ID does not exist
 * - Connection is not a TLS connection
 * - Connection is a TLS connection but no client certificate was used
 */
RedisModuleString *RM_GetClientCertificate(RedisModuleCtx *ctx, uint64_t client_id) {
    client *c = lookupClientByID(client_id);
    if (c == NULL) return NULL;

    sds cert = connGetPeerCert(c->conn);
    if (!cert) return NULL;

    RedisModuleString *s = createObject(OBJ_STRING, cert);
    if (ctx != NULL) autoMemoryAdd(ctx, REDISMODULE_AM_STRING, s);

    return s;
}

/* --------------------------------------------------------------------------
 * ## Modules Dictionary API
 *
 * Implements a sorted dictionary (actually backed by a radix tree) with
 * the usual get / set / del / num-items API, together with an iterator
 * capable of going back and forth.
 * -------------------------------------------------------------------------- */

/* Create a new dictionary. The 'ctx' pointer can be the current module context
 * or NULL, depending on what you want. Please follow the following rules:
 *
 * 1. Use a NULL context if you plan to retain a reference to this dictionary
 *    that will survive the time of the module callback where you created it.
 * 2. Use a NULL context if no context is available at the time you are creating
 *    the dictionary (of course...).
 * 3. However use the current callback context as 'ctx' argument if the
 *    dictionary time to live is just limited to the callback scope. In this
 *    case, if enabled, you can enjoy the automatic memory management that will
 *    reclaim the dictionary memory, as well as the strings returned by the
 *    Next / Prev dictionary iterator calls.
 */
RedisModuleDict *RM_CreateDict(RedisModuleCtx *ctx) {
    struct RedisModuleDict *d = zmalloc(sizeof(*d));
    d->rax = raxNew();
    if (ctx != NULL) autoMemoryAdd(ctx,REDISMODULE_AM_DICT,d);
    return d;
}

/* Free a dictionary created with RM_CreateDict(). You need to pass the
 * context pointer 'ctx' only if the dictionary was created using the
 * context instead of passing NULL. */
void RM_FreeDict(RedisModuleCtx *ctx, RedisModuleDict *d) {
    if (ctx != NULL) autoMemoryFreed(ctx,REDISMODULE_AM_DICT,d);
    raxFree(d->rax);
    zfree(d);
}

/* Return the size of the dictionary (number of keys). */
uint64_t RM_DictSize(RedisModuleDict *d) {
    return raxSize(d->rax);
}

/* Store the specified key into the dictionary, setting its value to the
 * pointer 'ptr'. If the key was added with success, since it did not
 * already exist, REDISMODULE_OK is returned. Otherwise if the key already
 * exists the function returns REDISMODULE_ERR. */
int RM_DictSetC(RedisModuleDict *d, void *key, size_t keylen, void *ptr) {
    int retval = raxTryInsert(d->rax,key,keylen,ptr,NULL);
    return (retval == 1) ? REDISMODULE_OK : REDISMODULE_ERR;
}

/* Like RedisModule_DictSetC() but will replace the key with the new
 * value if the key already exists. */
int RM_DictReplaceC(RedisModuleDict *d, void *key, size_t keylen, void *ptr) {
    int retval = raxInsert(d->rax,key,keylen,ptr,NULL);
    return (retval == 1) ? REDISMODULE_OK : REDISMODULE_ERR;
}

/* Like RedisModule_DictSetC() but takes the key as a RedisModuleString. */
int RM_DictSet(RedisModuleDict *d, RedisModuleString *key, void *ptr) {
    return RM_DictSetC(d,key->ptr,sdslen(key->ptr),ptr);
}

/* Like RedisModule_DictReplaceC() but takes the key as a RedisModuleString. */
int RM_DictReplace(RedisModuleDict *d, RedisModuleString *key, void *ptr) {
    return RM_DictReplaceC(d,key->ptr,sdslen(key->ptr),ptr);
}

/* Return the value stored at the specified key. The function returns NULL
 * both in the case the key does not exist, or if you actually stored
 * NULL at key. So, optionally, if the 'nokey' pointer is not NULL, it will
 * be set by reference to 1 if the key does not exist, or to 0 if the key
 * exists. */
void *RM_DictGetC(RedisModuleDict *d, void *key, size_t keylen, int *nokey) {
    void *res = raxFind(d->rax,key,keylen);
    if (nokey) *nokey = (res == raxNotFound);
    return (res == raxNotFound) ? NULL : res;
}

/* Like RedisModule_DictGetC() but takes the key as a RedisModuleString. */
void *RM_DictGet(RedisModuleDict *d, RedisModuleString *key, int *nokey) {
    return RM_DictGetC(d,key->ptr,sdslen(key->ptr),nokey);
}

/* Remove the specified key from the dictionary, returning REDISMODULE_OK if
 * the key was found and deleted, or REDISMODULE_ERR if instead there was
 * no such key in the dictionary. When the operation is successful, if
 * 'oldval' is not NULL, then '*oldval' is set to the value stored at the
 * key before it was deleted. Using this feature it is possible to get
 * a pointer to the value (for instance in order to release it), without
 * having to call RedisModule_DictGet() before deleting the key. */
int RM_DictDelC(RedisModuleDict *d, void *key, size_t keylen, void *oldval) {
    int retval = raxRemove(d->rax,key,keylen,oldval);
    return retval ? REDISMODULE_OK : REDISMODULE_ERR;
}

/* Like RedisModule_DictDelC() but gets the key as a RedisModuleString. */
int RM_DictDel(RedisModuleDict *d, RedisModuleString *key, void *oldval) {
    return RM_DictDelC(d,key->ptr,sdslen(key->ptr),oldval);
}

/* Return an iterator, setup in order to start iterating from the specified
 * key by applying the operator 'op', which is just a string specifying the
 * comparison operator to use in order to seek the first element. The
 * operators available are:
 *
 * * `^`   -- Seek the first (lexicographically smaller) key.
 * * `$`   -- Seek the last  (lexicographically bigger) key.
 * * `>`   -- Seek the first element greater than the specified key.
 * * `>=`  -- Seek the first element greater or equal than the specified key.
 * * `<`   -- Seek the first element smaller than the specified key.
 * * `<=`  -- Seek the first element smaller or equal than the specified key.
 * * `==`  -- Seek the first element matching exactly the specified key.
 *
 * Note that for `^` and `$` the passed key is not used, and the user may
 * just pass NULL with a length of 0.
 *
 * If the element to start the iteration cannot be seeked based on the
 * key and operator passed, RedisModule_DictNext() / Prev() will just return
 * REDISMODULE_ERR at the first call, otherwise they'll produce elements.
 */
RedisModuleDictIter *RM_DictIteratorStartC(RedisModuleDict *d, const char *op, void *key, size_t keylen) {
    RedisModuleDictIter *di = zmalloc(sizeof(*di));
    di->dict = d;
    raxStart(&di->ri,d->rax);
    raxSeek(&di->ri,op,key,keylen);
    return di;
}

/* Exactly like RedisModule_DictIteratorStartC, but the key is passed as a
 * RedisModuleString. */
RedisModuleDictIter *RM_DictIteratorStart(RedisModuleDict *d, const char *op, RedisModuleString *key) {
    return RM_DictIteratorStartC(d,op,key->ptr,sdslen(key->ptr));
}

/* Release the iterator created with RedisModule_DictIteratorStart(). This call
 * is mandatory otherwise a memory leak is introduced in the module. */
void RM_DictIteratorStop(RedisModuleDictIter *di) {
    raxStop(&di->ri);
    zfree(di);
}

/* After its creation with RedisModule_DictIteratorStart(), it is possible to
 * change the currently selected element of the iterator by using this
 * API call. The result based on the operator and key is exactly like
 * the function RedisModule_DictIteratorStart(), however in this case the
 * return value is just REDISMODULE_OK in case the seeked element was found,
 * or REDISMODULE_ERR in case it was not possible to seek the specified
 * element. It is possible to reseek an iterator as many times as you want. */
int RM_DictIteratorReseekC(RedisModuleDictIter *di, const char *op, void *key, size_t keylen) {
    return raxSeek(&di->ri,op,key,keylen);
}

/* Like RedisModule_DictIteratorReseekC() but takes the key as a
 * RedisModuleString. */
int RM_DictIteratorReseek(RedisModuleDictIter *di, const char *op, RedisModuleString *key) {
    return RM_DictIteratorReseekC(di,op,key->ptr,sdslen(key->ptr));
}

/* Return the current item of the dictionary iterator `di` and steps to the
 * next element. If the iterator already yield the last element and there
 * are no other elements to return, NULL is returned, otherwise a pointer
 * to a string representing the key is provided, and the `*keylen` length
 * is set by reference (if keylen is not NULL). The `*dataptr`, if not NULL
 * is set to the value of the pointer stored at the returned key as auxiliary
 * data (as set by the RedisModule_DictSet API).
 *
 * Usage example:
 *
 *      ... create the iterator here ...
 *      char *key;
 *      void *data;
 *      while((key = RedisModule_DictNextC(iter,&keylen,&data)) != NULL) {
 *          printf("%.*s %p\n", (int)keylen, key, data);
 *      }
 *
 * The returned pointer is of type void because sometimes it makes sense
 * to cast it to a `char*` sometimes to an unsigned `char*` depending on the
 * fact it contains or not binary data, so this API ends being more
 * comfortable to use.
 *
 * The validity of the returned pointer is until the next call to the
 * next/prev iterator step. Also the pointer is no longer valid once the
 * iterator is released. */
void *RM_DictNextC(RedisModuleDictIter *di, size_t *keylen, void **dataptr) {
    if (!raxNext(&di->ri)) return NULL;
    if (keylen) *keylen = di->ri.key_len;
    if (dataptr) *dataptr = di->ri.data;
    return di->ri.key;
}

/* This function is exactly like RedisModule_DictNext() but after returning
 * the currently selected element in the iterator, it selects the previous
 * element (lexicographically smaller) instead of the next one. */
void *RM_DictPrevC(RedisModuleDictIter *di, size_t *keylen, void **dataptr) {
    if (!raxPrev(&di->ri)) return NULL;
    if (keylen) *keylen = di->ri.key_len;
    if (dataptr) *dataptr = di->ri.data;
    return di->ri.key;
}

/* Like RedisModuleNextC(), but instead of returning an internally allocated
 * buffer and key length, it returns directly a module string object allocated
 * in the specified context 'ctx' (that may be NULL exactly like for the main
 * API RedisModule_CreateString).
 *
 * The returned string object should be deallocated after use, either manually
 * or by using a context that has automatic memory management active. */
RedisModuleString *RM_DictNext(RedisModuleCtx *ctx, RedisModuleDictIter *di, void **dataptr) {
    size_t keylen;
    void *key = RM_DictNextC(di,&keylen,dataptr);
    if (key == NULL) return NULL;
    return RM_CreateString(ctx,key,keylen);
}

/* Like RedisModule_DictNext() but after returning the currently selected
 * element in the iterator, it selects the previous element (lexicographically
 * smaller) instead of the next one. */
RedisModuleString *RM_DictPrev(RedisModuleCtx *ctx, RedisModuleDictIter *di, void **dataptr) {
    size_t keylen;
    void *key = RM_DictPrevC(di,&keylen,dataptr);
    if (key == NULL) return NULL;
    return RM_CreateString(ctx,key,keylen);
}

/* Compare the element currently pointed by the iterator to the specified
 * element given by key/keylen, according to the operator 'op' (the set of
 * valid operators are the same valid for RedisModule_DictIteratorStart).
 * If the comparison is successful the command returns REDISMODULE_OK
 * otherwise REDISMODULE_ERR is returned.
 *
 * This is useful when we want to just emit a lexicographical range, so
 * in the loop, as we iterate elements, we can also check if we are still
 * on range.
 *
 * The function return REDISMODULE_ERR if the iterator reached the
 * end of elements condition as well. */
int RM_DictCompareC(RedisModuleDictIter *di, const char *op, void *key, size_t keylen) {
    if (raxEOF(&di->ri)) return REDISMODULE_ERR;
    int res = raxCompare(&di->ri,op,key,keylen);
    return res ? REDISMODULE_OK : REDISMODULE_ERR;
}

/* Like RedisModule_DictCompareC but gets the key to compare with the current
 * iterator key as a RedisModuleString. */
int RM_DictCompare(RedisModuleDictIter *di, const char *op, RedisModuleString *key) {
    if (raxEOF(&di->ri)) return REDISMODULE_ERR;
    int res = raxCompare(&di->ri,op,key->ptr,sdslen(key->ptr));
    return res ? REDISMODULE_OK : REDISMODULE_ERR;
}




/* --------------------------------------------------------------------------
 * ## Modules Info fields
 * -------------------------------------------------------------------------- */

int RM_InfoEndDictField(RedisModuleInfoCtx *ctx);

/* Used to start a new section, before adding any fields. the section name will
 * be prefixed by `<modulename>_` and must only include A-Z,a-z,0-9.
 * NULL or empty string indicates the default section (only `<modulename>`) is used.
 * When return value is REDISMODULE_ERR, the section should and will be skipped. */
int RM_InfoAddSection(RedisModuleInfoCtx *ctx, const char *name) {
    sds full_name = sdsdup(ctx->module->name);
    if (name != NULL && strlen(name) > 0)
        full_name = sdscatfmt(full_name, "_%s", name);

    /* Implicitly end dicts, instead of returning an error which is likely un checked. */
    if (ctx->in_dict_field)
        RM_InfoEndDictField(ctx);

    /* proceed only if:
     * 1) no section was requested (emit all)
     * 2) the module name was requested (emit all)
     * 3) this specific section was requested. */
    if (ctx->requested_sections) {
        if ((!full_name || !dictFind(ctx->requested_sections, full_name)) &&
            (!dictFind(ctx->requested_sections, ctx->module->name)))
        {
            sdsfree(full_name);
            ctx->in_section = 0;
            return REDISMODULE_ERR;
        }
    }
    if (ctx->sections++) ctx->info = sdscat(ctx->info,"\r\n");
    ctx->info = sdscatfmt(ctx->info, "# %S\r\n", full_name);
    ctx->in_section = 1;
    sdsfree(full_name);
    return REDISMODULE_OK;
}

/* Starts a dict field, similar to the ones in INFO KEYSPACE. Use normal
 * RedisModule_InfoAddField* functions to add the items to this field, and
 * terminate with RedisModule_InfoEndDictField. */
int RM_InfoBeginDictField(RedisModuleInfoCtx *ctx, const char *name) {
    if (!ctx->in_section)
        return REDISMODULE_ERR;
    /* Implicitly end dicts, instead of returning an error which is likely un checked. */
    if (ctx->in_dict_field)
        RM_InfoEndDictField(ctx);
    char *tmpmodname, *tmpname;
    ctx->info = sdscatfmt(ctx->info,
        "%s_%s:",
        getSafeInfoString(ctx->module->name, strlen(ctx->module->name), &tmpmodname),
        getSafeInfoString(name, strlen(name), &tmpname));
    if (tmpmodname != NULL) zfree(tmpmodname);
    if (tmpname != NULL) zfree(tmpname);
    ctx->in_dict_field = 1;
    return REDISMODULE_OK;
}

/* Ends a dict field, see RedisModule_InfoBeginDictField */
int RM_InfoEndDictField(RedisModuleInfoCtx *ctx) {
    if (!ctx->in_dict_field)
        return REDISMODULE_ERR;
    /* trim the last ',' if found. */
    if (ctx->info[sdslen(ctx->info)-1]==',')
        sdsIncrLen(ctx->info, -1);
    ctx->info = sdscat(ctx->info, "\r\n");
    ctx->in_dict_field = 0;
    return REDISMODULE_OK;
}

/* Used by RedisModuleInfoFunc to add info fields.
 * Each field will be automatically prefixed by `<modulename>_`.
 * Field names or values must not include `\r\n` or `:`. */
int RM_InfoAddFieldString(RedisModuleInfoCtx *ctx, const char *field, RedisModuleString *value) {
    if (!ctx->in_section)
        return REDISMODULE_ERR;
    if (ctx->in_dict_field) {
        ctx->info = sdscatfmt(ctx->info,
            "%s=%S,",
            field,
            (sds)value->ptr);
        return REDISMODULE_OK;
    }
    ctx->info = sdscatfmt(ctx->info,
        "%s_%s:%S\r\n",
        ctx->module->name,
        field,
        (sds)value->ptr);
    return REDISMODULE_OK;
}

/* See RedisModule_InfoAddFieldString(). */
int RM_InfoAddFieldCString(RedisModuleInfoCtx *ctx, const char *field, const char *value) {
    if (!ctx->in_section)
        return REDISMODULE_ERR;
    if (ctx->in_dict_field) {
        ctx->info = sdscatfmt(ctx->info,
            "%s=%s,",
            field,
            value);
        return REDISMODULE_OK;
    }
    ctx->info = sdscatfmt(ctx->info,
        "%s_%s:%s\r\n",
        ctx->module->name,
        field,
        value);
    return REDISMODULE_OK;
}

/* See RedisModule_InfoAddFieldString(). */
int RM_InfoAddFieldDouble(RedisModuleInfoCtx *ctx, const char *field, double value) {
    if (!ctx->in_section)
        return REDISMODULE_ERR;
    if (ctx->in_dict_field) {
        ctx->info = sdscatprintf(ctx->info,
            "%s=%.17g,",
            field,
            value);
        return REDISMODULE_OK;
    }
    ctx->info = sdscatprintf(ctx->info,
        "%s_%s:%.17g\r\n",
        ctx->module->name,
        field,
        value);
    return REDISMODULE_OK;
}

/* See RedisModule_InfoAddFieldString(). */
int RM_InfoAddFieldLongLong(RedisModuleInfoCtx *ctx, const char *field, long long value) {
    if (!ctx->in_section)
        return REDISMODULE_ERR;
    if (ctx->in_dict_field) {
        ctx->info = sdscatfmt(ctx->info,
            "%s=%I,",
            field,
            value);
        return REDISMODULE_OK;
    }
    ctx->info = sdscatfmt(ctx->info,
        "%s_%s:%I\r\n",
        ctx->module->name,
        field,
        value);
    return REDISMODULE_OK;
}

/* See RedisModule_InfoAddFieldString(). */
int RM_InfoAddFieldULongLong(RedisModuleInfoCtx *ctx, const char *field, unsigned long long value) {
    if (!ctx->in_section)
        return REDISMODULE_ERR;
    if (ctx->in_dict_field) {
        ctx->info = sdscatfmt(ctx->info,
            "%s=%U,",
            field,
            value);
        return REDISMODULE_OK;
    }
    ctx->info = sdscatfmt(ctx->info,
        "%s_%s:%U\r\n",
        ctx->module->name,
        field,
        value);
    return REDISMODULE_OK;
}

/* Registers callback for the INFO command. The callback should add INFO fields
 * by calling the `RedisModule_InfoAddField*()` functions. */
int RM_RegisterInfoFunc(RedisModuleCtx *ctx, RedisModuleInfoFunc cb) {
    ctx->module->info_cb = cb;
    return REDISMODULE_OK;
}

sds modulesCollectInfo(sds info, dict *sections_dict, int for_crash_report, int sections) {
    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;

    while ((de = dictNext(di)) != NULL) {
        struct RedisModule *module = dictGetVal(de);
        if (!module->info_cb)
            continue;
        RedisModuleInfoCtx info_ctx = {module, sections_dict, info, sections, 0, 0};
        module->info_cb(&info_ctx, for_crash_report);
        /* Implicitly end dicts (no way to handle errors, and we must add the newline). */
        if (info_ctx.in_dict_field)
            RM_InfoEndDictField(&info_ctx);
        info = info_ctx.info;
        sections = info_ctx.sections;
    }
    dictReleaseIterator(di);
    return info;
}

/* Get information about the server similar to the one that returns from the
 * INFO command. This function takes an optional 'section' argument that may
 * be NULL. The return value holds the output and can be used with
 * RedisModule_ServerInfoGetField and alike to get the individual fields.
 * When done, it needs to be freed with RedisModule_FreeServerInfo or with the
 * automatic memory management mechanism if enabled. */
RedisModuleServerInfoData *RM_GetServerInfo(RedisModuleCtx *ctx, const char *section) {
    struct RedisModuleServerInfoData *d = zmalloc(sizeof(*d));
    d->rax = raxNew();
    if (ctx != NULL) autoMemoryAdd(ctx,REDISMODULE_AM_INFO,d);
    int all = 0, everything = 0;
    robj *argv[1];
    argv[0] = section ? createStringObject(section, strlen(section)) : NULL;
    dict *section_dict = genInfoSectionDict(argv, section ? 1 : 0, NULL, &all, &everything);
    sds info = genRedisInfoString(section_dict, all, everything);
    int totlines, i;
    sds *lines = sdssplitlen(info, sdslen(info), "\r\n", 2, &totlines);
    for(i=0; i<totlines; i++) {
        sds line = lines[i];
        if (line[0]=='#') continue;
        char *sep = strchr(line, ':');
        if (!sep) continue;
        unsigned char *key = (unsigned char*)line;
        size_t keylen = (intptr_t)sep-(intptr_t)line;
        sds val = sdsnewlen(sep+1,sdslen(line)-((intptr_t)sep-(intptr_t)line)-1);
        if (!raxTryInsert(d->rax,key,keylen,val,NULL))
            sdsfree(val);
    }
    sdsfree(info);
    sdsfreesplitres(lines,totlines);
    releaseInfoSectionDict(section_dict);
    if(argv[0]) decrRefCount(argv[0]);
    return d;
}

/* Free data created with RM_GetServerInfo(). You need to pass the
 * context pointer 'ctx' only if the dictionary was created using the
 * context instead of passing NULL. */
void RM_FreeServerInfo(RedisModuleCtx *ctx, RedisModuleServerInfoData *data) {
    if (ctx != NULL) autoMemoryFreed(ctx,REDISMODULE_AM_INFO,data);
    raxFreeWithCallback(data->rax, (void(*)(void*))sdsfree);
    zfree(data);
}

/* Get the value of a field from data collected with RM_GetServerInfo(). You
 * need to pass the context pointer 'ctx' only if you want to use auto memory
 * mechanism to release the returned string. Return value will be NULL if the
 * field was not found. */
RedisModuleString *RM_ServerInfoGetField(RedisModuleCtx *ctx, RedisModuleServerInfoData *data, const char* field) {
    sds val = raxFind(data->rax, (unsigned char *)field, strlen(field));
    if (val == raxNotFound) return NULL;
    RedisModuleString *o = createStringObject(val,sdslen(val));
    if (ctx != NULL) autoMemoryAdd(ctx,REDISMODULE_AM_STRING,o);
    return o;
}

/* Similar to RM_ServerInfoGetField, but returns a char* which should not be freed but the caller. */
const char *RM_ServerInfoGetFieldC(RedisModuleServerInfoData *data, const char* field) {
    sds val = raxFind(data->rax, (unsigned char *)field, strlen(field));
    if (val == raxNotFound) return NULL;
    return val;
}

/* Get the value of a field from data collected with RM_GetServerInfo(). If the
 * field is not found, or is not numerical or out of range, return value will be
 * 0, and the optional out_err argument will be set to REDISMODULE_ERR. */
long long RM_ServerInfoGetFieldSigned(RedisModuleServerInfoData *data, const char* field, int *out_err) {
    long long ll;
    sds val = raxFind(data->rax, (unsigned char *)field, strlen(field));
    if (val == raxNotFound) {
        if (out_err) *out_err = REDISMODULE_ERR;
        return 0;
    }
    if (!string2ll(val,sdslen(val),&ll)) {
        if (out_err) *out_err = REDISMODULE_ERR;
        return 0;
    }
    if (out_err) *out_err = REDISMODULE_OK;
    return ll;
}

/* Get the value of a field from data collected with RM_GetServerInfo(). If the
 * field is not found, or is not numerical or out of range, return value will be
 * 0, and the optional out_err argument will be set to REDISMODULE_ERR. */
unsigned long long RM_ServerInfoGetFieldUnsigned(RedisModuleServerInfoData *data, const char* field, int *out_err) {
    unsigned long long ll;
    sds val = raxFind(data->rax, (unsigned char *)field, strlen(field));
    if (val == raxNotFound) {
        if (out_err) *out_err = REDISMODULE_ERR;
        return 0;
    }
    if (!string2ull(val,&ll)) {
        if (out_err) *out_err = REDISMODULE_ERR;
        return 0;
    }
    if (out_err) *out_err = REDISMODULE_OK;
    return ll;
}

/* Get the value of a field from data collected with RM_GetServerInfo(). If the
 * field is not found, or is not a double, return value will be 0, and the
 * optional out_err argument will be set to REDISMODULE_ERR. */
double RM_ServerInfoGetFieldDouble(RedisModuleServerInfoData *data, const char* field, int *out_err) {
    double dbl;
    sds val = raxFind(data->rax, (unsigned char *)field, strlen(field));
    if (val == raxNotFound) {
        if (out_err) *out_err = REDISMODULE_ERR;
        return 0;
    }
    if (!string2d(val,sdslen(val),&dbl)) {
        if (out_err) *out_err = REDISMODULE_ERR;
        return 0;
    }
    if (out_err) *out_err = REDISMODULE_OK;
    return dbl;
}

/* --------------------------------------------------------------------------
 * ## Modules utility APIs
 * -------------------------------------------------------------------------- */

/* Return random bytes using SHA1 in counter mode with a /dev/urandom
 * initialized seed. This function is fast so can be used to generate
 * many bytes without any effect on the operating system entropy pool.
 * Currently this function is not thread safe. */
void RM_GetRandomBytes(unsigned char *dst, size_t len) {
    getRandomBytes(dst,len);
}

/* Like RedisModule_GetRandomBytes() but instead of setting the string to
 * random bytes the string is set to random characters in the in the
 * hex charset [0-9a-f]. */
void RM_GetRandomHexChars(char *dst, size_t len) {
    getRandomHexChars(dst,len);
}

/* --------------------------------------------------------------------------
 * ## Modules API exporting / importing
 * -------------------------------------------------------------------------- */

/* This function is called by a module in order to export some API with a
 * given name. Other modules will be able to use this API by calling the
 * symmetrical function RM_GetSharedAPI() and casting the return value to
 * the right function pointer.
 *
 * The function will return REDISMODULE_OK if the name is not already taken,
 * otherwise REDISMODULE_ERR will be returned and no operation will be
 * performed.
 *
 * IMPORTANT: the apiname argument should be a string literal with static
 * lifetime. The API relies on the fact that it will always be valid in
 * the future. */
int RM_ExportSharedAPI(RedisModuleCtx *ctx, const char *apiname, void *func) {
    RedisModuleSharedAPI *sapi = zmalloc(sizeof(*sapi));
    sapi->module = ctx->module;
    sapi->func = func;
    if (dictAdd(server.sharedapi, (char*)apiname, sapi) != DICT_OK) {
        zfree(sapi);
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}

/* Request an exported API pointer. The return value is just a void pointer
 * that the caller of this function will be required to cast to the right
 * function pointer, so this is a private contract between modules.
 *
 * If the requested API is not available then NULL is returned. Because
 * modules can be loaded at different times with different order, this
 * function calls should be put inside some module generic API registering
 * step, that is called every time a module attempts to execute a
 * command that requires external APIs: if some API cannot be resolved, the
 * command should return an error.
 *
 * Here is an example:
 *
 *     int ... myCommandImplementation() {
 *        if (getExternalAPIs() == 0) {
 *             reply with an error here if we cannot have the APIs
 *        }
 *        // Use the API:
 *        myFunctionPointer(foo);
 *     }
 *
 * And the function registerAPI() is:
 *
 *     int getExternalAPIs(void) {
 *         static int api_loaded = 0;
 *         if (api_loaded != 0) return 1; // APIs already resolved.
 *
 *         myFunctionPointer = RedisModule_GetSharedAPI("...");
 *         if (myFunctionPointer == NULL) return 0;
 *
 *         return 1;
 *     }
 */
void *RM_GetSharedAPI(RedisModuleCtx *ctx, const char *apiname) {
    dictEntry *de = dictFind(server.sharedapi, apiname);
    if (de == NULL) return NULL;
    RedisModuleSharedAPI *sapi = dictGetVal(de);
    if (listSearchKey(sapi->module->usedby,ctx->module) == NULL) {
        listAddNodeTail(sapi->module->usedby,ctx->module);
        listAddNodeTail(ctx->module->using,sapi->module);
    }
    return sapi->func;
}

/* Remove all the APIs registered by the specified module. Usually you
 * want this when the module is going to be unloaded. This function
 * assumes that's caller responsibility to make sure the APIs are not
 * used by other modules.
 *
 * The number of unregistered APIs is returned. */
int moduleUnregisterSharedAPI(RedisModule *module) {
    int count = 0;
    dictIterator *di = dictGetSafeIterator(server.sharedapi);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        const char *apiname = dictGetKey(de);
        RedisModuleSharedAPI *sapi = dictGetVal(de);
        if (sapi->module == module) {
            dictDelete(server.sharedapi,apiname);
            zfree(sapi);
            count++;
        }
    }
    dictReleaseIterator(di);
    return count;
}

/* Remove the specified module as an user of APIs of ever other module.
 * This is usually called when a module is unloaded.
 *
 * Returns the number of modules this module was using APIs from. */
int moduleUnregisterUsedAPI(RedisModule *module) {
    listIter li;
    listNode *ln;
    int count = 0;

    listRewind(module->using,&li);
    while((ln = listNext(&li))) {
        RedisModule *used = ln->value;
        listNode *ln = listSearchKey(used->usedby,module);
        if (ln) {
            listDelNode(used->usedby,ln);
            count++;
        }
    }
    return count;
}

/* Unregister all filters registered by a module.
 * This is called when a module is being unloaded.
 *
 * Returns the number of filters unregistered. */
int moduleUnregisterFilters(RedisModule *module) {
    listIter li;
    listNode *ln;
    int count = 0;

    listRewind(module->filters,&li);
    while((ln = listNext(&li))) {
        RedisModuleCommandFilter *filter = ln->value;
        listNode *ln = listSearchKey(moduleCommandFilters,filter);
        if (ln) {
            listDelNode(moduleCommandFilters,ln);
            count++;
        }
        zfree(filter);
    }
    return count;
}

/* --------------------------------------------------------------------------
 * ## Module Command Filter API
 * -------------------------------------------------------------------------- */

/* Register a new command filter function.
 *
 * Command filtering makes it possible for modules to extend Redis by plugging
 * into the execution flow of all commands.
 *
 * A registered filter gets called before Redis executes *any* command.  This
 * includes both core Redis commands and commands registered by any module.  The
 * filter applies in all execution paths including:
 *
 * 1. Invocation by a client.
 * 2. Invocation through `RedisModule_Call()` by any module.
 * 3. Invocation through Lua `redis.call()`.
 * 4. Replication of a command from a master.
 *
 * The filter executes in a special filter context, which is different and more
 * limited than a RedisModuleCtx.  Because the filter affects any command, it
 * must be implemented in a very efficient way to reduce the performance impact
 * on Redis.  All Redis Module API calls that require a valid context (such as
 * `RedisModule_Call()`, `RedisModule_OpenKey()`, etc.) are not supported in a
 * filter context.
 *
 * The `RedisModuleCommandFilterCtx` can be used to inspect or modify the
 * executed command and its arguments.  As the filter executes before Redis
 * begins processing the command, any change will affect the way the command is
 * processed.  For example, a module can override Redis commands this way:
 *
 * 1. Register a `MODULE.SET` command which implements an extended version of
 *    the Redis `SET` command.
 * 2. Register a command filter which detects invocation of `SET` on a specific
 *    pattern of keys.  Once detected, the filter will replace the first
 *    argument from `SET` to `MODULE.SET`.
 * 3. When filter execution is complete, Redis considers the new command name
 *    and therefore executes the module's own command.
 *
 * Note that in the above use case, if `MODULE.SET` itself uses
 * `RedisModule_Call()` the filter will be applied on that call as well.  If
 * that is not desired, the `REDISMODULE_CMDFILTER_NOSELF` flag can be set when
 * registering the filter.
 *
 * The `REDISMODULE_CMDFILTER_NOSELF` flag prevents execution flows that
 * originate from the module's own `RM_Call()` from reaching the filter.  This
 * flag is effective for all execution flows, including nested ones, as long as
 * the execution begins from the module's command context or a thread-safe
 * context that is associated with a blocking command.
 *
 * Detached thread-safe contexts are *not* associated with the module and cannot
 * be protected by this flag.
 *
 * If multiple filters are registered (by the same or different modules), they
 * are executed in the order of registration.
 */
RedisModuleCommandFilter *RM_RegisterCommandFilter(RedisModuleCtx *ctx, RedisModuleCommandFilterFunc callback, int flags) {
    RedisModuleCommandFilter *filter = zmalloc(sizeof(*filter));
    filter->module = ctx->module;
    filter->callback = callback;
    filter->flags = flags;

    listAddNodeTail(moduleCommandFilters, filter);
    listAddNodeTail(ctx->module->filters, filter);
    return filter;
}

/* Unregister a command filter.
 */
int RM_UnregisterCommandFilter(RedisModuleCtx *ctx, RedisModuleCommandFilter *filter) {
    listNode *ln;

    /* A module can only remove its own filters */
    if (filter->module != ctx->module) return REDISMODULE_ERR;

    ln = listSearchKey(moduleCommandFilters,filter);
    if (!ln) return REDISMODULE_ERR;
    listDelNode(moduleCommandFilters,ln);

    ln = listSearchKey(ctx->module->filters,filter);
    if (!ln) return REDISMODULE_ERR;    /* Shouldn't happen */
    listDelNode(ctx->module->filters,ln);

    zfree(filter);

    return REDISMODULE_OK;
}

void moduleCallCommandFilters(client *c) {
    if (listLength(moduleCommandFilters) == 0) return;

    listIter li;
    listNode *ln;
    listRewind(moduleCommandFilters,&li);

    RedisModuleCommandFilterCtx filter = {
        .argv = c->argv,
        .argv_len = c->argv_len,
        .argc = c->argc
    };

    while((ln = listNext(&li))) {
        RedisModuleCommandFilter *f = ln->value;

        /* Skip filter if REDISMODULE_CMDFILTER_NOSELF is set and module is
         * currently processing a command.
         */
        if ((f->flags & REDISMODULE_CMDFILTER_NOSELF) && f->module->in_call) continue;

        /* Call filter */
        f->callback(&filter);
    }

    c->argv = filter.argv;
    c->argv_len = filter.argv_len;
    c->argc = filter.argc;
}

/* Return the number of arguments a filtered command has.  The number of
 * arguments include the command itself.
 */
int RM_CommandFilterArgsCount(RedisModuleCommandFilterCtx *fctx)
{
    return fctx->argc;
}

/* Return the specified command argument.  The first argument (position 0) is
 * the command itself, and the rest are user-provided args.
 */
RedisModuleString *RM_CommandFilterArgGet(RedisModuleCommandFilterCtx *fctx, int pos)
{
    if (pos < 0 || pos >= fctx->argc) return NULL;
    return fctx->argv[pos];
}

/* Modify the filtered command by inserting a new argument at the specified
 * position.  The specified RedisModuleString argument may be used by Redis
 * after the filter context is destroyed, so it must not be auto-memory
 * allocated, freed or used elsewhere.
 */
int RM_CommandFilterArgInsert(RedisModuleCommandFilterCtx *fctx, int pos, RedisModuleString *arg)
{
    int i;

    if (pos < 0 || pos > fctx->argc) return REDISMODULE_ERR;

    if (fctx->argv_len < fctx->argc+1) {
        fctx->argv_len = fctx->argc+1;
        fctx->argv = zrealloc(fctx->argv, fctx->argv_len*sizeof(RedisModuleString *));
    }
    for (i = fctx->argc; i > pos; i--) {
        fctx->argv[i] = fctx->argv[i-1];
    }
    fctx->argv[pos] = arg;
    fctx->argc++;

    return REDISMODULE_OK;
}

/* Modify the filtered command by replacing an existing argument with a new one.
 * The specified RedisModuleString argument may be used by Redis after the
 * filter context is destroyed, so it must not be auto-memory allocated, freed
 * or used elsewhere.
 */
int RM_CommandFilterArgReplace(RedisModuleCommandFilterCtx *fctx, int pos, RedisModuleString *arg)
{
    if (pos < 0 || pos >= fctx->argc) return REDISMODULE_ERR;

    decrRefCount(fctx->argv[pos]);
    fctx->argv[pos] = arg;

    return REDISMODULE_OK;
}

/* Modify the filtered command by deleting an argument at the specified
 * position.
 */
int RM_CommandFilterArgDelete(RedisModuleCommandFilterCtx *fctx, int pos)
{
    int i;
    if (pos < 0 || pos >= fctx->argc) return REDISMODULE_ERR;

    decrRefCount(fctx->argv[pos]);
    for (i = pos; i < fctx->argc-1; i++) {
        fctx->argv[i] = fctx->argv[i+1];
    }
    fctx->argc--;

    return REDISMODULE_OK;
}

/* For a given pointer allocated via RedisModule_Alloc() or
 * RedisModule_Realloc(), return the amount of memory allocated for it.
 * Note that this may be different (larger) than the memory we allocated
 * with the allocation calls, since sometimes the underlying allocator
 * will allocate more memory.
 */
size_t RM_MallocSize(void* ptr) {
    return zmalloc_size(ptr);
}

/* Similar to RM_MallocSize, the difference is that RM_MallocUsableSize
 * returns the usable size of memory by the module. */
size_t RM_MallocUsableSize(void *ptr) {
    return zmalloc_usable_size(ptr);
}

/* Same as RM_MallocSize, except it works on RedisModuleString pointers.
 */
size_t RM_MallocSizeString(RedisModuleString* str) {
    serverAssert(str->type == OBJ_STRING);
    return sizeof(*str) + getStringObjectSdsUsedMemory(str);
}

/* Same as RM_MallocSize, except it works on RedisModuleDict pointers.
 * Note that the returned value is only the overhead of the underlying structures,
 * it does not include the allocation size of the keys and values.
 */
size_t RM_MallocSizeDict(RedisModuleDict* dict) {
    size_t size = sizeof(RedisModuleDict) + sizeof(rax);
    size += dict->rax->numnodes * sizeof(raxNode);
    /* For more info about this weird line, see streamRadixTreeMemoryUsage */
    size += dict->rax->numnodes * sizeof(long)*30;
    return size;
}

/* Return the a number between 0 to 1 indicating the amount of memory
 * currently used, relative to the Redis "maxmemory" configuration.
 *
 * * 0 - No memory limit configured.
 * * Between 0 and 1 - The percentage of the memory used normalized in 0-1 range.
 * * Exactly 1 - Memory limit reached.
 * * Greater 1 - More memory used than the configured limit.
 */
float RM_GetUsedMemoryRatio(){
    float level;
    getMaxmemoryState(NULL, NULL, NULL, &level);
    return level;
}

/* --------------------------------------------------------------------------
 * ## Scanning keyspace and hashes
 * -------------------------------------------------------------------------- */

typedef void (*RedisModuleScanCB)(RedisModuleCtx *ctx, RedisModuleString *keyname, RedisModuleKey *key, void *privdata);
typedef struct {
    RedisModuleCtx *ctx;
    void* user_data;
    RedisModuleScanCB fn;
} ScanCBData;

typedef struct RedisModuleScanCursor{
    unsigned long cursor;
    int done;
}RedisModuleScanCursor;

static void moduleScanCallback(void *privdata, const dictEntry *de) {
    ScanCBData *data = privdata;
    sds key = dictGetKey(de);
    robj* val = dictGetVal(de);
    RedisModuleString *keyname = createObject(OBJ_STRING,sdsdup(key));

    /* Setup the key handle. */
    RedisModuleKey kp = {0};
    moduleInitKey(&kp, data->ctx, keyname, val, REDISMODULE_READ);

    data->fn(data->ctx, keyname, &kp, data->user_data);

    moduleCloseKey(&kp);
    decrRefCount(keyname);
}

/* Create a new cursor to be used with RedisModule_Scan */
RedisModuleScanCursor *RM_ScanCursorCreate() {
    RedisModuleScanCursor* cursor = zmalloc(sizeof(*cursor));
    cursor->cursor = 0;
    cursor->done = 0;
    return cursor;
}

/* Restart an existing cursor. The keys will be rescanned. */
void RM_ScanCursorRestart(RedisModuleScanCursor *cursor) {
    cursor->cursor = 0;
    cursor->done = 0;
}

/* Destroy the cursor struct. */
void RM_ScanCursorDestroy(RedisModuleScanCursor *cursor) {
    zfree(cursor);
}

/* Scan API that allows a module to scan all the keys and value in
 * the selected db.
 *
 * Callback for scan implementation.
 *
 *     void scan_callback(RedisModuleCtx *ctx, RedisModuleString *keyname,
 *                        RedisModuleKey *key, void *privdata);
 *
 * - `ctx`: the redis module context provided to for the scan.
 * - `keyname`: owned by the caller and need to be retained if used after this
 *   function.
 * - `key`: holds info on the key and value, it is provided as best effort, in
 *   some cases it might be NULL, in which case the user should (can) use
 *   RedisModule_OpenKey() (and CloseKey too).
 *   when it is provided, it is owned by the caller and will be free when the
 *   callback returns.
 * - `privdata`: the user data provided to RedisModule_Scan().
 *
 * The way it should be used:
 *
 *      RedisModuleScanCursor *c = RedisModule_ScanCursorCreate();
 *      while(RedisModule_Scan(ctx, c, callback, privateData));
 *      RedisModule_ScanCursorDestroy(c);
 *
 * It is also possible to use this API from another thread while the lock
 * is acquired during the actual call to RM_Scan:
 *
 *      RedisModuleScanCursor *c = RedisModule_ScanCursorCreate();
 *      RedisModule_ThreadSafeContextLock(ctx);
 *      while(RedisModule_Scan(ctx, c, callback, privateData)){
 *          RedisModule_ThreadSafeContextUnlock(ctx);
 *          // do some background job
 *          RedisModule_ThreadSafeContextLock(ctx);
 *      }
 *      RedisModule_ScanCursorDestroy(c);
 *
 * The function will return 1 if there are more elements to scan and
 * 0 otherwise, possibly setting errno if the call failed.
 *
 * It is also possible to restart an existing cursor using RM_ScanCursorRestart.
 *
 * IMPORTANT: This API is very similar to the Redis SCAN command from the
 * point of view of the guarantees it provides. This means that the API
 * may report duplicated keys, but guarantees to report at least one time
 * every key that was there from the start to the end of the scanning process.
 *
 * NOTE: If you do database changes within the callback, you should be aware
 * that the internal state of the database may change. For instance it is safe
 * to delete or modify the current key, but may not be safe to delete any
 * other key.
 * Moreover playing with the Redis keyspace while iterating may have the
 * effect of returning more duplicates. A safe pattern is to store the keys
 * names you want to modify elsewhere, and perform the actions on the keys
 * later when the iteration is complete. However this can cost a lot of
 * memory, so it may make sense to just operate on the current key when
 * possible during the iteration, given that this is safe. */
int RM_Scan(RedisModuleCtx *ctx, RedisModuleScanCursor *cursor, RedisModuleScanCB fn, void *privdata) {
    if (cursor->done) {
        errno = ENOENT;
        return 0;
    }
    int ret = 1;
    ScanCBData data = { ctx, privdata, fn };
    cursor->cursor = dictScan(ctx->client->db->dict, cursor->cursor, moduleScanCallback, &data);
    if (cursor->cursor == 0) {
        cursor->done = 1;
        ret = 0;
    }
    errno = 0;
    return ret;
}

typedef void (*RedisModuleScanKeyCB)(RedisModuleKey *key, RedisModuleString *field, RedisModuleString *value, void *privdata);
typedef struct {
    RedisModuleKey *key;
    void* user_data;
    RedisModuleScanKeyCB fn;
} ScanKeyCBData;

static void moduleScanKeyCallback(void *privdata, const dictEntry *de) {
    ScanKeyCBData *data = privdata;
    sds key = dictGetKey(de);
    robj *o = data->key->value;
    robj *field = createStringObject(key, sdslen(key));
    robj *value = NULL;
    if (o->type == OBJ_SET) {
        value = NULL;
    } else if (o->type == OBJ_HASH) {
        sds val = dictGetVal(de);
        value = createStringObject(val, sdslen(val));
    } else if (o->type == OBJ_ZSET) {
        double *val = (double*)dictGetVal(de);
        value = createStringObjectFromLongDouble(*val, 0);
    }

    data->fn(data->key, field, value, data->user_data);
    decrRefCount(field);
    if (value) decrRefCount(value);
}

/* Scan api that allows a module to scan the elements in a hash, set or sorted set key
 *
 * Callback for scan implementation.
 *
 *     void scan_callback(RedisModuleKey *key, RedisModuleString* field, RedisModuleString* value, void *privdata);
 *
 * - key - the redis key context provided to for the scan.
 * - field - field name, owned by the caller and need to be retained if used
 *   after this function.
 * - value - value string or NULL for set type, owned by the caller and need to
 *   be retained if used after this function.
 * - privdata - the user data provided to RedisModule_ScanKey.
 *
 * The way it should be used:
 *
 *      RedisModuleScanCursor *c = RedisModule_ScanCursorCreate();
 *      RedisModuleKey *key = RedisModule_OpenKey(...)
 *      while(RedisModule_ScanKey(key, c, callback, privateData));
 *      RedisModule_CloseKey(key);
 *      RedisModule_ScanCursorDestroy(c);
 *
 * It is also possible to use this API from another thread while the lock is acquired during
 * the actual call to RM_ScanKey, and re-opening the key each time:
 *
 *      RedisModuleScanCursor *c = RedisModule_ScanCursorCreate();
 *      RedisModule_ThreadSafeContextLock(ctx);
 *      RedisModuleKey *key = RedisModule_OpenKey(...)
 *      while(RedisModule_ScanKey(ctx, c, callback, privateData)){
 *          RedisModule_CloseKey(key);
 *          RedisModule_ThreadSafeContextUnlock(ctx);
 *          // do some background job
 *          RedisModule_ThreadSafeContextLock(ctx);
 *          RedisModuleKey *key = RedisModule_OpenKey(...)
 *      }
 *      RedisModule_CloseKey(key);
 *      RedisModule_ScanCursorDestroy(c);
 *
 * The function will return 1 if there are more elements to scan and 0 otherwise,
 * possibly setting errno if the call failed.
 * It is also possible to restart an existing cursor using RM_ScanCursorRestart.
 *
 * NOTE: Certain operations are unsafe while iterating the object. For instance
 * while the API guarantees to return at least one time all the elements that
 * are present in the data structure consistently from the start to the end
 * of the iteration (see HSCAN and similar commands documentation), the more
 * you play with the elements, the more duplicates you may get. In general
 * deleting the current element of the data structure is safe, while removing
 * the key you are iterating is not safe. */
int RM_ScanKey(RedisModuleKey *key, RedisModuleScanCursor *cursor, RedisModuleScanKeyCB fn, void *privdata) {
    if (key == NULL || key->value == NULL) {
        errno = EINVAL;
        return 0;
    }
    dict *ht = NULL;
    robj *o = key->value;
    if (o->type == OBJ_SET) {
        if (o->encoding == OBJ_ENCODING_HT)
            ht = o->ptr;
    } else if (o->type == OBJ_HASH) {
        if (o->encoding == OBJ_ENCODING_HT)
            ht = o->ptr;
    } else if (o->type == OBJ_ZSET) {
        if (o->encoding == OBJ_ENCODING_SKIPLIST)
            ht = ((zset *)o->ptr)->dict;
    } else {
        errno = EINVAL;
        return 0;
    }
    if (cursor->done) {
        errno = ENOENT;
        return 0;
    }
    int ret = 1;
    if (ht) {
        ScanKeyCBData data = { key, privdata, fn };
        cursor->cursor = dictScan(ht, cursor->cursor, moduleScanKeyCallback, &data);
        if (cursor->cursor == 0) {
            cursor->done = 1;
            ret = 0;
        }
    } else if (o->type == OBJ_SET) {
        setTypeIterator *si = setTypeInitIterator(o);
        sds sdsele;
        while ((sdsele = setTypeNextObject(si)) != NULL) {
            robj *field = createObject(OBJ_STRING, sdsele);
            fn(key, field, NULL, privdata);
            decrRefCount(field);
        }
        setTypeReleaseIterator(si);
        cursor->cursor = 1;
        cursor->done = 1;
        ret = 0;
    } else if (o->type == OBJ_ZSET || o->type == OBJ_HASH) {
        unsigned char *p = lpSeek(o->ptr,0);
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;
        while(p) {
            vstr = lpGetValue(p,&vlen,&vll);
            robj *field = (vstr != NULL) ?
                createStringObject((char*)vstr,vlen) :
                createObject(OBJ_STRING,sdsfromlonglong(vll));
            p = lpNext(o->ptr,p);
            vstr = lpGetValue(p,&vlen,&vll);
            robj *value = (vstr != NULL) ?
                createStringObject((char*)vstr,vlen) :
                createObject(OBJ_STRING,sdsfromlonglong(vll));
            fn(key, field, value, privdata);
            p = lpNext(o->ptr,p);
            decrRefCount(field);
            decrRefCount(value);
        }
        cursor->cursor = 1;
        cursor->done = 1;
        ret = 0;
    }
    errno = 0;
    return ret;
}


/* --------------------------------------------------------------------------
 * ## Module fork API
 * -------------------------------------------------------------------------- */

/* Create a background child process with the current frozen snapshot of the
 * main process where you can do some processing in the background without
 * affecting / freezing the traffic and no need for threads and GIL locking.
 * Note that Redis allows for only one concurrent fork.
 * When the child wants to exit, it should call RedisModule_ExitFromChild.
 * If the parent wants to kill the child it should call RedisModule_KillForkChild
 * The done handler callback will be executed on the parent process when the
 * child existed (but not when killed)
 * Return: -1 on failure, on success the parent process will get a positive PID
 * of the child, and the child process will get 0.
 */
int RM_Fork(RedisModuleForkDoneHandler cb, void *user_data) {
    pid_t childpid;

    if ((childpid = redisFork(CHILD_TYPE_MODULE)) == 0) {
        /* Child */
        redisSetProcTitle("redis-module-fork");
    } else if (childpid == -1) {
        serverLog(LL_WARNING,"Can't fork for module: %s", strerror(errno));
    } else {
        /* Parent */
        moduleForkInfo.done_handler = cb;
        moduleForkInfo.done_handler_user_data = user_data;
        serverLog(LL_VERBOSE, "Module fork started pid: %ld ", (long) childpid);
    }
    return childpid;
}

/* The module is advised to call this function from the fork child once in a while,
 * so that it can report progress and COW memory to the parent which will be
 * reported in INFO.
 * The `progress` argument should between 0 and 1, or -1 when not available. */
void RM_SendChildHeartbeat(double progress) {
    sendChildInfoGeneric(CHILD_INFO_TYPE_CURRENT_INFO, 0, progress, "Module fork");
}

/* Call from the child process when you want to terminate it.
 * retcode will be provided to the done handler executed on the parent process.
 */
int RM_ExitFromChild(int retcode) {
    sendChildCowInfo(CHILD_INFO_TYPE_MODULE_COW_SIZE, "Module fork");
    exitFromChild(retcode);
    return REDISMODULE_OK;
}

/* Kill the active module forked child, if there is one active and the
 * pid matches, and returns C_OK. Otherwise if there is no active module
 * child or the pid does not match, return C_ERR without doing anything. */
int TerminateModuleForkChild(int child_pid, int wait) {
    /* Module child should be active and pid should match. */
    if (server.child_type != CHILD_TYPE_MODULE ||
        server.child_pid != child_pid) return C_ERR;

    int statloc;
    serverLog(LL_VERBOSE,"Killing running module fork child: %ld",
        (long) server.child_pid);
    if (kill(server.child_pid,SIGUSR1) != -1 && wait) {
        while(waitpid(server.child_pid, &statloc, 0) !=
              server.child_pid);
    }
    /* Reset the buffer accumulating changes while the child saves. */
    resetChildState();
    moduleForkInfo.done_handler = NULL;
    moduleForkInfo.done_handler_user_data = NULL;
    return C_OK;
}

/* Can be used to kill the forked child process from the parent process.
 * child_pid would be the return value of RedisModule_Fork. */
int RM_KillForkChild(int child_pid) {
    /* Kill module child, wait for child exit. */
    if (TerminateModuleForkChild(child_pid,1) == C_OK)
        return REDISMODULE_OK;
    else
        return REDISMODULE_ERR;
}

void ModuleForkDoneHandler(int exitcode, int bysignal) {
    serverLog(LL_NOTICE,
        "Module fork exited pid: %ld, retcode: %d, bysignal: %d",
        (long) server.child_pid, exitcode, bysignal);
    if (moduleForkInfo.done_handler) {
        moduleForkInfo.done_handler(exitcode, bysignal,
            moduleForkInfo.done_handler_user_data);
    }

    moduleForkInfo.done_handler = NULL;
    moduleForkInfo.done_handler_user_data = NULL;
}

/* --------------------------------------------------------------------------
 * ## Server hooks implementation
 * -------------------------------------------------------------------------- */

/* This must be synced with REDISMODULE_EVENT_*
 * We use -1 (MAX_UINT64) to denote that this event doesn't have
 * a data structure associated with it. We use MAX_UINT64 on purpose,
 * in order to pass the check in RedisModule_SubscribeToServerEvent. */
static uint64_t moduleEventVersions[] = {
    REDISMODULE_REPLICATIONINFO_VERSION, /* REDISMODULE_EVENT_REPLICATION_ROLE_CHANGED */
    -1, /* REDISMODULE_EVENT_PERSISTENCE */
    REDISMODULE_FLUSHINFO_VERSION, /* REDISMODULE_EVENT_FLUSHDB */
    -1, /* REDISMODULE_EVENT_LOADING */
    REDISMODULE_CLIENTINFO_VERSION, /* REDISMODULE_EVENT_CLIENT_CHANGE */
    -1, /* REDISMODULE_EVENT_SHUTDOWN */
    -1, /* REDISMODULE_EVENT_REPLICA_CHANGE */
    -1, /* REDISMODULE_EVENT_MASTER_LINK_CHANGE */
    REDISMODULE_CRON_LOOP_VERSION, /* REDISMODULE_EVENT_CRON_LOOP */
    REDISMODULE_MODULE_CHANGE_VERSION, /* REDISMODULE_EVENT_MODULE_CHANGE */
    REDISMODULE_LOADING_PROGRESS_VERSION, /* REDISMODULE_EVENT_LOADING_PROGRESS */
    REDISMODULE_SWAPDBINFO_VERSION, /* REDISMODULE_EVENT_SWAPDB */
    -1, /* REDISMODULE_EVENT_REPL_BACKUP */
    -1, /* REDISMODULE_EVENT_FORK_CHILD */
    -1, /* REDISMODULE_EVENT_REPL_ASYNC_LOAD */
    -1, /* REDISMODULE_EVENT_EVENTLOOP */
    -1, /* REDISMODULE_EVENT_CONFIG */
    REDISMODULE_KEYINFO_VERSION, /* REDISMODULE_EVENT_KEY */
};

/* Register to be notified, via a callback, when the specified server event
 * happens. The callback is called with the event as argument, and an additional
 * argument which is a void pointer and should be cased to a specific type
 * that is event-specific (but many events will just use NULL since they do not
 * have additional information to pass to the callback).
 *
 * If the callback is NULL and there was a previous subscription, the module
 * will be unsubscribed. If there was a previous subscription and the callback
 * is not null, the old callback will be replaced with the new one.
 *
 * The callback must be of this type:
 *
 *     int (*RedisModuleEventCallback)(RedisModuleCtx *ctx,
 *                                     RedisModuleEvent eid,
 *                                     uint64_t subevent,
 *                                     void *data);
 *
 * The 'ctx' is a normal Redis module context that the callback can use in
 * order to call other modules APIs. The 'eid' is the event itself, this
 * is only useful in the case the module subscribed to multiple events: using
 * the 'id' field of this structure it is possible to check if the event
 * is one of the events we registered with this callback. The 'subevent' field
 * depends on the event that fired.
 *
 * Finally the 'data' pointer may be populated, only for certain events, with
 * more relevant data.
 *
 * Here is a list of events you can use as 'eid' and related sub events:
 *
 * * RedisModuleEvent_ReplicationRoleChanged:
 *
 *     This event is called when the instance switches from master
 *     to replica or the other way around, however the event is
 *     also called when the replica remains a replica but starts to
 *     replicate with a different master.
 *
 *     The following sub events are available:
 *
 *     * `REDISMODULE_SUBEVENT_REPLROLECHANGED_NOW_MASTER`
 *     * `REDISMODULE_SUBEVENT_REPLROLECHANGED_NOW_REPLICA`
 *
 *     The 'data' field can be casted by the callback to a
 *     `RedisModuleReplicationInfo` structure with the following fields:
 *
 *         int master; // true if master, false if replica
 *         char *masterhost; // master instance hostname for NOW_REPLICA
 *         int masterport; // master instance port for NOW_REPLICA
 *         char *replid1; // Main replication ID
 *         char *replid2; // Secondary replication ID
 *         uint64_t repl1_offset; // Main replication offset
 *         uint64_t repl2_offset; // Offset of replid2 validity
 *
 * * RedisModuleEvent_Persistence
 *
 *     This event is called when RDB saving or AOF rewriting starts
 *     and ends. The following sub events are available:
 *
 *     * `REDISMODULE_SUBEVENT_PERSISTENCE_RDB_START`
 *     * `REDISMODULE_SUBEVENT_PERSISTENCE_AOF_START`
 *     * `REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_RDB_START`
 *     * `REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_AOF_START`
 *     * `REDISMODULE_SUBEVENT_PERSISTENCE_ENDED`
 *     * `REDISMODULE_SUBEVENT_PERSISTENCE_FAILED`
 *
 *     The above events are triggered not just when the user calls the
 *     relevant commands like BGSAVE, but also when a saving operation
 *     or AOF rewriting occurs because of internal server triggers.
 *     The SYNC_RDB_START sub events are happening in the foreground due to
 *     SAVE command, FLUSHALL, or server shutdown, and the other RDB and
 *     AOF sub events are executed in a background fork child, so any
 *     action the module takes can only affect the generated AOF or RDB,
 *     but will not be reflected in the parent process and affect connected
 *     clients and commands. Also note that the AOF_START sub event may end
 *     up saving RDB content in case of an AOF with rdb-preamble.
 *
 * * RedisModuleEvent_FlushDB
 *
 *     The FLUSHALL, FLUSHDB or an internal flush (for instance
 *     because of replication, after the replica synchronization)
 *     happened. The following sub events are available:
 *
 *     * `REDISMODULE_SUBEVENT_FLUSHDB_START`
 *     * `REDISMODULE_SUBEVENT_FLUSHDB_END`
 *
 *     The data pointer can be casted to a RedisModuleFlushInfo
 *     structure with the following fields:
 *
 *         int32_t async;  // True if the flush is done in a thread.
 *                         // See for instance FLUSHALL ASYNC.
 *                         // In this case the END callback is invoked
 *                         // immediately after the database is put
 *                         // in the free list of the thread.
 *         int32_t dbnum;  // Flushed database number, -1 for all the DBs
 *                         // in the case of the FLUSHALL operation.
 *
 *     The start event is called *before* the operation is initiated, thus
 *     allowing the callback to call DBSIZE or other operation on the
 *     yet-to-free keyspace.
 *
 * * RedisModuleEvent_Loading
 *
 *     Called on loading operations: at startup when the server is
 *     started, but also after a first synchronization when the
 *     replica is loading the RDB file from the master.
 *     The following sub events are available:
 *
 *     * `REDISMODULE_SUBEVENT_LOADING_RDB_START`
 *     * `REDISMODULE_SUBEVENT_LOADING_AOF_START`
 *     * `REDISMODULE_SUBEVENT_LOADING_REPL_START`
 *     * `REDISMODULE_SUBEVENT_LOADING_ENDED`
 *     * `REDISMODULE_SUBEVENT_LOADING_FAILED`
 *
 *     Note that AOF loading may start with an RDB data in case of
 *     rdb-preamble, in which case you'll only receive an AOF_START event.
 *
 * * RedisModuleEvent_ClientChange
 *
 *     Called when a client connects or disconnects.
 *     The data pointer can be casted to a RedisModuleClientInfo
 *     structure, documented in RedisModule_GetClientInfoById().
 *     The following sub events are available:
 *
 *     * `REDISMODULE_SUBEVENT_CLIENT_CHANGE_CONNECTED`
 *     * `REDISMODULE_SUBEVENT_CLIENT_CHANGE_DISCONNECTED`
 *
 * * RedisModuleEvent_Shutdown
 *
 *     The server is shutting down. No subevents are available.
 *
 * * RedisModuleEvent_ReplicaChange
 *
 *     This event is called when the instance (that can be both a
 *     master or a replica) get a new online replica, or lose a
 *     replica since it gets disconnected.
 *     The following sub events are available:
 *
 *     * `REDISMODULE_SUBEVENT_REPLICA_CHANGE_ONLINE`
 *     * `REDISMODULE_SUBEVENT_REPLICA_CHANGE_OFFLINE`
 *
 *     No additional information is available so far: future versions
 *     of Redis will have an API in order to enumerate the replicas
 *     connected and their state.
 *
 * * RedisModuleEvent_CronLoop
 *
 *     This event is called every time Redis calls the serverCron()
 *     function in order to do certain bookkeeping. Modules that are
 *     required to do operations from time to time may use this callback.
 *     Normally Redis calls this function 10 times per second, but
 *     this changes depending on the "hz" configuration.
 *     No sub events are available.
 *
 *     The data pointer can be casted to a RedisModuleCronLoop
 *     structure with the following fields:
 *
 *         int32_t hz;  // Approximate number of events per second.
 *
 * * RedisModuleEvent_MasterLinkChange
 *
 *     This is called for replicas in order to notify when the
 *     replication link becomes functional (up) with our master,
 *     or when it goes down. Note that the link is not considered
 *     up when we just connected to the master, but only if the
 *     replication is happening correctly.
 *     The following sub events are available:
 *
 *     * `REDISMODULE_SUBEVENT_MASTER_LINK_UP`
 *     * `REDISMODULE_SUBEVENT_MASTER_LINK_DOWN`
 *
 * * RedisModuleEvent_ModuleChange
 *
 *     This event is called when a new module is loaded or one is unloaded.
 *     The following sub events are available:
 *
 *     * `REDISMODULE_SUBEVENT_MODULE_LOADED`
 *     * `REDISMODULE_SUBEVENT_MODULE_UNLOADED`
 *
 *     The data pointer can be casted to a RedisModuleModuleChange
 *     structure with the following fields:
 *
 *         const char* module_name;  // Name of module loaded or unloaded.
 *         int32_t module_version;  // Module version.
 *
 * * RedisModuleEvent_LoadingProgress
 *
 *     This event is called repeatedly called while an RDB or AOF file
 *     is being loaded.
 *     The following sub events are available:
 *
 *     * `REDISMODULE_SUBEVENT_LOADING_PROGRESS_RDB`
 *     * `REDISMODULE_SUBEVENT_LOADING_PROGRESS_AOF`
 *
 *     The data pointer can be casted to a RedisModuleLoadingProgress
 *     structure with the following fields:
 *
 *         int32_t hz;  // Approximate number of events per second.
 *         int32_t progress;  // Approximate progress between 0 and 1024,
 *                            // or -1 if unknown.
 *
 * * RedisModuleEvent_SwapDB
 *
 *     This event is called when a SWAPDB command has been successfully
 *     Executed.
 *     For this event call currently there is no subevents available.
 *
 *     The data pointer can be casted to a RedisModuleSwapDbInfo
 *     structure with the following fields:
 *
 *         int32_t dbnum_first;    // Swap Db first dbnum
 *         int32_t dbnum_second;   // Swap Db second dbnum
 *
 * * RedisModuleEvent_ReplBackup
 * 
 *     WARNING: Replication Backup events are deprecated since Redis 7.0 and are never fired.
 *     See RedisModuleEvent_ReplAsyncLoad for understanding how Async Replication Loading events
 *     are now triggered when repl-diskless-load is set to swapdb.
 *
 *     Called when repl-diskless-load config is set to swapdb,
 *     And redis needs to backup the current database for the
 *     possibility to be restored later. A module with global data and
 *     maybe with aux_load and aux_save callbacks may need to use this
 *     notification to backup / restore / discard its globals.
 *     The following sub events are available:
 *
 *     * `REDISMODULE_SUBEVENT_REPL_BACKUP_CREATE`
 *     * `REDISMODULE_SUBEVENT_REPL_BACKUP_RESTORE`
 *     * `REDISMODULE_SUBEVENT_REPL_BACKUP_DISCARD`
 * 
 * * RedisModuleEvent_ReplAsyncLoad
 *
 *     Called when repl-diskless-load config is set to swapdb and a replication with a master of same
 *     data set history (matching replication ID) occurs.
 *     In which case redis serves current data set while loading new database in memory from socket.
 *     Modules must have declared they support this mechanism in order to activate it, through
 *     REDISMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD flag.
 *     The following sub events are available:
 *
 *     * `REDISMODULE_SUBEVENT_REPL_ASYNC_LOAD_STARTED`
 *     * `REDISMODULE_SUBEVENT_REPL_ASYNC_LOAD_ABORTED`
 *     * `REDISMODULE_SUBEVENT_REPL_ASYNC_LOAD_COMPLETED`
 *
 * * RedisModuleEvent_ForkChild
 *
 *     Called when a fork child (AOFRW, RDBSAVE, module fork...) is born/dies
 *     The following sub events are available:
 *
 *     * `REDISMODULE_SUBEVENT_FORK_CHILD_BORN`
 *     * `REDISMODULE_SUBEVENT_FORK_CHILD_DIED`
 *
 * * RedisModuleEvent_EventLoop
 *
 *     Called on each event loop iteration, once just before the event loop goes
 *     to sleep or just after it wakes up.
 *     The following sub events are available:
 *
 *     * `REDISMODULE_SUBEVENT_EVENTLOOP_BEFORE_SLEEP`
 *     * `REDISMODULE_SUBEVENT_EVENTLOOP_AFTER_SLEEP`
 *
 * * RedisModule_Event_Config
 *
 *     Called when a configuration event happens
 *     The following sub events are available:
 *
 *     * `REDISMODULE_SUBEVENT_CONFIG_CHANGE`
 *
 *     The data pointer can be casted to a RedisModuleConfigChange
 *     structure with the following fields:
 *
 *         const char **config_names; // An array of C string pointers containing the
 *                                    // name of each modified configuration item 
 *         uint32_t num_changes;      // The number of elements in the config_names array
 *
 * * RedisModule_Event_Key
 *
 *     Called when a key is removed from the keyspace. We can't modify any key in
 *     the event.
 *     The following sub events are available:
 *
 *     * `REDISMODULE_SUBEVENT_KEY_DELETED`
 *     * `REDISMODULE_SUBEVENT_KEY_EXPIRED`
 *     * `REDISMODULE_SUBEVENT_KEY_EVICTED`
 *     * `REDISMODULE_SUBEVENT_KEY_OVERWRITTEN`
 *
 *     The data pointer can be casted to a RedisModuleKeyInfo
 *     structure with the following fields:
 *
 *         RedisModuleKey *key;    // Key name
 *
 * The function returns REDISMODULE_OK if the module was successfully subscribed
 * for the specified event. If the API is called from a wrong context or unsupported event
 * is given then REDISMODULE_ERR is returned. */
int RM_SubscribeToServerEvent(RedisModuleCtx *ctx, RedisModuleEvent event, RedisModuleEventCallback callback) {
    RedisModuleEventListener *el;

    /* Protect in case of calls from contexts without a module reference. */
    if (ctx->module == NULL) return REDISMODULE_ERR;
    if (event.id >= _REDISMODULE_EVENT_NEXT) return REDISMODULE_ERR;
    if (event.dataver > moduleEventVersions[event.id]) return REDISMODULE_ERR; /* Module compiled with a newer redismodule.h than we support */

    /* Search an event matching this module and event ID. */
    listIter li;
    listNode *ln;
    listRewind(RedisModule_EventListeners,&li);
    while((ln = listNext(&li))) {
        el = ln->value;
        if (el->module == ctx->module && el->event.id == event.id)
            break; /* Matching event found. */
    }

    /* Modify or remove the event listener if we already had one. */
    if (ln) {
        if (callback == NULL) {
            listDelNode(RedisModule_EventListeners,ln);
            zfree(el);
        } else {
            el->callback = callback; /* Update the callback with the new one. */
        }
        return REDISMODULE_OK;
    }

    /* No event found, we need to add a new one. */
    el = zmalloc(sizeof(*el));
    el->module = ctx->module;
    el->event = event;
    el->callback = callback;
    listAddNodeTail(RedisModule_EventListeners,el);
    return REDISMODULE_OK;
}

/**
 * For a given server event and subevent, return zero if the
 * subevent is not supported and non-zero otherwise.
 */
int RM_IsSubEventSupported(RedisModuleEvent event, int64_t subevent) {
    switch (event.id) {
    case REDISMODULE_EVENT_REPLICATION_ROLE_CHANGED:
        return subevent < _REDISMODULE_EVENT_REPLROLECHANGED_NEXT;
    case REDISMODULE_EVENT_PERSISTENCE:
        return subevent < _REDISMODULE_SUBEVENT_PERSISTENCE_NEXT;
    case REDISMODULE_EVENT_FLUSHDB:
        return subevent < _REDISMODULE_SUBEVENT_FLUSHDB_NEXT;
    case REDISMODULE_EVENT_LOADING:
        return subevent < _REDISMODULE_SUBEVENT_LOADING_NEXT;
    case REDISMODULE_EVENT_CLIENT_CHANGE:
        return subevent < _REDISMODULE_SUBEVENT_CLIENT_CHANGE_NEXT;
    case REDISMODULE_EVENT_SHUTDOWN:
        return subevent < _REDISMODULE_SUBEVENT_SHUTDOWN_NEXT;
    case REDISMODULE_EVENT_REPLICA_CHANGE:
        return subevent < _REDISMODULE_EVENT_REPLROLECHANGED_NEXT;
    case REDISMODULE_EVENT_MASTER_LINK_CHANGE:
        return subevent < _REDISMODULE_SUBEVENT_MASTER_NEXT;
    case REDISMODULE_EVENT_CRON_LOOP:
        return subevent < _REDISMODULE_SUBEVENT_CRON_LOOP_NEXT;
    case REDISMODULE_EVENT_MODULE_CHANGE:
        return subevent < _REDISMODULE_SUBEVENT_MODULE_NEXT;
    case REDISMODULE_EVENT_LOADING_PROGRESS:
        return subevent < _REDISMODULE_SUBEVENT_LOADING_PROGRESS_NEXT;
    case REDISMODULE_EVENT_SWAPDB:
        return subevent < _REDISMODULE_SUBEVENT_SWAPDB_NEXT;
    case REDISMODULE_EVENT_REPL_ASYNC_LOAD:
        return subevent < _REDISMODULE_SUBEVENT_REPL_ASYNC_LOAD_NEXT;
    case REDISMODULE_EVENT_FORK_CHILD:
        return subevent < _REDISMODULE_SUBEVENT_FORK_CHILD_NEXT;
    case REDISMODULE_EVENT_EVENTLOOP:
        return subevent < _REDISMODULE_SUBEVENT_EVENTLOOP_NEXT;
    case REDISMODULE_EVENT_CONFIG:
        return subevent < _REDISMODULE_SUBEVENT_CONFIG_NEXT; 
    case REDISMODULE_EVENT_KEY:
        return subevent < _REDISMODULE_SUBEVENT_KEY_NEXT;
    default:
        break;
    }
    return 0;
}

typedef struct KeyInfo {
    int32_t dbnum;
    RedisModuleString *key;
    robj *value;
    int mode;
} KeyInfo;

/* This is called by the Redis internals every time we want to fire an
 * event that can be intercepted by some module. The pointer 'data' is useful
 * in order to populate the event-specific structure when needed, in order
 * to return the structure with more information to the callback.
 *
 * 'eid' and 'subid' are just the main event ID and the sub event associated
 * with the event, depending on what exactly happened. */
void moduleFireServerEvent(uint64_t eid, int subid, void *data) {
    /* Fast path to return ASAP if there is nothing to do, avoiding to
     * setup the iterator and so forth: we want this call to be extremely
     * cheap if there are no registered modules. */
    if (listLength(RedisModule_EventListeners) == 0) return;

    listIter li;
    listNode *ln;
    listRewind(RedisModule_EventListeners,&li);
    while((ln = listNext(&li))) {
        RedisModuleEventListener *el = ln->value;
        if (el->event.id == eid) {
            RedisModuleCtx ctx;
            if (eid == REDISMODULE_EVENT_CLIENT_CHANGE) {
                /* In the case of client changes, we're pushing the real client
                 * so the event handler can mutate it if needed. For example,
                 * to change its authentication state in a way that does not
                 * depend on specific commands executed later.
                 */
                moduleCreateContext(&ctx,el->module,REDISMODULE_CTX_NONE);
                ctx.client = (client *) data;
            } else {
                moduleCreateContext(&ctx,el->module,REDISMODULE_CTX_TEMP_CLIENT);
            }

            void *moduledata = NULL;
            RedisModuleClientInfoV1 civ1;
            RedisModuleReplicationInfoV1 riv1;
            RedisModuleModuleChangeV1 mcv1;
            RedisModuleKey key;
            RedisModuleKeyInfoV1 ki = {REDISMODULE_KEYINFO_VERSION, &key};

            /* Event specific context and data pointer setup. */
            if (eid == REDISMODULE_EVENT_CLIENT_CHANGE) {
                serverAssert(modulePopulateClientInfoStructure(&civ1,data, el->event.dataver) == REDISMODULE_OK);
                moduledata = &civ1;
            } else if (eid == REDISMODULE_EVENT_REPLICATION_ROLE_CHANGED) {
                serverAssert(modulePopulateReplicationInfoStructure(&riv1,el->event.dataver) == REDISMODULE_OK);
                moduledata = &riv1;
            } else if (eid == REDISMODULE_EVENT_FLUSHDB) {
                moduledata = data;
                RedisModuleFlushInfoV1 *fi = data;
                if (fi->dbnum != -1)
                    selectDb(ctx.client, fi->dbnum);
            } else if (eid == REDISMODULE_EVENT_MODULE_CHANGE) {
                RedisModule *m = data;
                if (m == el->module) {
                    moduleFreeContext(&ctx);
                    continue;
                }
                mcv1.version = REDISMODULE_MODULE_CHANGE_VERSION;
                mcv1.module_name = m->name;
                mcv1.module_version = m->ver;
                moduledata = &mcv1;
            } else if (eid == REDISMODULE_EVENT_LOADING_PROGRESS) {
                moduledata = data;
            } else if (eid == REDISMODULE_EVENT_CRON_LOOP) {
                moduledata = data;
            } else if (eid == REDISMODULE_EVENT_SWAPDB) {
                moduledata = data;
            } else if (eid == REDISMODULE_EVENT_CONFIG) {
                moduledata = data;
            } else if (eid == REDISMODULE_EVENT_KEY) {
                KeyInfo *info = data;
                selectDb(ctx.client, info->dbnum);
                moduleInitKey(&key, &ctx, info->key, info->value, info->mode);
                moduledata = &ki;
            }

            el->module->in_hook++;
            el->callback(&ctx,el->event,subid,moduledata);
            el->module->in_hook--;

            if (eid == REDISMODULE_EVENT_KEY) {
                moduleCloseKey(&key);
            }

            moduleFreeContext(&ctx);
        }
    }
}

/* Remove all the listeners for this module: this is used before unloading
 * a module. */
void moduleUnsubscribeAllServerEvents(RedisModule *module) {
    RedisModuleEventListener *el;
    listIter li;
    listNode *ln;
    listRewind(RedisModule_EventListeners,&li);

    while((ln = listNext(&li))) {
        el = ln->value;
        if (el->module == module) {
            listDelNode(RedisModule_EventListeners,ln);
            zfree(el);
        }
    }
}

void processModuleLoadingProgressEvent(int is_aof) {
    long long now = server.ustime;
    static long long next_event = 0;
    if (now >= next_event) {
        /* Fire the loading progress modules end event. */
        int progress = -1;
        if (server.loading_total_bytes)
            progress = (server.loading_loaded_bytes<<10) / server.loading_total_bytes;
        RedisModuleLoadingProgressV1 fi = {REDISMODULE_LOADING_PROGRESS_VERSION,
                                     server.hz,
                                     progress};
        moduleFireServerEvent(REDISMODULE_EVENT_LOADING_PROGRESS,
                              is_aof?
                                REDISMODULE_SUBEVENT_LOADING_PROGRESS_AOF:
                                REDISMODULE_SUBEVENT_LOADING_PROGRESS_RDB,
                              &fi);
        /* decide when the next event should fire. */
        next_event = now + 1000000 / server.hz;
    }
}

/* When a key is deleted (in dbAsyncDelete/dbSyncDelete/setKey), it
*  will be called to tell the module which key is about to be released. */
void moduleNotifyKeyUnlink(robj *key, robj *val, int dbid, int flags) {
    server.lazy_expire_disabled++;
    int subevent = REDISMODULE_SUBEVENT_KEY_DELETED;
    if (flags & DB_FLAG_KEY_EXPIRED) {
        subevent = REDISMODULE_SUBEVENT_KEY_EXPIRED;
    } else if (flags & DB_FLAG_KEY_EVICTED) {
        subevent = REDISMODULE_SUBEVENT_KEY_EVICTED;
    } else if (flags & DB_FLAG_KEY_OVERWRITE) {
        subevent = REDISMODULE_SUBEVENT_KEY_OVERWRITTEN;
    }
    KeyInfo info = {dbid, key, val, REDISMODULE_WRITE};
    moduleFireServerEvent(REDISMODULE_EVENT_KEY, subevent, &info);

    if (val->type == OBJ_MODULE) {
        moduleValue *mv = val->ptr;
        moduleType *mt = mv->type;
        /* We prefer to use the enhanced version. */
        if (mt->unlink2 != NULL) {
            RedisModuleKeyOptCtx ctx = {key, NULL, dbid, -1};
            mt->unlink2(&ctx,mv->value);
        } else if (mt->unlink != NULL) {
            mt->unlink(key,mv->value);
        }
    }
    server.lazy_expire_disabled--;
}

/* Return the free_effort of the module, it will automatically choose to call 
 * `free_effort` or `free_effort2`, and the default return value is 1.
 * value of 0 means very high effort (always asynchronous freeing). */
size_t moduleGetFreeEffort(robj *key, robj *val, int dbid) {
    moduleValue *mv = val->ptr;
    moduleType *mt = mv->type;
    size_t effort = 1;
    /* We prefer to use the enhanced version. */
    if (mt->free_effort2 != NULL) {
        RedisModuleKeyOptCtx ctx = {key, NULL, dbid, -1};
        effort = mt->free_effort2(&ctx,mv->value);
    } else if (mt->free_effort != NULL) {
        effort = mt->free_effort(key,mv->value);
    }  

    return effort;
}

/* Return the memory usage of the module, it will automatically choose to call 
 * `mem_usage` or `mem_usage2`, and the default return value is 0. */
size_t moduleGetMemUsage(robj *key, robj *val, size_t sample_size, int dbid) {
    moduleValue *mv = val->ptr;
    moduleType *mt = mv->type;
    size_t size = 0;
    /* We prefer to use the enhanced version. */
    if (mt->mem_usage2 != NULL) {
        RedisModuleKeyOptCtx ctx = {key, NULL, dbid, -1};
        size = mt->mem_usage2(&ctx, mv->value, sample_size);
    } else if (mt->mem_usage != NULL) {
        size = mt->mem_usage(mv->value);
    } 

    return size;
}

/* --------------------------------------------------------------------------
 * Modules API internals
 * -------------------------------------------------------------------------- */

/* server.moduleapi dictionary type. Only uses plain C strings since
 * this gets queries from modules. */

uint64_t dictCStringKeyHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, strlen((char*)key));
}

int dictCStringKeyCompare(dict *d, const void *key1, const void *key2) {
    UNUSED(d);
    return strcmp(key1,key2) == 0;
}

dictType moduleAPIDictType = {
    dictCStringKeyHash,        /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictCStringKeyCompare,     /* key compare */
    NULL,                      /* key destructor */
    NULL,                      /* val destructor */
    NULL                       /* allow to expand */
};

int moduleRegisterApi(const char *funcname, void *funcptr) {
    return dictAdd(server.moduleapi, (char*)funcname, funcptr);
}

#define REGISTER_API(name) \
    moduleRegisterApi("RedisModule_" #name, (void *)(unsigned long)RM_ ## name)

/* Global initialization at Redis startup. */
void moduleRegisterCoreAPI(void);

/* Currently, this function is just a placeholder for the module system
 * initialization steps that need to be run after server initialization.
 * A previous issue, selectDb() in createClient() requires that server.db has
 * been initialized, see #7323. */
void moduleInitModulesSystemLast(void) {
}


dictType sdsKeyValueHashDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictSdsDestructor,          /* val destructor */
    NULL                        /* allow to expand */
};

void moduleInitModulesSystem(void) {
    moduleUnblockedClients = listCreate();
    server.loadmodule_queue = listCreate();
    server.module_configs_queue = dictCreate(&sdsKeyValueHashDictType);
    modules = dictCreate(&modulesDictType);
    moduleAuthCallbacks = listCreate();

    /* Set up the keyspace notification subscriber list and static client */
    moduleKeyspaceSubscribers = listCreate();

    modulePostExecUnitJobs = listCreate();

    /* Set up filter list */
    moduleCommandFilters = listCreate();

    moduleRegisterCoreAPI();

    /* Create a pipe for module threads to be able to wake up the redis main thread.
     * Make the pipe non blocking. This is just a best effort aware mechanism
     * and we do not want to block not in the read nor in the write half.
     * Enable close-on-exec flag on pipes in case of the fork-exec system calls in
     * sentinels or redis servers. */
    if (anetPipe(server.module_pipe, O_CLOEXEC|O_NONBLOCK, O_CLOEXEC|O_NONBLOCK) == -1) {
        serverLog(LL_WARNING,
            "Can't create the pipe for module threads: %s", strerror(errno));
        exit(1);
    }

    /* Create the timers radix tree. */
    Timers = raxNew();

    /* Setup the event listeners data structures. */
    RedisModule_EventListeners = listCreate();

    /* Making sure moduleEventVersions is synced with the number of events. */
    serverAssert(sizeof(moduleEventVersions)/sizeof(moduleEventVersions[0]) == _REDISMODULE_EVENT_NEXT);

    /* Our thread-safe contexts GIL must start with already locked:
     * it is just unlocked when it's safe. */
    pthread_mutex_lock(&moduleGIL);
}

void modulesCron(void) {
    /* Check number of temporary clients in the pool and free the unused ones
     * since the last cron. moduleTempClientMinCount tracks minimum count of
     * clients in the pool since the last cron. This is the number of clients
     * that we didn't use for the last cron period. */

    /* Limit the max client count to be freed at once to avoid latency spikes.*/
    int iteration = 50;
    /* We are freeing clients if we have more than 8 unused clients. Keeping
     * small amount of clients to avoid client allocation costs if temporary
     * clients are required after some idle period. */
    const unsigned int min_client = 8;
    while (iteration > 0 && moduleTempClientCount > 0 && moduleTempClientMinCount > min_client) {
        client *c = moduleTempClients[--moduleTempClientCount];
        freeClient(c);
        iteration--;
        moduleTempClientMinCount--;
    }
    moduleTempClientMinCount = moduleTempClientCount;

    /* Shrink moduleTempClients array itself if it is wasting some space */
    if (moduleTempClientCap > 32 && moduleTempClientCap > moduleTempClientCount * 4) {
        moduleTempClientCap /= 4;
        moduleTempClients = zrealloc(moduleTempClients,sizeof(client*)*moduleTempClientCap);
    }
}

void moduleLoadQueueEntryFree(struct moduleLoadQueueEntry *loadmod) {
    if (!loadmod) return;
    sdsfree(loadmod->path);
    for (int i = 0; i < loadmod->argc; i++) {
        decrRefCount(loadmod->argv[i]);
    }
    zfree(loadmod->argv);
    zfree(loadmod);
}

/* Remove Module Configs from standardConfig array in config.c */
void moduleRemoveConfigs(RedisModule *module) {
    listIter li;
    listNode *ln;
    listRewind(module->module_configs, &li);
    while ((ln = listNext(&li))) {
        ModuleConfig *config = listNodeValue(ln);
        sds module_name = sdsnew(module->name);
        sds full_name = sdscat(sdscat(module_name, "."), config->name); /* ModuleName.ModuleConfig */
        removeConfig(full_name);
        sdsfree(full_name);
    }
}

/* Load all the modules in the server.loadmodule_queue list, which is
 * populated by `loadmodule` directives in the configuration file.
 * We can't load modules directly when processing the configuration file
 * because the server must be fully initialized before loading modules.
 *
 * The function aborts the server on errors, since to start with missing
 * modules is not considered sane: clients may rely on the existence of
 * given commands, loading AOF also may need some modules to exist, and
 * if this instance is a slave, it must understand commands from master. */
void moduleLoadFromQueue(void) {
    listIter li;
    listNode *ln;

    listRewind(server.loadmodule_queue,&li);
    while((ln = listNext(&li))) {
        struct moduleLoadQueueEntry *loadmod = ln->value;
        if (moduleLoad(loadmod->path,(void **)loadmod->argv,loadmod->argc, 0)
            == C_ERR)
        {
            serverLog(LL_WARNING,
                "Can't load module from %s: server aborting",
                loadmod->path);
            exit(1);
        }
        moduleLoadQueueEntryFree(loadmod);
        listDelNode(server.loadmodule_queue, ln);
    }
    if (dictSize(server.module_configs_queue)) {
        serverLog(LL_WARNING, "Module Configuration detected without loadmodule directive or no ApplyConfig call: aborting");
        exit(1);
    }
}

void moduleFreeModuleStructure(struct RedisModule *module) {
    listRelease(module->types);
    listRelease(module->filters);
    listRelease(module->usedby);
    listRelease(module->using);
    listRelease(module->module_configs);
    sdsfree(module->name);
    moduleLoadQueueEntryFree(module->loadmod);
    zfree(module);
}

void moduleFreeArgs(struct redisCommandArg *args, int num_args) {
    for (int j = 0; j < num_args; j++) {
        zfree((char *)args[j].name);
        zfree((char *)args[j].token);
        zfree((char *)args[j].summary);
        zfree((char *)args[j].since);
        zfree((char *)args[j].deprecated_since);
        zfree((char *)args[j].display_text);

        if (args[j].subargs) {
            moduleFreeArgs(args[j].subargs, args[j].num_args);
        }
    }
    zfree(args);
}

/* Free the command registered with the specified module.
 * On success C_OK is returned, otherwise C_ERR is returned.
 *
 * Note that caller needs to handle the deletion of the command table dict,
 * and after that needs to free the command->fullname and the command itself.
 */
int moduleFreeCommand(struct RedisModule *module, struct redisCommand *cmd) {
    if (cmd->proc != RedisModuleCommandDispatcher)
        return C_ERR;

    RedisModuleCommand *cp = cmd->module_cmd;
    if (cp->module != module)
        return C_ERR;

    /* Free everything except cmd->fullname and cmd itself. */
    for (int j = 0; j < cmd->key_specs_num; j++) {
        if (cmd->key_specs[j].notes)
            zfree((char *)cmd->key_specs[j].notes);
        if (cmd->key_specs[j].begin_search_type == KSPEC_BS_KEYWORD)
            zfree((char *)cmd->key_specs[j].bs.keyword.keyword);
    }
    if (cmd->key_specs != cmd->key_specs_static)
        zfree(cmd->key_specs);
    for (int j = 0; cmd->tips && cmd->tips[j]; j++)
        zfree((char *)cmd->tips[j]);
    zfree(cmd->tips);
    for (int j = 0; cmd->history && cmd->history[j].since; j++) {
        zfree((char *)cmd->history[j].since);
        zfree((char *)cmd->history[j].changes);
    }
    zfree(cmd->history);
    zfree((char *)cmd->summary);
    zfree((char *)cmd->since);
    zfree((char *)cmd->deprecated_since);
    zfree((char *)cmd->complexity);
    if (cmd->latency_histogram) {
        hdr_close(cmd->latency_histogram);
        cmd->latency_histogram = NULL;
    }
    moduleFreeArgs(cmd->args, cmd->num_args);
    zfree(cp);

    if (cmd->subcommands_dict) {
        dictEntry *de;
        dictIterator *di = dictGetSafeIterator(cmd->subcommands_dict);
        while ((de = dictNext(di)) != NULL) {
            struct redisCommand *sub = dictGetVal(de);
            if (moduleFreeCommand(module, sub) != C_OK) continue;

            serverAssert(dictDelete(cmd->subcommands_dict, sub->declared_name) == DICT_OK);
            sdsfree((sds)sub->declared_name);
            sdsfree(sub->fullname);
            zfree(sub);
        }
        dictReleaseIterator(di);
        dictRelease(cmd->subcommands_dict);
    }

    return C_OK;
}

void moduleUnregisterCommands(struct RedisModule *module) {
    /* Unregister all the commands registered by this module. */
    dictIterator *di = dictGetSafeIterator(server.commands);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *cmd = dictGetVal(de);
        if (moduleFreeCommand(module, cmd) != C_OK) continue;

        serverAssert(dictDelete(server.commands, cmd->fullname) == DICT_OK);
        serverAssert(dictDelete(server.orig_commands, cmd->fullname) == DICT_OK);
        sdsfree((sds)cmd->declared_name);
        sdsfree(cmd->fullname);
        zfree(cmd);
    }
    dictReleaseIterator(di);
}

/* We parse argv to add sds "NAME VALUE" pairs to the server.module_configs_queue list of configs.
 * We also increment the module_argv pointer to just after ARGS if there are args, otherwise
 * we set it to NULL */
int parseLoadexArguments(RedisModuleString ***module_argv, int *module_argc) {
    int args_specified = 0;
    RedisModuleString **argv = *module_argv;
    int argc = *module_argc;
    for (int i = 0; i < argc; i++) {
        char *arg_val = argv[i]->ptr;
        if (!strcasecmp(arg_val, "CONFIG")) {
            if (i + 2 >= argc) {
                serverLog(LL_NOTICE, "CONFIG specified without name value pair");
                return REDISMODULE_ERR;
            }
            sds name = sdsdup(argv[i + 1]->ptr);
            sds value = sdsdup(argv[i + 2]->ptr);
            if (!dictReplace(server.module_configs_queue, name, value)) sdsfree(name);
            i += 2;
        } else if (!strcasecmp(arg_val, "ARGS")) {
            args_specified = 1;
            i++;
            if (i >= argc) {
                *module_argv = NULL;
                *module_argc = 0;
            } else {
                *module_argv = argv + i;
                *module_argc = argc - i;
            }
            break;
        } else {
            serverLog(LL_NOTICE, "Syntax Error from arguments to loadex around %s.", arg_val);
            return REDISMODULE_ERR;
        }
    }
    if (!args_specified) {
        *module_argv = NULL;
        *module_argc = 0;
    }
    return REDISMODULE_OK;
}

/* Load a module and initialize it. On success C_OK is returned, otherwise
 * C_ERR is returned. */
int moduleLoad(const char *path, void **module_argv, int module_argc, int is_loadex) {
    int (*onload)(void *, void **, int);
    void *handle;

    struct stat st;
    if (stat(path, &st) == 0) {
        /* This check is best effort */
        if (!(st.st_mode & (S_IXUSR  | S_IXGRP | S_IXOTH))) {
            serverLog(LL_WARNING, "Module %s failed to load: It does not have execute permissions.", path);
            return C_ERR;
        }
    }

    handle = dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if (handle == NULL) {
        serverLog(LL_WARNING, "Module %s failed to load: %s", path, dlerror());
        return C_ERR;
    }
    onload = (int (*)(void *, void **, int))(unsigned long) dlsym(handle,"RedisModule_OnLoad");
    if (onload == NULL) {
        dlclose(handle);
        serverLog(LL_WARNING,
            "Module %s does not export RedisModule_OnLoad() "
            "symbol. Module not loaded.",path);
        return C_ERR;
    }
    RedisModuleCtx ctx;
    moduleCreateContext(&ctx, NULL, REDISMODULE_CTX_TEMP_CLIENT); /* We pass NULL since we don't have a module yet. */
    if (onload((void*)&ctx,module_argv,module_argc) == REDISMODULE_ERR) {
        serverLog(LL_WARNING,
            "Module %s initialization failed. Module not loaded",path);
        if (ctx.module) {
            moduleUnregisterCommands(ctx.module);
            moduleUnregisterSharedAPI(ctx.module);
            moduleUnregisterUsedAPI(ctx.module);
            moduleRemoveConfigs(ctx.module);
            moduleUnregisterAuthCBs(ctx.module);
            moduleFreeModuleStructure(ctx.module);
        }
        moduleFreeContext(&ctx);
        dlclose(handle);
        return C_ERR;
    }

    /* Redis module loaded! Register it. */
    dictAdd(modules,ctx.module->name,ctx.module);
    ctx.module->blocked_clients = 0;
    ctx.module->handle = handle;
    ctx.module->loadmod = zmalloc(sizeof(struct moduleLoadQueueEntry));
    ctx.module->loadmod->path = sdsnew(path);
    ctx.module->loadmod->argv = module_argc ? zmalloc(sizeof(robj*)*module_argc) : NULL;
    ctx.module->loadmod->argc = module_argc;
    for (int i = 0; i < module_argc; i++) {
        ctx.module->loadmod->argv[i] = module_argv[i];
        incrRefCount(ctx.module->loadmod->argv[i]);
    }

    /* If module commands have ACL categories, recompute command bits 
     * for all existing users once the modules has been registered. */
    if (ctx.module->num_commands_with_acl_categories) {
        ACLRecomputeCommandBitsFromCommandRulesAllUsers();
    }
    serverLog(LL_NOTICE,"Module '%s' loaded from %s",ctx.module->name,path);
    ctx.module->onload = 0;

    int post_load_err = 0;
    if (listLength(ctx.module->module_configs) && !ctx.module->configs_initialized) {
        serverLogRaw(LL_WARNING, "Module Configurations were not set, likely a missing LoadConfigs call. Unloading the module.");
        post_load_err = 1;
    }

    if (is_loadex && dictSize(server.module_configs_queue)) {
        serverLogRaw(LL_WARNING, "Loadex configurations were not applied, likely due to invalid arguments. Unloading the module.");
        post_load_err = 1;
    }

    if (post_load_err) {
        /* Unregister module auth callbacks (if any exist) that this Module registered onload. */
        moduleUnregisterAuthCBs(ctx.module);
        moduleUnload(ctx.module->name, NULL);
        moduleFreeContext(&ctx);
        return C_ERR;
    }

    /* Fire the loaded modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_MODULE_CHANGE,
                          REDISMODULE_SUBEVENT_MODULE_LOADED,
                          ctx.module);

    moduleFreeContext(&ctx);
    return C_OK;
}

/* Unload the module registered with the specified name. On success
 * C_OK is returned, otherwise C_ERR is returned and errmsg is set
 * with an appropriate message. */
int moduleUnload(sds name, const char **errmsg) {
    struct RedisModule *module = dictFetchValue(modules,name);

    if (module == NULL) {
        *errmsg = "no such module with that name";
        return C_ERR;
    } else if (listLength(module->types)) {
        *errmsg = "the module exports one or more module-side data "
                  "types, can't unload";
        return C_ERR;
    } else if (listLength(module->usedby)) {
        *errmsg = "the module exports APIs used by other modules. "
                  "Please unload them first and try again";
        return C_ERR;
    } else if (module->blocked_clients) {
        *errmsg = "the module has blocked clients. "
                  "Please wait for them to be unblocked and try again";
        return C_ERR;
    } else if (moduleHoldsTimer(module)) {
        *errmsg = "the module holds timer that is not fired. "
                  "Please stop the timer or wait until it fires.";
        return C_ERR;
    }

    /* Give module a chance to clean up. */
    int (*onunload)(void *);
    onunload = (int (*)(void *))(unsigned long) dlsym(module->handle, "RedisModule_OnUnload");
    if (onunload) {
        RedisModuleCtx ctx;
        moduleCreateContext(&ctx, module, REDISMODULE_CTX_TEMP_CLIENT);
        int unload_status = onunload((void*)&ctx);
        moduleFreeContext(&ctx);

        if (unload_status == REDISMODULE_ERR) {
            serverLog(LL_WARNING, "Module %s OnUnload failed.  Unload canceled.", name);
            errno = ECANCELED;
            return C_ERR;
        }
    }

    moduleFreeAuthenticatedClients(module);
    moduleUnregisterCommands(module);
    moduleUnregisterSharedAPI(module);
    moduleUnregisterUsedAPI(module);
    moduleUnregisterFilters(module);
    moduleUnregisterAuthCBs(module);
    moduleRemoveConfigs(module);

    /* Remove any notification subscribers this module might have */
    moduleUnsubscribeNotifications(module);
    moduleUnsubscribeAllServerEvents(module);

    /* Unload the dynamic library. */
    if (dlclose(module->handle) == -1) {
        char *error = dlerror();
        if (error == NULL) error = "Unknown error";
        serverLog(LL_WARNING,"Error when trying to close the %s module: %s",
            module->name, error);
    }

    /* Fire the unloaded modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_MODULE_CHANGE,
                          REDISMODULE_SUBEVENT_MODULE_UNLOADED,
                          module);

    /* Remove from list of modules. */
    serverLog(LL_NOTICE,"Module %s unloaded",module->name);
    dictDelete(modules,module->name);
    module->name = NULL; /* The name was already freed by dictDelete(). */
    moduleFreeModuleStructure(module);

    /* Recompute command bits for all users once the modules has been completely unloaded. */
    ACLRecomputeCommandBitsFromCommandRulesAllUsers();
    return C_OK;
}

void modulePipeReadable(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(fd);
    UNUSED(mask);
    UNUSED(privdata);

    char buf[128];
    while (read(fd, buf, sizeof(buf)) == sizeof(buf));

    /* Handle event loop events if pipe was written from event loop API */
    eventLoopHandleOneShotEvents();
}

/* Helper function for the MODULE and HELLO command: send the list of the
 * loaded modules to the client. */
void addReplyLoadedModules(client *c) {
    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;

    addReplyArrayLen(c,dictSize(modules));
    while ((de = dictNext(di)) != NULL) {
        sds name = dictGetKey(de);
        struct RedisModule *module = dictGetVal(de);
        sds path = module->loadmod->path;
        addReplyMapLen(c,4);
        addReplyBulkCString(c,"name");
        addReplyBulkCBuffer(c,name,sdslen(name));
        addReplyBulkCString(c,"ver");
        addReplyLongLong(c,module->ver);
        addReplyBulkCString(c,"path");
        addReplyBulkCBuffer(c,path,sdslen(path));
        addReplyBulkCString(c,"args");
        addReplyArrayLen(c,module->loadmod->argc);
        for (int i = 0; i < module->loadmod->argc; i++) {
            addReplyBulk(c,module->loadmod->argv[i]);
        }
    }
    dictReleaseIterator(di);
}

/* Helper for genModulesInfoString(): given a list of modules, return
 * an SDS string in the form "[modulename|modulename2|...]" */
sds genModulesInfoStringRenderModulesList(list *l) {
    listIter li;
    listNode *ln;
    listRewind(l,&li);
    sds output = sdsnew("[");
    while((ln = listNext(&li))) {
        RedisModule *module = ln->value;
        output = sdscat(output,module->name);
        if (ln != listLast(l))
            output = sdscat(output,"|");
    }
    output = sdscat(output,"]");
    return output;
}

/* Helper for genModulesInfoString(): render module options as an SDS string. */
sds genModulesInfoStringRenderModuleOptions(struct RedisModule *module) {
    sds output = sdsnew("[");
    if (module->options & REDISMODULE_OPTIONS_HANDLE_IO_ERRORS)
        output = sdscat(output,"handle-io-errors|");
    if (module->options & REDISMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD)
        output = sdscat(output,"handle-repl-async-load|");
    if (module->options & REDISMODULE_OPTION_NO_IMPLICIT_SIGNAL_MODIFIED)
        output = sdscat(output,"no-implicit-signal-modified|");
    output = sdstrim(output,"|");
    output = sdscat(output,"]");
    return output;
}


/* Helper function for the INFO command: adds loaded modules as to info's
 * output.
 *
 * After the call, the passed sds info string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds genModulesInfoString(sds info) {
    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;

    while ((de = dictNext(di)) != NULL) {
        sds name = dictGetKey(de);
        struct RedisModule *module = dictGetVal(de);

        sds usedby = genModulesInfoStringRenderModulesList(module->usedby);
        sds using = genModulesInfoStringRenderModulesList(module->using);
        sds options = genModulesInfoStringRenderModuleOptions(module);
        info = sdscatfmt(info,
            "module:name=%S,ver=%i,api=%i,filters=%i,"
            "usedby=%S,using=%S,options=%S\r\n",
                name, module->ver, module->apiver,
                (int)listLength(module->filters), usedby, using, options);
        sdsfree(usedby);
        sdsfree(using);
        sdsfree(options);
    }
    dictReleaseIterator(di);
    return info;
}

/* --------------------------------------------------------------------------
 * Module Configurations API internals
 * -------------------------------------------------------------------------- */
	 
/* Check if the configuration name is already registered */
int isModuleConfigNameRegistered(RedisModule *module, sds name) {
    listNode *match = listSearchKey(module->module_configs, (void *) name);
    return match != NULL;
}

/* Assert that the flags passed into the RM_RegisterConfig Suite are valid */
int moduleVerifyConfigFlags(unsigned int flags, configType type) {
    if ((flags & ~(REDISMODULE_CONFIG_DEFAULT
                    | REDISMODULE_CONFIG_IMMUTABLE
                    | REDISMODULE_CONFIG_SENSITIVE
                    | REDISMODULE_CONFIG_HIDDEN
                    | REDISMODULE_CONFIG_PROTECTED
                    | REDISMODULE_CONFIG_DENY_LOADING
                    | REDISMODULE_CONFIG_BITFLAGS
                    | REDISMODULE_CONFIG_MEMORY))) {
        serverLogRaw(LL_WARNING, "Invalid flag(s) for configuration");
        return REDISMODULE_ERR;
    }
    if (type != NUMERIC_CONFIG && flags & REDISMODULE_CONFIG_MEMORY) {
        serverLogRaw(LL_WARNING, "Numeric flag provided for non-numeric configuration.");
        return REDISMODULE_ERR;
    }
    if (type != ENUM_CONFIG && flags & REDISMODULE_CONFIG_BITFLAGS) {
        serverLogRaw(LL_WARNING, "Enum flag provided for non-enum configuration.");
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}

int moduleVerifyConfigName(sds name) {
    if (sdslen(name) == 0) {
        serverLogRaw(LL_WARNING, "Module config names cannot be an empty string.");
        return REDISMODULE_ERR;
    }
    for (size_t i = 0 ; i < sdslen(name) ; ++i) {
        char curr_char = name[i];
        if ((curr_char >= 'a' && curr_char <= 'z') ||
            (curr_char >= 'A' && curr_char <= 'Z') ||
            (curr_char >= '0' && curr_char <= '9') ||
            (curr_char == '_') || (curr_char == '-'))
        {
            continue;
        }
        serverLog(LL_WARNING, "Invalid character %c in Module Config name %s.", curr_char, name);
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}

/* This is a series of set functions for each type that act as dispatchers for 
 * config.c to call module set callbacks. */
#define CONFIG_ERR_SIZE 256
static char configerr[CONFIG_ERR_SIZE];
static void propagateErrorString(RedisModuleString *err_in, const char **err) {
    if (err_in) {
        redis_strlcpy(configerr, err_in->ptr, CONFIG_ERR_SIZE);
        decrRefCount(err_in);
        *err = configerr;
    }
}

int setModuleBoolConfig(ModuleConfig *config, int val, const char **err) {
    RedisModuleString *error = NULL;
    int return_code = config->set_fn.set_bool(config->name, val, config->privdata, &error);
    propagateErrorString(error, err);
    return return_code == REDISMODULE_OK ? 1 : 0;
}

int setModuleStringConfig(ModuleConfig *config, sds strval, const char **err) {
    RedisModuleString *error = NULL;
    RedisModuleString *new = createStringObject(strval, sdslen(strval));
    int return_code = config->set_fn.set_string(config->name, new, config->privdata, &error);
    propagateErrorString(error, err);
    decrRefCount(new);
    return return_code == REDISMODULE_OK ? 1 : 0;
}

int setModuleEnumConfig(ModuleConfig *config, int val, const char **err) {
    RedisModuleString *error = NULL;
    int return_code = config->set_fn.set_enum(config->name, val, config->privdata, &error);
    propagateErrorString(error, err);
    return return_code == REDISMODULE_OK ? 1 : 0;
}

int setModuleNumericConfig(ModuleConfig *config, long long val, const char **err) {
    RedisModuleString *error = NULL;
    int return_code = config->set_fn.set_numeric(config->name, val, config->privdata, &error);
    propagateErrorString(error, err);
    return return_code == REDISMODULE_OK ? 1 : 0;
}

/* This is a series of get functions for each type that act as dispatchers for 
 * config.c to call module set callbacks. */
int getModuleBoolConfig(ModuleConfig *module_config) {
    return module_config->get_fn.get_bool(module_config->name, module_config->privdata);
}

sds getModuleStringConfig(ModuleConfig *module_config) {
    RedisModuleString *val = module_config->get_fn.get_string(module_config->name, module_config->privdata);
    return val ? sdsdup(val->ptr) : NULL;
}

int getModuleEnumConfig(ModuleConfig *module_config) {
    return module_config->get_fn.get_enum(module_config->name, module_config->privdata);
}

long long getModuleNumericConfig(ModuleConfig *module_config) {
    return module_config->get_fn.get_numeric(module_config->name, module_config->privdata);
}

/* This function takes a module and a list of configs stored as sds NAME VALUE pairs.
 * It attempts to call set on each of these configs. */
int loadModuleConfigs(RedisModule *module) {
    listIter li;
    listNode *ln;
    const char *err = NULL;
    listRewind(module->module_configs, &li);
    while ((ln = listNext(&li))) {
        ModuleConfig *module_config = listNodeValue(ln);
        sds config_name = sdscatfmt(sdsempty(), "%s.%s", module->name, module_config->name);
        dictEntry *config_argument = dictFind(server.module_configs_queue, config_name);
        if (config_argument) {
            if (!performModuleConfigSetFromName(dictGetKey(config_argument), dictGetVal(config_argument), &err)) {
                serverLog(LL_WARNING, "Issue during loading of configuration %s : %s", (sds) dictGetKey(config_argument), err);
                sdsfree(config_name);
                dictEmpty(server.module_configs_queue, NULL);
                return REDISMODULE_ERR;
            }
        } else {
            if (!performModuleConfigSetDefaultFromName(config_name, &err)) {
                serverLog(LL_WARNING, "Issue attempting to set default value of configuration %s : %s", module_config->name, err);
                sdsfree(config_name);
                dictEmpty(server.module_configs_queue, NULL);
                return REDISMODULE_ERR;
            }
        }
        dictDelete(server.module_configs_queue, config_name);
        sdsfree(config_name);
    }
    module->configs_initialized = 1;
    return REDISMODULE_OK;
}

/* Add module_config to the list if the apply and privdata do not match one already in it. */
void addModuleConfigApply(list *module_configs, ModuleConfig *module_config) {
    if (!module_config->apply_fn) return;
    listIter li;
    listNode *ln;
    ModuleConfig *pending_apply;
    listRewind(module_configs, &li);
    while ((ln = listNext(&li))) {
        pending_apply = listNodeValue(ln);
        if (pending_apply->apply_fn == module_config->apply_fn && pending_apply->privdata == module_config->privdata) {
            return;
        }
    }
    listAddNodeTail(module_configs, module_config);
}

/* Call apply on all module configs specified in set, if an apply function was specified at registration time. */
int moduleConfigApplyConfig(list *module_configs, const char **err, const char **err_arg_name) {
    if (!listLength(module_configs)) return 1;
    listIter li;
    listNode *ln;
    ModuleConfig *module_config;
    RedisModuleString *error = NULL;
    RedisModuleCtx ctx;

    listRewind(module_configs, &li);
    while ((ln = listNext(&li))) {
        module_config = listNodeValue(ln);
        moduleCreateContext(&ctx, module_config->module, REDISMODULE_CTX_NONE);
        if (module_config->apply_fn(&ctx, module_config->privdata, &error)) {
            if (err_arg_name) *err_arg_name = module_config->name;
            propagateErrorString(error, err);
            moduleFreeContext(&ctx);
            return 0;
        }
        moduleFreeContext(&ctx);
    }
    return 1;
}

/* --------------------------------------------------------------------------
 * ## Module Configurations API
 * -------------------------------------------------------------------------- */

/* Create a module config object. */
ModuleConfig *createModuleConfig(sds name, RedisModuleConfigApplyFunc apply_fn, void *privdata, RedisModule *module) {
    ModuleConfig *new_config = zmalloc(sizeof(ModuleConfig));
    new_config->name = sdsdup(name);
    new_config->apply_fn = apply_fn;
    new_config->privdata = privdata;
    new_config->module = module;
    return new_config;
}

int moduleConfigValidityCheck(RedisModule *module, sds name, unsigned int flags, configType type) {
    if (!module->onload) {
        errno = EBUSY;
        return REDISMODULE_ERR;
    }
    if (moduleVerifyConfigFlags(flags, type) || moduleVerifyConfigName(name)) {
        errno = EINVAL;
        return REDISMODULE_ERR;
    }
    if (isModuleConfigNameRegistered(module, name)) {
        serverLog(LL_WARNING, "Configuration by the name: %s already registered", name);
        errno = EALREADY;
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}

unsigned int maskModuleConfigFlags(unsigned int flags) {
    unsigned int new_flags = 0;
    if (flags & REDISMODULE_CONFIG_DEFAULT) new_flags |= MODIFIABLE_CONFIG;
    if (flags & REDISMODULE_CONFIG_IMMUTABLE) new_flags |= IMMUTABLE_CONFIG;
    if (flags & REDISMODULE_CONFIG_HIDDEN) new_flags |= HIDDEN_CONFIG;
    if (flags & REDISMODULE_CONFIG_PROTECTED) new_flags |= PROTECTED_CONFIG;
    if (flags & REDISMODULE_CONFIG_DENY_LOADING) new_flags |= DENY_LOADING_CONFIG;
    return new_flags;
}

unsigned int maskModuleNumericConfigFlags(unsigned int flags) {
    unsigned int new_flags = 0;
    if (flags & REDISMODULE_CONFIG_MEMORY) new_flags |= MEMORY_CONFIG;
    return new_flags;
}

unsigned int maskModuleEnumConfigFlags(unsigned int flags) {
    unsigned int new_flags = 0;
    if (flags & REDISMODULE_CONFIG_BITFLAGS) new_flags |= MULTI_ARG_CONFIG;
    return new_flags;
}

/* Create a string config that Redis users can interact with via the Redis config file,
 * `CONFIG SET`, `CONFIG GET`, and `CONFIG REWRITE` commands.
 *
 * The actual config value is owned by the module, and the `getfn`, `setfn` and optional
 * `applyfn` callbacks that are provided to Redis in order to access or manipulate the
 * value. The `getfn` callback retrieves the value from the module, while the `setfn`
 * callback provides a value to be stored into the module config.
 * The optional `applyfn` callback is called after a `CONFIG SET` command modified one or
 * more configs using the `setfn` callback and can be used to atomically apply a config
 * after several configs were changed together.
 * If there are multiple configs with `applyfn` callbacks set by a single `CONFIG SET`
 * command, they will be deduplicated if their `applyfn` function and `privdata` pointers
 * are identical, and the callback will only be run once.
 * Both the `setfn` and `applyfn` can return an error if the provided value is invalid or
 * cannot be used.
 * The config also declares a type for the value that is validated by Redis and
 * provided to the module. The config system provides the following types:
 *
 * * Redis String: Binary safe string data.
 * * Enum: One of a finite number of string tokens, provided during registration.
 * * Numeric: 64 bit signed integer, which also supports min and max values.
 * * Bool: Yes or no value.
 *
 * The `setfn` callback is expected to return REDISMODULE_OK when the value is successfully
 * applied. It can also return REDISMODULE_ERR if the value can't be applied, and the
 * *err pointer can be set with a RedisModuleString error message to provide to the client.
 * This RedisModuleString will be freed by redis after returning from the set callback.
 *
 * All configs are registered with a name, a type, a default value, private data that is made
 * available in the callbacks, as well as several flags that modify the behavior of the config.
 * The name must only contain alphanumeric characters or dashes. The supported flags are:
 *
 * * REDISMODULE_CONFIG_DEFAULT: The default flags for a config. This creates a config that can be modified after startup.
 * * REDISMODULE_CONFIG_IMMUTABLE: This config can only be provided loading time.
 * * REDISMODULE_CONFIG_SENSITIVE: The value stored in this config is redacted from all logging.
 * * REDISMODULE_CONFIG_HIDDEN: The name is hidden from `CONFIG GET` with pattern matching.
 * * REDISMODULE_CONFIG_PROTECTED: This config will be only be modifiable based off the value of enable-protected-configs.
 * * REDISMODULE_CONFIG_DENY_LOADING: This config is not modifiable while the server is loading data.
 * * REDISMODULE_CONFIG_MEMORY: For numeric configs, this config will convert data unit notations into their byte equivalent.
 * * REDISMODULE_CONFIG_BITFLAGS: For enum configs, this config will allow multiple entries to be combined as bit flags.
 *
 * Default values are used on startup to set the value if it is not provided via the config file
 * or command line. Default values are also used to compare to on a config rewrite.
 *
 * Notes:
 *
 *  1. On string config sets that the string passed to the set callback will be freed after execution and the module must retain it.
 *  2. On string config gets the string will not be consumed and will be valid after execution.
 *
 * Example implementation:
 *
 *     RedisModuleString *strval;
 *     int adjustable = 1;
 *     RedisModuleString *getStringConfigCommand(const char *name, void *privdata) {
 *         return strval;
 *     }
 *
 *     int setStringConfigCommand(const char *name, RedisModuleString *new, void *privdata, RedisModuleString **err) {
 *        if (adjustable) {
 *            RedisModule_Free(strval);
 *            RedisModule_RetainString(NULL, new);
 *            strval = new;
 *            return REDISMODULE_OK;
 *        }
 *        *err = RedisModule_CreateString(NULL, "Not adjustable.", 15);
 *        return REDISMODULE_ERR;
 *     }
 *     ...
 *     RedisModule_RegisterStringConfig(ctx, "string", NULL, REDISMODULE_CONFIG_DEFAULT, getStringConfigCommand, setStringConfigCommand, NULL, NULL);
 *
 * If the registration fails, REDISMODULE_ERR is returned and one of the following
 * errno is set:
 * * EBUSY: Registering the Config outside of RedisModule_OnLoad.
 * * EINVAL: The provided flags are invalid for the registration or the name of the config contains invalid characters.
 * * EALREADY: The provided configuration name is already used. */
int RM_RegisterStringConfig(RedisModuleCtx *ctx, const char *name, const char *default_val, unsigned int flags, RedisModuleConfigGetStringFunc getfn, RedisModuleConfigSetStringFunc setfn, RedisModuleConfigApplyFunc applyfn, void *privdata) {
    RedisModule *module = ctx->module;
    sds config_name = sdsnew(name);
    if (moduleConfigValidityCheck(module, config_name, flags, NUMERIC_CONFIG)) {
        sdsfree(config_name);
        return REDISMODULE_ERR;
    }
    ModuleConfig *new_config = createModuleConfig(config_name, applyfn, privdata, module);
    sdsfree(config_name);
    new_config->get_fn.get_string = getfn;
    new_config->set_fn.set_string = setfn;
    listAddNodeTail(module->module_configs, new_config);
    flags = maskModuleConfigFlags(flags);
    addModuleStringConfig(module->name, name, flags, new_config, default_val ? sdsnew(default_val) : NULL);
    return REDISMODULE_OK;
}

/* Create a bool config that server clients can interact with via the 
 * `CONFIG SET`, `CONFIG GET`, and `CONFIG REWRITE` commands. See 
 * RedisModule_RegisterStringConfig for detailed information about configs. */
int RM_RegisterBoolConfig(RedisModuleCtx *ctx, const char *name, int default_val, unsigned int flags, RedisModuleConfigGetBoolFunc getfn, RedisModuleConfigSetBoolFunc setfn, RedisModuleConfigApplyFunc applyfn, void *privdata) {
    RedisModule *module = ctx->module;
    sds config_name = sdsnew(name);
    if (moduleConfigValidityCheck(module, config_name, flags, BOOL_CONFIG)) {
        sdsfree(config_name);
        return REDISMODULE_ERR;
    }
    ModuleConfig *new_config = createModuleConfig(config_name, applyfn, privdata, module);
    sdsfree(config_name);
    new_config->get_fn.get_bool = getfn;
    new_config->set_fn.set_bool = setfn;
    listAddNodeTail(module->module_configs, new_config);
    flags = maskModuleConfigFlags(flags);
    addModuleBoolConfig(module->name, name, flags, new_config, default_val);
    return REDISMODULE_OK;
}

/* 
 * Create an enum config that server clients can interact with via the 
 * `CONFIG SET`, `CONFIG GET`, and `CONFIG REWRITE` commands. 
 * Enum configs are a set of string tokens to corresponding integer values, where 
 * the string value is exposed to Redis clients but the value passed Redis and the
 * module is the integer value. These values are defined in enum_values, an array
 * of null-terminated c strings, and int_vals, an array of enum values who has an
 * index partner in enum_values.
 * Example Implementation:
 *      const char *enum_vals[3] = {"first", "second", "third"};
 *      const int int_vals[3] = {0, 2, 4};
 *      int enum_val = 0;
 *
 *      int getEnumConfigCommand(const char *name, void *privdata) {
 *          return enum_val;
 *      }
 *       
 *      int setEnumConfigCommand(const char *name, int val, void *privdata, const char **err) {
 *          enum_val = val;
 *          return REDISMODULE_OK;
 *      }
 *      ...
 *      RedisModule_RegisterEnumConfig(ctx, "enum", 0, REDISMODULE_CONFIG_DEFAULT, enum_vals, int_vals, 3, getEnumConfigCommand, setEnumConfigCommand, NULL, NULL);
 *
 * Note that you can use REDISMODULE_CONFIG_BITFLAGS so that multiple enum string
 * can be combined into one integer as bit flags, in which case you may want to
 * sort your enums so that the preferred combinations are present first.
 *
 * See RedisModule_RegisterStringConfig for detailed general information about configs. */
int RM_RegisterEnumConfig(RedisModuleCtx *ctx, const char *name, int default_val, unsigned int flags, const char **enum_values, const int *int_values, int num_enum_vals, RedisModuleConfigGetEnumFunc getfn, RedisModuleConfigSetEnumFunc setfn, RedisModuleConfigApplyFunc applyfn, void *privdata) {
    RedisModule *module = ctx->module;
    sds config_name = sdsnew(name);
    if (moduleConfigValidityCheck(module, config_name, flags, ENUM_CONFIG)) {
        sdsfree(config_name);
        return REDISMODULE_ERR;
    }
    ModuleConfig *new_config = createModuleConfig(config_name, applyfn, privdata, module);
    sdsfree(config_name);
    new_config->get_fn.get_enum = getfn;
    new_config->set_fn.set_enum = setfn;
    configEnum *enum_vals = zmalloc((num_enum_vals + 1) * sizeof(configEnum));
    for (int i = 0; i < num_enum_vals; i++) {
        enum_vals[i].name = zstrdup(enum_values[i]);
        enum_vals[i].val = int_values[i];
    }
    enum_vals[num_enum_vals].name = NULL;
    enum_vals[num_enum_vals].val = 0;
    listAddNodeTail(module->module_configs, new_config);
    flags = maskModuleConfigFlags(flags) | maskModuleEnumConfigFlags(flags);
    addModuleEnumConfig(module->name, name, flags, new_config, default_val, enum_vals);
    return REDISMODULE_OK;
}

/*
 * Create an integer config that server clients can interact with via the 
 * `CONFIG SET`, `CONFIG GET`, and `CONFIG REWRITE` commands. See 
 * RedisModule_RegisterStringConfig for detailed information about configs. */
int RM_RegisterNumericConfig(RedisModuleCtx *ctx, const char *name, long long default_val, unsigned int flags, long long min, long long max, RedisModuleConfigGetNumericFunc getfn, RedisModuleConfigSetNumericFunc setfn, RedisModuleConfigApplyFunc applyfn, void *privdata) {
    RedisModule *module = ctx->module;
    sds config_name = sdsnew(name);
    if (moduleConfigValidityCheck(module, config_name, flags, NUMERIC_CONFIG)) {
        sdsfree(config_name);
        return REDISMODULE_ERR;
    }
    ModuleConfig *new_config = createModuleConfig(config_name, applyfn, privdata, module);
    sdsfree(config_name);
    new_config->get_fn.get_numeric = getfn;
    new_config->set_fn.set_numeric = setfn;
    listAddNodeTail(module->module_configs, new_config);
    unsigned int numeric_flags = maskModuleNumericConfigFlags(flags);
    flags = maskModuleConfigFlags(flags);
    addModuleNumericConfig(module->name, name, flags, new_config, default_val, numeric_flags, min, max);
    return REDISMODULE_OK;
}

/* Applies all pending configurations on the module load. This should be called
 * after all of the configurations have been registered for the module inside of RedisModule_OnLoad.
 * This will return REDISMODULE_ERR if it is called outside RedisModule_OnLoad.
 * This API needs to be called when configurations are provided in either `MODULE LOADEX`
 * or provided as startup arguments. */
int RM_LoadConfigs(RedisModuleCtx *ctx) {
    if (!ctx || !ctx->module || !ctx->module->onload) {
        return REDISMODULE_ERR;
    }
    RedisModule *module = ctx->module;
    /* Load configs from conf file or arguments from loadex */
    if (loadModuleConfigs(module)) return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

/* Redis MODULE command.
 *
 * MODULE LIST
 * MODULE LOAD <path> [args...]
 * MODULE LOADEX <path> [[CONFIG NAME VALUE] [CONFIG NAME VALUE]] [ARGS ...]
 * MODULE UNLOAD <name>
 */
void moduleCommand(client *c) {
    char *subcmd = c->argv[1]->ptr;

    if (c->argc == 2 && !strcasecmp(subcmd,"help")) {
        const char *help[] = {
"LIST",
"    Return a list of loaded modules.",
"LOAD <path> [<arg> ...]",
"    Load a module library from <path>, passing to it any optional arguments.",
"LOADEX <path> [[CONFIG NAME VALUE] [CONFIG NAME VALUE]] [ARGS ...]",
"    Load a module library from <path>, while passing it module configurations and optional arguments.",
"UNLOAD <name>",
"    Unload a module.",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(subcmd,"load") && c->argc >= 3) {
        robj **argv = NULL;
        int argc = 0;

        if (c->argc > 3) {
            argc = c->argc - 3;
            argv = &c->argv[3];
        }

        if (moduleLoad(c->argv[2]->ptr,(void **)argv,argc, 0) == C_OK)
            addReply(c,shared.ok);
        else
            addReplyError(c,
                "Error loading the extension. Please check the server logs.");
    } else if (!strcasecmp(subcmd,"loadex") && c->argc >= 3) {
        robj **argv = NULL;
        int argc = 0;

        if (c->argc > 3) {
            argc = c->argc - 3;
            argv = &c->argv[3];
        }
        /* If this is a loadex command we want to populate server.module_configs_queue with 
         * sds NAME VALUE pairs. We also want to increment argv to just after ARGS, if supplied. */
        if (parseLoadexArguments((RedisModuleString ***) &argv, &argc) == REDISMODULE_OK &&
            moduleLoad(c->argv[2]->ptr, (void **)argv, argc, 1) == C_OK)
            addReply(c,shared.ok);
        else {
            dictEmpty(server.module_configs_queue, NULL);
            addReplyError(c,
                "Error loading the extension. Please check the server logs.");
        }

    } else if (!strcasecmp(subcmd,"unload") && c->argc == 3) {
        const char *errmsg = NULL;
        if (moduleUnload(c->argv[2]->ptr, &errmsg) == C_OK)
            addReply(c,shared.ok);
        else {
            if (errmsg == NULL) errmsg = "operation not possible.";
            addReplyErrorFormat(c, "Error unloading module: %s", errmsg);
            serverLog(LL_WARNING, "Error unloading module %s: %s", (sds) c->argv[2]->ptr, errmsg);
        }
    } else if (!strcasecmp(subcmd,"list") && c->argc == 2) {
        addReplyLoadedModules(c);
    } else {
        addReplySubcommandSyntaxError(c);
        return;
    }
}

/* Return the number of registered modules. */
size_t moduleCount(void) {
    return dictSize(modules);
}

/* --------------------------------------------------------------------------
 * ## Key eviction API
 * -------------------------------------------------------------------------- */

/* Set the key last access time for LRU based eviction. not relevant if the
 * servers's maxmemory policy is LFU based. Value is idle time in milliseconds.
 * returns REDISMODULE_OK if the LRU was updated, REDISMODULE_ERR otherwise. */
int RM_SetLRU(RedisModuleKey *key, mstime_t lru_idle) {
    if (!key->value)
        return REDISMODULE_ERR;
    if (objectSetLRUOrLFU(key->value, -1, lru_idle, lru_idle>=0 ? LRU_CLOCK() : 0, 1))
        return REDISMODULE_OK;
    return REDISMODULE_ERR;
}

/* Gets the key last access time.
 * Value is idletime in milliseconds or -1 if the server's eviction policy is
 * LFU based.
 * returns REDISMODULE_OK if when key is valid. */
int RM_GetLRU(RedisModuleKey *key, mstime_t *lru_idle) {
    *lru_idle = -1;
    if (!key->value)
        return REDISMODULE_ERR;
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU)
        return REDISMODULE_OK;
    *lru_idle = estimateObjectIdleTime(key->value);
    return REDISMODULE_OK;
}

/* Set the key access frequency. only relevant if the server's maxmemory policy
 * is LFU based.
 * The frequency is a logarithmic counter that provides an indication of
 * the access frequencyonly (must be <= 255).
 * returns REDISMODULE_OK if the LFU was updated, REDISMODULE_ERR otherwise. */
int RM_SetLFU(RedisModuleKey *key, long long lfu_freq) {
    if (!key->value)
        return REDISMODULE_ERR;
    if (objectSetLRUOrLFU(key->value, lfu_freq, -1, 0, 1))
        return REDISMODULE_OK;
    return REDISMODULE_ERR;
}

/* Gets the key access frequency or -1 if the server's eviction policy is not
 * LFU based.
 * returns REDISMODULE_OK if when key is valid. */
int RM_GetLFU(RedisModuleKey *key, long long *lfu_freq) {
    *lfu_freq = -1;
    if (!key->value)
        return REDISMODULE_ERR;
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU)
        *lfu_freq = LFUDecrAndReturn(key->value);
    return REDISMODULE_OK;
}

/* --------------------------------------------------------------------------
 * ## Miscellaneous APIs
 * -------------------------------------------------------------------------- */

/**
 * Returns the full module options flags mask, using the return value
 * the module can check if a certain set of module options are supported
 * by the redis server version in use.
 * Example:
 *
 *        int supportedFlags = RM_GetModuleOptionsAll();
 *        if (supportedFlags & REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS) {
 *              // REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS is supported
 *        } else{
 *              // REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS is not supported
 *        }
 */
int RM_GetModuleOptionsAll() {
    return _REDISMODULE_OPTIONS_FLAGS_NEXT - 1;
}

/**
 * Returns the full ContextFlags mask, using the return value
 * the module can check if a certain set of flags are supported
 * by the redis server version in use.
 * Example:
 *
 *        int supportedFlags = RM_GetContextFlagsAll();
 *        if (supportedFlags & REDISMODULE_CTX_FLAGS_MULTI) {
 *              // REDISMODULE_CTX_FLAGS_MULTI is supported
 *        } else{
 *              // REDISMODULE_CTX_FLAGS_MULTI is not supported
 *        }
 */
int RM_GetContextFlagsAll() {
    return _REDISMODULE_CTX_FLAGS_NEXT - 1;
}

/**
 * Returns the full KeyspaceNotification mask, using the return value
 * the module can check if a certain set of flags are supported
 * by the redis server version in use.
 * Example:
 *
 *        int supportedFlags = RM_GetKeyspaceNotificationFlagsAll();
 *        if (supportedFlags & REDISMODULE_NOTIFY_LOADED) {
 *              // REDISMODULE_NOTIFY_LOADED is supported
 *        } else{
 *              // REDISMODULE_NOTIFY_LOADED is not supported
 *        }
 */
int RM_GetKeyspaceNotificationFlagsAll() {
    return _REDISMODULE_NOTIFY_NEXT - 1;
}

/**
 * Return the redis version in format of 0x00MMmmpp.
 * Example for 6.0.7 the return value will be 0x00060007.
 */
int RM_GetServerVersion() {
    return REDIS_VERSION_NUM;
}

/**
 * Return the current redis-server runtime value of REDISMODULE_TYPE_METHOD_VERSION.
 * You can use that when calling RM_CreateDataType to know which fields of
 * RedisModuleTypeMethods are gonna be supported and which will be ignored.
 */
int RM_GetTypeMethodVersion() {
    return REDISMODULE_TYPE_METHOD_VERSION;
}

/* Replace the value assigned to a module type.
 *
 * The key must be open for writing, have an existing value, and have a moduleType
 * that matches the one specified by the caller.
 *
 * Unlike RM_ModuleTypeSetValue() which will free the old value, this function
 * simply swaps the old value with the new value.
 *
 * The function returns REDISMODULE_OK on success, REDISMODULE_ERR on errors
 * such as:
 *
 * 1. Key is not opened for writing.
 * 2. Key is not a module data type key.
 * 3. Key is a module datatype other than 'mt'.
 *
 * If old_value is non-NULL, the old value is returned by reference.
 */
int RM_ModuleTypeReplaceValue(RedisModuleKey *key, moduleType *mt, void *new_value, void **old_value) {
    if (!(key->mode & REDISMODULE_WRITE) || key->iter)
        return REDISMODULE_ERR;
    if (!key->value || key->value->type != OBJ_MODULE)
        return REDISMODULE_ERR;

    moduleValue *mv = key->value->ptr;
    if (mv->type != mt)
        return REDISMODULE_ERR;

    if (old_value)
        *old_value = mv->value;
    mv->value = new_value;

    return REDISMODULE_OK;
}

/* For a specified command, parse its arguments and return an array that
 * contains the indexes of all key name arguments. This function is
 * essentially a more efficient way to do `COMMAND GETKEYS`.
 *
 * The out_flags argument is optional, and can be set to NULL.
 * When provided it is filled with REDISMODULE_CMD_KEY_ flags in matching
 * indexes with the key indexes of the returned array.
 *
 * A NULL return value indicates the specified command has no keys, or
 * an error condition. Error conditions are indicated by setting errno
 * as follows:
 *
 * * ENOENT: Specified command does not exist.
 * * EINVAL: Invalid command arity specified.
 *
 * NOTE: The returned array is not a Redis Module object so it does not
 * get automatically freed even when auto-memory is used. The caller
 * must explicitly call RM_Free() to free it, same as the out_flags pointer if
 * used.
 */
int *RM_GetCommandKeysWithFlags(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, int *num_keys, int **out_flags) {
    UNUSED(ctx);
    struct redisCommand *cmd;
    int *res = NULL;

    /* Find command */
    if ((cmd = lookupCommand(argv,argc)) == NULL) {
        errno = ENOENT;
        return NULL;
    }

    /* Bail out if command has no keys */
    if (!doesCommandHaveKeys(cmd)) {
        errno = 0;
        return NULL;
    }

    if ((cmd->arity > 0 && cmd->arity != argc) || (argc < -cmd->arity)) {
        errno = EINVAL;
        return NULL;
    }

    getKeysResult result = GETKEYS_RESULT_INIT;
    getKeysFromCommand(cmd, argv, argc, &result);

    *num_keys = result.numkeys;
    if (!result.numkeys) {
        errno = 0;
        getKeysFreeResult(&result);
        return NULL;
    }

    /* The return value here expects an array of key positions */
    unsigned long int size = sizeof(int) * result.numkeys;
    res = zmalloc(size);
    if (out_flags)
        *out_flags = zmalloc(size);
    for (int i = 0; i < result.numkeys; i++) {
        res[i] = result.keys[i].pos;
        if (out_flags)
            (*out_flags)[i] = moduleConvertKeySpecsFlags(result.keys[i].flags, 0);
    }

    return res;
}

/* Identical to RM_GetCommandKeysWithFlags when flags are not needed. */
int *RM_GetCommandKeys(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, int *num_keys) {
    return RM_GetCommandKeysWithFlags(ctx, argv, argc, num_keys, NULL);
}

/* Return the name of the command currently running */
const char *RM_GetCurrentCommandName(RedisModuleCtx *ctx) {
    if (!ctx || !ctx->client || !ctx->client->cmd)
        return NULL;

    return (const char*)ctx->client->cmd->fullname;
}

/* --------------------------------------------------------------------------
 * ## Defrag API
 * -------------------------------------------------------------------------- */

/* The defrag context, used to manage state during calls to the data type
 * defrag callback.
 */
struct RedisModuleDefragCtx {
    long long int endtime;
    unsigned long *cursor;
    struct redisObject *key; /* Optional name of key processed, NULL when unknown. */
    int dbid;                /* The dbid of the key being processed, -1 when unknown. */
};

/* Register a defrag callback for global data, i.e. anything that the module
 * may allocate that is not tied to a specific data type.
 */
int RM_RegisterDefragFunc(RedisModuleCtx *ctx, RedisModuleDefragFunc cb) {
    ctx->module->defrag_cb = cb;
    return REDISMODULE_OK;
}

/* When the data type defrag callback iterates complex structures, this
 * function should be called periodically. A zero (false) return
 * indicates the callback may continue its work. A non-zero value (true)
 * indicates it should stop.
 *
 * When stopped, the callback may use RM_DefragCursorSet() to store its
 * position so it can later use RM_DefragCursorGet() to resume defragging.
 *
 * When stopped and more work is left to be done, the callback should
 * return 1. Otherwise, it should return 0.
 *
 * NOTE: Modules should consider the frequency in which this function is called,
 * so it generally makes sense to do small batches of work in between calls.
 */
int RM_DefragShouldStop(RedisModuleDefragCtx *ctx) {
    return (ctx->endtime != 0 && ctx->endtime < ustime());
}

/* Store an arbitrary cursor value for future re-use.
 *
 * This should only be called if RM_DefragShouldStop() has returned a non-zero
 * value and the defrag callback is about to exit without fully iterating its
 * data type.
 *
 * This behavior is reserved to cases where late defrag is performed. Late
 * defrag is selected for keys that implement the `free_effort` callback and
 * return a `free_effort` value that is larger than the defrag
 * 'active-defrag-max-scan-fields' configuration directive.
 *
 * Smaller keys, keys that do not implement `free_effort` or the global
 * defrag callback are not called in late-defrag mode. In those cases, a
 * call to this function will return REDISMODULE_ERR.
 *
 * The cursor may be used by the module to represent some progress into the
 * module's data type. Modules may also store additional cursor-related
 * information locally and use the cursor as a flag that indicates when
 * traversal of a new key begins. This is possible because the API makes
 * a guarantee that concurrent defragmentation of multiple keys will
 * not be performed.
 */
int RM_DefragCursorSet(RedisModuleDefragCtx *ctx, unsigned long cursor) {
    if (!ctx->cursor)
        return REDISMODULE_ERR;

    *ctx->cursor = cursor;
    return REDISMODULE_OK;
}

/* Fetch a cursor value that has been previously stored using RM_DefragCursorSet().
 *
 * If not called for a late defrag operation, REDISMODULE_ERR will be returned and
 * the cursor should be ignored. See RM_DefragCursorSet() for more details on
 * defrag cursors.
 */
int RM_DefragCursorGet(RedisModuleDefragCtx *ctx, unsigned long *cursor) {
    if (!ctx->cursor)
        return REDISMODULE_ERR;

    *cursor = *ctx->cursor;
    return REDISMODULE_OK;
}

/* Defrag a memory allocation previously allocated by RM_Alloc, RM_Calloc, etc.
 * The defragmentation process involves allocating a new memory block and copying
 * the contents to it, like realloc().
 *
 * If defragmentation was not necessary, NULL is returned and the operation has
 * no other effect.
 *
 * If a non-NULL value is returned, the caller should use the new pointer instead
 * of the old one and update any reference to the old pointer, which must not
 * be used again.
 */
void *RM_DefragAlloc(RedisModuleDefragCtx *ctx, void *ptr) {
    UNUSED(ctx);
    return activeDefragAlloc(ptr);
}

/* Defrag a RedisModuleString previously allocated by RM_Alloc, RM_Calloc, etc.
 * See RM_DefragAlloc() for more information on how the defragmentation process
 * works.
 *
 * NOTE: It is only possible to defrag strings that have a single reference.
 * Typically this means strings retained with RM_RetainString or RM_HoldString
 * may not be defragmentable. One exception is command argvs which, if retained
 * by the module, will end up with a single reference (because the reference
 * on the Redis side is dropped as soon as the command callback returns).
 */
RedisModuleString *RM_DefragRedisModuleString(RedisModuleDefragCtx *ctx, RedisModuleString *str) {
    UNUSED(ctx);
    return activeDefragStringOb(str);
}


/* Perform a late defrag of a module datatype key.
 *
 * Returns a zero value (and initializes the cursor) if no more needs to be done,
 * or a non-zero value otherwise.
 */
int moduleLateDefrag(robj *key, robj *value, unsigned long *cursor, long long endtime, int dbid) {
    moduleValue *mv = value->ptr;
    moduleType *mt = mv->type;

    RedisModuleDefragCtx defrag_ctx = { endtime, cursor, key, dbid};

    /* Invoke callback. Note that the callback may be missing if the key has been
     * replaced with a different type since our last visit.
     */
    int ret = 0;
    if (mt->defrag)
        ret = mt->defrag(&defrag_ctx, key, &mv->value);

    if (!ret) {
        *cursor = 0;    /* No more work to do */
        return 0;
    }

    return 1;
}

/* Attempt to defrag a module data type value. Depending on complexity,
 * the operation may happen immediately or be scheduled for later.
 *
 * Returns 1 if the operation has been completed or 0 if it needs to
 * be scheduled for late defrag.
 */
int moduleDefragValue(robj *key, robj *value, int dbid) {
    moduleValue *mv = value->ptr;
    moduleType *mt = mv->type;

    /* Try to defrag moduleValue itself regardless of whether or not
     * defrag callbacks are provided.
     */
    moduleValue *newmv = activeDefragAlloc(mv);
    if (newmv) {
        value->ptr = mv = newmv;
    }

    if (!mt->defrag)
        return 1;

    /* Use free_effort to determine complexity of module value, and if
     * necessary schedule it for defragLater instead of quick immediate
     * defrag.
     */
    size_t effort = moduleGetFreeEffort(key, value, dbid);
    if (!effort)
        effort = SIZE_MAX;
    if (effort > server.active_defrag_max_scan_fields) {
        return 0;  /* Defrag later */
    }

    RedisModuleDefragCtx defrag_ctx = { 0, NULL, key, dbid };
    mt->defrag(&defrag_ctx, key, &mv->value);
    return 1;
}

/* Call registered module API defrag functions */
void moduleDefragGlobals(void) {
    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;

    while ((de = dictNext(di)) != NULL) {
        struct RedisModule *module = dictGetVal(de);
        if (!module->defrag_cb)
            continue;
        RedisModuleDefragCtx defrag_ctx = { 0, NULL, NULL, -1};
        module->defrag_cb(&defrag_ctx);
    }
    dictReleaseIterator(di);
}

/* Returns the name of the key currently being processed.
 * There is no guarantee that the key name is always available, so this may return NULL.
 */
const RedisModuleString *RM_GetKeyNameFromDefragCtx(RedisModuleDefragCtx *ctx) {
    return ctx->key;
}

/* Returns the database id of the key currently being processed.
 * There is no guarantee that this info is always available, so this may return -1.
 */
int RM_GetDbIdFromDefragCtx(RedisModuleDefragCtx *ctx) {
    return ctx->dbid;
}

/* Register all the APIs we export. Keep this function at the end of the
 * file so that's easy to seek it to add new entries. */
void moduleRegisterCoreAPI(void) {
    server.moduleapi = dictCreate(&moduleAPIDictType);
    server.sharedapi = dictCreate(&moduleAPIDictType);
    REGISTER_API(Alloc);
    REGISTER_API(TryAlloc);
    REGISTER_API(Calloc);
    REGISTER_API(Realloc);
    REGISTER_API(Free);
    REGISTER_API(Strdup);
    REGISTER_API(CreateCommand);
    REGISTER_API(GetCommand);
    REGISTER_API(CreateSubcommand);
    REGISTER_API(SetCommandInfo);
    REGISTER_API(SetCommandACLCategories);
    REGISTER_API(SetModuleAttribs);
    REGISTER_API(IsModuleNameBusy);
    REGISTER_API(WrongArity);
    REGISTER_API(ReplyWithLongLong);
    REGISTER_API(ReplyWithError);
    REGISTER_API(ReplyWithSimpleString);
    REGISTER_API(ReplyWithArray);
    REGISTER_API(ReplyWithMap);
    REGISTER_API(ReplyWithSet);
    REGISTER_API(ReplyWithAttribute);
    REGISTER_API(ReplyWithNullArray);
    REGISTER_API(ReplyWithEmptyArray);
    REGISTER_API(ReplySetArrayLength);
    REGISTER_API(ReplySetMapLength);
    REGISTER_API(ReplySetSetLength);
    REGISTER_API(ReplySetAttributeLength);
    REGISTER_API(ReplyWithString);
    REGISTER_API(ReplyWithEmptyString);
    REGISTER_API(ReplyWithVerbatimString);
    REGISTER_API(ReplyWithVerbatimStringType);
    REGISTER_API(ReplyWithStringBuffer);
    REGISTER_API(ReplyWithCString);
    REGISTER_API(ReplyWithNull);
    REGISTER_API(ReplyWithBool);
    REGISTER_API(ReplyWithCallReply);
    REGISTER_API(ReplyWithDouble);
    REGISTER_API(ReplyWithBigNumber);
    REGISTER_API(ReplyWithLongDouble);
    REGISTER_API(GetSelectedDb);
    REGISTER_API(SelectDb);
    REGISTER_API(KeyExists);
    REGISTER_API(OpenKey);
    REGISTER_API(GetOpenKeyModesAll);
    REGISTER_API(CloseKey);
    REGISTER_API(KeyType);
    REGISTER_API(ValueLength);
    REGISTER_API(ListPush);
    REGISTER_API(ListPop);
    REGISTER_API(ListGet);
    REGISTER_API(ListSet);
    REGISTER_API(ListInsert);
    REGISTER_API(ListDelete);
    REGISTER_API(StringToLongLong);
    REGISTER_API(StringToULongLong);
    REGISTER_API(StringToDouble);
    REGISTER_API(StringToLongDouble);
    REGISTER_API(StringToStreamID);
    REGISTER_API(Call);
    REGISTER_API(CallReplyProto);
    REGISTER_API(FreeCallReply);
    REGISTER_API(CallReplyInteger);
    REGISTER_API(CallReplyDouble);
    REGISTER_API(CallReplyBigNumber);
    REGISTER_API(CallReplyVerbatim);
    REGISTER_API(CallReplyBool);
    REGISTER_API(CallReplySetElement);
    REGISTER_API(CallReplyMapElement);
    REGISTER_API(CallReplyAttributeElement);
    REGISTER_API(CallReplyPromiseSetUnblockHandler);
    REGISTER_API(CallReplyPromiseAbort);
    REGISTER_API(CallReplyAttribute);
    REGISTER_API(CallReplyType);
    REGISTER_API(CallReplyLength);
    REGISTER_API(CallReplyArrayElement);
    REGISTER_API(CallReplyStringPtr);
    REGISTER_API(CreateStringFromCallReply);
    REGISTER_API(CreateString);
    REGISTER_API(CreateStringFromLongLong);
    REGISTER_API(CreateStringFromULongLong);
    REGISTER_API(CreateStringFromDouble);
    REGISTER_API(CreateStringFromLongDouble);
    REGISTER_API(CreateStringFromString);
    REGISTER_API(CreateStringFromStreamID);
    REGISTER_API(CreateStringPrintf);
    REGISTER_API(FreeString);
    REGISTER_API(StringPtrLen);
    REGISTER_API(AutoMemory);
    REGISTER_API(Replicate);
    REGISTER_API(ReplicateVerbatim);
    REGISTER_API(DeleteKey);
    REGISTER_API(UnlinkKey);
    REGISTER_API(StringSet);
    REGISTER_API(StringDMA);
    REGISTER_API(StringTruncate);
    REGISTER_API(SetExpire);
    REGISTER_API(GetExpire);
    REGISTER_API(SetAbsExpire);
    REGISTER_API(GetAbsExpire);
    REGISTER_API(ResetDataset);
    REGISTER_API(DbSize);
    REGISTER_API(RandomKey);
    REGISTER_API(ZsetAdd);
    REGISTER_API(ZsetIncrby);
    REGISTER_API(ZsetScore);
    REGISTER_API(ZsetRem);
    REGISTER_API(ZsetRangeStop);
    REGISTER_API(ZsetFirstInScoreRange);
    REGISTER_API(ZsetLastInScoreRange);
    REGISTER_API(ZsetFirstInLexRange);
    REGISTER_API(ZsetLastInLexRange);
    REGISTER_API(ZsetRangeCurrentElement);
    REGISTER_API(ZsetRangeNext);
    REGISTER_API(ZsetRangePrev);
    REGISTER_API(ZsetRangeEndReached);
    REGISTER_API(HashSet);
    REGISTER_API(HashGet);
    REGISTER_API(StreamAdd);
    REGISTER_API(StreamDelete);
    REGISTER_API(StreamIteratorStart);
    REGISTER_API(StreamIteratorStop);
    REGISTER_API(StreamIteratorNextID);
    REGISTER_API(StreamIteratorNextField);
    REGISTER_API(StreamIteratorDelete);
    REGISTER_API(StreamTrimByLength);
    REGISTER_API(StreamTrimByID);
    REGISTER_API(IsKeysPositionRequest);
    REGISTER_API(KeyAtPos);
    REGISTER_API(KeyAtPosWithFlags);
    REGISTER_API(IsChannelsPositionRequest);
    REGISTER_API(ChannelAtPosWithFlags);
    REGISTER_API(GetClientId);
    REGISTER_API(GetClientUserNameById);
    REGISTER_API(GetContextFlags);
    REGISTER_API(AvoidReplicaTraffic);
    REGISTER_API(PoolAlloc);
    REGISTER_API(CreateDataType);
    REGISTER_API(ModuleTypeSetValue);
    REGISTER_API(ModuleTypeReplaceValue);
    REGISTER_API(ModuleTypeGetType);
    REGISTER_API(ModuleTypeGetValue);
    REGISTER_API(IsIOError);
    REGISTER_API(SetModuleOptions);
    REGISTER_API(SignalModifiedKey);
    REGISTER_API(SaveUnsigned);
    REGISTER_API(LoadUnsigned);
    REGISTER_API(SaveSigned);
    REGISTER_API(LoadSigned);
    REGISTER_API(SaveString);
    REGISTER_API(SaveStringBuffer);
    REGISTER_API(LoadString);
    REGISTER_API(LoadStringBuffer);
    REGISTER_API(SaveDouble);
    REGISTER_API(LoadDouble);
    REGISTER_API(SaveFloat);
    REGISTER_API(LoadFloat);
    REGISTER_API(SaveLongDouble);
    REGISTER_API(LoadLongDouble);
    REGISTER_API(SaveDataTypeToString);
    REGISTER_API(LoadDataTypeFromString);
    REGISTER_API(LoadDataTypeFromStringEncver);
    REGISTER_API(EmitAOF);
    REGISTER_API(Log);
    REGISTER_API(LogIOError);
    REGISTER_API(_Assert);
    REGISTER_API(LatencyAddSample);
    REGISTER_API(StringAppendBuffer);
    REGISTER_API(TrimStringAllocation);
    REGISTER_API(RetainString);
    REGISTER_API(HoldString);
    REGISTER_API(StringCompare);
    REGISTER_API(GetContextFromIO);
    REGISTER_API(GetKeyNameFromIO);
    REGISTER_API(GetKeyNameFromModuleKey);
    REGISTER_API(GetDbIdFromModuleKey);
    REGISTER_API(GetDbIdFromIO);
    REGISTER_API(GetKeyNameFromOptCtx);
    REGISTER_API(GetToKeyNameFromOptCtx);
    REGISTER_API(GetDbIdFromOptCtx);
    REGISTER_API(GetToDbIdFromOptCtx);
    REGISTER_API(GetKeyNameFromDefragCtx);
    REGISTER_API(GetDbIdFromDefragCtx);
    REGISTER_API(GetKeyNameFromDigest);
    REGISTER_API(GetDbIdFromDigest);
    REGISTER_API(BlockClient);
    REGISTER_API(BlockClientGetPrivateData);
    REGISTER_API(BlockClientSetPrivateData);
    REGISTER_API(BlockClientOnAuth);
    REGISTER_API(UnblockClient);
    REGISTER_API(IsBlockedReplyRequest);
    REGISTER_API(IsBlockedTimeoutRequest);
    REGISTER_API(GetBlockedClientPrivateData);
    REGISTER_API(AbortBlock);
    REGISTER_API(Milliseconds);
    REGISTER_API(MonotonicMicroseconds);
    REGISTER_API(Microseconds);
    REGISTER_API(CachedMicroseconds);
    REGISTER_API(BlockedClientMeasureTimeStart);
    REGISTER_API(BlockedClientMeasureTimeEnd);
    REGISTER_API(GetThreadSafeContext);
    REGISTER_API(GetDetachedThreadSafeContext);
    REGISTER_API(FreeThreadSafeContext);
    REGISTER_API(ThreadSafeContextLock);
    REGISTER_API(ThreadSafeContextTryLock);
    REGISTER_API(ThreadSafeContextUnlock);
    REGISTER_API(DigestAddStringBuffer);
    REGISTER_API(DigestAddLongLong);
    REGISTER_API(DigestEndSequence);
    REGISTER_API(NotifyKeyspaceEvent);
    REGISTER_API(GetNotifyKeyspaceEvents);
    REGISTER_API(SubscribeToKeyspaceEvents);
    REGISTER_API(AddPostNotificationJob);
    REGISTER_API(RegisterClusterMessageReceiver);
    REGISTER_API(SendClusterMessage);
    REGISTER_API(GetClusterNodeInfo);
    REGISTER_API(GetClusterNodesList);
    REGISTER_API(FreeClusterNodesList);
    REGISTER_API(CreateTimer);
    REGISTER_API(StopTimer);
    REGISTER_API(GetTimerInfo);
    REGISTER_API(GetMyClusterID);
    REGISTER_API(GetClusterSize);
    REGISTER_API(GetRandomBytes);
    REGISTER_API(GetRandomHexChars);
    REGISTER_API(BlockedClientDisconnected);
    REGISTER_API(SetDisconnectCallback);
    REGISTER_API(GetBlockedClientHandle);
    REGISTER_API(SetClusterFlags);
    REGISTER_API(CreateDict);
    REGISTER_API(FreeDict);
    REGISTER_API(DictSize);
    REGISTER_API(DictSetC);
    REGISTER_API(DictReplaceC);
    REGISTER_API(DictSet);
    REGISTER_API(DictReplace);
    REGISTER_API(DictGetC);
    REGISTER_API(DictGet);
    REGISTER_API(DictDelC);
    REGISTER_API(DictDel);
    REGISTER_API(DictIteratorStartC);
    REGISTER_API(DictIteratorStart);
    REGISTER_API(DictIteratorStop);
    REGISTER_API(DictIteratorReseekC);
    REGISTER_API(DictIteratorReseek);
    REGISTER_API(DictNextC);
    REGISTER_API(DictPrevC);
    REGISTER_API(DictNext);
    REGISTER_API(DictPrev);
    REGISTER_API(DictCompareC);
    REGISTER_API(DictCompare);
    REGISTER_API(ExportSharedAPI);
    REGISTER_API(GetSharedAPI);
    REGISTER_API(RegisterCommandFilter);
    REGISTER_API(UnregisterCommandFilter);
    REGISTER_API(CommandFilterArgsCount);
    REGISTER_API(CommandFilterArgGet);
    REGISTER_API(CommandFilterArgInsert);
    REGISTER_API(CommandFilterArgReplace);
    REGISTER_API(CommandFilterArgDelete);
    REGISTER_API(Fork);
    REGISTER_API(SendChildHeartbeat);
    REGISTER_API(ExitFromChild);
    REGISTER_API(KillForkChild);
    REGISTER_API(RegisterInfoFunc);
    REGISTER_API(InfoAddSection);
    REGISTER_API(InfoBeginDictField);
    REGISTER_API(InfoEndDictField);
    REGISTER_API(InfoAddFieldString);
    REGISTER_API(InfoAddFieldCString);
    REGISTER_API(InfoAddFieldDouble);
    REGISTER_API(InfoAddFieldLongLong);
    REGISTER_API(InfoAddFieldULongLong);
    REGISTER_API(GetServerInfo);
    REGISTER_API(FreeServerInfo);
    REGISTER_API(ServerInfoGetField);
    REGISTER_API(ServerInfoGetFieldC);
    REGISTER_API(ServerInfoGetFieldSigned);
    REGISTER_API(ServerInfoGetFieldUnsigned);
    REGISTER_API(ServerInfoGetFieldDouble);
    REGISTER_API(GetClientInfoById);
    REGISTER_API(GetClientNameById);
    REGISTER_API(SetClientNameById);
    REGISTER_API(PublishMessage);
    REGISTER_API(PublishMessageShard);
    REGISTER_API(SubscribeToServerEvent);
    REGISTER_API(SetLRU);
    REGISTER_API(GetLRU);
    REGISTER_API(SetLFU);
    REGISTER_API(GetLFU);
    REGISTER_API(BlockClientOnKeys);
    REGISTER_API(BlockClientOnKeysWithFlags);
    REGISTER_API(SignalKeyAsReady);
    REGISTER_API(GetBlockedClientReadyKey);
    REGISTER_API(GetUsedMemoryRatio);
    REGISTER_API(MallocSize);
    REGISTER_API(MallocUsableSize);
    REGISTER_API(MallocSizeString);
    REGISTER_API(MallocSizeDict);
    REGISTER_API(ScanCursorCreate);
    REGISTER_API(ScanCursorDestroy);
    REGISTER_API(ScanCursorRestart);
    REGISTER_API(Scan);
    REGISTER_API(ScanKey);
    REGISTER_API(CreateModuleUser);
    REGISTER_API(SetContextUser);
    REGISTER_API(SetModuleUserACL);
    REGISTER_API(SetModuleUserACLString);
    REGISTER_API(GetModuleUserACLString);
    REGISTER_API(GetCurrentUserName);
    REGISTER_API(GetModuleUserFromUserName);
    REGISTER_API(ACLCheckCommandPermissions);
    REGISTER_API(ACLCheckKeyPermissions);
    REGISTER_API(ACLCheckChannelPermissions);
    REGISTER_API(ACLAddLogEntry);
    REGISTER_API(ACLAddLogEntryByUserName);
    REGISTER_API(FreeModuleUser);
    REGISTER_API(DeauthenticateAndCloseClient);
    REGISTER_API(AuthenticateClientWithACLUser);
    REGISTER_API(AuthenticateClientWithUser);
    REGISTER_API(GetContextFlagsAll);
    REGISTER_API(GetModuleOptionsAll);
    REGISTER_API(GetKeyspaceNotificationFlagsAll);
    REGISTER_API(IsSubEventSupported);
    REGISTER_API(GetServerVersion);
    REGISTER_API(GetClientCertificate);
    REGISTER_API(RedactClientCommandArgument);
    REGISTER_API(GetCommandKeys);
    REGISTER_API(GetCommandKeysWithFlags);
    REGISTER_API(GetCurrentCommandName);
    REGISTER_API(GetTypeMethodVersion);
    REGISTER_API(RegisterDefragFunc);
    REGISTER_API(DefragAlloc);
    REGISTER_API(DefragRedisModuleString);
    REGISTER_API(DefragShouldStop);
    REGISTER_API(DefragCursorSet);
    REGISTER_API(DefragCursorGet);
    REGISTER_API(EventLoopAdd);
    REGISTER_API(EventLoopDel);
    REGISTER_API(EventLoopAddOneShot);
    REGISTER_API(Yield);
    REGISTER_API(RegisterBoolConfig);
    REGISTER_API(RegisterNumericConfig);
    REGISTER_API(RegisterStringConfig);
    REGISTER_API(RegisterEnumConfig);
    REGISTER_API(LoadConfigs);
    REGISTER_API(RegisterAuthCallback);
}

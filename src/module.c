#include "server.h"
#include "cluster.h"
#include <dlfcn.h>

#define REDISMODULE_CORE 1
#include "redismodule.h"

/* --------------------------------------------------------------------------
 * Private data structures used by the modules system. Those are data
 * structures that are never exposed to Redis Modules, if not as void
 * pointers that have an API the module can call with them)
 * -------------------------------------------------------------------------- */

/* This structure represents a module inside the system. */
struct RedisModule {
    void *handle;   /* Module dlopen() handle. */
    char *name;     /* Module name. */
    int ver;        /* Module version. We use just progressive integers. */
    int apiver;     /* Module API version as requested during initialization.*/
};
typedef struct RedisModule RedisModule;

static dict *modules; /* Hash table of modules. SDS -> RedisModule ptr.*/

/* Entries in the context->amqueue array, representing objects to free
 * when the callback returns. */
struct AutoMemEntry {
    void *ptr;
    int type;
};

/* AutMemEntry type field values. */
#define REDISMODULE_AM_KEY 0
#define REDISMODULE_AM_STRING 1
#define REDISMODULE_AM_REPLY 2
#define REDISMODULE_AM_FREED 3 /* Explicitly freed by user already. */

/* This structure represents the context in which Redis modules operate.
 * Most APIs module can access, get a pointer to the context, so that the API
 * implementation can hold state across calls, or remember what to free after
 * the call and so forth.
 *
 * Note that not all the context structure is always filled with actual values
 * but only the fields needed in a given context. */
struct RedisModuleCtx {
    void *getapifuncptr;            /* NOTE: Must be the first field. */
    struct RedisModule *module;     /* Module reference. */
    client *client;                 /* Client calling a command. */
    struct AutoMemEntry *amqueue;   /* Auto memory queue of objects to free. */
    int amqueue_len;                /* Number of slots in amqueue. */
    int amqueue_used;               /* Number of used slots in amqueue. */
    int flags;                      /* REDISMODULE_CTX_... flags. */
};
typedef struct RedisModuleCtx RedisModuleCtx;

#define REDISMODULE_CTX_INIT {(void*)(unsigned long)&RM_GetApi, NULL, NULL, NULL, 0, 0, 0}
#define REDISMODULE_CTX_MULTI_EMITTED (1<<0)
#define REDISMODULE_CTX_AUTO_MEMORY (1<<1)

/* This represents a Redis key opened with RM_OpenKey(). */
struct RedisModuleKey {
    RedisModuleCtx *ctx;
    redisDb *db;
    robj *key;      /* Key name object. */
    robj *value;    /* Value object, or NULL if the key was not found. */
    void *iter;     /* Iterator. */
    int mode;       /* Opening mode. */
    /* Zset iterator. */
    RedisModuleZsetRange *zr;   /* Zset iterator range passed by user. */
    void *zcurrent; /* Zset iterator current node. */
    int zer; /* Zset iterator end reached flag (true if end was reached). */
};
typedef struct RedisModuleKey RedisModuleKey;

/* Function pointer type of a function representing a command inside
 * a Redis module. */
typedef int (*RedisModuleCmdFunc) (RedisModuleCtx *ctx, void **argv, int argc);

/* This struct holds the information about a command registered by a module.*/
struct RedisModuleCommandProxy {
    struct RedisModule *module;
    RedisModuleCmdFunc func;
    struct redisCommand *rediscmd;
};
typedef struct RedisModuleCommandProxy RedisModuleCommandProxy;

#define REDISMODULE_REPLYFLAG_NONE 0
#define REDISMODULE_REPLYFLAG_TOPARSE (1<<0) /* Protocol must be parsed. */
#define REDISMODULE_REPLYFLAG_NESTED (1<<1)  /* Nested reply object. No proto
                                                or struct free. */

/* Reply of RM_Call() function. The function is filled in a lazy
 * way depending on the function called on the reply structure. By default
 * only the type, proto and protolen are filled. */
struct RedisModuleCallReply {
    RedisModuleCtx *ctx;
    int type;       /* REDISMODULE_REPLY_... */
    int flags;      /* REDISMODULE_REPLYFLAG_...  */
    size_t len;     /* Len of strings or num of elements of arrays. */
    char *proto;    /* Raw reply protocol. An SDS string at top-level object. */
    size_t protolen;/* Length of protocol. */
    union {
        const char *str; /* String pointer for string and error replies. This
                            does not need to be freed, always points inside
                            a reply->proto buffer of the reply object or, in
                            case of array elements, of parent reply objects. */
        long long ll;    /* Reply value for integer reply. */
        struct RedisModuleCallReply *array; /* Array of sub-reply elements. */
    } val;
};
typedef struct RedisModuleCallReply RedisModuleCallReply;

/* --------------------------------------------------------------------------
 * Prototypes
 * -------------------------------------------------------------------------- */

void RM_FreeCallReply(RedisModuleCallReply *reply);
void RM_CloseKey(RedisModuleKey *key);
void RM_AutoMemoryCollect(RedisModuleCtx *ctx);
robj **moduleCreateArgvFromUserFormat(const char *cmdname, const char *fmt, int *argcp, int *flags, va_list ap);
void moduleReplicateMultiIfNeeded(RedisModuleCtx *ctx);
void RM_ZsetRangeStop(RedisModuleKey *key);

/* --------------------------------------------------------------------------
 * Helpers for modules API implementation
 * -------------------------------------------------------------------------- */

/* Create an empty key of the specified type. 'kp' must point to a key object
 * opened for writing where the .value member is set to NULL because the
 * key was found to be non existing.
 *
 * On success REDISMODULE_OK is returned and the key is populated with
 * the value of the specified type. The function fails and returns
 * REDISMODULE_ERR if:
 *
 * 1) The key is not open for writing.
 * 2) The key is not empty.
 * 3) The specified type is unknown.
 */
int moduleCreateEmtpyKey(RedisModuleKey *key, int type) {
    robj *obj;

    /* The key must be open for writing and non existing to proceed. */
    if (!(key->mode & REDISMODULE_WRITE) || key->value)
        return REDISMODULE_ERR;

    switch(type) {
    case REDISMODULE_KEYTYPE_LIST:
        obj = createQuicklistObject();
        quicklistSetOptions(obj->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);
        break;
    case REDISMODULE_KEYTYPE_ZSET:
        obj = createZsetZiplistObject();
        break;
    default: return REDISMODULE_ERR;
    }
    dbAdd(key->db,key->key,obj);
    key->value = obj;
    return REDISMODULE_OK;
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
    case OBJ_HASH : isempty = hashTypeLength(o) == 0; break;
    default: isempty = 0;
    }

    if (isempty) {
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

/* Lookup the requested module API and store the function pointer into the
 * target pointer. The function returns REDISMODULE_ERR if there is no such
 * named API, otherwise REDISMODULE_OK. */
int RM_GetApi(const char *funcname, void **targetPtrPtr) {
    dictEntry *he = dictFind(server.moduleapi, funcname);
    if (!he) return REDISMODULE_ERR;
    *targetPtrPtr = dictGetVal(he);
    return REDISMODULE_OK;
}

/* This Redis command binds the normal Redis command invocation with commands
 * exported by modules. */
void RedisModuleCommandDispatcher(client *c) {
    RedisModuleCommandProxy *cp = (void*)(unsigned long)c->cmd->getkeys_proc;
    RedisModuleCtx ctx = REDISMODULE_CTX_INIT;

    ctx.module = cp->module;
    ctx.client = c;
    cp->func(&ctx,(void**)c->argv,c->argc);
    RM_AutoMemoryCollect(&ctx);
    preventCommandPropagation(c);

    /* Handle the replication of the final EXEC, since whatever a command
     * emits is always wrappered around MULTI/EXEC. */
    if (ctx.flags & REDISMODULE_CTX_MULTI_EMITTED) {
        robj *propargv[1];
        propargv[0] = createStringObject("EXEC",4);
        alsoPropagate(server.execCommand,c->db->id,propargv,1,
            PROPAGATE_AOF|PROPAGATE_REPL);
        decrRefCount(propargv[0]);
    }
}

/* Register a new command in the Redis server, that will be handled by
 * calling the function pointer 'func' using the RedisModule calling
 * convention. The function returns REDISMODULE_ERR if the specified command
 * name is already busy, otherwise REDISMODULE_OK. */
int RM_CreateCommand(RedisModuleCtx *ctx, const char *name, RedisModuleCmdFunc cmdfunc) {
    struct redisCommand *rediscmd;
    RedisModuleCommandProxy *cp;
    sds cmdname = sdsnew(name);

    /* Check if the command name is busy. */
    if (lookupCommand((char*)name) != NULL) {
        sdsfree(cmdname);
        return REDISMODULE_ERR;
    }

    /* Create a command "proxy", which is a structure that is referenced
     * in the command table, so that the generic command that works as
     * binidng between modules and Redis, can know what function to call
     * and what the module is.
     *
     * Note that we use the Redis command table 'getkeys_proc' in order to
     * pass a reference to the command proxy structure. */
    cp = zmalloc(sizeof(*cp));
    cp->module = ctx->module;
    cp->func = cmdfunc;
    cp->rediscmd = zmalloc(sizeof(*rediscmd));
    cp->rediscmd->name = cmdname;
    cp->rediscmd->proc = RedisModuleCommandDispatcher;
    cp->rediscmd->arity = -1;
    cp->rediscmd->flags = 0;
    cp->rediscmd->getkeys_proc = (redisGetKeysProc*)(unsigned long)cp;
    cp->rediscmd->firstkey = 1;
    cp->rediscmd->lastkey = 1;
    cp->rediscmd->keystep = 1;
    cp->rediscmd->microseconds = 0;
    cp->rediscmd->calls = 0;
    dictAdd(server.commands,sdsdup(cmdname),cp->rediscmd);
    dictAdd(server.orig_commands,sdsdup(cmdname),cp->rediscmd);
    return REDISMODULE_OK;
}

/* Called by RM_Init() to setup the ctx->module structure. */
void RM_SetModuleAttribs(RedisModuleCtx *ctx, const char *name, int ver, int apiver){
    RedisModule *module;

    if (ctx->module != NULL) return;
    module = zmalloc(sizeof(*module));
    module->name = sdsnew((char*)name);
    module->ver = ver;
    module->apiver = apiver;
    ctx->module = module;
}

/* --------------------------------------------------------------------------
 * Automatic memory management for modules
 * -------------------------------------------------------------------------- */

/* Enable auto memory. */
void RM_AutoMemory(RedisModuleCtx *ctx) {
    ctx->flags |= REDISMODULE_CTX_AUTO_MEMORY;
}

/* Add a new object to release automatically when the callback returns. */
void RM_AutoMemoryAdd(RedisModuleCtx *ctx, int type, void *ptr) {
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
 * free things manually if they want. */
void RM_AutoMemoryFreed(RedisModuleCtx *ctx, int type, void *ptr) {
    if (!(ctx->flags & REDISMODULE_CTX_AUTO_MEMORY)) return;

    int j;
    for (j = 0; j < ctx->amqueue_used; j++) {
        if (ctx->amqueue[j].type == type &&
            ctx->amqueue[j].ptr == ptr)
        {
            ctx->amqueue[j].type = REDISMODULE_AM_FREED;
            /* Optimization: if this is the last element, we can
             * reuse it. */
            if (j == ctx->amqueue_used-1) ctx->amqueue_used--;
        }
    }
}

/* Release all the objects in queue. */
void RM_AutoMemoryCollect(RedisModuleCtx *ctx) {
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
        }
    }
    ctx->flags |= REDISMODULE_CTX_AUTO_MEMORY;
    zfree(ctx->amqueue);
    ctx->amqueue = NULL;
    ctx->amqueue_len = 0;
    ctx->amqueue_used = 0;
}

/* --------------------------------------------------------------------------
 * String objects APIs
 * -------------------------------------------------------------------------- */

/* Create a new module string object. Must be freed with
 * RM_FreeString(), unless automatic memory is enabled. */
RedisModuleString *RM_CreateString(RedisModuleCtx *ctx, const char *ptr, size_t len)
{
    RedisModuleString *o = createStringObject(ptr,len);
    RM_AutoMemoryAdd(ctx,REDISMODULE_AM_STRING,o);
    return o;
}

/* Like RM_CreatString, but creates a string starting from a long long
 * integer instea of taking a buffer and length. */
RedisModuleString *RM_CreateStringFromLongLong(RedisModuleCtx *ctx, long long ll) {
    char buf[LONG_STR_SIZE];
    size_t len = ll2string(buf,sizeof(buf),ll);
    return RM_CreateString(ctx,buf,len);
}

/* Free a module string object obtained with one of the Redis API calls
 * that return new string objects. */
void RM_FreeString(RedisModuleCtx *ctx, RedisModuleString *str) {
    decrRefCount(str);
    RM_AutoMemoryFreed(ctx,REDISMODULE_AM_STRING,str);
}

/* Return the string pointer and length. */
const char *RM_StringPtrLen(RedisModuleString *str, size_t *len) {
    if (len) *len = sdslen(str->ptr);
    return str->ptr;
}

/* Turn the string into a long long, storing it at *ll.
 * Returns REDISMODULE_OK on success. If the string can't be parsed
 * as a valid, strict long long (no spaces before/after), REDISMODULE_ERR
 * is returned. */
int RM_StringToLongLong(RedisModuleString *str, long long *ll) {
    return string2ll(str->ptr,sdslen(str->ptr),ll) ? REDISMODULE_OK :
                                                     REDISMODULE_ERR;
}

/* Turn the string into a double, storing it at *d.
 * Returns REDISMODULE_OK on success or REDISMODULE_ERR if the string is
 * not a valid string representation of a double value. */
int RM_StringToDouble(RedisModuleString *str, double *d) {
    int retval = getDoubleFromObject(str,d);
    return (retval == C_OK) ? REDISMODULE_OK : REDISMODULE_ERR;
}

/* --------------------------------------------------------------------------
 * Reply APIs
 *
 * Most functions always return REDISMODULE_OK so you can use it with
 * 'return' in order to return from the command implementation with:
 *
 * if (... some condition ...)
 *     return RM_ReplyWithLongLong(ctx,mycount);
 * -------------------------------------------------------------------------- */

/* Send an error about the number of arguments given to the command. */
int RM_WrongArity(RedisModuleCtx *ctx) {
    addReplyErrorFormat(ctx->client,
        "wrong number of arguments for '%s' command",
        (char*)ctx->client->argv[0]->ptr);
    return REDISMODULE_OK;
}

/* Send an integer reply with the specified long long value.
 * The function always returns REDISMODULE_OK. */
int RM_ReplyWithLongLong(RedisModuleCtx *ctx, long long ll) {
    addReplyLongLong(ctx->client,ll);
    return REDISMODULE_OK;
}

/* Reply with an error or simple string (status message). Used to implement
 * ReplyWithSimpleString() and ReplyWithError(). */
int RM_ReplyWithStatus(RedisModuleCtx *ctx, const char *msg, char *prefix) {
    sds strmsg = sdsnewlen(prefix,1);
    strmsg = sdscat(strmsg,msg);
    strmsg = sdscatlen(strmsg,"\r\n",2);
    addReplySds(ctx->client,strmsg);
    return REDISMODULE_OK;
}

/* Reply with the error 'err'.
 *
 * Note that 'err' must contain all the error, including
 * the initial error code. The function only provides the initial "-", so
 * the usage is, for example:
 *
 * RM_ReplyWithError(ctx,"ERR Wrong Type");
 *
 * and not just:
 *
 * RM_ReplyWithError(ctx,"Wrong Type");
 */
int RM_ReplyWithError(RedisModuleCtx *ctx, const char *err) {
    return RM_ReplyWithStatus(ctx,err,"-");
}

/* Reply with a simple string (+... \r\n in RESP protocol). This replies
 * are suitalbe only when sending a small non-binary string wiht small
 * overhead, like "OK" or similar replies. */
int RM_ReplyWithSimpleString(RedisModuleCtx *ctx, const char *msg) {
    return RM_ReplyWithStatus(ctx,msg,"+");
}

/* Reply with an array type of 'len' elements. However 'len' other calls
 * to ReplyWith* style functions must follow in order to emit the elements
 * of the array. */
int RM_ReplyWithArray(RedisModuleCtx *ctx, int len) {
    addReplyMultiBulkLen(ctx->client,len);
    return REDISMODULE_OK;
}

/* Reply with a bulk string, taking in input a C buffer pointer and length. */
int RM_ReplyWithStringBuffer(RedisModuleCtx *ctx, const char *buf, size_t len) {
    addReplyBulkCBuffer(ctx->client,(char*)buf,len);
    return REDISMODULE_OK;
}

/* Reply with a bulk string, taking in input a RedisModuleString object. */
int RM_ReplyWithString(RedisModuleCtx *ctx, RedisModuleString *str) {
    addReplyBulk(ctx->client,str);
    return REDISMODULE_OK;
}

/* Reply with NULL. */
int RM_ReplyWithNull(RedisModuleCtx *ctx) {
    addReply(ctx->client,shared.nullbulk);
    return REDISMODULE_OK;
}

/* Reply exactly what a Redis command returned us with RM_Call(). */
int RM_ReplyWithCallReply(RedisModuleCtx *ctx, RedisModuleCallReply *reply) {
    sds proto = sdsnewlen(reply->proto, reply->protolen);
    addReplySds(ctx->client,proto);
    return REDISMODULE_OK;
}

/* Send a string reply obtained converting the double 'd' into a string. */
int RM_ReplyWithDouble(RedisModuleCtx *ctx, double d) {
    addReplyDouble(ctx->client,d);
    return REDISMODULE_OK;
}

/* --------------------------------------------------------------------------
 * Commands replication API
 * -------------------------------------------------------------------------- */

/* Helper function to replicate MULTI the first time we replicate something
 * in the context of a command execution. EXEC will be handled by the
 * RedisModuleCommandDispatcher() function. */
void moduleReplicateMultiIfNeeded(RedisModuleCtx *ctx) {
    if (ctx->flags & REDISMODULE_CTX_MULTI_EMITTED) return;
    execCommandPropagateMulti(ctx->client);
    ctx->flags |= REDISMODULE_CTX_MULTI_EMITTED;
}

/* Replicate the specified command and arguments to slaves and AOF, as effect
 * of execution of the calling command implementation.
 *
 * The replicated commands are always wrapepd into the MULTI/EXEC that
 * contains all the commands replicated in a given module command
 * execution. However the commands replicated with RM_Call()
 * are the first items, the ones replicated with RM_Replicate()
 * will all follow before the EXEC.
 *
 * Modules should try to use one interface or the other. */
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

    /* Replicate! */
    moduleReplicateMultiIfNeeded(ctx);
    alsoPropagate(cmd,ctx->client->db->id,argv,argc,
        PROPAGATE_AOF|PROPAGATE_REPL);

    /* Release the argv. */
    for (j = 0; j < argc; j++) decrRefCount(argv[j]);
    zfree(argv);
    return REDISMODULE_OK;
}

/* This function will replicate the command exactly as it was invoked
 * by the client. This function will not wrap the command into
 * a MULTI/EXEC stanza, so it should not be mixed with other replication
 * commands. */
int RM_ReplicateVerbatim(RedisModuleCtx *ctx) {
    alsoPropagate(ctx->client->cmd,ctx->client->db->id,
        ctx->client->argv,ctx->client->argc,
        PROPAGATE_AOF|PROPAGATE_REPL);
    return REDISMODULE_OK;
}

/* --------------------------------------------------------------------------
 * DB and Key APIs -- Generic API
 * -------------------------------------------------------------------------- */

/* Return the currently selected DB. */
int RM_GetSelectedDb(RedisModuleCtx *ctx) {
    return ctx->client->db->id;
}

/* Change the currently selected DB. Returns an error if the id
 * is out of range. */
int RM_SelectDb(RedisModuleCtx *ctx, int newid) {
    int retval = selectDb(ctx->client,newid);
    return (retval == C_OK) ? REDISMODULE_OK : REDISMODULE_ERR;
}

/* Return an handle representing a Redis key, so that it is possible
 * to call other APIs with the key handle as argument to perform
 * operations on the key.
 *
 * The return value is the handle repesenting the key, that must be
 * closed with RM_CloseKey().
 *
 * If the key does not exist and WRITE mode is requested, the handle
 * is still returned, since it is possible to perform operations on
 * a yet not existing key (that will be created, for example, after
 * a list push operation). If the mode is just READ instead, and the
 * key does not exist, NULL is returned. However it is still safe to
 * call RM_CloseKey() and RM_KeyType() on a NULL
 * value. */
void *RM_OpenKey(RedisModuleCtx *ctx, robj *keyname, int mode) {
    RedisModuleKey *kp;
    robj *value;

    if (mode & REDISMODULE_WRITE) {
        value = lookupKeyWrite(ctx->client->db,keyname);
    } else {
        value = lookupKeyRead(ctx->client->db,keyname);
        if (value == NULL) {
            return NULL;
        }
    }

    /* Setup the key handle. */
    kp = zmalloc(sizeof(*kp));
    kp->ctx = ctx;
    kp->db = ctx->client->db;
    kp->key = keyname;
    incrRefCount(keyname);
    kp->value = value;
    kp->iter = NULL;
    kp->mode = mode;
    RM_ZsetRangeStop(kp);
    RM_AutoMemoryAdd(ctx,REDISMODULE_AM_KEY,kp);
    return (void*)kp;
}

/* Close a key handle. */
void RM_CloseKey(RedisModuleKey *key) {
    if (key == NULL) return;
    if (key->mode & REDISMODULE_WRITE) signalModifiedKey(key->db,key->key);
    /* TODO: if (key->iter) RM_KeyIteratorStop(kp); */
    decrRefCount(key->key);
    RM_AutoMemoryFreed(key->ctx,REDISMODULE_AM_KEY,key);
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
    default: return 0;
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

/* Return the key expire value, as milliseconds of remaining TTL.
 * If no TTL is associated with the key or if the key is empty,
 * REDISMODULE_NO_EXPIRE is returned. */
mstime_t RM_GetExpire(RedisModuleKey *key) {
    mstime_t expire = getExpire(key->db,key->key);
    if (expire == -1 || key->value == NULL) return -1;
    expire -= mstime();
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
    if (!(key->mode & REDISMODULE_WRITE) || key->value == NULL)
        return REDISMODULE_ERR;
    if (expire != REDISMODULE_NO_EXPIRE) {
        expire += mstime();
        setExpire(key->db,key->key,expire);
    } else {
        removeExpire(key->db,key->key);
    }
    return REDISMODULE_OK;
}

/* --------------------------------------------------------------------------
 * Key API for String type
 * -------------------------------------------------------------------------- */

/* If the key is open for writing, set the specified string 'str' as the
 * value of the key, deleting the old value if any.
 * On success REDISMODULE_OK is returned. If the key is not open for
 * writing or there is an active iterator, REDISMODULE_ERR is returned. */
int RM_StringSet(RedisModuleKey *key, RedisModuleString *str) {
    if (!(key->mode & REDISMODULE_WRITE) || key->iter) return REDISMODULE_ERR;
    RM_DeleteKey(key);
    setKey(key->db,key->key,str);
    key->value = str;
    return REDISMODULE_OK;
}

/* Prepare the key associated string value for DMA access, and returns
 * a pointer and size (by reference), that the user can use to read or
 * modify the string in-place accessing it directly via pointer.
 *
 * The 'mode' is composed by bitwise OR-ing the following flags:
 *
 * REDISMODULE_READ -- Read access
 * REDISMODULE_WRITE -- WRite access
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

/* If the string is open for writing and is of string type, resize it, padding
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

    /* Empty key: fill it with a zero-length key so that we can handle the
     * resize with a common code path. */
    if (key->value == NULL) {
        robj *emptyobj = createStringObject("",0);
        setKey(key->db,key->key,emptyobj);
        key->value = emptyobj;
        decrRefCount(emptyobj);
    }

    /* Unshare and resize. */
    key->value = dbUnshareStringValue(key->db, key->key, key->value);
    size_t curlen = sdslen(key->value->ptr);
    if (newlen > curlen) {
        key->value->ptr = sdsgrowzero(key->value->ptr,newlen);
    } else if (newlen < curlen) {
        sdsrange(key->value->ptr,0,newlen-1);
        /* If the string is too wasteful, reallocate it. */
        if (sdslen(key->value->ptr) < sdsavail(key->value->ptr))
            key->value->ptr = sdsRemoveFreeSpace(key->value->ptr);
    }
    return REDISMODULE_OK;
}

/* --------------------------------------------------------------------------
 * Key API for List type
 * -------------------------------------------------------------------------- */

/* Push an element into a list, on head or tail depending on 'where' argumnet.
 * If the key pointer is about an empty key opened for writing, the key
 * is created. On error (key opened for read-only operations or of the wrong
 * type) REDISMODULE_ERR is returned, otherwise REDISMODULE_OK is returned. */
int RM_ListPush(RedisModuleKey *key, int where, RedisModuleString *ele) {
    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value == NULL) moduleCreateEmtpyKey(key,REDISMODULE_KEYTYPE_LIST);
    if (key->value->type != OBJ_LIST) return REDISMODULE_ERR;
    listTypePush(key->value, ele,
        (where == REDISMODULE_LIST_HEAD) ? QUICKLIST_HEAD : QUICKLIST_TAIL);
    return REDISMODULE_OK;
}

/* Pop an element from the list, and returns it as a module string object
 * that the user should be free with RM_FreeString() or by enabling
 * automatic memory. 'where' specifies if the element should be popped from
 * head or tail. The command returns NULL if:
 * 1) The list is empty.
 * 2) The key was not open for writing.
 * 3) The key is not a list. */
RedisModuleString *RM_ListPop(RedisModuleKey *key, int where) {
    if (!(key->mode & REDISMODULE_WRITE) ||
        key->value == NULL ||
        key->value->type != OBJ_LIST) return NULL;
    robj *ele = listTypePop(key->value,
        (where == REDISMODULE_LIST_HEAD) ? QUICKLIST_HEAD : QUICKLIST_TAIL);
    robj *decoded = getDecodedObject(ele);
    decrRefCount(ele);
    moduleDelKeyIfEmpty(key);
    RM_AutoMemoryAdd(key->ctx,REDISMODULE_AM_STRING,decoded);
    return decoded;
}

/* --------------------------------------------------------------------------
 * Key API for Sorted Set type
 * -------------------------------------------------------------------------- */

/* Conversion from/to public flags of the Modules API and our private flags,
 * so that we have everything decoupled. */
int RM_ZsetAddFlagsToCoreFlags(int flags) {
    int retflags = 0;
    if (flags & REDISMODULE_ZADD_XX) retflags |= ZADD_XX;
    if (flags & REDISMODULE_ZADD_NX) retflags |= ZADD_NX;
    return retflags;
}

/* See previous function comment. */
int RM_ZsetAddFlagsFromCoreFlags(int flags) {
    int retflags = 0;
    if (flags & ZADD_ADDED) retflags |= REDISMODULE_ZADD_ADDED;
    if (flags & ZADD_UPDATED) retflags |= REDISMODULE_ZADD_UPDATED;
    if (flags & ZADD_NOP) retflags |= REDISMODULE_ZADD_NOP;
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
 * REDISMODULE_ZADD_XX: Element must already exist. Do nothing otherwise.
 * REDISMODULE_ZADD_NX: Element must not exist. Do nothing otherwise.
 *
 * The output flags are:
 *
 * REDISMODULE_ZADD_ADDED: The new element was added to the sorted set.
 * REDISMODULE_ZADD_UPDATED: The score of the element was updated.
 * REDISMODULE_ZADD_NOP: No operation was performed because XX or NX flags.
 *
 * On success the function returns REDISMODULE_OK. On the following errors
 * REDISMODULE_ERR is returned:
 *
 * - The key was not opened for writing.
 * - The key is of the wrong type.
 * - 'score' double value is not a number (NaN).
 */
int RM_ZsetAdd(RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr) {
    int flags = 0;
    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value->type != OBJ_ZSET) return REDISMODULE_ERR;
    if (key->value == NULL) moduleCreateEmtpyKey(key,REDISMODULE_KEYTYPE_ZSET);
    if (flagsptr) flags = RM_ZsetAddFlagsToCoreFlags(*flagsptr);
    if (zsetAdd(key->value,score,ele->ptr,&flags,NULL) == 0) {
        if (flagsptr) *flagsptr = 0;
        return REDISMODULE_ERR;
    }
    if (flagsptr) *flagsptr = RM_ZsetAddFlagsFromCoreFlags(flags);
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
 * to the existing score resuts into a NaN (not a number) condition.
 *
 * This function has an additional field 'newscore', if not NULL is filled
 * with the new score of the element after the increment, if no error
 * is returned. */
int RM_ZsetIncrby(RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr, double *newscore) {
    int flags = 0;
    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value->type != OBJ_ZSET) return REDISMODULE_ERR;
    if (key->value == NULL) moduleCreateEmtpyKey(key,REDISMODULE_KEYTYPE_ZSET);
    if (flagsptr) flags = RM_ZsetAddFlagsToCoreFlags(*flagsptr);
    if (zsetAdd(key->value,score,ele->ptr,&flags,newscore) == 0) {
        if (flagsptr) *flagsptr = 0;
        return REDISMODULE_ERR;
    }
    /* zsetAdd() may signal back that the resulting score is not a number. */
    if (flagsptr && (*flagsptr & ZADD_NAN)) {
        *flagsptr = 0;
        return REDISMODULE_ERR;
    }
    if (flagsptr) *flagsptr = RM_ZsetAddFlagsFromCoreFlags(flags);
    return REDISMODULE_OK;
}

/* Remove the specified element from the sorted set.
 * The function returns REDISMODULE_OK on success, and REDISMODULE_ERR
 * on one of the following conditions:
 *
 * - The key was not opened for writing.
 * - The key is of the wrong type.
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
    if (key->value->type != OBJ_ZSET) return REDISMODULE_ERR;
    if (key->value != NULL && zsetDel(key->value,ele->ptr)) {
        if (deleted) *deleted = 1;
    } else {
        if (deleted) *deleted = 0;
    }
    return REDISMODULE_OK;
}

/* On success retrieve the double score associated at the sorted set element
 * 'ele' and returns REDISMODULE_OK. Otherwise REDISMODULE_ERR is returned
 * to signal one of the following conditions:
 *
 * - There is no such element 'ele' in the sorted set.
 * - The key is not a sorted set.
 * - The key is an open empty key.
 */
int RM_ZsetScore(RedisModuleKey *key, RedisModuleString *ele, double *score) {
    if (key->value->type != OBJ_ZSET) return REDISMODULE_ERR;
    if (key->value == NULL) return REDISMODULE_ERR;
    if (zsetScore(key->value,ele->ptr,score) == C_ERR) return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

/* --------------------------------------------------------------------------
 * Key API for Sorted Set iterator
 * -------------------------------------------------------------------------- */

/* Stop a sorted set iteration. */
void RM_ZsetRangeStop(RedisModuleKey *key) {
    /* Setup sensible values so that misused iteration API calls when an
     * iterator is not active will result into something more sensible
     * than crashing. */
    key->zr = NULL;
    key->zcurrent = NULL;
    key->zer = 1;
}

/* Return the "End of range" flag value to signal the end of the iteration. */
int RM_ZsetRangeEndReached(RedisModuleKey *key) {
    return key->zer;
}

/* Helper function for RM_ZsetFirstInRange() and RM_ZsetLastInRange().
 * Setup the sorted set iteration according to the specified range
 * (see the functions calling it for more info). If first is true the
 * first element in the range is used as a starting point for the iterator
 * otherwise the last. Return REDISMODULE_OK on success otherwise
 * REDISMODULE_ERR. */
int zsetInitRange(RedisModuleKey *key, RedisModuleZsetRange *zr, int first) {
    if (!key->value || key->value->type != OBJ_ZSET) return REDISMODULE_ERR;
    key->zr = zr;
    key->zcurrent = NULL;
    key->zer = 0;

    if (zr->type == REDISMODULE_ZSET_RANGE_SCORE) {
        /* Setup the range structure used by the sorted set core implementation
         * in order to seek at the specified element. */
        zrangespec zrs;
        zrs.min = zr->score_start;
        zrs.max = zr->score_end;
        zrs.minex = (zr->flags & REDISMODULE_ZSET_RANGE_START_EX) != 0;
        zrs.maxex = (zr->flags & REDISMODULE_ZSET_RANGE_END_EX) != 0;

        if (key->value->encoding == OBJ_ENCODING_ZIPLIST) {
            key->zcurrent = first ? zzlFirstInRange(key->value->ptr,&zrs) :
                                    zzlLastInRange(key->value->ptr,&zrs);
        } else if (key->value->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = key->value->ptr;
            zskiplist *zsl = zs->zsl;
            key->zcurrent = first ? zslFirstInRange(zsl,&zrs) :
                                    zslLastInRange(zsl,&zrs);
        } else {
            serverPanic("Unsupported zset encoding");
        }
        if (key->zcurrent == NULL) key->zer = 1;
        return REDISMODULE_OK;
    } else {
        return REDISMODULE_ERR;
    }
}

/* Setup a sorted set iterator seeking the first element in the specified
 * range. Returns REDISMODULE_OK if the iterator was correctly initialized
 * otherwise REDISMODULE_ERR is returned in the following conditions:
 *
 * 1. The value stored at key is not a sorted set or the key is empty.
 * 2. The iterator type is unrecognized. */
int RM_ZsetFirstInRange(RedisModuleKey *key, RedisModuleZsetRange *zr) {
    return zsetInitRange(key,zr,1);
}

/* Exactly like RM_ZsetFirstInRange() but the last element of the range
 * is seeked instead. */
int RM_ZsetLastInRange(RedisModuleKey *key, RedisModuleZsetRange *zr) {
    return zsetInitRange(key,zr,0);
}

/* Return the current sorted set element of an active sorted set iterator
 * or NULL if the range specified in the iterator does not include any
 * element. */
RedisModuleString *RM_ZsetRangeCurrentElement(RedisModuleKey *key, double *score) {
    RedisModuleString *str;

    if (key->zcurrent == NULL) return NULL;
    if (key->value->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *eptr, *sptr;
        eptr = key->zcurrent;
        sds ele = ziplistGetObject(eptr);
        if (score) {
            sptr = ziplistNext(key->value->ptr,eptr);
            *score = zzlGetScore(sptr);
        }
        str = createObject(OBJ_STRING,ele);
    } else if (key->value->encoding == OBJ_ENCODING_SKIPLIST) {
        zskiplistNode *ln = key->zcurrent;
        if (score) *score = ln->score;
        str = createStringObject(ln->ele,sdslen(ln->ele));
    } else {
        serverPanic("Unsupported zset encoding");
    }
    RM_AutoMemoryAdd(key->ctx,REDISMODULE_AM_STRING,str);
    return str;
}

/* Go to the next element of the sorted set iterator. Returns 1 if there was
 * a next element, 0 if we are already at the latest element or the range
 * does not include any item at all. */
int RM_ZsetRangeNext(RedisModuleKey *key) {
    if (!key->zr || !key->zcurrent) return 0; /* No active iterator. */
    zrangespec zrs;

    /* Convert to core range structure. */
    RedisModuleZsetRange *zr = key->zr;
    if (zr->type == REDISMODULE_ZSET_RANGE_SCORE) {
        zrs.min = zr->score_start;
        zrs.max = zr->score_end;
        zrs.minex = (zr->flags & REDISMODULE_ZSET_RANGE_START_EX) != 0;
        zrs.maxex = (zr->flags & REDISMODULE_ZSET_RANGE_END_EX) != 0;
    }

    if (key->value->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = key->value->ptr;
        unsigned char *eptr = key->zcurrent;
        unsigned char *next;
        next = ziplistNext(zl,eptr); /* Skip element. */
        if (next) next = ziplistNext(zl,next); /* Skip score. */
        if (next == NULL) {
            key->zer = 1;
            return 0;
        } else {
            /* Are we still within the range? */
            if (zr->type == REDISMODULE_ZSET_RANGE_SCORE) {
                /* Fetch the next element score for the
                 * range check. */
                unsigned char *saved_next = next;
                next = ziplistNext(zl,next); /* Skip next element. */
                double score = zzlGetScore(next); /* Obtain the next score. */
                if (!zslValueLteMax(score,&zrs)) {
                    key->zer = 1;
                    return 0;
                }
                next = saved_next;
            }
            key->zcurrent = next;
            return 1;
        }
    } else if (key->value->encoding == OBJ_ENCODING_SKIPLIST) {
        zskiplistNode *ln = key->zcurrent, *next = ln->level[0].forward;
        if (next == NULL) {
            key->zer = 1;
            return 0;
        } else {
            /* Are we still within the range? */
            if (zr->type == REDISMODULE_ZSET_RANGE_SCORE &&
                !zslValueLteMax(ln->score,&zrs))
            {
                key->zer = 1;
                return 0;
            }
            key->zcurrent = next;
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
    if (!key->zr || !key->zcurrent) return 0; /* No active iterator. */
    zrangespec zrs;

    /* Convert to core range structure. */
    RedisModuleZsetRange *zr = key->zr;
    if (zr->type == REDISMODULE_ZSET_RANGE_SCORE) {
        zrs.min = zr->score_start;
        zrs.max = zr->score_end;
        zrs.minex = (zr->flags & REDISMODULE_ZSET_RANGE_START_EX) != 0;
        zrs.maxex = (zr->flags & REDISMODULE_ZSET_RANGE_END_EX) != 0;
    }

    if (key->value->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = key->value->ptr;
        unsigned char *eptr = key->zcurrent;
        unsigned char *prev;
        prev = ziplistPrev(zl,eptr); /* Go back to previous score. */
        if (prev) prev = ziplistPrev(zl,prev); /* Back to previous ele. */
        if (prev == NULL) {
            key->zer = 1;
            return 0;
        } else {
            /* Are we still within the range? */
            if (zr->type == REDISMODULE_ZSET_RANGE_SCORE) {
                /* Fetch the previous element score for the
                 * range check. */
                unsigned char *saved_prev = prev;
                prev = ziplistNext(zl,prev); /* Skip element to get the score. */
                double score = zzlGetScore(prev); /* Obtain the prev score. */
                if (!zslValueGteMin(score,&zrs)) {
                    key->zer = 1;
                    return 0;
                }
                prev = saved_prev;
            }
            key->zcurrent = prev;
            return 1;
        }
    } else if (key->value->encoding == OBJ_ENCODING_SKIPLIST) {
        zskiplistNode *ln = key->zcurrent, *prev = ln->backward;
        if (prev == NULL) {
            key->zer = 1;
            return 0;
        } else {
            /* Are we still within the range? */
            if (zr->type == REDISMODULE_ZSET_RANGE_SCORE &&
                !zslValueGteMin(ln->score,&zrs))
            {
                key->zer = 1;
                return 0;
            }
            key->zcurrent = prev;
            return 1;
        }
    } else {
        serverPanic("Unsupported zset encoding");
    }
}

/* --------------------------------------------------------------------------
 * Redis <-> Modules generic Call() API
 * -------------------------------------------------------------------------- */

/* Create a new RedisModuleCallReply object. The processing of the reply
 * is lazy, the object is just populated with the raw protocol and later
 * is processed as needed. Initially we just make sure to set the right
 * reply type, which is extremely cheap to do. */
RedisModuleCallReply *moduleCreateCallReplyFromProto(RedisModuleCtx *ctx, sds proto) {
    RedisModuleCallReply *reply = zmalloc(sizeof(*reply));
    reply->ctx = ctx;
    reply->proto = proto;
    reply->protolen = sdslen(proto);
    reply->flags = REDISMODULE_REPLYFLAG_TOPARSE; /* Lazy parsing. */
    switch(proto[0]) {
    case '$':
    case '+': reply->type = REDISMODULE_REPLY_STRING; break;
    case '-': reply->type = REDISMODULE_REPLY_ERROR; break;
    case ':': reply->type = REDISMODULE_REPLY_INTEGER; break;
    case '*': reply->type = REDISMODULE_REPLY_ARRAY; break;
    default: reply->type = REDISMODULE_REPLY_UNKNOWN; break;
    }
    if ((proto[0] == '*' || proto[0] == '$') && proto[1] == '-')
        reply->type = REDISMODULE_REPLY_NULL;
    return reply;
}

void moduleParseCallReply_Int(RedisModuleCallReply *reply);
void moduleParseCallReply_BulkString(RedisModuleCallReply *reply);
void moduleParseCallReply_SimpleString(RedisModuleCallReply *reply);
void moduleParseCallReply_Array(RedisModuleCallReply *reply);

/* Do nothing if REDISMODULE_REPLYFLAG_TOPARSE is false, otherwise
 * use the protcol of the reply in reply->proto in order to fill the
 * reply with parsed data according to the reply type. */
void moduleParseCallReply(RedisModuleCallReply *reply) {
    if (!(reply->flags & REDISMODULE_REPLYFLAG_TOPARSE)) return;
    reply->flags &= ~REDISMODULE_REPLYFLAG_TOPARSE;

    switch(reply->proto[0]) {
    case ':': moduleParseCallReply_Int(reply); break;
    case '$': moduleParseCallReply_BulkString(reply); break;
    case '-': /* handled by next item. */
    case '+': moduleParseCallReply_SimpleString(reply); break;
    case '*': moduleParseCallReply_Array(reply); break;
    }
}

void moduleParseCallReply_Int(RedisModuleCallReply *reply) {
    char *proto = reply->proto;
    char *p = strchr(proto+1,'\r');

    string2ll(proto+1,p-proto-1,&reply->val.ll);
    reply->protolen = p-proto+2;
    reply->type = REDISMODULE_REPLY_INTEGER;
}

void moduleParseCallReply_BulkString(RedisModuleCallReply *reply) {
    char *proto = reply->proto;
    char *p = strchr(proto+1,'\r');
    long long bulklen;

    string2ll(proto+1,p-proto-1,&bulklen);
    if (bulklen == -1) {
        reply->protolen = p-proto+2;
        reply->type = REDISMODULE_REPLY_NULL;
    } else {
        reply->val.str = p+2;
        reply->len = bulklen;
        reply->protolen = p-proto+2+bulklen+2;
        reply->type = REDISMODULE_REPLY_STRING;
    }
}

void moduleParseCallReply_SimpleString(RedisModuleCallReply *reply) {
    char *proto = reply->proto;
    char *p = strchr(proto+1,'\r');

    reply->val.str = proto+1;
    reply->len = p-proto-1;
    reply->protolen = p-proto+2;
    reply->type = proto[0] == '+' ? REDISMODULE_REPLY_STRING :
                                    REDISMODULE_REPLY_ERROR;
}

void moduleParseCallReply_Array(RedisModuleCallReply *reply) {
    char *proto = reply->proto;
    char *p = strchr(proto+1,'\r');
    long long arraylen, j;

    string2ll(proto+1,p-proto-1,&arraylen);
    p += 2;

    if (arraylen == -1) {
        reply->protolen = p-proto;
        reply->type = REDISMODULE_REPLY_NULL;
        return;
    }

    reply->val.array = zmalloc(sizeof(RedisModuleCallReply)*arraylen);
    reply->len = arraylen;
    for (j = 0; j < arraylen; j++) {
        RedisModuleCallReply *ele = reply->val.array+j;
        ele->flags = REDISMODULE_REPLYFLAG_NESTED |
                     REDISMODULE_REPLYFLAG_TOPARSE;
        ele->proto = p;
        moduleParseCallReply(ele);
        p += ele->protolen;
    }
    reply->protolen = p-proto;
    reply->type = REDISMODULE_REPLY_ARRAY;
}

/* Free a Call reply and all the nested replies it contains if it's an
 * array. */
void RM_FreeCallReply_Rec(RedisModuleCallReply *reply, int freenested){
    /* Don't free nested replies by default: the user must always free the
     * toplevel reply. However be gentle and don't crash if the module
     * misuses the API. */
    if (!freenested && reply->flags & REDISMODULE_REPLYFLAG_NESTED) return;

    if (!(reply->flags & REDISMODULE_REPLYFLAG_TOPARSE)) {
        if (reply->type == REDISMODULE_REPLY_ARRAY) {
            size_t j;
            for (j = 0; j < reply->len; j++)
                RM_FreeCallReply_Rec(reply->val.array+j,1);
            zfree(reply->val.array);
        }
    }

    /* For nested replies, we don't free reply->proto (which if not NULL
     * references the parent reply->proto buffer), nor the structure
     * itself which is allocated as an array of structures, and is freed
     * when the array value is released. */
    if (!(reply->flags & REDISMODULE_REPLYFLAG_NESTED)) {
        if (reply->proto) sdsfree(reply->proto);
        zfree(reply);
    }
}

/* Wrapper for the recursive free reply function. This is needed in order
 * to have the first level function to return on nested replies, but only
 * if called by the module API. */
void RM_FreeCallReply(RedisModuleCallReply *reply) {
    RM_FreeCallReply_Rec(reply,0);
    RM_AutoMemoryFreed(reply->ctx,REDISMODULE_AM_REPLY,reply);
}

/* Return the reply type. */
int RM_CallReplyType(RedisModuleCallReply *reply) {
    return reply->type;
}

/* Return the reply type length, where applicable. */
size_t RM_CallReplyLength(RedisModuleCallReply *reply) {
    moduleParseCallReply(reply);
    switch(reply->type) {
    case REDISMODULE_REPLY_STRING:
    case REDISMODULE_REPLY_ERROR:
    case REDISMODULE_REPLY_ARRAY:
        return reply->len;
    default:
        return 0;
    }
}

/* Return the 'idx'-th nested call reply element of an array reply, or NULL
 * if the reply type is wrong or the index is out of range. */
RedisModuleCallReply *RM_CallReplyArrayElement(RedisModuleCallReply *reply, size_t idx) {
    moduleParseCallReply(reply);
    if (reply->type != REDISMODULE_REPLY_ARRAY) return NULL;
    if (idx >= reply->len) return NULL;
    return reply->val.array+idx;
}

/* Return the long long of an integer reply. */
long long RM_CallReplyInteger(RedisModuleCallReply *reply) {
    moduleParseCallReply(reply);
    if (reply->type != REDISMODULE_REPLY_INTEGER) return LLONG_MIN;
    return reply->val.ll;
}

/* Return the pointer and length of a string or error reply. */
const char *RM_CallReplyStringPtr(RedisModuleCallReply *reply, size_t *len) {
    moduleParseCallReply(reply);
    if (reply->type != REDISMODULE_REPLY_STRING &&
        reply->type != REDISMODULE_REPLY_ERROR) return NULL;
    if (len) *len = reply->len;
    return reply->val.str;
}

/* Return a new string object from a call reply of type string, error or
 * integer. Otherwise (wrong reply type) return NULL. */
RedisModuleString *RM_CreateStringFromCallReply(RedisModuleCallReply *reply) {
    moduleParseCallReply(reply);
    switch(reply->type) {
    case REDISMODULE_REPLY_STRING:
    case REDISMODULE_REPLY_ERROR:
        return RM_CreateString(reply->ctx,reply->val.str,reply->len);
    case REDISMODULE_REPLY_INTEGER: {
        char buf[64];
        int len = ll2string(buf,sizeof(buf),reply->val.ll);
        return RM_CreateString(reply->ctx,buf,len);
        }
    default: return NULL;
    }
}

/* Returns an array of robj pointers, and populates *argc with the number
 * of items, by parsing the format specifier "fmt" as described for
 * the RM_Call(), RM_Replicate() and other module APIs.
 *
 * The integer pointed by 'flags' is populated with flags according
 * to special modifiers in "fmt". For now only one exists:
 *
 * "!" -> REDISMODULE_ARGV_REPLICATE
 *
 * On error (format specifier error) NULL is returned and nothing is
 * allocated. On success the argument vector is returned. */

#define REDISMODULE_ARGV_REPLICATE (1<<0)

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
            argv[argc++] = obj;
            incrRefCount(obj);
        } else if (*p == 'b') {
            char *buf = va_arg(ap,char*);
            size_t len = va_arg(ap,size_t);
            argv[argc++] = createStringObject(buf,len);
        } else if (*p == 'l') {
            long ll = va_arg(ap,long long);
            argv[argc++] = createStringObjectFromLongLong(ll);
        } else if (*p == 'v') {
            /* TODO: work in progress. */
        } else if (*p == '!') {
            if (flags) (*flags) |= REDISMODULE_ARGV_REPLICATE;
        } else {
            goto fmterr;
        }
        p++;
    }
    *argcp = argc;
    return argv;

fmterr:
    for (j = 0; j < argc; j++)
        decrRefCount(argv[j]);
    zfree(argv);
    return NULL;
}

/* Exported API to call any Redis command from modules.
 * On success a RedisModuleCallReply object is returned, otherwise
 * NULL is returned and errno is set to the following values:
 *
 * EINVAL: command non existing, wrong arity, wrong format specifier.
 * EPERM:  operation in Cluster instance with key in non local slot. */
RedisModuleCallReply *RM_Call(RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...) {
    struct redisCommand *cmd;
    client *c = NULL;
    robj **argv = NULL;
    int argc = 0, flags = 0;
    va_list ap;
    RedisModuleCallReply *reply = NULL;
    int replicate = 0; /* Replicate this command? */

    cmd = lookupCommandByCString((char*)cmdname);
    if (!cmd) {
        errno = EINVAL;
        return NULL;
    }

    /* Create the client and dispatch the command. */
    va_start(ap, fmt);
    c = createClient(-1);
    argv = moduleCreateArgvFromUserFormat(cmdname,fmt,&argc,&flags,ap);
    replicate = flags & REDISMODULE_ARGV_REPLICATE;
    va_end(ap);

    /* Setup our fake client for command execution. */
    c->flags |= CLIENT_MODULE;
    c->argv = argv;
    c->argc = argc;
    c->cmd = c->lastcmd = cmd;
    /* We handle the above format error only when the client is setup so that
     * we can free it normally. */
    if (argv == NULL) goto cleanup;

    /* Basic arity checks. */
    if ((cmd->arity > 0 && cmd->arity != argc) || (argc < -cmd->arity)) {
        errno = EINVAL;
        goto cleanup;
    }

    /* If this is a Redis Cluster node, we need to make sure the module is not
     * trying to access non-local keys, with the exception of commands
     * received from our master. */
    if (server.cluster_enabled && !(ctx->client->flags & CLIENT_MASTER)) {
        /* Duplicate relevant flags in the module client. */
        c->flags &= ~(CLIENT_READONLY|CLIENT_ASKING);
        c->flags |= ctx->client->flags & (CLIENT_READONLY|CLIENT_ASKING);
        if (getNodeByQuery(c,c->cmd,c->argv,c->argc,NULL,NULL) !=
                           server.cluster->myself)
        {
            errno = EPERM;
            goto cleanup;
        }
    }

    /* If we are using single commands replication, we need to wrap what
     * we propagate into a MULTI/EXEC block, so that it will be atomic like
     * a Lua script in the context of AOF and slaves. */
    if (replicate) moduleReplicateMultiIfNeeded(ctx);

    /* Run the command */
    int call_flags = CMD_CALL_SLOWLOG | CMD_CALL_STATS;
    if (replicate) {
        call_flags |= CMD_CALL_PROPAGATE_AOF;
        call_flags |= CMD_CALL_PROPAGATE_REPL;
    }
    call(c,call_flags);

    /* Convert the result of the Redis command into a suitable Lua type.
     * The first thing we need is to create a single string from the client
     * output buffers. */
    sds proto = sdsnewlen(c->buf,c->bufpos);
    c->bufpos = 0;
    while(listLength(c->reply)) {
        sds o = listNodeValue(listFirst(c->reply));

        proto = sdscatsds(proto,o);
        listDelNode(c->reply,listFirst(c->reply));
    }
    reply = moduleCreateCallReplyFromProto(ctx,proto);
    RM_AutoMemoryAdd(ctx,REDISMODULE_AM_REPLY,reply);

cleanup:
    freeClient(c);
    return reply;
}

/* Return a pointer, and a length, to the protocol returned by the command
 * that returned the reply object. */
const char *RM_CallReplyProto(RedisModuleCallReply *reply, size_t *len) {
    if (reply->proto) *len = sdslen(reply->proto);
    return reply->proto;
}

/* --------------------------------------------------------------------------
 * Modules API internals
 * -------------------------------------------------------------------------- */

/* server.moduleapi dictionary type. Only uses plain C strings since
 * this gets queries from modules. */

unsigned int dictCStringKeyHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, strlen((char*)key));
}

int dictCStringKeyCompare(void *privdata, const void *key1, const void *key2) {
    DICT_NOTUSED(privdata);
    return strcmp(key1,key2) == 0;
}

dictType moduleAPIDictType = {
    dictCStringKeyHash,        /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictCStringKeyCompare,     /* key compare */
    NULL,                      /* key destructor */
    NULL                       /* val destructor */
};

int moduleRegisterApi(const char *funcname, void *funcptr) {
    return dictAdd(server.moduleapi, (char*)funcname, funcptr);
}

#define REGISTER_API(name) \
    moduleRegisterApi("RedisModule_" #name, (void *)(unsigned long)RM_ ## name)

/* Register all the APIs we export. */
void moduleRegisterCoreAPI(void) {
    server.moduleapi = dictCreate(&moduleAPIDictType,NULL);
    REGISTER_API(CreateCommand);
    REGISTER_API(SetModuleAttribs);
    REGISTER_API(WrongArity);
    REGISTER_API(ReplyWithLongLong);
    REGISTER_API(ReplyWithError);
    REGISTER_API(ReplyWithSimpleString);
    REGISTER_API(ReplyWithArray);
    REGISTER_API(ReplyWithString);
    REGISTER_API(ReplyWithStringBuffer);
    REGISTER_API(ReplyWithNull);
    REGISTER_API(ReplyWithCallReply);
    REGISTER_API(ReplyWithDouble);
    REGISTER_API(GetSelectedDb);
    REGISTER_API(SelectDb);
    REGISTER_API(OpenKey);
    REGISTER_API(CloseKey);
    REGISTER_API(KeyType);
    REGISTER_API(ValueLength);
    REGISTER_API(ListPush);
    REGISTER_API(ListPop);
    REGISTER_API(StringToLongLong);
    REGISTER_API(StringToDouble);
    REGISTER_API(Call);
    REGISTER_API(CallReplyProto);
    REGISTER_API(FreeCallReply);
    REGISTER_API(CallReplyInteger);
    REGISTER_API(CallReplyType);
    REGISTER_API(CallReplyLength);
    REGISTER_API(CallReplyArrayElement);
    REGISTER_API(CallReplyStringPtr);
    REGISTER_API(CreateStringFromCallReply);
    REGISTER_API(CreateString);
    REGISTER_API(CreateStringFromLongLong);
    REGISTER_API(FreeString);
    REGISTER_API(StringPtrLen);
    REGISTER_API(AutoMemory);
    REGISTER_API(Replicate);
    REGISTER_API(ReplicateVerbatim);
    REGISTER_API(DeleteKey);
    REGISTER_API(StringSet);
    REGISTER_API(StringDMA);
    REGISTER_API(StringTruncate);
    REGISTER_API(SetExpire);
    REGISTER_API(GetExpire);
    REGISTER_API(ZsetAdd);
    REGISTER_API(ZsetIncrby);
    REGISTER_API(ZsetScore);
    REGISTER_API(ZsetRem);
    REGISTER_API(ZsetRangeStop);
    REGISTER_API(ZsetFirstInRange);
    REGISTER_API(ZsetLastInRange);
    REGISTER_API(ZsetRangeCurrentElement);
    REGISTER_API(ZsetRangeNext);
    REGISTER_API(ZsetRangePrev);
    REGISTER_API(ZsetRangeEndReached);
}

/* Global initialization at Redis startup. */
void moduleInitModulesSystem(void) {
    server.loadmodule_queue = listCreate();
    modules = dictCreate(&modulesDictType,NULL);
    moduleRegisterCoreAPI();
}

/* Load all the modules in the server.loadmodule_queue list, which is
 * populated by `loadmodule` directives in the configuration file.
 * We can't load modules directly when processing the configuration file
 * because the server must be fully initialized before loading modules.
 *
 * The function aborts the server on errors, since to start with missing
 * modules is not considered sane: clients may rely on the existance of
 * given commands, loading AOF also may need some modules to exist, and
 * if this instance is a slave, it must understand commands from master. */
void moduleLoadFromQueue(void) {
    listIter li;
    listNode *ln;

    listRewind(server.loadmodule_queue,&li);
    while((ln = listNext(&li))) {
        sds modulepath = ln->value;
        if (moduleLoad(modulepath) == C_ERR) {
            serverLog(LL_WARNING,
                "Can't load module from %s: server aborting",
                modulepath);
            exit(1);
        }
    }
}

void moduleFreeModuleStructure(struct RedisModule *module) {
    sdsfree(module->name);
    zfree(module);
}

/* Load a module and initialize it. On success C_OK is returned, otherwise
 * C_ERR is returned. */
int moduleLoad(const char *path) {
    int (*onload)(void *);
    void *handle;
    RedisModuleCtx ctx = REDISMODULE_CTX_INIT;

    handle = dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if (handle == NULL) {
        serverLog(LL_WARNING, "Module %s failed to load: %s", path, dlerror());
        return C_ERR;
    }
    onload = (int (*)(void *))(unsigned long) dlsym(handle,"RedisModule_OnLoad");
    if (onload == NULL) {
        serverLog(LL_WARNING,
            "Module %s does not export RedisModule_OnLoad() "
            "symbol. Module not loaded.",path);
        return C_ERR;
    }
    if (onload((void*)&ctx) == REDISMODULE_ERR) {
        if (ctx.module) moduleFreeModuleStructure(ctx.module);
        dlclose(handle);
        serverLog(LL_WARNING,
            "Module %s initialization failed. Module not loaded",path);
        return C_ERR;
    }

    /* Redis module loaded! Register it. */
    dictAdd(modules,ctx.module->name,ctx.module);
    ctx.module->handle = handle;
    serverLog(LL_NOTICE,"Module '%s' loaded from %s",ctx.module->name,path);
    return C_OK;
}

/* Unload the module registered with the specified name. On success
 * C_OK is returned, otherwise C_ERR is returned and errno is set
 * to the following values depending on the type of error:
 *
 * ENONET: No such module having the specified name. */
int moduleUnload(sds name) {
    struct RedisModule *module = dictFetchValue(modules,name);
    if (module == NULL) {
        errno = ENOENT;
        return REDISMODULE_ERR;
    }

    /* Unregister all the commands registered by this module. */
    dictIterator *di = dictGetSafeIterator(server.commands);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *cmd = dictGetVal(de);
        if (cmd->proc == RedisModuleCommandDispatcher) {
            RedisModuleCommandProxy *cp =
                (void*)(unsigned long)cmd->getkeys_proc;
            sds cmdname = cp->rediscmd->name;
            if (cp->module == module) {
                dictDelete(server.commands,cmdname);
                dictDelete(server.orig_commands,cmdname);
                sdsfree(cmdname);
                zfree(cp->rediscmd);
                zfree(cp);
            }
        }
    }
    dictReleaseIterator(di);

    /* Unregister all the hooks. TODO: Yet no hooks support here. */

    /* Unload the dynamic library. */
    if (dlclose(module->handle) == -1) {
        char *error = dlerror();
        if (error == NULL) error = "Unknown error";
        serverLog(LL_WARNING,"Error when trying to close the %s module: %s",
            module->name, error);
    }

    /* Remove from list of modules. */
    serverLog(LL_NOTICE,"Module %s unloaded",module->name);
    dictDelete(modules,module->name);

    /* Free the module structure. */
    zfree(module);

    return REDISMODULE_OK;
}

/* Redis MODULE command.
 *
 * MODULE LOAD <path> */
void moduleCommand(client *c) {
    char *subcmd = c->argv[1]->ptr;

    if (!strcasecmp(subcmd,"load") && c->argc == 3) {
        if (moduleLoad(c->argv[2]->ptr) == C_OK)
            addReply(c,shared.ok);
        else
            addReplyError(c,
                "Error loading the extension. Please check the server logs.");
    } else if (!strcasecmp(subcmd,"unload") && c->argc == 3) {
        if (moduleUnload(c->argv[2]->ptr) == C_OK)
            addReply(c,shared.ok);
        else {
            char *errmsg = "operation not possible.";
            switch(errno) {
            case ENOENT: errmsg = "no such module with that name";
            }
            addReplyErrorFormat(c,"Error unloading module: %s",errmsg);
        }
    } else if (!strcasecmp(subcmd,"list") && c->argc == 2) {
        dictIterator *di = dictGetIterator(modules);
        dictEntry *de;

        addReplyMultiBulkLen(c,dictSize(modules));
        while ((de = dictNext(di)) != NULL) {
            sds name = dictGetKey(de);
            struct RedisModule *module = dictGetVal(de);
            addReplyMultiBulkLen(c,4);
            addReplyBulkCString(c,"name");
            addReplyBulkCBuffer(c,name,sdslen(name));
            addReplyBulkCString(c,"ver");
            addReplyLongLong(c,module->ver);
        }
        dictReleaseIterator(di);
    } else {
        addReply(c,shared.syntaxerr);
    }
}

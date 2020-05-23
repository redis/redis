#define REDISMODULE_EXPERIMENTAL_API
#include "redismodule.h"

#include <string.h>
#include <assert.h>
#include <unistd.h>

#define LIST_SIZE 1024

typedef struct {
    long long list[LIST_SIZE];
    long long length;
} fsl_t; /* Fixed-size list */

static RedisModuleType *fsltype = NULL;

fsl_t *fsl_type_create() {
    fsl_t *o;
    o = RedisModule_Alloc(sizeof(*o));
    o->length = 0;
    return o;
}

void fsl_type_free(fsl_t *o) {
    RedisModule_Free(o);
}

/* ========================== "fsltype" type methods ======================= */

void *fsl_rdb_load(RedisModuleIO *rdb, int encver) {
    if (encver != 0) {
        return NULL;
    }
    fsl_t *fsl = fsl_type_create();
    fsl->length = RedisModule_LoadUnsigned(rdb);
    for (long long i = 0; i < fsl->length; i++)
        fsl->list[i] = RedisModule_LoadSigned(rdb);
    return fsl;
}

void fsl_rdb_save(RedisModuleIO *rdb, void *value) {
    fsl_t *fsl = value;
    RedisModule_SaveUnsigned(rdb,fsl->length);
    for (long long i = 0; i < fsl->length; i++)
        RedisModule_SaveSigned(rdb, fsl->list[i]);
}

void fsl_aofrw(RedisModuleIO *aof, RedisModuleString *key, void *value) {
    fsl_t *fsl = value;
    for (long long i = 0; i < fsl->length; i++)
        RedisModule_EmitAOF(aof, "FSL.PUSH","sl", key, fsl->list[i]);
}

void fsl_free(void *value) {
    fsl_type_free(value);
}

/* ========================== helper methods ======================= */

int get_fsl(RedisModuleCtx *ctx, RedisModuleString *keyname, int mode, int create, fsl_t **fsl, int reply_on_failure) {
    RedisModuleKey *key = RedisModule_OpenKey(ctx, keyname, mode);

    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_EMPTY && RedisModule_ModuleTypeGetType(key) != fsltype) {
        RedisModule_CloseKey(key);
        if (reply_on_failure)
            RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return 0;
    }

    /* Create an empty value object if the key is currently empty. */
    if (type == REDISMODULE_KEYTYPE_EMPTY) {
        if (!create) {
            /* Key is empty but we cannot create */
            RedisModule_CloseKey(key);
            *fsl = NULL;
            return 1;
        }
        *fsl = fsl_type_create();
        RedisModule_ModuleTypeSetValue(key, fsltype, *fsl);
    } else {
        *fsl = RedisModule_ModuleTypeGetValue(key);
    }

    RedisModule_CloseKey(key);
    return 1;
}

/* ========================== commands ======================= */

/* FSL.PUSH <key> <int> - Push an integer to the fixed-size list (to the right).
 * It must be greater than the element in the head of the list. */
int fsl_push(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3)
        return RedisModule_WrongArity(ctx);

    long long ele;
    if (RedisModule_StringToLongLong(argv[2],&ele) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx,"ERR invalid integer");

    fsl_t *fsl;
    if (!get_fsl(ctx, argv[1], REDISMODULE_WRITE, 1, &fsl, 1))
        return REDISMODULE_OK;

    if (fsl->length == LIST_SIZE)
        return RedisModule_ReplyWithError(ctx,"ERR list is full");

    if (fsl->length != 0 && fsl->list[fsl->length-1] >= ele)
        return RedisModule_ReplyWithError(ctx,"ERR new element has to be greater than the head element");

    fsl->list[fsl->length++] = ele;
    RedisModule_SignalKeyAsReady(ctx, argv[1]);

    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

int bpop_reply_callback(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModuleString *keyname = RedisModule_GetBlockedClientReadyKey(ctx);

    fsl_t *fsl;
    if (!get_fsl(ctx, keyname, REDISMODULE_READ, 0, &fsl, 0) || !fsl)
        return REDISMODULE_ERR;

    RedisModule_ReplyWithLongLong(ctx, fsl->list[--fsl->length]);
    return REDISMODULE_OK;
}

int bpop_timeout_callback(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    return RedisModule_ReplyWithSimpleString(ctx, "Request timedout");
}

/* FSL.BPOP <key> <timeout> - Block clients until list has two or more elements.
 * When that happens, unblock client and pop the last two elements (from the right). */
int fsl_bpop(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3)
        return RedisModule_WrongArity(ctx);

    long long timeout;
    if (RedisModule_StringToLongLong(argv[2],&timeout) != REDISMODULE_OK || timeout < 0)
        return RedisModule_ReplyWithError(ctx,"ERR invalid timeout");

    fsl_t *fsl;
    if (!get_fsl(ctx, argv[1], REDISMODULE_READ, 0, &fsl, 1))
        return REDISMODULE_OK;

    if (!fsl) {
        RedisModule_BlockClientOnKeys(ctx, bpop_reply_callback, bpop_timeout_callback,
                                      NULL, timeout, &argv[1], 1, NULL);
    } else {
        RedisModule_ReplyWithLongLong(ctx, fsl->list[--fsl->length]);
    }

    return REDISMODULE_OK;
}

int bpopgt_reply_callback(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModuleString *keyname = RedisModule_GetBlockedClientReadyKey(ctx);
    long long *pgt = RedisModule_GetBlockedClientPrivateData(ctx);

    fsl_t *fsl;
    if (!get_fsl(ctx, keyname, REDISMODULE_READ, 0, &fsl, 0) || !fsl)
        return REDISMODULE_ERR;

    if (fsl->list[fsl->length-1] <= *pgt)
        return REDISMODULE_ERR;

    RedisModule_ReplyWithLongLong(ctx, fsl->list[--fsl->length]);
    return REDISMODULE_OK;
}

int bpopgt_timeout_callback(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    return RedisModule_ReplyWithSimpleString(ctx, "Request timedout");
}

void bpopgt_free_privdata(RedisModuleCtx *ctx, void *privdata) {
    REDISMODULE_NOT_USED(ctx);
    RedisModule_Free(privdata);
}

/* FSL.BPOPGT <key> <gt> <timeout> - Block clients until list has an element greater than <gt>.
 * When that happens, unblock client and pop the last element (from the right). */
int fsl_bpopgt(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4)
        return RedisModule_WrongArity(ctx);

    long long gt;
    if (RedisModule_StringToLongLong(argv[2],&gt) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx,"ERR invalid integer");

    long long timeout;
    if (RedisModule_StringToLongLong(argv[3],&timeout) != REDISMODULE_OK || timeout < 0)
        return RedisModule_ReplyWithError(ctx,"ERR invalid timeout");

    fsl_t *fsl;
    if (!get_fsl(ctx, argv[1], REDISMODULE_READ, 0, &fsl, 1))
        return REDISMODULE_OK;

    if (!fsl || fsl->list[fsl->length-1] <= gt) {
        /* We use malloc so the tests in blockedonkeys.tcl can check for memory leaks */
        long long *pgt = RedisModule_Alloc(sizeof(long long));
        *pgt = gt;
        RedisModule_BlockClientOnKeys(ctx, bpopgt_reply_callback, bpopgt_timeout_callback,
                                      bpopgt_free_privdata, timeout, &argv[1], 1, pgt);
    } else {
        RedisModule_ReplyWithLongLong(ctx, fsl->list[--fsl->length]);
    }

    return REDISMODULE_OK;
}

int bpoppush_reply_callback(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModuleString *src_keyname = RedisModule_GetBlockedClientReadyKey(ctx);
    RedisModuleString *dst_keyname = RedisModule_GetBlockedClientPrivateData(ctx);

    fsl_t *src;
    if (!get_fsl(ctx, src_keyname, REDISMODULE_READ, 0, &src, 0) || !src)
        return REDISMODULE_ERR;

    fsl_t *dst;
    if (!get_fsl(ctx, dst_keyname, REDISMODULE_WRITE, 1, &dst, 0) || !dst)
        return REDISMODULE_ERR;

    long long ele = src->list[--src->length];
    dst->list[dst->length++] = ele;
    RedisModule_SignalKeyAsReady(ctx, dst_keyname);
    return RedisModule_ReplyWithLongLong(ctx, ele);
}

int bpoppush_timeout_callback(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    return RedisModule_ReplyWithSimpleString(ctx, "Request timedout");
}

void bpoppush_free_privdata(RedisModuleCtx *ctx, void *privdata) {
    RedisModule_FreeString(ctx, privdata);
}

/* FSL.BPOPPUSH <src> <dst> <timeout> - Block clients until <src> has an element.
 * When that happens, unblock client, pop the last element from <src> and push it to <dst>
 * (from the right). */
int fsl_bpoppush(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4)
        return RedisModule_WrongArity(ctx);

    long long timeout;
    if (RedisModule_StringToLongLong(argv[3],&timeout) != REDISMODULE_OK || timeout < 0)
        return RedisModule_ReplyWithError(ctx,"ERR invalid timeout");

    fsl_t *src;
    if (!get_fsl(ctx, argv[1], REDISMODULE_READ, 0, &src, 1))
        return REDISMODULE_OK;

    if (!src) {
        /* Retain string for reply callback */
        RedisModule_RetainString(ctx, argv[2]);
        /* Key is empty, we must block */
        RedisModule_BlockClientOnKeys(ctx, bpoppush_reply_callback, bpoppush_timeout_callback,
                                      bpoppush_free_privdata, timeout, &argv[1], 1, argv[2]);
    } else {
        fsl_t *dst;
        if (!get_fsl(ctx, argv[2], REDISMODULE_WRITE, 1, &dst, 1))
            return REDISMODULE_OK;
        long long ele = src->list[--src->length];
        dst->list[dst->length++] = ele;
        RedisModule_SignalKeyAsReady(ctx, argv[2]);
        RedisModule_ReplyWithLongLong(ctx, ele);
    }

    return REDISMODULE_OK;
}

/* FSL.GETALL <key> - Reply with an array containing all elements. */
int fsl_getall(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2)
        return RedisModule_WrongArity(ctx);

    fsl_t *fsl;
    if (!get_fsl(ctx, argv[1], REDISMODULE_READ, 0, &fsl, 1))
        return REDISMODULE_OK;

    if (!fsl)
        return RedisModule_ReplyWithArray(ctx, 0);

    RedisModule_ReplyWithArray(ctx, fsl->length);
    for (int i = 0; i < fsl->length; i++)
        RedisModule_ReplyWithLongLong(ctx, fsl->list[i]);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "blockonkeys", 1, REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModuleTypeMethods tm = {
        .version = REDISMODULE_TYPE_METHOD_VERSION,
        .rdb_load = fsl_rdb_load,
        .rdb_save = fsl_rdb_save,
        .aof_rewrite = fsl_aofrw,
        .mem_usage = NULL,
        .free = fsl_free,
        .digest = NULL
    };

    fsltype = RedisModule_CreateDataType(ctx, "fsltype_t", 0, &tm);
    if (fsltype == NULL)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"fsl.push",fsl_push,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"fsl.bpop",fsl_bpop,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"fsl.bpopgt",fsl_bpopgt,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"fsl.bpoppush",fsl_bpoppush,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"fsl.getall",fsl_getall,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

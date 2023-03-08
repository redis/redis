#include "redismodule.h"

#include <string.h>
#include <strings.h>
#include <assert.h>
#include <unistd.h>

#define UNUSED(V) ((void) V)

#define LIST_SIZE 1024

/* The FSL (Fixed-Size List) data type is a low-budget imitation of the
 * native Redis list, in order to test list-like commands implemented
 * by a module.
 * Examples: FSL.PUSH, FSL.BPOP, etc. */

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

/* Wrapper to the boilerplate code of opening a key, checking its type, etc.
 * Returns 0 if `keyname` exists in the dataset, but it's of the wrong type (i.e. not FSL) */
int get_fsl(RedisModuleCtx *ctx, RedisModuleString *keyname, int mode, int create, fsl_t **fsl, int reply_on_failure) {
    *fsl = NULL;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, keyname, mode);

    if (RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_EMPTY) {
        /* Key exists */
        if (RedisModule_ModuleTypeGetType(key) != fsltype) {
            /* Key is not FSL */
            RedisModule_CloseKey(key);
            if (reply_on_failure)
                RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
            RedisModuleCallReply *reply = RedisModule_Call(ctx, "INCR", "c", "fsl_wrong_type");
            RedisModule_FreeCallReply(reply);
            return 0;
        }

        *fsl = RedisModule_ModuleTypeGetValue(key);
        if (*fsl && !(*fsl)->length && mode & REDISMODULE_WRITE) {
            /* Key exists, but it's logically empty */
            if (create) {
                create = 0; /* No need to create, key exists in its basic state */
            } else {
                RedisModule_DeleteKey(key);
                *fsl = NULL;
            }
        } else {
            /* Key exists, and has elements in it - no need to create anything */
            create = 0;
        }
    }

    if (create) {
        *fsl = fsl_type_create();
        RedisModule_ModuleTypeSetValue(key, fsltype, *fsl);
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
    if (!get_fsl(ctx, keyname, REDISMODULE_WRITE, 0, &fsl, 0) || !fsl)
        return REDISMODULE_ERR;

    RedisModule_Assert(fsl->length);
    RedisModule_ReplyWithLongLong(ctx, fsl->list[--fsl->length]);
    return REDISMODULE_OK;
}

int bpop_timeout_callback(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    return RedisModule_ReplyWithSimpleString(ctx, "Request timedout");
}

/* FSL.BPOP <key> <timeout> [NO_TO_CB]- Block clients until list has two or more elements.
 * When that happens, unblock client and pop the last two elements (from the right). */
int fsl_bpop(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3)
        return RedisModule_WrongArity(ctx);

    long long timeout;
    if (RedisModule_StringToLongLong(argv[2],&timeout) != REDISMODULE_OK || timeout < 0)
        return RedisModule_ReplyWithError(ctx,"ERR invalid timeout");

    int to_cb = 1;
    if (argc == 4) {
        if (strcasecmp("NO_TO_CB", RedisModule_StringPtrLen(argv[3], NULL)))
            return RedisModule_ReplyWithError(ctx,"ERR invalid argument");
        to_cb = 0;
    }

    fsl_t *fsl;
    if (!get_fsl(ctx, argv[1], REDISMODULE_WRITE, 0, &fsl, 1))
        return REDISMODULE_OK;

    if (!fsl) {
        RedisModule_BlockClientOnKeys(ctx, bpop_reply_callback, to_cb ? bpop_timeout_callback : NULL,
                                      NULL, timeout, &argv[1], 1, NULL);
    } else {
        RedisModule_Assert(fsl->length);
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
    if (!get_fsl(ctx, keyname, REDISMODULE_WRITE, 0, &fsl, 0) || !fsl)
        return RedisModule_ReplyWithError(ctx,"UNBLOCKED key no longer exists");

    if (fsl->list[fsl->length-1] <= *pgt)
        return REDISMODULE_ERR;

    RedisModule_Assert(fsl->length);
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
    if (!get_fsl(ctx, argv[1], REDISMODULE_WRITE, 0, &fsl, 1))
        return REDISMODULE_OK;

    if (!fsl)
        return RedisModule_ReplyWithError(ctx,"ERR key must exist");

    if (fsl->list[fsl->length-1] <= gt) {
        /* We use malloc so the tests in blockedonkeys.tcl can check for memory leaks */
        long long *pgt = RedisModule_Alloc(sizeof(long long));
        *pgt = gt;
        RedisModule_BlockClientOnKeysWithFlags(
            ctx, bpopgt_reply_callback, bpopgt_timeout_callback,
            bpopgt_free_privdata, timeout, &argv[1], 1, pgt,
            REDISMODULE_BLOCK_UNBLOCK_DELETED);
    } else {
        RedisModule_Assert(fsl->length);
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
    if (!get_fsl(ctx, src_keyname, REDISMODULE_WRITE, 0, &src, 0) || !src)
        return REDISMODULE_ERR;

    fsl_t *dst;
    if (!get_fsl(ctx, dst_keyname, REDISMODULE_WRITE, 1, &dst, 0) || !dst)
        return REDISMODULE_ERR;

    RedisModule_Assert(src->length);
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
    if (!get_fsl(ctx, argv[1], REDISMODULE_WRITE, 0, &src, 1))
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

        RedisModule_Assert(src->length);
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

/* Callback for blockonkeys_popall */
int blockonkeys_popall_reply_callback(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argc);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_LIST) {
        RedisModuleString *elem;
        long len = 0;
        RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
        while ((elem = RedisModule_ListPop(key, REDISMODULE_LIST_HEAD)) != NULL) {
            len++;
            RedisModule_ReplyWithString(ctx, elem);
            RedisModule_FreeString(ctx, elem);
        }
        RedisModule_ReplySetArrayLength(ctx, len);
    } else {
        RedisModule_ReplyWithError(ctx, "ERR Not a list");
    }
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

int blockonkeys_popall_timeout_callback(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    return RedisModule_ReplyWithError(ctx, "ERR Timeout");
}

/* BLOCKONKEYS.POPALL key
 *
 * Blocks on an empty key for up to 3 seconds. When unblocked by a list
 * operation like LPUSH, all the elements are popped and returned. Fails with an
 * error on timeout. */
int blockonkeys_popall(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2)
        return RedisModule_WrongArity(ctx);

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
        RedisModule_BlockClientOnKeys(ctx, blockonkeys_popall_reply_callback,
                                      blockonkeys_popall_timeout_callback,
                                      NULL, 3000, &argv[1], 1, NULL);
    } else {
        RedisModule_ReplyWithError(ctx, "ERR Key not empty");
    }
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

/* BLOCKONKEYS.LPUSH key val [val ..]
 * BLOCKONKEYS.LPUSH_UNBLOCK key val [val ..]
 *
 * A module equivalent of LPUSH. If the name LPUSH_UNBLOCK is used,
 * RM_SignalKeyAsReady() is also called. */
int blockonkeys_lpush(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3)
        return RedisModule_WrongArity(ctx);

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    if (RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_EMPTY &&
        RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_LIST) {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    } else {
        for (int i = 2; i < argc; i++) {
            if (RedisModule_ListPush(key, REDISMODULE_LIST_HEAD,
                                     argv[i]) != REDISMODULE_OK) {
                RedisModule_CloseKey(key);
                return RedisModule_ReplyWithError(ctx, "ERR Push failed");
            }
        }
    }
    RedisModule_CloseKey(key);

    /* signal key as ready if the command is lpush_unblock */
    size_t len;
    const char *str = RedisModule_StringPtrLen(argv[0], &len);
    if (!strncasecmp(str, "blockonkeys.lpush_unblock", len)) {
        RedisModule_SignalKeyAsReady(ctx, argv[1]);
    }
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* Callback for the BLOCKONKEYS.BLPOPN command */
int blockonkeys_blpopn_reply_callback(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argc);
    long long n;
    RedisModule_StringToLongLong(argv[2], &n);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    int result;
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_LIST &&
        RedisModule_ValueLength(key) >= (size_t)n) {
        RedisModule_ReplyWithArray(ctx, n);
        for (long i = 0; i < n; i++) {
            RedisModuleString *elem = RedisModule_ListPop(key, REDISMODULE_LIST_HEAD);
            RedisModule_ReplyWithString(ctx, elem);
            RedisModule_FreeString(ctx, elem);
        }
        result = REDISMODULE_OK;
    } else if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_LIST ||
               RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
        const char *module_cmd = RedisModule_StringPtrLen(argv[0], NULL);
        if (!strcasecmp(module_cmd, "blockonkeys.blpopn_or_unblock"))
            RedisModule_UnblockClient(RedisModule_GetBlockedClientHandle(ctx), NULL);

        /* continue blocking */
        result = REDISMODULE_ERR;
    } else {
        result = RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    }
    RedisModule_CloseKey(key);
    return result;
}

int blockonkeys_blpopn_timeout_callback(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    return RedisModule_ReplyWithError(ctx, "ERR Timeout");
}

int blockonkeys_blpopn_abort_callback(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    return RedisModule_ReplyWithSimpleString(ctx, "Action aborted");
}

/* BLOCKONKEYS.BLPOPN key N
 *
 * Blocks until key has N elements and then pops them or fails after 3 seconds.
 */
int blockonkeys_blpopn(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) return RedisModule_WrongArity(ctx);

    long long n, timeout = 3000LL;
    if (RedisModule_StringToLongLong(argv[2], &n) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx, "ERR Invalid N");
    }

    if (argc > 3 ) {
        if (RedisModule_StringToLongLong(argv[3], &timeout) != REDISMODULE_OK) {
            return RedisModule_ReplyWithError(ctx, "ERR Invalid timeout value");
        }
    }
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    int keytype = RedisModule_KeyType(key);
    if (keytype != REDISMODULE_KEYTYPE_EMPTY &&
        keytype != REDISMODULE_KEYTYPE_LIST) {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    } else if (keytype == REDISMODULE_KEYTYPE_LIST &&
               RedisModule_ValueLength(key) >= (size_t)n) {
        RedisModule_ReplyWithArray(ctx, n);
        for (long i = 0; i < n; i++) {
            RedisModuleString *elem = RedisModule_ListPop(key, REDISMODULE_LIST_HEAD);
            RedisModule_ReplyWithString(ctx, elem);
            RedisModule_FreeString(ctx, elem);
        }
    } else {
        RedisModule_BlockClientOnKeys(ctx, blockonkeys_blpopn_reply_callback,
                                      timeout ? blockonkeys_blpopn_timeout_callback : blockonkeys_blpopn_abort_callback,
                                      NULL, timeout, &argv[1], 1, NULL);
    }
    RedisModule_CloseKey(key);
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
        .digest = NULL,
    };

    fsltype = RedisModule_CreateDataType(ctx, "fsltype_t", 0, &tm);
    if (fsltype == NULL)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"fsl.push",fsl_push,"write",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"fsl.bpop",fsl_bpop,"write",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"fsl.bpopgt",fsl_bpopgt,"write",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"fsl.bpoppush",fsl_bpoppush,"write",1,2,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"fsl.getall",fsl_getall,"",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "blockonkeys.popall", blockonkeys_popall,
                                  "write", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "blockonkeys.lpush", blockonkeys_lpush,
                                  "write", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "blockonkeys.lpush_unblock", blockonkeys_lpush,
                                  "write", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "blockonkeys.blpopn", blockonkeys_blpopn,
                                  "write", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "blockonkeys.blpopn_or_unblock", blockonkeys_blpopn,
                                      "write", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

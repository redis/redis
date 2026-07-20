/* Test module for point-in-time HASH snapshots (RM_CreateKeyspaceSnapshot etc).
 *
 *   KVSNAP.TEST      -- runs an in-module scenario, replies +OK or an error
 *                       string naming the first failed check.
 *   KVSNAP.THREADGET -- create on the main thread, read a hash field on a worker
 *                       under the GIL (concurrent-read consistency check).
 */
#include "redismodule.h"
#include <string.h>
#include <pthread.h>
#include <time.h>

static int rmstrEq(RedisModuleString *s, const char *c) {
    if (!s) return 0;
    size_t len; const char *p = RedisModule_StringPtrLen(s, &len);
    return len == strlen(c) && memcmp(p, c, len) == 0;
}

#define FAIL(msg) do { RedisModule_ReplyWithError(ctx, "ERR " msg); goto done; } while (0)

/* Read hash field `f` of `keyc` as-of `snap` via SnapshotOpenKey+HashGet; 1 if it
 * equals `want` (want==NULL means the field must be absent as-of-V). */
static int snapField(RedisModuleCtx *ctx, RedisModuleKeyspaceSnapshot *snap,
                     const char *keyc, const char *f, const char *want) {
    RedisModuleString *k = RedisModule_CreateString(ctx, keyc, strlen(keyc));
    RedisModuleKey *kk = RedisModule_SnapshotOpenKey(ctx, snap, k);
    if (!kk) return want == NULL ? 0 : -1; /* key not a hash as-of-V */
    RedisModuleString *v = NULL;
    RedisModule_HashGet(kk, REDISMODULE_HASH_CFIELDS, f, &v, NULL);
    RedisModule_CloseKey(kk);
    if (want == NULL) return v == NULL;
    return v && rmstrEq(v, want);
}

static int cmd_test(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    RedisModuleKeyspaceSnapshot *snap = NULL;

    /* --- field reconstruction: changed / deleted / unchanged / added --- */
    RedisModule_Call(ctx, "FLUSHALL", "");
    RedisModule_Call(ctx, "HSET", "ccccccc", "h", "f1", "a", "f2", "b", "f3", "c");
    snap = RedisModule_CreateKeyspaceSnapshot(ctx);
    if (!snap) FAIL("create returned NULL");
    RedisModule_Call(ctx, "HSET", "ccc", "h", "f1", "X"); /* change */
    RedisModule_Call(ctx, "HDEL", "cc",  "h", "f2");      /* delete */
    RedisModule_Call(ctx, "HSET", "ccc", "h", "f4", "d"); /* add */
    if (snapField(ctx, snap, "h", "f1", "a") != 1) FAIL("changed field != a");
    if (snapField(ctx, snap, "h", "f2", "b") != 1) FAIL("deleted field != b");
    if (snapField(ctx, snap, "h", "f3", "c") != 1) FAIL("unchanged field != c");
    if (snapField(ctx, snap, "h", "f4", NULL) != 1) FAIL("added field not absent as-of-V");

    /* --- cheap per-field read (no whole-hash materialize) --- */
    RedisModuleString *hk = RedisModule_CreateString(ctx, "h", 1);
    RedisModuleString *f1 = RedisModule_CreateString(ctx, "f1", 2);
    RedisModuleString *f4 = RedisModule_CreateString(ctx, "f4", 2);
    RedisModuleString *v = RedisModule_SnapshotHashGet(ctx, snap, hk, f1);
    if (!rmstrEq(v, "a")) FAIL("SnapshotHashGet f1 != a");
    if (RedisModule_SnapshotHashGet(ctx, snap, hk, f4) != NULL) FAIL("SnapshotHashGet f4 not absent");
    RedisModule_FreeKeyspaceSnapshot(ctx, snap); snap = NULL;
    RedisModuleCallReply *r = RedisModule_Call(ctx, "HGET", "cc", "h", "f1");
    if (!rmstrEq(RedisModule_CreateStringFromCallReply(r), "X")) FAIL("live f1 != X");

    /* --- whole-key DELETE materializes the as-of-V hash --- */
    RedisModule_Call(ctx, "FLUSHALL", "");
    RedisModule_Call(ctx, "HSET", "ccccc", "d", "x", "1", "y", "2");
    snap = RedisModule_CreateKeyspaceSnapshot(ctx);
    RedisModule_Call(ctx, "HSET", "ccc", "d", "x", "9"); /* change x -> recorded */
    RedisModule_Call(ctx, "DEL", "c", "d");              /* materialize before free */
    if (snapField(ctx, snap, "d", "x", "1") != 1) FAIL("after DEL: x != 1");
    if (snapField(ctx, snap, "d", "y", "2") != 1) FAIL("after DEL: y (untouched) != 2");
    RedisModule_FreeKeyspaceSnapshot(ctx, snap); snap = NULL;

    /* --- whole-key OVERWRITE (hash replaced by a string) preserves as-of-V hash --- */
    RedisModule_Call(ctx, "FLUSHALL", "");
    RedisModule_Call(ctx, "HSET", "ccccc", "o", "a", "1", "b", "2");
    snap = RedisModule_CreateKeyspaceSnapshot(ctx);
    RedisModule_Call(ctx, "SET", "cc", "o", "nowastring"); /* overwrite hash w/ string */
    if (snapField(ctx, snap, "o", "a", "1") != 1) FAIL("after OVERWRITE: a != 1");
    if (snapField(ctx, snap, "o", "b", "2") != 1) FAIL("after OVERWRITE: b != 2");
    RedisModule_FreeKeyspaceSnapshot(ctx, snap); snap = NULL;

    /* --- reject non-hash: opening a string key via the snapshot API returns NULL --- */
    RedisModule_Call(ctx, "FLUSHALL", "");
    RedisModule_Call(ctx, "SET", "cc", "s", "v");
    snap = RedisModule_CreateKeyspaceSnapshot(ctx);
    RedisModuleString *sk = RedisModule_CreateString(ctx, "s", 1);
    RedisModuleKey *skk = RedisModule_SnapshotOpenKey(ctx, snap, sk);
    if (skk != NULL) { RedisModule_CloseKey(skk); FAIL("non-hash key was not rejected"); }
    RedisModule_FreeKeyspaceSnapshot(ctx, snap); snap = NULL;

    RedisModule_ReplyWithSimpleString(ctx, "OK");
done:
    if (snap) RedisModule_FreeKeyspaceSnapshot(ctx, snap);
    return REDISMODULE_OK;
}

/* --- threaded read: create on the main thread, read a hash field on a worker
 *     under the GIL, while a concurrent write hits the main thread ---------- */
typedef struct {
    RedisModuleBlockedClient *bc;
    RedisModuleKeyspaceSnapshot *snap;
    char *key; size_t klen; char *field; long long delay_ms;
} ThreadArg;
typedef struct { int found; char *val; size_t len; } ThreadResult;

static int threadget_reply(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv); REDISMODULE_NOT_USED(argc);
    ThreadResult *res = RedisModule_GetBlockedClientPrivateData(ctx);
    if (res->found) return RedisModule_ReplyWithStringBuffer(ctx, res->val, res->len);
    return RedisModule_ReplyWithNull(ctx);
}
static int threadget_timeout(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv); REDISMODULE_NOT_USED(argc);
    return RedisModule_ReplyWithSimpleString(ctx, "TIMEOUT");
}
static void threadget_free(RedisModuleCtx *ctx, void *privdata) {
    REDISMODULE_NOT_USED(ctx); ThreadResult *res = privdata;
    if (res->val) RedisModule_Free(res->val);
    RedisModule_Free(res);
}
static void *threadget_main(void *arg) {
    ThreadArg *ta = arg;
    struct timespec ts = { ta->delay_ms / 1000, (ta->delay_ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
    RedisModuleCtx *tctx = RedisModule_GetThreadSafeContext(ta->bc);
    RedisModule_ThreadSafeContextLock(tctx);
    RedisModuleString *kn = RedisModule_CreateString(tctx, ta->key, ta->klen);
    RedisModuleKey *k = RedisModule_SnapshotOpenKey(tctx, ta->snap, kn);
    ThreadResult *res = RedisModule_Alloc(sizeof(*res));
    res->found = 0; res->val = NULL; res->len = 0;
    if (k) {
        RedisModuleString *val = NULL;
        RedisModule_HashGet(k, REDISMODULE_HASH_CFIELDS, ta->field, &val, NULL);
        if (val) { size_t vl; const char *vp = RedisModule_StringPtrLen(val, &vl);
            res->val = RedisModule_Alloc(vl ? vl : 1); memcpy(res->val, vp, vl);
            res->len = vl; res->found = 1; RedisModule_FreeString(tctx, val); }
        RedisModule_CloseKey(k);
    }
    RedisModule_FreeString(tctx, kn);
    RedisModule_FreeKeyspaceSnapshot(tctx, ta->snap);
    RedisModule_ThreadSafeContextUnlock(tctx);
    RedisModule_FreeThreadSafeContext(tctx);
    RedisModule_UnblockClient(ta->bc, res);
    RedisModule_Free(ta->key); RedisModule_Free(ta->field); RedisModule_Free(ta);
    return NULL;
}
/* KVSNAP.THREADGET <key> <field> <delay_ms> */
static int cmd_threadget(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) return RedisModule_WrongArity(ctx);
    long long delay;
    if (RedisModule_StringToLongLong(argv[3], &delay) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "ERR invalid delay");
    RedisModuleKeyspaceSnapshot *snap = RedisModule_CreateKeyspaceSnapshot(ctx);
    RedisModuleBlockedClient *bc =
        RedisModule_BlockClient(ctx, threadget_reply, threadget_timeout, threadget_free, 0);
    ThreadArg *ta = RedisModule_Alloc(sizeof(*ta));
    ta->bc = bc; ta->snap = snap; ta->delay_ms = delay;
    size_t kl, fl;
    const char *kp = RedisModule_StringPtrLen(argv[1], &kl);
    const char *fp = RedisModule_StringPtrLen(argv[2], &fl);
    ta->key = RedisModule_Alloc(kl ? kl : 1); memcpy(ta->key, kp, kl); ta->klen = kl;
    ta->field = RedisModule_Alloc(fl + 1); memcpy(ta->field, fp, fl); ta->field[fl] = '\0';
    pthread_t tid;
    if (pthread_create(&tid, NULL, threadget_main, ta) != 0) {
        RedisModule_AbortBlock(bc); RedisModule_FreeKeyspaceSnapshot(ctx, snap);
        RedisModule_Free(ta->key); RedisModule_Free(ta->field); RedisModule_Free(ta);
        return RedisModule_ReplyWithError(ctx, "ERR pthread_create failed");
    }
    pthread_detach(tid);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv); REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "kvsnapshot", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "kvsnap.test", cmd_test, "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "kvsnap.threadget", cmd_threadget, "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

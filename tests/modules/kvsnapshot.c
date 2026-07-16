/* Test module for keyspace value-MVCC snapshots (RM_CreateKeyspaceSnapshot etc).
 *
 * Exposes:
 *   KVSNAP.TEST      -- runs an in-module scenario, replies +OK or an error
 *                       string naming the first failed check.
 *   KVSNAP.THREADGET -- create on the main thread, read on a worker under the
 *                       GIL (concurrent-read consistency check).
 */

#include "redismodule.h"
#include <string.h>
#include <pthread.h>
#include <time.h>

/* Compare a RedisModuleString to a C string; returns 1 on match. */
static int rmstrEq(RedisModuleString *s, const char *c) {
    if (!s) return 0;
    size_t len;
    const char *p = RedisModule_StringPtrLen(s, &len);
    return len == strlen(c) && memcmp(p, c, len) == 0;
}

#define FAIL(msg) do { RedisModule_ReplyWithError(ctx, "ERR " msg); goto done; } while (0)

/* A trivial module data type (a heap long long) that registers a `copy`
 * callback — like RedisJSON — so we can exercise snapshotting of module-type
 * values. */
static RedisModuleType *MyType;

static void *MyType_RdbLoad(RedisModuleIO *rdb, int encver) {
    if (encver != 0) return NULL;
    long long *v = RedisModule_Alloc(sizeof(*v));
    *v = RedisModule_LoadSigned(rdb);
    return v;
}
static void MyType_RdbSave(RedisModuleIO *rdb, void *value) {
    RedisModule_SaveSigned(rdb, *(long long *)value);
}
static void MyType_Free(void *value) { RedisModule_Free(value); }
static void *MyType_Copy(RedisModuleString *fromkey, RedisModuleString *tokey, const void *value) {
    REDISMODULE_NOT_USED(fromkey); REDISMODULE_NOT_USED(tokey);
    long long *c = RedisModule_Alloc(sizeof(*c));
    *c = *(const long long *)value;
    return c;
}

static int cmd_test(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);

    RedisModuleKeyspaceSnapshot *snap = NULL;
    RedisModuleKey *k = NULL;

    /* Setup: a hash, a string, and a to-be-deleted string. */
    RedisModule_Call(ctx, "FLUSHALL", "");
    RedisModule_Call(ctx, "HSET", "ccc", "h", "f", "v1");
    RedisModule_Call(ctx, "SET",  "cc",  "s", "a");
    RedisModule_Call(ctx, "SET",  "cc",  "d", "x");

    /* Snapshot, then mutate all three. */
    snap = RedisModule_CreateKeyspaceSnapshot(ctx, NULL);
    if (!snap) FAIL("create snapshot returned NULL");

    RedisModule_Call(ctx, "HSET", "ccc", "h", "f", "v2");
    RedisModule_Call(ctx, "SET",  "cc",  "s", "b");
    RedisModule_Call(ctx, "DEL",  "c",   "d");

    /* --- snapshot sees the pre-mutation hash field --- */
    RedisModuleString *hkey = RedisModule_CreateString(ctx, "h", 1);
    k = RedisModule_SnapshotOpenKey(ctx, snap, hkey);
    if (!k) FAIL("snapshot open hash returned NULL");
    if (RedisModule_KeyType(k) != REDISMODULE_KEYTYPE_HASH) FAIL("snapshot hash wrong type");
    RedisModuleString *hval = NULL;
    RedisModule_HashGet(k, REDISMODULE_HASH_CFIELDS, "f", &hval, NULL);
    if (!rmstrEq(hval, "v1")) FAIL("snapshot hash field != v1");
    RedisModule_CloseKey(k); k = NULL;

    /* --- snapshot sees the pre-mutation string --- */
    RedisModuleString *skey = RedisModule_CreateString(ctx, "s", 1);
    k = RedisModule_SnapshotOpenKey(ctx, snap, skey);
    if (!k) FAIL("snapshot open string returned NULL");
    size_t slen;
    char *sbuf = RedisModule_StringDMA(k, &slen, REDISMODULE_READ);
    if (!(slen == 1 && sbuf[0] == 'a')) FAIL("snapshot string != a");
    RedisModule_CloseKey(k); k = NULL;

    /* --- snapshot still sees the deleted key's value --- */
    RedisModuleString *dkey = RedisModule_CreateString(ctx, "d", 1);
    k = RedisModule_SnapshotOpenKey(ctx, snap, dkey);
    if (!k) FAIL("snapshot open deleted key returned NULL");
    char *dbuf = RedisModule_StringDMA(k, &slen, REDISMODULE_READ);
    if (!(slen == 1 && dbuf[0] == 'x')) FAIL("snapshot deleted-key value != x");
    RedisModule_CloseKey(k); k = NULL;

    /* --- live keyspace reflects the mutations --- */
    RedisModuleCallReply *r;
    r = RedisModule_Call(ctx, "HGET", "cc", "h", "f");
    RedisModuleString *live = RedisModule_CreateStringFromCallReply(r);
    if (!rmstrEq(live, "v2")) FAIL("live hash field != v2");
    r = RedisModule_Call(ctx, "GET", "c", "s");
    live = RedisModule_CreateStringFromCallReply(r);
    if (!rmstrEq(live, "b")) FAIL("live string != b");
    r = RedisModule_Call(ctx, "EXISTS", "c", "d");
    if (RedisModule_CallReplyInteger(r) != 0) FAIL("live deleted key still exists");

    RedisModule_FreeKeyspaceSnapshot(ctx, snap); snap = NULL;

    /* --- after free, live data intact --- */
    r = RedisModule_Call(ctx, "GET", "c", "s");
    live = RedisModule_CreateStringFromCallReply(r);
    if (!rmstrEq(live, "b")) FAIL("live string after free != b");

    /* --- module-type value snapshot: clone via the type's copy callback --- */
    RedisModuleString *mk = RedisModule_CreateString(ctx, "mkey", 4);
    RedisModuleKey *mkk = RedisModule_OpenKey(ctx, mk, REDISMODULE_WRITE);
    long long *mv0 = RedisModule_Alloc(sizeof(*mv0)); *mv0 = 100;
    RedisModule_ModuleTypeSetValue(mkk, MyType, mv0);
    RedisModule_CloseKey(mkk);
    snap = RedisModule_CreateKeyspaceSnapshot(ctx, NULL);
    /* Mutate the module value in place — goes through the write-lookup hook,
     * which clones the pre-mutation value (100) into the snapshot. */
    mkk = RedisModule_OpenKey(ctx, mk, REDISMODULE_WRITE);
    long long *mvlive = RedisModule_ModuleTypeGetValue(mkk); *mvlive = 200;
    RedisModule_CloseKey(mkk);
    RedisModuleKey *msk = RedisModule_SnapshotOpenKey(ctx, snap, mk);
    if (!msk) FAIL("snapshot open module key returned NULL");
    if (RedisModule_ModuleTypeGetType(msk) != MyType) FAIL("snapshot module wrong type");
    long long *mfrozen = RedisModule_ModuleTypeGetValue(msk);
    if (!mfrozen || *mfrozen != 100) FAIL("snapshot module value != 100");
    RedisModule_CloseKey(msk); msk = NULL;
    RedisModule_FreeKeyspaceSnapshot(ctx, snap); snap = NULL;
    mkk = RedisModule_OpenKey(ctx, mk, REDISMODULE_READ);
    long long *mvchk = RedisModule_ModuleTypeGetValue(mkk);
    if (!mvchk || *mvchk != 200) { RedisModule_CloseKey(mkk); FAIL("live module value != 200"); }
    RedisModule_CloseKey(mkk);

    /* --- DB scoping: a write in another DB must not leak into this snapshot ---
     * Save/restore the selected DB: SelectDb changes the calling client's DB, so
     * leaking it would break later commands on this connection. */
    int origdb = RedisModule_GetSelectedDb(ctx);
    int otherdb = (origdb == 1) ? 2 : 1;
    RedisModule_Call(ctx, "SET", "cc", "dbk", "zero");     /* origdb */
    snap = RedisModule_CreateKeyspaceSnapshot(ctx, NULL);  /* scoped to origdb */
    RedisModule_SelectDb(ctx, otherdb);
    RedisModule_Call(ctx, "SET", "cc", "dbk", "one");      /* other-DB write: must not preserve */
    RedisModule_SelectDb(ctx, origdb);
    RedisModule_Call(ctx, "SET", "cc", "dbk", "zeroTwo");  /* origdb write: preserves "zero" */
    RedisModuleString *dbkey = RedisModule_CreateString(ctx, "dbk", 3);
    k = RedisModule_SnapshotOpenKey(ctx, snap, dbkey);
    if (!k) FAIL("snapshot open dbk returned NULL");
    char *dkb = RedisModule_StringDMA(k, &slen, REDISMODULE_READ);
    if (!(slen == 4 && memcmp(dkb, "zero", 4) == 0)) FAIL("db-scoped snapshot leaked a cross-DB value");
    RedisModule_CloseKey(k); k = NULL;
    RedisModule_FreeKeyspaceSnapshot(ctx, snap); snap = NULL;
    /* Clean up the other-DB key and make sure the selected DB is restored. */
    RedisModule_SelectDb(ctx, otherdb);
    RedisModule_Call(ctx, "DEL", "c", "dbk");
    RedisModule_SelectDb(ctx, origdb);

    /* --- config struct: prefix scoping (exercises the cfg path of Create) ---
     * A snapshot scoped to "keep:" preserves in-scope keys but not others; an
     * out-of-scope key reads live (not preserved). */
    RedisModule_Call(ctx, "SET", "cc", "keep:x", "old");
    RedisModule_Call(ctx, "SET", "cc", "skip:y", "old");
    RedisModuleKeyspaceSnapshotConfig pcfg = {
        .version = REDISMODULE_KEYSPACE_SNAPSHOT_CONFIG_VERSION,
        .prefix = "keep:", .prefix_len = 5,
    };
    snap = RedisModule_CreateKeyspaceSnapshot(ctx, &pcfg);
    RedisModule_Call(ctx, "SET", "cc", "keep:x", "new");
    RedisModule_Call(ctx, "SET", "cc", "skip:y", "new");
    RedisModuleString *kx = RedisModule_CreateString(ctx, "keep:x", 6);
    RedisModuleString *ky = RedisModule_CreateString(ctx, "skip:y", 6);
    size_t plen; char *pbuf;
    k = RedisModule_SnapshotOpenKey(ctx, snap, kx);   /* in scope -> preserved -> old */
    pbuf = k ? RedisModule_StringDMA(k, &plen, REDISMODULE_READ) : NULL;
    if (!(pbuf && plen == 3 && pbuf[0] == 'o')) FAIL("prefix cfg: in-scope key != old");
    RedisModule_CloseKey(k); k = NULL;
    k = RedisModule_SnapshotOpenKey(ctx, snap, ky);   /* out of scope -> live "new" */
    pbuf = k ? RedisModule_StringDMA(k, &plen, REDISMODULE_READ) : NULL;
    if (!(pbuf && plen == 3 && pbuf[0] == 'n')) FAIL("prefix cfg: out-of-scope key != new");
    RedisModule_CloseKey(k); k = NULL;
    RedisModule_FreeKeyspaceSnapshot(ctx, snap); snap = NULL;

    RedisModule_ReplyWithSimpleString(ctx, "OK");
done:
    if (k) RedisModule_CloseKey(k);
    if (snap) RedisModule_FreeKeyspaceSnapshot(ctx, snap);
    return REDISMODULE_OK;
}

/* --- threaded read: create on the main thread, read on a worker under the
 *     GIL, while a concurrent write hits the main thread ------------------- */

typedef struct {
    RedisModuleBlockedClient *bc;
    RedisModuleKeyspaceSnapshot *snap;
    char *key; size_t klen;
    char *field;                /* null-terminated (for REDISMODULE_HASH_CFIELDS) */
    long long delay_ms;
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
    REDISMODULE_NOT_USED(ctx);
    ThreadResult *res = privdata;
    if (res->val) RedisModule_Free(res->val);
    RedisModule_Free(res);
}

static void *threadget_main(void *arg) {
    ThreadArg *ta = arg;

    /* Sleep first, so the test can land a concurrent write (on the main thread)
     * between snapshot creation and this read. */
    struct timespec ts = { ta->delay_ms / 1000, (ta->delay_ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);

    RedisModuleCtx *tctx = RedisModule_GetThreadSafeContext(ta->bc);
    RedisModule_ThreadSafeContextLock(tctx);            /* acquire the GIL */

    RedisModuleString *kn = RedisModule_CreateString(tctx, ta->key, ta->klen);
    RedisModuleKey *k = RedisModule_SnapshotOpenKey(tctx, ta->snap, kn);
    ThreadResult *res = RedisModule_Alloc(sizeof(*res));
    res->found = 0; res->val = NULL; res->len = 0;
    if (k && RedisModule_KeyType(k) == REDISMODULE_KEYTYPE_HASH) {
        RedisModuleString *v = NULL;
        RedisModule_HashGet(k, REDISMODULE_HASH_CFIELDS, ta->field, &v, NULL);
        if (v) {
            size_t vl; const char *vp = RedisModule_StringPtrLen(v, &vl);
            res->val = RedisModule_Alloc(vl ? vl : 1);
            memcpy(res->val, vp, vl); res->len = vl; res->found = 1;
        }
    }
    if (k) RedisModule_CloseKey(k);
    RedisModule_FreeKeyspaceSnapshot(tctx, ta->snap); /* free under the GIL */
    RedisModule_ThreadSafeContextUnlock(tctx);
    RedisModule_FreeThreadSafeContext(tctx);

    RedisModule_UnblockClient(ta->bc, res);
    RedisModule_Free(ta->key); RedisModule_Free(ta->field); RedisModule_Free(ta);
    return NULL;
}

/* KVSNAP.THREADGET <key> <field> <delay_ms> -- create a snapshot now (main
 * thread), then read <key>.<field> as-of the snapshot from a background thread
 * after <delay_ms>. Replies the field value the snapshot sees. */
static int cmd_threadget(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) return RedisModule_WrongArity(ctx);
    long long delay;
    if (RedisModule_StringToLongLong(argv[3], &delay) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "ERR invalid delay");

    RedisModuleKeyspaceSnapshot *snap = RedisModule_CreateKeyspaceSnapshot(ctx, NULL);
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
        RedisModule_AbortBlock(bc);
        RedisModule_FreeKeyspaceSnapshot(ctx, snap);
        RedisModule_Free(ta->key); RedisModule_Free(ta->field); RedisModule_Free(ta);
        return RedisModule_ReplyWithError(ctx, "ERR pthread_create failed");
    }
    pthread_detach(tid);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "kvsnapshot", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModuleTypeMethods tm = {
        .version = REDISMODULE_TYPE_METHOD_VERSION,
        .rdb_load = MyType_RdbLoad,
        .rdb_save = MyType_RdbSave,
        .free = MyType_Free,
        .copy = MyType_Copy,
    };
    MyType = RedisModule_CreateDataType(ctx, "kvsnaptyp", 0, &tm);
    if (MyType == NULL) return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "kvsnap.test", cmd_test, "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "kvsnap.threadget", cmd_threadget, "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

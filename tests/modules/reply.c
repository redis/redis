/* 
 * A module the tests RM_ReplyWith family of commands
 */

#include "redismodule.h"
#include <math.h>
#include <assert.h>
#include <pthread.h>
#include <string.h>

int rw_string(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    return RedisModule_ReplyWithString(ctx, argv[1]);
}

int rw_cstring(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);

    return RedisModule_ReplyWithSimpleString(ctx, "A simple string");
}

int rw_int(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    long long integer;
    if (RedisModule_StringToLongLong(argv[1], &integer) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "Arg cannot be parsed as an integer");

    return RedisModule_ReplyWithLongLong(ctx, integer);
}

/* When one argument is given, it is returned as a double,
 * when two arguments are given, it returns a/b. */
int rw_double(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc==1)
        return RedisModule_ReplyWithDouble(ctx, NAN);

    if (argc != 2 && argc != 3) return RedisModule_WrongArity(ctx);

    double dbl, dbl2;
    if (RedisModule_StringToDouble(argv[1], &dbl) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "Arg cannot be parsed as a double");
    if (argc == 3) {
        if (RedisModule_StringToDouble(argv[2], &dbl2) != REDISMODULE_OK)
            return RedisModule_ReplyWithError(ctx, "Arg cannot be parsed as a double");
        dbl /= dbl2;
    }

    return RedisModule_ReplyWithDouble(ctx, dbl);
}

int rw_longdouble(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    long double longdbl;
    if (RedisModule_StringToLongDouble(argv[1], &longdbl) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "Arg cannot be parsed as a double");

    return RedisModule_ReplyWithLongDouble(ctx, longdbl);
}

int rw_bignumber(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    size_t bignum_len;
    const char *bignum_str = RedisModule_StringPtrLen(argv[1], &bignum_len);

    return RedisModule_ReplyWithBigNumber(ctx, bignum_str, bignum_len);
}

int rw_array(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    long long integer;
    if (RedisModule_StringToLongLong(argv[1], &integer) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "Arg cannot be parsed as a integer");

    RedisModule_ReplyWithArray(ctx, integer);
    for (int i = 0; i < integer; ++i) {
        RedisModule_ReplyWithLongLong(ctx, i);
    }

    return REDISMODULE_OK;
}

int rw_simplestring_array(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) return RedisModule_WrongArity(ctx);

    RedisModule_ReplyWithArray(ctx, argc - 1);
    for (int i = 1; i < argc; ++i) {
        size_t len;
        const char *str = RedisModule_StringPtrLen(argv[i], &len);
        RedisModule_ReplyWithSimpleString(ctx, str);
    }

    return REDISMODULE_OK;
}

int rw_map(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    long long integer;
    if (RedisModule_StringToLongLong(argv[1], &integer) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "Arg cannot be parsed as a integer");

    RedisModule_ReplyWithMap(ctx, integer);
    for (int i = 0; i < integer; ++i) {
        RedisModule_ReplyWithLongLong(ctx, i);
        RedisModule_ReplyWithDouble(ctx, i * 1.5);
    }

    return REDISMODULE_OK;
}

int rw_set(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    long long integer;
    if (RedisModule_StringToLongLong(argv[1], &integer) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "Arg cannot be parsed as a integer");

    RedisModule_ReplyWithSet(ctx, integer);
    for (int i = 0; i < integer; ++i) {
        RedisModule_ReplyWithLongLong(ctx, i);
    }

    return REDISMODULE_OK;
}

int rw_attribute(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    long long integer;
    if (RedisModule_StringToLongLong(argv[1], &integer) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "Arg cannot be parsed as a integer");

    if (RedisModule_ReplyWithAttribute(ctx, integer) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx, "Attributes aren't supported by RESP 2");
    }

    for (int i = 0; i < integer; ++i) {
        RedisModule_ReplyWithLongLong(ctx, i);
        RedisModule_ReplyWithDouble(ctx, i * 1.5);
    }

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int rw_bool(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);

    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithBool(ctx, 0);
    return RedisModule_ReplyWithBool(ctx, 1);
}

int rw_null(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);

    return RedisModule_ReplyWithNull(ctx);
}

int rw_error(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);

    return RedisModule_ReplyWithError(ctx, "An error");
}

int rw_error_format(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) return RedisModule_WrongArity(ctx);

    return RedisModule_ReplyWithErrorFormat(ctx,
                                            RedisModule_StringPtrLen(argv[1], NULL),
                                            RedisModule_StringPtrLen(argv[2], NULL));
}

int rw_verbatim(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    size_t verbatim_len;
    const char *verbatim_str = RedisModule_StringPtrLen(argv[1], &verbatim_len);

    return RedisModule_ReplyWithVerbatimString(ctx, verbatim_str, verbatim_len);
}

/* Reply buffers are owned by the blocked handle, including buffers whose
 * contents are discarded. These commands deliberately exercise that lifetime. */
static int rw_buffer(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) return RedisModule_WrongArity(ctx);
    const char *mode = RedisModule_StringPtrLen(argv[1], NULL);
    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);
    RedisModuleCtx *a = RedisModule_GetReplyBufferContext(bc);
    RedisModuleCtx *b = RedisModule_GetReplyBufferContext(bc);
    assert(RedisModule_GetReplyBufferContext(NULL) == NULL);
    if (RedisModule_GetContextFlags(ctx) & REDISMODULE_CTX_FLAGS_MULTI) {
        RedisModule_ReplyWithString(a, argv[2]);
        RedisModule_UnblockClient(bc, NULL);
        return REDISMODULE_OK;
    }

    assert((RedisModule_GetContextFlags(a) & REDISMODULE_CTX_FLAGS_RESP3) ==
           (RedisModule_GetContextFlags(ctx) & REDISMODULE_CTX_FLAGS_RESP3));

    if (!strcmp(mode, "reuse")) {
        RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_LEN);
        RedisModule_ReplyWithSimpleString(ctx, "before");
        RedisModule_ReplyWithArray(a, REDISMODULE_POSTPONED_LEN);
        RedisModule_ReplyWithString(a, argv[2]);
        RedisModule_ReplyWithMap(a, REDISMODULE_POSTPONED_LEN);
        RedisModule_ReplyWithSimpleString(a, "key");
        RedisModule_ReplyWithSet(a, REDISMODULE_POSTPONED_LEN);
        RedisModule_ReplyWithLongLong(a, 42);
        RedisModule_ReplySetSetLength(a, 1);
        RedisModule_ReplySetMapLength(a, 1);
        RedisModule_ReplySetArrayLength(a, 2);
        assert(RedisModule_ReplyWithBufferedReply(b, a) == REDISMODULE_OK);
        assert(RedisModule_ReplyWithBufferedReply(ctx, b) == REDISMODULE_OK);
        /* Empty moves must not duplicate bytes or affect outer placeholders. */
        assert(RedisModule_ReplyWithBufferedReply(ctx, a) == REDISMODULE_OK);
        assert(RedisModule_ReplyWithBufferedReply(ctx, b) == REDISMODULE_OK);
        RedisModule_ReplyWithBool(a, 1);
        assert(RedisModule_ReplyWithBufferedReply(ctx, a) == REDISMODULE_OK);
        RedisModule_ReplyWithSimpleString(b, "after");
        assert(RedisModule_ReplyWithBufferedReply(ctx, b) == REDISMODULE_OK);
        RedisModule_ReplySetArrayLength(ctx, 4);
    } else if (!strcmp(mode, "invalid")) {
        RedisModuleCtx *ts = RedisModule_GetThreadSafeContext(bc);
        assert(RedisModule_ReplyWithBufferedReply(ctx, ctx) == REDISMODULE_ERR);
        assert(RedisModule_ReplyWithBufferedReply(ctx, ts) == REDISMODULE_ERR);
        assert(RedisModule_ReplyWithBufferedReply(ctx, NULL) == REDISMODULE_ERR);
        RedisModule_ReplyWithArray(a, REDISMODULE_POSTPONED_LEN);
        RedisModule_ReplyWithString(a, argv[2]);
        assert(RedisModule_ReplyWithBufferedReply(ctx, a) == REDISMODULE_ERR);
        RedisModule_ReplySetArrayLength(a, 1);
        assert(RedisModule_ReplyWithBufferedReply(a, a) == REDISMODULE_ERR);
        assert(RedisModule_ReplyWithBufferedReply(ts, a) == REDISMODULE_OK);
        RedisModule_FreeThreadSafeContext(ts);
    } else if (!strcmp(mode, "errors")) {
        RedisModule_ReplyWithError(a, "BUFFERERR sent");
        assert(RedisModule_ReplyWithBufferedReply(b, a) == REDISMODULE_OK);
        assert(RedisModule_ReplyWithBufferedReply(ctx, b) == REDISMODULE_OK);
        assert(RedisModule_ReplyWithBufferedReply(ctx, a) == REDISMODULE_OK);
        assert(RedisModule_ReplyWithBufferedReply(ctx, b) == REDISMODULE_OK);
    } else {
        if (!strcmp(mode, "open"))
            RedisModule_ReplyWithArray(a, REDISMODULE_POSTPONED_LEN);
        RedisModule_ReplyWithString(a, argv[2]);
        RedisModule_ReplyWithError(a, "BUFFERDROP discarded");
        if (!strcmp(mode, "detached")) {
            RedisModuleCtx *detached = RedisModule_GetDetachedThreadSafeContext(ctx);
            assert(RedisModule_ReplyWithBufferedReply(detached, a) == REDISMODULE_OK);
            assert(RedisModule_ReplyWithBufferedReply(ctx, a) == REDISMODULE_OK);
            RedisModule_FreeThreadSafeContext(detached);
        }
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    }
    if (!strcmp(mode, "abort"))
        RedisModule_AbortBlock(bc);
    else
        RedisModule_UnblockClient(bc, NULL);
    return REDISMODULE_OK;
}

typedef struct BufferJob {
    RedisModuleBlockedClient *bc;
    RedisModuleCtx *buffer;
    char *payload;
    size_t len;
    int error;
    int ready;
    int finish;
    int taken;
} BufferJob;

/* Only one outstanding job is needed. The condition variable keeps the worker
 * alive after publication until the test explicitly permits cleanup. */
static BufferJob *buffer_job;
static long long buffer_freed;
static pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t buffer_cond = PTHREAD_COND_INITIALIZER;

static void *rw_buffer_worker(void *arg) {
    BufferJob *job = arg;
    RedisModuleCtx *ts = RedisModule_GetThreadSafeContext(job->bc);
    RedisModule_ThreadSafeContextLock(ts);
    RedisModuleCtx *staging = RedisModule_GetReplyBufferContext(job->bc);
    RedisModule_ThreadSafeContextUnlock(ts);

    /* Serialize and move between independent accumulators without the GIL. */
    if (job->error)
        RedisModule_ReplyWithError(staging, "BUFFERERR worker");
    else
        RedisModule_ReplyWithStringBuffer(staging, job->payload, job->len);
    assert(RedisModule_ReplyWithBufferedReply(job->buffer, staging) == REDISMODULE_OK);

    pthread_mutex_lock(&buffer_mutex);
    job->ready = 1;
    while (!job->finish) pthread_cond_wait(&buffer_cond, &buffer_mutex);
    pthread_mutex_unlock(&buffer_mutex);

    /* This can run after timeout or disconnect. Creation must not dereference
     * the original client, and unconsumed errors must remain unreported. */
    RedisModule_ThreadSafeContextLock(ts);
    RedisModuleCtx *late = RedisModule_GetReplyBufferContext(job->bc);
    RedisModule_ThreadSafeContextUnlock(ts);
    RedisModule_ReplyWithError(late, "BUFFERDROP late");
    RedisModule_FreeThreadSafeContext(ts);
    RedisModule_UnblockClient(job->bc, job);
    return NULL;
}

static int rw_buffer_callback(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    BufferJob *job = buffer_job;
    pthread_mutex_lock(&buffer_mutex);
    assert(job->ready);
    pthread_mutex_unlock(&buffer_mutex);
    RedisModule_ReplyWithArray(ctx, job->taken ? 1 : 2);
    assert(RedisModule_ReplyWithBufferedReply(ctx, job->buffer) == REDISMODULE_OK);
    RedisModule_ReplyWithSimpleString(ctx, "done");
    job->taken = 1;
    return REDISMODULE_OK;
}

static void rw_buffer_free(RedisModuleCtx *ctx, void *data) {
    REDISMODULE_NOT_USED(ctx);
    BufferJob *job = data;
    /* The borrowed buffer is still alive during free_privdata. */
    RedisModule_ReplyWithError(job->buffer, "BUFFERDROP free");
    RedisModule_Free(job->payload);
    RedisModule_Free(job);
    buffer_job = NULL;
    buffer_freed++;
}

static int rw_buffer_start(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) return RedisModule_WrongArity(ctx);
    if (buffer_job) return RedisModule_ReplyWithError(ctx, "ERR job already running");
    BufferJob *job = RedisModule_Calloc(1, sizeof(*job));
    const char *payload = RedisModule_StringPtrLen(argv[1], &job->len);
    job->payload = RedisModule_Alloc(job->len);
    memcpy(job->payload, payload, job->len);
    job->error = !strcmp(RedisModule_StringPtrLen(argv[2], NULL), "error");
    job->bc = RedisModule_BlockClient(ctx, rw_buffer_callback, rw_buffer_callback, rw_buffer_free, 0);
    job->buffer = RedisModule_GetReplyBufferContext(job->bc);
    buffer_job = job;
    pthread_t tid;
    assert(pthread_create(&tid, NULL, rw_buffer_worker, job) == 0);
    pthread_detach(tid);
    return REDISMODULE_OK;
}

/* Transfer to a different real client, including a client using a different
 * protocol or one with CLIENT REPLY OFF. A following empty take returns OK. */
static int rw_buffer_take(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 1 && argc != 2) return RedisModule_WrongArity(ctx);
    BufferJob *job = buffer_job;
    assert(job);
    pthread_mutex_lock(&buffer_mutex);
    assert(job->ready);
    pthread_mutex_unlock(&buffer_mutex);
    /* Under a small output limit this closes the destination before the move,
     * exercising AddReplyFromClient's early return with a nonempty source. */
    if (argc == 2 && !strcmp(RedisModule_StringPtrLen(argv[1], NULL), "close"))
        RedisModule_ReplyWithStringBuffer(ctx, job->payload, job->len);
    if (RedisModule_ReplyWithBufferedReply(ctx, job->buffer) == REDISMODULE_ERR)
        return RedisModule_ReplyWithError(ctx, "ERR incompatible protocol");
    if (job->taken) RedisModule_ReplyWithSimpleString(ctx, "OK");
    job->taken = 1;
    return REDISMODULE_OK;
}

static int rw_buffer_finish(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);
    pthread_mutex_lock(&buffer_mutex);
    assert(buffer_job && buffer_job->ready);
    buffer_job->finish = 1;
    pthread_cond_signal(&buffer_cond);
    pthread_mutex_unlock(&buffer_mutex);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

static int rw_buffer_status(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);
    pthread_mutex_lock(&buffer_mutex);
    RedisModule_ReplyWithArray(ctx, 3);
    RedisModule_ReplyWithLongLong(ctx, buffer_job != NULL);
    RedisModule_ReplyWithLongLong(ctx, buffer_job && buffer_job->ready);
    RedisModule_ReplyWithLongLong(ctx, buffer_freed);
    pthread_mutex_unlock(&buffer_mutex);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "replywith", 1, REDISMODULE_APIVER_1) != REDISMODULE_OK)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"rw.string",rw_string,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.cstring",rw_cstring,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.bignumber",rw_bignumber,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.int",rw_int,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.double",rw_double,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.longdouble",rw_longdouble,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.array",rw_array,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.simplestring_array",rw_simplestring_array,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.map",rw_map,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.attribute",rw_attribute,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.set",rw_set,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.bool",rw_bool,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.null",rw_null,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.error",rw_error,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.error_format",rw_error_format,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.verbatim",rw_verbatim,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"rw.buffer",rw_buffer,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.buffer_start",rw_buffer_start,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.buffer_take",rw_buffer_take,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.buffer_finish",rw_buffer_finish,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.buffer_status",rw_buffer_status,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

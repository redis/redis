/* define macros for having usleep */
#define _BSD_SOURCE
#define _DEFAULT_SOURCE
#include <unistd.h>

#include "redismodule.h"
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <strings.h>

#define UNUSED(V) ((void) V)

/* used to test processing events during slow bg operation */
static volatile int g_slow_bg_operation = 0;
static volatile int g_is_in_slow_bg_operation = 0;

void *sub_worker(void *arg) {
    // Get Redis module context
    RedisModuleCtx *ctx = (RedisModuleCtx *)arg;

    // Try acquiring GIL
    int res = RedisModule_ThreadSafeContextTryLock(ctx);

    // GIL is already taken by the calling thread expecting to fail.
    assert(res != REDISMODULE_OK);

    return NULL;
}

void *worker(void *arg) {
    // Retrieve blocked client
    RedisModuleBlockedClient *bc = (RedisModuleBlockedClient *)arg;

    // Get Redis module context
    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(bc);

    // Acquire GIL
    RedisModule_ThreadSafeContextLock(ctx);

    // Create another thread which will try to acquire the GIL
    pthread_t tid;
    int res = pthread_create(&tid, NULL, sub_worker, ctx);
    assert(res == 0);

    // Wait for thread
    pthread_join(tid, NULL);

    // Release GIL
    RedisModule_ThreadSafeContextUnlock(ctx);

    // Reply to client
    RedisModule_ReplyWithSimpleString(ctx, "OK");

    // Unblock client
    RedisModule_UnblockClient(bc, NULL);

    // Free the Redis module context
    RedisModule_FreeThreadSafeContext(ctx);

    return NULL;
}

int acquire_gil(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    UNUSED(argv);
    UNUSED(argc);

    int flags = RedisModule_GetContextFlags(ctx);
    int allFlags = RedisModule_GetContextFlagsAll();
    if ((allFlags & REDISMODULE_CTX_FLAGS_MULTI) &&
        (flags & REDISMODULE_CTX_FLAGS_MULTI)) {
        RedisModule_ReplyWithSimpleString(ctx, "Blocked client is not supported inside multi");
        return REDISMODULE_OK;
    }

    if ((allFlags & REDISMODULE_CTX_FLAGS_DENY_BLOCKING) &&
        (flags & REDISMODULE_CTX_FLAGS_DENY_BLOCKING)) {
        RedisModule_ReplyWithSimpleString(ctx, "Blocked client is not allowed");
        return REDISMODULE_OK;
    }

    /* This command handler tries to acquire the GIL twice
     * once in the worker thread using "RedisModule_ThreadSafeContextLock"
     * second in the sub-worker thread
     * using "RedisModule_ThreadSafeContextTryLock"
     * as the GIL is already locked. */
    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);

    pthread_t tid;
    int res = pthread_create(&tid, NULL, worker, bc);
    assert(res == 0);

    return REDISMODULE_OK;
}

typedef struct {
    RedisModuleString **argv;
    int argc;
    RedisModuleBlockedClient *bc;
} bg_call_data;

void *bg_call_worker(void *arg) {
    bg_call_data *bg = arg;

    // Get Redis module context
    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(bg->bc);

    // Acquire GIL
    RedisModule_ThreadSafeContextLock(ctx);

    // Test slow operation yielding
    if (g_slow_bg_operation) {
        g_is_in_slow_bg_operation = 1;
        while (g_slow_bg_operation) {
            RedisModule_Yield(ctx, REDISMODULE_YIELD_FLAG_CLIENTS, "Slow module operation");
            usleep(1000);
        }
        g_is_in_slow_bg_operation = 0;
    }

    // Call the command
    const char *module_cmd = RedisModule_StringPtrLen(bg->argv[0], NULL);
    int cmd_pos = 1;
    RedisModuleString *format_redis_str = RedisModule_CreateString(NULL, "v", 1);
    if (!strcasecmp(module_cmd, "do_bg_rm_call_format")) {
        cmd_pos = 2;
        size_t format_len;
        const char *format = RedisModule_StringPtrLen(bg->argv[1], &format_len);
        RedisModule_StringAppendBuffer(NULL, format_redis_str, format, format_len);
        RedisModule_StringAppendBuffer(NULL, format_redis_str, "E", 1);
    }
    const char *format = RedisModule_StringPtrLen(format_redis_str, NULL);
    const char *cmd = RedisModule_StringPtrLen(bg->argv[cmd_pos], NULL);
    RedisModuleCallReply *rep = RedisModule_Call(ctx, cmd, format, bg->argv + cmd_pos + 1, bg->argc - cmd_pos - 1);
    RedisModule_FreeString(NULL, format_redis_str);

    // Release GIL
    RedisModule_ThreadSafeContextUnlock(ctx);

    // Reply to client
    if (!rep) {
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }

    // Unblock client
    RedisModule_UnblockClient(bg->bc, NULL);

    /* Free the arguments */
    for (int i=0; i<bg->argc; i++)
        RedisModule_FreeString(ctx, bg->argv[i]);
    RedisModule_Free(bg->argv);
    RedisModule_Free(bg);

    // Free the Redis module context
    RedisModule_FreeThreadSafeContext(ctx);

    return NULL;
}

int do_bg_rm_call(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    UNUSED(argv);
    UNUSED(argc);

    /* Make sure we're not trying to block a client when we shouldn't */
    int flags = RedisModule_GetContextFlags(ctx);
    int allFlags = RedisModule_GetContextFlagsAll();
    if ((allFlags & REDISMODULE_CTX_FLAGS_MULTI) &&
        (flags & REDISMODULE_CTX_FLAGS_MULTI)) {
        RedisModule_ReplyWithSimpleString(ctx, "Blocked client is not supported inside multi");
        return REDISMODULE_OK;
    }
    if ((allFlags & REDISMODULE_CTX_FLAGS_DENY_BLOCKING) &&
        (flags & REDISMODULE_CTX_FLAGS_DENY_BLOCKING)) {
        RedisModule_ReplyWithSimpleString(ctx, "Blocked client is not allowed");
        return REDISMODULE_OK;
    }

    /* Make a copy of the arguments and pass them to the thread. */
    bg_call_data *bg = RedisModule_Alloc(sizeof(bg_call_data));
    bg->argv = RedisModule_Alloc(sizeof(RedisModuleString*)*argc);
    bg->argc = argc;
    for (int i=0; i<argc; i++)
        bg->argv[i] = RedisModule_HoldString(ctx, argv[i]);

    /* Block the client */
    bg->bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);

    /* Start a thread to handle the request */
    pthread_t tid;
    int res = pthread_create(&tid, NULL, bg_call_worker, bg);
    assert(res == 0);

    return REDISMODULE_OK;
}

int do_rm_call(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    UNUSED(argv);
    UNUSED(argc);

    if(argc < 2){
        return RedisModule_WrongArity(ctx);
    }

    const char* cmd = RedisModule_StringPtrLen(argv[1], NULL);

    RedisModuleCallReply* rep = RedisModule_Call(ctx, cmd, "Ev", argv + 2, argc - 2);
    if(!rep){
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    }else{
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }

    return REDISMODULE_OK;
}

static void rm_call_async_send_reply(RedisModuleCtx *ctx, RedisModuleCallReply *reply) {
    RedisModule_ReplyWithCallReply(ctx, reply);
    RedisModule_FreeCallReply(reply);
}

/* Called when the command that was blocked on 'RM_Call' gets unblocked
 * and send the reply to the blocked client. */
static void rm_call_async_on_unblocked(RedisModuleCtx *ctx, RedisModuleCallReply *reply, void *private_data) {
    UNUSED(ctx);
    RedisModuleBlockedClient *bc = private_data;
    RedisModuleCtx *bctx = RedisModule_GetThreadSafeContext(bc);
    rm_call_async_send_reply(bctx, reply);
    RedisModule_FreeThreadSafeContext(bctx);
    RedisModule_UnblockClient(bc, RedisModule_BlockClientGetPrivateData(bc));
}

int do_rm_call_async_fire_and_forget(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    UNUSED(argv);
    UNUSED(argc);

    if(argc < 2){
        return RedisModule_WrongArity(ctx);
    }
    const char* cmd = RedisModule_StringPtrLen(argv[1], NULL);

    RedisModuleCallReply* rep = RedisModule_Call(ctx, cmd, "!KEv", argv + 2, argc - 2);

    if(RedisModule_CallReplyType(rep) != REDISMODULE_REPLY_PROMISE) {
        RedisModule_ReplyWithCallReply(ctx, rep);
    } else {
        RedisModule_ReplyWithSimpleString(ctx, "Blocked");
    }
    RedisModule_FreeCallReply(rep);

    return REDISMODULE_OK;
}

static void do_rm_call_async_free_pd(RedisModuleCtx * ctx, void *pd) {
    UNUSED(ctx);
    RedisModule_FreeCallReply(pd);
}

static void do_rm_call_async_disconnect(RedisModuleCtx *ctx, struct RedisModuleBlockedClient *bc) {
    UNUSED(ctx);
    RedisModuleCallReply* rep = RedisModule_BlockClientGetPrivateData(bc);
    RedisModule_CallReplyPromiseAbort(rep, NULL);
    RedisModule_FreeCallReply(rep);
    RedisModule_AbortBlock(bc);
}

/*
 * Callback for do_rm_call_async / do_rm_call_async_script_mode
 * Gets the command to invoke as the first argument to the command and runs it,
 * passing the rest of the arguments to the command invocation.
 * If the command got blocked, blocks the client and unblock it when the command gets unblocked,
 * this allows check the K (allow blocking) argument to RM_Call.
 */
int do_rm_call_async(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    UNUSED(argv);
    UNUSED(argc);

    if(argc < 2){
        return RedisModule_WrongArity(ctx);
    }

    size_t format_len = 0;
    char format[6] = {0};

    if (!(RedisModule_GetContextFlags(ctx) & REDISMODULE_CTX_FLAGS_DENY_BLOCKING)) {
        /* We are allowed to block the client so we can allow RM_Call to also block us */
        format[format_len++] = 'K';
    }

    const char* invoked_cmd = RedisModule_StringPtrLen(argv[0], NULL);
    if (strcasecmp(invoked_cmd, "do_rm_call_async_script_mode") == 0) {
        format[format_len++] = 'S';
    }

    format[format_len++] = 'E';
    format[format_len++] = 'v';
    if (strcasecmp(invoked_cmd, "do_rm_call_async_no_replicate") != 0) {
        /* Notice, without the '!' flag we will have inconsistency between master and replica.
         * This is used only to check '!' flag correctness on blocked commands. */
        format[format_len++] = '!';
    }

    const char* cmd = RedisModule_StringPtrLen(argv[1], NULL);

    RedisModuleCallReply* rep = RedisModule_Call(ctx, cmd, format, argv + 2, argc - 2);

    if(RedisModule_CallReplyType(rep) != REDISMODULE_REPLY_PROMISE) {
        rm_call_async_send_reply(ctx, rep);
    } else {
        RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, NULL, NULL, do_rm_call_async_free_pd, 0);
        RedisModule_SetDisconnectCallback(bc, do_rm_call_async_disconnect);
        RedisModule_BlockClientSetPrivateData(bc, rep);
        RedisModule_CallReplyPromiseSetUnblockHandler(rep, rm_call_async_on_unblocked, bc);
    }

    return REDISMODULE_OK;
}

/* Private data for wait_and_do_rm_call_async that holds information about:
 * 1. the block client, to unblock when done.
 * 2. the arguments, contains the command to run using RM_Call */
typedef struct WaitAndDoRMCallCtx {
    RedisModuleBlockedClient *bc;
    RedisModuleString **argv;
    int argc;
} WaitAndDoRMCallCtx;

/*
 * This callback will be called when the 'wait' command invoke on 'wait_and_do_rm_call_async' will finish.
 * This callback will continue the execution flow just like 'do_rm_call_async' command.
 */
static void wait_and_do_rm_call_async_on_unblocked(RedisModuleCtx *ctx, RedisModuleCallReply *reply, void *private_data) {
    WaitAndDoRMCallCtx *wctx = private_data;
    if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_INTEGER) {
        goto done;
    }

    if (RedisModule_CallReplyInteger(reply) != 1) {
        goto done;
    }

    RedisModule_FreeCallReply(reply);
    reply = NULL;

    const char* cmd = RedisModule_StringPtrLen(wctx->argv[0], NULL);
    reply = RedisModule_Call(ctx, cmd, "!EKv", wctx->argv + 1, wctx->argc - 1);

done:
    if(RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_PROMISE) {
        RedisModuleCtx *bctx = RedisModule_GetThreadSafeContext(wctx->bc);
        rm_call_async_send_reply(bctx, reply);
        RedisModule_FreeThreadSafeContext(bctx);
        RedisModule_UnblockClient(wctx->bc, NULL);
    } else {
        RedisModule_CallReplyPromiseSetUnblockHandler(reply, rm_call_async_on_unblocked, wctx->bc);
        RedisModule_FreeCallReply(reply);
    }
    for (int i = 0 ; i < wctx->argc ; ++i) {
        RedisModule_FreeString(NULL, wctx->argv[i]);
    }
    RedisModule_Free(wctx->argv);
    RedisModule_Free(wctx);
}

/*
 * Callback for wait_and_do_rm_call
 * Gets the command to invoke as the first argument, runs 'wait'
 * command (using the K flag to RM_Call). Once the wait finished, runs the
 * command that was given (just like 'do_rm_call_async').
 */
int wait_and_do_rm_call_async(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    if(argc < 2){
        return RedisModule_WrongArity(ctx);
    }

    int flags = RedisModule_GetContextFlags(ctx);
    if (flags & REDISMODULE_CTX_FLAGS_DENY_BLOCKING) {
        return RedisModule_ReplyWithError(ctx, "Err can not run wait, blocking is not allowed.");
    }

    RedisModuleCallReply* rep = RedisModule_Call(ctx, "wait", "!EKcc", "1", "0");
    if(RedisModule_CallReplyType(rep) != REDISMODULE_REPLY_PROMISE) {
        rm_call_async_send_reply(ctx, rep);
    } else {
        RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);
        WaitAndDoRMCallCtx *wctx = RedisModule_Alloc(sizeof(*wctx));
        *wctx = (WaitAndDoRMCallCtx){
                .bc = bc,
                .argv = RedisModule_Alloc((argc - 1) * sizeof(RedisModuleString*)),
                .argc = argc - 1,
        };

        for (int i = 1 ; i < argc ; ++i) {
            wctx->argv[i - 1] = RedisModule_HoldString(NULL, argv[i]);
        }
        RedisModule_CallReplyPromiseSetUnblockHandler(rep, wait_and_do_rm_call_async_on_unblocked, wctx);
        RedisModule_FreeCallReply(rep);
    }

    return REDISMODULE_OK;
}

static void blpop_and_set_multiple_keys_on_unblocked(RedisModuleCtx *ctx, RedisModuleCallReply *reply, void *private_data) {
    /* ignore the reply */
    RedisModule_FreeCallReply(reply);
    WaitAndDoRMCallCtx *wctx = private_data;
    for (int i = 0 ; i < wctx->argc ; i += 2) {
        RedisModuleCallReply* rep = RedisModule_Call(ctx, "set", "!ss", wctx->argv[i], wctx->argv[i + 1]);
        RedisModule_FreeCallReply(rep);
    }

    RedisModuleCtx *bctx = RedisModule_GetThreadSafeContext(wctx->bc);
    RedisModule_ReplyWithSimpleString(bctx, "OK");
    RedisModule_FreeThreadSafeContext(bctx);
    RedisModule_UnblockClient(wctx->bc, NULL);

    for (int i = 0 ; i < wctx->argc ; ++i) {
        RedisModule_FreeString(NULL, wctx->argv[i]);
    }
    RedisModule_Free(wctx->argv);
    RedisModule_Free(wctx);

}

/*
 * Performs a blpop command on a given list and when unblocked set multiple string keys.
 * This command allows checking that the unblock callback is performed as a unit
 * and its effect are replicated to the replica and AOF wrapped with multi exec.
 */
int blpop_and_set_multiple_keys(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    if(argc < 2 || argc % 2 != 0){
        return RedisModule_WrongArity(ctx);
    }

    int flags = RedisModule_GetContextFlags(ctx);
    if (flags & REDISMODULE_CTX_FLAGS_DENY_BLOCKING) {
        return RedisModule_ReplyWithError(ctx, "Err can not run wait, blocking is not allowed.");
    }

    RedisModuleCallReply* rep = RedisModule_Call(ctx, "blpop", "!EKsc", argv[1], "0");
    if(RedisModule_CallReplyType(rep) != REDISMODULE_REPLY_PROMISE) {
        rm_call_async_send_reply(ctx, rep);
    } else {
        RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);
        WaitAndDoRMCallCtx *wctx = RedisModule_Alloc(sizeof(*wctx));
        *wctx = (WaitAndDoRMCallCtx){
                .bc = bc,
                .argv = RedisModule_Alloc((argc - 2) * sizeof(RedisModuleString*)),
                .argc = argc - 2,
        };

        for (int i = 0 ; i < argc - 2 ; ++i) {
            wctx->argv[i] = RedisModule_HoldString(NULL, argv[i + 2]);
        }
        RedisModule_CallReplyPromiseSetUnblockHandler(rep, blpop_and_set_multiple_keys_on_unblocked, wctx);
        RedisModule_FreeCallReply(rep);
    }

    return REDISMODULE_OK;
}

/* simulate a blocked client replying to a thread safe context without creating a thread */
int do_fake_bg_true(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);
    RedisModuleCtx *bctx = RedisModule_GetThreadSafeContext(bc);

    RedisModule_ReplyWithBool(bctx, 1);

    RedisModule_FreeThreadSafeContext(bctx);
    RedisModule_UnblockClient(bc, NULL);

    return REDISMODULE_OK;
}


/* this flag is used to work with busy commands, that might take a while
 * and ability to stop the busy work with a different command*/
static volatile int abort_flag = 0;

int slow_fg_command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }
    long long block_time = 0;
    if (RedisModule_StringToLongLong(argv[1], &block_time) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "Invalid integer value");
        return REDISMODULE_OK;
    }

    uint64_t start_time = RedisModule_MonotonicMicroseconds();
    /* when not blocking indefinitely, we don't process client commands in this test. */
    int yield_flags = block_time? REDISMODULE_YIELD_FLAG_NONE: REDISMODULE_YIELD_FLAG_CLIENTS;
    while (!abort_flag) {
        RedisModule_Yield(ctx, yield_flags, "Slow module operation");
        usleep(1000);
        if (block_time && RedisModule_MonotonicMicroseconds() - start_time > (uint64_t)block_time)
            break;
    }

    abort_flag = 0;
    RedisModule_ReplyWithLongLong(ctx, 1);
    return REDISMODULE_OK;
}

int stop_slow_fg_command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    abort_flag = 1;
    RedisModule_ReplyWithLongLong(ctx, 1);
    return REDISMODULE_OK;
}

/* used to enable or disable slow operation in do_bg_rm_call */
static int set_slow_bg_operation(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }
    long long ll;
    if (RedisModule_StringToLongLong(argv[1], &ll) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "Invalid integer value");
        return REDISMODULE_OK;
    }
    g_slow_bg_operation = ll;
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* used to test if we reached the slow operation in do_bg_rm_call */
static int is_in_slow_bg_operation(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    if (argc != 1) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RedisModule_ReplyWithLongLong(ctx, g_is_in_slow_bg_operation);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "blockedclient", 1, REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "acquire_gil", acquire_gil, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "do_rm_call", do_rm_call,
                                  "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "do_rm_call_async", do_rm_call_async,
                                  "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "do_rm_call_async_script_mode", do_rm_call_async,
                                  "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "do_rm_call_async_no_replicate", do_rm_call_async,
                                  "write", 0, 0, 0) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "do_rm_call_fire_and_forget", do_rm_call_async_fire_and_forget,
                                  "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "wait_and_do_rm_call", wait_and_do_rm_call_async,
                                  "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "blpop_and_set_multiple_keys", blpop_and_set_multiple_keys,
                                      "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "do_bg_rm_call", do_bg_rm_call, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "do_bg_rm_call_format", do_bg_rm_call, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "do_fake_bg_true", do_fake_bg_true, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "slow_fg_command", slow_fg_command,"", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "stop_slow_fg_command", stop_slow_fg_command,"allow-busy", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "set_slow_bg_operation", set_slow_bg_operation, "allow-busy", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "is_in_slow_bg_operation", is_in_slow_bg_operation, "allow-busy", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

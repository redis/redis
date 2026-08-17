#define _XOPEN_SOURCE 700
#include <time.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include "redismodule.h"

#define UNUSED(x) (void)(x)

/* wrapper for RM_Call */
int aofd_rm_call(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
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

/* wrapper for RM_Call with flags */
int aofd_rm_call_flags(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    if(argc < 3){
        return RedisModule_WrongArity(ctx);
    }

    /* Append Ev to the provided flags. */
    RedisModuleString *flags = RedisModule_CreateStringFromString(ctx, argv[1]);
    RedisModule_StringAppendBuffer(ctx, flags, "Ev", 2);

    const char* flg = RedisModule_StringPtrLen(flags, NULL);
    const char* cmd = RedisModule_StringPtrLen(argv[2], NULL);

    RedisModuleCallReply* rep = RedisModule_Call(ctx, cmd, flg, argv + 3, argc - 3);
    if(!rep){
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    }else{
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }
    RedisModule_FreeString(ctx, flags);

    return REDISMODULE_OK;
}

int aofd_rm_replicateVerbatim(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    REDISMODULE_NOT_USED(argv);
    if(argc != 1){
        return RedisModule_WrongArity(ctx);
    }
    RedisModule_ReplicateVerbatim(ctx);
    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;
}

/* wrapper for RM_Replicate */
int aofd_rm_replicate(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    if(argc < 2){
        return RedisModule_WrongArity(ctx);
    }

    const char* cmd = RedisModule_StringPtrLen(argv[1], NULL);
    int response = RedisModule_Replicate(ctx, cmd, "Ev", argv + 2, argc - 2);

    if(response != REDISMODULE_OK){
        RedisModule_ReplyWithError(ctx, "RM_Replicate returned error");
    }else{
        RedisModule_ReplyWithSimpleString(ctx,"OK");
    }

    return REDISMODULE_OK;
}

/* wrapper for RM_Call and RM_Replicate with flags */
int aofd_rm_replicate_flags(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if(argc < 3){
        return RedisModule_WrongArity(ctx);
    }

    /* Append Ev to the provided flags. */
    RedisModuleString *flags = RedisModule_CreateStringFromString(ctx, argv[1]);
    RedisModule_StringAppendBuffer(ctx, flags, "Ev", 2);

    const char* flg = RedisModule_StringPtrLen(flags, NULL);
    const char* cmd = RedisModule_StringPtrLen(argv[2], NULL);

    int response = RedisModule_Replicate(ctx, cmd, flg, argv + 3, argc - 3);
    if(response != REDISMODULE_OK){
        RedisModule_ReplyWithError(ctx, "RM_Replicate returned error");
    }else{
        RedisModule_ReplyWithSimpleString(ctx,"OK");
    }

    RedisModule_FreeString(ctx, flags);
    return REDISMODULE_OK;
}

/* wait 'delay' time before return. If it is originally being called by blocked
 * client, then take care to call BlockedClientMeasureTimeStart\End API as well */
int aofd_sleep(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    long long delayusec;

    if (argc < 2) return RedisModule_WrongArity(ctx);
    if (RedisModule_StringToLongLong(argv[1],&delayusec) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx,"ERR invalid count");
    }

    struct timespec ts = { delayusec / 1000000, (delayusec % 1000000) * 1000};
    RedisModuleBlockedClient *bc = RedisModule_GetBlockedClientHandle(ctx);
    if (bc) RedisModule_BlockedClientMeasureTimeStart(bc);
    nanosleep(&ts, NULL);
    if (bc) RedisModule_BlockedClientMeasureTimeEnd(bc);

    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;
}

/* Gets set of steps to execute. Apply one-by-one and return array of responses.
 * Input is a set of tuples of type <argc, cmd-step, args*>, such that 'argc'
 * is equals to 1+(number of args)
 *
 * Example to argv for two commands, rm_call that "set x 1" and "sleep_usec 100":
 *   >   [4, rm_call, set, x, 1, 2, sleep_usec, 100 ]
 * */
int aofd_rm_run_steps(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    int count_cmds = 0, i = 1;
    size_t len;

    RedisModule_ReplyWithArray(ctx,REDISMODULE_POSTPONED_LEN);

    /* steps constructed from tuples: < argc, cmd-step, *args... > */
    while (i+2 <= argc) {
        long long step_argc;

        ++count_cmds;

        /* read argc */
        if (RedisModule_StringToLongLong(argv[i++], &step_argc) != REDISMODULE_OK) {
            return RedisModule_ReplyWithError(ctx, "aofd_rm_run_steps(): ERR invalid value");
        }

        /* read step_argc */
        const char *cmd_step = RedisModule_StringPtrLen(argv[i], &len);

        if (strcmp(cmd_step, "rm_call") == 0) {
            aofd_rm_call(ctx, argv + i, step_argc);
        } else if (strcmp(cmd_step, "rm_call_flags") == 0) {
            aofd_rm_call_flags(ctx, argv + i, step_argc);
        } else if (strcmp(cmd_step, "sleep_usec") == 0) {
            aofd_sleep(ctx, argv + i, step_argc);
        } else if (strcmp(cmd_step, "rm_replicate") == 0) {
            aofd_rm_replicate(ctx, argv + i, step_argc);
        } else if (strcmp(cmd_step, "rm_replicate_flags") == 0) {
            aofd_rm_replicate_flags(ctx, argv + i, step_argc);
        } else if (strcmp(cmd_step, "rm_replicateVerbatim") == 0) {
            aofd_rm_replicateVerbatim(ctx, argv + i, step_argc);
        } else {
            RedisModule_ReplyWithError(ctx, "aofd_rm_exec_steps(): ERR unsupported command");
            break;
        }

        i += step_argc;
    }
    RedisModule_ReplySetArrayLength(ctx,count_cmds);

    return REDISMODULE_OK;
}

typedef struct {
    RedisModuleString **argv;
    int argc;
    RedisModuleBlockedClient *bc;
} run_steps_bg_data;

void *run_steps_bg_worker(void *arg) {
    run_steps_bg_data *bg = arg;

    // Get Redis module context
    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(bg->bc);

    RedisModule_ThreadSafeContextLock(ctx);

    aofd_rm_run_steps(ctx, bg->argv, bg->argc );

    RedisModule_ThreadSafeContextUnlock(ctx);

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

int aofd_rm_run_steps_bg(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

    /* Make a copy of the arguments and pass them to the thread. */
    run_steps_bg_data *bg = RedisModule_Alloc(sizeof(run_steps_bg_data));
    bg->argv = RedisModule_Alloc(sizeof(RedisModuleString*)*argc);
    bg->argc = argc;
    for (int i=0; i<argc; i++)
        bg->argv[i] = RedisModule_HoldString(ctx, argv[i]);

    /* Block the client */
    bg->bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);

    /* Start a thread to handle the request */
    pthread_t tid;
    int res = pthread_create(&tid, NULL, run_steps_bg_worker, bg);
    assert(res == 0);

    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx,"aofduration",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "aofd.rm_call", aofd_rm_call,"allow-stale", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "aofd.rm_call_flags", aofd_rm_call_flags,"allow-stale", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "aofd.rm_replicate", aofd_rm_replicate,"allow-stale", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "aofd.rm_replicate_flags", aofd_rm_replicate_flags,"allow-stale", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "aofd.rm_replicateVerbatim", aofd_rm_replicateVerbatim,"allow-stale", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "aofd.rm_run_steps", aofd_rm_run_steps,"allow-stale", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "aofd.rm_run_steps_bg", aofd_rm_run_steps_bg,"allow-stale", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "aofd.sleep_usec", aofd_sleep,"allow-stale", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

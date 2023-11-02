#include "redismodule.h"

#include <string.h>

static RedisModuleString *log_key_name;

static const char log_command_name[] = "commandfilter.log";
static const char ping_command_name[] = "commandfilter.ping";
static const char retained_command_name[] = "commandfilter.retained";
static const char unregister_command_name[] = "commandfilter.unregister";
static const char unfiltered_clientid_name[] = "unfilter_clientid";
static int in_log_command = 0;

unsigned long long unfiltered_clientid = 0;

static RedisModuleCommandFilter *filter, *filter1;
static RedisModuleString *retained;

int CommandFilter_UnregisterCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    (void) argc;
    (void) argv;

    RedisModule_ReplyWithLongLong(ctx,
            RedisModule_UnregisterCommandFilter(ctx, filter));

    return REDISMODULE_OK;
}

int CommandFilter_PingCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    (void) argc;
    (void) argv;

    RedisModuleCallReply *reply = RedisModule_Call(ctx, "ping", "c", "@log");
    if (reply) {
        RedisModule_ReplyWithCallReply(ctx, reply);
        RedisModule_FreeCallReply(reply);
    } else {
        RedisModule_ReplyWithSimpleString(ctx, "Unknown command or invalid arguments");
    }

    return REDISMODULE_OK;
}

int CommandFilter_Retained(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    (void) argc;
    (void) argv;

    if (retained) {
        RedisModule_ReplyWithString(ctx, retained);
    } else {
        RedisModule_ReplyWithNull(ctx);
    }

    return REDISMODULE_OK;
}

int CommandFilter_LogCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisModuleString *s = RedisModule_CreateString(ctx, "", 0);

    int i;
    for (i = 1; i < argc; i++) {
        size_t arglen;
        const char *arg = RedisModule_StringPtrLen(argv[i], &arglen);

        if (i > 1) RedisModule_StringAppendBuffer(ctx, s, " ", 1);
        RedisModule_StringAppendBuffer(ctx, s, arg, arglen);
    }

    RedisModuleKey *log = RedisModule_OpenKey(ctx, log_key_name, REDISMODULE_WRITE|REDISMODULE_READ);
    RedisModule_ListPush(log, REDISMODULE_LIST_HEAD, s);
    RedisModule_CloseKey(log);
    RedisModule_FreeString(ctx, s);

    in_log_command = 1;

    size_t cmdlen;
    const char *cmdname = RedisModule_StringPtrLen(argv[1], &cmdlen);
    RedisModuleCallReply *reply = RedisModule_Call(ctx, cmdname, "v", &argv[2], argc - 2);
    if (reply) {
        RedisModule_ReplyWithCallReply(ctx, reply);
        RedisModule_FreeCallReply(reply);
    } else {
        RedisModule_ReplyWithSimpleString(ctx, "Unknown command or invalid arguments");
    }

    in_log_command = 0;

    return REDISMODULE_OK;
}

int CommandFilter_UnfilteredClientdId(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc < 2)
        return RedisModule_WrongArity(ctx);

    long long id;
    if (RedisModule_StringToLongLong(argv[1], &id) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "invalid client id");
        return REDISMODULE_OK;
    }
    if (id < 0) {
        RedisModule_ReplyWithError(ctx, "invalid client id");
        return REDISMODULE_OK;
    }

    unfiltered_clientid = id;
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* Filter to protect against Bug #11894 reappearing
 *
 * ensures that the filter is only run the first time through, and not on reprocessing
 */
void CommandFilter_BlmoveSwap(RedisModuleCommandFilterCtx *filter)
{
    if (RedisModule_CommandFilterArgsCount(filter) != 6)
        return;

    RedisModuleString *arg = RedisModule_CommandFilterArgGet(filter, 0);
    size_t arg_len;
    const char *arg_str = RedisModule_StringPtrLen(arg, &arg_len);

    if (arg_len != 6 || strncmp(arg_str, "blmove", 6))
        return;

    /*
     * Swapping directional args (right/left) from source and destination.
     * need to hold here, can't push into the ArgReplace func, as it will cause other to freed -> use after free
     */
    RedisModuleString *dir1 = RedisModule_HoldString(NULL, RedisModule_CommandFilterArgGet(filter, 3));
    RedisModuleString *dir2 = RedisModule_HoldString(NULL, RedisModule_CommandFilterArgGet(filter, 4));
    RedisModule_CommandFilterArgReplace(filter, 3, dir2);
    RedisModule_CommandFilterArgReplace(filter, 4, dir1);
}

void CommandFilter_CommandFilter(RedisModuleCommandFilterCtx *filter)
{
    unsigned long long id = RedisModule_CommandFilterGetClientId(filter);
    if (id == unfiltered_clientid) return;

    if (in_log_command) return;  /* don't process our own RM_Call() from CommandFilter_LogCommand() */

    /* Fun manipulations:
     * - Remove @delme
     * - Replace @replaceme
     * - Append @insertbefore or @insertafter
     * - Prefix with Log command if @log encountered
     */
    int log = 0;
    int pos = 0;
    while (pos < RedisModule_CommandFilterArgsCount(filter)) {
        const RedisModuleString *arg = RedisModule_CommandFilterArgGet(filter, pos);
        size_t arg_len;
        const char *arg_str = RedisModule_StringPtrLen(arg, &arg_len);

        if (arg_len == 6 && !memcmp(arg_str, "@delme", 6)) {
            RedisModule_CommandFilterArgDelete(filter, pos);
            continue;
        } 
        if (arg_len == 10 && !memcmp(arg_str, "@replaceme", 10)) {
            RedisModule_CommandFilterArgReplace(filter, pos,
                    RedisModule_CreateString(NULL, "--replaced--", 12));
        } else if (arg_len == 13 && !memcmp(arg_str, "@insertbefore", 13)) {
            RedisModule_CommandFilterArgInsert(filter, pos,
                    RedisModule_CreateString(NULL, "--inserted-before--", 19));
            pos++;
        } else if (arg_len == 12 && !memcmp(arg_str, "@insertafter", 12)) {
            RedisModule_CommandFilterArgInsert(filter, pos + 1,
                    RedisModule_CreateString(NULL, "--inserted-after--", 18));
            pos++;
        } else if (arg_len == 7 && !memcmp(arg_str, "@retain", 7)) {
            if (retained) RedisModule_FreeString(NULL, retained);
            retained = RedisModule_CommandFilterArgGet(filter, pos + 1);
            RedisModule_RetainString(NULL, retained);
            pos++;
        } else if (arg_len == 4 && !memcmp(arg_str, "@log", 4)) {
            log = 1;
        }
        pos++;
    }

    if (log) RedisModule_CommandFilterArgInsert(filter, 0,
            RedisModule_CreateString(NULL, log_command_name, sizeof(log_command_name)-1));
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx,"commandfilter",1,REDISMODULE_APIVER_1)
            == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (argc != 2) {
        RedisModule_Log(ctx, "warning", "Log key name not specified");
        return REDISMODULE_ERR;
    }

    long long noself = 0;
    log_key_name = RedisModule_CreateStringFromString(ctx, argv[0]);
    RedisModule_StringToLongLong(argv[1], &noself);
    retained = NULL;

    if (RedisModule_CreateCommand(ctx,log_command_name,
                CommandFilter_LogCommand,"write deny-oom",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,ping_command_name,
                CommandFilter_PingCommand,"deny-oom",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,retained_command_name,
                CommandFilter_Retained,"readonly",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,unregister_command_name,
                CommandFilter_UnregisterCommand,"write deny-oom",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, unfiltered_clientid_name,
                CommandFilter_UnfilteredClientdId, "admin", 1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if ((filter = RedisModule_RegisterCommandFilter(ctx, CommandFilter_CommandFilter, 
                    noself ? REDISMODULE_CMDFILTER_NOSELF : 0))
            == NULL) return REDISMODULE_ERR;

    if ((filter1 = RedisModule_RegisterCommandFilter(ctx, CommandFilter_BlmoveSwap, 0)) == NULL)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    RedisModule_FreeString(ctx, log_key_name);
    if (retained) RedisModule_FreeString(NULL, retained);

    return REDISMODULE_OK;
}

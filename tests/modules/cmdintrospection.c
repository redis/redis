#include "redismodule.h"

#define UNUSED(V) ((void) V)

int cmd_xadd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    int spec_id;

    if (RedisModule_Init(ctx, "cmdintrospection", 1, REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"cmdintrospection.xadd",cmd_xadd,"write deny-oom random fast",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    RedisModuleCommandProxy *xadd = RedisModule_GetCommandProxy(ctx,"cmdintrospection.xadd");

    if (RedisModule_AddCommandKeySpec(xadd,"write",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(xadd,spec_id,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(xadd,spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* NOTE: All versions specified should be the module's versions, not Redis`!
     * We use Redis versions in this example for the purpose of testing (comparing
     * the output with the output of the vanilla XADD */

    RedisModule_SetCommandArity(xadd, -5);
    RedisModule_SetCommandSummary(xadd, "Appends a new entry to a stream");
    RedisModule_SetCommandDebutVersion(xadd, "5.0.0");
    RedisModule_SetCommandComplexity(xadd, "O(1) when adding a new entry, O(N) when trimming where N being the number of entries evicted.");
    RedisModule_AppendCommandHistoryEntry(xadd, "6.2", "Added the `NOMKSTREAM` option, `MINID` trimming strategy and the `LIMIT` option.");
    RedisModule_SetCommandHints(xadd, "hint1 hint2 hint3");

    // Trimming args
    RedisModuleCommandArg *trim_maxlen = RedisModule_CreateCommandArg("maxlen", REDISMODULE_ARG_TYPE_PURE_TOKEN, -1, "MAXLEN", NULL, NULL, REDISMODULE_CMD_ARG_NONE, NULL);
    RedisModuleCommandArg *trim_minid = RedisModule_CreateCommandArg("minid", REDISMODULE_ARG_TYPE_PURE_TOKEN, -1, "MINID", NULL, "6.2", REDISMODULE_CMD_ARG_NONE, NULL);
    RedisModuleCommandArg *trim_startegy = RedisModule_CreateCommandArg("strategy", REDISMODULE_ARG_TYPE_ONEOF, -1, NULL, NULL, NULL, REDISMODULE_CMD_ARG_NONE, NULL);
    RedisModule_AppendSubarg(trim_startegy, trim_maxlen);
    RedisModule_AppendSubarg(trim_startegy, trim_minid);

    RedisModuleCommandArg *trim_exact = RedisModule_CreateCommandArg("equal", REDISMODULE_ARG_TYPE_PURE_TOKEN, -1, "=", NULL, NULL, REDISMODULE_CMD_ARG_NONE, NULL);
    RedisModuleCommandArg *trim_approx = RedisModule_CreateCommandArg("approximately", REDISMODULE_ARG_TYPE_PURE_TOKEN, -1, "~", NULL, NULL, REDISMODULE_CMD_ARG_NONE, NULL);
    RedisModuleCommandArg *trim_op = RedisModule_CreateCommandArg("operator", REDISMODULE_ARG_TYPE_ONEOF, -1, NULL, NULL, NULL, REDISMODULE_CMD_ARG_OPTIONAL, NULL);
    RedisModule_AppendSubarg(trim_op, trim_exact);
    RedisModule_AppendSubarg(trim_op, trim_approx);

    RedisModuleCommandArg *trim_threshold = RedisModule_CreateCommandArg("threshold", REDISMODULE_ARG_TYPE_STRING, -1, NULL, NULL, NULL, REDISMODULE_CMD_ARG_NONE, "threshold");

    RedisModuleCommandArg *trim_count = RedisModule_CreateCommandArg("count", REDISMODULE_ARG_TYPE_INTEGER, -1, "LIMIT", NULL, "6.2", REDISMODULE_CMD_ARG_OPTIONAL, "count");

    RedisModuleCommandArg *trimming = RedisModule_CreateCommandArg("trim", REDISMODULE_ARG_TYPE_BLOCK, -1, NULL, NULL, NULL, REDISMODULE_CMD_ARG_OPTIONAL, NULL);
    RedisModule_AppendSubarg(trimming, trim_startegy);
    RedisModule_AppendSubarg(trimming, trim_op);
    RedisModule_AppendSubarg(trimming, trim_threshold);
    RedisModule_AppendSubarg(trimming, trim_count);

    // ID arg
    RedisModuleCommandArg *id_auto = RedisModule_CreateCommandArg("auto_id", REDISMODULE_ARG_TYPE_PURE_TOKEN, -1, "*", NULL, NULL, REDISMODULE_CMD_ARG_NONE, NULL);
    RedisModuleCommandArg *id_given = RedisModule_CreateCommandArg("id", REDISMODULE_ARG_TYPE_STRING, -1, NULL, NULL, NULL, REDISMODULE_CMD_ARG_NONE, "id");
    RedisModuleCommandArg *id = RedisModule_CreateCommandArg("id_or_auto", REDISMODULE_ARG_TYPE_ONEOF, -1, NULL, NULL, NULL, REDISMODULE_CMD_ARG_NONE, NULL);
    RedisModule_AppendSubarg(id, id_auto);
    RedisModule_AppendSubarg(id, id_given);

    // Fields and values
    RedisModuleCommandArg *field = RedisModule_CreateCommandArg("field", REDISMODULE_ARG_TYPE_STRING, -1, NULL, NULL, NULL, REDISMODULE_CMD_ARG_NONE, "field");
    RedisModuleCommandArg *value = RedisModule_CreateCommandArg("value", REDISMODULE_ARG_TYPE_STRING, -1, NULL, NULL, NULL, REDISMODULE_CMD_ARG_NONE, "value");
    RedisModuleCommandArg *fieldsvalues = RedisModule_CreateCommandArg("field_value", REDISMODULE_ARG_TYPE_BLOCK, -1, NULL, NULL, NULL, REDISMODULE_CMD_ARG_MULTIPLE, NULL);
    RedisModule_AppendSubarg(fieldsvalues, field);
    RedisModule_AppendSubarg(fieldsvalues, value);

    // Key
    RedisModuleCommandArg *key = RedisModule_CreateCommandArg("key", REDISMODULE_ARG_TYPE_KEY, 0, NULL, NULL, NULL, REDISMODULE_CMD_ARG_NONE, "key");

    // NOMKSTREAM
    RedisModuleCommandArg *nomkstream = RedisModule_CreateCommandArg("nomkstream", REDISMODULE_ARG_TYPE_PURE_TOKEN, -1, "NOMKSTREAM", NULL, "6.2", REDISMODULE_CMD_ARG_OPTIONAL, NULL);

    // Append all args
    RedisModule_AppendArgToCommand(xadd, key);
    RedisModule_AppendArgToCommand(xadd, nomkstream);
    RedisModule_AppendArgToCommand(xadd, trimming);
    RedisModule_AppendArgToCommand(xadd, id);
    RedisModule_AppendArgToCommand(xadd, fieldsvalues);

    return REDISMODULE_OK;
}

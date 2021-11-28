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

    if (RedisModule_CreateCommand(ctx,"cmdintrospection.xadd",NULL,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    RedisModuleCommandProxy *xadd = RedisModule_GetCommandProxy(ctx,"cmdintrospection.xadd");

    if (RedisModule_AddCommandKeySpec(xadd,"write",&spec_id) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecBeginSearchIndex(xadd,spec_id,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_SetCommandKeySpecFindKeysRange(xadd,spec_id,0,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModule_SetCommandArity(xadd, -5);
    RedisModule_SetCommandSummary(xadd, "Appends a new entry to a stream");
    RedisModule_SetCommandDebutVersion(xadd, "5.0.0");
    RedisModule_SetCommandComplexity(xadd, "O(1) when adding a new entry, O(N) when trimming where N being the number of entries evicted.");
    RedisModule_AppendCommandHistoryEntry(xadd, "6.2", "Added the NOMKSTREAM option, MINID trimming strategy and the LIMIT option");
    RedisModule_SetCommandHints(xadd, "hint1 hint2 hint3");

    /* Trimming args */
    RedisModuleCommandArg *trim_maxlen = RedisModule_CreateCommandArg("maxlen", REDISMODULE_ARG_TYPE_PURE_TOKEN, "MAXLEN", NULL, NULL, REDISMODULE_CMD_ARG_NONE, NULL);
    RedisModuleCommandArg *trim_minid = RedisModule_CreateCommandArg("minid", REDISMODULE_ARG_TYPE_PURE_TOKEN, "MINID", NULL, "6.2.0", REDISMODULE_CMD_ARG_NONE, NULL);
    RedisModuleCommandArg *trim_startegy = RedisModule_CreateCommandArg("trim_startegy", REDISMODULE_ARG_TYPE_ONEOF, NULL, NULL, NULL, REDISMODULE_CMD_ARG_NONE, NULL);
    RedisModule_AppendSubarg(trim_startegy, trim_maxlen);
    RedisModule_AppendSubarg(trim_startegy, trim_minid);

    RedisModuleCommandArg *trim_exact = RedisModule_CreateCommandArg("exact", REDISMODULE_ARG_TYPE_PURE_TOKEN, "=", NULL, NULL, REDISMODULE_CMD_ARG_NONE, NULL);
    RedisModuleCommandArg *trim_approx = RedisModule_CreateCommandArg("approx", REDISMODULE_ARG_TYPE_PURE_TOKEN, "~", NULL, NULL, REDISMODULE_CMD_ARG_NONE, NULL);
    RedisModuleCommandArg *trim_op = RedisModule_CreateCommandArg("trim_op", REDISMODULE_ARG_TYPE_ONEOF, NULL, NULL, NULL, REDISMODULE_CMD_ARG_OPTIONAL, NULL);
    RedisModule_AppendSubarg(trim_op, trim_exact);
    RedisModule_AppendSubarg(trim_op, trim_approx);

    RedisModuleCommandArg *trim_threshold = RedisModule_CreateCommandArg("trim_threshold", REDISMODULE_ARG_TYPE_STRING, NULL, NULL, NULL, REDISMODULE_CMD_ARG_NONE, "threshold");

    RedisModuleCommandArg *trim_count = RedisModule_CreateCommandArg("trim_count", REDISMODULE_ARG_TYPE_INTEGER, "LIMIT", NULL, "6.2.0", REDISMODULE_CMD_ARG_OPTIONAL, "count");

    RedisModuleCommandArg *trimming = RedisModule_CreateCommandArg("trimming", REDISMODULE_ARG_TYPE_BLOCK, NULL, NULL, NULL, REDISMODULE_CMD_ARG_OPTIONAL, NULL);
    RedisModule_AppendSubarg(trimming, trim_startegy);
    RedisModule_AppendSubarg(trimming, trim_op);
    RedisModule_AppendSubarg(trimming, trim_threshold);
    RedisModule_AppendSubarg(trimming, trim_count);

    /* ID arg */
    RedisModuleCommandArg *id_auto = RedisModule_CreateCommandArg("id_auto", REDISMODULE_ARG_TYPE_PURE_TOKEN, "*", NULL, NULL, REDISMODULE_CMD_ARG_NONE, NULL);
    RedisModuleCommandArg *id_given = RedisModule_CreateCommandArg("id_given", REDISMODULE_ARG_TYPE_STRING, NULL, NULL, NULL, REDISMODULE_CMD_ARG_NONE, "id");
    RedisModuleCommandArg *id = RedisModule_CreateCommandArg("id", REDISMODULE_ARG_TYPE_ONEOF, NULL, NULL, NULL, REDISMODULE_CMD_ARG_NONE, NULL);
    RedisModule_AppendSubarg(id, id_auto);
    RedisModule_AppendSubarg(id, id_given);

    /* Fields and values */
    RedisModuleCommandArg *field = RedisModule_CreateCommandArg("field", REDISMODULE_ARG_TYPE_STRING, NULL, NULL, NULL, REDISMODULE_CMD_ARG_NONE, "field");
    RedisModuleCommandArg *value = RedisModule_CreateCommandArg("value", REDISMODULE_ARG_TYPE_STRING, NULL, NULL, NULL, REDISMODULE_CMD_ARG_NONE, "value");
    RedisModuleCommandArg *fieldsvalues = RedisModule_CreateCommandArg("fields_and_values", REDISMODULE_ARG_TYPE_BLOCK, NULL, NULL, NULL, REDISMODULE_CMD_ARG_MULTIPLE, NULL);
    RedisModule_AppendSubarg(fieldsvalues, field);
    RedisModule_AppendSubarg(fieldsvalues, value);

    /* Key */
    RedisModuleCommandArg *key = RedisModule_CreateCommandArg("key", REDISMODULE_ARG_TYPE_KEY, NULL, NULL, NULL, REDISMODULE_CMD_ARG_NONE, "key");

    /* NOMKSTREAM */
    RedisModuleCommandArg *nomkstream = RedisModule_CreateCommandArg("nomkstream", REDISMODULE_ARG_TYPE_PURE_TOKEN, "NOMKSTREAM", NULL, NULL, REDISMODULE_CMD_ARG_OPTIONAL, NULL);

    /* Append all args */
    RedisModule_AppendArgToCommand(xadd, key);
    RedisModule_AppendArgToCommand(xadd, nomkstream);
    RedisModule_AppendArgToCommand(xadd, trimming);
    RedisModule_AppendArgToCommand(xadd, id);
    RedisModule_AppendArgToCommand(xadd, fieldsvalues);

    return REDISMODULE_OK;
}

#if 0
RedisModuleCommandArg *RM_CreateCommandArg(const char* argname, RedisModuleCommandArgType type, const char *token, const char *summary,
const char* since, int flags, const char *value) {

/* XADD trim strategy argument table */
struct redisCommandArg XADD_trim_strategy_Subargs[] = {
{"maxlen",ARG_TYPE_PURE_TOKEN,"MAXLEN",NULL,NULL,CMD_ARG_NONE},
{"minid",ARG_TYPE_PURE_TOKEN,"MINID",NULL,NULL,CMD_ARG_NONE},
{0}
};

/* XADD trim operator argument table */
struct redisCommandArg XADD_trim_operator_Subargs[] = {
{"equal",ARG_TYPE_PURE_TOKEN,"=",NULL,NULL,CMD_ARG_NONE},
{"approximately",ARG_TYPE_PURE_TOKEN,"~",NULL,NULL,CMD_ARG_NONE},
{0}
};

/* XADD trim argument table */
struct redisCommandArg XADD_trim_Subargs[] = {
{"strategy",ARG_TYPE_ONEOF,NULL,NULL,NULL,CMD_ARG_NONE,.value.subargs=XADD_trim_strategy_Subargs},
{"operator",ARG_TYPE_ONEOF,NULL,NULL,NULL,CMD_ARG_OPTIONAL,.value.subargs=XADD_trim_operator_Subargs},
{"threshold",ARG_TYPE_STRING,NULL,NULL,NULL,CMD_ARG_NONE,.value.string="threshold"},
{"count",ARG_TYPE_INTEGER,"LIMIT",NULL,NULL,CMD_ARG_OPTIONAL,.value.string="count"},
{0}
};

/* XADD id_or_auto argument table */
struct redisCommandArg XADD_id_or_auto_Subargs[] = {
{"auto_id",ARG_TYPE_PURE_TOKEN,"*",NULL,NULL,CMD_ARG_NONE},
{"id",ARG_TYPE_STRING,NULL,NULL,NULL,CMD_ARG_NONE,.value.string="ID"},
{0}
};

/* XADD field_value argument table */
struct redisCommandArg XADD_field_value_Subargs[] = {
{"field",ARG_TYPE_STRING,NULL,NULL,NULL,CMD_ARG_NONE,.value.string="field"},
{"value",ARG_TYPE_STRING,NULL,NULL,NULL,CMD_ARG_NONE,.value.string="value"},
{0}
};

/* XADD argument table */
struct redisCommandArg XADD_Args[] = {
{"key",ARG_TYPE_KEY,NULL,NULL,NULL,CMD_ARG_NONE,.value.string="key"},
{"nomkstream",ARG_TYPE_PURE_TOKEN,"NOMKSTREAM",NULL,NULL,CMD_ARG_OPTIONAL},
{"trim",ARG_TYPE_BLOCK,NULL,NULL,NULL,CMD_ARG_OPTIONAL,.value.subargs=XADD_trim_Subargs},
{"id_or_auto",ARG_TYPE_ONEOF,NULL,NULL,NULL,CMD_ARG_NONE,.value.subargs=XADD_id_or_auto_Subargs},
{"field_value",ARG_TYPE_BLOCK,NULL,NULL,NULL,CMD_ARG_MULTIPLE,.value.subargs=XADD_field_value_Subargs},
{0}
};
#endif

#include "redismodule.h"
#include <errno.h>
#include <string.h>

typedef struct {
    const char *label;
    int actual_success;
    int expected_success;
    int actual_errno;
    int expected_errno;
} RegistrationResult;

static RegistrationResult results[32];
static int result_count;

static int ReplyOK(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

static void Record(const char *label, int actual_success, int expected_success,
                   int actual_errno, int expected_errno) {
    RedisModule_Assert(result_count < 32);
    results[result_count++] = (RegistrationResult){
        label, actual_success, expected_success, actual_errno, expected_errno
    };
}

static void CheckCommand(RedisModuleCtx *ctx, const char *label, const char *name,
                         const char *flags, int expected_success, int expected_errno) {
    errno = 0;
    int rc = RedisModule_CreateCommand(ctx, name, ReplyOK, flags, 0, 0, 0);
    int actual_errno = errno;
    Record(label, rc == REDISMODULE_OK, expected_success, actual_errno, expected_errno);
}

static void CheckType(RedisModuleCtx *ctx, const char *label, const char *name,
                      int encver, int methods_version, int expected_success, int expected_errno) {
    RedisModuleTypeMethods methods = {0};
    methods.version = methods_version;
    errno = 0;
    RedisModuleType *type = RedisModule_CreateDataType(ctx, name, encver, &methods);
    int actual_errno = errno;
    Record(label, type != NULL, expected_success, actual_errno, expected_errno);
}

static int ReplyResults(RedisModuleCtx *ctx, int first) {
    RedisModule_ReplyWithArray(ctx, result_count - first);
    for (int i = first; i < result_count; i++) {
        RedisModule_ReplyWithArray(ctx, 5);
        RedisModule_ReplyWithSimpleString(ctx, results[i].label);
        RedisModule_ReplyWithLongLong(ctx, results[i].actual_success);
        RedisModule_ReplyWithLongLong(ctx, results[i].expected_success);
        RedisModule_ReplyWithLongLong(ctx, results[i].actual_errno);
        RedisModule_ReplyWithLongLong(ctx, results[i].expected_errno);
    }
    return REDISMODULE_OK;
}

static int Results(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    return ReplyResults(ctx, 0);
}

static int OutsideOnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    int first = result_count;
    CheckCommand(ctx, "command_outside_onload", "registrationerrno.outside.new", "", 0, EINVAL);
    CheckType(ctx, "type_outside_onload", "errtype05", 0, REDISMODULE_TYPE_METHOD_VERSION, 0, EINVAL);
    int rc = ReplyResults(ctx, first);
    result_count = first;
    return rc;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx, "registrationerrno", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (argc == 1 && strcmp(RedisModule_StringPtrLen(argv[0], NULL), "abort") == 0) {
        RedisModuleTypeMethods methods = {0};
        methods.version = REDISMODULE_TYPE_METHOD_VERSION;
        errno = 0;
        RedisModuleType *type = RedisModule_CreateDataType(ctx, "short", 0, &methods);
        int actual_errno = errno;
        RedisModule_Log(ctx, "warning", "registration errno abort: %s",
                        type == NULL && actual_errno == EINVAL ? "EINVAL" : "unexpected");
        return REDISMODULE_ERR;
    }

    CheckCommand(ctx, "command_positive", "registrationerrno.valid", "", 1, 0);
    CheckCommand(ctx, "command_null_flags", "registrationerrno.nullflags", NULL, 1, 0);
    CheckCommand(ctx, "command_duplicate", "registrationerrno.valid", "", 0, EBUSY);
    CheckCommand(ctx, "command_invalid_flags", "registrationerrno.badflags", "invalid-registration-flag", 0, EINVAL);

    const char *names[] = {"bad name", "bad\rname", "bad\nname", "bad|name", "bad@name", "bad=name", "bad,name"};
    const char *labels[] = {"name_space", "name_cr", "name_lf", "name_pipe", "name_at", "name_equal", "name_comma"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        CheckCommand(ctx, labels[i], names[i], "", 0, EINVAL);

    int cluster = (RedisModule_GetContextFlags(ctx) & REDISMODULE_CTX_FLAGS_CLUSTER) != 0;
    CheckCommand(ctx, "command_no_cluster", "registrationerrno.no_cluster", "no-cluster",
                 !cluster, cluster ? ENOTSUP : 0);

    CheckType(ctx, "type_positive", "errtype01", 0, REDISMODULE_TYPE_METHOD_VERSION, 1, 0);
    CheckType(ctx, "type_short_name", "short", 0, REDISMODULE_TYPE_METHOD_VERSION, 0, EINVAL);
    CheckType(ctx, "type_invalid_charset", "bad!type1", 0, REDISMODULE_TYPE_METHOD_VERSION, 0, EINVAL);
    CheckType(ctx, "type_reserved_id", "AAAAAAAAA", 0, REDISMODULE_TYPE_METHOD_VERSION, 0, EINVAL);
    CheckType(ctx, "type_negative_encoding", "errtype02", -1, REDISMODULE_TYPE_METHOD_VERSION, 0, EINVAL);
    CheckType(ctx, "type_high_encoding", "errtype03", 1024, REDISMODULE_TYPE_METHOD_VERSION, 0, EINVAL);
    /* datatype.so must have finished loading before this module. */
    CheckType(ctx, "type_duplicate", "test___dt", 1, REDISMODULE_TYPE_METHOD_VERSION, 0, EBUSY);
    CheckType(ctx, "type_zero_methods", "errtype04", 0, 0, 0, EINVAL);

    if (RedisModule_CreateCommand(ctx, "registrationerrno.results", Results, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "registrationerrno.outside", OutsideOnLoad, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

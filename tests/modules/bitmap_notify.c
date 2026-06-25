#include "redismodule.h"
#include <string.h>
#include <strings.h>

static RedisModuleString *armed_key = NULL;
static RedisModuleString *armed_event = NULL;
static RedisModuleString *armed_value = NULL;
static int armed_action = 0;
static long long hits = 0;

#define ACTION_DEL 1
#define ACTION_SET 2

static void clearArm(RedisModuleCtx *ctx) {
    if (armed_key) RedisModule_FreeString(ctx, armed_key);
    if (armed_event) RedisModule_FreeString(ctx, armed_event);
    if (armed_value) RedisModule_FreeString(ctx, armed_value);
    armed_key = NULL;
    armed_event = NULL;
    armed_value = NULL;
    armed_action = 0;
}

static int stringEqualsBuffer(RedisModuleString *str, const char *buf, size_t len) {
    size_t strlen;
    const char *strbuf = RedisModule_StringPtrLen(str, &strlen);
    return strlen == len && memcmp(strbuf, buf, len) == 0;
}

static int BitmapNotifyCallback(RedisModuleCtx *ctx, int type, const char *event,
                                RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);

    if (!armed_key || !armed_event) return REDISMODULE_OK;

    size_t event_len = strlen(event);
    if (!stringEqualsBuffer(armed_event, event, event_len)) return REDISMODULE_OK;

    size_t key_len;
    const char *key_buf = RedisModule_StringPtrLen(key, &key_len);
    if (!stringEqualsBuffer(armed_key, key_buf, key_len)) return REDISMODULE_OK;

    int action = armed_action;
    RedisModuleString *value = armed_value;
    if (value) RedisModule_RetainString(ctx, value);
    clearArm(ctx);
    hits++;

    RedisModuleCallReply *reply = NULL;
    if (action == ACTION_DEL) {
        reply = RedisModule_Call(ctx, "DEL", "s!", key);
    } else if (action == ACTION_SET && value) {
        reply = RedisModule_Call(ctx, "SET", "!ss", key, value);
    }

    if (value) RedisModule_FreeString(ctx, value);
    if (reply) RedisModule_FreeCallReply(reply);
    return REDISMODULE_OK;
}

static int ArmCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 4 || argc > 5) return RedisModule_WrongArity(ctx);

    size_t action_len;
    const char *action = RedisModule_StringPtrLen(argv[3], &action_len);
    int new_action;
    if (action_len == 3 && !strncasecmp(action, "del", 3)) {
        if (argc != 4) return RedisModule_WrongArity(ctx);
        new_action = ACTION_DEL;
    } else if (action_len == 3 && !strncasecmp(action, "set", 3)) {
        if (argc != 5) return RedisModule_WrongArity(ctx);
        new_action = ACTION_SET;
    } else {
        return RedisModule_ReplyWithError(ctx, "ERR unknown action");
    }

    clearArm(ctx);
    armed_key = RedisModule_HoldString(ctx, argv[1]);
    armed_event = RedisModule_HoldString(ctx, argv[2]);
    armed_action = new_action;
    if (new_action == ACTION_SET)
        armed_value = RedisModule_HoldString(ctx, argv[4]);

    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

static int ClearCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);
    clearArm(ctx);
    hits = 0;
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

static int HitsCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);
    return RedisModule_ReplyWithLongLong(ctx, hits);
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "bitmapnotify", 1, REDISMODULE_APIVER_1) ==
        REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_SubscribeToKeyspaceEvents(ctx,
            REDISMODULE_NOTIFY_NEW |
            REDISMODULE_NOTIFY_OVERWRITTEN |
            REDISMODULE_NOTIFY_TYPE_CHANGED,
            BitmapNotifyCallback) != REDISMODULE_OK)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "bitmapnotify.arm", ArmCommand,
                                  "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "bitmapnotify.clear", ClearCommand,
                                  "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "bitmapnotify.hits", HitsCommand,
                                  "readonly", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    clearArm(ctx);
    return REDISMODULE_OK;
}

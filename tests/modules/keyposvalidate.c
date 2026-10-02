/* Test module for key/channel position validation.
 *
 * Provides commands that deliberately declare invalid key/channel positions
 * (pos = 0, pos = argc, pos > argc) during getkeys-api / getchannels-api
 * introspection. Used to verify that moduleGetCommandKeysViaAPI and
 * moduleGetCommandChannelsViaAPI filter out invalid positions instead of
 * causing an OOB read in ACL checks.
 *
 * See: tests/unit/moduleapi/keyposvalidate.tcl
 */

#include "redismodule.h"

#define UNUSED(V) ((void) V)

/* ------------------------------------------------------------------ */
/* Key-position commands                                              */
/* ------------------------------------------------------------------ */

/* Declares pos=0 (invalid: must be > 0). */
int keypos_validate_zero(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    if (RedisModule_IsKeysPositionRequest(ctx)) {
        RedisModule_KeyAtPos(ctx, 0);  /* invalid */
        return REDISMODULE_OK;
    }
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* Declares pos=argc (invalid: argv is 0-indexed, max valid is argc-1). */
int keypos_validate_at_argc(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    if (RedisModule_IsKeysPositionRequest(ctx)) {
        RedisModule_KeyAtPos(ctx, argc);  /* invalid: out of bounds */
        return REDISMODULE_OK;
    }
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* Declares pos=999 (invalid: way beyond argc). */
int keypos_validate_above_argc(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    if (RedisModule_IsKeysPositionRequest(ctx)) {
        RedisModule_KeyAtPos(ctx, 999);  /* invalid: OOB */
        return REDISMODULE_OK;
    }
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* Declares a mix of valid and invalid positions: pos=1 (valid),
 * pos=999 (invalid), pos=argc (invalid), pos=0 (invalid), pos=2 (valid). */
int keypos_validate_mixed(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    if (RedisModule_IsKeysPositionRequest(ctx)) {
        RedisModule_KeyAtPos(ctx, 1);            /* valid */
        RedisModule_KeyAtPos(ctx, 999);          /* invalid: way OOB */
        RedisModule_KeyAtPos(ctx, argc);         /* invalid: OOB */
        RedisModule_KeyAtPos(ctx, 0);            /* invalid: must be > 0 */
        if (argc >= 3)
            RedisModule_KeyAtPos(ctx, 2);        /* valid */
        return REDISMODULE_OK;
    }
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* ------------------------------------------------------------------ */
/* Channel-position commands (use ChannelAtPosWithFlags)             */
/* ------------------------------------------------------------------ */

/* Declares pos=0 (invalid: must be > 0). */
int chanpos_validate_zero(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    if (RedisModule_IsChannelsPositionRequest(ctx)) {
        RedisModule_ChannelAtPosWithFlags(ctx, 0, REDISMODULE_CMD_CHANNEL_SUBSCRIBE);
        return REDISMODULE_OK;
    }
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* Declares pos=argc (invalid). */
int chanpos_validate_at_argc(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    if (RedisModule_IsChannelsPositionRequest(ctx)) {
        RedisModule_ChannelAtPosWithFlags(ctx, argc, REDISMODULE_CMD_CHANNEL_SUBSCRIBE);
        return REDISMODULE_OK;
    }
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* Declares pos=999 (invalid: way OOB). */
int chanpos_validate_above_argc(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    if (RedisModule_IsChannelsPositionRequest(ctx)) {
        RedisModule_ChannelAtPosWithFlags(ctx, 999, REDISMODULE_CMD_CHANNEL_SUBSCRIBE);
        return REDISMODULE_OK;
    }
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* Declares valid + invalid positions mixed. */
int chanpos_validate_mixed(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    if (RedisModule_IsChannelsPositionRequest(ctx)) {
        RedisModule_ChannelAtPosWithFlags(ctx, 1, REDISMODULE_CMD_CHANNEL_SUBSCRIBE);
        RedisModule_ChannelAtPosWithFlags(ctx, 999, REDISMODULE_CMD_CHANNEL_SUBSCRIBE);
        RedisModule_ChannelAtPosWithFlags(ctx, argc, REDISMODULE_CMD_CHANNEL_SUBSCRIBE);
        RedisModule_ChannelAtPosWithFlags(ctx, 0, REDISMODULE_CMD_CHANNEL_SUBSCRIBE);
        if (argc >= 3)
            RedisModule_ChannelAtPosWithFlags(ctx, 2, REDISMODULE_CMD_CHANNEL_SUBSCRIBE);
        return REDISMODULE_OK;
    }
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* ------------------------------------------------------------------ */
/* Module onLoad                                                      */
/* ------------------------------------------------------------------ */

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    if (RedisModule_Init(ctx, "keyposvalidate", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* Key-position commands (use "getkeys-api" + "no-mandatory-keys" so that
     * when all positions are filtered out as invalid, COMMAND GETKEYS returns
     * an empty array rather than an error. This is what we want to test:
     * invalid positions are filtered, command still works. */
    if (RedisModule_CreateCommand(ctx, "keypos.validate.zero",
                                  keypos_validate_zero, "getkeys-api no-mandatory-keys",
                                  0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "keypos.validate.at_argc",
                                  keypos_validate_at_argc, "getkeys-api no-mandatory-keys",
                                  0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "keypos.validate.above_argc",
                                  keypos_validate_above_argc, "getkeys-api no-mandatory-keys",
                                  0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "keypos.validate.mixed",
                                  keypos_validate_mixed, "getkeys-api no-mandatory-keys",
                                  0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* Channel-position commands (use "getchannels-api"). */
    if (RedisModule_CreateCommand(ctx, "chanpos.validate.zero",
                                  chanpos_validate_zero, "getchannels-api",
                                  0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "chanpos.validate.at_argc",
                                  chanpos_validate_at_argc, "getchannels-api",
                                  0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "chanpos.validate.above_argc",
                                  chanpos_validate_above_argc, "getchannels-api",
                                  0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "chanpos.validate.mixed",
                                  chanpos_validate_mixed, "getchannels-api",
                                  0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

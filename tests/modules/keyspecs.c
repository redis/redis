#include "redismodule.h"

#define UNUSED(V) ((void) V)

/* This function implements all commands in this module. All we care about is
 * the COMMAND metadata anyway. */
int kspec_impl(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    /* Handle getkeys-api introspection (for "kspec.nonewithgetkeys")  */
    if (RedisModule_IsKeysPositionRequest(ctx)) {
        for (int i = 1; i < argc; i += 2)
            RedisModule_KeyAtPosWithFlags(ctx, i, REDISMODULE_CMD_KEY_RO | REDISMODULE_CMD_KEY_ACCESS);

        return REDISMODULE_OK;
    }

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int createKspecNone(RedisModuleCtx *ctx) {
    /* A command without keyspecs; only the legacy (first,last,step) triple (MSET like spec). */
    if (RedisModule_CreateCommand(ctx,"kspec.none",kspec_impl,"",1,-1,2) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

int createKspecNoneWithGetkeys(RedisModuleCtx *ctx) {
    /* A command without keyspecs; only the legacy (first,last,step) triple (MSET like spec), but also has a getkeys callback */
    if (RedisModule_CreateCommand(ctx,"kspec.nonewithgetkeys",kspec_impl,"getkeys-api",1,-1,2) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

int createKspecTwoRanges(RedisModuleCtx *ctx) {
    /* Test that two position/range-based key specs are combined to produce the
     * legacy (first,last,step) values representing both keys. */
    if (RedisModule_CreateCommand(ctx,"kspec.tworanges",kspec_impl,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModuleCommand *command = RedisModule_GetCommand(ctx,"kspec.tworanges");
    RedisModuleCommandInfo info = {
        .version = REDISMODULE_COMMAND_INFO_VERSION,
        .arity = -2,
        .key_specs = (RedisModuleCommandKeySpec[]){
            {
                .flags = REDISMODULE_CMD_KEY_RO | REDISMODULE_CMD_KEY_ACCESS,
                .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 1,
                .find_keys_type = REDISMODULE_KSPEC_FK_RANGE,
                .fk.range = {0,1,0}
            },
            {
                .flags = REDISMODULE_CMD_KEY_RW | REDISMODULE_CMD_KEY_UPDATE,
                .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 2,
                /* Omitted find_keys_type is shorthand for RANGE {0,1,0} */
            },
            {0}
        }
    };
    if (RedisModule_SetCommandInfo(command, &info) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

int createKspecTwoRangesWithGap(RedisModuleCtx *ctx) {
    /* Test that two position/range-based key specs are combined to produce the
     * legacy (first,last,step) values representing just one key. */
    if (RedisModule_CreateCommand(ctx,"kspec.tworangeswithgap",kspec_impl,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModuleCommand *command = RedisModule_GetCommand(ctx,"kspec.tworangeswithgap");
    RedisModuleCommandInfo info = {
        .version = REDISMODULE_COMMAND_INFO_VERSION,
        .arity = -2,
        .key_specs = (RedisModuleCommandKeySpec[]){
            {
                .flags = REDISMODULE_CMD_KEY_RO | REDISMODULE_CMD_KEY_ACCESS,
                .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 1,
                .find_keys_type = REDISMODULE_KSPEC_FK_RANGE,
                .fk.range = {0,1,0}
            },
            {
                .flags = REDISMODULE_CMD_KEY_RW | REDISMODULE_CMD_KEY_UPDATE,
                .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 3,
                /* Omitted find_keys_type is shorthand for RANGE {0,1,0} */
            },
            {0}
        }
    };
    if (RedisModule_SetCommandInfo(command, &info) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

int createKspecKeyword(RedisModuleCtx *ctx) {
    /* Only keyword-based specs. The legacy triple is wiped and set to (0,0,0). */
    if (RedisModule_CreateCommand(ctx,"kspec.keyword",kspec_impl,"",3,-1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModuleCommand *command = RedisModule_GetCommand(ctx,"kspec.keyword");
    RedisModuleCommandInfo info = {
        .version = REDISMODULE_COMMAND_INFO_VERSION,
        .key_specs = (RedisModuleCommandKeySpec[]){
            {
                .flags = REDISMODULE_CMD_KEY_RO | REDISMODULE_CMD_KEY_ACCESS,
                .begin_search_type = REDISMODULE_KSPEC_BS_KEYWORD,
                .bs.keyword.keyword = "KEYS",
                .bs.keyword.startfrom = 1,
                .find_keys_type = REDISMODULE_KSPEC_FK_RANGE,
                .fk.range = {-1,1,0}
            },
            {0}
        }
    };
    if (RedisModule_SetCommandInfo(command, &info) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

int createKspecComplex1(RedisModuleCtx *ctx) {
    /* First is a range a single key. The rest are keyword-based specs. */
    if (RedisModule_CreateCommand(ctx,"kspec.complex1",kspec_impl,"",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModuleCommand *command = RedisModule_GetCommand(ctx,"kspec.complex1");
    RedisModuleCommandInfo info = {
        .version = REDISMODULE_COMMAND_INFO_VERSION,
        .key_specs = (RedisModuleCommandKeySpec[]){
            {
                .flags = REDISMODULE_CMD_KEY_RO,
                .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 1,
            },
            {
                .flags = REDISMODULE_CMD_KEY_RW | REDISMODULE_CMD_KEY_UPDATE,
                .begin_search_type = REDISMODULE_KSPEC_BS_KEYWORD,
                .bs.keyword.keyword = "STORE",
                .bs.keyword.startfrom = 2,
            },
            {
                .flags = REDISMODULE_CMD_KEY_RO | REDISMODULE_CMD_KEY_ACCESS,
                .begin_search_type = REDISMODULE_KSPEC_BS_KEYWORD,
                .bs.keyword.keyword = "KEYS",
                .bs.keyword.startfrom = 2,
                .find_keys_type = REDISMODULE_KSPEC_FK_KEYNUM,
                .fk.keynum = {0,1,1}
            },
            {0}
        }
    };
    if (RedisModule_SetCommandInfo(command, &info) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

int createKspecComplex2(RedisModuleCtx *ctx) {
    /* First is not legacy, more than STATIC_KEYS_SPECS_NUM specs */
    if (RedisModule_CreateCommand(ctx,"kspec.complex2",kspec_impl,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModuleCommand *command = RedisModule_GetCommand(ctx,"kspec.complex2");
    RedisModuleCommandInfo info = {
        .version = REDISMODULE_COMMAND_INFO_VERSION,
        .key_specs = (RedisModuleCommandKeySpec[]){
            {
                .flags = REDISMODULE_CMD_KEY_RW | REDISMODULE_CMD_KEY_UPDATE,
                .begin_search_type = REDISMODULE_KSPEC_BS_KEYWORD,
                .bs.keyword.keyword = "STORE",
                .bs.keyword.startfrom = 5,
                .find_keys_type = REDISMODULE_KSPEC_FK_RANGE,
                .fk.range = {0,1,0}
            },
            {
                .flags = REDISMODULE_CMD_KEY_RO | REDISMODULE_CMD_KEY_ACCESS,
                .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 1,
                .find_keys_type = REDISMODULE_KSPEC_FK_RANGE,
                .fk.range = {0,1,0}
            },
            {
                .flags = REDISMODULE_CMD_KEY_RO | REDISMODULE_CMD_KEY_ACCESS,
                .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 2,
                .find_keys_type = REDISMODULE_KSPEC_FK_RANGE,
                .fk.range = {0,1,0}
            },
            {
                .flags = REDISMODULE_CMD_KEY_RW | REDISMODULE_CMD_KEY_UPDATE,
                .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 3,
                .find_keys_type = REDISMODULE_KSPEC_FK_KEYNUM,
                .fk.keynum = {0,1,1}
            },
            {
                .flags = REDISMODULE_CMD_KEY_RW | REDISMODULE_CMD_KEY_UPDATE,
                .begin_search_type = REDISMODULE_KSPEC_BS_KEYWORD,
                .bs.keyword.keyword = "MOREKEYS",
                .bs.keyword.startfrom = 5,
                .find_keys_type = REDISMODULE_KSPEC_FK_RANGE,
                .fk.range = {-1,1,0}
            },
            {0}
        }
    };
    if (RedisModule_SetCommandInfo(command, &info) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "keyspecs", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (createKspecNone(ctx) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (createKspecNoneWithGetkeys(ctx) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (createKspecTwoRanges(ctx) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (createKspecTwoRangesWithGap(ctx) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (createKspecKeyword(ctx) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (createKspecComplex1(ctx) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (createKspecComplex2(ctx) == REDISMODULE_ERR) return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

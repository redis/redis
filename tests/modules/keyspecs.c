#include "redismodule.h"
#include <errno.h>

#define UNUSED(V) ((void) V)

int kspec_legacy(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int kspec_complex1(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int kspec_complex2(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int createKspecLegacy(RedisModuleCtx *ctx) {
    /* Test legacy range "gluing" */
    if (RedisModule_CreateCommand(ctx,"kspec.legacy",kspec_legacy,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModuleCommand *command = RedisModule_GetCommand(ctx,"kspec.legacy");
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
        },
        0
    };
    if (RedisModule_SetCommandInfo(command, &info) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

int createKspecComplex1(RedisModuleCtx *ctx) {
    /* First is legacy, rest are new specs */
    if (RedisModule_CreateCommand(ctx,"kspec.complex1",kspec_complex1,"",1,1,1) == REDISMODULE_ERR)
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
        },
        0
    };
    if (RedisModule_SetCommandInfo(command, &info) == REDISMODULE_ERR) {
        printf("ERRNO %d (line %d)\n", errno, __LINE__);
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

int createKspecComplex2(RedisModuleCtx *ctx) {
    /* First is not legacy, more than STATIC_KEYS_SPECS_NUM specs */
    if (RedisModule_CreateCommand(ctx,"kspec.complex2",kspec_complex2,"",0,0,0) == REDISMODULE_ERR)
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
        },
        0
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

    if (createKspecLegacy(ctx) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (createKspecComplex1(ctx) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (createKspecComplex2(ctx) == REDISMODULE_ERR) return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

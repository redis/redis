#include "redismodule.h"

#include <string.h>

void InfoFunc(RedisModuleInfoCtx *ctx, int for_crash_report) {
    RedisModule_InfoAddSection(ctx, "");
    RedisModule_InfoAddFieldLongLong(ctx, "global", -2);
    RedisModule_InfoAddFieldULongLong(ctx, "uglobal", (unsigned long long)-2);

    RedisModule_InfoAddSection(ctx, "Spanish");
    RedisModule_InfoAddFieldCString(ctx, "uno", "one");
    RedisModule_InfoAddFieldLongLong(ctx, "dos", 2);

    RedisModule_InfoAddSection(ctx, "Italian");
    RedisModule_InfoAddFieldLongLong(ctx, "due", 2);
    RedisModule_InfoAddFieldDouble(ctx, "tre", 3.3);

    RedisModule_InfoAddSection(ctx, "keyspace");
    RedisModule_InfoBeginDictField(ctx, "db0");
    RedisModule_InfoAddFieldLongLong(ctx, "keys", 3);
    RedisModule_InfoAddFieldLongLong(ctx, "expires", 1);
    RedisModule_InfoEndDictField(ctx);

    if (for_crash_report) {
        RedisModule_InfoAddSection(ctx, "Klingon");
        RedisModule_InfoAddFieldCString(ctx, "one", "wa’");
        RedisModule_InfoAddFieldCString(ctx, "two", "cha’");
        RedisModule_InfoAddFieldCString(ctx, "three", "wej");
    }

}

int info_get(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, char field_type)
{
    if (argc != 3 && argc != 4) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }
    int err = REDISMODULE_OK;
    const char *section, *field;
    section = RedisModule_StringPtrLen(argv[1], NULL);
    field = RedisModule_StringPtrLen(argv[2], NULL);
    RedisModuleServerInfoData *info = RedisModule_GetServerInfo(ctx, section);
    if (field_type=='i') {
        long long ll = RedisModule_ServerInfoGetFieldSigned(info, field, &err);
        if (err==REDISMODULE_OK)
            RedisModule_ReplyWithLongLong(ctx, ll);
    } else if (field_type=='u') {
        unsigned long long ll = (unsigned long long)RedisModule_ServerInfoGetFieldUnsigned(info, field, &err);
        if (err==REDISMODULE_OK)
            RedisModule_ReplyWithLongLong(ctx, ll);
    } else if (field_type=='d') {
        double d = RedisModule_ServerInfoGetFieldDouble(info, field, &err);
        if (err==REDISMODULE_OK)
            RedisModule_ReplyWithDouble(ctx, d);
    } else if (field_type=='c') {
        const char *str = RedisModule_ServerInfoGetFieldC(info, field);
        if (str)
            RedisModule_ReplyWithCString(ctx, str);
    } else {
        RedisModuleString *str = RedisModule_ServerInfoGetField(ctx, info, field);
        if (str) {
            RedisModule_ReplyWithString(ctx, str);
            RedisModule_FreeString(ctx, str);
        } else
            err=REDISMODULE_ERR;
    }
    if (err!=REDISMODULE_OK)
        RedisModule_ReplyWithError(ctx, "not found");
    RedisModule_FreeServerInfo(ctx, info);
    return REDISMODULE_OK;
}

int info_gets(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return info_get(ctx, argv, argc, 's');
}

int info_getc(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return info_get(ctx, argv, argc, 'c');
}

int info_geti(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return info_get(ctx, argv, argc, 'i');
}

int info_getu(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return info_get(ctx, argv, argc, 'u');
}

int info_getd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return info_get(ctx, argv, argc, 'd');
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx,"infotest",1,REDISMODULE_APIVER_1)
            == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_RegisterInfoFunc(ctx, InfoFunc) == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"info.gets", info_gets,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"info.getc", info_getc,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"info.geti", info_geti,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"info.getu", info_getu,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"info.getd", info_getd,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

#include "redismodule.h"
#include <stdio.h>
#include <string.h>

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 1 || argc > 2) return REDISMODULE_ERR;
    /* Bootstrap the one API needed to select the module name before Init. */
    RedisModule_GetApi = (int (*)(const char *, void *))((void **)ctx)[0];
    REDISMODULE_GET_API(StringPtrLen);
    const char *name = argc == 2 ? RedisModule_StringPtrLen(argv[1], NULL) : "replicationcompat";
    if (RedisModule_Init(ctx, name, 1, REDISMODULE_APIVER_1) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    size_t len;
    const char *value = RedisModule_StringPtrLen(argv[0], &len);
    if (!strcmp(value, "none")) return REDISMODULE_OK;
    char port[32];
    if (!strcmp(value, "local-port")) {
        RedisModuleServerInfoData *info = RedisModule_GetServerInfo(ctx, "server");
        int err;
        long long n = RedisModule_ServerInfoGetFieldSigned(info, "tcp_port", &err);
        RedisModule_FreeServerInfo(ctx, info);
        if (err) return REDISMODULE_ERR;
        len = snprintf(port, sizeof(port), "%lld", n);
        value = port;
    }
    return RedisModule_SetReplicationCompatibility(ctx, value, len);
}

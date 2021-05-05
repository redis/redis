#define REDISMODULE_EXPERIMENTAL_API

#include "redismodule.h"
#include <strings.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>

#define UNUSED(V) ((void) V)

/* A sample movable keys command that returns a list of all
 * arguments that follow a KEY argument, i.e.
 */
int getkeys_command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    int i;
    int count = 0;

    /* Handle getkeys-api introspection */
    if (RedisModule_IsKeysPositionRequest(ctx)) {
        for (i = 0; i < argc; i++) {
            size_t len;
            const char *str = RedisModule_StringPtrLen(argv[i], &len);

            if (len == 3 && !strncasecmp(str, "key", 3) && i + 1 < argc)
                RedisModule_KeyAtPos(ctx, i + 1);
        }

        return REDISMODULE_OK;
    }

    /* Handle real command invocation */
    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
    for (i = 0; i < argc; i++) {
        size_t len;
        const char *str = RedisModule_StringPtrLen(argv[i], &len);

        if (len == 3 && !strncasecmp(str, "key", 3) && i + 1 < argc) {
            RedisModule_ReplyWithString(ctx, argv[i+1]);
            count++;
        }
    }
    RedisModule_ReplySetArrayLength(ctx, count);

    return REDISMODULE_OK;
}

int getkeys_fixed(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    int i;

    RedisModule_ReplyWithArray(ctx, argc - 1);
    for (i = 1; i < argc; i++) {
        RedisModule_ReplyWithString(ctx, argv[i]);
    }
    return REDISMODULE_OK;
}

/* Introspect a command using RM_GetCommandKeys() and returns the list
 * of keys. Essentially this is COMMAND GETKEYS implemented in a module.
 */
int getkeys_introspect(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    UNUSED(argv);
    UNUSED(argc);

    if (argc < 3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    int num_keys;
    int *keyidx = RedisModule_GetCommandKeys(ctx, &argv[1], argc - 1, &num_keys);

    if (!keyidx) {
        if (!errno)
            RedisModule_ReplyWithEmptyArray(ctx);
        else {
            char err[100];
            switch (errno) {
                case ENOENT:
                    RedisModule_ReplyWithError(ctx, "ERR ENOENT");
                    break;
                case EINVAL:
                    RedisModule_ReplyWithError(ctx, "ERR EINVAL");
                    break;
                default:
                    snprintf(err, sizeof(err) - 1, "ERR errno=%d", errno);
                    RedisModule_ReplyWithError(ctx, err);
                    break;
            }
        }
    } else {
        int i;

        RedisModule_ReplyWithArray(ctx, num_keys);
        for (i = 0; i < num_keys; i++)
            RedisModule_ReplyWithString(ctx, argv[1 + keyidx[i]]);

        RedisModule_Free(keyidx);
    }

    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    if (RedisModule_Init(ctx,"getkeys",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"getkeys.command", getkeys_command,"getkeys-api",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"getkeys.fixed", getkeys_fixed,"",2,4,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"getkeys.introspect", getkeys_introspect,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

#include "redismodule.h"

#include <strings.h>
#include <sys/mman.h>

#define UNUSED(V) ((void) V)

void assertCrash(RedisModuleInfoCtx *ctx, int for_crash_report) {
    UNUSED(ctx);
    UNUSED(for_crash_report);
    RedisModule_Assert(0);
}

void segfaultCrash(RedisModuleInfoCtx *ctx, int for_crash_report) {
    UNUSED(ctx);
    UNUSED(for_crash_report);
    /* Compiler gives warnings about writing to a random address
     * e.g "*((char*)-1) = 'x';". As a workaround, we map a read-only area
     * and try to write there to trigger segmentation fault. */
    char *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    *p = 'x';
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx,"infocrash",1,REDISMODULE_APIVER_1)
            == REDISMODULE_ERR) return REDISMODULE_ERR;
    RedisModule_Assert(argc == 1);
    if (!strcasecmp(RedisModule_StringPtrLen(argv[0], NULL), "segfault")) {
        if (RedisModule_RegisterInfoFunc(ctx, segfaultCrash) == REDISMODULE_ERR) return REDISMODULE_ERR;
    } else if(!strcasecmp(RedisModule_StringPtrLen(argv[0], NULL), "assert")) {
        if (RedisModule_RegisterInfoFunc(ctx, assertCrash) == REDISMODULE_ERR) return REDISMODULE_ERR;
    } else {
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

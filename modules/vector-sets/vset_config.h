#include "../../src/redismodule.h"

#define HNSW_DEFAULT_MAX_THREADS 32
#define HNSW_MAX_MAX_THREADS 32

typedef struct {
  int hnswMaxThreads;
} VSConfig;

#define RM_TRY(expr)                                                  \
  if (expr == REDISMODULE_ERR) {                                      \
    RedisModule_Log(ctx, "warning", "Could not run " __STRING(expr)); \
    return REDISMODULE_ERR;                                           \
    }

#define RM_TRY_F(f, ...)                                                       \
  if (f(__VA_ARGS__) == REDISMODULE_ERR) {                                     \
    RedisModule_Log(ctx, "warning", "Could not run " #f "(" #__VA_ARGS__ ")"); \
    return REDISMODULE_ERR;                                                    \
  } else {                                                                     \
    RedisModule_Log(ctx, "verbose", "Successfully executed " #f);              \
  }


#define VS_DEFAULT_CONFIG {                                                                           \
  .hnswMaxThreads = HNSW_DEFAULT_MAX_THREADS,                                                         \
}

int RegisterModuleConfig(RedisModuleCtx *ctx);

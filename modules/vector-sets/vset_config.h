#ifndef VSET_CONFIG_H
#define VSET_CONFIG_H

#include "../../src/redismodule.h"

#define HNSW_CONF_DFLT_MAX_THREADS 32
#define HNSW_CONF_MAX_MAX_THREADS 32

typedef struct {
  int hnswMaxThreads;
} VSConfig;

extern VSConfig VSGlobalConfig;


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
  .hnswMaxThreads = HNSW_CONF_DFLT_MAX_THREADS,                                                       \
}

int RegisterModuleConfig(RedisModuleCtx *ctx);

#endif

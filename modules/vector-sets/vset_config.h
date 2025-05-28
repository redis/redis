#ifndef VSET_CONFIG_H
#define VSET_CONFIG_H

#include "../../src/redismodule.h"

#define HNSW_CONF_DFLT_MAX_THREADS 32
#define HNSW_CONF_MAX_MAX_THREADS 32

typedef struct {
  uint8_t hnswMaxThreads;
} VSConfig;

extern VSConfig VSGlobalConfig;

#define VS_DEFAULT_CONFIG {                                                                           \
  .hnswMaxThreads = HNSW_CONF_DFLT_MAX_THREADS,                                                       \
}

int RegisterModuleConfig(RedisModuleCtx *ctx);

#endif

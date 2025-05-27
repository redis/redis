#include "vset_config.h"

VSConfig VSGlobalConfig = VS_DEFAULT_CONFIG;

int set_numeric_config(const char *name, long long val, void *privdata,
    RedisModuleString **err) {
REDISMODULE_NOT_USED(name);
REDISMODULE_NOT_USED(err);
*(long long *)privdata = val;
return REDISMODULE_OK;
}

long long get_numeric_config(const char *name, void *privdata) {
REDISMODULE_NOT_USED(name);
return (*(long long *)privdata);
}

int RegisterModuleConfig(RedisModuleCtx *ctx) {
  RedisModule_Log(ctx, "warning", "config load from config file");
    // Numeric parameters
  RM_TRY(
    RedisModule_RegisterNumericConfig(
      ctx, "vectorset-hnsw_max_threads", HNSW_DEFAULT_MAX_THREADS,
      REDISMODULE_CONFIG_UNPREFIXED | REDISMODULE_CONFIG_IMMUTABLE, 0,
      HNSW_CONF_MAX_MAX_THREADS, get_numeric_config,
      set_numeric_config, NULL,
      (void *)&(VSGlobalConfig.hnswMaxThreads)
    )
  )

  return REDISMODULE_OK;
}

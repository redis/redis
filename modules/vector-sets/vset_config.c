/* vector set module configuration.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/

#include "vset_config.h"

#define RM_TRY(expr)                                                  \
  if (expr == REDISMODULE_ERR) {                                      \
    RedisModule_Log(ctx, "warning", "Could not run " __STRING(expr)); \
    return REDISMODULE_ERR;                                           \
  }

VSConfig VSGlobalConfig;

int set_uint8_numeric_config(const char *name, long long val,
                                                   void *privdata, RedisModuleString **err)
{
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(err);
    *(uint8_t * ) privdata = (uint8_t) val;
    return REDISMODULE_OK;
}

long long get_uint8_numeric_config(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(name);
    return (long long)(*(uint8_t *)privdata);
}

int RegisterModuleConfig(RedisModuleCtx *ctx) {
  // Numeric parameters
  RM_TRY(
    RedisModule_RegisterNumericConfig(
      ctx, "vectorset-hnsw-max-threads", VSET_DEFAULT_MAX_THREADS,
      REDISMODULE_CONFIG_UNPREFIXED | REDISMODULE_CONFIG_IMMUTABLE, 0,
      VSET_MAX_ALLOWED_THREADS, get_uint8_numeric_config,
      set_uint8_numeric_config, NULL,
      (void *)&(VSGlobalConfig.hnswMaxThreads)
    )
  )

  return REDISMODULE_OK;
}

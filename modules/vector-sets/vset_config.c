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

/* Define __STRING macro for portability (not available in all environments) */
#ifndef __STRING
#define __STRING(x) #x
#endif

#define RM_TRY(expr)                                                  \
  if (expr == REDISMODULE_ERR) {                                      \
    RedisModule_Log(ctx, "warning", "Could not run " __STRING(expr)); \
    return REDISMODULE_ERR;                                           \
  }

VSConfig VSGlobalConfig;

int set_bool_config(const char *name, int val, void *privdata,
                    RedisModuleString **err) {
  REDISMODULE_NOT_USED(name);
  REDISMODULE_NOT_USED(err);
  *(int *)privdata = val;
  return REDISMODULE_OK;
}

int get_bool_config(const char *name, void *privdata) {
  REDISMODULE_NOT_USED(name);
  return *(int *)privdata;
}

int RegisterModuleConfig(RedisModuleCtx *ctx) {
  // Numeric parameters
  RM_TRY(
    RedisModule_RegisterBoolConfig(
      ctx, "vset-force-single-threaded-execution", 0,
      REDISMODULE_CONFIG_UNPREFIXED,
      get_bool_config, set_bool_config, NULL,
      (void *)&(VSGlobalConfig.forceSingleThreadExec)
    )
  )

  return REDISMODULE_OK;
}

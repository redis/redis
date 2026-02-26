/* vector set module configuration.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
*/

#ifndef VSET_CONFIG_H
#define VSET_CONFIG_H

#include "../../src/redismodule.h"

typedef struct {
  int forceSingleThreadExec;
} VSConfig;

extern VSConfig VSGlobalConfig;

int RegisterModuleConfig(RedisModuleCtx *ctx);

#endif

/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#ifndef REDIS_RANDOM_H
#define REDIS_RANDOM_H

int32_t redisLrand48(void);
void redisSrand48(int32_t seedval);

#define REDIS_LRAND48_MAX INT32_MAX

#endif

/*
 * CPU capability flags definitions.
 *
 * Copyright (c) 2025-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef REDIS_CPU_CAP_H
#define REDIS_CPU_CAP_H

/* CPU capability flags exposed via RedisModule_GetCpuCapabilities() */
#define CPU_CAP_SVE2     (1ULL << 0)  /* ARM64 SVE2 - Scalable Vector Extension 2 */
#define CPU_CAP_SVE      (1ULL << 1)  /* ARM64 SVE - Scalable Vector Extension */
#define CPU_CAP_NEON     (1ULL << 2)  /* ARM64 NEON - Advanced SIMD */
#define CPU_CAP_AVX2     (1ULL << 3)  /* x86 AVX2 */
#define CPU_CAP_AVX512F  (1ULL << 4)  /* x86 AVX-512 Foundation */
#define CPU_CAP_POPCNT   (1ULL << 5)  /* x86 POPCNT instruction */

/* Get CPU capabilities at runtime.
 * This function is called once at server startup and cached. */
uint64_t getCpuCapabilities(void);

#endif /* __REDIS_CPU_CAP_H */

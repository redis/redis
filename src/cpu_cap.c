/*
 * CPU capability detection.
 *
 * Copyright (c) 2025-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include <stdint.h>
#include "cpu_cap.h"

#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif

/* Detect CPU capabilities at runtime.
 * Returns a bitmask of CPU_CAP_* flags. */
uint64_t getCpuCapabilities(void) {
    uint64_t caps = 0;

#if defined(__aarch64__) && defined(__linux__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    unsigned long hwcap2 = getauxval(AT_HWCAP2);
    /* HWCAP_SVE (bit 22 of HWCAP) - Scalable Vector Extension (SVE) */
    if (hwcap & (1UL << 22))
        caps |= CPU_CAP_SVE;
    /* HWCAP2_SVE2 (bit 1 of HWCAP2) - Scalable Vector Extension 2 */
    if (hwcap2 & (1UL << 1))
        caps |= CPU_CAP_SVE2;
    /* HWCAP_ASIMD (bit 1 of HWCAP) - Advanced SIMD/NEON */
    if (hwcap & (1UL << 1))
        caps |= CPU_CAP_NEON;
#elif defined(__x86_64__) || defined(__i386__) || defined(__amd64__)
    /* x86/x64 CPU capability detection using compiler builtins.
     * Note: avx512f check is guarded because older GCC/Clang versions don't
     * support this feature string in __builtin_cpu_supports. */
    if (__builtin_cpu_supports("avx2"))
        caps |= CPU_CAP_AVX2;
#if (defined(__GNUC__) && __GNUC__ >= 5) || (defined(__clang__) && __clang_major__ >= 4)
    if (__builtin_cpu_supports("avx512f"))
        caps |= CPU_CAP_AVX512F;
#endif
    if (__builtin_cpu_supports("popcnt"))
        caps |= CPU_CAP_POPCNT;
#endif

    return caps;
}

/*
 * Gate data type: minimal unit test harness (no dependencies).
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Usage:
 *
 *     #include "testutil.h"
 *     TEST(sigmoid_is_stable) { CHECK(...); CHECK_NEAR(a, b, 1e-12); }
 *     int main(void) { RUN(sigmoid_is_stable); return TEST_REPORT(); }
 *
 * A failed CHECK records the failure, prints file:line and keeps going, so
 * one run reports every broken expectation. TEST_REPORT() returns the
 * process exit status (0 only if every check passed).
 */

#ifndef GATE_TESTUTIL_H
#define GATE_TESTUTIL_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gate_test_checks = 0;
static int gate_test_failures = 0;
static const char *gate_test_current = "?";

#define TEST(name) static void test_##name(void)

#define RUN(name) do { \
    gate_test_current = #name; \
    int _before = gate_test_failures; \
    test_##name(); \
    printf("[%s] %s\n", gate_test_failures == _before ? "ok" : "FAILED", #name); \
} while (0)

#define CHECK(cond) do { \
    gate_test_checks++; \
    if (!(cond)) { \
        gate_test_failures++; \
        printf("  %s:%d: in %s: CHECK(%s) failed\n", __FILE__, __LINE__, \
               gate_test_current, #cond); \
    } \
} while (0)

/* |a - b| <= tol, and neither is NaN. */
#define CHECK_NEAR(a, b, tol) do { \
    double _a = (double)(a), _b = (double)(b), _t = (double)(tol); \
    gate_test_checks++; \
    if (!(fabs(_a - _b) <= _t)) { \
        gate_test_failures++; \
        printf("  %s:%d: in %s: CHECK_NEAR(%s, %s) failed: %.17g vs %.17g (tol %.3g)\n", \
               __FILE__, __LINE__, gate_test_current, #a, #b, _a, _b, _t); \
    } \
} while (0)

#define CHECK_EQ_INT(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    gate_test_checks++; \
    if (_a != _b) { \
        gate_test_failures++; \
        printf("  %s:%d: in %s: CHECK_EQ_INT(%s, %s) failed: %lld vs %lld\n", \
               __FILE__, __LINE__, gate_test_current, #a, #b, _a, _b); \
    } \
} while (0)

#define CHECK_EQ_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    gate_test_checks++; \
    if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) { \
        gate_test_failures++; \
        printf("  %s:%d: in %s: CHECK_EQ_STR(%s, %s) failed: \"%s\" vs \"%s\"\n", \
               __FILE__, __LINE__, gate_test_current, #a, #b, \
               _a ? _a : "(null)", _b ? _b : "(null)"); \
    } \
} while (0)

static inline int gate_test_report(void) {
    printf("%d checks, %d failures\n", gate_test_checks, gate_test_failures);
    return gate_test_failures == 0 ? 0 : 1;
}
#define TEST_REPORT() gate_test_report()

/* Deterministic PRNG for tests and simulations only (splitmix64). Production
 * audit sampling never uses this. */
static inline uint64_t gate_test_rand64(uint64_t *state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
/* Uniform double in [0,1). */
static inline double gate_test_uniform(uint64_t *state) {
    return (double)(gate_test_rand64(state) >> 11) * (1.0 / 9007199254740992.0);
}

#endif /* GATE_TESTUTIL_H */

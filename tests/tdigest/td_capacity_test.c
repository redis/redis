/*
 * Capacity-boundary regression for td_init (follow-up to #41 / #44).
 *
 * The accepted side of the capacity guard is the largest compression whose node capacity
 * (cap = 6*compression + 10) still fits within every limit the helper enforces: INT_MAX and the
 * per-array element counts SIZE_MAX/sizeof(double) and SIZE_MAX/sizeof(long long). On a 64-bit
 * size_t target INT_MAX binds; on a 32-bit size_t target the SIZE_MAX/8 element limit is smaller
 * and binds instead, so the boundary is derived from the minimum rather than assuming INT_MAX.
 * Verifying it through td_new()/td_init() is impractical because at cap ~ INT_MAX the two 8-byte
 * node arrays total ~34 GB (~32 GiB). This test instead includes the translation unit and drives
 * the factored, allocation-free helper capacity_from_compression() directly, so the exact
 * boundary (and one past it) is checked without committing tens of GiB.
 */
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "tdigest.c" /* brings in the static capacity_from_compression + cap_from_compression */

static int failures = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s\n", (msg));                                                  \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

int main(void) {
    /* The binding capacity limit is the minimum of every check the helper enforces:
     * INT_MAX and the per-array element counts SIZE_MAX/sizeof(double|long long). On 64-bit
     * size_t this is INT_MAX; on 32-bit size_t it is the (smaller) SIZE_MAX/8. */
    uint64_t max_capacity = (uint64_t)INT_MAX;
    if (SIZE_MAX / sizeof(double) < max_capacity) {
        max_capacity = (uint64_t)(SIZE_MAX / sizeof(double));
    }
    if (SIZE_MAX / sizeof(long long) < max_capacity) {
        max_capacity = (uint64_t)(SIZE_MAX / sizeof(long long));
    }

    /* The largest integer compression whose capacity still fits within max_capacity. */
    const long long max_ok = (long long)((max_capacity - 10) / 6);

    /* Sanity on the arithmetic: this capacity must be within the limit, the next one must not. */
    const uint64_t cap_ok = cap_from_compression((uint64_t)max_ok);
    const uint64_t cap_over = cap_from_compression((uint64_t)(max_ok + 1));
    CHECK(cap_ok <= max_capacity, "boundary capacity should be within the binding limit");
    CHECK(cap_over > max_capacity, "boundary+1 capacity should exceed the binding limit");

    const size_t SENTINEL = (size_t)0xA5A5A5A5A5A5A5A5ULL;
    size_t cap;

    /* Accepted boundary: returns 0 and writes a capacity that matches the formula. */
    cap = SENTINEL;
    CHECK(capacity_from_compression((double)max_ok, &cap) == 0,
          "largest in-range compression must be accepted");
    CHECK(cap == (size_t)cap_ok, "accepted boundary capacity must match 6*c+10");

    /* One past the boundary: rejected, and *capacity is left untouched. */
    cap = SENTINEL;
    CHECK(capacity_from_compression((double)(max_ok + 1), &cap) == 1,
          "compression just past the boundary must be rejected");
    CHECK(cap == SENTINEL, "rejected input must leave *capacity untouched");

    /* Invalid inputs are rejected and never touch *capacity. */
    const double bad[] = {NAN, INFINITY, -INFINITY, 0.0, -1.0, (double)INT_MAX};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        cap = SENTINEL;
        CHECK(capacity_from_compression(bad[i], &cap) == 1, "invalid compression must be rejected");
        CHECK(cap == SENTINEL, "rejected input must leave *capacity untouched");
    }

    /* A small valid compression still computes the documented capacity. */
    cap = SENTINEL;
    CHECK(capacity_from_compression(100.0, &cap) == 0, "compression 100 must be accepted");
    CHECK(cap == 610, "cap(100) must be 6*100 + 10");

    if (failures == 0) {
        printf("OK: capacity boundary max_ok=%lld cap=%llu\n", max_ok, (unsigned long long)cap_ok);
        return 0;
    }
    fprintf(stderr, "%d capacity check(s) failed\n", failures);
    return 1;
}

void _serverAssert(const char *e, const char *f, int line) {
    fprintf(stderr,"%s:%d: %s\n",f,line,e); abort();
}

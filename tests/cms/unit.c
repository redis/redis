/* Count-Min Sketch algorithm regression tests, derived from RedisBloom's
 * tests/unit/test_cms.c workloads, with assertions instead of printed output.
 * Copyright (c) 2026-Present, Redis Ltd.; RSALv2/SSPLv1/AGPLv3. */
#include "cms.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void _serverAssert(const char *expression, const char *file, int line) {
    fprintf(stderr, "%s:%d: %s\n", file, line, expression);
    abort();
}

int main(void) {
    const unsigned sizes[] = {1, 2, 4, 8};
    for (unsigned i = 0; i < 4; i++) {
        CMSketch *a = NewCMSketch(100, 5, sizes[i]);
        CMSketch *b = NewCMSketch(100, 5, sizes[i]);
        assert(a && b);
        uint64_t count;
        assert(CMS_IncrBy(a, "a", 1, 10, &count) == CMS_STATUS_OK && count == 10);
        assert(CMS_IncrBy(a, "a", 1, -11, &count) == CMS_STATUS_UNDERFLOW);
        assert(CMS_Query(a, "a", 1) == 10);
        assert(CMS_IncrBy(a, "a", 1, -3, &count) == CMS_STATUS_OK && count == 7);
        assert(CMS_IncrBy(b, "a", 1, 2, &count) == CMS_STATUS_OK);
        const CMSketch *sources[] = {a, b};
        long long weights[] = {2, -1};
        assert(CMS_Merge(a, 2, sources, weights) == 0);
        assert(CMS_Query(a, "a", 1) == 12 && a->counter == 12);
        weights[0] = LLONG_MAX;
        assert(CMS_Merge(a, 2, sources, weights) != 0);
        assert(CMS_Query(a, "a", 1) == 12 && a->counter == 12);
        CMS_Destroy(a); CMS_Destroy(b);
        a = NewCMSketch(1, 1, sizes[i]);
        assert(CMS_IncrBy(a, "a", 1, CMS_CELL_MAX(sizes[i]), &count) == CMS_STATUS_OK);
        assert(CMS_IncrBy(a, "a", 1, 1, &count) == CMS_STATUS_OVERFLOW);
        assert(CMS_Query(a, "a", 1) == CMS_CELL_MAX(sizes[i]));
        CMS_Destroy(a);
    }
    CMSketch *cms = NewCMSketch(2000, 7, 4);
    char item[32];
    uint64_t count;
    for (int i = 0; i < 10000; i++) {
        snprintf(item, sizeof(item), "%d", i % 1000);
        assert(CMS_IncrBy(cms, item, strlen(item), 1, &count) == CMS_STATUS_OK);
    }
    for (int i = 0; i < 1000; i++) {
        snprintf(item, sizeof(item), "%d", i);
        assert(CMS_Query(cms, item, strlen(item)) >= 10);
    }
    assert(cms->counter == 10000);
    CMS_Destroy(cms);
    assert(NewCMSketch(SIZE_MAX, 2, 8) == NULL);
    puts("CMS algorithm tests passed");
    return 0;
}

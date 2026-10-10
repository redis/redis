#include "bloom.h"
#include <stdio.h>

#define NUM_ITERATIONS 50000000
#define NUM_ITEMS 100000
#define ERROR_RATE 0.0001


int main(void) {

    int err;

    SBChain *chain = SB_NewChain(NUM_ITEMS, ERROR_RATE, 0, 2, &err);
    for (size_t ii = 0; ii < NUM_ITERATIONS; ++ii) {
        size_t elem = ii % NUM_ITEMS;
        SBChain_Add(chain, &elem, sizeof elem);
        SBChain_Check(chain, &elem, sizeof elem);
    }
    SBChain_Free(chain);
    return 0;
}

void _serverAssert(const char *expression, const char *file, int line) {
    fprintf(stderr, "%s:%d: %s\n", file, line, expression);
    abort();
}

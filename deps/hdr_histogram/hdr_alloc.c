/**
 * hdr_alloc.c
 * Written by Filipe Oliveira and released to the public domain,
 * as explained at http://creativecommons.org/publicdomain/zero/1.0/
 */

#include "hdr_alloc.h"
#include <stdlib.h>

hdrAllocFuncs hdrAllocFns = {
    .mallocFn = malloc,
    .callocFn = calloc,
    .reallocFn = realloc,
    .freeFn = free,
};

/* Override hdr' allocators with ones supplied by the user */
hdrAllocFuncs hdrSetAllocators(hdrAllocFuncs *override) {
    hdrAllocFuncs orig = hdrAllocFns;

    hdrAllocFns = *override;

    return orig;
}

/* Reset allocators to use build time defaults */
void hdrResetAllocators(void) {
    hdrAllocFns = (hdrAllocFuncs){
        .mallocFn = malloc,
        .callocFn = calloc,
        .reallocFn = realloc,
        .freeFn = free,
    };
}

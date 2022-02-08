/**
 * hdr_alloc.h
 * Written by Filipe Oliveira and released to the public domain,
 * as explained at http://creativecommons.org/publicdomain/zero/1.0/
 *
 * Allocator selection.
 *
 * This file is used in order to change the HdrHistogram allocator at run
 * time. */

#ifndef HDR_ALLOC_H
#define HDR_ALLOC_H

#include <stddef.h> /* for size_t */
#include <stdint.h>

/* Structure pointing to our actually configured allocators */
typedef struct hdrAllocFuncs {
    void *(*mallocFn)(size_t);
    void *(*callocFn)(size_t, size_t);
    void *(*reallocFn)(void *, size_t);
    void (*freeFn)(void *);
} hdrAllocFuncs;

/* hdr' configured allocator function pointer struct */
extern hdrAllocFuncs hdrAllocFns;

hdrAllocFuncs hdrSetAllocators(hdrAllocFuncs *ha);
void hdrResetAllocators(void);

static inline void *hdr_malloc(size_t size) {
    return hdrAllocFns.mallocFn(size);
}

static inline void *hdr_calloc(size_t nmemb, size_t size) {
    return hdrAllocFns.callocFn(nmemb, size);
}

static inline void *hdr_realloc(void *ptr, size_t size) {
    return hdrAllocFns.reallocFn(ptr, size);
}

static inline void hdr_free(void *ptr) {
    hdrAllocFns.freeFn(ptr);
}

#endif /* HDR_ALLOC_H */

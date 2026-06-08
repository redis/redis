#include "server.h"
#include "bitmap_roaring.h"

#include <roaring/memory.h>

static void *bitmapRoaringMalloc(size_t size) {
    return zmalloc(size);
}

static void *bitmapRoaringRealloc(void *ptr, size_t size) {
    return zrealloc(ptr, size);
}

static void *bitmapRoaringCalloc(size_t nmemb, size_t size) {
    return zcalloc_num(nmemb, size);
}

static void bitmapRoaringFree(void *ptr) {
    zfree(ptr);
}

static int bitmapRoaringNormalizeAlignment(size_t *alignment) {
    size_t normalized;

    if (*alignment < sizeof(void *)) {
        *alignment = sizeof(void *);
        return C_OK;
    }
    if ((*alignment & (*alignment - 1)) == 0) return C_OK;

    normalized = sizeof(void *);
    while (normalized < *alignment) {
        if (normalized > SIZE_MAX / 2) return C_ERR;
        normalized <<= 1;
    }
    *alignment = normalized;
    return C_OK;
}

static void *bitmapRoaringAlignedMalloc(size_t alignment, size_t size) {
    void *base;
    uintptr_t raw, aligned;
    size_t extra;

    if (bitmapRoaringNormalizeAlignment(&alignment) != C_OK) return NULL;
    if (alignment - 1 > SIZE_MAX - sizeof(void *)) return NULL;
    extra = alignment - 1 + sizeof(void *);
    if (size > SIZE_MAX - extra) return NULL;

    base = zmalloc(size + extra);
    raw = (uintptr_t)base + sizeof(void *);
    aligned = (raw + alignment - 1) & ~((uintptr_t)alignment - 1);
    ((void **)aligned)[-1] = base;
    return (void *)aligned;
}

static void bitmapRoaringAlignedFree(void *ptr) {
    if (ptr == NULL) return;
    zfree(((void **)ptr)[-1]);
}

void bitmapRoaringInit(void) {
    roaring_memory_t memory_hook = {
        .malloc = bitmapRoaringMalloc,
        .realloc = bitmapRoaringRealloc,
        .calloc = bitmapRoaringCalloc,
        .free = bitmapRoaringFree,
        .aligned_malloc = bitmapRoaringAlignedMalloc,
        .aligned_free = bitmapRoaringAlignedFree,
    };

    roaring_init_memory_hook(memory_hook);
}

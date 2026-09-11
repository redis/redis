#include <stdlib.h>

#include <roaring/memory.h>

// without the following, we get lots of warnings about posix_memalign
#ifndef __cplusplus
extern int posix_memalign(void** __memptr, size_t __alignment, size_t __size);
#endif  //__cplusplus // C++ does not have a well defined signature

// portable version of  posix_memalign
static void* roaring_bitmap_aligned_malloc(size_t alignment, size_t size) {
    void* p;
#ifdef _MSC_VER
    p = _aligned_malloc(size, alignment);
#elif defined(__MINGW32__) || defined(__MINGW64__)
    p = __mingw_aligned_malloc(size, alignment);
#else
    // somehow, if this is used before including "x86intrin.h", it creates an
    // implicit defined warning.
    if (posix_memalign(&p, alignment, size) != 0) return NULL;
#endif
    return p;
}

static void roaring_bitmap_aligned_free(void* memblock) {
#ifdef _MSC_VER
    _aligned_free(memblock);
#elif defined(__MINGW32__) || defined(__MINGW64__)
    __mingw_aligned_free(memblock);
#else
    free(memblock);
#endif
}

static roaring_memory_t global_memory_hook = {
    .malloc = malloc,
    .realloc = realloc,
    .calloc = calloc,
    .free = free,
    .aligned_malloc = roaring_bitmap_aligned_malloc,
    .aligned_free = roaring_bitmap_aligned_free,
};

void roaring_init_memory_hook(roaring_memory_t memory_hook) {
    global_memory_hook = memory_hook;
}

void* roaring_malloc(size_t n) { return global_memory_hook.malloc(n); }

void* roaring_realloc(void* p, size_t new_sz) {
    return global_memory_hook.realloc(p, new_sz);
}

void* roaring_calloc(size_t n_elements, size_t element_size) {
    return global_memory_hook.calloc(n_elements, element_size);
}

void roaring_free(void* p) { global_memory_hook.free(p); }

void* roaring_aligned_malloc(size_t alignment, size_t size) {
    return global_memory_hook.aligned_malloc(alignment, size);
}

void roaring_aligned_free(void* p) { global_memory_hook.aligned_free(p); }

#include "server.h"
#include "atomicvar.h"

#include <memkind.h>
#include <memkind/internal/memkind_pmem.h>

#define update_memkind_malloc_stat_alloc(__n) do { \
    size_t _n = (__n); \
    if (_n&(sizeof(long)-1)) _n += sizeof(long)-(_n&(sizeof(long)-1)); \
    atomicIncr(used_memory,__n); \
} while(0)

#define update_memkind_malloc_stat_free(__n) do { \
    size_t _n = (__n); \
    if (_n&(sizeof(long)-1)) _n += sizeof(long)-(_n&(sizeof(long)-1)); \
    atomicDecr(used_memory,__n); \
} while(0)

static size_t used_memory = 0;

void *memkind_alloc_wrapper(size_t size) {
    void *ptr = memkind_malloc(server.pmem_kind1, size);
    if (ptr) {
        size = jemk_malloc_usable_size(ptr);
        update_memkind_malloc_stat_alloc(size);
    }
    return ptr;
}

void *memkind_calloc_wrapper(size_t size) {
    void *ptr = memkind_calloc(server.pmem_kind1, 1, size);
    if (ptr) {
        size = jemk_malloc_usable_size(ptr);
        update_memkind_malloc_stat_alloc(size);
    }
    return ptr;
}

void *memkind_realloc_wrapper(void *ptr, size_t size) {
    size_t oldsize;
    if (ptr == NULL) return memkind_alloc_wrapper(size);
    oldsize = jemk_malloc_usable_size(ptr);
    void *newptr = memkind_realloc(server.pmem_kind1, ptr, size);
    if (newptr) {
        update_memkind_malloc_stat_free(oldsize);
        size = jemk_malloc_usable_size(newptr);
        update_memkind_malloc_stat_alloc(size);
    }
    return newptr;
}

void memkind_free_wrapper(void *ptr) {
    if(!ptr) return;
    size_t oldsize = jemk_malloc_usable_size(ptr);
    update_memkind_malloc_stat_free(oldsize);
    memkind_free(server.pmem_kind1, ptr);
}

void memkind_free_no_tcache_wrapper(void *ptr) {
    if(!ptr) return;
    size_t oldsize;
    oldsize = jemk_malloc_usable_size(ptr);
    update_memkind_malloc_stat_free(oldsize);
    jemk_dallocx(ptr, MALLOCX_TCACHE_NONE);
}

size_t memkind_malloc_used_memory(void){
    size_t um;
    atomicGet(used_memory,um);
    return um;
}

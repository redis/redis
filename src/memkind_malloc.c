#include "server.h"
#include "atomicvar.h"

#include <memkind.h>
#include <memkind/internal/memkind_pmem.h>

#define PREFIX_SIZE (sizeof(size_t))

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
    void *ptr = memkind_malloc(server.pmem_kind1, size+PREFIX_SIZE);
    if (ptr==NULL) return NULL;
    *((size_t*)ptr) = size;
    update_memkind_malloc_stat_alloc(size+PREFIX_SIZE);
    return (char*)ptr+PREFIX_SIZE;
}

void *memkind_calloc_wrapper(size_t size) {
    void *ptr = memkind_calloc(server.pmem_kind1, 1, size+PREFIX_SIZE);
    if (ptr==NULL) return NULL;
    *((size_t*)ptr) = size;
    update_memkind_malloc_stat_alloc(size+PREFIX_SIZE);
    return (char*)ptr+PREFIX_SIZE;
}

void *memkind_realloc_wrapper(void *ptr, size_t size) {
    size_t oldsize;
    void *realptr;
    if (ptr == NULL) return memkind_alloc_wrapper(size);
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    void *newptr = memkind_realloc(server.pmem_kind1, realptr, size+PREFIX_SIZE);
    if (newptr==NULL) return NULL;

    *((size_t*)newptr) = size;
    update_memkind_malloc_stat_free(oldsize);
    update_memkind_malloc_stat_alloc(size);
    return (char*)newptr+PREFIX_SIZE;
}

void memkind_free_wrapper(void *ptr) {
    if(!ptr) return;
    void *realptr;
    size_t oldsize;
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    update_memkind_malloc_stat_free(oldsize+PREFIX_SIZE);
    memkind_free(server.pmem_kind1, realptr);
}


size_t memkind_malloc_used_memory(void){
    size_t um;
    atomicGet(used_memory,um);
    return um;
}

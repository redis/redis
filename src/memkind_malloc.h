size_t jemk_malloc_usable_size(void *ptr);
int jemk_get_defrag_hint(void* ptr, int *bin_util, int *run_util);
int jemk_mallctl(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
void jemk_dallocx(void *ptr, int flags);

void *memkind_alloc_wrapper(size_t size);
void *memkind_calloc_wrapper(size_t size);
void *memkind_realloc_wrapper(void *ptr, size_t size);
void memkind_free_wrapper(void *ptr);
void memkind_free_no_tcache_wrapper(void *ptr);
size_t memkind_malloc_used_memory(void);

#define mmalloc memkind_alloc_wrapper
#define mcalloc memkind_calloc_wrapper
#define mrealloc memkind_realloc_wrapper
#define mfree memkind_free_wrapper
#define mmalloc_usable_size jemk_malloc_usable_size
#define mget_defrag_hint jemk_get_defrag_hint
#define mfree_no_tcache memkind_free_no_tcache_wrapper

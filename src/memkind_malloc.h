void *memkind_alloc_wrapper(size_t size);
void *memkind_calloc_wrapper(size_t size);
void *memkind_realloc_wrapper(void *ptr, size_t size);
void memkind_free_wrapper(void *ptr);

#define mmalloc memkind_alloc_wrapper
#define mcalloc memkind_calloc_wrapper
#define mrealloc memkind_realloc_wrapper
#define mfree memkind_free_wrapper

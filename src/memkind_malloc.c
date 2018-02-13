#include "server.h"

#include <memkind.h>
#include <memkind/internal/memkind_pmem.h>

void *memkind_alloc_wrapper(size_t size) {
	void *ptr = memkind_malloc(server.pmem_kind1, size);
	printf("alloc size=%d, ptr=%p\n",size, ptr);
	return ptr;
}

void *memkind_calloc_wrapper(size_t size) {
	void *ptr = memkind_calloc(server.pmem_kind1, 1, size);
	printf("calloc size=%d, ptr=%p\n",size, ptr);
	return ptr;
}

void *memkind_realloc_wrapper(void *ptr, size_t size) {
	if (ptr == NULL) {
		void *newptr = memkind_malloc(server.pmem_kind1, size);
		printf("realloc null size=%d, ptr=%p\n",size, newptr);
		return newptr;
	}
	void *newptr = memkind_realloc(server.pmem_kind1, ptr, size);
	printf("realloc,size=%d, ptr=%p, newptr=%p\n", size, ptr, newptr);
	return newptr;
}

void memkind_free_wrapper(void *ptr) {
	printf("free %p\n", ptr);
	memkind_free(server.pmem_kind1, ptr);
	printf("free %p done\n", ptr);
}

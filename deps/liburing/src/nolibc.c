/* SPDX-License-Identifier: MIT */

#ifndef CONFIG_NOLIBC
#error "This file should only be compiled for no libc build"
#endif

#include "lib.h"
#include "syscall.h"

void *__uring_memset(void *s, int c, size_t n)
{
	size_t i;
	unsigned char *p = s;

	for (i = 0; i < n; i++) {
		p[i] = (unsigned char) c;

		/*
		 * An empty inline ASM to avoid auto-vectorization
		 * because it's too bloated for liburing.
		 */
		__asm__ volatile ("");
	}

	return s;
}

struct uring_heap {
	size_t		len;
	char		user_p[] __attribute__((__aligned__));
};

void *__uring_malloc(size_t len)
{
	struct uring_heap *heap;

	heap = __sys_mmap(NULL, sizeof(*heap) + len, PROT_READ | PROT_WRITE,
			  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (IS_ERR(heap))
		return NULL;

	heap->len = sizeof(*heap) + len;
	return heap->user_p;
}

void __uring_free(void *p)
{
	struct uring_heap *heap;

	if (uring_unlikely(!p))
		return;

	heap = container_of(p, struct uring_heap, user_p);
	__sys_munmap(heap, heap->len);
}

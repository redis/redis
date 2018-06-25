#include "jemalloc/internal/jemalloc_internal.h"
#ifndef JEMALLOC_VALGRIND
#  error "This source file is for Valgrind integration."
#endif

#include <valgrind/memcheck.h>

void
valgrind_make_mem_noaccess(void *ptr, size_t usize)
{

	VALGRIND_MAKE_MEM_NOACCESS(ptr, usize);
}

void
valgrind_make_mem_undefined(void *ptr, size_t usize)
{

	VALGRIND_MAKE_MEM_UNDEFINED(ptr, usize);
}

void
valgrind_make_mem_defined(void *ptr, size_t usize)
{

	VALGRIND_MAKE_MEM_DEFINED(ptr, usize);
}

void
valgrind_freelike_block(void *ptr, size_t usize)
{

	VALGRIND_FREELIKE_BLOCK(ptr, usize);
}

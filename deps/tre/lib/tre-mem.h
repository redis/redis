/*
  tre-mem.h - TRE memory allocator interface

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/

#ifndef TRE_MEM_H
#define TRE_MEM_H 1

#include <stdlib.h>

#define TRE_MEM_BLOCK_SIZE 1024

typedef struct tre_list {
  void *data;
  struct tre_list *next;
} tre_list_t;

typedef struct tre_mem_struct {
  tre_list_t *blocks;
  tre_list_t *current;
  char *ptr;
  size_t n;
  int failed;
  void **provided;
} *tre_mem_t;


tre_mem_t tre_mem_new_impl(int provided, void *provided_block);
void *tre_mem_alloc_impl(tre_mem_t mem, int provided, void *provided_block,
			 int zero, size_t size);

/* Returns a new memory allocator or NULL if out of memory. */
#define tre_mem_new()  tre_mem_new_impl(0, NULL)

/* Allocates a block of `size' bytes from `mem'.  Returns a pointer to the
   allocated block or NULL if an underlying malloc() failed. */
#define tre_mem_alloc(mem, size) tre_mem_alloc_impl(mem, 0, NULL, 0, size)

/* Allocates a block of `size' bytes from `mem'.  Returns a pointer to the
   allocated block or NULL if an underlying malloc() failed.  The memory
   is set to zero. */
#define tre_mem_calloc(mem, size) tre_mem_alloc_impl(mem, 0, NULL, 1, size)

#ifdef TRE_USE_ALLOCA
/* alloca() versions.  Like above, but memory is allocated with alloca()
   instead of malloc(). */

#define tre_mem_newa() \
  tre_mem_new_impl(1, alloca(sizeof(struct tre_mem_struct)))

#define tre_mem_alloca(mem, size)					      \
  ((mem)->n >= (size)							      \
   ? tre_mem_alloc_impl((mem), 1, NULL, 0, (size))			      \
   : tre_mem_alloc_impl((mem), 1, alloca(TRE_MEM_BLOCK_SIZE), 0, (size)))
#endif /* TRE_USE_ALLOCA */


/* Frees the memory allocator and all memory allocated with it. */
void tre_mem_destroy(tre_mem_t mem);

#endif /* TRE_MEM_H */

/* EOF */

/*
  tre-mem.c - TRE memory allocator

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/

/*
  This memory allocator is for allocating small memory blocks efficiently
  in terms of memory overhead and execution speed.  The allocated blocks
  cannot be freed individually, only all at once.  There can be multiple
  allocators, though.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */
#include <stdlib.h>
#include <string.h>

#include "tre-internal.h"
#include "tre-mem.h"
#include "xmalloc.h"


/* Returns a new memory allocator or NULL if out of memory. */
tre_mem_t
tre_mem_new_impl(int provided, void *provided_block)
{
  tre_mem_t mem;
  if (provided)
    {
      mem = provided_block;
      memset(mem, 0, sizeof(*mem));
    }
  else
    mem = xcalloc(1, sizeof(*mem));
  if (mem == NULL)
    return NULL;
  return mem;
}


/* Frees the memory allocator and all memory allocated with it. */
void
tre_mem_destroy(tre_mem_t mem)
{
  tre_list_t *tmp, *l = mem->blocks;

  while (l != NULL)
    {
      xfree(l->data);
      tmp = l->next;
      xfree(l);
      l = tmp;
    }
  xfree(mem);
}


/* Allocates a block of `size' bytes from `mem'.  Returns a pointer to the
   allocated block or NULL if an underlying malloc() failed. */
void *
tre_mem_alloc_impl(tre_mem_t mem, int provided, void *provided_block,
		   int zero, size_t size)
{
  void *ptr;

  if (mem->failed)
    {
      DPRINT(("tre_mem_alloc: oops, called after failure?!\n"));
      return NULL;
    }

#ifdef MALLOC_DEBUGGING
  if (!provided)
    {
      ptr = xmalloc(1);
      if (ptr == NULL)
	{
	  DPRINT(("tre_mem_alloc: xmalloc forced failure\n"));
	  mem->failed = 1;
	  return NULL;
	}
      xfree(ptr);
    }
#endif /* MALLOC_DEBUGGING */

  if (mem->n < size)
    {
      /* We need more memory than is available in the current block.
	 Allocate a new block. */
      tre_list_t *l;
      if (provided)
	{
	  DPRINT(("tre_mem_alloc: using provided block\n"));
	  if (provided_block == NULL)
	    {
	      DPRINT(("tre_mem_alloc: provided block was NULL\n"));
	      mem->failed = 1;
	      return NULL;
	    }
	  mem->ptr = provided_block;
	  mem->n = TRE_MEM_BLOCK_SIZE;
	}
      else
	{
	  size_t block_size;
	  if (size * 8 > TRE_MEM_BLOCK_SIZE)
	    block_size = size * 8;
	  else
	    block_size = TRE_MEM_BLOCK_SIZE;
	  DPRINT(("tre_mem_alloc: allocating new %zu byte block\n",
		  block_size));
	  l = xmalloc(sizeof(*l));
	  if (l == NULL)
	    {
	      mem->failed = 1;
	      return NULL;
	    }
	  l->data = xmalloc(block_size);
	  if (l->data == NULL)
	    {
	      xfree(l);
	      mem->failed = 1;
	      return NULL;
	    }
	  l->next = NULL;
	  if (mem->current != NULL)
	    mem->current->next = l;
	  if (mem->blocks == NULL)
	    mem->blocks = l;
	  mem->current = l;
	  mem->ptr = l->data;
	  mem->n = block_size;
	}
    }

  /* Make sure the next pointer will be aligned. */
  size += ALIGN(mem->ptr + size, long);

  /* Allocate from current block. */
  ptr = mem->ptr;
  mem->ptr += size;
  mem->n -= size;

  /* Set to zero if needed. */
  if (zero)
    memset(ptr, 0, size);

  return ptr;
}

/* EOF */

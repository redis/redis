/*
  xmalloc.c - Simple malloc debugging library implementation

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/

/*
  TODO:
   - red zones
   - group dumps by source location
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#define XMALLOC_INTERNAL 1
#include "xmalloc.h"


/*
  Internal stuff.
*/

typedef struct hashTableItemRec {
  void *ptr;
  size_t bytes;
  const char *file;
  int line;
  const char *func;
  struct hashTableItemRec *next;
} hashTableItem;

typedef struct {
  hashTableItem **table;
} hashTable;

static int xmalloc_peak;
int xmalloc_current;
static int xmalloc_peak_blocks;
int xmalloc_current_blocks;
static int xmalloc_fail_after;

#define TABLE_BITS 8
#define TABLE_MASK ((1 << TABLE_BITS) - 1)
#define TABLE_SIZE (1 << TABLE_BITS)

static hashTable *
hash_table_new(void)
{
  hashTable *tbl;

  tbl = malloc(sizeof(*tbl));

  if (tbl != NULL)
    {
      tbl->table = calloc(TABLE_SIZE, sizeof(*tbl->table));

      if (tbl->table == NULL)
	{
	  free(tbl);
	  return NULL;
	}
    }

  return tbl;
}

static unsigned int
hash_void_ptr(void *ptr)
{
  unsigned int hash;
  unsigned int i;

  /* I took this hash function just off the top of my head, I have
     no idea whether it is bad or very bad. */
  hash = 0;
  for (i = 0; i < sizeof(ptr) * 8 / TABLE_BITS; i++)
    {
      hash ^= (uintptr_t)ptr >> i * 8;
      hash += i * 17;
      hash &= TABLE_MASK;
    }
  return hash;
}

static void
hash_table_add(hashTable *tbl, void *ptr, size_t bytes,
	       const char *file, int line, const char *func)
{
  unsigned int i;
  hashTableItem *item, *new;

  i = hash_void_ptr(ptr);

  item = tbl->table[i];
  if (item != NULL)
    while (item->next != NULL)
      item = item->next;

  new = malloc(sizeof(*new));
  assert(new != NULL);
  new->ptr = ptr;
  new->bytes = bytes;
  new->file = file;
  new->line = line;
  new->func = func;
  new->next = NULL;
  if (item != NULL)
    item->next = new;
  else
    tbl->table[i] = new;

  xmalloc_current += bytes;
  if (xmalloc_current > xmalloc_peak)
    xmalloc_peak = xmalloc_current;
  xmalloc_current_blocks++;
  if (xmalloc_current_blocks > xmalloc_peak_blocks)
    xmalloc_peak_blocks = xmalloc_current_blocks;
}

static void
#if defined(__GNUC__) && __GNUC__ >= 10
__attribute__((access(none, 2)))
#endif
hash_table_del(hashTable *tbl, void *ptr)
{
  int i;
  hashTableItem *item, *prev;

  i = hash_void_ptr(ptr);

  item = tbl->table[i];
  if (item == NULL)
    {
      printf("xfree: invalid ptr %p\n", ptr);
      abort();
    }
  prev = NULL;
  while (item->ptr != ptr)
    {
      prev = item;
      item = item->next;
    }
  if (item->ptr != ptr)
    {
      printf("xfree: invalid ptr %p\n", ptr);
      abort();
    }

  xmalloc_current -= item->bytes;
  xmalloc_current_blocks--;

  if (prev != NULL)
    {
      prev->next = item->next;
      free(item);
    }
  else
    {
      tbl->table[i] = item->next;
      free(item);
    }
}

static hashTable *xmalloc_table = NULL;

static void
xmalloc_init(void)
{
  if (xmalloc_table == NULL)
    {
      xmalloc_table = hash_table_new();
      xmalloc_peak = 0;
      xmalloc_peak_blocks = 0;
      xmalloc_current = 0;
      xmalloc_current_blocks = 0;
      xmalloc_fail_after = -1;
    }
  assert(xmalloc_table != NULL);
  assert(xmalloc_table->table != NULL);
}



/*
  Public API.
*/

void
xmalloc_configure(int fail_after)
{
  xmalloc_init();
  xmalloc_fail_after = fail_after;
}

int
xmalloc_dump_leaks(void)
{
  unsigned int i;
  unsigned int num_leaks = 0;
  size_t leaked_bytes = 0;
  hashTableItem *item;

  xmalloc_init();

  for (i = 0; i < TABLE_SIZE; i++)
    {
      item = xmalloc_table->table[i];
      while (item != NULL)
	{
	  printf("%s:%d: %s: %zu bytes at %p not freed\n",
		 item->file, item->line, item->func, item->bytes, item->ptr);
	  num_leaks++;
	  leaked_bytes += item->bytes;
	  item = item->next;
	}
    }
  if (num_leaks == 0)
    printf("No memory leaks.\n");
  else
    printf("%u unfreed memory chuncks, total %zu unfreed bytes.\n",
	   num_leaks, leaked_bytes);
  printf("Peak memory consumption %d bytes (%.1f kB, %.1f MB) in %d blocks ",
	 xmalloc_peak, (double)xmalloc_peak / 1024,
	 (double)xmalloc_peak / (1024*1024), xmalloc_peak_blocks);
  printf("(average ");
  if (xmalloc_peak_blocks)
    printf("%d", ((xmalloc_peak + xmalloc_peak_blocks / 2)
		  / xmalloc_peak_blocks));
  else
    printf("N/A");
  printf(" bytes per block).\n");

  return num_leaks;
}

void *
xmalloc_impl(size_t size, const char *file, int line, const char *func)
{
  void *ptr;

  xmalloc_init();
  assert(size > 0);

  if (xmalloc_fail_after == 0)
    {
      xmalloc_fail_after = -2;
#if 0
      printf("xmalloc: forced failure %s:%d: %s\n", file, line, func);
#endif
      return NULL;
    }
  else if (xmalloc_fail_after == -2)
    {
      printf("xmalloc: called after failure from %s:%d: %s\n",
	     file, line, func);
      assert(0);
    }
  else if (xmalloc_fail_after > 0)
    xmalloc_fail_after--;

  ptr = malloc(size);
  if (ptr != NULL)
    hash_table_add(xmalloc_table, ptr, (int)size, file, line, func);
  return ptr;
}

void *
xcalloc_impl(size_t nmemb, size_t size, const char *file, int line,
	     const char *func)
{
  void *ptr;

  xmalloc_init();
  assert(size > 0);

  if (xmalloc_fail_after == 0)
    {
      xmalloc_fail_after = -2;
#if 0
      printf("xcalloc: forced failure %s:%d: %s\n", file, line, func);
#endif
      return NULL;
    }
  else if (xmalloc_fail_after == -2)
    {
      printf("xcalloc: called after failure from %s:%d: %s\n",
	     file, line, func);
      assert(0);
    }
  else if (xmalloc_fail_after > 0)
    xmalloc_fail_after--;

  ptr = calloc(nmemb, size);
  if (ptr != NULL)
    hash_table_add(xmalloc_table, ptr, (int)(nmemb * size), file, line, func);
  return ptr;
}

void
xfree_impl(void *ptr, const char *file, int line, const char *func)
{
  /*LINTED*/(void)&file;
  /*LINTED*/(void)&line;
  /*LINTED*/(void)&func;
  xmalloc_init();

  if (ptr != NULL)
    hash_table_del(xmalloc_table, ptr);
  free(ptr);
}

void *
xrealloc_impl(void *ptr, size_t new_size, const char *file, int line,
	      const char *func)
{
  void *new_ptr;

  xmalloc_init();
  assert(ptr != NULL);
  assert(new_size > 0);

  if (xmalloc_fail_after == 0)
    {
      xmalloc_fail_after = -2;
      return NULL;
    }
  else if (xmalloc_fail_after == -2)
    {
      printf("xrealloc: called after failure from %s:%d: %s\n",
	     file, line, func);
      assert(0);
    }
  else if (xmalloc_fail_after > 0)
    xmalloc_fail_after--;

  new_ptr = realloc(ptr, new_size);
  if (new_ptr != NULL && new_ptr != ptr)
    {
      hash_table_del(xmalloc_table, ptr);
      hash_table_add(xmalloc_table, new_ptr, (int)new_size, file, line, func);
    }
  return new_ptr;
}



/* EOF */

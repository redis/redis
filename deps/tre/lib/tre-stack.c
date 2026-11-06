/*
  tre-stack.c - Simple stack implementation

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */
#include <stdlib.h>
#include <assert.h>

#include "tre-internal.h"
#include "tre-stack.h"
#include "xmalloc.h"

union tre_stack_item {
  void *voidptr_value;
  int int_value;
};

struct tre_stack_rec {
  size_t size;
  size_t max_size;
  size_t ptr;
  union tre_stack_item *stack;
};


tre_stack_t *
tre_stack_new(size_t size, size_t max_size)
{
  tre_stack_t *s;

  s = xmalloc(sizeof(*s));
  if (s != NULL)
    {
      s->stack = xmalloc(sizeof(*s->stack) * size);
      if (s->stack == NULL)
	{
	  xfree(s);
	  return NULL;
	}
      s->size = size;
      s->max_size = max_size;
      s->ptr = 0;
    }
  return s;
}

void
tre_stack_destroy(tre_stack_t *s)
{
  xfree(s->stack);
  xfree(s);
}

size_t
tre_stack_num_items(tre_stack_t *s)
{
  return s->ptr;
}

static reg_errcode_t
tre_stack_push(tre_stack_t *s, union tre_stack_item value)
{
  if (s->ptr < s->size)
    {
      s->stack[s->ptr] = value;
      s->ptr++;
    }
  else
    {
      if (s->size >= s->max_size)
	{
	  DPRINT(("tre_stack_push: stack full\n"));
	  return REG_ESPACE;
	}
      else
	{
	  union tre_stack_item *new_buffer;
	  size_t new_size;
	  DPRINT(("tre_stack_push: trying to realloc more space\n"));
	  new_size = s->size + s->size;
	  if (new_size > s->max_size)
	    new_size = s->max_size;
	  new_buffer = xrealloc(s->stack, sizeof(*new_buffer) * new_size);
	  if (new_buffer == NULL)
	    {
	      DPRINT(("tre_stack_push: realloc failed.\n"));
	      return REG_ESPACE;
	    }
	  DPRINT(("tre_stack_push: realloc succeeded.\n"));
	  assert(new_size > s->size);
	  s->size = new_size;
	  s->stack = new_buffer;
	  tre_stack_push(s, value);
	}
    }
  return REG_OK;
}

#define define_pushf(typetag, type)  \
  declare_pushf(typetag, type) {     \
    union tre_stack_item item;	     \
    item.typetag ## _value = value;  \
    return tre_stack_push(s, item);  \
}

define_pushf(int, int)
define_pushf(voidptr, void *)

#define define_popf(typetag, type)		    \
  declare_popf(typetag, type) {			    \
    return s->stack[--s->ptr].typetag ## _value;    \
  }

define_popf(int, int)
define_popf(voidptr, void *)

/* EOF */

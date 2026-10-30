/*
  tre-stack.h: Stack definitions

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/


#ifndef TRE_STACK_H
#define TRE_STACK_H 1

#include "../local_includes/tre.h"

typedef struct tre_stack_rec tre_stack_t;

/* Creates a new stack object with initial size `size' and maximum size
   `max_size'. Pushing an additional item onto a full stack will resize
   the stack to double its capacity until the maximum is reached. Returns
   the stack object or NULL if out of memory. */
tre_stack_t *
tre_stack_new(size_t size, size_t max_size);

/* Frees the stack object. */
void
tre_stack_destroy(tre_stack_t *s);

/* Returns the current number of items on the stack. */
size_t
tre_stack_num_items(tre_stack_t *s);

/* Each tre_stack_push_*(tre_stack_t *s, <type> value) function pushes
   `value' on top of stack `s'.  Returns REG_ESPACE if out of memory.
   This tries to realloc() more space before failing if maximum size
   has not yet been reached.  Returns REG_OK if successful. */
#define declare_pushf(typetag, type)					      \
  reg_errcode_t tre_stack_push_ ## typetag(tre_stack_t *s, type value)

declare_pushf(voidptr, void *);
declare_pushf(int, int);

/* Each tre_stack_pop_*(tre_stack_t *s) function pops the topmost
   element off of stack `s' and returns it.  The stack must not be
   empty. */
#define declare_popf(typetag, type)		  \
  type tre_stack_pop_ ## typetag(tre_stack_t *s)

declare_popf(voidptr, void *);
declare_popf(int, int);

/* Just to save some typing. */
#define STACK_PUSH(s, typetag, value)					      \
  do									      \
    {									      \
      status = tre_stack_push_ ## typetag(s, value);			      \
    }									      \
  while (/*CONSTCOND*/(void)0,0)

#define STACK_PUSHX(s, typetag, value)					      \
  {									      \
    status = tre_stack_push_ ## typetag(s, value);			      \
    if (status != REG_OK)						      \
      break;								      \
  }

#define STACK_PUSHR(s, typetag, value)					      \
  {									      \
    reg_errcode_t _status;						      \
    _status = tre_stack_push_ ## typetag(s, value);			      \
    if (_status != REG_OK)						      \
      return _status;							      \
  }

#endif /* TRE_STACK_H */

/* EOF */

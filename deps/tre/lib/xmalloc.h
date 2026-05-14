/*
  xmalloc.h - Simple malloc debugging library API

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/

#ifndef _XMALLOC_H
#define _XMALLOC_H 1

void *xmalloc_impl(size_t size, const char *file, int line, const char *func);
void *xcalloc_impl(size_t nmemb, size_t size, const char *file, int line,
		   const char *func);
void xfree_impl(void *ptr, const char *file, int line, const char *func);
void *xrealloc_impl(void *ptr, size_t new_size, const char *file, int line,
		    const char *func);
int xmalloc_dump_leaks(void);
void xmalloc_configure(int fail_after);


#ifndef XMALLOC_INTERNAL
#ifdef MALLOC_DEBUGGING

/* Version 2.4 and later of GCC define a magical variable `__PRETTY_FUNCTION__'
   which contains the name of the function currently being defined.
#  define __XMALLOC_FUNCTION	 __PRETTY_FUNCTION__
   This is broken in G++ before version 2.6.
   C9x has a similar variable called __func__, but prefer the GCC one since
   it demangles C++ function names.  */
# ifdef __GNUC__
#  if __GNUC__ > 2 || (__GNUC__ == 2 \
		       && __GNUC_MINOR__ >= (defined __cplusplus ? 6 : 4))
#   define __XMALLOC_FUNCTION	 __PRETTY_FUNCTION__
#  else
#   define __XMALLOC_FUNCTION	 ((const char *) 0)
#  endif
# else
#  if defined __STDC_VERSION__ && __STDC_VERSION__ >= 199901L
#   define __XMALLOC_FUNCTION	 __func__
#  else
#   define __XMALLOC_FUNCTION	 ((const char *) 0)
#  endif
# endif

#define xmalloc(size) xmalloc_impl(size, __FILE__, __LINE__, \
				   __XMALLOC_FUNCTION)
#define xcalloc(nmemb, size) xcalloc_impl(nmemb, size, __FILE__, __LINE__, \
					  __XMALLOC_FUNCTION)
#define xfree(ptr) xfree_impl(ptr, __FILE__, __LINE__, __XMALLOC_FUNCTION)
#define xrealloc(ptr, new_size) xrealloc_impl(ptr, new_size, __FILE__, \
					      __LINE__, __XMALLOC_FUNCTION)
#undef malloc
#undef calloc
#undef free
#undef realloc

#define malloc	USE_XMALLOC_INSTEAD_OF_MALLOC
#define calloc	USE_XCALLOC_INSTEAD_OF_CALLOC
#define free	USE_XFREE_INSTEAD_OF_FREE
#define realloc USE_XREALLOC_INSTEAD_OF_REALLOC

#else /* !MALLOC_DEBUGGING */

#include <stdlib.h>

#define xmalloc(size) malloc(size)
#define xcalloc(nmemb, size) calloc(nmemb, size)
#define xfree(ptr) free(ptr)
#define xrealloc(ptr, new_size) realloc(ptr, new_size)

#endif /* !MALLOC_DEBUGGING */
#endif /* !XMALLOC_INTERNAL */

#endif /* _XMALLOC_H */

/* EOF */

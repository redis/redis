#ifndef __CONFIG_H
#define __CONFIG_H

/* test for malloc_size() */
#ifdef __APPLE__
#include <malloc/malloc.h>
#define HAVE_MALLOC_SIZE 1
#define redis_malloc_size(p) malloc_size(p)
#endif

/* define redis_fstat to fstat or fstat64() */
#ifdef __APPLE__
#define redis_fstat fstat64
#define redis_stat stat64
#else
#define redis_fstat fstat
#define redis_stat stat
#endif

/* test for backtrace() */
#if defined(__APPLE__) || defined(__linux__)
#define HAVE_BACKTRACE 1
#endif

#endif

#ifndef __CONFIG_H
#define __CONFIG_H

#ifdef __APPLE__
#include <AvailabilityMacros.h>
#endif

/* use tcmalloc's malloc_size() when available */
#if defined(USE_TCMALLOC)
#include <google/tcmalloc.h>
#if TC_VERSION_MAJOR >= 1 && TC_VERSION_MINOR >= 6
#define HAVE_MALLOC_SIZE 1
#define redis_malloc_size(p) tc_malloc_size(p)
#endif
#endif

/* fallback to native malloc_size() for osx */
#if defined(__APPLE__) && !defined(HAVE_MALLOC_SIZE)
#include <malloc/malloc.h>
#define HAVE_MALLOC_SIZE 1
#define redis_malloc_size(p) malloc_size(p)
#endif

/* define redis_fstat to fstat or fstat64() */
#if defined(__APPLE__) && !defined(MAC_OS_X_VERSION_10_6)
#define redis_fstat fstat64
#define redis_stat stat64
#else
#define redis_fstat fstat
#define redis_stat stat
#endif

/* test for proc filesystem */
#ifdef __linux__
#define HAVE_PROCFS 1
#endif

/* test for task_info() */
#if defined(__APPLE__)
#define HAVE_TASKINFO 1
#endif

/* test for backtrace() */
#if defined(__APPLE__) || defined(__linux__)
#define HAVE_BACKTRACE 1
#endif

/* test for polling API */
#ifdef __linux__
#define HAVE_EPOLL 1
#endif

#if (defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6)) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined (__NetBSD__)
#define HAVE_KQUEUE 1
#endif

/* define aof_fsync to fdatasync() in Linux and fsync() for all the rest */
#ifdef __linux__
#define aof_fsync fdatasync
#else
#define aof_fsync fsync
#endif

#endif

#ifndef __CONFIG_H
#define __CONFIG_H

#ifdef __APPLE__
#include <AvailabilityMacros.h>
#endif

/* Define redis_fstat to fstat or fstat64() */
#if defined(__APPLE__) && !defined(MAC_OS_X_VERSION_10_6)
#define redis_fstat fstat64
#define redis_stat stat64
#else
#define redis_fstat fstat
#define redis_stat stat
#endif

/* Test for proc filesystem */
#ifdef __linux__
#define HAVE_PROCFS 1
#endif

/* Test for task_info() */
#if defined(__APPLE__)
#define HAVE_TASKINFO 1
#endif

/* Test for backtrace() */
#if defined(__APPLE__) || defined(__linux__) || defined(__sun)
#define HAVE_BACKTRACE 1
#endif

/* Test for polling API */
#ifdef __linux__
#define HAVE_EPOLL 1
#endif

#if (defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6)) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined (__NetBSD__)
#define HAVE_KQUEUE 1
#endif

#ifdef __sun
#include <sys/feature_tests.h>
#ifdef _DTRACE_VERSION
#define HAVE_EVPORT 1
#endif
#endif

/* Define aof_fsync to fdatasync() in Linux and fsync() for all the rest */
#ifdef __linux__
#define aof_fsync fdatasync
#else
#define aof_fsync fsync
#endif

/* Define rdb_fsync_range to sync_file_range() on Linux, otherwise we use
 * the plain fsync() call. */
#ifdef __linux__
#define rdb_fsync_range(fd,off,size) sync_file_range(fd,off,size,SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE)
#else
#define rdb_fsync_range(fd,off,size) fsync(fd)
#endif

/* Byte ordering detection */
#include <sys/types.h> /* This will likely define BYTE_ORDER */

#ifndef BYTE_ORDER
#if (BSD >= 199103)
# include <machine/endian.h>
#else
#if defined(linux) || defined(__linux__)
# include <endian.h>
#else
#define	LITTLE_ENDIAN	1234	/* least-significant byte first (vax, pc) */
#define	BIG_ENDIAN	4321	/* most-significant byte first (IBM, net) */
#define	PDP_ENDIAN	3412	/* LSB first in word, MSW first in long (pdp)*/

#if defined(__i386__) || defined(__x86_64__) || defined(__amd64__) || \
   defined(vax) || defined(ns32000) || defined(sun386) || \
   defined(MIPSEL) || defined(_MIPSEL) || defined(BIT_ZERO_ON_RIGHT) || \
   defined(__alpha__) || defined(__alpha)
#define BYTE_ORDER    LITTLE_ENDIAN
#endif

#if defined(sel) || defined(pyr) || defined(mc68000) || defined(sparc) || \
    defined(is68k) || defined(tahoe) || defined(ibm032) || defined(ibm370) || \
    defined(MIPSEB) || defined(_MIPSEB) || defined(_IBMR2) || defined(DGUX) ||\
    defined(apollo) || defined(__convex__) || defined(_CRAY) || \
    defined(__hppa) || defined(__hp9000) || \
    defined(__hp9000s300) || defined(__hp9000s700) || \
    defined (BIT_ZERO_ON_LEFT) || defined(m68k) || defined(__sparc)
#define BYTE_ORDER	BIG_ENDIAN
#endif
#endif /* linux */
#endif /* BSD */
#endif /* BYTE_ORDER */

#if defined(__BYTE_ORDER) && !defined(BYTE_ORDER)
#if (__BYTE_ORDER == __LITTLE_ENDIAN)
#define BYTE_ORDER LITTLE_ENDIAN
#else
#define BYTE_ORDER BIG_ENDIAN
#endif
#endif

#if !defined(BYTE_ORDER) || \
    (BYTE_ORDER != BIG_ENDIAN && BYTE_ORDER != LITTLE_ENDIAN)
	/* you must determine what the correct bit order is for
	 * your compiler - the next line is an intentional error
	 * which will force your compiles to bomb until you fix
	 * the above macros.
	 */
#error "Undefined or invalid BYTE_ORDER"
#endif

#if (__i386 || __amd64) && __GNUC__
#define GNUC_VERSION (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#if GNUC_VERSION >= 40100
#define HAVE_ATOMIC
#endif
#endif


#endif

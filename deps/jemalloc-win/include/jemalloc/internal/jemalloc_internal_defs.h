/* include/jemalloc/internal/jemalloc_internal_defs.h.  Generated from jemalloc_internal_defs.h.in by configure.  */
#ifndef JEMALLOC_INTERNAL_DEFS_H_
#define	JEMALLOC_INTERNAL_DEFS_H_
/*
 * If JEMALLOC_PREFIX is defined via --with-jemalloc-prefix, it will cause all
 * public APIs to be prefixed.  This makes it possible, with some care, to use
 * multiple allocators simultaneously.
 */
/* #undef JEMALLOC_PREFIX */
/* #undef JEMALLOC_CPREFIX */

/*
 * JEMALLOC_PRIVATE_NAMESPACE is used as a prefix for all library-private APIs.
 * For shared libraries, symbol visibility mechanisms prevent these symbols
 * from being exported, but for static libraries, naming collisions are a real
 * possibility.
 */
#define JEMALLOC_PRIVATE_NAMESPACE  je_

/*
 * Hyper-threaded CPUs may need a special instruction inside spin loops in
 * order to yield to another virtual CPU.
 */
#define CPU_SPINWAIT                __asm__ volatile("pause")

/* Defined if the equivalent of FreeBSD's atomic(9) functions are available. */
/* #undef JEMALLOC_ATOMIC9 */

/*
 * Defined if OSAtomic*() functions are available, as provided by Darwin, and
 * documented in the atomic(3) manual page.
 */
/* #undef JEMALLOC_OSATOMIC */

/*
 * Defined if __sync_add_and_fetch(uint32_t *, uint32_t) and
 * __sync_sub_and_fetch(uint32_t *, uint32_t) are available, despite
 * __GCC_HAVE_SYNC_COMPARE_AND_SWAP_4 not being defined (which means the
 * functions are defined in libgcc instead of being inlines)
 */
/* #undef JE_FORCE_SYNC_COMPARE_AND_SWAP_4 */

/*
 * Defined if __sync_add_and_fetch(uint64_t *, uint64_t) and
 * __sync_sub_and_fetch(uint64_t *, uint64_t) are available, despite
 * __GCC_HAVE_SYNC_COMPARE_AND_SWAP_8 not being defined (which means the
 * functions are defined in libgcc instead of being inlines)
 */
/* #undef JE_FORCE_SYNC_COMPARE_AND_SWAP_8 */

/*
 * Defined if OSSpin*() functions are available, as provided by Darwin, and
 * documented in the spinlock(3) manual page.
 */
/* #undef JEMALLOC_OSSPIN */

/*
 * Defined if _malloc_thread_cleanup() exists.  At least in the case of
 * FreeBSD, pthread_key_create() allocates, which if used during malloc
 * bootstrapping will cause recursion into the pthreads library.  Therefore, if
 * _malloc_thread_cleanup() exists, use it as the basis for thread cleanup in
 * malloc_tsd.
 */
/* #undef JEMALLOC_MALLOC_THREAD_CLEANUP */

/*
 * Defined if threaded initialization is known to be safe on this platform.
 * Among other things, it must be possible to initialize a mutex without
 * triggering allocation in order for threaded allocation to be safe.
 */
/* #undef JEMALLOC_THREADED_INIT */

/*
 * Defined if the pthreads implementation defines
 * _pthread_mutex_init_calloc_cb(), in which case the function is used in order
 * to avoid recursive allocation during mutex initialization.
 */
/* #undef JEMALLOC_MUTEX_INIT_CB */

#define JEMALLOC_MANGLE

/* Defined if sbrk() is supported. */
#define JEMALLOC_HAVE_SBRK 

/* Non-empty if the tls_model attribute is supported. */
#define JEMALLOC_TLS_MODEL 

/* JEMALLOC_CC_SILENCE enables code that silences unuseful compiler warnings. */
/* #undef JEMALLOC_CC_SILENCE */

/* JEMALLOC_CODE_COVERAGE enables test code coverage analysis. */
/* #undef JEMALLOC_CODE_COVERAGE */

/*
 * JEMALLOC_DEBUG enables assertions and other sanity checks, and disables
 * inline functions.
 */
/* #undef JEMALLOC_DEBUG */

/* JEMALLOC_STATS enables statistics calculation. */
#define JEMALLOC_STATS 

/* JEMALLOC_PROF enables allocation profiling. */
/* #undef JEMALLOC_PROF */

/* Use libunwind for profile backtracing if defined. */
/* #undef JEMALLOC_PROF_LIBUNWIND */

/* Use libgcc for profile backtracing if defined. */
/* #undef JEMALLOC_PROF_LIBGCC */

/* Use gcc intrinsics for profile backtracing if defined. */
/* #undef JEMALLOC_PROF_GCC */

/*
 * JEMALLOC_TCACHE enables a thread-specific caching layer for small objects.
 * This makes it possible to allocate/deallocate objects without any locking
 * when the cache is in the steady state.
 */
//#define JEMALLOC_TCACHE 

/*
 * JEMALLOC_DSS enables use of sbrk(2) to allocate chunks from the data storage
 * segment (DSS).
 */
/* #undef JEMALLOC_DSS */

/* Support memory filling (junk/zero/quarantine/redzone). */
#define JEMALLOC_FILL 

/* Support utrace(2)-based tracing. */
/* #undef JEMALLOC_UTRACE */

/* Support Valgrind. */
/* #undef JEMALLOC_VALGRIND */

/* Support optional abort() on OOM. */
/* #undef JEMALLOC_XMALLOC */

/* Support lazy locking (avoid locking unless a second thread is launched). */
/* #undef JEMALLOC_LAZY_LOCK */

/* One page is 2^STATIC_PAGE_SHIFT bytes. */
#ifdef _WIN32
#define STATIC_PAGE_SHIFT       12
#else
#define STATIC_PAGE_SHIFT       16
#endif

/*
 * If defined, use munmap() to unmap freed chunks, rather than storing them for
 * later reuse.  This is disabled by default on Linux because common sequences
 * of mmap()/munmap() calls will cause virtual memory map holes.
 */
#define JEMALLOC_MUNMAP

/*
 * If defined, use mremap(...MREMAP_FIXED...) for huge realloc().  This is
 * disabled by default because it is Linux-specific and it will cause virtual
 * memory map holes, much like munmap(2) does.
 */
/* #undef JEMALLOC_MREMAP */

/* TLS is used to map arenas and magazine caches to threads. */
#if !defined(_MSC_VER)
#define JEMALLOC_TLS
#endif

#if defined(_MSC_VER) && (_MSC_VER >= 1500)
#ifndef	__thread
//#define __thread            __declspec(thread)
#endif
#endif  /* _MSC_VER */

/*
 * JEMALLOC_IVSALLOC enables ivsalloc(), which verifies that pointers reside
 * within jemalloc-owned chunks before dereferencing them.
 */
/* #undef JEMALLOC_IVSALLOC */

/*
 * Darwin (OS X) uses zones to work around Mach-O symbol override shortcomings.
 */
/* #undef JEMALLOC_ZONE */
/* #undef JEMALLOC_ZONE_VERSION */

/*
 * Methods for purging unused pages differ between operating systems.
 *
 *   madvise(..., MADV_DONTNEED) : On Linux, this immediately discards pages,
 *                                 such that new pages will be demand-zeroed if
 *                                 the address region is later touched.
 *   madvise(..., MADV_FREE) : On FreeBSD and Darwin, this marks pages as being
 *                             unused, such that they will be discarded rather
 *                             than swapped out.
 */
/* #undef JEMALLOC_PURGE_MADVISE_DONTNEED */
/* #undef JEMALLOC_PURGE_MADVISE_FREE */

#define JEMALLOC_PURGE_MADVISE_DONTNEED

/*
 * Define if operating system has alloca.h header.
 */
/* #undef JEMALLOC_HAS_ALLOCA_H */

/* C99 restrict keyword supported. */
#if (defined(_MSC_VER) && (_MSC_VER >= 1500)) || !defined(_MSC_VER)
#define JEMALLOC_HAS_RESTRICT   1
#endif

/* For use by hash code. */
/* #undef JEMALLOC_BIG_ENDIAN */

/* sizeof(int) == 2^LG_SIZEOF_INT. */
#define LG_SIZEOF_INT       2

#if defined(_WIN32) || defined(_M_IX86)

/* sizeof(long) == 2^LG_SIZEOF_LONG. */
#define LG_SIZEOF_LONG      2

/* sizeof(intmax_t) == 2^LG_SIZEOF_INTMAX_T. */
#define LG_SIZEOF_INTMAX_T  2

#elif defined(_WIN64) || defined(_M_X64)

/* sizeof(long) == 2^LG_SIZEOF_LONG. */
#define LG_SIZEOF_LONG      3

/* sizeof(intmax_t) == 2^LG_SIZEOF_INTMAX_T. */
#define LG_SIZEOF_INTMAX_T  3

#else

/* sizeof(long) == 2^LG_SIZEOF_LONG. */
#define LG_SIZEOF_LONG      2

/* sizeof(intmax_t) == 2^LG_SIZEOF_INTMAX_T. */
#define LG_SIZEOF_INTMAX_T  2

#endif  /* _WIN32 || _M_IX86 */

#endif  /* JEMALLOC_INTERNAL_DEFS_H_ */

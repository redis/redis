/* include/jemalloc/jemalloc_defs.h.  Generated from jemalloc_defs.h.in by configure.  */
/* Defined if __attribute__((...)) syntax is supported. */
#ifndef _MSC_VER
#define JEMALLOC_HAVE_ATTR
#endif

/* Support the experimental API. */
#define JEMALLOC_EXPERIMENTAL

/*
 * Define overrides for non-standard allocator-related functions if they are
 * present on the system.
 */
#define JEMALLOC_OVERRIDE_MEMALIGN
#define JEMALLOC_OVERRIDE_VALLOC

/*
 * At least Linux omits the "const" in:
 *
 *   size_t malloc_usable_size(const void *ptr);
 *
 * Match the operating system's prototype.
 */
#define JEMALLOC_USABLE_SIZE_CONST const

/* sizeof(void *) == 2^LG_SIZEOF_PTR. */
#if defined(_WIN32) || defined(_M_IX86)
#define LG_SIZEOF_PTR       2
#elif defined(_WIN64) || defined(_M_X64)
#define LG_SIZEOF_PTR       3
#else
#define LG_SIZEOF_PTR       3
#endif

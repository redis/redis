/*
 * Name mangling for public symbols is controlled by --with-mangling and
 * --with-jemalloc-prefix.  With default settings the je_ prefix is stripped by
 * these macro definitions.
 */
#ifndef JEMALLOC_NO_RENAME
#  define je_malloc_conf malloc_conf
#  define je_malloc_message malloc_message
#  define je_malloc malloc
#  define je_calloc calloc
#  define je_posix_memalign posix_memalign
#  define je_aligned_alloc aligned_alloc
#  define je_realloc realloc
#  define je_free free
#  define je_mallocx mallocx
#  define je_rallocx rallocx
#  define je_xallocx xallocx
#  define je_sallocx sallocx
#  define je_dallocx dallocx
#  define je_nallocx nallocx
#  define je_mallctl mallctl
#  define je_mallctlnametomib mallctlnametomib
#  define je_mallctlbymib mallctlbymib
#  define je_malloc_stats_print malloc_stats_print
#  define je_malloc_usable_size malloc_usable_size
#  define je_memalign memalign
#  define je_valloc valloc
#  define je_allocm allocm
#  define je_dallocm dallocm
#  define je_nallocm nallocm
#  define je_rallocm rallocm
#  define je_sallocm sallocm
#endif

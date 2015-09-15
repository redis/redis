/*
 * By default application code must explicitly refer to mangled symbol names,
 * so that it is possible to use jemalloc in conjunction with another allocator
 * in the same application.  Define JEMALLOC_MANGLE in order to cause automatic
 * name mangling that matches the API prefixing that happened as a result of
 * --with-mangling and/or --with-jemalloc-prefix configuration settings.
 */
#ifdef JEMALLOC_MANGLE
#  ifndef JEMALLOC_NO_DEMANGLE
#    define JEMALLOC_NO_DEMANGLE
#  endif
#  define malloc_conf jet_malloc_conf
#  define malloc_message jet_malloc_message
#  define malloc jet_malloc
#  define calloc jet_calloc
#  define posix_memalign jet_posix_memalign
#  define aligned_alloc jet_aligned_alloc
#  define realloc jet_realloc
#  define free jet_free
#  define mallocx jet_mallocx
#  define rallocx jet_rallocx
#  define xallocx jet_xallocx
#  define sallocx jet_sallocx
#  define dallocx jet_dallocx
#  define nallocx jet_nallocx
#  define mallctl jet_mallctl
#  define mallctlnametomib jet_mallctlnametomib
#  define mallctlbymib jet_mallctlbymib
#  define malloc_stats_print jet_malloc_stats_print
#  define malloc_usable_size jet_malloc_usable_size
#  define memalign jet_memalign
#  define valloc jet_valloc
#  define allocm jet_allocm
#  define dallocm jet_dallocm
#  define nallocm jet_nallocm
#  define rallocm jet_rallocm
#  define sallocm jet_sallocm
#endif

/*
 * The jet_* macros can be used as stable alternative names for the
 * public jemalloc API if JEMALLOC_NO_DEMANGLE is defined.  This is primarily
 * meant for use in jemalloc itself, but it can be used by application code to
 * provide isolation from the name mangling specified via --with-mangling
 * and/or --with-jemalloc-prefix.
 */
#ifndef JEMALLOC_NO_DEMANGLE
#  undef jet_malloc_conf
#  undef jet_malloc_message
#  undef jet_malloc
#  undef jet_calloc
#  undef jet_posix_memalign
#  undef jet_aligned_alloc
#  undef jet_realloc
#  undef jet_free
#  undef jet_mallocx
#  undef jet_rallocx
#  undef jet_xallocx
#  undef jet_sallocx
#  undef jet_dallocx
#  undef jet_nallocx
#  undef jet_mallctl
#  undef jet_mallctlnametomib
#  undef jet_mallctlbymib
#  undef jet_malloc_stats_print
#  undef jet_malloc_usable_size
#  undef jet_memalign
#  undef jet_valloc
#  undef jet_allocm
#  undef jet_dallocm
#  undef jet_nallocm
#  undef jet_rallocm
#  undef jet_sallocm
#endif

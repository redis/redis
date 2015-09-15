/*
 * The jet_ prefix on the following public symbol declarations is an artifact
 * of namespace management, and should be omitted in application code unless
 * JEMALLOC_NO_DEMANGLE is defined (see jemalloc_mangle@install_suffix@.h).
 */
extern JEMALLOC_EXPORT const char	*je_malloc_conf;
extern JEMALLOC_EXPORT void		    (*je_malloc_message)(void *cbopaque,
                                                         const char *s);
extern JEMALLOC_EXPORT void         je_init(void);
extern JEMALLOC_EXPORT void         je_uninit(void);

JEMALLOC_EXPORT void	*jet_malloc(size_t size) JEMALLOC_ATTR(malloc);
JEMALLOC_EXPORT void	*jet_calloc(size_t num, size_t size)
    JEMALLOC_ATTR(malloc);
JEMALLOC_EXPORT int	jet_posix_memalign(void **memptr, size_t alignment,
    size_t size) JEMALLOC_ATTR(nonnull(1));
JEMALLOC_EXPORT void	*jet_aligned_alloc(size_t alignment, size_t size)
    JEMALLOC_ATTR(malloc);
JEMALLOC_EXPORT void	*jet_realloc(void *ptr, size_t size);
JEMALLOC_EXPORT void	jet_free(void *ptr);

JEMALLOC_EXPORT void	*jet_mallocx(size_t size, int flags);
JEMALLOC_EXPORT void	*jet_rallocx(void *ptr, size_t size, int flags);
JEMALLOC_EXPORT size_t	jet_xallocx(void *ptr, size_t size, size_t extra,
    int flags);
JEMALLOC_EXPORT size_t	jet_sallocx(const void *ptr, int flags);
JEMALLOC_EXPORT void	jet_dallocx(void *ptr, int flags);
JEMALLOC_EXPORT size_t	jet_nallocx(size_t size, int flags);

JEMALLOC_EXPORT int	jet_mallctl(const char *name, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen);
JEMALLOC_EXPORT int	jet_mallctlnametomib(const char *name, size_t *mibp,
    size_t *miblenp);
JEMALLOC_EXPORT int	jet_mallctlbymib(const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen);
JEMALLOC_EXPORT void	jet_malloc_stats_print(void (*write_cb)(void *,
    const char *), void *jet_cbopaque, const char *opts);
JEMALLOC_EXPORT size_t	jet_malloc_usable_size(
    JEMALLOC_USABLE_SIZE_CONST void *ptr);

#ifdef JEMALLOC_OVERRIDE_MEMALIGN
JEMALLOC_EXPORT void *	jet_memalign(size_t alignment, size_t size)
    JEMALLOC_ATTR(malloc);
#endif

#ifdef JEMALLOC_OVERRIDE_VALLOC
JEMALLOC_EXPORT void *	jet_valloc(size_t size) JEMALLOC_ATTR(malloc);
#endif

#ifdef JEMALLOC_EXPERIMENTAL
JEMALLOC_EXPORT int	jet_allocm(void **ptr, size_t *rsize, size_t size,
    int flags) JEMALLOC_ATTR(nonnull(1));
JEMALLOC_EXPORT int	jet_rallocm(void **ptr, size_t *rsize, size_t size,
    size_t extra, int flags) JEMALLOC_ATTR(nonnull(1));
JEMALLOC_EXPORT int	jet_sallocm(const void *ptr, size_t *rsize, int flags)
    JEMALLOC_ATTR(nonnull(1));
JEMALLOC_EXPORT int	jet_dallocm(void *ptr, int flags)
    JEMALLOC_ATTR(nonnull(1));
JEMALLOC_EXPORT int	jet_nallocm(size_t *rsize, size_t size, int flags);
#endif

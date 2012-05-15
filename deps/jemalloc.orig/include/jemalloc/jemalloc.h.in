#ifndef JEMALLOC_H_
#define	JEMALLOC_H_
#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <strings.h>

#define	JEMALLOC_VERSION "@jemalloc_version@"
#define	JEMALLOC_VERSION_MAJOR @jemalloc_version_major@
#define	JEMALLOC_VERSION_MINOR @jemalloc_version_minor@
#define	JEMALLOC_VERSION_BUGFIX @jemalloc_version_bugfix@
#define	JEMALLOC_VERSION_NREV @jemalloc_version_nrev@
#define	JEMALLOC_VERSION_GID "@jemalloc_version_gid@"

#include "jemalloc_defs@install_suffix@.h"
#ifndef JEMALLOC_P
#  define JEMALLOC_P(s) s
#endif

#define	ALLOCM_LG_ALIGN(la)	(la)
#if LG_SIZEOF_PTR == 2
#define	ALLOCM_ALIGN(a)	(ffs(a)-1)
#else
#define	ALLOCM_ALIGN(a)	((a < (size_t)INT_MAX) ? ffs(a)-1 : ffs(a>>32)+31)
#endif
#define	ALLOCM_ZERO	((int)0x40)
#define	ALLOCM_NO_MOVE	((int)0x80)

#define	ALLOCM_SUCCESS		0
#define	ALLOCM_ERR_OOM		1
#define	ALLOCM_ERR_NOT_MOVED	2

extern const char	*JEMALLOC_P(malloc_conf);
extern void		(*JEMALLOC_P(malloc_message))(void *, const char *);

void	*JEMALLOC_P(malloc)(size_t size) JEMALLOC_ATTR(malloc);
void	*JEMALLOC_P(calloc)(size_t num, size_t size) JEMALLOC_ATTR(malloc);
int	JEMALLOC_P(posix_memalign)(void **memptr, size_t alignment, size_t size)
    JEMALLOC_ATTR(nonnull(1));
void	*JEMALLOC_P(realloc)(void *ptr, size_t size);
void	JEMALLOC_P(free)(void *ptr);

size_t	JEMALLOC_P(malloc_usable_size)(const void *ptr);
void	JEMALLOC_P(malloc_stats_print)(void (*write_cb)(void *, const char *),
    void *cbopaque, const char *opts);
int	JEMALLOC_P(mallctl)(const char *name, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen);
int	JEMALLOC_P(mallctlnametomib)(const char *name, size_t *mibp,
    size_t *miblenp);
int	JEMALLOC_P(mallctlbymib)(const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen);

int	JEMALLOC_P(allocm)(void **ptr, size_t *rsize, size_t size, int flags)
    JEMALLOC_ATTR(nonnull(1));
int	JEMALLOC_P(rallocm)(void **ptr, size_t *rsize, size_t size,
    size_t extra, int flags) JEMALLOC_ATTR(nonnull(1));
int	JEMALLOC_P(sallocm)(const void *ptr, size_t *rsize, int flags)
    JEMALLOC_ATTR(nonnull(1));
int	JEMALLOC_P(dallocm)(void *ptr, int flags) JEMALLOC_ATTR(nonnull(1));

#ifdef __cplusplus
};
#endif
#endif /* JEMALLOC_H_ */

#ifndef	JEMALLOC_H_
#define	JEMALLOC_H_

/* Defined if __attribute__((...)) syntax is supported.	*/
#ifndef	_MSC_VER
#define	JEMALLOC_HAVE_ATTR
#endif

/* Support the experimental	API. */
//#define	JEMALLOC_EXPERIMENTAL

/*
 * Define overrides	for	non-standard allocator-related functions if	they are
 * present on the system.
 */
//#define	JEMALLOC_OVERRIDE_MEMALIGN
//#define	JEMALLOC_OVERRIDE_VALLOC

/*
 * At least	Linux omits	the	"const"	in:
 *
 *	 size_t	malloc_usable_size(const void *ptr);
 *
 * Match the operating system's	prototype.
 */
#define	JEMALLOC_USABLE_SIZE_CONST const

/* sizeof(void *) == 2^LG_SIZEOF_PTR. */
#if	defined(_WIN32)	|| defined(_M_IX86)
#define	LG_SIZEOF_PTR		2
#elif defined(_WIN64) || defined(_M_X64)
#define	LG_SIZEOF_PTR		3
#else
#define	LG_SIZEOF_PTR		3
#endif

/*
 * Name	mangling for public	symbols	is controlled by --with-mangling and
 * --with-jemalloc-prefix.	With default settings the je_ prefix is	stripped by
 * these macro definitions.
 */
#ifndef	JEMALLOC_NO_RENAME
#define	JEMALLOC_NO_RENAME
#endif

#ifndef	JEMALLOC_NO_RENAME
#  define je_malloc_conf malloc_conf
#  define je_malloc_message	malloc_message
#  define je_malloc	malloc
#  define je_calloc	calloc
#  define je_posix_memalign	posix_memalign
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
#  define je_malloc_stats_print	malloc_stats_print
#  define je_malloc_usable_size	malloc_usable_size
#  define je_memalign memalign
#  define je_valloc	valloc
#  define je_allocm	allocm
#  define je_dallocm dallocm
#  define je_nallocm nallocm
#  define je_rallocm rallocm
#  define je_sallocm sallocm
#endif

#include <limits.h>

#define __STDC_FORMAT_MACROS    // For PRIXPTR in <inttypes.h>

#ifndef	_MSC_VER
#include <stdint.h>
#include <inttypes.h>
#include <strings.h>
#else
#include "msvc_compat/stdint.h"
#include "msvc_compat/inttypes.h"
#include "msvc_compat/strings.h"
#endif	// _MSC_VER

#ifdef __cplusplus
extern "C" {
#endif

#define	JEMALLOC_VERSION			"3.6.0-0-g46c0af68bd248b04df75e4f92d5fb804c3d75340"
#define	JEMALLOC_VERSION_MAJOR		3
#define	JEMALLOC_VERSION_MINOR		6
#define	JEMALLOC_VERSION_BUGFIX		0
#define	JEMALLOC_VERSION_NREV		0
#define	JEMALLOC_VERSION_GID		"46c0af68bd248b04df75e4f92d5fb804c3d75340"

#  define MALLOCX_LG_ALIGN(la)		(la)
#  if LG_SIZEOF_PTR	== 2
#	 define	MALLOCX_ALIGN(a)		(ffs(a)-1)
#  else
#	 define	MALLOCX_ALIGN(a)						\
	 ((a < (size_t)INT_MAX)	? ffs(a)-1 : ffs(a>>32)+31)
#  endif
#  define MALLOCX_ZERO				((int)0x40)
/* Bias	arena index	bits so	that 0 encodes "MALLOCX_ARENA()	unspecified". */
#  define MALLOCX_ARENA(a)			((int)(((a)+1) << 8))

#ifdef JEMALLOC_EXPERIMENTAL
#  define ALLOCM_LG_ALIGN(la)		(la)
#  if LG_SIZEOF_PTR	== 2
#	 define	ALLOCM_ALIGN(a)			(ffs(a)-1)
#  else
#	 define	ALLOCM_ALIGN(a)						\
	 ((a < (size_t)INT_MAX)	? ffs(a)-1 : ffs(a>>32)+31)
#  endif
#  define ALLOCM_ZERO				((int)0x40)
#  define ALLOCM_NO_MOVE			((int)0x80)
/* Bias	arena index	bits so	that 0 encodes "ALLOCM_ARENA() unspecified". */
#  define ALLOCM_ARENA(a)			((int)(((a)+1) << 8))
#  define ALLOCM_SUCCESS			0
#  define ALLOCM_ERR_OOM			1
#  define ALLOCM_ERR_NOT_MOVED		2
#endif

#ifdef JEMALLOC_HAVE_ATTR
#  define JEMALLOC_ATTR(s)			__attribute__((s))
#  define JEMALLOC_EXPORT			JEMALLOC_ATTR(visibility("default"))
#  define JEMALLOC_ALIGNED(s)		JEMALLOC_ATTR(aligned(s))
#  define JEMALLOC_SECTION(s)		JEMALLOC_ATTR(section(s))
#  define JEMALLOC_NOINLINE			JEMALLOC_ATTR(noinline)
#elif _MSC_VER
#  define JEMALLOC_ATTR(s)
#  if defined(USE_STATIC) || defined(STATIC_ENABLE)
#	 define	JEMALLOC_ATTR(s)
#	 define	JEMALLOC_EXPORT
#	 define	JEMALLOC_ALIGNED(s)
#	 define	JEMALLOC_SECTION(s)
#	 define	JEMALLOC_NOINLINE
#  else
#	 if	defined(DLLEXPORT) || defined(IS_DLL) || defined(DLL_EXPORT)
#	   define JEMALLOC_EXPORT		__declspec(dllexport)
#	 else
#	   define JEMALLOC_EXPORT		__declspec(dllimport)
#	 endif	/* DLLEXPORT */
#	 define	JEMALLOC_ALIGNED(s)		__declspec(align(s))
#	 define	JEMALLOC_SECTION(s)		__declspec(allocate(s))
#	 define	JEMALLOC_NOINLINE		__declspec(noinline)
#  endif  /* USE_STATIC	*/
#else
#  define JEMALLOC_ATTR(s)
#  define JEMALLOC_EXPORT
#  define JEMALLOC_ALIGNED(s)
#  define JEMALLOC_SECTION(s)
#  define JEMALLOC_NOINLINE
#endif

#ifndef	JEMALLOC_HAS_RESTRICT
#  define __RESTRICT
#else  /* !JEMALLOC_HAS_RESTRICT */
#  ifndef __RESTRICT
#	 define	__RESTRICT		__restrict
#  endif  /* __RESTRICT	*/
#endif	/* JEMALLOC_HAS_RESTRICT */

#if defined(_MSC_VER) && (_MSC_VER >= 1500)
#ifndef	__thread
//#define __thread            __declspec(thread)
#endif
#endif  /* _MSC_VER */

/*
 * The je_ prefix on the following public symbol declarations is an	artifact
 * of namespace	management,	and	should be omitted in application code unless
 * JEMALLOC_NO_DEMANGLE	is defined (see	jemalloc_mangle.h).
 */
extern JEMALLOC_EXPORT const char	*je_malloc_conf;
extern JEMALLOC_EXPORT void			(*je_malloc_message)(void *cbopaque,
														 const char	*s);
extern JEMALLOC_EXPORT void			je_init(void);
extern JEMALLOC_EXPORT void			je_uninit(void);

JEMALLOC_EXPORT	void	*je_malloc(size_t size)	JEMALLOC_ATTR(malloc);
JEMALLOC_EXPORT	void	*je_calloc(size_t num, size_t size)
	JEMALLOC_ATTR(malloc);
JEMALLOC_EXPORT	int	je_posix_memalign(void **memptr, size_t	alignment,
	size_t size) JEMALLOC_ATTR(nonnull(1));
JEMALLOC_EXPORT	void	*je_aligned_alloc(size_t alignment,	size_t size)
	JEMALLOC_ATTR(malloc);
JEMALLOC_EXPORT	void	*je_realloc(void *ptr, size_t size);
JEMALLOC_EXPORT	void	je_free(void *ptr);

JEMALLOC_EXPORT	void	*je_mallocx(size_t size, int flags);
JEMALLOC_EXPORT	void	*je_rallocx(void *ptr, size_t size,	int	flags);
JEMALLOC_EXPORT	size_t	je_xallocx(void	*ptr, size_t size, size_t extra,
	int	flags);
JEMALLOC_EXPORT	size_t	je_sallocx(const void *ptr,	int	flags);
JEMALLOC_EXPORT	void	je_dallocx(void	*ptr, int flags);
JEMALLOC_EXPORT	size_t	je_nallocx(size_t size,	int	flags);

JEMALLOC_EXPORT	int	je_mallctl(const char *name, void *oldp,
	size_t *oldlenp, void *newp, size_t	newlen);
JEMALLOC_EXPORT	int	je_mallctlnametomib(const char *name, size_t *mibp,
	size_t *miblenp);
JEMALLOC_EXPORT	int	je_mallctlbymib(const size_t *mib, size_t miblen,
	void *oldp,	size_t *oldlenp, void *newp, size_t	newlen);
JEMALLOC_EXPORT	void	je_malloc_stats_print(void (*write_cb)(void	*,
	const char *), void	*je_cbopaque, const	char *opts);
JEMALLOC_EXPORT	size_t	je_malloc_usable_size(
	JEMALLOC_USABLE_SIZE_CONST void	*ptr);

#ifdef JEMALLOC_OVERRIDE_MEMALIGN
JEMALLOC_EXPORT	void *	je_memalign(size_t alignment, size_t size)
	JEMALLOC_ATTR(malloc);
#endif

#ifdef JEMALLOC_OVERRIDE_VALLOC
JEMALLOC_EXPORT	void *	je_valloc(size_t size) JEMALLOC_ATTR(malloc);
#endif

#ifdef JEMALLOC_EXPERIMENTAL
JEMALLOC_EXPORT	int	je_allocm(void **ptr, size_t *rsize, size_t	size,
	int	flags) JEMALLOC_ATTR(nonnull(1));
JEMALLOC_EXPORT	int	je_rallocm(void	**ptr, size_t *rsize, size_t size,
	size_t extra, int flags) JEMALLOC_ATTR(nonnull(1));
JEMALLOC_EXPORT	int	je_sallocm(const void *ptr,	size_t *rsize, int flags)
	JEMALLOC_ATTR(nonnull(1));
JEMALLOC_EXPORT	int	je_dallocm(void	*ptr, int flags)
	JEMALLOC_ATTR(nonnull(1));
JEMALLOC_EXPORT	int	je_nallocm(size_t *rsize, size_t size, int flags);
#endif

/*
 * By default application code must	explicitly refer to	mangled	symbol names,
 * so that it is possible to use jemalloc in conjunction with another allocator
 * in the same application.	 Define	JEMALLOC_MANGLE	in order to	cause automatic
 * name	mangling that matches the API prefixing	that happened as a result of
 * --with-mangling and/or --with-jemalloc-prefix configuration settings.
 */
#ifdef JEMALLOC_MANGLE
#  ifndef JEMALLOC_NO_DEMANGLE
#	 define	JEMALLOC_NO_DEMANGLE
#  endif
#  define malloc_conf je_malloc_conf
#  define malloc_message je_malloc_message
#  define malloc je_malloc
#  define calloc je_calloc
#  define posix_memalign je_posix_memalign
#  define aligned_alloc	je_aligned_alloc
#  define realloc je_realloc
#  define free je_free
#  define mallocx je_mallocx
#  define rallocx je_rallocx
#  define xallocx je_xallocx
#  define sallocx je_sallocx
#  define dallocx je_dallocx
#  define nallocx je_nallocx
#  define mallctl je_mallctl
#  define mallctlnametomib je_mallctlnametomib
#  define mallctlbymib je_mallctlbymib
#  define malloc_stats_print je_malloc_stats_print
#  define malloc_usable_size je_malloc_usable_size
#  define memalign je_memalign
#  define valloc je_valloc
#  define allocm je_allocm
#  define dallocm je_dallocm
#  define nallocm je_nallocm
#  define rallocm je_rallocm
#  define sallocm je_sallocm
#endif

/*
 * The je_*	macros can be used as stable alternative names for the
 * public jemalloc API if JEMALLOC_NO_DEMANGLE is defined.	This is	primarily
 * meant for use in	jemalloc itself, but it	can	be used	by application code	to
 * provide isolation from the name mangling	specified via --with-mangling
 * and/or --with-jemalloc-prefix.
 */
#ifndef	JEMALLOC_NO_DEMANGLE
#  undef je_malloc_conf
#  undef je_malloc_message
#  undef je_malloc
#  undef je_calloc
#  undef je_posix_memalign
#  undef je_aligned_alloc
#  undef je_realloc
#  undef je_free
#  undef je_mallocx
#  undef je_rallocx
#  undef je_xallocx
#  undef je_sallocx
#  undef je_dallocx
#  undef je_nallocx
#  undef je_mallctl
#  undef je_mallctlnametomib
#  undef je_mallctlbymib
#  undef je_malloc_stats_print
#  undef je_malloc_usable_size
#  undef je_memalign
#  undef je_valloc
#  undef je_allocm
#  undef je_dallocm
#  undef je_nallocm
#  undef je_rallocm
#  undef je_sallocm
#endif

#ifdef __cplusplus
}
#endif

#endif /* JEMALLOC_H_ */

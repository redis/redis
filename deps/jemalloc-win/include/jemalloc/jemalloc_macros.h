#include <limits.h>

#ifndef	_MSC_VER
#include <strings.h>
#else
#include "msvc_compat/strings.h"
#endif	// _WIN32

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

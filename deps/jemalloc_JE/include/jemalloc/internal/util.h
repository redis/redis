/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#ifdef _WIN32
#  ifdef _WIN64
#    define FMT64_PREFIX "ll"
#    define FMTPTR_PREFIX "ll"
#  else
#    define FMT64_PREFIX "ll"
#    define FMTPTR_PREFIX ""
#  endif
#  define FMTd32 "d"
#  define FMTu32 "u"
#  define FMTx32 "x"
#  define FMTd64 FMT64_PREFIX "d"
#  define FMTu64 FMT64_PREFIX "u"
#  define FMTx64 FMT64_PREFIX "x"
#  define FMTdPTR FMTPTR_PREFIX "d"
#  define FMTuPTR FMTPTR_PREFIX "u"
#  define FMTxPTR FMTPTR_PREFIX "x"
#else
#  include <inttypes.h>
#  define FMTd32 PRId32
#  define FMTu32 PRIu32
#  define FMTx32 PRIx32
#  define FMTd64 PRId64
#  define FMTu64 PRIu64
#  define FMTx64 PRIx64
#  define FMTdPTR PRIdPTR
#  define FMTuPTR PRIuPTR
#  define FMTxPTR PRIxPTR
#endif

/* Size of stack-allocated buffer passed to buferror(). */
#define	BUFERROR_BUF		64

/*
 * Size of stack-allocated buffer used by malloc_{,v,vc}printf().  This must be
 * large enough for all possible uses within jemalloc.
 */
#define	MALLOC_PRINTF_BUFSIZE	4096

/*
 * Wrap a cpp argument that contains commas such that it isn't broken up into
 * multiple arguments.
 */
#define	JEMALLOC_ARG_CONCAT(...) __VA_ARGS__

/*
 * Silence compiler warnings due to uninitialized values.  This is used
 * wherever the compiler fails to recognize that the variable is never used
 * uninitialized.
 */
#ifdef JEMALLOC_CC_SILENCE
#	define JEMALLOC_CC_SILENCE_INIT(v) = v
#else
#	define JEMALLOC_CC_SILENCE_INIT(v)
#endif

#define	JEMALLOC_GNUC_PREREQ(major, minor)				\
    (!defined(__clang__) &&						\
    (__GNUC__ > (major) || (__GNUC__ == (major) && __GNUC_MINOR__ >= (minor))))
#ifndef __has_builtin
#  define __has_builtin(builtin) (0)
#endif
#define	JEMALLOC_CLANG_HAS_BUILTIN(builtin)				\
    (defined(__clang__) && __has_builtin(builtin))

#ifdef __GNUC__
#	define likely(x)   __builtin_expect(!!(x), 1)
#	define unlikely(x) __builtin_expect(!!(x), 0)
#  if JEMALLOC_GNUC_PREREQ(4, 6) ||					\
      JEMALLOC_CLANG_HAS_BUILTIN(__builtin_unreachable)
#	define unreachable() __builtin_unreachable()
#  else
#	define unreachable()
#  endif
#else
#	define likely(x)   !!(x)
#	define unlikely(x) !!(x)
#	define unreachable()
#endif

/*
 * Define a custom assert() in order to reduce the chances of deadlock during
 * assertion failure.
 */
#ifndef assert
#define	assert(e) do {							\
	if (unlikely(config_debug && !(e))) {				\
		malloc_printf(						\
		    "<jemalloc>: %s:%d: Failed assertion: \"%s\"\n",	\
		    __FILE__, __LINE__, #e);				\
		abort();						\
	}								\
} while (0)
#endif

#ifndef not_reached
#define	not_reached() do {						\
	if (config_debug) {						\
		malloc_printf(						\
		    "<jemalloc>: %s:%d: Unreachable code reached\n",	\
		    __FILE__, __LINE__);				\
		abort();						\
	}								\
	unreachable();							\
} while (0)
#endif

#ifndef not_implemented
#define	not_implemented() do {						\
	if (config_debug) {						\
		malloc_printf("<jemalloc>: %s:%d: Not implemented\n",	\
		    __FILE__, __LINE__);				\
		abort();						\
	}								\
} while (0)
#endif

#ifndef assert_not_implemented
#define	assert_not_implemented(e) do {					\
	if (unlikely(config_debug && !(e)))				\
		not_implemented();					\
} while (0)
#endif

/* Use to assert a particular configuration, e.g., cassert(config_debug). */
#define	cassert(c) do {							\
	if (unlikely(!(c)))						\
		not_reached();						\
} while (0)

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

int	buferror(int err, char *buf, size_t buflen);
uintmax_t	malloc_strtoumax(const char *restrict nptr,
    char **restrict endptr, int base);
void	malloc_write(const char *s);

/*
 * malloc_vsnprintf() supports a subset of snprintf(3) that avoids floating
 * point math.
 */
int	malloc_vsnprintf(char *str, size_t size, const char *format,
    va_list ap);
int	malloc_snprintf(char *str, size_t size, const char *format, ...)
    JEMALLOC_FORMAT_PRINTF(3, 4);
void	malloc_vcprintf(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *format, va_list ap);
void malloc_cprintf(void (*write)(void *, const char *), void *cbopaque,
    const char *format, ...) JEMALLOC_FORMAT_PRINTF(3, 4);
void	malloc_printf(const char *format, ...) JEMALLOC_FORMAT_PRINTF(1, 2);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
int	jemalloc_ffsl(long bitmap);
int	jemalloc_ffs(int bitmap);
size_t	pow2_ceil(size_t x);
size_t	lg_floor(size_t x);
void	set_errno(int errnum);
int	get_errno(void);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_UTIL_C_))

/* Sanity check. */
#if !defined(JEMALLOC_INTERNAL_FFSL) || !defined(JEMALLOC_INTERNAL_FFS)
#  error Both JEMALLOC_INTERNAL_FFSL && JEMALLOC_INTERNAL_FFS should have been defined by configure
#endif

JEMALLOC_ALWAYS_INLINE int
jemalloc_ffsl(long bitmap)
{

	return (JEMALLOC_INTERNAL_FFSL(bitmap));
}

JEMALLOC_ALWAYS_INLINE int
jemalloc_ffs(int bitmap)
{

	return (JEMALLOC_INTERNAL_FFS(bitmap));
}

/* Compute the smallest power of 2 that is >= x. */
JEMALLOC_INLINE size_t
pow2_ceil(size_t x)
{

	x--;
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
#if (LG_SIZEOF_PTR == 3)
	x |= x >> 32;
#endif
	x++;
	return (x);
}

#if (defined(__i386__) || defined(__amd64__) || defined(__x86_64__))
JEMALLOC_INLINE size_t
lg_floor(size_t x)
{
	size_t ret;

	assert(x != 0);

	asm ("bsr %1, %0"
	    : "=r"(ret) // Outputs.
	    : "r"(x)    // Inputs.
	    );
	return (ret);
}
#elif (defined(_MSC_VER))
JEMALLOC_INLINE size_t
lg_floor(size_t x)
{
	unsigned long ret;

	assert(x != 0);

#if (LG_SIZEOF_PTR == 3)
	_BitScanReverse64(&ret, x);
#elif (LG_SIZEOF_PTR == 2)
	_BitScanReverse(&ret, x);
#else
#  error "Unsupported type sizes for lg_floor()"
#endif
	return (ret);
}
#elif (defined(JEMALLOC_HAVE_BUILTIN_CLZ))
JEMALLOC_INLINE size_t
lg_floor(size_t x)
{

	assert(x != 0);

#if (LG_SIZEOF_PTR == LG_SIZEOF_INT)
	return (((8 << LG_SIZEOF_PTR) - 1) - __builtin_clz(x));
#elif (LG_SIZEOF_PTR == LG_SIZEOF_LONG)
	return (((8 << LG_SIZEOF_PTR) - 1) - __builtin_clzl(x));
#else
#  error "Unsupported type sizes for lg_floor()"
#endif
}
#else
JEMALLOC_INLINE size_t
lg_floor(size_t x)
{

	assert(x != 0);

	x |= (x >> 1);
	x |= (x >> 2);
	x |= (x >> 4);
	x |= (x >> 8);
	x |= (x >> 16);
#if (LG_SIZEOF_PTR == 3 && LG_SIZEOF_PTR == LG_SIZEOF_LONG)
	x |= (x >> 32);
	if (x == KZU(0xffffffffffffffff))
		return (63);
	x++;
	return (jemalloc_ffsl(x) - 2);
#elif (LG_SIZEOF_PTR == 2)
	if (x == KZU(0xffffffff))
		return (31);
	x++;
	return (jemalloc_ffs(x) - 2);
#else
#  error "Unsupported type sizes for lg_floor()"
#endif
}
#endif

/* Set error code. */
JEMALLOC_INLINE void
set_errno(int errnum)
{

#ifdef _WIN32
	SetLastError(errnum);
#else
	errno = errnum;
#endif
}

/* Get last error code. */
JEMALLOC_INLINE int
get_errno(void)
{

#ifdef _WIN32
	return (GetLastError());
#else
	return (errno);
#endif
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/

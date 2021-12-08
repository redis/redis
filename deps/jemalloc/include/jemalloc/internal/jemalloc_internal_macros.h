#ifndef JEMALLOC_INTERNAL_MACROS_H
#define JEMALLOC_INTERNAL_MACROS_H

#ifdef JEMALLOC_DEBUG
#  define JEMALLOC_ALWAYS_INLINE static inline
#else
#  define JEMALLOC_ALWAYS_INLINE JEMALLOC_ATTR(always_inline) static inline
#endif
#ifdef _MSC_VER
#  define inline _inline
#endif

#define UNUSED JEMALLOC_ATTR(unused)

#define ZU(z)	((size_t)z)
#define ZD(z)	((ssize_t)z)
#define QU(q)	((uint64_t)q)
#define QD(q)	((int64_t)q)

#define KZU(z)	ZU(z##ULL)
#define KZD(z)	ZD(z##LL)
#define KQU(q)	QU(q##ULL)
#define KQD(q)	QI(q##LL)

#ifndef __DECONST
#  define	__DECONST(type, var)	((type)(uintptr_t)(const void *)(var))
#endif

#if !defined(JEMALLOC_HAS_RESTRICT) || defined(__cplusplus)
#  define restrict
#endif

/* Various function pointers are static and immutable except during testing. */
#ifdef JEMALLOC_JET
#  define JET_MUTABLE
#else
#  define JET_MUTABLE const
#endif

#define JEMALLOC_VA_ARGS_HEAD(head, ...) head
#define JEMALLOC_VA_ARGS_TAIL(head, ...) __VA_ARGS__

#if (defined(__GNUC__) || defined(__GNUG__)) && !defined(__clang__) \
  && defined(JEMALLOC_HAVE_ATTR) && (__GNUC__ >= 7)
#define JEMALLOC_FALLTHROUGH JEMALLOC_ATTR(fallthrough);
#else
#define JEMALLOC_FALLTHROUGH /* falls through */
#endif

/* Diagnostic suppression macros */
#if defined(_MSC_VER) && !defined(__clang__)
#  define JEMALLOC_DIAGNOSTIC_PUSH __pragma(warning(push))
#  define JEMALLOC_DIAGNOSTIC_POP __pragma(warning(pop))
#  define JEMALLOC_DIAGNOSTIC_IGNORE(W) __pragma(warning(disable:W))
#  define JEMALLOC_DIAGNOSTIC_IGNORE_MISSING_STRUCT_FIELD_INITIALIZERS
#  define JEMALLOC_DIAGNOSTIC_IGNORE_TYPE_LIMITS
#  define JEMALLOC_DIAGNOSTIC_IGNORE_ALLOC_SIZE_LARGER_THAN
#  define JEMALLOC_DIAGNOSTIC_DISABLE_SPURIOUS
/* #pragma GCC diagnostic first appeared in gcc 4.6. */
#elif (defined(__GNUC__) && ((__GNUC__ > 4) || ((__GNUC__ == 4) && \
  (__GNUC_MINOR__ > 5)))) || defined(__clang__)
/*
 * The JEMALLOC_PRAGMA__ macro is an implementation detail of the GCC and Clang
 * diagnostic suppression macros and should not be used anywhere else.
 */
#  define JEMALLOC_PRAGMA__(X) _Pragma(#X)
#  define JEMALLOC_DIAGNOSTIC_PUSH JEMALLOC_PRAGMA__(GCC diagnostic push)
#  define JEMALLOC_DIAGNOSTIC_POP JEMALLOC_PRAGMA__(GCC diagnostic pop)
#  define JEMALLOC_DIAGNOSTIC_IGNORE(W) \
     JEMALLOC_PRAGMA__(GCC diagnostic ignored W)

/*
 * The -Wmissing-field-initializers warning is buggy in GCC versions < 5.1 and
 * all clang versions up to version 7 (currently trunk, unreleased).  This macro
 * suppresses the warning for the affected compiler versions only.
 */
#  if ((defined(__GNUC__) && !defined(__clang__)) && (__GNUC__ < 5)) || \
     defined(__clang__)
#    define JEMALLOC_DIAGNOSTIC_IGNORE_MISSING_STRUCT_FIELD_INITIALIZERS  \
          JEMALLOC_DIAGNOSTIC_IGNORE("-Wmissing-field-initializers")
#  else
#    define JEMALLOC_DIAGNOSTIC_IGNORE_MISSING_STRUCT_FIELD_INITIALIZERS
#  endif

#  define JEMALLOC_DIAGNOSTIC_IGNORE_TYPE_LIMITS  \
     JEMALLOC_DIAGNOSTIC_IGNORE("-Wtype-limits")
#  define JEMALLOC_DIAGNOSTIC_IGNORE_UNUSED_PARAMETER \
     JEMALLOC_DIAGNOSTIC_IGNORE("-Wunused-parameter")
#  if defined(__GNUC__) && !defined(__clang__) && (__GNUC__ >= 7)
#    define JEMALLOC_DIAGNOSTIC_IGNORE_ALLOC_SIZE_LARGER_THAN \
       JEMALLOC_DIAGNOSTIC_IGNORE("-Walloc-size-larger-than=")
#  else
#    define JEMALLOC_DIAGNOSTIC_IGNORE_ALLOC_SIZE_LARGER_THAN
#  endif
#  define JEMALLOC_DIAGNOSTIC_DISABLE_SPURIOUS \
  JEMALLOC_DIAGNOSTIC_PUSH \
  JEMALLOC_DIAGNOSTIC_IGNORE_UNUSED_PARAMETER
#else
#  define JEMALLOC_DIAGNOSTIC_PUSH
#  define JEMALLOC_DIAGNOSTIC_POP
#  define JEMALLOC_DIAGNOSTIC_IGNORE(W)
#  define JEMALLOC_DIAGNOSTIC_IGNORE_MISSING_STRUCT_FIELD_INITIALIZERS
#  define JEMALLOC_DIAGNOSTIC_IGNORE_TYPE_LIMITS
#  define JEMALLOC_DIAGNOSTIC_IGNORE_ALLOC_SIZE_LARGER_THAN
#  define JEMALLOC_DIAGNOSTIC_DISABLE_SPURIOUS
#endif

/*
 * Disables spurious diagnostics for all headers.  Since these headers are not
 * included by users directly, it does not affect their diagnostic settings.
 */
JEMALLOC_DIAGNOSTIC_DISABLE_SPURIOUS

#endif /* JEMALLOC_INTERNAL_MACROS_H */

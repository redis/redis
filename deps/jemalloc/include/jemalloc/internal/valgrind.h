/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#ifdef JEMALLOC_VALGRIND
#include <valgrind/valgrind.h>

/*
 * The size that is reported to Valgrind must be consistent through a chain of
 * malloc..realloc..realloc calls.  Request size isn't recorded anywhere in
 * jemalloc, so it is critical that all callers of these macros provide usize
 * rather than request size.  As a result, buffer overflow detection is
 * technically weakened for the standard API, though it is generally accepted
 * practice to consider any extra bytes reported by malloc_usable_size() as
 * usable space.
 */
#define	JEMALLOC_VALGRIND_MAKE_MEM_NOACCESS(ptr, usize) do {		\
	if (unlikely(in_valgrind))					\
		valgrind_make_mem_noaccess(ptr, usize);			\
} while (0)
#define	JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(ptr, usize) do {		\
	if (unlikely(in_valgrind))					\
		valgrind_make_mem_undefined(ptr, usize);		\
} while (0)
#define	JEMALLOC_VALGRIND_MAKE_MEM_DEFINED(ptr, usize) do {		\
	if (unlikely(in_valgrind))					\
		valgrind_make_mem_defined(ptr, usize);			\
} while (0)
/*
 * The VALGRIND_MALLOCLIKE_BLOCK() and VALGRIND_RESIZEINPLACE_BLOCK() macro
 * calls must be embedded in macros rather than in functions so that when
 * Valgrind reports errors, there are no extra stack frames in the backtraces.
 */
#define	JEMALLOC_VALGRIND_MALLOC(cond, tsdn, ptr, usize, zero) do {	\
	if (unlikely(in_valgrind && cond)) {				\
		VALGRIND_MALLOCLIKE_BLOCK(ptr, usize, p2rz(tsdn, ptr),	\
		    zero);						\
	}								\
} while (0)
#define	JEMALLOC_VALGRIND_REALLOC_MOVED_no(ptr, old_ptr)		\
    (false)
#define	JEMALLOC_VALGRIND_REALLOC_MOVED_maybe(ptr, old_ptr)		\
    ((ptr) != (old_ptr))
#define	JEMALLOC_VALGRIND_REALLOC_PTR_NULL_no(ptr)			\
    (false)
#define	JEMALLOC_VALGRIND_REALLOC_PTR_NULL_maybe(ptr)			\
    (ptr == NULL)
#define	JEMALLOC_VALGRIND_REALLOC_OLD_PTR_NULL_no(old_ptr)		\
    (false)
#define	JEMALLOC_VALGRIND_REALLOC_OLD_PTR_NULL_maybe(old_ptr)		\
    (old_ptr == NULL)
#define	JEMALLOC_VALGRIND_REALLOC(moved, tsdn, ptr, usize, ptr_null,	\
    old_ptr, old_usize, old_rzsize, old_ptr_null, zero) do {		\
	if (unlikely(in_valgrind)) {					\
		size_t rzsize = p2rz(tsdn, ptr);			\
									\
		if (!JEMALLOC_VALGRIND_REALLOC_MOVED_##moved(ptr,	\
		    old_ptr)) {						\
			VALGRIND_RESIZEINPLACE_BLOCK(ptr, old_usize,	\
			    usize, rzsize);				\
			if (zero && old_usize < usize) {		\
				valgrind_make_mem_defined(		\
				    (void *)((uintptr_t)ptr +		\
				    old_usize), usize - old_usize);	\
			}						\
		} else {						\
			if (!JEMALLOC_VALGRIND_REALLOC_OLD_PTR_NULL_##	\
			    old_ptr_null(old_ptr)) {			\
				valgrind_freelike_block(old_ptr,	\
				    old_rzsize);			\
			}						\
			if (!JEMALLOC_VALGRIND_REALLOC_PTR_NULL_##	\
			    ptr_null(ptr)) {				\
				size_t copy_size = (old_usize < usize)	\
				    ?  old_usize : usize;		\
				size_t tail_size = usize - copy_size;	\
				VALGRIND_MALLOCLIKE_BLOCK(ptr, usize,	\
				    rzsize, false);			\
				if (copy_size > 0) {			\
					valgrind_make_mem_defined(ptr,	\
					copy_size);			\
				}					\
				if (zero && tail_size > 0) {		\
					valgrind_make_mem_defined(	\
					    (void *)((uintptr_t)ptr +	\
					    copy_size), tail_size);	\
				}					\
			}						\
		}							\
	}								\
} while (0)
#define	JEMALLOC_VALGRIND_FREE(ptr, rzsize) do {			\
	if (unlikely(in_valgrind))					\
		valgrind_freelike_block(ptr, rzsize);			\
} while (0)
#else
#define	RUNNING_ON_VALGRIND	((unsigned)0)
#define	JEMALLOC_VALGRIND_MAKE_MEM_NOACCESS(ptr, usize) do {} while (0)
#define	JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(ptr, usize) do {} while (0)
#define	JEMALLOC_VALGRIND_MAKE_MEM_DEFINED(ptr, usize) do {} while (0)
#define	JEMALLOC_VALGRIND_MALLOC(cond, tsdn, ptr, usize, zero) do {} while (0)
#define	JEMALLOC_VALGRIND_REALLOC(maybe_moved, tsdn, ptr, usize,	\
    ptr_maybe_null, old_ptr, old_usize, old_rzsize, old_ptr_maybe_null,	\
    zero) do {} while (0)
#define	JEMALLOC_VALGRIND_FREE(ptr, rzsize) do {} while (0)
#endif

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

#ifdef JEMALLOC_VALGRIND
void	valgrind_make_mem_noaccess(void *ptr, size_t usize);
void	valgrind_make_mem_undefined(void *ptr, size_t usize);
void	valgrind_make_mem_defined(void *ptr, size_t usize);
void	valgrind_freelike_block(void *ptr, size_t usize);
#endif

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/


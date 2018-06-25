/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

/*
 * Size and alignment of memory chunks that are allocated by the OS's virtual
 * memory system.
 */
#define	LG_CHUNK_DEFAULT	21

/* Return the chunk address for allocation address a. */
#define	CHUNK_ADDR2BASE(a)						\
	((void *)((uintptr_t)(a) & ~chunksize_mask))

/* Return the chunk offset of address a. */
#define	CHUNK_ADDR2OFFSET(a)						\
	((size_t)((uintptr_t)(a) & chunksize_mask))

/* Return the smallest chunk multiple that is >= s. */
#define	CHUNK_CEILING(s)						\
	(((s) + chunksize_mask) & ~chunksize_mask)

#define	CHUNK_HOOKS_INITIALIZER {					\
    NULL,								\
    NULL,								\
    NULL,								\
    NULL,								\
    NULL,								\
    NULL,								\
    NULL								\
}

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

extern size_t		opt_lg_chunk;
extern const char	*opt_dss;

extern rtree_t		chunks_rtree;

extern size_t		chunksize;
extern size_t		chunksize_mask; /* (chunksize - 1). */
extern size_t		chunk_npages;

extern const chunk_hooks_t	chunk_hooks_default;

chunk_hooks_t	chunk_hooks_get(tsdn_t *tsdn, arena_t *arena);
chunk_hooks_t	chunk_hooks_set(tsdn_t *tsdn, arena_t *arena,
    const chunk_hooks_t *chunk_hooks);

bool	chunk_register(const void *chunk, const extent_node_t *node,
    bool *gdump);
void	chunk_deregister(const void *chunk, const extent_node_t *node);
void	*chunk_alloc_base(size_t size);
void	*chunk_alloc_cache(tsdn_t *tsdn, arena_t *arena,
    chunk_hooks_t *chunk_hooks, void *new_addr, size_t size, size_t alignment,
    size_t *sn, bool *zero, bool *commit, bool dalloc_node);
void	*chunk_alloc_wrapper(tsdn_t *tsdn, arena_t *arena,
    chunk_hooks_t *chunk_hooks, void *new_addr, size_t size, size_t alignment,
    size_t *sn, bool *zero, bool *commit);
void	chunk_dalloc_cache(tsdn_t *tsdn, arena_t *arena,
    chunk_hooks_t *chunk_hooks, void *chunk, size_t size, size_t sn,
    bool committed);
void	chunk_dalloc_wrapper(tsdn_t *tsdn, arena_t *arena,
    chunk_hooks_t *chunk_hooks, void *chunk, size_t size, size_t sn,
    bool zeroed, bool committed);
bool	chunk_purge_wrapper(tsdn_t *tsdn, arena_t *arena,
    chunk_hooks_t *chunk_hooks, void *chunk, size_t size, size_t offset,
    size_t length);
bool	chunk_boot(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
extent_node_t	*chunk_lookup(const void *chunk, bool dependent);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_CHUNK_C_))
JEMALLOC_INLINE extent_node_t *
chunk_lookup(const void *ptr, bool dependent)
{

	return (rtree_get(&chunks_rtree, (uintptr_t)ptr, dependent));
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/

#include "jemalloc/internal/chunk_dss.h"
#include "jemalloc/internal/chunk_mmap.h"

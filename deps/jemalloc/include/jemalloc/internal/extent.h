/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef struct extent_node_s extent_node_t;

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

/* Tree of extents.  Use accessor functions for en_* fields. */
struct extent_node_s {
	/* Arena from which this extent came, if any. */
	arena_t			*en_arena;

	/* Pointer to the extent that this tree node is responsible for. */
	void			*en_addr;

	/* Total region size. */
	size_t			en_size;

	/*
	 * The zeroed flag is used by chunk recycling code to track whether
	 * memory is zero-filled.
	 */
	bool			en_zeroed;

	/*
	 * True if physical memory is committed to the extent, whether
	 * explicitly or implicitly as on a system that overcommits and
	 * satisfies physical memory needs on demand via soft page faults.
	 */
	bool			en_committed;

	/*
	 * The achunk flag is used to validate that huge allocation lookups
	 * don't return arena chunks.
	 */
	bool			en_achunk;

	/* Profile counters, used for huge objects. */
	prof_tctx_t		*en_prof_tctx;

	/* Linkage for arena's runs_dirty and chunks_cache rings. */
	arena_runs_dirty_link_t	rd;
	qr(extent_node_t)	cc_link;

	union {
		/* Linkage for the size/address-ordered tree. */
		rb_node(extent_node_t)	szad_link;

		/* Linkage for arena's huge and node_cache lists. */
		ql_elm(extent_node_t)	ql_link;
	};

	/* Linkage for the address-ordered tree. */
	rb_node(extent_node_t)	ad_link;
};
typedef rb_tree(extent_node_t) extent_tree_t;

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

rb_proto(, extent_tree_szad_, extent_tree_t, extent_node_t)

rb_proto(, extent_tree_ad_, extent_tree_t, extent_node_t)

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
arena_t	*extent_node_arena_get(const extent_node_t *node);
void	*extent_node_addr_get(const extent_node_t *node);
size_t	extent_node_size_get(const extent_node_t *node);
bool	extent_node_zeroed_get(const extent_node_t *node);
bool	extent_node_committed_get(const extent_node_t *node);
bool	extent_node_achunk_get(const extent_node_t *node);
prof_tctx_t	*extent_node_prof_tctx_get(const extent_node_t *node);
void	extent_node_arena_set(extent_node_t *node, arena_t *arena);
void	extent_node_addr_set(extent_node_t *node, void *addr);
void	extent_node_size_set(extent_node_t *node, size_t size);
void	extent_node_zeroed_set(extent_node_t *node, bool zeroed);
void	extent_node_committed_set(extent_node_t *node, bool committed);
void	extent_node_achunk_set(extent_node_t *node, bool achunk);
void	extent_node_prof_tctx_set(extent_node_t *node, prof_tctx_t *tctx);
void	extent_node_init(extent_node_t *node, arena_t *arena, void *addr,
    size_t size, bool zeroed, bool committed);
void	extent_node_dirty_linkage_init(extent_node_t *node);
void	extent_node_dirty_insert(extent_node_t *node,
    arena_runs_dirty_link_t *runs_dirty, extent_node_t *chunks_dirty);
void	extent_node_dirty_remove(extent_node_t *node);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_EXTENT_C_))
JEMALLOC_INLINE arena_t *
extent_node_arena_get(const extent_node_t *node)
{

	return (node->en_arena);
}

JEMALLOC_INLINE void *
extent_node_addr_get(const extent_node_t *node)
{

	return (node->en_addr);
}

JEMALLOC_INLINE size_t
extent_node_size_get(const extent_node_t *node)
{

	return (node->en_size);
}

JEMALLOC_INLINE bool
extent_node_zeroed_get(const extent_node_t *node)
{

	return (node->en_zeroed);
}

JEMALLOC_INLINE bool
extent_node_committed_get(const extent_node_t *node)
{

	assert(!node->en_achunk);
	return (node->en_committed);
}

JEMALLOC_INLINE bool
extent_node_achunk_get(const extent_node_t *node)
{

	return (node->en_achunk);
}

JEMALLOC_INLINE prof_tctx_t *
extent_node_prof_tctx_get(const extent_node_t *node)
{

	return (node->en_prof_tctx);
}

JEMALLOC_INLINE void
extent_node_arena_set(extent_node_t *node, arena_t *arena)
{

	node->en_arena = arena;
}

JEMALLOC_INLINE void
extent_node_addr_set(extent_node_t *node, void *addr)
{

	node->en_addr = addr;
}

JEMALLOC_INLINE void
extent_node_size_set(extent_node_t *node, size_t size)
{

	node->en_size = size;
}

JEMALLOC_INLINE void
extent_node_zeroed_set(extent_node_t *node, bool zeroed)
{

	node->en_zeroed = zeroed;
}

JEMALLOC_INLINE void
extent_node_committed_set(extent_node_t *node, bool committed)
{

	node->en_committed = committed;
}

JEMALLOC_INLINE void
extent_node_achunk_set(extent_node_t *node, bool achunk)
{

	node->en_achunk = achunk;
}

JEMALLOC_INLINE void
extent_node_prof_tctx_set(extent_node_t *node, prof_tctx_t *tctx)
{

	node->en_prof_tctx = tctx;
}

JEMALLOC_INLINE void
extent_node_init(extent_node_t *node, arena_t *arena, void *addr, size_t size,
    bool zeroed, bool committed)
{

	extent_node_arena_set(node, arena);
	extent_node_addr_set(node, addr);
	extent_node_size_set(node, size);
	extent_node_zeroed_set(node, zeroed);
	extent_node_committed_set(node, committed);
	extent_node_achunk_set(node, false);
	if (config_prof)
		extent_node_prof_tctx_set(node, NULL);
}

JEMALLOC_INLINE void
extent_node_dirty_linkage_init(extent_node_t *node)
{

	qr_new(&node->rd, rd_link);
	qr_new(node, cc_link);
}

JEMALLOC_INLINE void
extent_node_dirty_insert(extent_node_t *node,
    arena_runs_dirty_link_t *runs_dirty, extent_node_t *chunks_dirty)
{

	qr_meld(runs_dirty, &node->rd, rd_link);
	qr_meld(chunks_dirty, node, cc_link);
}

JEMALLOC_INLINE void
extent_node_dirty_remove(extent_node_t *node)
{

	qr_remove(&node->rd, rd_link);
	qr_remove(node, cc_link);
}

#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/


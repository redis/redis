/*
 * This radix tree implementation is tailored to the singular purpose of
 * associating metadata with chunks that are currently owned by jemalloc.
 *
 *******************************************************************************
 */
#ifdef JEMALLOC_H_TYPES

typedef struct rtree_node_elm_s rtree_node_elm_t;
typedef struct rtree_level_s rtree_level_t;
typedef struct rtree_s rtree_t;

/*
 * RTREE_BITS_PER_LEVEL must be a power of two that is no larger than the
 * machine address width.
 */
#define	LG_RTREE_BITS_PER_LEVEL	4
#define	RTREE_BITS_PER_LEVEL	(1U << LG_RTREE_BITS_PER_LEVEL)
/* Maximum rtree height. */
#define	RTREE_HEIGHT_MAX						\
    ((1U << (LG_SIZEOF_PTR+3)) / RTREE_BITS_PER_LEVEL)

/* Used for two-stage lock-free node initialization. */
#define	RTREE_NODE_INITIALIZING	((rtree_node_elm_t *)0x1)

/*
 * The node allocation callback function's argument is the number of contiguous
 * rtree_node_elm_t structures to allocate, and the resulting memory must be
 * zeroed.
 */
typedef rtree_node_elm_t *(rtree_node_alloc_t)(size_t);
typedef void (rtree_node_dalloc_t)(rtree_node_elm_t *);

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

struct rtree_node_elm_s {
	union {
		void			*pun;
		rtree_node_elm_t	*child;
		extent_node_t		*val;
	};
};

struct rtree_level_s {
	/*
	 * A non-NULL subtree points to a subtree rooted along the hypothetical
	 * path to the leaf node corresponding to key 0.  Depending on what keys
	 * have been used to store to the tree, an arbitrary combination of
	 * subtree pointers may remain NULL.
	 *
	 * Suppose keys comprise 48 bits, and LG_RTREE_BITS_PER_LEVEL is 4.
	 * This results in a 3-level tree, and the leftmost leaf can be directly
	 * accessed via subtrees[2], the subtree prefixed by 0x0000 (excluding
	 * 0x00000000) can be accessed via subtrees[1], and the remainder of the
	 * tree can be accessed via subtrees[0].
	 *
	 *   levels[0] : [<unused> | 0x0001******** | 0x0002******** | ...]
	 *
	 *   levels[1] : [<unused> | 0x00000001**** | 0x00000002**** | ... ]
	 *
	 *   levels[2] : [val(0x000000000000) | val(0x000000000001) | ...]
	 *
	 * This has practical implications on x64, which currently uses only the
	 * lower 47 bits of virtual address space in userland, thus leaving
	 * subtrees[0] unused and avoiding a level of tree traversal.
	 */
	union {
		void			*subtree_pun;
		rtree_node_elm_t	*subtree;
	};
	/* Number of key bits distinguished by this level. */
	unsigned		bits;
	/*
	 * Cumulative number of key bits distinguished by traversing to
	 * corresponding tree level.
	 */
	unsigned		cumbits;
};

struct rtree_s {
	rtree_node_alloc_t	*alloc;
	rtree_node_dalloc_t	*dalloc;
	unsigned		height;
	/*
	 * Precomputed table used to convert from the number of leading 0 key
	 * bits to which subtree level to start at.
	 */
	unsigned		start_level[RTREE_HEIGHT_MAX];
	rtree_level_t		levels[RTREE_HEIGHT_MAX];
};

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

bool rtree_new(rtree_t *rtree, unsigned bits, rtree_node_alloc_t *alloc,
    rtree_node_dalloc_t *dalloc);
void	rtree_delete(rtree_t *rtree);
rtree_node_elm_t	*rtree_subtree_read_hard(rtree_t *rtree,
    unsigned level);
rtree_node_elm_t	*rtree_child_read_hard(rtree_t *rtree,
    rtree_node_elm_t *elm, unsigned level);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
unsigned	rtree_start_level(rtree_t *rtree, uintptr_t key);
uintptr_t	rtree_subkey(rtree_t *rtree, uintptr_t key, unsigned level);

bool	rtree_node_valid(rtree_node_elm_t *node);
rtree_node_elm_t	*rtree_child_tryread(rtree_node_elm_t *elm,
    bool dependent);
rtree_node_elm_t	*rtree_child_read(rtree_t *rtree, rtree_node_elm_t *elm,
    unsigned level, bool dependent);
extent_node_t	*rtree_val_read(rtree_t *rtree, rtree_node_elm_t *elm,
    bool dependent);
void	rtree_val_write(rtree_t *rtree, rtree_node_elm_t *elm,
    const extent_node_t *val);
rtree_node_elm_t	*rtree_subtree_tryread(rtree_t *rtree, unsigned level,
    bool dependent);
rtree_node_elm_t	*rtree_subtree_read(rtree_t *rtree, unsigned level,
    bool dependent);

extent_node_t	*rtree_get(rtree_t *rtree, uintptr_t key, bool dependent);
bool	rtree_set(rtree_t *rtree, uintptr_t key, const extent_node_t *val);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_RTREE_C_))
JEMALLOC_ALWAYS_INLINE unsigned
rtree_start_level(rtree_t *rtree, uintptr_t key)
{
	unsigned start_level;

	if (unlikely(key == 0))
		return (rtree->height - 1);

	start_level = rtree->start_level[lg_floor(key) >>
	    LG_RTREE_BITS_PER_LEVEL];
	assert(start_level < rtree->height);
	return (start_level);
}

JEMALLOC_ALWAYS_INLINE uintptr_t
rtree_subkey(rtree_t *rtree, uintptr_t key, unsigned level)
{

	return ((key >> ((ZU(1) << (LG_SIZEOF_PTR+3)) -
	    rtree->levels[level].cumbits)) & ((ZU(1) <<
	    rtree->levels[level].bits) - 1));
}

JEMALLOC_ALWAYS_INLINE bool
rtree_node_valid(rtree_node_elm_t *node)
{

	return ((uintptr_t)node > (uintptr_t)RTREE_NODE_INITIALIZING);
}

JEMALLOC_ALWAYS_INLINE rtree_node_elm_t *
rtree_child_tryread(rtree_node_elm_t *elm, bool dependent)
{
	rtree_node_elm_t *child;

	/* Double-checked read (first read may be stale. */
	child = elm->child;
	if (!dependent && !rtree_node_valid(child))
		child = atomic_read_p(&elm->pun);
	assert(!dependent || child != NULL);
	return (child);
}

JEMALLOC_ALWAYS_INLINE rtree_node_elm_t *
rtree_child_read(rtree_t *rtree, rtree_node_elm_t *elm, unsigned level,
    bool dependent)
{
	rtree_node_elm_t *child;

	child = rtree_child_tryread(elm, dependent);
	if (!dependent && unlikely(!rtree_node_valid(child)))
		child = rtree_child_read_hard(rtree, elm, level);
	assert(!dependent || child != NULL);
	return (child);
}

JEMALLOC_ALWAYS_INLINE extent_node_t *
rtree_val_read(rtree_t *rtree, rtree_node_elm_t *elm, bool dependent)
{

	if (dependent) {
		/*
		 * Reading a val on behalf of a pointer to a valid allocation is
		 * guaranteed to be a clean read even without synchronization,
		 * because the rtree update became visible in memory before the
		 * pointer came into existence.
		 */
		return (elm->val);
	} else {
		/*
		 * An arbitrary read, e.g. on behalf of ivsalloc(), may not be
		 * dependent on a previous rtree write, which means a stale read
		 * could result if synchronization were omitted here.
		 */
		return (atomic_read_p(&elm->pun));
	}
}

JEMALLOC_INLINE void
rtree_val_write(rtree_t *rtree, rtree_node_elm_t *elm, const extent_node_t *val)
{

	atomic_write_p(&elm->pun, val);
}

JEMALLOC_ALWAYS_INLINE rtree_node_elm_t *
rtree_subtree_tryread(rtree_t *rtree, unsigned level, bool dependent)
{
	rtree_node_elm_t *subtree;

	/* Double-checked read (first read may be stale. */
	subtree = rtree->levels[level].subtree;
	if (!dependent && unlikely(!rtree_node_valid(subtree)))
		subtree = atomic_read_p(&rtree->levels[level].subtree_pun);
	assert(!dependent || subtree != NULL);
	return (subtree);
}

JEMALLOC_ALWAYS_INLINE rtree_node_elm_t *
rtree_subtree_read(rtree_t *rtree, unsigned level, bool dependent)
{
	rtree_node_elm_t *subtree;

	subtree = rtree_subtree_tryread(rtree, level, dependent);
	if (!dependent && unlikely(!rtree_node_valid(subtree)))
		subtree = rtree_subtree_read_hard(rtree, level);
	assert(!dependent || subtree != NULL);
	return (subtree);
}

JEMALLOC_ALWAYS_INLINE extent_node_t *
rtree_get(rtree_t *rtree, uintptr_t key, bool dependent)
{
	uintptr_t subkey;
	unsigned start_level;
	rtree_node_elm_t *node;

	start_level = rtree_start_level(rtree, key);

	node = rtree_subtree_tryread(rtree, start_level, dependent);
#define	RTREE_GET_BIAS	(RTREE_HEIGHT_MAX - rtree->height)
	switch (start_level + RTREE_GET_BIAS) {
#define	RTREE_GET_SUBTREE(level)					\
	case level:							\
		assert(level < (RTREE_HEIGHT_MAX-1));			\
		if (!dependent && unlikely(!rtree_node_valid(node)))	\
			return (NULL);					\
		subkey = rtree_subkey(rtree, key, level -		\
		    RTREE_GET_BIAS);					\
		node = rtree_child_tryread(&node[subkey], dependent);	\
		/* Fall through. */
#define	RTREE_GET_LEAF(level)						\
	case level:							\
		assert(level == (RTREE_HEIGHT_MAX-1));			\
		if (!dependent && unlikely(!rtree_node_valid(node)))	\
			return (NULL);					\
		subkey = rtree_subkey(rtree, key, level -		\
		    RTREE_GET_BIAS);					\
		/*							\
		 * node is a leaf, so it contains values rather than	\
		 * child pointers.					\
		 */							\
		return (rtree_val_read(rtree, &node[subkey],		\
		    dependent));
#if RTREE_HEIGHT_MAX > 1
	RTREE_GET_SUBTREE(0)
#endif
#if RTREE_HEIGHT_MAX > 2
	RTREE_GET_SUBTREE(1)
#endif
#if RTREE_HEIGHT_MAX > 3
	RTREE_GET_SUBTREE(2)
#endif
#if RTREE_HEIGHT_MAX > 4
	RTREE_GET_SUBTREE(3)
#endif
#if RTREE_HEIGHT_MAX > 5
	RTREE_GET_SUBTREE(4)
#endif
#if RTREE_HEIGHT_MAX > 6
	RTREE_GET_SUBTREE(5)
#endif
#if RTREE_HEIGHT_MAX > 7
	RTREE_GET_SUBTREE(6)
#endif
#if RTREE_HEIGHT_MAX > 8
	RTREE_GET_SUBTREE(7)
#endif
#if RTREE_HEIGHT_MAX > 9
	RTREE_GET_SUBTREE(8)
#endif
#if RTREE_HEIGHT_MAX > 10
	RTREE_GET_SUBTREE(9)
#endif
#if RTREE_HEIGHT_MAX > 11
	RTREE_GET_SUBTREE(10)
#endif
#if RTREE_HEIGHT_MAX > 12
	RTREE_GET_SUBTREE(11)
#endif
#if RTREE_HEIGHT_MAX > 13
	RTREE_GET_SUBTREE(12)
#endif
#if RTREE_HEIGHT_MAX > 14
	RTREE_GET_SUBTREE(13)
#endif
#if RTREE_HEIGHT_MAX > 15
	RTREE_GET_SUBTREE(14)
#endif
#if RTREE_HEIGHT_MAX > 16
#  error Unsupported RTREE_HEIGHT_MAX
#endif
	RTREE_GET_LEAF(RTREE_HEIGHT_MAX-1)
#undef RTREE_GET_SUBTREE
#undef RTREE_GET_LEAF
	default: not_reached();
	}
#undef RTREE_GET_BIAS
	not_reached();
}

JEMALLOC_INLINE bool
rtree_set(rtree_t *rtree, uintptr_t key, const extent_node_t *val)
{
	uintptr_t subkey;
	unsigned i, start_level;
	rtree_node_elm_t *node, *child;

	start_level = rtree_start_level(rtree, key);

	node = rtree_subtree_read(rtree, start_level, false);
	if (node == NULL)
		return (true);
	for (i = start_level; /**/; i++, node = child) {
		subkey = rtree_subkey(rtree, key, i);
		if (i == rtree->height - 1) {
			/*
			 * node is a leaf, so it contains values rather than
			 * child pointers.
			 */
			rtree_val_write(rtree, &node[subkey], val);
			return (false);
		}
		assert(i + 1 < rtree->height);
		child = rtree_child_read(rtree, &node[subkey], i, false);
		if (child == NULL)
			return (true);
	}
	not_reached();
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/

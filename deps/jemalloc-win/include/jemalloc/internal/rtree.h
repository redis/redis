/*
 * This radix tree implementation is tailored to the singular purpose of
 * tracking which chunks are currently owned by jemalloc.  This functionality
 * is mandatory for OS X, where jemalloc must be able to respond to object
 * ownership queries.
 *
 *******************************************************************************
 */
#ifdef JEMALLOC_H_TYPES

typedef struct rtree_s rtree_t;

/*
 * Size of each radix tree node (must be a power of 2).  This impacts tree
 * depth.
 */
#define	RTREE_NODESIZE (1U << 16)

typedef void *(rtree_alloc_t)(size_t);
typedef void (rtree_dalloc_t)(void *);

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

struct rtree_s {
	rtree_alloc_t	*alloc;
	rtree_dalloc_t	*dalloc;
	malloc_mutex_t	mutex;
	void		**root;
	unsigned	height;
	unsigned	level2bits[1]; /* Dynamically sized. */
};

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

rtree_t	*rtree_new(unsigned bits, rtree_alloc_t *alloc, rtree_dalloc_t *dalloc);
void	rtree_delete(rtree_t *rtree);
void	rtree_prefork(rtree_t *rtree);
void	rtree_postfork_parent(rtree_t *rtree);
void	rtree_postfork_child(rtree_t *rtree);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
#ifdef JEMALLOC_DEBUG
uint8_t rtree_get_locked(rtree_t *rtree, uintptr_t key);
#endif
uint8_t	rtree_get(rtree_t *rtree, uintptr_t key);
bool	rtree_set(rtree_t *rtree, uintptr_t key, uint8_t val);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_RTREE_C_))
#define	RTREE_GET_GENERATE(f)						\
/* The least significant bits of the key are ignored. */		\
JEMALLOC_INLINE uint8_t							\
f(rtree_t *rtree, uintptr_t key)					\
{									\
	uint8_t ret;							\
	uintptr_t subkey;						\
	unsigned i, lshift, height, bits;				\
	void **node, **child;						\
									\
	RTREE_LOCK(&rtree->mutex);					\
	for (i = lshift = 0, height = rtree->height, node = rtree->root;\
	    i < height - 1;						\
	    i++, lshift += bits, node = child) {			\
		bits = rtree->level2bits[i];				\
		subkey = (key << lshift) >> ((ZU(1) << (LG_SIZEOF_PTR +	\
		    3)) - bits);					\
		child = (void**)node[subkey];				\
		if (child == NULL) {					\
			RTREE_UNLOCK(&rtree->mutex);			\
			return (0);					\
		}							\
	}								\
									\
	/*								\
	 * node is a leaf, so it contains values rather than node	\
	 * pointers.							\
	 */								\
	bits = rtree->level2bits[i];					\
	subkey = (key << lshift) >> ((ZU(1) << (LG_SIZEOF_PTR+3)) -	\
	    bits);							\
	{								\
		uint8_t *leaf = (uint8_t *)node;			\
		ret = leaf[subkey];					\
	}								\
	RTREE_UNLOCK(&rtree->mutex);					\
									\
	RTREE_GET_VALIDATE						\
	return (ret);							\
}

#ifdef JEMALLOC_DEBUG
#  define RTREE_LOCK(l)		malloc_mutex_lock(l)
#  define RTREE_UNLOCK(l)	malloc_mutex_unlock(l)
#  define RTREE_GET_VALIDATE
RTREE_GET_GENERATE(rtree_get_locked)
#  undef RTREE_LOCK
#  undef RTREE_UNLOCK
#  undef RTREE_GET_VALIDATE
#endif

#define	RTREE_LOCK(l)
#define	RTREE_UNLOCK(l)
#ifdef JEMALLOC_DEBUG
   /*
    * Suppose that it were possible for a jemalloc-allocated chunk to be
    * munmap()ped, followed by a different allocator in another thread re-using
    * overlapping virtual memory, all without invalidating the cached rtree
    * value.  The result would be a false positive (the rtree would claim that
    * jemalloc owns memory that it had actually discarded).  This scenario
    * seems impossible, but the following assertion is a prudent sanity check.
    */
#  define RTREE_GET_VALIDATE						\
	assert(rtree_get_locked(rtree, key) == ret);
#else
#  define RTREE_GET_VALIDATE
#endif
RTREE_GET_GENERATE(rtree_get)
#undef RTREE_LOCK
#undef RTREE_UNLOCK
#undef RTREE_GET_VALIDATE

JEMALLOC_INLINE bool
rtree_set(rtree_t *rtree, uintptr_t key, uint8_t val)
{
	uintptr_t subkey;
	unsigned i, lshift, height, bits;
	void **node, **child;

	malloc_mutex_lock(&rtree->mutex);
	for (i = lshift = 0, height = rtree->height, node = rtree->root;
	    i < height - 1;
	    i++, lshift += bits, node = child) {
		bits = rtree->level2bits[i];
		subkey = (key << lshift) >> ((ZU(1) << (LG_SIZEOF_PTR+3)) -
		    bits);
		child = (void**)node[subkey];
		if (child == NULL) {
			size_t size = ((i + 1 < height - 1) ? sizeof(void *)
			    : (sizeof(uint8_t))) << rtree->level2bits[i+1];
			child = (void**)rtree->alloc(size);
			if (child == NULL) {
				malloc_mutex_unlock(&rtree->mutex);
				return (true);
			}
			memset(child, 0, size);
			node[subkey] = child;
		}
	}

	/* node is a leaf, so it contains values rather than node pointers. */
	bits = rtree->level2bits[i];
	subkey = (key << lshift) >> ((ZU(1) << (LG_SIZEOF_PTR+3)) - bits);
	{
		uint8_t *leaf = (uint8_t *)node;
		leaf[subkey] = val;
	}
	malloc_mutex_unlock(&rtree->mutex);

	return (false);
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/

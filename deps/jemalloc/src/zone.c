#include "jemalloc/internal/jemalloc_internal.h"
#ifndef JEMALLOC_ZONE
#  error "This source file is for zones on Darwin (OS X)."
#endif

/******************************************************************************/
/* Data. */

static malloc_zone_t zone, szone;
static struct malloc_introspection_t zone_introspect, ozone_introspect;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static size_t	zone_size(malloc_zone_t *zone, void *ptr);
static void	*zone_malloc(malloc_zone_t *zone, size_t size);
static void	*zone_calloc(malloc_zone_t *zone, size_t num, size_t size);
static void	*zone_valloc(malloc_zone_t *zone, size_t size);
static void	zone_free(malloc_zone_t *zone, void *ptr);
static void	*zone_realloc(malloc_zone_t *zone, void *ptr, size_t size);
#if (JEMALLOC_ZONE_VERSION >= 6)
static void	*zone_memalign(malloc_zone_t *zone, size_t alignment,
    size_t size);
static void	zone_free_definite_size(malloc_zone_t *zone, void *ptr,
    size_t size);
#endif
static void	*zone_destroy(malloc_zone_t *zone);
static size_t	zone_good_size(malloc_zone_t *zone, size_t size);
static void	zone_force_lock(malloc_zone_t *zone);
static void	zone_force_unlock(malloc_zone_t *zone);
static size_t	ozone_size(malloc_zone_t *zone, void *ptr);
static void	ozone_free(malloc_zone_t *zone, void *ptr);
static void	*ozone_realloc(malloc_zone_t *zone, void *ptr, size_t size);
static unsigned	ozone_batch_malloc(malloc_zone_t *zone, size_t size,
    void **results, unsigned num_requested);
static void	ozone_batch_free(malloc_zone_t *zone, void **to_be_freed,
    unsigned num);
#if (JEMALLOC_ZONE_VERSION >= 6)
static void	ozone_free_definite_size(malloc_zone_t *zone, void *ptr,
    size_t size);
#endif
static void	ozone_force_lock(malloc_zone_t *zone);
static void	ozone_force_unlock(malloc_zone_t *zone);

/******************************************************************************/
/*
 * Functions.
 */

static size_t
zone_size(malloc_zone_t *zone, void *ptr)
{

	/*
	 * There appear to be places within Darwin (such as setenv(3)) that
	 * cause calls to this function with pointers that *no* zone owns.  If
	 * we knew that all pointers were owned by *some* zone, we could split
	 * our zone into two parts, and use one as the default allocator and
	 * the other as the default deallocator/reallocator.  Since that will
	 * not work in practice, we must check all pointers to assure that they
	 * reside within a mapped chunk before determining size.
	 */
	return (ivsalloc(ptr));
}

static void *
zone_malloc(malloc_zone_t *zone, size_t size)
{

	return (JEMALLOC_P(malloc)(size));
}

static void *
zone_calloc(malloc_zone_t *zone, size_t num, size_t size)
{

	return (JEMALLOC_P(calloc)(num, size));
}

static void *
zone_valloc(malloc_zone_t *zone, size_t size)
{
	void *ret = NULL; /* Assignment avoids useless compiler warning. */

	JEMALLOC_P(posix_memalign)(&ret, PAGE_SIZE, size);

	return (ret);
}

static void
zone_free(malloc_zone_t *zone, void *ptr)
{

	JEMALLOC_P(free)(ptr);
}

static void *
zone_realloc(malloc_zone_t *zone, void *ptr, size_t size)
{

	return (JEMALLOC_P(realloc)(ptr, size));
}

#if (JEMALLOC_ZONE_VERSION >= 6)
static void *
zone_memalign(malloc_zone_t *zone, size_t alignment, size_t size)
{
	void *ret = NULL; /* Assignment avoids useless compiler warning. */

	JEMALLOC_P(posix_memalign)(&ret, alignment, size);

	return (ret);
}

static void
zone_free_definite_size(malloc_zone_t *zone, void *ptr, size_t size)
{

	assert(ivsalloc(ptr) == size);
	JEMALLOC_P(free)(ptr);
}
#endif

static void *
zone_destroy(malloc_zone_t *zone)
{

	/* This function should never be called. */
	assert(false);
	return (NULL);
}

static size_t
zone_good_size(malloc_zone_t *zone, size_t size)
{
	size_t ret;
	void *p;

	/*
	 * Actually create an object of the appropriate size, then find out
	 * how large it could have been without moving up to the next size
	 * class.
	 */
	p = JEMALLOC_P(malloc)(size);
	if (p != NULL) {
		ret = isalloc(p);
		JEMALLOC_P(free)(p);
	} else
		ret = size;

	return (ret);
}

static void
zone_force_lock(malloc_zone_t *zone)
{

	if (isthreaded)
		jemalloc_prefork();
}

static void
zone_force_unlock(malloc_zone_t *zone)
{

	if (isthreaded)
		jemalloc_postfork();
}

malloc_zone_t *
create_zone(void)
{

	zone.size = (void *)zone_size;
	zone.malloc = (void *)zone_malloc;
	zone.calloc = (void *)zone_calloc;
	zone.valloc = (void *)zone_valloc;
	zone.free = (void *)zone_free;
	zone.realloc = (void *)zone_realloc;
	zone.destroy = (void *)zone_destroy;
	zone.zone_name = "jemalloc_zone";
	zone.batch_malloc = NULL;
	zone.batch_free = NULL;
	zone.introspect = &zone_introspect;
	zone.version = JEMALLOC_ZONE_VERSION;
#if (JEMALLOC_ZONE_VERSION >= 6)
	zone.memalign = zone_memalign;
	zone.free_definite_size = zone_free_definite_size;
#endif

	zone_introspect.enumerator = NULL;
	zone_introspect.good_size = (void *)zone_good_size;
	zone_introspect.check = NULL;
	zone_introspect.print = NULL;
	zone_introspect.log = NULL;
	zone_introspect.force_lock = (void *)zone_force_lock;
	zone_introspect.force_unlock = (void *)zone_force_unlock;
	zone_introspect.statistics = NULL;
#if (JEMALLOC_ZONE_VERSION >= 6)
	zone_introspect.zone_locked = NULL;
#endif

	return (&zone);
}

static size_t
ozone_size(malloc_zone_t *zone, void *ptr)
{
	size_t ret;

	ret = ivsalloc(ptr);
	if (ret == 0)
		ret = szone.size(zone, ptr);

	return (ret);
}

static void
ozone_free(malloc_zone_t *zone, void *ptr)
{

	if (ivsalloc(ptr) != 0)
		JEMALLOC_P(free)(ptr);
	else {
		size_t size = szone.size(zone, ptr);
		if (size != 0)
			(szone.free)(zone, ptr);
	}
}

static void *
ozone_realloc(malloc_zone_t *zone, void *ptr, size_t size)
{
	size_t oldsize;

	if (ptr == NULL)
		return (JEMALLOC_P(malloc)(size));

	oldsize = ivsalloc(ptr);
	if (oldsize != 0)
		return (JEMALLOC_P(realloc)(ptr, size));
	else {
		oldsize = szone.size(zone, ptr);
		if (oldsize == 0)
			return (JEMALLOC_P(malloc)(size));
		else {
			void *ret = JEMALLOC_P(malloc)(size);
			if (ret != NULL) {
				memcpy(ret, ptr, (oldsize < size) ? oldsize :
				    size);
				(szone.free)(zone, ptr);
			}
			return (ret);
		}
	}
}

static unsigned
ozone_batch_malloc(malloc_zone_t *zone, size_t size, void **results,
    unsigned num_requested)
{

	/* Don't bother implementing this interface, since it isn't required. */
	return (0);
}

static void
ozone_batch_free(malloc_zone_t *zone, void **to_be_freed, unsigned num)
{
	unsigned i;

	for (i = 0; i < num; i++)
		ozone_free(zone, to_be_freed[i]);
}

#if (JEMALLOC_ZONE_VERSION >= 6)
static void
ozone_free_definite_size(malloc_zone_t *zone, void *ptr, size_t size)
{

	if (ivsalloc(ptr) != 0) {
		assert(ivsalloc(ptr) == size);
		JEMALLOC_P(free)(ptr);
	} else {
		assert(size == szone.size(zone, ptr));
		szone.free_definite_size(zone, ptr, size);
	}
}
#endif

static void
ozone_force_lock(malloc_zone_t *zone)
{

	/* jemalloc locking is taken care of by the normal jemalloc zone. */
	szone.introspect->force_lock(zone);
}

static void
ozone_force_unlock(malloc_zone_t *zone)
{

	/* jemalloc locking is taken care of by the normal jemalloc zone. */
	szone.introspect->force_unlock(zone);
}

/*
 * Overlay the default scalable zone (szone) such that existing allocations are
 * drained, and further allocations come from jemalloc.  This is necessary
 * because Core Foundation directly accesses and uses the szone before the
 * jemalloc library is even loaded.
 */
void
szone2ozone(malloc_zone_t *zone)
{

	/*
	 * Stash a copy of the original szone so that we can call its
	 * functions as needed.  Note that the internally, the szone stores its
	 * bookkeeping data structures immediately following the malloc_zone_t
	 * header, so when calling szone functions, we need to pass a pointer
	 * to the original zone structure.
	 */
	memcpy(&szone, zone, sizeof(malloc_zone_t));

	zone->size = (void *)ozone_size;
	zone->malloc = (void *)zone_malloc;
	zone->calloc = (void *)zone_calloc;
	zone->valloc = (void *)zone_valloc;
	zone->free = (void *)ozone_free;
	zone->realloc = (void *)ozone_realloc;
	zone->destroy = (void *)zone_destroy;
	zone->zone_name = "jemalloc_ozone";
	zone->batch_malloc = ozone_batch_malloc;
	zone->batch_free = ozone_batch_free;
	zone->introspect = &ozone_introspect;
	zone->version = JEMALLOC_ZONE_VERSION;
#if (JEMALLOC_ZONE_VERSION >= 6)
	zone->memalign = zone_memalign;
	zone->free_definite_size = ozone_free_definite_size;
#endif

	ozone_introspect.enumerator = NULL;
	ozone_introspect.good_size = (void *)zone_good_size;
	ozone_introspect.check = NULL;
	ozone_introspect.print = NULL;
	ozone_introspect.log = NULL;
	ozone_introspect.force_lock = (void *)ozone_force_lock;
	ozone_introspect.force_unlock = (void *)ozone_force_unlock;
	ozone_introspect.statistics = NULL;
#if (JEMALLOC_ZONE_VERSION >= 6)
	ozone_introspect.zone_locked = NULL;
#endif
}

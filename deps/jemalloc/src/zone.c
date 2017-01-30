#include "jemalloc/internal/jemalloc_internal.h"
#ifndef JEMALLOC_ZONE
#  error "This source file is for zones on Darwin (OS X)."
#endif

/*
 * The malloc_default_purgeable_zone() function is only available on >= 10.6.
 * We need to check whether it is present at runtime, thus the weak_import.
 */
extern malloc_zone_t *malloc_default_purgeable_zone(void)
JEMALLOC_ATTR(weak_import);

/******************************************************************************/
/* Data. */

static malloc_zone_t *default_zone, *purgeable_zone;
static malloc_zone_t jemalloc_zone;
static struct malloc_introspection_t jemalloc_zone_introspect;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static size_t	zone_size(malloc_zone_t *zone, void *ptr);
static void	*zone_malloc(malloc_zone_t *zone, size_t size);
static void	*zone_calloc(malloc_zone_t *zone, size_t num, size_t size);
static void	*zone_valloc(malloc_zone_t *zone, size_t size);
static void	zone_free(malloc_zone_t *zone, void *ptr);
static void	*zone_realloc(malloc_zone_t *zone, void *ptr, size_t size);
#if (JEMALLOC_ZONE_VERSION >= 5)
static void	*zone_memalign(malloc_zone_t *zone, size_t alignment,
#endif
#if (JEMALLOC_ZONE_VERSION >= 6)
    size_t size);
static void	zone_free_definite_size(malloc_zone_t *zone, void *ptr,
    size_t size);
#endif
static void	*zone_destroy(malloc_zone_t *zone);
static size_t	zone_good_size(malloc_zone_t *zone, size_t size);
static void	zone_force_lock(malloc_zone_t *zone);
static void	zone_force_unlock(malloc_zone_t *zone);

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
	return (ivsalloc(tsdn_fetch(), ptr, config_prof));
}

static void *
zone_malloc(malloc_zone_t *zone, size_t size)
{

	return (je_malloc(size));
}

static void *
zone_calloc(malloc_zone_t *zone, size_t num, size_t size)
{

	return (je_calloc(num, size));
}

static void *
zone_valloc(malloc_zone_t *zone, size_t size)
{
	void *ret = NULL; /* Assignment avoids useless compiler warning. */

	je_posix_memalign(&ret, PAGE, size);

	return (ret);
}

static void
zone_free(malloc_zone_t *zone, void *ptr)
{

	if (ivsalloc(tsdn_fetch(), ptr, config_prof) != 0) {
		je_free(ptr);
		return;
	}

	free(ptr);
}

static void *
zone_realloc(malloc_zone_t *zone, void *ptr, size_t size)
{

	if (ivsalloc(tsdn_fetch(), ptr, config_prof) != 0)
		return (je_realloc(ptr, size));

	return (realloc(ptr, size));
}

#if (JEMALLOC_ZONE_VERSION >= 5)
static void *
zone_memalign(malloc_zone_t *zone, size_t alignment, size_t size)
{
	void *ret = NULL; /* Assignment avoids useless compiler warning. */

	je_posix_memalign(&ret, alignment, size);

	return (ret);
}
#endif

#if (JEMALLOC_ZONE_VERSION >= 6)
static void
zone_free_definite_size(malloc_zone_t *zone, void *ptr, size_t size)
{
	size_t alloc_size;

	alloc_size = ivsalloc(tsdn_fetch(), ptr, config_prof);
	if (alloc_size != 0) {
		assert(alloc_size == size);
		je_free(ptr);
		return;
	}

	free(ptr);
}
#endif

static void *
zone_destroy(malloc_zone_t *zone)
{

	/* This function should never be called. */
	not_reached();
	return (NULL);
}

static size_t
zone_good_size(malloc_zone_t *zone, size_t size)
{

	if (size == 0)
		size = 1;
	return (s2u(size));
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

	/*
	 * Call jemalloc_postfork_child() rather than
	 * jemalloc_postfork_parent(), because this function is executed by both
	 * parent and child.  The parent can tolerate having state
	 * reinitialized, but the child cannot unlock mutexes that were locked
	 * by the parent.
	 */
	if (isthreaded)
		jemalloc_postfork_child();
}

static void
zone_init(void)
{

	jemalloc_zone.size = (void *)zone_size;
	jemalloc_zone.malloc = (void *)zone_malloc;
	jemalloc_zone.calloc = (void *)zone_calloc;
	jemalloc_zone.valloc = (void *)zone_valloc;
	jemalloc_zone.free = (void *)zone_free;
	jemalloc_zone.realloc = (void *)zone_realloc;
	jemalloc_zone.destroy = (void *)zone_destroy;
	jemalloc_zone.zone_name = "jemalloc_zone";
	jemalloc_zone.batch_malloc = NULL;
	jemalloc_zone.batch_free = NULL;
	jemalloc_zone.introspect = &jemalloc_zone_introspect;
	jemalloc_zone.version = JEMALLOC_ZONE_VERSION;
#if (JEMALLOC_ZONE_VERSION >= 5)
	jemalloc_zone.memalign = zone_memalign;
#endif
#if (JEMALLOC_ZONE_VERSION >= 6)
	jemalloc_zone.free_definite_size = zone_free_definite_size;
#endif
#if (JEMALLOC_ZONE_VERSION >= 8)
	jemalloc_zone.pressure_relief = NULL;
#endif

	jemalloc_zone_introspect.enumerator = NULL;
	jemalloc_zone_introspect.good_size = (void *)zone_good_size;
	jemalloc_zone_introspect.check = NULL;
	jemalloc_zone_introspect.print = NULL;
	jemalloc_zone_introspect.log = NULL;
	jemalloc_zone_introspect.force_lock = (void *)zone_force_lock;
	jemalloc_zone_introspect.force_unlock = (void *)zone_force_unlock;
	jemalloc_zone_introspect.statistics = NULL;
#if (JEMALLOC_ZONE_VERSION >= 6)
	jemalloc_zone_introspect.zone_locked = NULL;
#endif
#if (JEMALLOC_ZONE_VERSION >= 7)
	jemalloc_zone_introspect.enable_discharge_checking = NULL;
	jemalloc_zone_introspect.disable_discharge_checking = NULL;
	jemalloc_zone_introspect.discharge = NULL;
#  ifdef __BLOCKS__
	jemalloc_zone_introspect.enumerate_discharged_pointers = NULL;
#  else
	jemalloc_zone_introspect.enumerate_unavailable_without_blocks = NULL;
#  endif
#endif
}

static malloc_zone_t *
zone_default_get(void)
{
	malloc_zone_t **zones = NULL;
	unsigned int num_zones = 0;

	/*
	 * On OSX 10.12, malloc_default_zone returns a special zone that is not
	 * present in the list of registered zones. That zone uses a "lite zone"
	 * if one is present (apparently enabled when malloc stack logging is
	 * enabled), or the first registered zone otherwise. In practice this
	 * means unless malloc stack logging is enabled, the first registered
	 * zone is the default.  So get the list of zones to get the first one,
	 * instead of relying on malloc_default_zone.
	 */
	if (KERN_SUCCESS != malloc_get_all_zones(0, NULL,
	    (vm_address_t**)&zones, &num_zones)) {
		/*
		 * Reset the value in case the failure happened after it was
		 * set.
		 */
		num_zones = 0;
	}

	if (num_zones)
		return (zones[0]);

	return (malloc_default_zone());
}

/* As written, this function can only promote jemalloc_zone. */
static void
zone_promote(void)
{
	malloc_zone_t *zone;

	do {
		/*
		 * Unregister and reregister the default zone.  On OSX >= 10.6,
		 * unregistering takes the last registered zone and places it
		 * at the location of the specified zone.  Unregistering the
		 * default zone thus makes the last registered one the default.
		 * On OSX < 10.6, unregistering shifts all registered zones.
		 * The first registered zone then becomes the default.
		 */
		malloc_zone_unregister(default_zone);
		malloc_zone_register(default_zone);

		/*
		 * On OSX 10.6, having the default purgeable zone appear before
		 * the default zone makes some things crash because it thinks it
		 * owns the default zone allocated pointers.  We thus
		 * unregister/re-register it in order to ensure it's always
		 * after the default zone.  On OSX < 10.6, there is no purgeable
		 * zone, so this does nothing.  On OSX >= 10.6, unregistering
		 * replaces the purgeable zone with the last registered zone
		 * above, i.e. the default zone.  Registering it again then puts
		 * it at the end, obviously after the default zone.
		 */
		if (purgeable_zone != NULL) {
			malloc_zone_unregister(purgeable_zone);
			malloc_zone_register(purgeable_zone);
		}

		zone = zone_default_get();
	} while (zone != &jemalloc_zone);
}

JEMALLOC_ATTR(constructor)
void
zone_register(void)
{

	/*
	 * If something else replaced the system default zone allocator, don't
	 * register jemalloc's.
	 */
	default_zone = zone_default_get();
	if (!default_zone->zone_name || strcmp(default_zone->zone_name,
	    "DefaultMallocZone") != 0)
		return;

	/*
	 * The default purgeable zone is created lazily by OSX's libc.  It uses
	 * the default zone when it is created for "small" allocations
	 * (< 15 KiB), but assumes the default zone is a scalable_zone.  This
	 * obviously fails when the default zone is the jemalloc zone, so
	 * malloc_default_purgeable_zone() is called beforehand so that the
	 * default purgeable zone is created when the default zone is still
	 * a scalable_zone.  As purgeable zones only exist on >= 10.6, we need
	 * to check for the existence of malloc_default_purgeable_zone() at
	 * run time.
	 */
	purgeable_zone = (malloc_default_purgeable_zone == NULL) ? NULL :
	    malloc_default_purgeable_zone();

	/* Register the custom zone.  At this point it won't be the default. */
	zone_init();
	malloc_zone_register(&jemalloc_zone);

	/* Promote the custom zone to be default. */
	zone_promote();
}

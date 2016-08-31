#define	JEMALLOC_QUARANTINE_C_
#include "jemalloc/internal/jemalloc_internal.h"

/*
 * Quarantine pointers close to NULL are used to encode state information that
 * is used for cleaning up during thread shutdown.
 */
#define	QUARANTINE_STATE_REINCARNATED	((quarantine_t *)(uintptr_t)1)
#define	QUARANTINE_STATE_PURGATORY	((quarantine_t *)(uintptr_t)2)
#define	QUARANTINE_STATE_MAX		QUARANTINE_STATE_PURGATORY

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static quarantine_t	*quarantine_grow(tsd_t *tsd, quarantine_t *quarantine);
static void	quarantine_drain_one(tsd_t *tsd, quarantine_t *quarantine);
static void	quarantine_drain(tsd_t *tsd, quarantine_t *quarantine,
    size_t upper_bound);

/******************************************************************************/

static quarantine_t *
quarantine_init(tsd_t *tsd, size_t lg_maxobjs)
{
	quarantine_t *quarantine;

	assert(tsd_nominal(tsd));

	quarantine = (quarantine_t *)iallocztm(tsd, offsetof(quarantine_t, objs)
	    + ((ZU(1) << lg_maxobjs) * sizeof(quarantine_obj_t)), false,
	    tcache_get(tsd, true), true, NULL);
	if (quarantine == NULL)
		return (NULL);
	quarantine->curbytes = 0;
	quarantine->curobjs = 0;
	quarantine->first = 0;
	quarantine->lg_maxobjs = lg_maxobjs;

	return (quarantine);
}

void
quarantine_alloc_hook_work(tsd_t *tsd)
{
	quarantine_t *quarantine;

	if (!tsd_nominal(tsd))
		return;

	quarantine = quarantine_init(tsd, LG_MAXOBJS_INIT);
	/*
	 * Check again whether quarantine has been initialized, because
	 * quarantine_init() may have triggered recursive initialization.
	 */
	if (tsd_quarantine_get(tsd) == NULL)
		tsd_quarantine_set(tsd, quarantine);
	else
		idalloctm(tsd, quarantine, tcache_get(tsd, false), true);
}

static quarantine_t *
quarantine_grow(tsd_t *tsd, quarantine_t *quarantine)
{
	quarantine_t *ret;

	ret = quarantine_init(tsd, quarantine->lg_maxobjs + 1);
	if (ret == NULL) {
		quarantine_drain_one(tsd, quarantine);
		return (quarantine);
	}

	ret->curbytes = quarantine->curbytes;
	ret->curobjs = quarantine->curobjs;
	if (quarantine->first + quarantine->curobjs <= (ZU(1) <<
	    quarantine->lg_maxobjs)) {
		/* objs ring buffer data are contiguous. */
		memcpy(ret->objs, &quarantine->objs[quarantine->first],
		    quarantine->curobjs * sizeof(quarantine_obj_t));
	} else {
		/* objs ring buffer data wrap around. */
		size_t ncopy_a = (ZU(1) << quarantine->lg_maxobjs) -
		    quarantine->first;
		size_t ncopy_b = quarantine->curobjs - ncopy_a;

		memcpy(ret->objs, &quarantine->objs[quarantine->first], ncopy_a
		    * sizeof(quarantine_obj_t));
		memcpy(&ret->objs[ncopy_a], quarantine->objs, ncopy_b *
		    sizeof(quarantine_obj_t));
	}
	idalloctm(tsd, quarantine, tcache_get(tsd, false), true);

	tsd_quarantine_set(tsd, ret);
	return (ret);
}

static void
quarantine_drain_one(tsd_t *tsd, quarantine_t *quarantine)
{
	quarantine_obj_t *obj = &quarantine->objs[quarantine->first];
	assert(obj->usize == isalloc(obj->ptr, config_prof));
	idalloctm(tsd, obj->ptr, NULL, false);
	quarantine->curbytes -= obj->usize;
	quarantine->curobjs--;
	quarantine->first = (quarantine->first + 1) & ((ZU(1) <<
	    quarantine->lg_maxobjs) - 1);
}

static void
quarantine_drain(tsd_t *tsd, quarantine_t *quarantine, size_t upper_bound)
{

	while (quarantine->curbytes > upper_bound && quarantine->curobjs > 0)
		quarantine_drain_one(tsd, quarantine);
}

void
quarantine(tsd_t *tsd, void *ptr)
{
	quarantine_t *quarantine;
	size_t usize = isalloc(ptr, config_prof);

	cassert(config_fill);
	assert(opt_quarantine);

	if ((quarantine = tsd_quarantine_get(tsd)) == NULL) {
		idalloctm(tsd, ptr, NULL, false);
		return;
	}
	/*
	 * Drain one or more objects if the quarantine size limit would be
	 * exceeded by appending ptr.
	 */
	if (quarantine->curbytes + usize > opt_quarantine) {
		size_t upper_bound = (opt_quarantine >= usize) ? opt_quarantine
		    - usize : 0;
		quarantine_drain(tsd, quarantine, upper_bound);
	}
	/* Grow the quarantine ring buffer if it's full. */
	if (quarantine->curobjs == (ZU(1) << quarantine->lg_maxobjs))
		quarantine = quarantine_grow(tsd, quarantine);
	/* quarantine_grow() must free a slot if it fails to grow. */
	assert(quarantine->curobjs < (ZU(1) << quarantine->lg_maxobjs));
	/* Append ptr if its size doesn't exceed the quarantine size. */
	if (quarantine->curbytes + usize <= opt_quarantine) {
		size_t offset = (quarantine->first + quarantine->curobjs) &
		    ((ZU(1) << quarantine->lg_maxobjs) - 1);
		quarantine_obj_t *obj = &quarantine->objs[offset];
		obj->ptr = ptr;
		obj->usize = usize;
		quarantine->curbytes += usize;
		quarantine->curobjs++;
		if (config_fill && unlikely(opt_junk_free)) {
			/*
			 * Only do redzone validation if Valgrind isn't in
			 * operation.
			 */
			if ((!config_valgrind || likely(!in_valgrind))
			    && usize <= SMALL_MAXCLASS)
				arena_quarantine_junk_small(ptr, usize);
			else
				memset(ptr, 0x5a, usize);
		}
	} else {
		assert(quarantine->curbytes == 0);
		idalloctm(tsd, ptr, NULL, false);
	}
}

void
quarantine_cleanup(tsd_t *tsd)
{
	quarantine_t *quarantine;

	if (!config_fill)
		return;

	quarantine = tsd_quarantine_get(tsd);
	if (quarantine != NULL) {
		quarantine_drain(tsd, quarantine, 0);
		idalloctm(tsd, quarantine, tcache_get(tsd, false), true);
		tsd_quarantine_set(tsd, NULL);
	}
}

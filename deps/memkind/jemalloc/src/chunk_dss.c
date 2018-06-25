#define	JEMALLOC_CHUNK_DSS_C_
#include "jemalloc/internal/jemalloc_internal.h"
/******************************************************************************/
/* Data. */

const char	*dss_prec_names[] = {
	"disabled",
	"primary",
	"secondary",
	"N/A"
};

/*
 * Current dss precedence default, used when creating new arenas.  NB: This is
 * stored as unsigned rather than dss_prec_t because in principle there's no
 * guarantee that sizeof(dss_prec_t) is the same as sizeof(unsigned), and we use
 * atomic operations to synchronize the setting.
 */
static unsigned		dss_prec_default = (unsigned)DSS_PREC_DEFAULT;

/* Base address of the DSS. */
static void		*dss_base;
/* Atomic boolean indicating whether the DSS is exhausted. */
static unsigned		dss_exhausted;
/* Atomic current upper limit on DSS addresses. */
static void		*dss_max;

/******************************************************************************/

static void *
chunk_dss_sbrk(intptr_t increment)
{

#ifdef JEMALLOC_DSS
	return (sbrk(increment));
#else
	not_implemented();
	return (NULL);
#endif
}

dss_prec_t
chunk_dss_prec_get(void)
{
	dss_prec_t ret;

	if (!have_dss)
		return (dss_prec_disabled);
	ret = (dss_prec_t)atomic_read_u(&dss_prec_default);
	return (ret);
}

bool
chunk_dss_prec_set(dss_prec_t dss_prec)
{

	if (!have_dss)
		return (dss_prec != dss_prec_disabled);
	atomic_write_u(&dss_prec_default, (unsigned)dss_prec);
	return (false);
}

static void *
chunk_dss_max_update(void *new_addr)
{
	void *max_cur;
	spin_t spinner;

	/*
	 * Get the current end of the DSS as max_cur and assure that dss_max is
	 * up to date.
	 */
	spin_init(&spinner);
	while (true) {
		void *max_prev = atomic_read_p(&dss_max);

		max_cur = chunk_dss_sbrk(0);
		if ((uintptr_t)max_prev > (uintptr_t)max_cur) {
			/*
			 * Another thread optimistically updated dss_max.  Wait
			 * for it to finish.
			 */
			spin_adaptive(&spinner);
			continue;
		}
		if (!atomic_cas_p(&dss_max, max_prev, max_cur))
			break;
	}
	/* Fixed new_addr can only be supported if it is at the edge of DSS. */
	if (new_addr != NULL && max_cur != new_addr)
		return (NULL);

	return (max_cur);
}

void *
chunk_alloc_dss(tsdn_t *tsdn, arena_t *arena, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit)
{
	cassert(have_dss);
	assert(size > 0 && (size & chunksize_mask) == 0);
	assert(alignment > 0 && (alignment & chunksize_mask) == 0);

	/*
	 * sbrk() uses a signed increment argument, so take care not to
	 * interpret a huge allocation request as a negative increment.
	 */
	if ((intptr_t)size < 0)
		return (NULL);

	if (!atomic_read_u(&dss_exhausted)) {
		/*
		 * The loop is necessary to recover from races with other
		 * threads that are using the DSS for something other than
		 * malloc.
		 */
		while (true) {
			void *ret, *max_cur, *dss_next, *dss_prev;
			void *gap_addr_chunk, *gap_addr_subchunk;
			size_t gap_size_chunk, gap_size_subchunk;
			intptr_t incr;

			max_cur = chunk_dss_max_update(new_addr);
			if (max_cur == NULL)
				goto label_oom;

			/*
			 * Compute how much chunk-aligned gap space (if any) is
			 * necessary to satisfy alignment.  This space can be
			 * recycled for later use.
			 */
			gap_addr_chunk = (void *)(CHUNK_CEILING(
			    (uintptr_t)max_cur));
			ret = (void *)ALIGNMENT_CEILING(
			    (uintptr_t)gap_addr_chunk, alignment);
			gap_size_chunk = (uintptr_t)ret -
			    (uintptr_t)gap_addr_chunk;
			/*
			 * Compute the address just past the end of the desired
			 * allocation space.
			 */
			dss_next = (void *)((uintptr_t)ret + size);
			if ((uintptr_t)ret < (uintptr_t)max_cur ||
			    (uintptr_t)dss_next < (uintptr_t)max_cur)
				goto label_oom; /* Wrap-around. */
			/* Compute the increment, including subchunk bytes. */
			gap_addr_subchunk = max_cur;
			gap_size_subchunk = (uintptr_t)ret -
			    (uintptr_t)gap_addr_subchunk;
			incr = gap_size_subchunk + size;

			assert((uintptr_t)max_cur + incr == (uintptr_t)ret +
			    size);

			/*
			 * Optimistically update dss_max, and roll back below if
			 * sbrk() fails.  No other thread will try to extend the
			 * DSS while dss_max is greater than the current DSS
			 * max reported by sbrk(0).
			 */
			if (atomic_cas_p(&dss_max, max_cur, dss_next))
				continue;

			/* Try to allocate. */
			dss_prev = chunk_dss_sbrk(incr);
			if (dss_prev == max_cur) {
				/* Success. */
				if (gap_size_chunk != 0) {
					chunk_hooks_t chunk_hooks =
					    CHUNK_HOOKS_INITIALIZER;
					chunk_dalloc_wrapper(tsdn, arena,
					    &chunk_hooks, gap_addr_chunk,
					    gap_size_chunk,
					    arena_extent_sn_next(arena), false,
					    true);
				}
				if (*zero) {
					JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(
					    ret, size);
					memset(ret, 0, size);
				}
				if (!*commit)
					*commit = pages_decommit(ret, size);
				return (ret);
			}

			/*
			 * Failure, whether due to OOM or a race with a raw
			 * sbrk() call from outside the allocator.  Try to roll
			 * back optimistic dss_max update; if rollback fails,
			 * it's due to another caller of this function having
			 * succeeded since this invocation started, in which
			 * case rollback is not necessary.
			 */
			atomic_cas_p(&dss_max, dss_next, max_cur);
			if (dss_prev == (void *)-1) {
				/* OOM. */
				atomic_write_u(&dss_exhausted, (unsigned)true);
				goto label_oom;
			}
		}
	}
label_oom:
	return (NULL);
}

static bool
chunk_in_dss_helper(void *chunk, void *max)
{

	return ((uintptr_t)chunk >= (uintptr_t)dss_base && (uintptr_t)chunk <
	    (uintptr_t)max);
}

bool
chunk_in_dss(void *chunk)
{

	cassert(have_dss);

	return (chunk_in_dss_helper(chunk, atomic_read_p(&dss_max)));
}

bool
chunk_dss_mergeable(void *chunk_a, void *chunk_b)
{
	void *max;

	cassert(have_dss);

	max = atomic_read_p(&dss_max);
	return (chunk_in_dss_helper(chunk_a, max) ==
	    chunk_in_dss_helper(chunk_b, max));
}

void
chunk_dss_boot(void)
{

	cassert(have_dss);

	dss_base = chunk_dss_sbrk(0);
	dss_exhausted = (unsigned)(dss_base == (void *)-1);
	dss_max = dss_base;
}

/******************************************************************************/

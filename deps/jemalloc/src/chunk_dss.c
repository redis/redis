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

/* Current dss precedence default, used when creating new arenas. */
static dss_prec_t	dss_prec_default = DSS_PREC_DEFAULT;

/*
 * Protects sbrk() calls.  This avoids malloc races among threads, though it
 * does not protect against races with threads that call sbrk() directly.
 */
static malloc_mutex_t	dss_mtx;

/* Base address of the DSS. */
static void		*dss_base;
/* Current end of the DSS, or ((void *)-1) if the DSS is exhausted. */
static void		*dss_prev;
/* Current upper limit on DSS addresses. */
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
	malloc_mutex_lock(&dss_mtx);
	ret = dss_prec_default;
	malloc_mutex_unlock(&dss_mtx);
	return (ret);
}

bool
chunk_dss_prec_set(dss_prec_t dss_prec)
{

	if (!have_dss)
		return (dss_prec != dss_prec_disabled);
	malloc_mutex_lock(&dss_mtx);
	dss_prec_default = dss_prec;
	malloc_mutex_unlock(&dss_mtx);
	return (false);
}

void *
chunk_alloc_dss(arena_t *arena, void *new_addr, size_t size, size_t alignment,
    bool *zero, bool *commit)
{
	void *ret;

	cassert(have_dss);
	assert(size > 0 && (size & chunksize_mask) == 0);
	assert(alignment > 0 && (alignment & chunksize_mask) == 0);

	/*
	 * sbrk() uses a signed increment argument, so take care not to
	 * interpret a huge allocation request as a negative increment.
	 */
	if ((intptr_t)size < 0)
		return (NULL);

	malloc_mutex_lock(&dss_mtx);
	if (dss_prev != (void *)-1) {
		size_t gap_size, cpad_size;
		void *cpad, *dss_next;
		intptr_t incr;

		/*
		 * The loop is necessary to recover from races with other
		 * threads that are using the DSS for something other than
		 * malloc.
		 */
		do {
			/* Avoid an unnecessary system call. */
			if (new_addr != NULL && dss_max != new_addr)
				break;

			/* Get the current end of the DSS. */
			dss_max = chunk_dss_sbrk(0);

			/* Make sure the earlier condition still holds. */
			if (new_addr != NULL && dss_max != new_addr)
				break;

			/*
			 * Calculate how much padding is necessary to
			 * chunk-align the end of the DSS.
			 */
			gap_size = (chunksize - CHUNK_ADDR2OFFSET(dss_max)) &
			    chunksize_mask;
			/*
			 * Compute how much chunk-aligned pad space (if any) is
			 * necessary to satisfy alignment.  This space can be
			 * recycled for later use.
			 */
			cpad = (void *)((uintptr_t)dss_max + gap_size);
			ret = (void *)ALIGNMENT_CEILING((uintptr_t)dss_max,
			    alignment);
			cpad_size = (uintptr_t)ret - (uintptr_t)cpad;
			dss_next = (void *)((uintptr_t)ret + size);
			if ((uintptr_t)ret < (uintptr_t)dss_max ||
			    (uintptr_t)dss_next < (uintptr_t)dss_max) {
				/* Wrap-around. */
				malloc_mutex_unlock(&dss_mtx);
				return (NULL);
			}
			incr = gap_size + cpad_size + size;
			dss_prev = chunk_dss_sbrk(incr);
			if (dss_prev == dss_max) {
				/* Success. */
				dss_max = dss_next;
				malloc_mutex_unlock(&dss_mtx);
				if (cpad_size != 0) {
					chunk_hooks_t chunk_hooks =
					    CHUNK_HOOKS_INITIALIZER;
					chunk_dalloc_wrapper(arena,
					    &chunk_hooks, cpad, cpad_size,
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
		} while (dss_prev != (void *)-1);
	}
	malloc_mutex_unlock(&dss_mtx);

	return (NULL);
}

bool
chunk_in_dss(void *chunk)
{
	bool ret;

	cassert(have_dss);

	malloc_mutex_lock(&dss_mtx);
	if ((uintptr_t)chunk >= (uintptr_t)dss_base
	    && (uintptr_t)chunk < (uintptr_t)dss_max)
		ret = true;
	else
		ret = false;
	malloc_mutex_unlock(&dss_mtx);

	return (ret);
}

bool
chunk_dss_boot(void)
{

	cassert(have_dss);

	if (malloc_mutex_init(&dss_mtx))
		return (true);
	dss_base = chunk_dss_sbrk(0);
	dss_prev = dss_base;
	dss_max = dss_base;

	return (false);
}

void
chunk_dss_prefork(void)
{

	if (have_dss)
		malloc_mutex_prefork(&dss_mtx);
}

void
chunk_dss_postfork_parent(void)
{

	if (have_dss)
		malloc_mutex_postfork_parent(&dss_mtx);
}

void
chunk_dss_postfork_child(void)
{

	if (have_dss)
		malloc_mutex_postfork_child(&dss_mtx);
}

/******************************************************************************/

#include "test/jemalloc_test.h"

#include "jemalloc/internal/spin.h"

static unsigned		arena_ind;
static size_t		sz;
static size_t		esz;
#define NEPOCHS		8
#define PER_THD_NALLOCS	1
static atomic_u_t	epoch;
static atomic_u_t	nfinished;

static unsigned
do_arena_create(extent_hooks_t *h) {
	unsigned arena_ind;
	size_t sz = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz,
	    (void *)(h != NULL ? &h : NULL), (h != NULL ? sizeof(h) : 0)), 0,
	    "Unexpected mallctl() failure");
	return arena_ind;
}

static void
do_arena_destroy(unsigned arena_ind) {
	size_t mib[3];
	size_t miblen;

	miblen = sizeof(mib)/sizeof(size_t);
	assert_d_eq(mallctlnametomib("arena.0.destroy", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = (size_t)arena_ind;
	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
}

static void
do_refresh(void) {
	uint64_t epoch = 1;
	assert_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch,
	    sizeof(epoch)), 0, "Unexpected mallctl() failure");
}

static size_t
do_get_size_impl(const char *cmd, unsigned arena_ind) {
	size_t mib[4];
	size_t miblen = sizeof(mib) / sizeof(size_t);
	size_t z = sizeof(size_t);

	assert_d_eq(mallctlnametomib(cmd, mib, &miblen),
	    0, "Unexpected mallctlnametomib(\"%s\", ...) failure", cmd);
	mib[2] = arena_ind;
	size_t size;
	assert_d_eq(mallctlbymib(mib, miblen, (void *)&size, &z, NULL, 0),
	    0, "Unexpected mallctlbymib([\"%s\"], ...) failure", cmd);

	return size;
}

static size_t
do_get_active(unsigned arena_ind) {
	return do_get_size_impl("stats.arenas.0.pactive", arena_ind) * PAGE;
}

static size_t
do_get_mapped(unsigned arena_ind) {
	return do_get_size_impl("stats.arenas.0.mapped", arena_ind);
}

static void *
thd_start(void *arg) {
	for (unsigned next_epoch = 1; next_epoch < NEPOCHS; next_epoch++) {
		/* Busy-wait for next epoch. */
		unsigned cur_epoch;
		spin_t spinner = SPIN_INITIALIZER;
		while ((cur_epoch = atomic_load_u(&epoch, ATOMIC_ACQUIRE)) !=
		    next_epoch) {
			spin_adaptive(&spinner);
		}
		assert_u_eq(cur_epoch, next_epoch, "Unexpected epoch");

		/*
		 * Allocate.  The main thread will reset the arena, so there's
		 * no need to deallocate.
		 */
		for (unsigned i = 0; i < PER_THD_NALLOCS; i++) {
			void *p = mallocx(sz, MALLOCX_ARENA(arena_ind) |
			    MALLOCX_TCACHE_NONE
			    );
			assert_ptr_not_null(p,
			    "Unexpected mallocx() failure\n");
		}

		/* Let the main thread know we've finished this iteration. */
		atomic_fetch_add_u(&nfinished, 1, ATOMIC_RELEASE);
	}

	return NULL;
}

TEST_BEGIN(test_retained) {
	test_skip_if(!config_stats);

	arena_ind = do_arena_create(NULL);
	sz = nallocx(HUGEPAGE, 0);
	esz = sz + sz_large_pad;

	atomic_store_u(&epoch, 0, ATOMIC_RELAXED);

	unsigned nthreads = ncpus * 2;
	VARIABLE_ARRAY(thd_t, threads, nthreads);
	for (unsigned i = 0; i < nthreads; i++) {
		thd_create(&threads[i], thd_start, NULL);
	}

	for (unsigned e = 1; e < NEPOCHS; e++) {
		atomic_store_u(&nfinished, 0, ATOMIC_RELEASE);
		atomic_store_u(&epoch, e, ATOMIC_RELEASE);

		/* Wait for threads to finish allocating. */
		spin_t spinner = SPIN_INITIALIZER;
		while (atomic_load_u(&nfinished, ATOMIC_ACQUIRE) < nthreads) {
			spin_adaptive(&spinner);
		}

		/*
		 * Assert that retained is no more than the sum of size classes
		 * that should have been used to satisfy the worker threads'
		 * requests, discounting per growth fragmentation.
		 */
		do_refresh();

		size_t allocated = esz * nthreads * PER_THD_NALLOCS;
		size_t active = do_get_active(arena_ind);
		assert_zu_le(allocated, active, "Unexpected active memory");
		size_t mapped = do_get_mapped(arena_ind);
		assert_zu_le(active, mapped, "Unexpected mapped memory");

		arena_t *arena = arena_get(tsdn_fetch(), arena_ind, false);
		size_t usable = 0;
		size_t fragmented = 0;
		for (pszind_t pind = sz_psz2ind(HUGEPAGE); pind <
		    arena->extent_grow_next; pind++) {
			size_t psz = sz_pind2sz(pind);
			size_t psz_fragmented = psz % esz;
			size_t psz_usable = psz - psz_fragmented;
			/*
			 * Only consider size classes that wouldn't be skipped.
			 */
			if (psz_usable > 0) {
				assert_zu_lt(usable, allocated,
				    "Excessive retained memory "
				    "(%#zx[+%#zx] > %#zx)", usable, psz_usable,
				    allocated);
				fragmented += psz_fragmented;
				usable += psz_usable;
			}
		}

		/*
		 * Clean up arena.  Destroying and recreating the arena
		 * is simpler that specifying extent hooks that deallocate
		 * (rather than retaining) during reset.
		 */
		do_arena_destroy(arena_ind);
		assert_u_eq(do_arena_create(NULL), arena_ind,
		    "Unexpected arena index");
	}

	for (unsigned i = 0; i < nthreads; i++) {
		thd_join(threads[i], NULL);
	}

	do_arena_destroy(arena_ind);
}
TEST_END

int
main(void) {
	return test(
	    test_retained);
}

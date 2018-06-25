#include "test/jemalloc_test.h"

/*
 * Size class that is a divisor of the page size, ideally 4+ regions per run.
 */
#if LG_PAGE <= 14
#define	SZ	(ZU(1) << (LG_PAGE - 2))
#else
#define	SZ	4096
#endif

/*
 * Number of chunks to consume at high water mark.  Should be at least 2 so that
 * if mmap()ed memory grows downward, downward growth of mmap()ed memory is
 * tested.
 */
#define	NCHUNKS	8

static unsigned
binind_compute(void)
{
	size_t sz;
	unsigned nbins, i;

	sz = sizeof(nbins);
	assert_d_eq(mallctl("arenas.nbins", (void *)&nbins, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");

	for (i = 0; i < nbins; i++) {
		size_t mib[4];
		size_t miblen = sizeof(mib)/sizeof(size_t);
		size_t size;

		assert_d_eq(mallctlnametomib("arenas.bin.0.size", mib,
		    &miblen), 0, "Unexpected mallctlnametomb failure");
		mib[2] = (size_t)i;

		sz = sizeof(size);
		assert_d_eq(mallctlbymib(mib, miblen, (void *)&size, &sz, NULL,
		    0), 0, "Unexpected mallctlbymib failure");
		if (size == SZ)
			return (i);
	}

	test_fail("Unable to compute nregs_per_run");
	return (0);
}

static size_t
nregs_per_run_compute(void)
{
	uint32_t nregs;
	size_t sz;
	unsigned binind = binind_compute();
	size_t mib[4];
	size_t miblen = sizeof(mib)/sizeof(size_t);

	assert_d_eq(mallctlnametomib("arenas.bin.0.nregs", mib, &miblen), 0,
	    "Unexpected mallctlnametomb failure");
	mib[2] = (size_t)binind;
	sz = sizeof(nregs);
	assert_d_eq(mallctlbymib(mib, miblen, (void *)&nregs, &sz, NULL,
	    0), 0, "Unexpected mallctlbymib failure");
	return (nregs);
}

static size_t
npages_per_run_compute(void)
{
	size_t sz;
	unsigned binind = binind_compute();
	size_t mib[4];
	size_t miblen = sizeof(mib)/sizeof(size_t);
	size_t run_size;

	assert_d_eq(mallctlnametomib("arenas.bin.0.run_size", mib, &miblen), 0,
	    "Unexpected mallctlnametomb failure");
	mib[2] = (size_t)binind;
	sz = sizeof(run_size);
	assert_d_eq(mallctlbymib(mib, miblen, (void *)&run_size, &sz, NULL,
	    0), 0, "Unexpected mallctlbymib failure");
	return (run_size >> LG_PAGE);
}

static size_t
npages_per_chunk_compute(void)
{

	return ((chunksize >> LG_PAGE) - map_bias);
}

static size_t
nruns_per_chunk_compute(void)
{

	return (npages_per_chunk_compute() / npages_per_run_compute());
}

static unsigned
arenas_extend_mallctl(void)
{
	unsigned arena_ind;
	size_t sz;

	sz = sizeof(arena_ind);
	assert_d_eq(mallctl("arenas.extend", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Error in arenas.extend");

	return (arena_ind);
}

static void
arena_reset_mallctl(unsigned arena_ind)
{
	size_t mib[3];
	size_t miblen = sizeof(mib)/sizeof(size_t);

	assert_d_eq(mallctlnametomib("arena.0.reset", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = (size_t)arena_ind;
	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
}

TEST_BEGIN(test_pack)
{
	unsigned arena_ind = arenas_extend_mallctl();
	size_t nregs_per_run = nregs_per_run_compute();
	size_t nruns_per_chunk = nruns_per_chunk_compute();
	size_t nruns = nruns_per_chunk * NCHUNKS;
	size_t nregs = nregs_per_run * nruns;
	VARIABLE_ARRAY(void *, ptrs, nregs);
	size_t i, j, offset;

	/* Fill matrix. */
	for (i = offset = 0; i < nruns; i++) {
		for (j = 0; j < nregs_per_run; j++) {
			void *p = mallocx(SZ, MALLOCX_ARENA(arena_ind) |
			    MALLOCX_TCACHE_NONE);
			assert_ptr_not_null(p,
			    "Unexpected mallocx(%zu, MALLOCX_ARENA(%u) |"
			    " MALLOCX_TCACHE_NONE) failure, run=%zu, reg=%zu",
			    SZ, arena_ind, i, j);
			ptrs[(i * nregs_per_run) + j] = p;
		}
	}

	/*
	 * Free all but one region of each run, but rotate which region is
	 * preserved, so that subsequent allocations exercise the within-run
	 * layout policy.
	 */
	offset = 0;
	for (i = offset = 0;
	    i < nruns;
	    i++, offset = (offset + 1) % nregs_per_run) {
		for (j = 0; j < nregs_per_run; j++) {
			void *p = ptrs[(i * nregs_per_run) + j];
			if (offset == j)
				continue;
			dallocx(p, MALLOCX_ARENA(arena_ind) |
			    MALLOCX_TCACHE_NONE);
		}
	}

	/*
	 * Logically refill matrix, skipping preserved regions and verifying
	 * that the matrix is unmodified.
	 */
	offset = 0;
	for (i = offset = 0;
	    i < nruns;
	    i++, offset = (offset + 1) % nregs_per_run) {
		for (j = 0; j < nregs_per_run; j++) {
			void *p;

			if (offset == j)
				continue;
			p = mallocx(SZ, MALLOCX_ARENA(arena_ind) |
			    MALLOCX_TCACHE_NONE);
			assert_ptr_eq(p, ptrs[(i * nregs_per_run) + j],
			    "Unexpected refill discrepancy, run=%zu, reg=%zu\n",
			    i, j);
		}
	}

	/* Clean up. */
	arena_reset_mallctl(arena_ind);
}
TEST_END

int
main(void)
{

	return (test(
	    test_pack));
}

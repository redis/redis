#include "test/jemalloc_test.h"

TEST_BEGIN(test_small_run_size)
{
	unsigned nbins, i;
	size_t sz, run_size;
	size_t mib[4];
	size_t miblen = sizeof(mib) / sizeof(size_t);

	/*
	 * Iterate over all small size classes, get their run sizes, and verify
	 * that the quantized size is the same as the run size.
	 */

	sz = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.nbins", (void *)&nbins, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");

	assert_d_eq(mallctlnametomib("arenas.bin.0.run_size", mib, &miblen), 0,
	    "Unexpected mallctlnametomib failure");
	for (i = 0; i < nbins; i++) {
		mib[2] = i;
		sz = sizeof(size_t);
		assert_d_eq(mallctlbymib(mib, miblen, (void *)&run_size, &sz,
		    NULL, 0), 0, "Unexpected mallctlbymib failure");
		assert_zu_eq(run_size, run_quantize_floor(run_size),
		    "Small run quantization should be a no-op (run_size=%zu)",
		    run_size);
		assert_zu_eq(run_size, run_quantize_ceil(run_size),
		    "Small run quantization should be a no-op (run_size=%zu)",
		    run_size);
	}
}
TEST_END

TEST_BEGIN(test_large_run_size)
{
	bool cache_oblivious;
	unsigned nlruns, i;
	size_t sz, run_size_prev, ceil_prev;
	size_t mib[4];
	size_t miblen = sizeof(mib) / sizeof(size_t);

	/*
	 * Iterate over all large size classes, get their run sizes, and verify
	 * that the quantized size is the same as the run size.
	 */

	sz = sizeof(bool);
	assert_d_eq(mallctl("config.cache_oblivious", (void *)&cache_oblivious,
	    &sz, NULL, 0), 0, "Unexpected mallctl failure");

	sz = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.nlruns", (void *)&nlruns, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");

	assert_d_eq(mallctlnametomib("arenas.lrun.0.size", mib, &miblen), 0,
	    "Unexpected mallctlnametomib failure");
	for (i = 0; i < nlruns; i++) {
		size_t lrun_size, run_size, floor, ceil;

		mib[2] = i;
		sz = sizeof(size_t);
		assert_d_eq(mallctlbymib(mib, miblen, (void *)&lrun_size, &sz,
		    NULL, 0), 0, "Unexpected mallctlbymib failure");
		run_size = cache_oblivious ? lrun_size + PAGE : lrun_size;
		floor = run_quantize_floor(run_size);
		ceil = run_quantize_ceil(run_size);

		assert_zu_eq(run_size, floor,
		    "Large run quantization should be a no-op for precise "
		    "size (lrun_size=%zu, run_size=%zu)", lrun_size, run_size);
		assert_zu_eq(run_size, ceil,
		    "Large run quantization should be a no-op for precise "
		    "size (lrun_size=%zu, run_size=%zu)", lrun_size, run_size);

		if (i > 0) {
			assert_zu_eq(run_size_prev, run_quantize_floor(run_size
			    - PAGE), "Floor should be a precise size");
			if (run_size_prev < ceil_prev) {
				assert_zu_eq(ceil_prev, run_size,
				    "Ceiling should be a precise size "
				    "(run_size_prev=%zu, ceil_prev=%zu, "
				    "run_size=%zu)", run_size_prev, ceil_prev,
				    run_size);
			}
		}
		run_size_prev = floor;
		ceil_prev = run_quantize_ceil(run_size + PAGE);
	}
}
TEST_END

TEST_BEGIN(test_monotonic)
{
	unsigned nbins, nlruns, i;
	size_t sz, floor_prev, ceil_prev;

	/*
	 * Iterate over all run sizes and verify that
	 * run_quantize_{floor,ceil}() are monotonic.
	 */

	sz = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.nbins", (void *)&nbins, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");

	sz = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.nlruns", (void *)&nlruns, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");

	floor_prev = 0;
	ceil_prev = 0;
	for (i = 1; i <= chunksize >> LG_PAGE; i++) {
		size_t run_size, floor, ceil;

		run_size = i << LG_PAGE;
		floor = run_quantize_floor(run_size);
		ceil = run_quantize_ceil(run_size);

		assert_zu_le(floor, run_size,
		    "Floor should be <= (floor=%zu, run_size=%zu, ceil=%zu)",
		    floor, run_size, ceil);
		assert_zu_ge(ceil, run_size,
		    "Ceiling should be >= (floor=%zu, run_size=%zu, ceil=%zu)",
		    floor, run_size, ceil);

		assert_zu_le(floor_prev, floor, "Floor should be monotonic "
		    "(floor_prev=%zu, floor=%zu, run_size=%zu, ceil=%zu)",
		    floor_prev, floor, run_size, ceil);
		assert_zu_le(ceil_prev, ceil, "Ceiling should be monotonic "
		    "(floor=%zu, run_size=%zu, ceil_prev=%zu, ceil=%zu)",
		    floor, run_size, ceil_prev, ceil);

		floor_prev = floor;
		ceil_prev = ceil;
	}
}
TEST_END

int
main(void)
{

	return (test(
	    test_small_run_size,
	    test_large_run_size,
	    test_monotonic));
}

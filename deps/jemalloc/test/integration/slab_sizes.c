#include "test/jemalloc_test.h"

/* Note that this test relies on the unusual slab sizes set in slab_sizes.sh. */

TEST_BEGIN(test_slab_sizes) {
	unsigned nbins;
	size_t page;
	size_t sizemib[4];
	size_t slabmib[4];
	size_t len;

	len = sizeof(nbins);
	assert_d_eq(mallctl("arenas.nbins", &nbins, &len, NULL, 0), 0,
	    "nbins mallctl failure");

	len = sizeof(page);
	assert_d_eq(mallctl("arenas.page", &page, &len, NULL, 0), 0,
	    "page mallctl failure");

	len = 4;
	assert_d_eq(mallctlnametomib("arenas.bin.0.size", sizemib, &len), 0,
	    "bin size mallctlnametomib failure");

	len = 4;
	assert_d_eq(mallctlnametomib("arenas.bin.0.slab_size", slabmib, &len),
	    0, "slab size mallctlnametomib failure");

	size_t biggest_slab_seen = 0;

	for (unsigned i = 0; i < nbins; i++) {
		size_t bin_size;
		size_t slab_size;
		len = sizeof(size_t);
		sizemib[2] = i;
		slabmib[2] = i;
		assert_d_eq(mallctlbymib(sizemib, 4, (void *)&bin_size, &len,
		    NULL, 0), 0, "bin size mallctlbymib failure");

		len = sizeof(size_t);
		assert_d_eq(mallctlbymib(slabmib, 4, (void *)&slab_size, &len,
		    NULL, 0), 0, "slab size mallctlbymib failure");

		if (bin_size < 100) {
			/*
			 * Then we should be as close to 17 as possible.  Since
			 * not all page sizes are valid (because of bitmap
			 * limitations on the number of items in a slab), we
			 * should at least make sure that the number of pages
			 * goes up.
			 */
			assert_zu_ge(slab_size, biggest_slab_seen,
			    "Slab sizes should go up");
			biggest_slab_seen = slab_size;
		} else if (
		    (100 <= bin_size && bin_size < 128)
		    || (128 < bin_size && bin_size <= 200)) {
			assert_zu_eq(slab_size, page,
			    "Forced-small slabs should be small");
		} else if (bin_size == 128) {
			assert_zu_eq(slab_size, 2 * page,
			    "Forced-2-page slab should be 2 pages");
		} else if (200 < bin_size && bin_size <= 4096) {
			assert_zu_ge(slab_size, biggest_slab_seen,
			    "Slab sizes should go up");
			biggest_slab_seen = slab_size;
		}
	}
	/*
	 * For any reasonable configuration, 17 pages should be a valid slab
	 * size for 4096-byte items.
	 */
	assert_zu_eq(biggest_slab_seen, 17 * page, "Didn't hit page target");
}
TEST_END

int
main(void) {
	return test(
	    test_slab_sizes);
}

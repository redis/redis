#include "test/jemalloc_test.h"

TEST_BEGIN(test_sz_psz2ind) {
	/*
	 * Testing page size classes which reside prior to the regular group
	 * with all size classes divisible by page size.
	 * For x86_64 Linux, it's 4096, 8192, 12288, 16384, with corresponding
	 * pszind 0, 1, 2 and 3.
	 */
	for (size_t i = 0; i < SC_NGROUP; i++) {
		for (size_t psz = i * PAGE + 1; psz <= (i + 1) * PAGE; psz++) {
			pszind_t ind = sz_psz2ind(psz);
			expect_zu_eq(ind, i, "Got %u as sz_psz2ind of %zu", ind,
			    psz);
		}
	}

	sc_data_t data;
	memset(&data, 0, sizeof(data));
	sc_data_init(&data);
	/*
	 * 'base' is the base of the first regular group with all size classes
	 * divisible by page size.
	 * For x86_64 Linux, it's 16384, and base_ind is 36.
	 */
	size_t base_psz = 1 << (SC_LG_NGROUP + LG_PAGE);
	size_t base_ind = 0;
	while (base_ind < SC_NSIZES &&
	    reg_size_compute(data.sc[base_ind].lg_base,
		data.sc[base_ind].lg_delta,
		data.sc[base_ind].ndelta) < base_psz) {
		base_ind++;
	}
	expect_zu_eq(
	    reg_size_compute(data.sc[base_ind].lg_base,
		data.sc[base_ind].lg_delta, data.sc[base_ind].ndelta),
	    base_psz, "Size class equal to %zu not found", base_psz);
	/*
	 * Test different sizes falling into groups after the 'base'. The
	 * increment is PAGE / 3 for the execution speed purpose.
	 */
	base_ind -= SC_NGROUP;
	for (size_t psz = base_psz; psz <= 64 * 1024 * 1024; psz += PAGE / 3) {
		pszind_t ind = sz_psz2ind(psz);
		sc_t gt_sc = data.sc[ind + base_ind];
		expect_zu_gt(psz,
		    reg_size_compute(gt_sc.lg_base, gt_sc.lg_delta,
			gt_sc.ndelta),
		    "Got %u as sz_psz2ind of %zu", ind, psz);
		sc_t le_sc = data.sc[ind + base_ind + 1];
		expect_zu_le(psz,
		    reg_size_compute(le_sc.lg_base, le_sc.lg_delta,
			le_sc.ndelta),
		    "Got %u as sz_psz2ind of %zu", ind, psz);
	}

	pszind_t max_ind = sz_psz2ind(SC_LARGE_MAXCLASS + 1);
	expect_lu_eq(max_ind, SC_NPSIZES,
	    "Got %u as sz_psz2ind of %llu", max_ind, SC_LARGE_MAXCLASS);
}
TEST_END

int
main(void) {
	return test(test_sz_psz2ind);
}

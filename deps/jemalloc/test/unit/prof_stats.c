#include "test/jemalloc_test.h"

#define N_PTRS 3

static void
test_combinations(szind_t ind, size_t sizes_array[N_PTRS],
    int flags_array[N_PTRS]) {
#define MALLCTL_STR_LEN 64
	assert(opt_prof && opt_prof_stats);

	char mallctl_live_str[MALLCTL_STR_LEN];
	char mallctl_accum_str[MALLCTL_STR_LEN];
	if (ind < SC_NBINS) {
		malloc_snprintf(mallctl_live_str, MALLCTL_STR_LEN,
		    "prof.stats.bins.%u.live", (unsigned)ind);
		malloc_snprintf(mallctl_accum_str, MALLCTL_STR_LEN,
		    "prof.stats.bins.%u.accum", (unsigned)ind);
	} else {
		malloc_snprintf(mallctl_live_str, MALLCTL_STR_LEN,
		    "prof.stats.lextents.%u.live", (unsigned)(ind - SC_NBINS));
		malloc_snprintf(mallctl_accum_str, MALLCTL_STR_LEN,
		    "prof.stats.lextents.%u.accum", (unsigned)(ind - SC_NBINS));
	}

	size_t stats_len = 2 * sizeof(uint64_t);

	uint64_t live_stats_orig[2];
	assert_d_eq(mallctl(mallctl_live_str, &live_stats_orig, &stats_len,
	    NULL, 0), 0, "");
	uint64_t accum_stats_orig[2];
	assert_d_eq(mallctl(mallctl_accum_str, &accum_stats_orig, &stats_len,
	    NULL, 0), 0, "");

	void *ptrs[N_PTRS];

	uint64_t live_req_sum = 0;
	uint64_t live_count = 0;
	uint64_t accum_req_sum = 0;
	uint64_t accum_count = 0;

	for (size_t i = 0; i < N_PTRS; ++i) {
		size_t sz = sizes_array[i];
		int flags = flags_array[i];
		void *p = mallocx(sz, flags);
		assert_ptr_not_null(p, "malloc() failed");
		assert(TEST_MALLOC_SIZE(p) == sz_index2size(ind));
		ptrs[i] = p;
		live_req_sum += sz;
		live_count++;
		accum_req_sum += sz;
		accum_count++;
		uint64_t live_stats[2];
		assert_d_eq(mallctl(mallctl_live_str, &live_stats, &stats_len,
		    NULL, 0), 0, "");
		expect_u64_eq(live_stats[0] - live_stats_orig[0],
		    live_req_sum, "");
		expect_u64_eq(live_stats[1] - live_stats_orig[1],
		    live_count, "");
		uint64_t accum_stats[2];
		assert_d_eq(mallctl(mallctl_accum_str, &accum_stats, &stats_len,
		    NULL, 0), 0, "");
		expect_u64_eq(accum_stats[0] - accum_stats_orig[0],
		    accum_req_sum, "");
		expect_u64_eq(accum_stats[1] - accum_stats_orig[1],
		    accum_count, "");
	}

	for (size_t i = 0; i < N_PTRS; ++i) {
		size_t sz = sizes_array[i];
		int flags = flags_array[i];
		sdallocx(ptrs[i], sz, flags);
		live_req_sum -= sz;
		live_count--;
		uint64_t live_stats[2];
		assert_d_eq(mallctl(mallctl_live_str, &live_stats, &stats_len,
		    NULL, 0), 0, "");
		expect_u64_eq(live_stats[0] - live_stats_orig[0],
		    live_req_sum, "");
		expect_u64_eq(live_stats[1] - live_stats_orig[1],
		    live_count, "");
		uint64_t accum_stats[2];
		assert_d_eq(mallctl(mallctl_accum_str, &accum_stats, &stats_len,
		    NULL, 0), 0, "");
		expect_u64_eq(accum_stats[0] - accum_stats_orig[0],
		    accum_req_sum, "");
		expect_u64_eq(accum_stats[1] - accum_stats_orig[1],
		    accum_count, "");
	}
#undef MALLCTL_STR_LEN
}

static void
test_szind_wrapper(szind_t ind) {
	size_t sizes_array[N_PTRS];
	int flags_array[N_PTRS];
	for (size_t i = 0, sz = sz_index2size(ind) - N_PTRS; i < N_PTRS;
	    ++i, ++sz) {
		sizes_array[i] = sz;
		flags_array[i] = 0;
	}
	test_combinations(ind, sizes_array, flags_array);
}

TEST_BEGIN(test_prof_stats) {
	test_skip_if(!config_prof);
	test_szind_wrapper(0);
	test_szind_wrapper(1);
	test_szind_wrapper(2);
	test_szind_wrapper(SC_NBINS);
	test_szind_wrapper(SC_NBINS + 1);
	test_szind_wrapper(SC_NBINS + 2);
}
TEST_END

static void
test_szind_aligned_wrapper(szind_t ind, unsigned lg_align) {
	size_t sizes_array[N_PTRS];
	int flags_array[N_PTRS];
	int flags = MALLOCX_LG_ALIGN(lg_align);
	for (size_t i = 0, sz = sz_index2size(ind) - N_PTRS; i < N_PTRS;
	    ++i, ++sz) {
		sizes_array[i] = sz;
		flags_array[i] = flags;
	}
	test_combinations(
	    sz_size2index(sz_sa2u(sz_index2size(ind), 1 << lg_align)),
	    sizes_array, flags_array);
}

TEST_BEGIN(test_prof_stats_aligned) {
	test_skip_if(!config_prof);
	for (szind_t ind = 0; ind < 10; ++ind) {
		for (unsigned lg_align = 0; lg_align < 10; ++lg_align) {
			test_szind_aligned_wrapper(ind, lg_align);
		}
	}
	for (szind_t ind = SC_NBINS - 5; ind < SC_NBINS + 5; ++ind) {
		for (unsigned lg_align = SC_LG_LARGE_MINCLASS - 5;
		    lg_align < SC_LG_LARGE_MINCLASS + 5; ++lg_align) {
			test_szind_aligned_wrapper(ind, lg_align);
		}
	}
}
TEST_END

int
main(void) {
	return test(
	    test_prof_stats,
	    test_prof_stats_aligned);
}

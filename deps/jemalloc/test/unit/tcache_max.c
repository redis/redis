#include "test/jemalloc_test.h"
#include "test/san.h"

const char *malloc_conf = TEST_SAN_UAF_ALIGN_DISABLE;

enum {
	alloc_option_start = 0,
	use_malloc = 0,
	use_mallocx,
	alloc_option_end
};

enum {
	dalloc_option_start = 0,
	use_free = 0,
	use_dallocx,
	use_sdallocx,
	dalloc_option_end
};

static unsigned alloc_option, dalloc_option;
static size_t tcache_max;

static void *
alloc_func(size_t sz) {
	void *ret;

	switch (alloc_option) {
	case use_malloc:
		ret = malloc(sz);
		break;
	case use_mallocx:
		ret = mallocx(sz, 0);
		break;
	default:
		unreachable();
	}
	expect_ptr_not_null(ret, "Unexpected malloc / mallocx failure");

	return ret;
}

static void
dalloc_func(void *ptr, size_t sz) {
	switch (dalloc_option) {
	case use_free:
		free(ptr);
		break;
	case use_dallocx:
		dallocx(ptr, 0);
		break;
	case use_sdallocx:
		sdallocx(ptr, sz, 0);
		break;
	default:
		unreachable();
	}
}

static size_t
tcache_bytes_read(void) {
	uint64_t epoch;
	assert_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");

	size_t tcache_bytes;
	size_t sz = sizeof(tcache_bytes);
	assert_d_eq(mallctl(
	    "stats.arenas." STRINGIFY(MALLCTL_ARENAS_ALL) ".tcache_bytes",
	    &tcache_bytes, &sz, NULL, 0), 0, "Unexpected mallctl failure");

	return tcache_bytes;
}

static void
tcache_bytes_check_update(size_t *prev, ssize_t diff) {
	size_t tcache_bytes = tcache_bytes_read();
	expect_zu_eq(tcache_bytes, *prev + diff, "tcache bytes not expected");

	*prev += diff;
}

static void
test_tcache_bytes_alloc(size_t alloc_size) {
	expect_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0), 0,
	    "Unexpected tcache flush failure");

	size_t usize = sz_s2u(alloc_size);
	/* No change is expected if usize is outside of tcache_max range. */
	bool cached = (usize <= tcache_max);
	ssize_t diff = cached ? usize : 0;

	void *ptr1 = alloc_func(alloc_size);
	void *ptr2 = alloc_func(alloc_size);

	size_t bytes = tcache_bytes_read();
	dalloc_func(ptr2, alloc_size);
	/* Expect tcache_bytes increase after dalloc */
	tcache_bytes_check_update(&bytes, diff);

	dalloc_func(ptr1, alloc_size);
	/* Expect tcache_bytes increase again */
	tcache_bytes_check_update(&bytes, diff);

	void *ptr3 = alloc_func(alloc_size);
	if (cached) {
		expect_ptr_eq(ptr1, ptr3, "Unexpected cached ptr");
	}
	/* Expect tcache_bytes decrease after alloc */
	tcache_bytes_check_update(&bytes, -diff);

	void *ptr4 = alloc_func(alloc_size);
	if (cached) {
		expect_ptr_eq(ptr2, ptr4, "Unexpected cached ptr");
	}
	/* Expect tcache_bytes decrease again */
	tcache_bytes_check_update(&bytes, -diff);

	dalloc_func(ptr3, alloc_size);
	tcache_bytes_check_update(&bytes, diff);
	dalloc_func(ptr4, alloc_size);
	tcache_bytes_check_update(&bytes, diff);
}

static void
test_tcache_max_impl(void) {
	size_t sz;
	sz = sizeof(tcache_max);
	assert_d_eq(mallctl("arenas.tcache_max", (void *)&tcache_max,
	    &sz, NULL, 0), 0, "Unexpected mallctl() failure");

	/* opt.tcache_max set to 1024 in tcache_max.sh */
	expect_zu_eq(tcache_max, 1024, "tcache_max not expected");

	test_tcache_bytes_alloc(1);
	test_tcache_bytes_alloc(tcache_max - 1);
	test_tcache_bytes_alloc(tcache_max);
	test_tcache_bytes_alloc(tcache_max + 1);

	test_tcache_bytes_alloc(PAGE - 1);
	test_tcache_bytes_alloc(PAGE);
	test_tcache_bytes_alloc(PAGE + 1);

	size_t large;
	sz = sizeof(large);
	assert_d_eq(mallctl("arenas.lextent.0.size", (void *)&large, &sz, NULL,
	    0), 0, "Unexpected mallctl() failure");

	test_tcache_bytes_alloc(large - 1);
	test_tcache_bytes_alloc(large);
	test_tcache_bytes_alloc(large + 1);
}

TEST_BEGIN(test_tcache_max) {
	test_skip_if(!config_stats);
	test_skip_if(!opt_tcache);
	test_skip_if(opt_prof);
	test_skip_if(san_uaf_detection_enabled());

	for (alloc_option = alloc_option_start;
	     alloc_option < alloc_option_end;
	     alloc_option++) {
		for (dalloc_option = dalloc_option_start;
		     dalloc_option < dalloc_option_end;
		     dalloc_option++) {
			test_tcache_max_impl();
		}
	}
}
TEST_END

int
main(void) {
	return test(test_tcache_max);
}

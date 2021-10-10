#include "test/jemalloc_test.h"

static const bool config_stats =
#ifdef JEMALLOC_STATS
    true
#else
    false
#endif
    ;

void *
thd_start(void *arg) {
	int err;
	void *p;
	uint64_t a0, a1, d0, d1;
	uint64_t *ap0, *ap1, *dp0, *dp1;
	size_t sz, usize;

	sz = sizeof(a0);
	if ((err = mallctl("thread.allocated", (void *)&a0, &sz, NULL, 0))) {
		if (err == ENOENT) {
			goto label_ENOENT;
		}
		test_fail("%s(): Error in mallctl(): %s", __func__,
		    strerror(err));
	}
	sz = sizeof(ap0);
	if ((err = mallctl("thread.allocatedp", (void *)&ap0, &sz, NULL, 0))) {
		if (err == ENOENT) {
			goto label_ENOENT;
		}
		test_fail("%s(): Error in mallctl(): %s", __func__,
		    strerror(err));
	}
	assert_u64_eq(*ap0, a0,
	    "\"thread.allocatedp\" should provide a pointer to internal "
	    "storage");

	sz = sizeof(d0);
	if ((err = mallctl("thread.deallocated", (void *)&d0, &sz, NULL, 0))) {
		if (err == ENOENT) {
			goto label_ENOENT;
		}
		test_fail("%s(): Error in mallctl(): %s", __func__,
		    strerror(err));
	}
	sz = sizeof(dp0);
	if ((err = mallctl("thread.deallocatedp", (void *)&dp0, &sz, NULL,
	    0))) {
		if (err == ENOENT) {
			goto label_ENOENT;
		}
		test_fail("%s(): Error in mallctl(): %s", __func__,
		    strerror(err));
	}
	assert_u64_eq(*dp0, d0,
	    "\"thread.deallocatedp\" should provide a pointer to internal "
	    "storage");

	p = malloc(1);
	assert_ptr_not_null(p, "Unexpected malloc() error");

	sz = sizeof(a1);
	mallctl("thread.allocated", (void *)&a1, &sz, NULL, 0);
	sz = sizeof(ap1);
	mallctl("thread.allocatedp", (void *)&ap1, &sz, NULL, 0);
	assert_u64_eq(*ap1, a1,
	    "Dereferenced \"thread.allocatedp\" value should equal "
	    "\"thread.allocated\" value");
	assert_ptr_eq(ap0, ap1,
	    "Pointer returned by \"thread.allocatedp\" should not change");

	usize = malloc_usable_size(p);
	assert_u64_le(a0 + usize, a1,
	    "Allocated memory counter should increase by at least the amount "
	    "explicitly allocated");

	free(p);

	sz = sizeof(d1);
	mallctl("thread.deallocated", (void *)&d1, &sz, NULL, 0);
	sz = sizeof(dp1);
	mallctl("thread.deallocatedp", (void *)&dp1, &sz, NULL, 0);
	assert_u64_eq(*dp1, d1,
	    "Dereferenced \"thread.deallocatedp\" value should equal "
	    "\"thread.deallocated\" value");
	assert_ptr_eq(dp0, dp1,
	    "Pointer returned by \"thread.deallocatedp\" should not change");

	assert_u64_le(d0 + usize, d1,
	    "Deallocated memory counter should increase by at least the amount "
	    "explicitly deallocated");

	return NULL;
label_ENOENT:
	assert_false(config_stats,
	    "ENOENT should only be returned if stats are disabled");
	test_skip("\"thread.allocated\" mallctl not available");
	return NULL;
}

TEST_BEGIN(test_main_thread) {
	thd_start(NULL);
}
TEST_END

TEST_BEGIN(test_subthread) {
	thd_t thd;

	thd_create(&thd, thd_start, NULL);
	thd_join(thd, NULL);
}
TEST_END

int
main(void) {
	/* Run tests multiple times to check for bad interactions. */
	return test(
	    test_main_thread,
	    test_subthread,
	    test_main_thread,
	    test_subthread,
	    test_main_thread);
}

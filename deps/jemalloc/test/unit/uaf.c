#include "test/jemalloc_test.h"
#include "test/arena_util.h"
#include "test/san.h"

#include "jemalloc/internal/cache_bin.h"
#include "jemalloc/internal/san.h"
#include "jemalloc/internal/safety_check.h"

const char *malloc_conf = TEST_SAN_UAF_ALIGN_ENABLE;

static size_t san_uaf_align;

static bool fake_abort_called;
void fake_abort(const char *message) {
	(void)message;
	fake_abort_called = true;
}

static void
test_write_after_free_pre(void) {
	safety_check_set_abort(&fake_abort);
	fake_abort_called = false;
}

static void
test_write_after_free_post(void) {
	assert_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
	    0, "Unexpected tcache flush failure");
	expect_true(fake_abort_called, "Use-after-free check didn't fire.");
	safety_check_set_abort(NULL);
}

static bool
uaf_detection_enabled(void) {
	if (!config_uaf_detection || !san_uaf_detection_enabled()) {
		return false;
	}

	ssize_t lg_san_uaf_align;
	size_t sz = sizeof(lg_san_uaf_align);
	assert_d_eq(mallctl("opt.lg_san_uaf_align", &lg_san_uaf_align, &sz,
	    NULL, 0), 0, "Unexpected mallctl failure");
	if (lg_san_uaf_align < 0) {
		return false;
	}
	assert_zd_ge(lg_san_uaf_align, LG_PAGE, "san_uaf_align out of range");
	san_uaf_align = (size_t)1 << lg_san_uaf_align;

	bool tcache_enabled;
	sz = sizeof(tcache_enabled);
	assert_d_eq(mallctl("thread.tcache.enabled", &tcache_enabled, &sz, NULL,
	    0), 0, "Unexpected mallctl failure");
	if (!tcache_enabled) {
		return false;
	}

	return true;
}

static size_t
read_tcache_stashed_bytes(unsigned arena_ind) {
	if (!config_stats) {
		return 0;
	}

	uint64_t epoch;
	assert_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");

	size_t tcache_stashed_bytes;
	size_t sz = sizeof(tcache_stashed_bytes);
	assert_d_eq(mallctl(
	    "stats.arenas." STRINGIFY(MALLCTL_ARENAS_ALL)
	    ".tcache_stashed_bytes", &tcache_stashed_bytes, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");

	return tcache_stashed_bytes;
}

static void
test_use_after_free(size_t alloc_size, bool write_after_free) {
	void *ptr = (void *)(uintptr_t)san_uaf_align;
	assert_true(cache_bin_nonfast_aligned(ptr), "Wrong alignment");
	ptr = (void *)((uintptr_t)123 * (uintptr_t)san_uaf_align);
	assert_true(cache_bin_nonfast_aligned(ptr), "Wrong alignment");
	ptr = (void *)((uintptr_t)san_uaf_align + 1);
	assert_false(cache_bin_nonfast_aligned(ptr), "Wrong alignment");

	/*
	 * Disable purging (-1) so that all dirty pages remain committed, to
	 * make use-after-free tolerable.
	 */
	unsigned arena_ind = do_arena_create(-1, -1);
	int flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;

	size_t n_max = san_uaf_align * 2;
	void **items = mallocx(n_max * sizeof(void *), flags);
	assert_ptr_not_null(items, "Unexpected mallocx failure");

	bool found = false;
	size_t iter = 0;
	char magic = 's';
	assert_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
	    0, "Unexpected tcache flush failure");
	while (!found) {
		ptr = mallocx(alloc_size, flags);
		assert_ptr_not_null(ptr, "Unexpected mallocx failure");

		found = cache_bin_nonfast_aligned(ptr);
		*(char *)ptr = magic;
		items[iter] = ptr;
		assert_zu_lt(iter++, n_max, "No aligned ptr found");
	}

	if (write_after_free) {
		test_write_after_free_pre();
	}
	bool junked = false;
	while (iter-- != 0) {
		char *volatile mem = items[iter];
		assert_c_eq(*mem, magic, "Unexpected memory content");
		size_t stashed_before = read_tcache_stashed_bytes(arena_ind);
		free(mem);
		if (*mem != magic) {
			junked = true;
			assert_c_eq(*mem, (char)uaf_detect_junk,
			    "Unexpected junk-filling bytes");
			if (write_after_free) {
				*(char *)mem = magic + 1;
			}

			size_t stashed_after = read_tcache_stashed_bytes(
			    arena_ind);
			/*
			 * An edge case is the deallocation above triggering the
			 * tcache GC event, in which case the stashed pointers
			 * may get flushed immediately, before returning from
			 * free().  Treat these cases as checked already.
			 */
			if (stashed_after <= stashed_before) {
				fake_abort_called = true;
			}
		}
		/* Flush tcache (including stashed). */
		assert_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
		    0, "Unexpected tcache flush failure");
	}
	expect_true(junked, "Aligned ptr not junked");
	if (write_after_free) {
		test_write_after_free_post();
	}

	dallocx(items, flags);
	do_arena_destroy(arena_ind);
}

TEST_BEGIN(test_read_after_free) {
	test_skip_if(!uaf_detection_enabled());

	test_use_after_free(sizeof(void *), /* write_after_free */ false);
	test_use_after_free(sizeof(void *) + 1, /* write_after_free */ false);
	test_use_after_free(16, /* write_after_free */ false);
	test_use_after_free(20, /* write_after_free */ false);
	test_use_after_free(32, /* write_after_free */ false);
	test_use_after_free(33, /* write_after_free */ false);
	test_use_after_free(48, /* write_after_free */ false);
	test_use_after_free(64, /* write_after_free */ false);
	test_use_after_free(65, /* write_after_free */ false);
	test_use_after_free(129, /* write_after_free */ false);
	test_use_after_free(255, /* write_after_free */ false);
	test_use_after_free(256, /* write_after_free */ false);
}
TEST_END

TEST_BEGIN(test_write_after_free) {
	test_skip_if(!uaf_detection_enabled());

	test_use_after_free(sizeof(void *), /* write_after_free */ true);
	test_use_after_free(sizeof(void *) + 1, /* write_after_free */ true);
	test_use_after_free(16, /* write_after_free */ true);
	test_use_after_free(20, /* write_after_free */ true);
	test_use_after_free(32, /* write_after_free */ true);
	test_use_after_free(33, /* write_after_free */ true);
	test_use_after_free(48, /* write_after_free */ true);
	test_use_after_free(64, /* write_after_free */ true);
	test_use_after_free(65, /* write_after_free */ true);
	test_use_after_free(129, /* write_after_free */ true);
	test_use_after_free(255, /* write_after_free */ true);
	test_use_after_free(256, /* write_after_free */ true);
}
TEST_END

static bool
check_allocated_intact(void **allocated, size_t n_alloc) {
	for (unsigned i = 0; i < n_alloc; i++) {
		void *ptr = *(void **)allocated[i];
		bool found = false;
		for (unsigned j = 0; j < n_alloc; j++) {
			if (ptr == allocated[j]) {
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
	}

	return true;
}

TEST_BEGIN(test_use_after_free_integration) {
	test_skip_if(!uaf_detection_enabled());

	unsigned arena_ind = do_arena_create(-1, -1);
	int flags = MALLOCX_ARENA(arena_ind);

	size_t n_alloc = san_uaf_align * 2;
	void **allocated = mallocx(n_alloc * sizeof(void *), flags);
	assert_ptr_not_null(allocated, "Unexpected mallocx failure");

	for (unsigned i = 0; i < n_alloc; i++) {
		allocated[i] = mallocx(sizeof(void *) * 8, flags);
		assert_ptr_not_null(allocated[i], "Unexpected mallocx failure");
		if (i > 0) {
			/* Emulate a circular list. */
			*(void **)allocated[i] = allocated[i - 1];
		}
	}
	*(void **)allocated[0] = allocated[n_alloc - 1];
	expect_true(check_allocated_intact(allocated, n_alloc),
	    "Allocated data corrupted");

	for (unsigned i = 0; i < n_alloc; i++) {
		free(allocated[i]);
	}
	/* Read-after-free */
	expect_false(check_allocated_intact(allocated, n_alloc),
	    "Junk-filling not detected");

	test_write_after_free_pre();
	for (unsigned i = 0; i < n_alloc; i++) {
		allocated[i] = mallocx(sizeof(void *), flags);
		assert_ptr_not_null(allocated[i], "Unexpected mallocx failure");
		*(void **)allocated[i] = (void *)(uintptr_t)i;
	}
	/* Write-after-free */
	for (unsigned i = 0; i < n_alloc; i++) {
		free(allocated[i]);
		*(void **)allocated[i] = NULL;
	}
	test_write_after_free_post();
}
TEST_END

int
main(void) {
	return test(
	    test_read_after_free,
	    test_write_after_free,
	    test_use_after_free_integration);
}

#include "test/jemalloc_test.h"

#include "jemalloc/internal/ctl.h"

static void
arena_mallctl(const char *mallctl_str, unsigned arena, void *oldp,
    size_t *oldlen, void *newp, size_t newlen) {
	int err;
	char buf[100];
	malloc_snprintf(buf, sizeof(buf), mallctl_str, arena);

	err = mallctl(buf, oldp, oldlen, newp, newlen);
	expect_d_eq(0, err, "Mallctl failed; %s", buf);
}

TEST_BEGIN(test_oversize_threshold_get_set) {
	int err;
	size_t old_threshold;
	size_t new_threshold;
	size_t threshold_sz = sizeof(old_threshold);

	unsigned arena;
	size_t arena_sz = sizeof(arena);
	err = mallctl("arenas.create", (void *)&arena, &arena_sz, NULL, 0);
	expect_d_eq(0, err, "Arena creation failed");

	/* Just a write. */
	new_threshold = 1024 * 1024;
	arena_mallctl("arena.%u.oversize_threshold", arena, NULL, NULL,
	    &new_threshold, threshold_sz);

	/* Read and write */
	new_threshold = 2 * 1024 * 1024;
	arena_mallctl("arena.%u.oversize_threshold", arena, &old_threshold,
	    &threshold_sz, &new_threshold, threshold_sz);
	expect_zu_eq(1024 * 1024, old_threshold, "Should have read old value");

	/* Just a read */
	arena_mallctl("arena.%u.oversize_threshold", arena, &old_threshold,
	    &threshold_sz, NULL, 0);
	expect_zu_eq(2 * 1024 * 1024, old_threshold, "Should have read old value");
}
TEST_END

static size_t max_purged = 0;
static bool
purge_forced_record_max(extent_hooks_t* hooks, void *addr, size_t sz,
    size_t offset, size_t length, unsigned arena_ind) {
	if (length > max_purged) {
		max_purged = length;
	}
	return false;
}

static bool
dalloc_record_max(extent_hooks_t *extent_hooks, void *addr, size_t sz,
    bool comitted, unsigned arena_ind) {
	if (sz > max_purged) {
		max_purged = sz;
	}
	return false;
}

extent_hooks_t max_recording_extent_hooks;

TEST_BEGIN(test_oversize_threshold) {
	max_recording_extent_hooks = ehooks_default_extent_hooks;
	max_recording_extent_hooks.purge_forced = &purge_forced_record_max;
	max_recording_extent_hooks.dalloc = &dalloc_record_max;

	extent_hooks_t *extent_hooks = &max_recording_extent_hooks;

	int err;

	unsigned arena;
	size_t arena_sz = sizeof(arena);
	err = mallctl("arenas.create", (void *)&arena, &arena_sz, NULL, 0);
	expect_d_eq(0, err, "Arena creation failed");
	arena_mallctl("arena.%u.extent_hooks", arena, NULL, NULL, &extent_hooks,
	    sizeof(extent_hooks));

	/*
	 * This test will fundamentally race with purging, since we're going to
	 * check the dirty stats to see if our oversized allocation got purged.
	 * We don't want other purging to happen accidentally.  We can't just
	 * disable purging entirely, though, since that will also disable
	 * oversize purging.  Just set purging intervals to be very large.
	 */
	ssize_t decay_ms = 100 * 1000;
	ssize_t decay_ms_sz = sizeof(decay_ms);
	arena_mallctl("arena.%u.dirty_decay_ms", arena, NULL, NULL, &decay_ms,
	    decay_ms_sz);
	arena_mallctl("arena.%u.muzzy_decay_ms", arena, NULL, NULL, &decay_ms,
	    decay_ms_sz);

	/* Clean everything out. */
	arena_mallctl("arena.%u.purge", arena, NULL, NULL, NULL, 0);
	max_purged = 0;

	/* Set threshold to 1MB. */
	size_t threshold = 1024 * 1024;
	size_t threshold_sz = sizeof(threshold);
	arena_mallctl("arena.%u.oversize_threshold", arena, NULL, NULL,
	    &threshold, threshold_sz);

	/* Allocating and freeing half a megabyte should leave them dirty. */
	void *ptr = mallocx(512 * 1024, MALLOCX_ARENA(arena));
	dallocx(ptr, MALLOCX_TCACHE_NONE);
	if (!is_background_thread_enabled()) {
		expect_zu_lt(max_purged, 512 * 1024, "Expected no 512k purge");
	}

	/* Purge again to reset everything out. */
	arena_mallctl("arena.%u.purge", arena, NULL, NULL, NULL, 0);
	max_purged = 0;

	/*
	 * Allocating and freeing 2 megabytes should have them purged because of
	 * the oversize threshold.
	 */
	ptr = mallocx(2 * 1024 * 1024, MALLOCX_ARENA(arena));
	dallocx(ptr, MALLOCX_TCACHE_NONE);
	expect_zu_ge(max_purged, 2 * 1024 * 1024, "Expected a 2MB purge");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_oversize_threshold_get_set,
	    test_oversize_threshold);
}


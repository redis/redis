#include "test/jemalloc_test.h"

#include "jemalloc/internal/util.h"

static arena_dalloc_junk_small_t *arena_dalloc_junk_small_orig;
static large_dalloc_junk_t *large_dalloc_junk_orig;
static large_dalloc_maybe_junk_t *large_dalloc_maybe_junk_orig;
static void *watch_for_junking;
static bool saw_junking;

static void
watch_junking(void *p) {
	watch_for_junking = p;
	saw_junking = false;
}

static void
arena_dalloc_junk_small_intercept(void *ptr, const bin_info_t *bin_info) {
	size_t i;

	arena_dalloc_junk_small_orig(ptr, bin_info);
	for (i = 0; i < bin_info->reg_size; i++) {
		assert_u_eq(((uint8_t *)ptr)[i], JEMALLOC_FREE_JUNK,
		    "Missing junk fill for byte %zu/%zu of deallocated region",
		    i, bin_info->reg_size);
	}
	if (ptr == watch_for_junking) {
		saw_junking = true;
	}
}

static void
large_dalloc_junk_intercept(void *ptr, size_t usize) {
	size_t i;

	large_dalloc_junk_orig(ptr, usize);
	for (i = 0; i < usize; i++) {
		assert_u_eq(((uint8_t *)ptr)[i], JEMALLOC_FREE_JUNK,
		    "Missing junk fill for byte %zu/%zu of deallocated region",
		    i, usize);
	}
	if (ptr == watch_for_junking) {
		saw_junking = true;
	}
}

static void
large_dalloc_maybe_junk_intercept(void *ptr, size_t usize) {
	large_dalloc_maybe_junk_orig(ptr, usize);
	if (ptr == watch_for_junking) {
		saw_junking = true;
	}
}

static void
test_junk(size_t sz_min, size_t sz_max) {
	uint8_t *s;
	size_t sz_prev, sz, i;

	if (opt_junk_free) {
		arena_dalloc_junk_small_orig = arena_dalloc_junk_small;
		arena_dalloc_junk_small = arena_dalloc_junk_small_intercept;
		large_dalloc_junk_orig = large_dalloc_junk;
		large_dalloc_junk = large_dalloc_junk_intercept;
		large_dalloc_maybe_junk_orig = large_dalloc_maybe_junk;
		large_dalloc_maybe_junk = large_dalloc_maybe_junk_intercept;
	}

	sz_prev = 0;
	s = (uint8_t *)mallocx(sz_min, 0);
	assert_ptr_not_null((void *)s, "Unexpected mallocx() failure");

	for (sz = sallocx(s, 0); sz <= sz_max;
	    sz_prev = sz, sz = sallocx(s, 0)) {
		if (sz_prev > 0) {
			assert_u_eq(s[0], 'a',
			    "Previously allocated byte %zu/%zu is corrupted",
			    ZU(0), sz_prev);
			assert_u_eq(s[sz_prev-1], 'a',
			    "Previously allocated byte %zu/%zu is corrupted",
			    sz_prev-1, sz_prev);
		}

		for (i = sz_prev; i < sz; i++) {
			if (opt_junk_alloc) {
				assert_u_eq(s[i], JEMALLOC_ALLOC_JUNK,
				    "Newly allocated byte %zu/%zu isn't "
				    "junk-filled", i, sz);
			}
			s[i] = 'a';
		}

		if (xallocx(s, sz+1, 0, 0) == sz) {
			uint8_t *t;
			watch_junking(s);
			t = (uint8_t *)rallocx(s, sz+1, 0);
			assert_ptr_not_null((void *)t,
			    "Unexpected rallocx() failure");
			assert_zu_ge(sallocx(t, 0), sz+1,
			    "Unexpectedly small rallocx() result");
			if (!background_thread_enabled()) {
				assert_ptr_ne(s, t,
				    "Unexpected in-place rallocx()");
				assert_true(!opt_junk_free || saw_junking,
				    "Expected region of size %zu to be "
				    "junk-filled", sz);
			}
			s = t;
		}
	}

	watch_junking(s);
	dallocx(s, 0);
	assert_true(!opt_junk_free || saw_junking,
	    "Expected region of size %zu to be junk-filled", sz);

	if (opt_junk_free) {
		arena_dalloc_junk_small = arena_dalloc_junk_small_orig;
		large_dalloc_junk = large_dalloc_junk_orig;
		large_dalloc_maybe_junk = large_dalloc_maybe_junk_orig;
	}
}

TEST_BEGIN(test_junk_small) {
	test_skip_if(!config_fill);
	test_junk(1, SMALL_MAXCLASS-1);
}
TEST_END

TEST_BEGIN(test_junk_large) {
	test_skip_if(!config_fill);
	test_junk(SMALL_MAXCLASS+1, (1U << (LG_LARGE_MINCLASS+1)));
}
TEST_END

int
main(void) {
	return test(
	    test_junk_small,
	    test_junk_large);
}

#include "test/jemalloc_test.h"

#ifdef JEMALLOC_FILL
#  ifndef JEMALLOC_TEST_JUNK_OPT
#    define JEMALLOC_TEST_JUNK_OPT "junk:true"
#  endif
const char *malloc_conf =
    "abort:false,zero:false,redzone:true,quarantine:0," JEMALLOC_TEST_JUNK_OPT;
#endif

static arena_dalloc_junk_small_t *arena_dalloc_junk_small_orig;
static arena_dalloc_junk_large_t *arena_dalloc_junk_large_orig;
static huge_dalloc_junk_t *huge_dalloc_junk_orig;
static void *watch_for_junking;
static bool saw_junking;

static void
watch_junking(void *p)
{

	watch_for_junking = p;
	saw_junking = false;
}

static void
arena_dalloc_junk_small_intercept(void *ptr, arena_bin_info_t *bin_info)
{
	size_t i;

	arena_dalloc_junk_small_orig(ptr, bin_info);
	for (i = 0; i < bin_info->reg_size; i++) {
		assert_u_eq(((uint8_t *)ptr)[i], JEMALLOC_FREE_JUNK,
		    "Missing junk fill for byte %zu/%zu of deallocated region",
		    i, bin_info->reg_size);
	}
	if (ptr == watch_for_junking)
		saw_junking = true;
}

static void
arena_dalloc_junk_large_intercept(void *ptr, size_t usize)
{
	size_t i;

	arena_dalloc_junk_large_orig(ptr, usize);
	for (i = 0; i < usize; i++) {
		assert_u_eq(((uint8_t *)ptr)[i], JEMALLOC_FREE_JUNK,
		    "Missing junk fill for byte %zu/%zu of deallocated region",
		    i, usize);
	}
	if (ptr == watch_for_junking)
		saw_junking = true;
}

static void
huge_dalloc_junk_intercept(void *ptr, size_t usize)
{

	huge_dalloc_junk_orig(ptr, usize);
	/*
	 * The conditions under which junk filling actually occurs are nuanced
	 * enough that it doesn't make sense to duplicate the decision logic in
	 * test code, so don't actually check that the region is junk-filled.
	 */
	if (ptr == watch_for_junking)
		saw_junking = true;
}

static void
test_junk(size_t sz_min, size_t sz_max)
{
	uint8_t *s;
	size_t sz_prev, sz, i;

	if (opt_junk_free) {
		arena_dalloc_junk_small_orig = arena_dalloc_junk_small;
		arena_dalloc_junk_small = arena_dalloc_junk_small_intercept;
		arena_dalloc_junk_large_orig = arena_dalloc_junk_large;
		arena_dalloc_junk_large = arena_dalloc_junk_large_intercept;
		huge_dalloc_junk_orig = huge_dalloc_junk;
		huge_dalloc_junk = huge_dalloc_junk_intercept;
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
			watch_junking(s);
			s = (uint8_t *)rallocx(s, sz+1, 0);
			assert_ptr_not_null((void *)s,
			    "Unexpected rallocx() failure");
			assert_true(!opt_junk_free || saw_junking,
			    "Expected region of size %zu to be junk-filled",
			    sz);
		}
	}

	watch_junking(s);
	dallocx(s, 0);
	assert_true(!opt_junk_free || saw_junking,
	    "Expected region of size %zu to be junk-filled", sz);

	if (opt_junk_free) {
		arena_dalloc_junk_small = arena_dalloc_junk_small_orig;
		arena_dalloc_junk_large = arena_dalloc_junk_large_orig;
		huge_dalloc_junk = huge_dalloc_junk_orig;
	}
}

TEST_BEGIN(test_junk_small)
{

	test_skip_if(!config_fill);
	test_junk(1, SMALL_MAXCLASS-1);
}
TEST_END

TEST_BEGIN(test_junk_large)
{

	test_skip_if(!config_fill);
	test_junk(SMALL_MAXCLASS+1, large_maxclass);
}
TEST_END

TEST_BEGIN(test_junk_huge)
{

	test_skip_if(!config_fill);
	test_junk(large_maxclass+1, chunksize*2);
}
TEST_END

arena_ralloc_junk_large_t *arena_ralloc_junk_large_orig;
static void *most_recently_trimmed;

static size_t
shrink_size(size_t size)
{
	size_t shrink_size;

	for (shrink_size = size - 1; nallocx(shrink_size, 0) == size;
	    shrink_size--)
		; /* Do nothing. */

	return (shrink_size);
}

static void
arena_ralloc_junk_large_intercept(void *ptr, size_t old_usize, size_t usize)
{

	arena_ralloc_junk_large_orig(ptr, old_usize, usize);
	assert_zu_eq(old_usize, large_maxclass, "Unexpected old_usize");
	assert_zu_eq(usize, shrink_size(large_maxclass), "Unexpected usize");
	most_recently_trimmed = ptr;
}

TEST_BEGIN(test_junk_large_ralloc_shrink)
{
	void *p1, *p2;

	p1 = mallocx(large_maxclass, 0);
	assert_ptr_not_null(p1, "Unexpected mallocx() failure");

	arena_ralloc_junk_large_orig = arena_ralloc_junk_large;
	arena_ralloc_junk_large = arena_ralloc_junk_large_intercept;

	p2 = rallocx(p1, shrink_size(large_maxclass), 0);
	assert_ptr_eq(p1, p2, "Unexpected move during shrink");

	arena_ralloc_junk_large = arena_ralloc_junk_large_orig;

	assert_ptr_eq(most_recently_trimmed, p1,
	    "Expected trimmed portion of region to be junk-filled");
}
TEST_END

static bool detected_redzone_corruption;

static void
arena_redzone_corruption_replacement(void *ptr, size_t usize, bool after,
    size_t offset, uint8_t byte)
{

	detected_redzone_corruption = true;
}

TEST_BEGIN(test_junk_redzone)
{
	char *s;
	arena_redzone_corruption_t *arena_redzone_corruption_orig;

	test_skip_if(!config_fill);
	test_skip_if(!opt_junk_alloc || !opt_junk_free);

	arena_redzone_corruption_orig = arena_redzone_corruption;
	arena_redzone_corruption = arena_redzone_corruption_replacement;

	/* Test underflow. */
	detected_redzone_corruption = false;
	s = (char *)mallocx(1, 0);
	assert_ptr_not_null((void *)s, "Unexpected mallocx() failure");
	s[-1] = 0xbb;
	dallocx(s, 0);
	assert_true(detected_redzone_corruption,
	    "Did not detect redzone corruption");

	/* Test overflow. */
	detected_redzone_corruption = false;
	s = (char *)mallocx(1, 0);
	assert_ptr_not_null((void *)s, "Unexpected mallocx() failure");
	s[sallocx(s, 0)] = 0xbb;
	dallocx(s, 0);
	assert_true(detected_redzone_corruption,
	    "Did not detect redzone corruption");

	arena_redzone_corruption = arena_redzone_corruption_orig;
}
TEST_END

int
main(void)
{

	return (test(
	    test_junk_small,
	    test_junk_large,
	    test_junk_huge,
	    test_junk_large_ralloc_shrink,
	    test_junk_redzone));
}

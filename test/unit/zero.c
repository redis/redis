#include "test/jemalloc_test.h"

static void
test_zero(size_t sz_min, size_t sz_max) {
	uint8_t *s;
	size_t sz_prev, sz, i;
#define MAGIC	((uint8_t)0x61)

	sz_prev = 0;
	s = (uint8_t *)mallocx(sz_min, 0);
	assert_ptr_not_null((void *)s, "Unexpected mallocx() failure");

	for (sz = sallocx(s, 0); sz <= sz_max;
	    sz_prev = sz, sz = sallocx(s, 0)) {
		if (sz_prev > 0) {
			assert_u_eq(s[0], MAGIC,
			    "Previously allocated byte %zu/%zu is corrupted",
			    ZU(0), sz_prev);
			assert_u_eq(s[sz_prev-1], MAGIC,
			    "Previously allocated byte %zu/%zu is corrupted",
			    sz_prev-1, sz_prev);
		}

		for (i = sz_prev; i < sz; i++) {
			assert_u_eq(s[i], 0x0,
			    "Newly allocated byte %zu/%zu isn't zero-filled",
			    i, sz);
			s[i] = MAGIC;
		}

		if (xallocx(s, sz+1, 0, 0) == sz) {
			s = (uint8_t *)rallocx(s, sz+1, 0);
			assert_ptr_not_null((void *)s,
			    "Unexpected rallocx() failure");
		}
	}

	dallocx(s, 0);
#undef MAGIC
}

TEST_BEGIN(test_zero_small) {
	test_skip_if(!config_fill);
	test_zero(1, SC_SMALL_MAXCLASS - 1);
}
TEST_END

TEST_BEGIN(test_zero_large) {
	test_skip_if(!config_fill);
	test_zero(SC_SMALL_MAXCLASS + 1, 1U << (SC_LG_LARGE_MINCLASS + 1));
}
TEST_END

int
main(void) {
	return test(
	    test_zero_small,
	    test_zero_large);
}

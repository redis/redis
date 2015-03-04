#include "test/jemalloc_test.h"

#ifdef JEMALLOC_FILL
const char *malloc_conf =
    "abort:false,junk:false,zero:true,redzone:false,quarantine:0";
#endif

static void
test_zero(size_t sz_min, size_t sz_max)
{
	char *s;
	size_t sz_prev, sz, i;

	sz_prev = 0;
	s = (char *)mallocx(sz_min, 0);
	assert_ptr_not_null((void *)s, "Unexpected mallocx() failure");

	for (sz = sallocx(s, 0); sz <= sz_max;
	    sz_prev = sz, sz = sallocx(s, 0)) {
		if (sz_prev > 0) {
			assert_c_eq(s[0], 'a',
			    "Previously allocated byte %zu/%zu is corrupted",
			    ZU(0), sz_prev);
			assert_c_eq(s[sz_prev-1], 'a',
			    "Previously allocated byte %zu/%zu is corrupted",
			    sz_prev-1, sz_prev);
		}

		for (i = sz_prev; i < sz; i++) {
			assert_c_eq(s[i], 0x0,
			    "Newly allocated byte %zu/%zu isn't zero-filled",
			    i, sz);
			s[i] = 'a';
		}

		if (xallocx(s, sz+1, 0, 0) == sz) {
			s = (char *)rallocx(s, sz+1, 0);
			assert_ptr_not_null((void *)s,
			    "Unexpected rallocx() failure");
		}
	}

	dallocx(s, 0);
}

TEST_BEGIN(test_zero_small)
{

	test_skip_if(!config_fill);
	test_zero(1, SMALL_MAXCLASS-1);
}
TEST_END

TEST_BEGIN(test_zero_large)
{

	test_skip_if(!config_fill);
	test_zero(SMALL_MAXCLASS+1, arena_maxclass);
}
TEST_END

TEST_BEGIN(test_zero_huge)
{

	test_skip_if(!config_fill);
	test_zero(arena_maxclass+1, chunksize*2);
}
TEST_END

int
main(void)
{

	return (test(
	    test_zero_small,
	    test_zero_large,
	    test_zero_huge));
}

#include "test/jemalloc_test.h"

#define	QUARANTINE_SIZE		8192
#define	STRINGIFY_HELPER(x)	#x
#define	STRINGIFY(x)		STRINGIFY_HELPER(x)

#ifdef JEMALLOC_FILL
const char *malloc_conf = "abort:false,junk:true,redzone:true,quarantine:"
    STRINGIFY(QUARANTINE_SIZE);
#endif

void
quarantine_clear(void)
{
	void *p;

	p = mallocx(QUARANTINE_SIZE*2, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	dallocx(p, 0);
}

TEST_BEGIN(test_quarantine)
{
#define	SZ		ZU(256)
#define	NQUARANTINED	(QUARANTINE_SIZE/SZ)
	void *quarantined[NQUARANTINED+1];
	size_t i, j;

	test_skip_if(!config_fill);

	assert_zu_eq(nallocx(SZ, 0), SZ,
	    "SZ=%zu does not precisely equal a size class", SZ);

	quarantine_clear();

	/*
	 * Allocate enough regions to completely fill the quarantine, plus one
	 * more.  The last iteration occurs with a completely full quarantine,
	 * but no regions should be drained from the quarantine until the last
	 * deallocation occurs.  Therefore no region recycling should occur
	 * until after this loop completes.
	 */
	for (i = 0; i < NQUARANTINED+1; i++) {
		void *p = mallocx(SZ, 0);
		assert_ptr_not_null(p, "Unexpected mallocx() failure");
		quarantined[i] = p;
		dallocx(p, 0);
		for (j = 0; j < i; j++) {
			assert_ptr_ne(p, quarantined[j],
			    "Quarantined region recycled too early; "
			    "i=%zu, j=%zu", i, j);
		}
	}
#undef NQUARANTINED
#undef SZ
}
TEST_END

static bool detected_redzone_corruption;

static void
arena_redzone_corruption_replacement(void *ptr, size_t usize, bool after,
    size_t offset, uint8_t byte)
{

	detected_redzone_corruption = true;
}

TEST_BEGIN(test_quarantine_redzone)
{
	char *s;
	arena_redzone_corruption_t *arena_redzone_corruption_orig;

	test_skip_if(!config_fill);

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
	    test_quarantine,
	    test_quarantine_redzone));
}

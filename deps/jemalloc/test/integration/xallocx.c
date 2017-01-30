#include "test/jemalloc_test.h"

#ifdef JEMALLOC_FILL
const char *malloc_conf = "junk:false";
#endif

/*
 * Use a separate arena for xallocx() extension/contraction tests so that
 * internal allocation e.g. by heap profiling can't interpose allocations where
 * xallocx() would ordinarily be able to extend.
 */
static unsigned
arena_ind(void)
{
	static unsigned ind = 0;

	if (ind == 0) {
		size_t sz = sizeof(ind);
		assert_d_eq(mallctl("arenas.extend", (void *)&ind, &sz, NULL,
		    0), 0, "Unexpected mallctl failure creating arena");
	}

	return (ind);
}

TEST_BEGIN(test_same_size)
{
	void *p;
	size_t sz, tsz;

	p = mallocx(42, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	sz = sallocx(p, 0);

	tsz = xallocx(p, sz, 0, 0);
	assert_zu_eq(tsz, sz, "Unexpected size change: %zu --> %zu", sz, tsz);

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_extra_no_move)
{
	void *p;
	size_t sz, tsz;

	p = mallocx(42, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	sz = sallocx(p, 0);

	tsz = xallocx(p, sz, sz-42, 0);
	assert_zu_eq(tsz, sz, "Unexpected size change: %zu --> %zu", sz, tsz);

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_no_move_fail)
{
	void *p;
	size_t sz, tsz;

	p = mallocx(42, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	sz = sallocx(p, 0);

	tsz = xallocx(p, sz + 5, 0, 0);
	assert_zu_eq(tsz, sz, "Unexpected size change: %zu --> %zu", sz, tsz);

	dallocx(p, 0);
}
TEST_END

static unsigned
get_nsizes_impl(const char *cmd)
{
	unsigned ret;
	size_t z;

	z = sizeof(unsigned);
	assert_d_eq(mallctl(cmd, (void *)&ret, &z, NULL, 0), 0,
	    "Unexpected mallctl(\"%s\", ...) failure", cmd);

	return (ret);
}

static unsigned
get_nsmall(void)
{

	return (get_nsizes_impl("arenas.nbins"));
}

static unsigned
get_nlarge(void)
{

	return (get_nsizes_impl("arenas.nlruns"));
}

static unsigned
get_nhuge(void)
{

	return (get_nsizes_impl("arenas.nhchunks"));
}

static size_t
get_size_impl(const char *cmd, size_t ind)
{
	size_t ret;
	size_t z;
	size_t mib[4];
	size_t miblen = 4;

	z = sizeof(size_t);
	assert_d_eq(mallctlnametomib(cmd, mib, &miblen),
	    0, "Unexpected mallctlnametomib(\"%s\", ...) failure", cmd);
	mib[2] = ind;
	z = sizeof(size_t);
	assert_d_eq(mallctlbymib(mib, miblen, (void *)&ret, &z, NULL, 0),
	    0, "Unexpected mallctlbymib([\"%s\", %zu], ...) failure", cmd, ind);

	return (ret);
}

static size_t
get_small_size(size_t ind)
{

	return (get_size_impl("arenas.bin.0.size", ind));
}

static size_t
get_large_size(size_t ind)
{

	return (get_size_impl("arenas.lrun.0.size", ind));
}

static size_t
get_huge_size(size_t ind)
{

	return (get_size_impl("arenas.hchunk.0.size", ind));
}

TEST_BEGIN(test_size)
{
	size_t small0, hugemax;
	void *p;

	/* Get size classes. */
	small0 = get_small_size(0);
	hugemax = get_huge_size(get_nhuge()-1);

	p = mallocx(small0, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() error");

	/* Test smallest supported size. */
	assert_zu_eq(xallocx(p, 1, 0, 0), small0,
	    "Unexpected xallocx() behavior");

	/* Test largest supported size. */
	assert_zu_le(xallocx(p, hugemax, 0, 0), hugemax,
	    "Unexpected xallocx() behavior");

	/* Test size overflow. */
	assert_zu_le(xallocx(p, hugemax+1, 0, 0), hugemax,
	    "Unexpected xallocx() behavior");
	assert_zu_le(xallocx(p, SIZE_T_MAX, 0, 0), hugemax,
	    "Unexpected xallocx() behavior");

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_size_extra_overflow)
{
	size_t small0, hugemax;
	void *p;

	/* Get size classes. */
	small0 = get_small_size(0);
	hugemax = get_huge_size(get_nhuge()-1);

	p = mallocx(small0, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() error");

	/* Test overflows that can be resolved by clamping extra. */
	assert_zu_le(xallocx(p, hugemax-1, 2, 0), hugemax,
	    "Unexpected xallocx() behavior");
	assert_zu_le(xallocx(p, hugemax, 1, 0), hugemax,
	    "Unexpected xallocx() behavior");

	/* Test overflow such that hugemax-size underflows. */
	assert_zu_le(xallocx(p, hugemax+1, 2, 0), hugemax,
	    "Unexpected xallocx() behavior");
	assert_zu_le(xallocx(p, hugemax+2, 3, 0), hugemax,
	    "Unexpected xallocx() behavior");
	assert_zu_le(xallocx(p, SIZE_T_MAX-2, 2, 0), hugemax,
	    "Unexpected xallocx() behavior");
	assert_zu_le(xallocx(p, SIZE_T_MAX-1, 1, 0), hugemax,
	    "Unexpected xallocx() behavior");

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_extra_small)
{
	size_t small0, small1, hugemax;
	void *p;

	/* Get size classes. */
	small0 = get_small_size(0);
	small1 = get_small_size(1);
	hugemax = get_huge_size(get_nhuge()-1);

	p = mallocx(small0, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() error");

	assert_zu_eq(xallocx(p, small1, 0, 0), small0,
	    "Unexpected xallocx() behavior");

	assert_zu_eq(xallocx(p, small1, 0, 0), small0,
	    "Unexpected xallocx() behavior");

	assert_zu_eq(xallocx(p, small0, small1 - small0, 0), small0,
	    "Unexpected xallocx() behavior");

	/* Test size+extra overflow. */
	assert_zu_eq(xallocx(p, small0, hugemax - small0 + 1, 0), small0,
	    "Unexpected xallocx() behavior");
	assert_zu_eq(xallocx(p, small0, SIZE_T_MAX - small0, 0), small0,
	    "Unexpected xallocx() behavior");

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_extra_large)
{
	int flags = MALLOCX_ARENA(arena_ind());
	size_t smallmax, large0, large1, large2, huge0, hugemax;
	void *p;

	/* Get size classes. */
	smallmax = get_small_size(get_nsmall()-1);
	large0 = get_large_size(0);
	large1 = get_large_size(1);
	large2 = get_large_size(2);
	huge0 = get_huge_size(0);
	hugemax = get_huge_size(get_nhuge()-1);

	p = mallocx(large2, flags);
	assert_ptr_not_null(p, "Unexpected mallocx() error");

	assert_zu_eq(xallocx(p, large2, 0, flags), large2,
	    "Unexpected xallocx() behavior");
	/* Test size decrease with zero extra. */
	assert_zu_eq(xallocx(p, large0, 0, flags), large0,
	    "Unexpected xallocx() behavior");
	assert_zu_eq(xallocx(p, smallmax, 0, flags), large0,
	    "Unexpected xallocx() behavior");

	assert_zu_eq(xallocx(p, large2, 0, flags), large2,
	    "Unexpected xallocx() behavior");
	/* Test size decrease with non-zero extra. */
	assert_zu_eq(xallocx(p, large0, large2 - large0, flags), large2,
	    "Unexpected xallocx() behavior");
	assert_zu_eq(xallocx(p, large1, large2 - large1, flags), large2,
	    "Unexpected xallocx() behavior");
	assert_zu_eq(xallocx(p, large0, large1 - large0, flags), large1,
	    "Unexpected xallocx() behavior");
	assert_zu_eq(xallocx(p, smallmax, large0 - smallmax, flags), large0,
	    "Unexpected xallocx() behavior");

	assert_zu_eq(xallocx(p, large0, 0, flags), large0,
	    "Unexpected xallocx() behavior");
	/* Test size increase with zero extra. */
	assert_zu_eq(xallocx(p, large2, 0, flags), large2,
	    "Unexpected xallocx() behavior");
	assert_zu_eq(xallocx(p, huge0, 0, flags), large2,
	    "Unexpected xallocx() behavior");

	assert_zu_eq(xallocx(p, large0, 0, flags), large0,
	    "Unexpected xallocx() behavior");
	/* Test size increase with non-zero extra. */
	assert_zu_lt(xallocx(p, large0, huge0 - large0, flags), huge0,
	    "Unexpected xallocx() behavior");

	assert_zu_eq(xallocx(p, large0, 0, flags), large0,
	    "Unexpected xallocx() behavior");
	/* Test size increase with non-zero extra. */
	assert_zu_eq(xallocx(p, large0, large2 - large0, flags), large2,
	    "Unexpected xallocx() behavior");

	assert_zu_eq(xallocx(p, large2, 0, flags), large2,
	    "Unexpected xallocx() behavior");
	/* Test size+extra overflow. */
	assert_zu_lt(xallocx(p, large2, hugemax - large2 + 1, flags), huge0,
	    "Unexpected xallocx() behavior");

	dallocx(p, flags);
}
TEST_END

TEST_BEGIN(test_extra_huge)
{
	int flags = MALLOCX_ARENA(arena_ind());
	size_t largemax, huge1, huge2, huge3, hugemax;
	void *p;

	/* Get size classes. */
	largemax = get_large_size(get_nlarge()-1);
	huge1 = get_huge_size(1);
	huge2 = get_huge_size(2);
	huge3 = get_huge_size(3);
	hugemax = get_huge_size(get_nhuge()-1);

	p = mallocx(huge3, flags);
	assert_ptr_not_null(p, "Unexpected mallocx() error");

	assert_zu_eq(xallocx(p, huge3, 0, flags), huge3,
	    "Unexpected xallocx() behavior");
	/* Test size decrease with zero extra. */
	assert_zu_ge(xallocx(p, huge1, 0, flags), huge1,
	    "Unexpected xallocx() behavior");
	assert_zu_ge(xallocx(p, largemax, 0, flags), huge1,
	    "Unexpected xallocx() behavior");

	assert_zu_eq(xallocx(p, huge3, 0, flags), huge3,
	    "Unexpected xallocx() behavior");
	/* Test size decrease with non-zero extra. */
	assert_zu_eq(xallocx(p, huge1, huge3 - huge1, flags), huge3,
	    "Unexpected xallocx() behavior");
	assert_zu_eq(xallocx(p, huge2, huge3 - huge2, flags), huge3,
	    "Unexpected xallocx() behavior");
	assert_zu_eq(xallocx(p, huge1, huge2 - huge1, flags), huge2,
	    "Unexpected xallocx() behavior");
	assert_zu_ge(xallocx(p, largemax, huge1 - largemax, flags), huge1,
	    "Unexpected xallocx() behavior");

	assert_zu_ge(xallocx(p, huge1, 0, flags), huge1,
	    "Unexpected xallocx() behavior");
	/* Test size increase with zero extra. */
	assert_zu_le(xallocx(p, huge3, 0, flags), huge3,
	    "Unexpected xallocx() behavior");
	assert_zu_le(xallocx(p, hugemax+1, 0, flags), huge3,
	    "Unexpected xallocx() behavior");

	assert_zu_ge(xallocx(p, huge1, 0, flags), huge1,
	    "Unexpected xallocx() behavior");
	/* Test size increase with non-zero extra. */
	assert_zu_le(xallocx(p, huge1, SIZE_T_MAX - huge1, flags), hugemax,
	    "Unexpected xallocx() behavior");

	assert_zu_ge(xallocx(p, huge1, 0, flags), huge1,
	    "Unexpected xallocx() behavior");
	/* Test size increase with non-zero extra. */
	assert_zu_le(xallocx(p, huge1, huge3 - huge1, flags), huge3,
	    "Unexpected xallocx() behavior");

	assert_zu_eq(xallocx(p, huge3, 0, flags), huge3,
	    "Unexpected xallocx() behavior");
	/* Test size+extra overflow. */
	assert_zu_le(xallocx(p, huge3, hugemax - huge3 + 1, flags), hugemax,
	    "Unexpected xallocx() behavior");

	dallocx(p, flags);
}
TEST_END

static void
print_filled_extents(const void *p, uint8_t c, size_t len)
{
	const uint8_t *pc = (const uint8_t *)p;
	size_t i, range0;
	uint8_t c0;

	malloc_printf("  p=%p, c=%#x, len=%zu:", p, c, len);
	range0 = 0;
	c0 = pc[0];
	for (i = 0; i < len; i++) {
		if (pc[i] != c0) {
			malloc_printf(" %#x[%zu..%zu)", c0, range0, i);
			range0 = i;
			c0 = pc[i];
		}
	}
	malloc_printf(" %#x[%zu..%zu)\n", c0, range0, i);
}

static bool
validate_fill(const void *p, uint8_t c, size_t offset, size_t len)
{
	const uint8_t *pc = (const uint8_t *)p;
	bool err;
	size_t i;

	for (i = offset, err = false; i < offset+len; i++) {
		if (pc[i] != c)
			err = true;
	}

	if (err)
		print_filled_extents(p, c, offset + len);

	return (err);
}

static void
test_zero(size_t szmin, size_t szmax)
{
	int flags = MALLOCX_ARENA(arena_ind()) | MALLOCX_ZERO;
	size_t sz, nsz;
	void *p;
#define	FILL_BYTE 0x7aU

	sz = szmax;
	p = mallocx(sz, flags);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	assert_false(validate_fill(p, 0x00, 0, sz), "Memory not filled: sz=%zu",
	    sz);

	/*
	 * Fill with non-zero so that non-debug builds are more likely to detect
	 * errors.
	 */
	memset(p, FILL_BYTE, sz);
	assert_false(validate_fill(p, FILL_BYTE, 0, sz),
	    "Memory not filled: sz=%zu", sz);

	/* Shrink in place so that we can expect growing in place to succeed. */
	sz = szmin;
	assert_zu_eq(xallocx(p, sz, 0, flags), sz,
	    "Unexpected xallocx() error");
	assert_false(validate_fill(p, FILL_BYTE, 0, sz),
	    "Memory not filled: sz=%zu", sz);

	for (sz = szmin; sz < szmax; sz = nsz) {
		nsz = nallocx(sz+1, flags);
		assert_zu_eq(xallocx(p, sz+1, 0, flags), nsz,
		    "Unexpected xallocx() failure");
		assert_false(validate_fill(p, FILL_BYTE, 0, sz),
		    "Memory not filled: sz=%zu", sz);
		assert_false(validate_fill(p, 0x00, sz, nsz-sz),
		    "Memory not filled: sz=%zu, nsz-sz=%zu", sz, nsz-sz);
		memset((void *)((uintptr_t)p + sz), FILL_BYTE, nsz-sz);
		assert_false(validate_fill(p, FILL_BYTE, 0, nsz),
		    "Memory not filled: nsz=%zu", nsz);
	}

	dallocx(p, flags);
}

TEST_BEGIN(test_zero_large)
{
	size_t large0, largemax;

	/* Get size classes. */
	large0 = get_large_size(0);
	largemax = get_large_size(get_nlarge()-1);

	test_zero(large0, largemax);
}
TEST_END

TEST_BEGIN(test_zero_huge)
{
	size_t huge0, huge1;

	/* Get size classes. */
	huge0 = get_huge_size(0);
	huge1 = get_huge_size(1);

	test_zero(huge1, huge0 * 2);
}
TEST_END

int
main(void)
{

	return (test(
	    test_same_size,
	    test_extra_no_move,
	    test_no_move_fail,
	    test_size,
	    test_size_extra_overflow,
	    test_extra_small,
	    test_extra_large,
	    test_extra_huge,
	    test_zero_large,
	    test_zero_huge));
}

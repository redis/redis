#include "test/jemalloc_test.h"

/*
 * Use a separate arena for xallocx() extension/contraction tests so that
 * internal allocation e.g. by heap profiling can't interpose allocations where
 * xallocx() would ordinarily be able to extend.
 */
static unsigned
arena_ind(void) {
	static unsigned ind = 0;

	if (ind == 0) {
		size_t sz = sizeof(ind);
		expect_d_eq(mallctl("arenas.create", (void *)&ind, &sz, NULL,
		    0), 0, "Unexpected mallctl failure creating arena");
	}

	return ind;
}

TEST_BEGIN(test_same_size) {
	void *p;
	size_t sz, tsz;

	p = mallocx(42, 0);
	expect_ptr_not_null(p, "Unexpected mallocx() error");
	sz = sallocx(p, 0);

	tsz = xallocx(p, sz, 0, 0);
	expect_zu_eq(tsz, sz, "Unexpected size change: %zu --> %zu", sz, tsz);

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_extra_no_move) {
	void *p;
	size_t sz, tsz;

	p = mallocx(42, 0);
	expect_ptr_not_null(p, "Unexpected mallocx() error");
	sz = sallocx(p, 0);

	tsz = xallocx(p, sz, sz-42, 0);
	expect_zu_eq(tsz, sz, "Unexpected size change: %zu --> %zu", sz, tsz);

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_no_move_fail) {
	void *p;
	size_t sz, tsz;

	p = mallocx(42, 0);
	expect_ptr_not_null(p, "Unexpected mallocx() error");
	sz = sallocx(p, 0);

	tsz = xallocx(p, sz + 5, 0, 0);
	expect_zu_eq(tsz, sz, "Unexpected size change: %zu --> %zu", sz, tsz);

	dallocx(p, 0);
}
TEST_END

static unsigned
get_nsizes_impl(const char *cmd) {
	unsigned ret;
	size_t z;

	z = sizeof(unsigned);
	expect_d_eq(mallctl(cmd, (void *)&ret, &z, NULL, 0), 0,
	    "Unexpected mallctl(\"%s\", ...) failure", cmd);

	return ret;
}

static unsigned
get_nsmall(void) {
	return get_nsizes_impl("arenas.nbins");
}

static unsigned
get_nlarge(void) {
	return get_nsizes_impl("arenas.nlextents");
}

static size_t
get_size_impl(const char *cmd, size_t ind) {
	size_t ret;
	size_t z;
	size_t mib[4];
	size_t miblen = 4;

	z = sizeof(size_t);
	expect_d_eq(mallctlnametomib(cmd, mib, &miblen),
	    0, "Unexpected mallctlnametomib(\"%s\", ...) failure", cmd);
	mib[2] = ind;
	z = sizeof(size_t);
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&ret, &z, NULL, 0),
	    0, "Unexpected mallctlbymib([\"%s\", %zu], ...) failure", cmd, ind);

	return ret;
}

static size_t
get_small_size(size_t ind) {
	return get_size_impl("arenas.bin.0.size", ind);
}

static size_t
get_large_size(size_t ind) {
	return get_size_impl("arenas.lextent.0.size", ind);
}

TEST_BEGIN(test_size) {
	size_t small0, largemax;
	void *p;

	/* Get size classes. */
	small0 = get_small_size(0);
	largemax = get_large_size(get_nlarge()-1);

	p = mallocx(small0, 0);
	expect_ptr_not_null(p, "Unexpected mallocx() error");

	/* Test smallest supported size. */
	expect_zu_eq(xallocx(p, 1, 0, 0), small0,
	    "Unexpected xallocx() behavior");

	/* Test largest supported size. */
	expect_zu_le(xallocx(p, largemax, 0, 0), largemax,
	    "Unexpected xallocx() behavior");

	/* Test size overflow. */
	expect_zu_le(xallocx(p, largemax+1, 0, 0), largemax,
	    "Unexpected xallocx() behavior");
	expect_zu_le(xallocx(p, SIZE_T_MAX, 0, 0), largemax,
	    "Unexpected xallocx() behavior");

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_size_extra_overflow) {
	size_t small0, largemax;
	void *p;

	/* Get size classes. */
	small0 = get_small_size(0);
	largemax = get_large_size(get_nlarge()-1);

	p = mallocx(small0, 0);
	expect_ptr_not_null(p, "Unexpected mallocx() error");

	/* Test overflows that can be resolved by clamping extra. */
	expect_zu_le(xallocx(p, largemax-1, 2, 0), largemax,
	    "Unexpected xallocx() behavior");
	expect_zu_le(xallocx(p, largemax, 1, 0), largemax,
	    "Unexpected xallocx() behavior");

	/* Test overflow such that largemax-size underflows. */
	expect_zu_le(xallocx(p, largemax+1, 2, 0), largemax,
	    "Unexpected xallocx() behavior");
	expect_zu_le(xallocx(p, largemax+2, 3, 0), largemax,
	    "Unexpected xallocx() behavior");
	expect_zu_le(xallocx(p, SIZE_T_MAX-2, 2, 0), largemax,
	    "Unexpected xallocx() behavior");
	expect_zu_le(xallocx(p, SIZE_T_MAX-1, 1, 0), largemax,
	    "Unexpected xallocx() behavior");

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_extra_small) {
	size_t small0, small1, largemax;
	void *p;

	/* Get size classes. */
	small0 = get_small_size(0);
	small1 = get_small_size(1);
	largemax = get_large_size(get_nlarge()-1);

	p = mallocx(small0, 0);
	expect_ptr_not_null(p, "Unexpected mallocx() error");

	expect_zu_eq(xallocx(p, small1, 0, 0), small0,
	    "Unexpected xallocx() behavior");

	expect_zu_eq(xallocx(p, small1, 0, 0), small0,
	    "Unexpected xallocx() behavior");

	expect_zu_eq(xallocx(p, small0, small1 - small0, 0), small0,
	    "Unexpected xallocx() behavior");

	/* Test size+extra overflow. */
	expect_zu_eq(xallocx(p, small0, largemax - small0 + 1, 0), small0,
	    "Unexpected xallocx() behavior");
	expect_zu_eq(xallocx(p, small0, SIZE_T_MAX - small0, 0), small0,
	    "Unexpected xallocx() behavior");

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_extra_large) {
	int flags = MALLOCX_ARENA(arena_ind());
	size_t smallmax, large1, large2, large3, largemax;
	void *p;

	/* Get size classes. */
	smallmax = get_small_size(get_nsmall()-1);
	large1 = get_large_size(1);
	large2 = get_large_size(2);
	large3 = get_large_size(3);
	largemax = get_large_size(get_nlarge()-1);

	p = mallocx(large3, flags);
	expect_ptr_not_null(p, "Unexpected mallocx() error");

	expect_zu_eq(xallocx(p, large3, 0, flags), large3,
	    "Unexpected xallocx() behavior");
	/* Test size decrease with zero extra. */
	expect_zu_ge(xallocx(p, large1, 0, flags), large1,
	    "Unexpected xallocx() behavior");
	expect_zu_ge(xallocx(p, smallmax, 0, flags), large1,
	    "Unexpected xallocx() behavior");

	if (xallocx(p, large3, 0, flags) != large3) {
		p = rallocx(p, large3, flags);
		expect_ptr_not_null(p, "Unexpected rallocx() failure");
	}
	/* Test size decrease with non-zero extra. */
	expect_zu_eq(xallocx(p, large1, large3 - large1, flags), large3,
	    "Unexpected xallocx() behavior");
	expect_zu_eq(xallocx(p, large2, large3 - large2, flags), large3,
	    "Unexpected xallocx() behavior");
	expect_zu_ge(xallocx(p, large1, large2 - large1, flags), large2,
	    "Unexpected xallocx() behavior");
	expect_zu_ge(xallocx(p, smallmax, large1 - smallmax, flags), large1,
	    "Unexpected xallocx() behavior");

	expect_zu_ge(xallocx(p, large1, 0, flags), large1,
	    "Unexpected xallocx() behavior");
	/* Test size increase with zero extra. */
	expect_zu_le(xallocx(p, large3, 0, flags), large3,
	    "Unexpected xallocx() behavior");
	expect_zu_le(xallocx(p, largemax+1, 0, flags), large3,
	    "Unexpected xallocx() behavior");

	expect_zu_ge(xallocx(p, large1, 0, flags), large1,
	    "Unexpected xallocx() behavior");
	/* Test size increase with non-zero extra. */
	expect_zu_le(xallocx(p, large1, SIZE_T_MAX - large1, flags), largemax,
	    "Unexpected xallocx() behavior");

	expect_zu_ge(xallocx(p, large1, 0, flags), large1,
	    "Unexpected xallocx() behavior");
	/* Test size increase with non-zero extra. */
	expect_zu_le(xallocx(p, large1, large3 - large1, flags), large3,
	    "Unexpected xallocx() behavior");

	if (xallocx(p, large3, 0, flags) != large3) {
		p = rallocx(p, large3, flags);
		expect_ptr_not_null(p, "Unexpected rallocx() failure");
	}
	/* Test size+extra overflow. */
	expect_zu_le(xallocx(p, large3, largemax - large3 + 1, flags), largemax,
	    "Unexpected xallocx() behavior");

	dallocx(p, flags);
}
TEST_END

static void
print_filled_extents(const void *p, uint8_t c, size_t len) {
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
validate_fill(const void *p, uint8_t c, size_t offset, size_t len) {
	const uint8_t *pc = (const uint8_t *)p;
	bool err;
	size_t i;

	for (i = offset, err = false; i < offset+len; i++) {
		if (pc[i] != c) {
			err = true;
		}
	}

	if (err) {
		print_filled_extents(p, c, offset + len);
	}

	return err;
}

static void
test_zero(size_t szmin, size_t szmax) {
	int flags = MALLOCX_ARENA(arena_ind()) | MALLOCX_ZERO;
	size_t sz, nsz;
	void *p;
#define FILL_BYTE 0x7aU

	sz = szmax;
	p = mallocx(sz, flags);
	expect_ptr_not_null(p, "Unexpected mallocx() error");
	expect_false(validate_fill(p, 0x00, 0, sz), "Memory not filled: sz=%zu",
	    sz);

	/*
	 * Fill with non-zero so that non-debug builds are more likely to detect
	 * errors.
	 */
	memset(p, FILL_BYTE, sz);
	expect_false(validate_fill(p, FILL_BYTE, 0, sz),
	    "Memory not filled: sz=%zu", sz);

	/* Shrink in place so that we can expect growing in place to succeed. */
	sz = szmin;
	if (xallocx(p, sz, 0, flags) != sz) {
		p = rallocx(p, sz, flags);
		expect_ptr_not_null(p, "Unexpected rallocx() failure");
	}
	expect_false(validate_fill(p, FILL_BYTE, 0, sz),
	    "Memory not filled: sz=%zu", sz);

	for (sz = szmin; sz < szmax; sz = nsz) {
		nsz = nallocx(sz+1, flags);
		if (xallocx(p, sz+1, 0, flags) != nsz) {
			p = rallocx(p, sz+1, flags);
			expect_ptr_not_null(p, "Unexpected rallocx() failure");
		}
		expect_false(validate_fill(p, FILL_BYTE, 0, sz),
		    "Memory not filled: sz=%zu", sz);
		expect_false(validate_fill(p, 0x00, sz, nsz-sz),
		    "Memory not filled: sz=%zu, nsz-sz=%zu", sz, nsz-sz);
		memset((void *)((uintptr_t)p + sz), FILL_BYTE, nsz-sz);
		expect_false(validate_fill(p, FILL_BYTE, 0, nsz),
		    "Memory not filled: nsz=%zu", nsz);
	}

	dallocx(p, flags);
}

TEST_BEGIN(test_zero_large) {
	size_t large0, large1;

	/* Get size classes. */
	large0 = get_large_size(0);
	large1 = get_large_size(1);

	test_zero(large1, large0 * 2);
}
TEST_END

int
main(void) {
	return test(
	    test_same_size,
	    test_extra_no_move,
	    test_no_move_fail,
	    test_size,
	    test_size_extra_overflow,
	    test_extra_small,
	    test_extra_large,
	    test_zero_large);
}

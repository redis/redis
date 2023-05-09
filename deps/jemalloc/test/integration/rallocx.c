#include "test/jemalloc_test.h"

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
get_large_size(size_t ind) {
	return get_size_impl("arenas.lextent.0.size", ind);
}

TEST_BEGIN(test_grow_and_shrink) {
	/*
	 * Use volatile to workaround buffer overflow false positives
	 * (-D_FORTIFY_SOURCE=3).
	 */
	void *volatile p, *volatile q;
	size_t tsz;
#define NCYCLES 3
	unsigned i, j;
#define NSZS 1024
	size_t szs[NSZS];
#define MAXSZ ZU(12 * 1024 * 1024)

	p = mallocx(1, 0);
	expect_ptr_not_null(p, "Unexpected mallocx() error");
	szs[0] = sallocx(p, 0);

	for (i = 0; i < NCYCLES; i++) {
		for (j = 1; j < NSZS && szs[j-1] < MAXSZ; j++) {
			q = rallocx(p, szs[j-1]+1, 0);
			expect_ptr_not_null(q,
			    "Unexpected rallocx() error for size=%zu-->%zu",
			    szs[j-1], szs[j-1]+1);
			szs[j] = sallocx(q, 0);
			expect_zu_ne(szs[j], szs[j-1]+1,
			    "Expected size to be at least: %zu", szs[j-1]+1);
			p = q;
		}

		for (j--; j > 0; j--) {
			q = rallocx(p, szs[j-1], 0);
			expect_ptr_not_null(q,
			    "Unexpected rallocx() error for size=%zu-->%zu",
			    szs[j], szs[j-1]);
			tsz = sallocx(q, 0);
			expect_zu_eq(tsz, szs[j-1],
			    "Expected size=%zu, got size=%zu", szs[j-1], tsz);
			p = q;
		}
	}

	dallocx(p, 0);
#undef MAXSZ
#undef NSZS
#undef NCYCLES
}
TEST_END

static bool
validate_fill(void *p, uint8_t c, size_t offset, size_t len) {
	bool ret = false;
	/*
	 * Use volatile to workaround buffer overflow false positives
	 * (-D_FORTIFY_SOURCE=3).
	 */
	uint8_t *volatile buf = (uint8_t *)p;
	size_t i;

	for (i = 0; i < len; i++) {
		uint8_t b = buf[offset+i];
		if (b != c) {
			test_fail("Allocation at %p (len=%zu) contains %#x "
			    "rather than %#x at offset %zu", p, len, b, c,
			    offset+i);
			ret = true;
		}
	}

	return ret;
}

TEST_BEGIN(test_zero) {
	/*
	 * Use volatile to workaround buffer overflow false positives
	 * (-D_FORTIFY_SOURCE=3).
	 */
	void *volatile p, *volatile q;
	size_t psz, qsz, i, j;
	size_t start_sizes[] = {1, 3*1024, 63*1024, 4095*1024};
#define FILL_BYTE 0xaaU
#define RANGE 2048

	for (i = 0; i < sizeof(start_sizes)/sizeof(size_t); i++) {
		size_t start_size = start_sizes[i];
		p = mallocx(start_size, MALLOCX_ZERO);
		expect_ptr_not_null(p, "Unexpected mallocx() error");
		psz = sallocx(p, 0);

		expect_false(validate_fill(p, 0, 0, psz),
		    "Expected zeroed memory");
		memset(p, FILL_BYTE, psz);
		expect_false(validate_fill(p, FILL_BYTE, 0, psz),
		    "Expected filled memory");

		for (j = 1; j < RANGE; j++) {
			q = rallocx(p, start_size+j, MALLOCX_ZERO);
			expect_ptr_not_null(q, "Unexpected rallocx() error");
			qsz = sallocx(q, 0);
			if (q != p || qsz != psz) {
				expect_false(validate_fill(q, FILL_BYTE, 0,
				    psz), "Expected filled memory");
				expect_false(validate_fill(q, 0, psz, qsz-psz),
				    "Expected zeroed memory");
			}
			if (psz != qsz) {
				memset((void *)((uintptr_t)q+psz), FILL_BYTE,
				    qsz-psz);
				psz = qsz;
			}
			p = q;
		}
		expect_false(validate_fill(p, FILL_BYTE, 0, psz),
		    "Expected filled memory");
		dallocx(p, 0);
	}
#undef FILL_BYTE
}
TEST_END

TEST_BEGIN(test_align) {
	void *p, *q;
	size_t align;
#define MAX_ALIGN (ZU(1) << 25)

	align = ZU(1);
	p = mallocx(1, MALLOCX_ALIGN(align));
	expect_ptr_not_null(p, "Unexpected mallocx() error");

	for (align <<= 1; align <= MAX_ALIGN; align <<= 1) {
		q = rallocx(p, 1, MALLOCX_ALIGN(align));
		expect_ptr_not_null(q,
		    "Unexpected rallocx() error for align=%zu", align);
		expect_ptr_null(
		    (void *)((uintptr_t)q & (align-1)),
		    "%p inadequately aligned for align=%zu",
		    q, align);
		p = q;
	}
	dallocx(p, 0);
#undef MAX_ALIGN
}
TEST_END

TEST_BEGIN(test_align_enum) {
/* Span both small sizes and large sizes. */
#define LG_MIN 12
#define LG_MAX 15
	for (size_t lg_align = LG_MIN; lg_align <= LG_MAX; ++lg_align) {
		for (size_t lg_size = LG_MIN; lg_size <= LG_MAX; ++lg_size) {
			size_t size = 1 << lg_size;
			for (size_t lg_align_next = LG_MIN;
			    lg_align_next <= LG_MAX; ++lg_align_next) {
				int flags = MALLOCX_LG_ALIGN(lg_align);
				void *p = mallocx(1, flags);
				assert_ptr_not_null(p,
				    "Unexpected mallocx() error");
				assert_zu_eq(nallocx(1, flags),
				    TEST_MALLOC_SIZE(p),
				    "Wrong mallocx() usable size");
				int flags_next =
				    MALLOCX_LG_ALIGN(lg_align_next);
				p = rallocx(p, size, flags_next);
				assert_ptr_not_null(p,
				    "Unexpected rallocx() error");
				expect_zu_eq(nallocx(size, flags_next),
				    TEST_MALLOC_SIZE(p),
				    "Wrong rallocx() usable size");
				free(p);
			}
		}
	}
#undef LG_MAX
#undef LG_MIN
}
TEST_END

TEST_BEGIN(test_lg_align_and_zero) {
	/*
	 * Use volatile to workaround buffer overflow false positives
	 * (-D_FORTIFY_SOURCE=3).
	 */
	void *volatile p, *volatile q;
	unsigned lg_align;
	size_t sz;
#define MAX_LG_ALIGN 25
#define MAX_VALIDATE (ZU(1) << 22)

	lg_align = 0;
	p = mallocx(1, MALLOCX_LG_ALIGN(lg_align)|MALLOCX_ZERO);
	expect_ptr_not_null(p, "Unexpected mallocx() error");

	for (lg_align++; lg_align <= MAX_LG_ALIGN; lg_align++) {
		q = rallocx(p, 1, MALLOCX_LG_ALIGN(lg_align)|MALLOCX_ZERO);
		expect_ptr_not_null(q,
		    "Unexpected rallocx() error for lg_align=%u", lg_align);
		expect_ptr_null(
		    (void *)((uintptr_t)q & ((ZU(1) << lg_align)-1)),
		    "%p inadequately aligned for lg_align=%u", q, lg_align);
		sz = sallocx(q, 0);
		if ((sz << 1) <= MAX_VALIDATE) {
			expect_false(validate_fill(q, 0, 0, sz),
			    "Expected zeroed memory");
		} else {
			expect_false(validate_fill(q, 0, 0, MAX_VALIDATE),
			    "Expected zeroed memory");
			expect_false(validate_fill(
			    (void *)((uintptr_t)q+sz-MAX_VALIDATE),
			    0, 0, MAX_VALIDATE), "Expected zeroed memory");
		}
		p = q;
	}
	dallocx(p, 0);
#undef MAX_VALIDATE
#undef MAX_LG_ALIGN
}
TEST_END

/*
 * GCC "-Walloc-size-larger-than" warning detects when one of the memory
 * allocation functions is called with a size larger than the maximum size that
 * they support. Here we want to explicitly test that the allocation functions
 * do indeed fail properly when this is the case, which triggers the warning.
 * Therefore we disable the warning for these tests.
 */
JEMALLOC_DIAGNOSTIC_PUSH
JEMALLOC_DIAGNOSTIC_IGNORE_ALLOC_SIZE_LARGER_THAN

TEST_BEGIN(test_overflow) {
	size_t largemax;
	void *p;

	largemax = get_large_size(get_nlarge()-1);

	p = mallocx(1, 0);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");

	expect_ptr_null(rallocx(p, largemax+1, 0),
	    "Expected OOM for rallocx(p, size=%#zx, 0)", largemax+1);

	expect_ptr_null(rallocx(p, ZU(PTRDIFF_MAX)+1, 0),
	    "Expected OOM for rallocx(p, size=%#zx, 0)", ZU(PTRDIFF_MAX)+1);

	expect_ptr_null(rallocx(p, SIZE_T_MAX, 0),
	    "Expected OOM for rallocx(p, size=%#zx, 0)", SIZE_T_MAX);

	expect_ptr_null(rallocx(p, 1, MALLOCX_ALIGN(ZU(PTRDIFF_MAX)+1)),
	    "Expected OOM for rallocx(p, size=1, MALLOCX_ALIGN(%#zx))",
	    ZU(PTRDIFF_MAX)+1);

	dallocx(p, 0);
}
TEST_END

/* Re-enable the "-Walloc-size-larger-than=" warning */
JEMALLOC_DIAGNOSTIC_POP

int
main(void) {
	return test(
	    test_grow_and_shrink,
	    test_zero,
	    test_align,
	    test_align_enum,
	    test_lg_align_and_zero,
	    test_overflow);
}

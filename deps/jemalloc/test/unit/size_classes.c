#include "test/jemalloc_test.h"

static size_t
get_max_size_class(void) {
	unsigned nlextents;
	size_t mib[4];
	size_t sz, miblen, max_size_class;

	sz = sizeof(unsigned);
	expect_d_eq(mallctl("arenas.nlextents", (void *)&nlextents, &sz, NULL,
	    0), 0, "Unexpected mallctl() error");

	miblen = sizeof(mib) / sizeof(size_t);
	expect_d_eq(mallctlnametomib("arenas.lextent.0.size", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() error");
	mib[2] = nlextents - 1;

	sz = sizeof(size_t);
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&max_size_class, &sz,
	    NULL, 0), 0, "Unexpected mallctlbymib() error");

	return max_size_class;
}

TEST_BEGIN(test_size_classes) {
	size_t size_class, max_size_class;
	szind_t index, max_index;

	max_size_class = get_max_size_class();
	max_index = sz_size2index(max_size_class);

	for (index = 0, size_class = sz_index2size(index); index < max_index ||
	    size_class < max_size_class; index++, size_class =
	    sz_index2size(index)) {
		expect_true(index < max_index,
		    "Loop conditionals should be equivalent; index=%u, "
		    "size_class=%zu (%#zx)", index, size_class, size_class);
		expect_true(size_class < max_size_class,
		    "Loop conditionals should be equivalent; index=%u, "
		    "size_class=%zu (%#zx)", index, size_class, size_class);

		expect_u_eq(index, sz_size2index(size_class),
		    "sz_size2index() does not reverse sz_index2size(): index=%u"
		    " --> size_class=%zu --> index=%u --> size_class=%zu",
		    index, size_class, sz_size2index(size_class),
		    sz_index2size(sz_size2index(size_class)));
		expect_zu_eq(size_class,
		    sz_index2size(sz_size2index(size_class)),
		    "sz_index2size() does not reverse sz_size2index(): index=%u"
		    " --> size_class=%zu --> index=%u --> size_class=%zu",
		    index, size_class, sz_size2index(size_class),
		    sz_index2size(sz_size2index(size_class)));

		expect_u_eq(index+1, sz_size2index(size_class+1),
		    "Next size_class does not round up properly");

		expect_zu_eq(size_class, (index > 0) ?
		    sz_s2u(sz_index2size(index-1)+1) : sz_s2u(1),
		    "sz_s2u() does not round up to size class");
		expect_zu_eq(size_class, sz_s2u(size_class-1),
		    "sz_s2u() does not round up to size class");
		expect_zu_eq(size_class, sz_s2u(size_class),
		    "sz_s2u() does not compute same size class");
		expect_zu_eq(sz_s2u(size_class+1), sz_index2size(index+1),
		    "sz_s2u() does not round up to next size class");
	}

	expect_u_eq(index, sz_size2index(sz_index2size(index)),
	    "sz_size2index() does not reverse sz_index2size()");
	expect_zu_eq(max_size_class, sz_index2size(
	    sz_size2index(max_size_class)),
	    "sz_index2size() does not reverse sz_size2index()");

	expect_zu_eq(size_class, sz_s2u(sz_index2size(index-1)+1),
	    "sz_s2u() does not round up to size class");
	expect_zu_eq(size_class, sz_s2u(size_class-1),
	    "sz_s2u() does not round up to size class");
	expect_zu_eq(size_class, sz_s2u(size_class),
	    "sz_s2u() does not compute same size class");
}
TEST_END

TEST_BEGIN(test_psize_classes) {
	size_t size_class, max_psz;
	pszind_t pind, max_pind;

	max_psz = get_max_size_class() + PAGE;
	max_pind = sz_psz2ind(max_psz);

	for (pind = 0, size_class = sz_pind2sz(pind);
	    pind < max_pind || size_class < max_psz;
	    pind++, size_class = sz_pind2sz(pind)) {
		expect_true(pind < max_pind,
		    "Loop conditionals should be equivalent; pind=%u, "
		    "size_class=%zu (%#zx)", pind, size_class, size_class);
		expect_true(size_class < max_psz,
		    "Loop conditionals should be equivalent; pind=%u, "
		    "size_class=%zu (%#zx)", pind, size_class, size_class);

		expect_u_eq(pind, sz_psz2ind(size_class),
		    "sz_psz2ind() does not reverse sz_pind2sz(): pind=%u -->"
		    " size_class=%zu --> pind=%u --> size_class=%zu", pind,
		    size_class, sz_psz2ind(size_class),
		    sz_pind2sz(sz_psz2ind(size_class)));
		expect_zu_eq(size_class, sz_pind2sz(sz_psz2ind(size_class)),
		    "sz_pind2sz() does not reverse sz_psz2ind(): pind=%u -->"
		    " size_class=%zu --> pind=%u --> size_class=%zu", pind,
		    size_class, sz_psz2ind(size_class),
		    sz_pind2sz(sz_psz2ind(size_class)));

		if (size_class == SC_LARGE_MAXCLASS) {
			expect_u_eq(SC_NPSIZES, sz_psz2ind(size_class + 1),
			    "Next size_class does not round up properly");
		} else {
			expect_u_eq(pind + 1, sz_psz2ind(size_class + 1),
			    "Next size_class does not round up properly");
		}

		expect_zu_eq(size_class, (pind > 0) ?
		    sz_psz2u(sz_pind2sz(pind-1)+1) : sz_psz2u(1),
		    "sz_psz2u() does not round up to size class");
		expect_zu_eq(size_class, sz_psz2u(size_class-1),
		    "sz_psz2u() does not round up to size class");
		expect_zu_eq(size_class, sz_psz2u(size_class),
		    "sz_psz2u() does not compute same size class");
		expect_zu_eq(sz_psz2u(size_class+1), sz_pind2sz(pind+1),
		    "sz_psz2u() does not round up to next size class");
	}

	expect_u_eq(pind, sz_psz2ind(sz_pind2sz(pind)),
	    "sz_psz2ind() does not reverse sz_pind2sz()");
	expect_zu_eq(max_psz, sz_pind2sz(sz_psz2ind(max_psz)),
	    "sz_pind2sz() does not reverse sz_psz2ind()");

	expect_zu_eq(size_class, sz_psz2u(sz_pind2sz(pind-1)+1),
	    "sz_psz2u() does not round up to size class");
	expect_zu_eq(size_class, sz_psz2u(size_class-1),
	    "sz_psz2u() does not round up to size class");
	expect_zu_eq(size_class, sz_psz2u(size_class),
	    "sz_psz2u() does not compute same size class");
}
TEST_END

TEST_BEGIN(test_overflow) {
	size_t max_size_class, max_psz;

	max_size_class = get_max_size_class();
	max_psz = max_size_class + PAGE;

	expect_u_eq(sz_size2index(max_size_class+1), SC_NSIZES,
	    "sz_size2index() should return NSIZES on overflow");
	expect_u_eq(sz_size2index(ZU(PTRDIFF_MAX)+1), SC_NSIZES,
	    "sz_size2index() should return NSIZES on overflow");
	expect_u_eq(sz_size2index(SIZE_T_MAX), SC_NSIZES,
	    "sz_size2index() should return NSIZES on overflow");

	expect_zu_eq(sz_s2u(max_size_class+1), 0,
	    "sz_s2u() should return 0 for unsupported size");
	expect_zu_eq(sz_s2u(ZU(PTRDIFF_MAX)+1), 0,
	    "sz_s2u() should return 0 for unsupported size");
	expect_zu_eq(sz_s2u(SIZE_T_MAX), 0,
	    "sz_s2u() should return 0 on overflow");

	expect_u_eq(sz_psz2ind(max_size_class+1), SC_NPSIZES,
	    "sz_psz2ind() should return NPSIZES on overflow");
	expect_u_eq(sz_psz2ind(ZU(PTRDIFF_MAX)+1), SC_NPSIZES,
	    "sz_psz2ind() should return NPSIZES on overflow");
	expect_u_eq(sz_psz2ind(SIZE_T_MAX), SC_NPSIZES,
	    "sz_psz2ind() should return NPSIZES on overflow");

	expect_zu_eq(sz_psz2u(max_size_class+1), max_psz,
	    "sz_psz2u() should return (LARGE_MAXCLASS + PAGE) for unsupported"
	    " size");
	expect_zu_eq(sz_psz2u(ZU(PTRDIFF_MAX)+1), max_psz,
	    "sz_psz2u() should return (LARGE_MAXCLASS + PAGE) for unsupported "
	    "size");
	expect_zu_eq(sz_psz2u(SIZE_T_MAX), max_psz,
	    "sz_psz2u() should return (LARGE_MAXCLASS + PAGE) on overflow");
}
TEST_END

int
main(void) {
	return test(
	    test_size_classes,
	    test_psize_classes,
	    test_overflow);
}

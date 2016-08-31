#include "test/jemalloc_test.h"

static unsigned
get_nsizes_impl(const char *cmd)
{
	unsigned ret;
	size_t z;

	z = sizeof(unsigned);
	assert_d_eq(mallctl(cmd, &ret, &z, NULL, 0), 0,
	    "Unexpected mallctl(\"%s\", ...) failure", cmd);

	return (ret);
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
	assert_d_eq(mallctlbymib(mib, miblen, &ret, &z, NULL, 0),
	    0, "Unexpected mallctlbymib([\"%s\", %zu], ...) failure", cmd, ind);

	return (ret);
}

static size_t
get_huge_size(size_t ind)
{

	return (get_size_impl("arenas.hchunk.0.size", ind));
}

TEST_BEGIN(test_oom)
{
	size_t hugemax, size, alignment;

	hugemax = get_huge_size(get_nhuge()-1);

	/*
	 * It should be impossible to allocate two objects that each consume
	 * more than half the virtual address space.
	 */
	{
		void *p;

		p = mallocx(hugemax, 0);
		if (p != NULL) {
			assert_ptr_null(mallocx(hugemax, 0),
			    "Expected OOM for mallocx(size=%#zx, 0)", hugemax);
			dallocx(p, 0);
		}
	}

#if LG_SIZEOF_PTR == 3
	size      = ZU(0x8000000000000000);
	alignment = ZU(0x8000000000000000);
#else
	size      = ZU(0x80000000);
	alignment = ZU(0x80000000);
#endif
	assert_ptr_null(mallocx(size, MALLOCX_ALIGN(alignment)),
	    "Expected OOM for mallocx(size=%#zx, MALLOCX_ALIGN(%#zx)", size,
	    alignment);
}
TEST_END

TEST_BEGIN(test_basic)
{
#define	MAXSZ (((size_t)1) << 26)
	size_t sz;

	for (sz = 1; sz < MAXSZ; sz = nallocx(sz, 0) + 1) {
		size_t nsz, rsz;
		void *p;
		nsz = nallocx(sz, 0);
		assert_zu_ne(nsz, 0, "Unexpected nallocx() error");
		p = mallocx(sz, 0);
		assert_ptr_not_null(p, "Unexpected mallocx() error");
		rsz = sallocx(p, 0);
		assert_zu_ge(rsz, sz, "Real size smaller than expected");
		assert_zu_eq(nsz, rsz, "nallocx()/sallocx() size mismatch");
		dallocx(p, 0);

		p = mallocx(sz, 0);
		assert_ptr_not_null(p, "Unexpected mallocx() error");
		dallocx(p, 0);

		nsz = nallocx(sz, MALLOCX_ZERO);
		assert_zu_ne(nsz, 0, "Unexpected nallocx() error");
		p = mallocx(sz, MALLOCX_ZERO);
		assert_ptr_not_null(p, "Unexpected mallocx() error");
		rsz = sallocx(p, 0);
		assert_zu_eq(nsz, rsz, "nallocx()/sallocx() rsize mismatch");
		dallocx(p, 0);
	}
#undef MAXSZ
}
TEST_END

TEST_BEGIN(test_alignment_and_size)
{
#define	MAXALIGN (((size_t)1) << 25)
#define	NITER 4
	size_t nsz, rsz, sz, alignment, total;
	unsigned i;
	void *ps[NITER];

	for (i = 0; i < NITER; i++)
		ps[i] = NULL;

	for (alignment = 8;
	    alignment <= MAXALIGN;
	    alignment <<= 1) {
		total = 0;
		for (sz = 1;
		    sz < 3 * alignment && sz < (1U << 31);
		    sz += (alignment >> (LG_SIZEOF_PTR-1)) - 1) {
			for (i = 0; i < NITER; i++) {
				nsz = nallocx(sz, MALLOCX_ALIGN(alignment) |
				    MALLOCX_ZERO);
				assert_zu_ne(nsz, 0,
				    "nallocx() error for alignment=%zu, "
				    "size=%zu (%#zx)", alignment, sz, sz);
				ps[i] = mallocx(sz, MALLOCX_ALIGN(alignment) |
				    MALLOCX_ZERO);
				assert_ptr_not_null(ps[i],
				    "mallocx() error for alignment=%zu, "
				    "size=%zu (%#zx)", alignment, sz, sz);
				rsz = sallocx(ps[i], 0);
				assert_zu_ge(rsz, sz,
				    "Real size smaller than expected for "
				    "alignment=%zu, size=%zu", alignment, sz);
				assert_zu_eq(nsz, rsz,
				    "nallocx()/sallocx() size mismatch for "
				    "alignment=%zu, size=%zu", alignment, sz);
				assert_ptr_null(
				    (void *)((uintptr_t)ps[i] & (alignment-1)),
				    "%p inadequately aligned for"
				    " alignment=%zu, size=%zu", ps[i],
				    alignment, sz);
				total += rsz;
				if (total >= (MAXALIGN << 1))
					break;
			}
			for (i = 0; i < NITER; i++) {
				if (ps[i] != NULL) {
					dallocx(ps[i], 0);
					ps[i] = NULL;
				}
			}
		}
	}
#undef MAXALIGN
#undef NITER
}
TEST_END

int
main(void)
{

	return (test(
	    test_oom,
	    test_basic,
	    test_alignment_and_size));
}

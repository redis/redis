#include "test/jemalloc_test.h"

#define	CHUNK 0x400000
#define	MAXALIGN (((size_t)1) << 25)
#define	NITER 4

TEST_BEGIN(test_basic)
{
	size_t nsz, rsz, sz;
	void *p;

	sz = 42;
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
TEST_END

TEST_BEGIN(test_alignment_and_size)
{
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
}
TEST_END

int
main(void)
{

	return (test(
	    test_basic,
	    test_alignment_and_size));
}

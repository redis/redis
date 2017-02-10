#include "test/jemalloc_test.h"

#define	MAXALIGN (((size_t)1) << 22)
#define	NITER 3

TEST_BEGIN(test_basic)
{
	void *ptr = mallocx(64, 0);
	sdallocx(ptr, 64, 0);
}
TEST_END

TEST_BEGIN(test_alignment_and_size)
{
	size_t nsz, sz, alignment, total;
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
				ps[i] = mallocx(sz, MALLOCX_ALIGN(alignment) |
				    MALLOCX_ZERO);
				total += nsz;
				if (total >= (MAXALIGN << 1))
					break;
			}
			for (i = 0; i < NITER; i++) {
				if (ps[i] != NULL) {
					sdallocx(ps[i], sz,
					    MALLOCX_ALIGN(alignment));
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

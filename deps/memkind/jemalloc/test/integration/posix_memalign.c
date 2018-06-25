#include "test/jemalloc_test.h"

#define	CHUNK 0x400000
#define	MAXALIGN (((size_t)1) << 23)

/*
 * On systems which can't merge extents, tests that call this function generate
 * a lot of dirty memory very quickly.  Purging between cycles mitigates
 * potential OOM on e.g. 32-bit Windows.
 */
static void
purge(void)
{

	assert_d_eq(mallctl("arena.0.purge", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl error");
}

TEST_BEGIN(test_alignment_errors)
{
	size_t alignment;
	void *p;

	for (alignment = 0; alignment < sizeof(void *); alignment++) {
		assert_d_eq(posix_memalign(&p, alignment, 1), EINVAL,
		    "Expected error for invalid alignment %zu",
		    alignment);
	}

	for (alignment = sizeof(size_t); alignment < MAXALIGN;
	    alignment <<= 1) {
		assert_d_ne(posix_memalign(&p, alignment + 1, 1), 0,
		    "Expected error for invalid alignment %zu",
		    alignment + 1);
	}
}
TEST_END

TEST_BEGIN(test_oom_errors)
{
	size_t alignment, size;
	void *p;

#if LG_SIZEOF_PTR == 3
	alignment = UINT64_C(0x8000000000000000);
	size      = UINT64_C(0x8000000000000000);
#else
	alignment = 0x80000000LU;
	size      = 0x80000000LU;
#endif
	assert_d_ne(posix_memalign(&p, alignment, size), 0,
	    "Expected error for posix_memalign(&p, %zu, %zu)",
	    alignment, size);

#if LG_SIZEOF_PTR == 3
	alignment = UINT64_C(0x4000000000000000);
	size      = UINT64_C(0xc000000000000001);
#else
	alignment = 0x40000000LU;
	size      = 0xc0000001LU;
#endif
	assert_d_ne(posix_memalign(&p, alignment, size), 0,
	    "Expected error for posix_memalign(&p, %zu, %zu)",
	    alignment, size);

	alignment = 0x10LU;
#if LG_SIZEOF_PTR == 3
	size = UINT64_C(0xfffffffffffffff0);
#else
	size = 0xfffffff0LU;
#endif
	assert_d_ne(posix_memalign(&p, alignment, size), 0,
	    "Expected error for posix_memalign(&p, %zu, %zu)",
	    alignment, size);
}
TEST_END

TEST_BEGIN(test_alignment_and_size)
{
#define	NITER 4
	size_t alignment, size, total;
	unsigned i;
	int err;
	void *ps[NITER];

	for (i = 0; i < NITER; i++)
		ps[i] = NULL;

	for (alignment = 8;
	    alignment <= MAXALIGN;
	    alignment <<= 1) {
		total = 0;
		for (size = 1;
		    size < 3 * alignment && size < (1U << 31);
		    size += (alignment >> (LG_SIZEOF_PTR-1)) - 1) {
			for (i = 0; i < NITER; i++) {
				err = posix_memalign(&ps[i],
				    alignment, size);
				if (err) {
					char buf[BUFERROR_BUF];

					buferror(get_errno(), buf, sizeof(buf));
					test_fail(
					    "Error for alignment=%zu, "
					    "size=%zu (%#zx): %s",
					    alignment, size, size, buf);
				}
				total += malloc_usable_size(ps[i]);
				if (total >= (MAXALIGN << 1))
					break;
			}
			for (i = 0; i < NITER; i++) {
				if (ps[i] != NULL) {
					free(ps[i]);
					ps[i] = NULL;
				}
			}
		}
		purge();
	}
#undef NITER
}
TEST_END

int
main(void)
{

	return (test(
	    test_alignment_errors,
	    test_oom_errors,
	    test_alignment_and_size));
}

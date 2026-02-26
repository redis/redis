#include "test/jemalloc_test.h"

#define MAXALIGN (((size_t)1) << 23)

/*
 * On systems which can't merge extents, tests that call this function generate
 * a lot of dirty memory very quickly.  Purging between cycles mitigates
 * potential OOM on e.g. 32-bit Windows.
 */
static void
purge(void) {
	expect_d_eq(mallctl("arena.0.purge", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl error");
}

TEST_BEGIN(test_alignment_errors) {
	size_t alignment;
	void *p;

	alignment = 0;
	set_errno(0);
	p = aligned_alloc(alignment, 1);
	expect_false(p != NULL || get_errno() != EINVAL,
	    "Expected error for invalid alignment %zu", alignment);

	for (alignment = sizeof(size_t); alignment < MAXALIGN;
	    alignment <<= 1) {
		set_errno(0);
		p = aligned_alloc(alignment + 1, 1);
		expect_false(p != NULL || get_errno() != EINVAL,
		    "Expected error for invalid alignment %zu",
		    alignment + 1);
	}
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

TEST_BEGIN(test_oom_errors) {
	size_t alignment, size;
	void *p;

#if LG_SIZEOF_PTR == 3
	alignment = UINT64_C(0x8000000000000000);
	size      = UINT64_C(0x8000000000000000);
#else
	alignment = 0x80000000LU;
	size      = 0x80000000LU;
#endif
	set_errno(0);
	p = aligned_alloc(alignment, size);
	expect_false(p != NULL || get_errno() != ENOMEM,
	    "Expected error for aligned_alloc(%zu, %zu)",
	    alignment, size);

#if LG_SIZEOF_PTR == 3
	alignment = UINT64_C(0x4000000000000000);
	size      = UINT64_C(0xc000000000000001);
#else
	alignment = 0x40000000LU;
	size      = 0xc0000001LU;
#endif
	set_errno(0);
	p = aligned_alloc(alignment, size);
	expect_false(p != NULL || get_errno() != ENOMEM,
	    "Expected error for aligned_alloc(%zu, %zu)",
	    alignment, size);

	alignment = 0x10LU;
#if LG_SIZEOF_PTR == 3
	size = UINT64_C(0xfffffffffffffff0);
#else
	size = 0xfffffff0LU;
#endif
	set_errno(0);
	p = aligned_alloc(alignment, size);
	expect_false(p != NULL || get_errno() != ENOMEM,
	    "Expected error for aligned_alloc(&p, %zu, %zu)",
	    alignment, size);
}
TEST_END

/* Re-enable the "-Walloc-size-larger-than=" warning */
JEMALLOC_DIAGNOSTIC_POP

TEST_BEGIN(test_alignment_and_size) {
#define NITER 4
	size_t alignment, size, total;
	unsigned i;
	void *ps[NITER];

	for (i = 0; i < NITER; i++) {
		ps[i] = NULL;
	}

	for (alignment = 8;
	    alignment <= MAXALIGN;
	    alignment <<= 1) {
		total = 0;
		for (size = 1;
		    size < 3 * alignment && size < (1U << 31);
		    size += (alignment >> (LG_SIZEOF_PTR-1)) - 1) {
			for (i = 0; i < NITER; i++) {
				ps[i] = aligned_alloc(alignment, size);
				if (ps[i] == NULL) {
					char buf[BUFERROR_BUF];

					buferror(get_errno(), buf, sizeof(buf));
					test_fail(
					    "Error for alignment=%zu, "
					    "size=%zu (%#zx): %s",
					    alignment, size, size, buf);
				}
				total += TEST_MALLOC_SIZE(ps[i]);
				if (total >= (MAXALIGN << 1)) {
					break;
				}
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

TEST_BEGIN(test_zero_alloc) {
	void *res = aligned_alloc(8, 0);
	assert(res);
	size_t usable = TEST_MALLOC_SIZE(res);
	assert(usable > 0);
	free(res);
}
TEST_END

int
main(void) {
	return test(
	    test_alignment_errors,
	    test_oom_errors,
	    test_alignment_and_size,
	    test_zero_alloc);
}

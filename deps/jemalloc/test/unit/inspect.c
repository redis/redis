#include "test/jemalloc_test.h"

#define TEST_UTIL_EINVAL(node, a, b, c, d, why_inval) do {		\
	assert_d_eq(mallctl("experimental.utilization." node,		\
	    a, b, c, d), EINVAL, "Should fail when " why_inval);	\
	assert_zu_eq(out_sz, out_sz_ref,				\
	    "Output size touched when given invalid arguments");	\
	assert_d_eq(memcmp(out, out_ref, out_sz_ref), 0,		\
	    "Output content touched when given invalid arguments");	\
} while (0)

#define TEST_UTIL_QUERY_EINVAL(a, b, c, d, why_inval)			\
	TEST_UTIL_EINVAL("query", a, b, c, d, why_inval)
#define TEST_UTIL_BATCH_EINVAL(a, b, c, d, why_inval)			\
	TEST_UTIL_EINVAL("batch_query", a, b, c, d, why_inval)

#define TEST_UTIL_VALID(node) do {					\
        assert_d_eq(mallctl("experimental.utilization." node,		\
	    out, &out_sz, in, in_sz), 0,				\
	    "Should return 0 on correct arguments");			\
        expect_zu_eq(out_sz, out_sz_ref, "incorrect output size");	\
	expect_d_ne(memcmp(out, out_ref, out_sz_ref), 0,		\
	    "Output content should be changed");			\
} while (0)

#define TEST_UTIL_BATCH_VALID TEST_UTIL_VALID("batch_query")

#define TEST_MAX_SIZE (1 << 20)

TEST_BEGIN(test_query) {
	size_t sz;
	/*
	 * Select some sizes that can span both small and large sizes, and are
	 * numerically unrelated to any size boundaries.
	 */
	for (sz = 7; sz <= TEST_MAX_SIZE && sz <= SC_LARGE_MAXCLASS;
	    sz += (sz <= SC_SMALL_MAXCLASS ? 1009 : 99989)) {
		void *p = mallocx(sz, 0);
		void **in = &p;
		size_t in_sz = sizeof(const void *);
		size_t out_sz = sizeof(void *) + sizeof(size_t) * 5;
		void *out = mallocx(out_sz, 0);
		void *out_ref = mallocx(out_sz, 0);
		size_t out_sz_ref = out_sz;

		assert_ptr_not_null(p,
		    "test pointer allocation failed");
		assert_ptr_not_null(out,
		    "test output allocation failed");
		assert_ptr_not_null(out_ref,
		    "test reference output allocation failed");

#define SLABCUR_READ(out) (*(void **)out)
#define COUNTS(out) ((size_t *)((void **)out + 1))
#define NFREE_READ(out) COUNTS(out)[0]
#define NREGS_READ(out) COUNTS(out)[1]
#define SIZE_READ(out) COUNTS(out)[2]
#define BIN_NFREE_READ(out) COUNTS(out)[3]
#define BIN_NREGS_READ(out) COUNTS(out)[4]

		SLABCUR_READ(out) = NULL;
		NFREE_READ(out) = NREGS_READ(out) = SIZE_READ(out) = -1;
		BIN_NFREE_READ(out) = BIN_NREGS_READ(out) = -1;
		memcpy(out_ref, out, out_sz);

		/* Test invalid argument(s) errors */
		TEST_UTIL_QUERY_EINVAL(NULL, &out_sz, in, in_sz,
		    "old is NULL");
		TEST_UTIL_QUERY_EINVAL(out, NULL, in, in_sz,
		    "oldlenp is NULL");
		TEST_UTIL_QUERY_EINVAL(out, &out_sz, NULL, in_sz,
		    "newp is NULL");
		TEST_UTIL_QUERY_EINVAL(out, &out_sz, in, 0,
		    "newlen is zero");
		in_sz -= 1;
		TEST_UTIL_QUERY_EINVAL(out, &out_sz, in, in_sz,
		    "invalid newlen");
		in_sz += 1;
		out_sz_ref = out_sz -= 2 * sizeof(size_t);
		TEST_UTIL_QUERY_EINVAL(out, &out_sz, in, in_sz,
		    "invalid *oldlenp");
		out_sz_ref = out_sz += 2 * sizeof(size_t);

		/* Examine output for valid call */
		TEST_UTIL_VALID("query");
		expect_zu_le(sz, SIZE_READ(out),
		    "Extent size should be at least allocation size");
		expect_zu_eq(SIZE_READ(out) & (PAGE - 1), 0,
		    "Extent size should be a multiple of page size");

		/*
		 * We don't do much bin checking if prof is on, since profiling
		 * can produce extents that are for small size classes but not
		 * slabs, which interferes with things like region counts.
		 */
		if (!opt_prof && sz <= SC_SMALL_MAXCLASS) {
			expect_zu_le(NFREE_READ(out), NREGS_READ(out),
			    "Extent free count exceeded region count");
			expect_zu_le(NREGS_READ(out), SIZE_READ(out),
			    "Extent region count exceeded size");
			expect_zu_ne(NREGS_READ(out), 0,
			    "Extent region count must be positive");
			expect_true(NFREE_READ(out) == 0 || (SLABCUR_READ(out)
			    != NULL && SLABCUR_READ(out) <= p),
			    "Allocation should follow first fit principle");

			if (config_stats) {
				expect_zu_le(BIN_NFREE_READ(out),
				    BIN_NREGS_READ(out),
				    "Bin free count exceeded region count");
				expect_zu_ne(BIN_NREGS_READ(out), 0,
				    "Bin region count must be positive");
				expect_zu_le(NFREE_READ(out),
				    BIN_NFREE_READ(out),
				    "Extent free count exceeded bin free count");
				expect_zu_le(NREGS_READ(out),
				    BIN_NREGS_READ(out),
				    "Extent region count exceeded "
				    "bin region count");
				expect_zu_eq(BIN_NREGS_READ(out)
				    % NREGS_READ(out), 0,
				    "Bin region count isn't a multiple of "
				    "extent region count");
				expect_zu_le(
				    BIN_NFREE_READ(out) - NFREE_READ(out),
				    BIN_NREGS_READ(out) - NREGS_READ(out),
				    "Free count in other extents in the bin "
				    "exceeded region count in other extents "
				    "in the bin");
				expect_zu_le(NREGS_READ(out) - NFREE_READ(out),
				    BIN_NREGS_READ(out) - BIN_NFREE_READ(out),
				    "Extent utilized count exceeded "
				    "bin utilized count");
			}
		} else if (sz > SC_SMALL_MAXCLASS) {
			expect_zu_eq(NFREE_READ(out), 0,
			    "Extent free count should be zero");
			expect_zu_eq(NREGS_READ(out), 1,
			    "Extent region count should be one");
			expect_ptr_null(SLABCUR_READ(out),
			    "Current slab must be null for large size classes");
			if (config_stats) {
				expect_zu_eq(BIN_NFREE_READ(out), 0,
				    "Bin free count must be zero for "
				    "large sizes");
				expect_zu_eq(BIN_NREGS_READ(out), 0,
				    "Bin region count must be zero for "
				    "large sizes");
			}
		}

#undef BIN_NREGS_READ
#undef BIN_NFREE_READ
#undef SIZE_READ
#undef NREGS_READ
#undef NFREE_READ
#undef COUNTS
#undef SLABCUR_READ

		free(out_ref);
		free(out);
		free(p);
	}
}
TEST_END

TEST_BEGIN(test_batch) {
	size_t sz;
	/*
	 * Select some sizes that can span both small and large sizes, and are
	 * numerically unrelated to any size boundaries.
	 */
	for (sz = 17; sz <= TEST_MAX_SIZE && sz <= SC_LARGE_MAXCLASS;
	    sz += (sz <= SC_SMALL_MAXCLASS ? 1019 : 99991)) {
		void *p = mallocx(sz, 0);
		void *q = mallocx(sz, 0);
		void *in[] = {p, q};
		size_t in_sz = sizeof(const void *) * 2;
		size_t out[] = {-1, -1, -1, -1, -1, -1};
		size_t out_sz = sizeof(size_t) * 6;
		size_t out_ref[] = {-1, -1, -1, -1, -1, -1};
		size_t out_sz_ref = out_sz;

		assert_ptr_not_null(p, "test pointer allocation failed");
		assert_ptr_not_null(q, "test pointer allocation failed");

		/* Test invalid argument(s) errors */
		TEST_UTIL_BATCH_EINVAL(NULL, &out_sz, in, in_sz,
		    "old is NULL");
		TEST_UTIL_BATCH_EINVAL(out, NULL, in, in_sz,
		    "oldlenp is NULL");
		TEST_UTIL_BATCH_EINVAL(out, &out_sz, NULL, in_sz,
		    "newp is NULL");
		TEST_UTIL_BATCH_EINVAL(out, &out_sz, in, 0,
		    "newlen is zero");
		in_sz -= 1;
		TEST_UTIL_BATCH_EINVAL(out, &out_sz, in, in_sz,
		    "newlen is not an exact multiple");
		in_sz += 1;
		out_sz_ref = out_sz -= 2 * sizeof(size_t);
		TEST_UTIL_BATCH_EINVAL(out, &out_sz, in, in_sz,
		    "*oldlenp is not an exact multiple");
		out_sz_ref = out_sz += 2 * sizeof(size_t);
		in_sz -= sizeof(const void *);
		TEST_UTIL_BATCH_EINVAL(out, &out_sz, in, in_sz,
		    "*oldlenp and newlen do not match");
		in_sz += sizeof(const void *);

	/* Examine output for valid calls */
#define TEST_EQUAL_REF(i, message) \
	assert_d_eq(memcmp(out + (i) * 3, out_ref + (i) * 3, 3), 0, message)

#define NFREE_READ(out, i) out[(i) * 3]
#define NREGS_READ(out, i) out[(i) * 3 + 1]
#define SIZE_READ(out, i) out[(i) * 3 + 2]

		out_sz_ref = out_sz /= 2;
		in_sz /= 2;
		TEST_UTIL_BATCH_VALID;
		expect_zu_le(sz, SIZE_READ(out, 0),
		    "Extent size should be at least allocation size");
		expect_zu_eq(SIZE_READ(out, 0) & (PAGE - 1), 0,
		    "Extent size should be a multiple of page size");
		/*
		 * See the corresponding comment in test_query; profiling breaks
		 * our slab count expectations.
		 */
		if (sz <= SC_SMALL_MAXCLASS && !opt_prof) {
			expect_zu_le(NFREE_READ(out, 0), NREGS_READ(out, 0),
			    "Extent free count exceeded region count");
			expect_zu_le(NREGS_READ(out, 0), SIZE_READ(out, 0),
			    "Extent region count exceeded size");
			expect_zu_ne(NREGS_READ(out, 0), 0,
			    "Extent region count must be positive");
		} else if (sz > SC_SMALL_MAXCLASS) {
			expect_zu_eq(NFREE_READ(out, 0), 0,
			    "Extent free count should be zero");
			expect_zu_eq(NREGS_READ(out, 0), 1,
			    "Extent region count should be one");
		}
		TEST_EQUAL_REF(1,
		    "Should not overwrite content beyond what's needed");
		in_sz *= 2;
		out_sz_ref = out_sz *= 2;

		memcpy(out_ref, out, 3 * sizeof(size_t));
		TEST_UTIL_BATCH_VALID;
		TEST_EQUAL_REF(0, "Statistics should be stable across calls");
		if (sz <= SC_SMALL_MAXCLASS) {
			expect_zu_le(NFREE_READ(out, 1), NREGS_READ(out, 1),
			    "Extent free count exceeded region count");
		} else {
			expect_zu_eq(NFREE_READ(out, 0), 0,
			    "Extent free count should be zero");
		}
		expect_zu_eq(NREGS_READ(out, 0), NREGS_READ(out, 1),
		    "Extent region count should be same for same region size");
		expect_zu_eq(SIZE_READ(out, 0), SIZE_READ(out, 1),
		    "Extent size should be same for same region size");

#undef SIZE_READ
#undef NREGS_READ
#undef NFREE_READ

#undef TEST_EQUAL_REF

		free(q);
		free(p);
	}
}
TEST_END

int
main(void) {
	assert_zu_lt(SC_SMALL_MAXCLASS + 100000, TEST_MAX_SIZE,
	    "Test case cannot cover large classes");
	return test(test_query, test_batch);
}

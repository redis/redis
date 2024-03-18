#include "test/jemalloc_test.h"

#include "jemalloc/internal/bit_util.h"

#define TEST_POW2_CEIL(t, suf, pri) do {				\
	unsigned i, pow2;						\
	t x;								\
									\
	expect_##suf##_eq(pow2_ceil_##suf(0), 0, "Unexpected result");	\
									\
	for (i = 0; i < sizeof(t) * 8; i++) {				\
		expect_##suf##_eq(pow2_ceil_##suf(((t)1) << i), ((t)1)	\
		    << i, "Unexpected result");				\
	}								\
									\
	for (i = 2; i < sizeof(t) * 8; i++) {				\
		expect_##suf##_eq(pow2_ceil_##suf((((t)1) << i) - 1),	\
		    ((t)1) << i, "Unexpected result");			\
	}								\
									\
	for (i = 0; i < sizeof(t) * 8 - 1; i++) {			\
		expect_##suf##_eq(pow2_ceil_##suf((((t)1) << i) + 1),	\
		    ((t)1) << (i+1), "Unexpected result");		\
	}								\
									\
	for (pow2 = 1; pow2 < 25; pow2++) {				\
		for (x = (((t)1) << (pow2-1)) + 1; x <= ((t)1) << pow2;	\
		    x++) {						\
			expect_##suf##_eq(pow2_ceil_##suf(x),		\
			    ((t)1) << pow2,				\
			    "Unexpected result, x=%"pri, x);		\
		}							\
	}								\
} while (0)

TEST_BEGIN(test_pow2_ceil_u64) {
	TEST_POW2_CEIL(uint64_t, u64, FMTu64);
}
TEST_END

TEST_BEGIN(test_pow2_ceil_u32) {
	TEST_POW2_CEIL(uint32_t, u32, FMTu32);
}
TEST_END

TEST_BEGIN(test_pow2_ceil_zu) {
	TEST_POW2_CEIL(size_t, zu, "zu");
}
TEST_END

void
expect_lg_ceil_range(size_t input, unsigned answer) {
	if (input == 1) {
		expect_u_eq(0, answer, "Got %u as lg_ceil of 1", answer);
		return;
	}
	expect_zu_le(input, (ZU(1) << answer),
	    "Got %u as lg_ceil of %zu", answer, input);
	expect_zu_gt(input, (ZU(1) << (answer - 1)),
	    "Got %u as lg_ceil of %zu", answer, input);
}

void
expect_lg_floor_range(size_t input, unsigned answer) {
	if (input == 1) {
		expect_u_eq(0, answer, "Got %u as lg_floor of 1", answer);
		return;
	}
	expect_zu_ge(input, (ZU(1) << answer),
	    "Got %u as lg_floor of %zu", answer, input);
	expect_zu_lt(input, (ZU(1) << (answer + 1)),
	    "Got %u as lg_floor of %zu", answer, input);
}

TEST_BEGIN(test_lg_ceil_floor) {
	for (size_t i = 1; i < 10 * 1000 * 1000; i++) {
		expect_lg_ceil_range(i, lg_ceil(i));
		expect_lg_ceil_range(i, LG_CEIL(i));
		expect_lg_floor_range(i, lg_floor(i));
		expect_lg_floor_range(i, LG_FLOOR(i));
	}
	for (int i = 10; i < 8 * (1 << LG_SIZEOF_PTR) - 5; i++) {
		for (size_t j = 0; j < (1 << 4); j++) {
			size_t num1 = ((size_t)1 << i)
			    - j * ((size_t)1 << (i - 4));
			size_t num2 = ((size_t)1 << i)
			    + j * ((size_t)1 << (i - 4));
			expect_zu_ne(num1, 0, "Invalid lg argument");
			expect_zu_ne(num2, 0, "Invalid lg argument");
			expect_lg_ceil_range(num1, lg_ceil(num1));
			expect_lg_ceil_range(num1, LG_CEIL(num1));
			expect_lg_ceil_range(num2, lg_ceil(num2));
			expect_lg_ceil_range(num2, LG_CEIL(num2));

			expect_lg_floor_range(num1, lg_floor(num1));
			expect_lg_floor_range(num1, LG_FLOOR(num1));
			expect_lg_floor_range(num2, lg_floor(num2));
			expect_lg_floor_range(num2, LG_FLOOR(num2));
		}
	}
}
TEST_END

#define TEST_FFS(t, suf, test_suf, pri) do {				\
	for (unsigned i = 0; i < sizeof(t) * 8; i++) {			\
		for (unsigned j = 0; j <= i; j++) {			\
			for (unsigned k = 0; k <= j; k++) {		\
				t x = (t)1 << i;			\
				x |= (t)1 << j;				\
				x |= (t)1 << k;				\
				expect_##test_suf##_eq(ffs_##suf(x), k,	\
				    "Unexpected result, x=%"pri, x);	\
			}						\
		}							\
	}								\
} while(0)

TEST_BEGIN(test_ffs_u) {
	TEST_FFS(unsigned, u, u,"u");
}
TEST_END

TEST_BEGIN(test_ffs_lu) {
	TEST_FFS(unsigned long, lu, lu, "lu");
}
TEST_END

TEST_BEGIN(test_ffs_llu) {
	TEST_FFS(unsigned long long, llu, qd, "llu");
}
TEST_END

TEST_BEGIN(test_ffs_u32) {
	TEST_FFS(uint32_t, u32, u32, FMTu32);
}
TEST_END

TEST_BEGIN(test_ffs_u64) {
	TEST_FFS(uint64_t, u64, u64, FMTu64);
}
TEST_END

TEST_BEGIN(test_ffs_zu) {
	TEST_FFS(size_t, zu, zu, "zu");
}
TEST_END

#define TEST_FLS(t, suf, test_suf, pri) do {				\
	for (unsigned i = 0; i < sizeof(t) * 8; i++) {			\
		for (unsigned j = 0; j <= i; j++) {			\
			for (unsigned k = 0; k <= j; k++) {		\
				t x = (t)1 << i;			\
				x |= (t)1 << j;				\
				x |= (t)1 << k;				\
				expect_##test_suf##_eq(fls_##suf(x), i,	\
				    "Unexpected result, x=%"pri, x);	\
			}						\
		}							\
	}								\
} while(0)

TEST_BEGIN(test_fls_u) {
	TEST_FLS(unsigned, u, u,"u");
}
TEST_END

TEST_BEGIN(test_fls_lu) {
	TEST_FLS(unsigned long, lu, lu, "lu");
}
TEST_END

TEST_BEGIN(test_fls_llu) {
	TEST_FLS(unsigned long long, llu, qd, "llu");
}
TEST_END

TEST_BEGIN(test_fls_u32) {
	TEST_FLS(uint32_t, u32, u32, FMTu32);
}
TEST_END

TEST_BEGIN(test_fls_u64) {
	TEST_FLS(uint64_t, u64, u64, FMTu64);
}
TEST_END

TEST_BEGIN(test_fls_zu) {
	TEST_FLS(size_t, zu, zu, "zu");
}
TEST_END

TEST_BEGIN(test_fls_u_slow) {
	TEST_FLS(unsigned, u_slow, u,"u");
}
TEST_END

TEST_BEGIN(test_fls_lu_slow) {
	TEST_FLS(unsigned long, lu_slow, lu, "lu");
}
TEST_END

TEST_BEGIN(test_fls_llu_slow) {
	TEST_FLS(unsigned long long, llu_slow, qd, "llu");
}
TEST_END

static unsigned
popcount_byte(unsigned byte) {
	int count = 0;
	for (int i = 0; i < 8; i++) {
		if ((byte & (1 << i)) != 0) {
			count++;
		}
	}
	return count;
}

static uint64_t
expand_byte_to_mask(unsigned byte) {
	uint64_t result = 0;
	for (int i = 0; i < 8; i++) {
		if ((byte & (1 << i)) != 0) {
			result |= ((uint64_t)0xFF << (i * 8));
		}
	}
	return result;
}

#define TEST_POPCOUNT(t, suf, pri_hex) do {				\
	t bmul = (t)0x0101010101010101ULL;				\
	for (unsigned i = 0; i < (1 << sizeof(t)); i++) {		\
		for (unsigned j = 0; j < 256; j++) {			\
			/*						\
			 * Replicate the byte j into various		\
			 * bytes of the integer (as indicated by the	\
			 * mask in i), and ensure that the popcount of	\
			 * the result is popcount(i) * popcount(j)	\
			 */						\
			t mask = (t)expand_byte_to_mask(i);		\
			t x = (bmul * j) & mask;			\
			expect_u_eq(					\
			    popcount_byte(i) * popcount_byte(j),	\
			    popcount_##suf(x),				\
			    "Unexpected result, x=0x%"pri_hex, x);	\
		}							\
	}								\
} while (0)

TEST_BEGIN(test_popcount_u) {
	TEST_POPCOUNT(unsigned, u, "x");
}
TEST_END

TEST_BEGIN(test_popcount_u_slow) {
	TEST_POPCOUNT(unsigned, u_slow, "x");
}
TEST_END

TEST_BEGIN(test_popcount_lu) {
	TEST_POPCOUNT(unsigned long, lu, "lx");
}
TEST_END

TEST_BEGIN(test_popcount_lu_slow) {
	TEST_POPCOUNT(unsigned long, lu_slow, "lx");
}
TEST_END

TEST_BEGIN(test_popcount_llu) {
	TEST_POPCOUNT(unsigned long long, llu, "llx");
}
TEST_END

TEST_BEGIN(test_popcount_llu_slow) {
	TEST_POPCOUNT(unsigned long long, llu_slow, "llx");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_pow2_ceil_u64,
	    test_pow2_ceil_u32,
	    test_pow2_ceil_zu,
	    test_lg_ceil_floor,
	    test_ffs_u,
	    test_ffs_lu,
	    test_ffs_llu,
	    test_ffs_u32,
	    test_ffs_u64,
	    test_ffs_zu,
	    test_fls_u,
	    test_fls_lu,
	    test_fls_llu,
	    test_fls_u32,
	    test_fls_u64,
	    test_fls_zu,
	    test_fls_u_slow,
	    test_fls_lu_slow,
	    test_fls_llu_slow,
	    test_popcount_u,
	    test_popcount_u_slow,
	    test_popcount_lu,
	    test_popcount_lu_slow,
	    test_popcount_llu,
	    test_popcount_llu_slow);
}

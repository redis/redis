#include "test/jemalloc_test.h"

static void
test_prng_lg_range_u32(bool atomic) {
	atomic_u32_t sa, sb;
	uint32_t ra, rb;
	unsigned lg_range;

	atomic_store_u32(&sa, 42, ATOMIC_RELAXED);
	ra = prng_lg_range_u32(&sa, 32, atomic);
	atomic_store_u32(&sa, 42, ATOMIC_RELAXED);
	rb = prng_lg_range_u32(&sa, 32, atomic);
	assert_u32_eq(ra, rb,
	    "Repeated generation should produce repeated results");

	atomic_store_u32(&sb, 42, ATOMIC_RELAXED);
	rb = prng_lg_range_u32(&sb, 32, atomic);
	assert_u32_eq(ra, rb,
	    "Equivalent generation should produce equivalent results");

	atomic_store_u32(&sa, 42, ATOMIC_RELAXED);
	ra = prng_lg_range_u32(&sa, 32, atomic);
	rb = prng_lg_range_u32(&sa, 32, atomic);
	assert_u32_ne(ra, rb,
	    "Full-width results must not immediately repeat");

	atomic_store_u32(&sa, 42, ATOMIC_RELAXED);
	ra = prng_lg_range_u32(&sa, 32, atomic);
	for (lg_range = 31; lg_range > 0; lg_range--) {
		atomic_store_u32(&sb, 42, ATOMIC_RELAXED);
		rb = prng_lg_range_u32(&sb, lg_range, atomic);
		assert_u32_eq((rb & (UINT32_C(0xffffffff) << lg_range)),
		    0, "High order bits should be 0, lg_range=%u", lg_range);
		assert_u32_eq(rb, (ra >> (32 - lg_range)),
		    "Expected high order bits of full-width result, "
		    "lg_range=%u", lg_range);
	}
}

static void
test_prng_lg_range_u64(void) {
	uint64_t sa, sb, ra, rb;
	unsigned lg_range;

	sa = 42;
	ra = prng_lg_range_u64(&sa, 64);
	sa = 42;
	rb = prng_lg_range_u64(&sa, 64);
	assert_u64_eq(ra, rb,
	    "Repeated generation should produce repeated results");

	sb = 42;
	rb = prng_lg_range_u64(&sb, 64);
	assert_u64_eq(ra, rb,
	    "Equivalent generation should produce equivalent results");

	sa = 42;
	ra = prng_lg_range_u64(&sa, 64);
	rb = prng_lg_range_u64(&sa, 64);
	assert_u64_ne(ra, rb,
	    "Full-width results must not immediately repeat");

	sa = 42;
	ra = prng_lg_range_u64(&sa, 64);
	for (lg_range = 63; lg_range > 0; lg_range--) {
		sb = 42;
		rb = prng_lg_range_u64(&sb, lg_range);
		assert_u64_eq((rb & (UINT64_C(0xffffffffffffffff) << lg_range)),
		    0, "High order bits should be 0, lg_range=%u", lg_range);
		assert_u64_eq(rb, (ra >> (64 - lg_range)),
		    "Expected high order bits of full-width result, "
		    "lg_range=%u", lg_range);
	}
}

static void
test_prng_lg_range_zu(bool atomic) {
	atomic_zu_t sa, sb;
	size_t ra, rb;
	unsigned lg_range;

	atomic_store_zu(&sa, 42, ATOMIC_RELAXED);
	ra = prng_lg_range_zu(&sa, ZU(1) << (3 + LG_SIZEOF_PTR), atomic);
	atomic_store_zu(&sa, 42, ATOMIC_RELAXED);
	rb = prng_lg_range_zu(&sa, ZU(1) << (3 + LG_SIZEOF_PTR), atomic);
	assert_zu_eq(ra, rb,
	    "Repeated generation should produce repeated results");

	atomic_store_zu(&sb, 42, ATOMIC_RELAXED);
	rb = prng_lg_range_zu(&sb, ZU(1) << (3 + LG_SIZEOF_PTR), atomic);
	assert_zu_eq(ra, rb,
	    "Equivalent generation should produce equivalent results");

	atomic_store_zu(&sa, 42, ATOMIC_RELAXED);
	ra = prng_lg_range_zu(&sa, ZU(1) << (3 + LG_SIZEOF_PTR), atomic);
	rb = prng_lg_range_zu(&sa, ZU(1) << (3 + LG_SIZEOF_PTR), atomic);
	assert_zu_ne(ra, rb,
	    "Full-width results must not immediately repeat");

	atomic_store_zu(&sa, 42, ATOMIC_RELAXED);
	ra = prng_lg_range_zu(&sa, ZU(1) << (3 + LG_SIZEOF_PTR), atomic);
	for (lg_range = (ZU(1) << (3 + LG_SIZEOF_PTR)) - 1; lg_range > 0;
	    lg_range--) {
		atomic_store_zu(&sb, 42, ATOMIC_RELAXED);
		rb = prng_lg_range_zu(&sb, lg_range, atomic);
		assert_zu_eq((rb & (SIZE_T_MAX << lg_range)),
		    0, "High order bits should be 0, lg_range=%u", lg_range);
		assert_zu_eq(rb, (ra >> ((ZU(1) << (3 + LG_SIZEOF_PTR)) -
		    lg_range)), "Expected high order bits of full-width "
		    "result, lg_range=%u", lg_range);
	}
}

TEST_BEGIN(test_prng_lg_range_u32_nonatomic) {
	test_prng_lg_range_u32(false);
}
TEST_END

TEST_BEGIN(test_prng_lg_range_u32_atomic) {
	test_prng_lg_range_u32(true);
}
TEST_END

TEST_BEGIN(test_prng_lg_range_u64_nonatomic) {
	test_prng_lg_range_u64();
}
TEST_END

TEST_BEGIN(test_prng_lg_range_zu_nonatomic) {
	test_prng_lg_range_zu(false);
}
TEST_END

TEST_BEGIN(test_prng_lg_range_zu_atomic) {
	test_prng_lg_range_zu(true);
}
TEST_END

static void
test_prng_range_u32(bool atomic) {
	uint32_t range;
#define MAX_RANGE	10000000
#define RANGE_STEP	97
#define NREPS		10

	for (range = 2; range < MAX_RANGE; range += RANGE_STEP) {
		atomic_u32_t s;
		unsigned rep;

		atomic_store_u32(&s, range, ATOMIC_RELAXED);
		for (rep = 0; rep < NREPS; rep++) {
			uint32_t r = prng_range_u32(&s, range, atomic);

			assert_u32_lt(r, range, "Out of range");
		}
	}
}

static void
test_prng_range_u64(void) {
	uint64_t range;
#define MAX_RANGE	10000000
#define RANGE_STEP	97
#define NREPS		10

	for (range = 2; range < MAX_RANGE; range += RANGE_STEP) {
		uint64_t s;
		unsigned rep;

		s = range;
		for (rep = 0; rep < NREPS; rep++) {
			uint64_t r = prng_range_u64(&s, range);

			assert_u64_lt(r, range, "Out of range");
		}
	}
}

static void
test_prng_range_zu(bool atomic) {
	size_t range;
#define MAX_RANGE	10000000
#define RANGE_STEP	97
#define NREPS		10

	for (range = 2; range < MAX_RANGE; range += RANGE_STEP) {
		atomic_zu_t s;
		unsigned rep;

		atomic_store_zu(&s, range, ATOMIC_RELAXED);
		for (rep = 0; rep < NREPS; rep++) {
			size_t r = prng_range_zu(&s, range, atomic);

			assert_zu_lt(r, range, "Out of range");
		}
	}
}

TEST_BEGIN(test_prng_range_u32_nonatomic) {
	test_prng_range_u32(false);
}
TEST_END

TEST_BEGIN(test_prng_range_u32_atomic) {
	test_prng_range_u32(true);
}
TEST_END

TEST_BEGIN(test_prng_range_u64_nonatomic) {
	test_prng_range_u64();
}
TEST_END

TEST_BEGIN(test_prng_range_zu_nonatomic) {
	test_prng_range_zu(false);
}
TEST_END

TEST_BEGIN(test_prng_range_zu_atomic) {
	test_prng_range_zu(true);
}
TEST_END

int
main(void) {
	return test(
	    test_prng_lg_range_u32_nonatomic,
	    test_prng_lg_range_u32_atomic,
	    test_prng_lg_range_u64_nonatomic,
	    test_prng_lg_range_zu_nonatomic,
	    test_prng_lg_range_zu_atomic,
	    test_prng_range_u32_nonatomic,
	    test_prng_range_u32_atomic,
	    test_prng_range_u64_nonatomic,
	    test_prng_range_zu_nonatomic,
	    test_prng_range_zu_atomic);
}

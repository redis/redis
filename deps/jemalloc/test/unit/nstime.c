#include "test/jemalloc_test.h"

#define BILLION	UINT64_C(1000000000)

TEST_BEGIN(test_nstime_init) {
	nstime_t nst;

	nstime_init(&nst, 42000000043);
	expect_u64_eq(nstime_ns(&nst), 42000000043, "ns incorrectly read");
	expect_u64_eq(nstime_sec(&nst), 42, "sec incorrectly read");
	expect_u64_eq(nstime_nsec(&nst), 43, "nsec incorrectly read");
}
TEST_END

TEST_BEGIN(test_nstime_init2) {
	nstime_t nst;

	nstime_init2(&nst, 42, 43);
	expect_u64_eq(nstime_sec(&nst), 42, "sec incorrectly read");
	expect_u64_eq(nstime_nsec(&nst), 43, "nsec incorrectly read");
}
TEST_END

TEST_BEGIN(test_nstime_copy) {
	nstime_t nsta, nstb;

	nstime_init2(&nsta, 42, 43);
	nstime_init_zero(&nstb);
	nstime_copy(&nstb, &nsta);
	expect_u64_eq(nstime_sec(&nstb), 42, "sec incorrectly copied");
	expect_u64_eq(nstime_nsec(&nstb), 43, "nsec incorrectly copied");
}
TEST_END

TEST_BEGIN(test_nstime_compare) {
	nstime_t nsta, nstb;

	nstime_init2(&nsta, 42, 43);
	nstime_copy(&nstb, &nsta);
	expect_d_eq(nstime_compare(&nsta, &nstb), 0, "Times should be equal");
	expect_d_eq(nstime_compare(&nstb, &nsta), 0, "Times should be equal");

	nstime_init2(&nstb, 42, 42);
	expect_d_eq(nstime_compare(&nsta, &nstb), 1,
	    "nsta should be greater than nstb");
	expect_d_eq(nstime_compare(&nstb, &nsta), -1,
	    "nstb should be less than nsta");

	nstime_init2(&nstb, 42, 44);
	expect_d_eq(nstime_compare(&nsta, &nstb), -1,
	    "nsta should be less than nstb");
	expect_d_eq(nstime_compare(&nstb, &nsta), 1,
	    "nstb should be greater than nsta");

	nstime_init2(&nstb, 41, BILLION - 1);
	expect_d_eq(nstime_compare(&nsta, &nstb), 1,
	    "nsta should be greater than nstb");
	expect_d_eq(nstime_compare(&nstb, &nsta), -1,
	    "nstb should be less than nsta");

	nstime_init2(&nstb, 43, 0);
	expect_d_eq(nstime_compare(&nsta, &nstb), -1,
	    "nsta should be less than nstb");
	expect_d_eq(nstime_compare(&nstb, &nsta), 1,
	    "nstb should be greater than nsta");
}
TEST_END

TEST_BEGIN(test_nstime_add) {
	nstime_t nsta, nstb;

	nstime_init2(&nsta, 42, 43);
	nstime_copy(&nstb, &nsta);
	nstime_add(&nsta, &nstb);
	nstime_init2(&nstb, 84, 86);
	expect_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect addition result");

	nstime_init2(&nsta, 42, BILLION - 1);
	nstime_copy(&nstb, &nsta);
	nstime_add(&nsta, &nstb);
	nstime_init2(&nstb, 85, BILLION - 2);
	expect_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect addition result");
}
TEST_END

TEST_BEGIN(test_nstime_iadd) {
	nstime_t nsta, nstb;

	nstime_init2(&nsta, 42, BILLION - 1);
	nstime_iadd(&nsta, 1);
	nstime_init2(&nstb, 43, 0);
	expect_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect addition result");

	nstime_init2(&nsta, 42, 1);
	nstime_iadd(&nsta, BILLION + 1);
	nstime_init2(&nstb, 43, 2);
	expect_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect addition result");
}
TEST_END

TEST_BEGIN(test_nstime_subtract) {
	nstime_t nsta, nstb;

	nstime_init2(&nsta, 42, 43);
	nstime_copy(&nstb, &nsta);
	nstime_subtract(&nsta, &nstb);
	nstime_init_zero(&nstb);
	expect_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect subtraction result");

	nstime_init2(&nsta, 42, 43);
	nstime_init2(&nstb, 41, 44);
	nstime_subtract(&nsta, &nstb);
	nstime_init2(&nstb, 0, BILLION - 1);
	expect_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect subtraction result");
}
TEST_END

TEST_BEGIN(test_nstime_isubtract) {
	nstime_t nsta, nstb;

	nstime_init2(&nsta, 42, 43);
	nstime_isubtract(&nsta, 42*BILLION + 43);
	nstime_init_zero(&nstb);
	expect_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect subtraction result");

	nstime_init2(&nsta, 42, 43);
	nstime_isubtract(&nsta, 41*BILLION + 44);
	nstime_init2(&nstb, 0, BILLION - 1);
	expect_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect subtraction result");
}
TEST_END

TEST_BEGIN(test_nstime_imultiply) {
	nstime_t nsta, nstb;

	nstime_init2(&nsta, 42, 43);
	nstime_imultiply(&nsta, 10);
	nstime_init2(&nstb, 420, 430);
	expect_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect multiplication result");

	nstime_init2(&nsta, 42, 666666666);
	nstime_imultiply(&nsta, 3);
	nstime_init2(&nstb, 127, 999999998);
	expect_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect multiplication result");
}
TEST_END

TEST_BEGIN(test_nstime_idivide) {
	nstime_t nsta, nstb;

	nstime_init2(&nsta, 42, 43);
	nstime_copy(&nstb, &nsta);
	nstime_imultiply(&nsta, 10);
	nstime_idivide(&nsta, 10);
	expect_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect division result");

	nstime_init2(&nsta, 42, 666666666);
	nstime_copy(&nstb, &nsta);
	nstime_imultiply(&nsta, 3);
	nstime_idivide(&nsta, 3);
	expect_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect division result");
}
TEST_END

TEST_BEGIN(test_nstime_divide) {
	nstime_t nsta, nstb, nstc;

	nstime_init2(&nsta, 42, 43);
	nstime_copy(&nstb, &nsta);
	nstime_imultiply(&nsta, 10);
	expect_u64_eq(nstime_divide(&nsta, &nstb), 10,
	    "Incorrect division result");

	nstime_init2(&nsta, 42, 43);
	nstime_copy(&nstb, &nsta);
	nstime_imultiply(&nsta, 10);
	nstime_init(&nstc, 1);
	nstime_add(&nsta, &nstc);
	expect_u64_eq(nstime_divide(&nsta, &nstb), 10,
	    "Incorrect division result");

	nstime_init2(&nsta, 42, 43);
	nstime_copy(&nstb, &nsta);
	nstime_imultiply(&nsta, 10);
	nstime_init(&nstc, 1);
	nstime_subtract(&nsta, &nstc);
	expect_u64_eq(nstime_divide(&nsta, &nstb), 9,
	    "Incorrect division result");
}
TEST_END

void
test_nstime_since_once(nstime_t *t) {
	nstime_t old_t;
	nstime_copy(&old_t, t);

	uint64_t ns_since = nstime_ns_since(t);
	nstime_update(t);

	nstime_t new_t;
	nstime_copy(&new_t, t);
	nstime_subtract(&new_t, &old_t);

	expect_u64_ge(nstime_ns(&new_t), ns_since,
	    "Incorrect time since result");
}

TEST_BEGIN(test_nstime_ns_since) {
	nstime_t t;

	nstime_init_update(&t);
	for (uint64_t i = 0; i < 10000; i++) {
		/* Keeps updating t and verifies ns_since is valid. */
		test_nstime_since_once(&t);
	}
}
TEST_END

TEST_BEGIN(test_nstime_monotonic) {
	nstime_monotonic();
}
TEST_END

int
main(void) {
	return test(
	    test_nstime_init,
	    test_nstime_init2,
	    test_nstime_copy,
	    test_nstime_compare,
	    test_nstime_add,
	    test_nstime_iadd,
	    test_nstime_subtract,
	    test_nstime_isubtract,
	    test_nstime_imultiply,
	    test_nstime_idivide,
	    test_nstime_divide,
	    test_nstime_ns_since,
	    test_nstime_monotonic);
}

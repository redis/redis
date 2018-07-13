#include "test/jemalloc_test.h"

#define BILLION	UINT64_C(1000000000)

TEST_BEGIN(test_nstime_init) {
	nstime_t nst;

	nstime_init(&nst, 42000000043);
	assert_u64_eq(nstime_ns(&nst), 42000000043, "ns incorrectly read");
	assert_u64_eq(nstime_sec(&nst), 42, "sec incorrectly read");
	assert_u64_eq(nstime_nsec(&nst), 43, "nsec incorrectly read");
}
TEST_END

TEST_BEGIN(test_nstime_init2) {
	nstime_t nst;

	nstime_init2(&nst, 42, 43);
	assert_u64_eq(nstime_sec(&nst), 42, "sec incorrectly read");
	assert_u64_eq(nstime_nsec(&nst), 43, "nsec incorrectly read");
}
TEST_END

TEST_BEGIN(test_nstime_copy) {
	nstime_t nsta, nstb;

	nstime_init2(&nsta, 42, 43);
	nstime_init(&nstb, 0);
	nstime_copy(&nstb, &nsta);
	assert_u64_eq(nstime_sec(&nstb), 42, "sec incorrectly copied");
	assert_u64_eq(nstime_nsec(&nstb), 43, "nsec incorrectly copied");
}
TEST_END

TEST_BEGIN(test_nstime_compare) {
	nstime_t nsta, nstb;

	nstime_init2(&nsta, 42, 43);
	nstime_copy(&nstb, &nsta);
	assert_d_eq(nstime_compare(&nsta, &nstb), 0, "Times should be equal");
	assert_d_eq(nstime_compare(&nstb, &nsta), 0, "Times should be equal");

	nstime_init2(&nstb, 42, 42);
	assert_d_eq(nstime_compare(&nsta, &nstb), 1,
	    "nsta should be greater than nstb");
	assert_d_eq(nstime_compare(&nstb, &nsta), -1,
	    "nstb should be less than nsta");

	nstime_init2(&nstb, 42, 44);
	assert_d_eq(nstime_compare(&nsta, &nstb), -1,
	    "nsta should be less than nstb");
	assert_d_eq(nstime_compare(&nstb, &nsta), 1,
	    "nstb should be greater than nsta");

	nstime_init2(&nstb, 41, BILLION - 1);
	assert_d_eq(nstime_compare(&nsta, &nstb), 1,
	    "nsta should be greater than nstb");
	assert_d_eq(nstime_compare(&nstb, &nsta), -1,
	    "nstb should be less than nsta");

	nstime_init2(&nstb, 43, 0);
	assert_d_eq(nstime_compare(&nsta, &nstb), -1,
	    "nsta should be less than nstb");
	assert_d_eq(nstime_compare(&nstb, &nsta), 1,
	    "nstb should be greater than nsta");
}
TEST_END

TEST_BEGIN(test_nstime_add) {
	nstime_t nsta, nstb;

	nstime_init2(&nsta, 42, 43);
	nstime_copy(&nstb, &nsta);
	nstime_add(&nsta, &nstb);
	nstime_init2(&nstb, 84, 86);
	assert_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect addition result");

	nstime_init2(&nsta, 42, BILLION - 1);
	nstime_copy(&nstb, &nsta);
	nstime_add(&nsta, &nstb);
	nstime_init2(&nstb, 85, BILLION - 2);
	assert_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect addition result");
}
TEST_END

TEST_BEGIN(test_nstime_iadd) {
	nstime_t nsta, nstb;

	nstime_init2(&nsta, 42, BILLION - 1);
	nstime_iadd(&nsta, 1);
	nstime_init2(&nstb, 43, 0);
	assert_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect addition result");

	nstime_init2(&nsta, 42, 1);
	nstime_iadd(&nsta, BILLION + 1);
	nstime_init2(&nstb, 43, 2);
	assert_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect addition result");
}
TEST_END

TEST_BEGIN(test_nstime_subtract) {
	nstime_t nsta, nstb;

	nstime_init2(&nsta, 42, 43);
	nstime_copy(&nstb, &nsta);
	nstime_subtract(&nsta, &nstb);
	nstime_init(&nstb, 0);
	assert_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect subtraction result");

	nstime_init2(&nsta, 42, 43);
	nstime_init2(&nstb, 41, 44);
	nstime_subtract(&nsta, &nstb);
	nstime_init2(&nstb, 0, BILLION - 1);
	assert_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect subtraction result");
}
TEST_END

TEST_BEGIN(test_nstime_isubtract) {
	nstime_t nsta, nstb;

	nstime_init2(&nsta, 42, 43);
	nstime_isubtract(&nsta, 42*BILLION + 43);
	nstime_init(&nstb, 0);
	assert_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect subtraction result");

	nstime_init2(&nsta, 42, 43);
	nstime_isubtract(&nsta, 41*BILLION + 44);
	nstime_init2(&nstb, 0, BILLION - 1);
	assert_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect subtraction result");
}
TEST_END

TEST_BEGIN(test_nstime_imultiply) {
	nstime_t nsta, nstb;

	nstime_init2(&nsta, 42, 43);
	nstime_imultiply(&nsta, 10);
	nstime_init2(&nstb, 420, 430);
	assert_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect multiplication result");

	nstime_init2(&nsta, 42, 666666666);
	nstime_imultiply(&nsta, 3);
	nstime_init2(&nstb, 127, 999999998);
	assert_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect multiplication result");
}
TEST_END

TEST_BEGIN(test_nstime_idivide) {
	nstime_t nsta, nstb;

	nstime_init2(&nsta, 42, 43);
	nstime_copy(&nstb, &nsta);
	nstime_imultiply(&nsta, 10);
	nstime_idivide(&nsta, 10);
	assert_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect division result");

	nstime_init2(&nsta, 42, 666666666);
	nstime_copy(&nstb, &nsta);
	nstime_imultiply(&nsta, 3);
	nstime_idivide(&nsta, 3);
	assert_d_eq(nstime_compare(&nsta, &nstb), 0,
	    "Incorrect division result");
}
TEST_END

TEST_BEGIN(test_nstime_divide) {
	nstime_t nsta, nstb, nstc;

	nstime_init2(&nsta, 42, 43);
	nstime_copy(&nstb, &nsta);
	nstime_imultiply(&nsta, 10);
	assert_u64_eq(nstime_divide(&nsta, &nstb), 10,
	    "Incorrect division result");

	nstime_init2(&nsta, 42, 43);
	nstime_copy(&nstb, &nsta);
	nstime_imultiply(&nsta, 10);
	nstime_init(&nstc, 1);
	nstime_add(&nsta, &nstc);
	assert_u64_eq(nstime_divide(&nsta, &nstb), 10,
	    "Incorrect division result");

	nstime_init2(&nsta, 42, 43);
	nstime_copy(&nstb, &nsta);
	nstime_imultiply(&nsta, 10);
	nstime_init(&nstc, 1);
	nstime_subtract(&nsta, &nstc);
	assert_u64_eq(nstime_divide(&nsta, &nstb), 9,
	    "Incorrect division result");
}
TEST_END

TEST_BEGIN(test_nstime_monotonic) {
	nstime_monotonic();
}
TEST_END

TEST_BEGIN(test_nstime_update) {
	nstime_t nst;

	nstime_init(&nst, 0);

	assert_false(nstime_update(&nst), "Basic time update failed.");

	/* Only Rip Van Winkle sleeps this long. */
	{
		nstime_t addend;
		nstime_init2(&addend, 631152000, 0);
		nstime_add(&nst, &addend);
	}
	{
		nstime_t nst0;
		nstime_copy(&nst0, &nst);
		assert_true(nstime_update(&nst),
		    "Update should detect time roll-back.");
		assert_d_eq(nstime_compare(&nst, &nst0), 0,
		    "Time should not have been modified");
	}
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
	    test_nstime_monotonic,
	    test_nstime_update);
}

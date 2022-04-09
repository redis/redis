#include "test/jemalloc_test.h"

/*
 * We *almost* have consistent short names (e.g. "u32" for uint32_t, "b" for
 * bool, etc.  The one exception is that the short name for void * is "p" in
 * some places and "ptr" in others.  In the long run it would be nice to unify
 * these, but in the short run we'll use this shim.
 */
#define assert_p_eq assert_ptr_eq

/*
 * t: the non-atomic type, like "uint32_t".
 * ta: the short name for the type, like "u32".
 * val[1,2,3]: Values of the given type.  The CAS tests use val2 for expected,
 * and val3 for desired.
 */

#define DO_TESTS(t, ta, val1, val2, val3) do {				\
	t val;								\
	t expected;							\
	bool success;							\
	/* This (along with the load below) also tests ATOMIC_LOAD. */	\
	atomic_##ta##_t atom = ATOMIC_INIT(val1);			\
									\
	/* ATOMIC_INIT and load. */					\
	val = atomic_load_##ta(&atom, ATOMIC_RELAXED);			\
	assert_##ta##_eq(val1, val, "Load or init failed");		\
									\
	/* Store. */							\
	atomic_store_##ta(&atom, val1, ATOMIC_RELAXED);			\
	atomic_store_##ta(&atom, val2, ATOMIC_RELAXED);			\
	val = atomic_load_##ta(&atom, ATOMIC_RELAXED);			\
	assert_##ta##_eq(val2, val, "Store failed");			\
									\
	/* Exchange. */							\
	atomic_store_##ta(&atom, val1, ATOMIC_RELAXED);			\
	val = atomic_exchange_##ta(&atom, val2, ATOMIC_RELAXED);	\
	assert_##ta##_eq(val1, val, "Exchange returned invalid value");	\
	val = atomic_load_##ta(&atom, ATOMIC_RELAXED);			\
	assert_##ta##_eq(val2, val, "Exchange store invalid value");	\
									\
	/* 								\
	 * Weak CAS.  Spurious failures are allowed, so we loop a few	\
	 * times.							\
	 */								\
	atomic_store_##ta(&atom, val1, ATOMIC_RELAXED);			\
	success = false;						\
	for (int i = 0; i < 10 && !success; i++) {			\
		expected = val2;					\
		success = atomic_compare_exchange_weak_##ta(&atom,	\
		    &expected, val3, ATOMIC_RELAXED, ATOMIC_RELAXED);	\
		assert_##ta##_eq(val1, expected, 			\
		    "CAS should update expected");			\
	}								\
	assert_b_eq(val1 == val2, success,				\
	    "Weak CAS did the wrong state update");			\
	val = atomic_load_##ta(&atom, ATOMIC_RELAXED);			\
	if (success) {							\
		assert_##ta##_eq(val3, val,				\
		    "Successful CAS should update atomic");		\
	} else {							\
		assert_##ta##_eq(val1, val,				\
		    "Unsuccessful CAS should not update atomic");	\
	}								\
									\
	/* Strong CAS. */						\
	atomic_store_##ta(&atom, val1, ATOMIC_RELAXED);			\
	expected = val2;						\
	success = atomic_compare_exchange_strong_##ta(&atom, &expected,	\
	    val3, ATOMIC_RELAXED, ATOMIC_RELAXED);			\
	assert_b_eq(val1 == val2, success,				\
	    "Strong CAS did the wrong state update");			\
	val = atomic_load_##ta(&atom, ATOMIC_RELAXED);			\
	if (success) {							\
		assert_##ta##_eq(val3, val,				\
		    "Successful CAS should update atomic");		\
	} else {							\
		assert_##ta##_eq(val1, val,				\
		    "Unsuccessful CAS should not update atomic");	\
	}								\
									\
									\
} while (0)

#define DO_INTEGER_TESTS(t, ta, val1, val2) do {			\
	atomic_##ta##_t atom;						\
	t val;								\
									\
	/* Fetch-add. */						\
	atomic_store_##ta(&atom, val1, ATOMIC_RELAXED);			\
	val = atomic_fetch_add_##ta(&atom, val2, ATOMIC_RELAXED);	\
	assert_##ta##_eq(val1, val,					\
	    "Fetch-add should return previous value");			\
	val = atomic_load_##ta(&atom, ATOMIC_RELAXED);			\
	assert_##ta##_eq(val1 + val2, val,				\
	    "Fetch-add should update atomic");				\
									\
	/* Fetch-sub. */						\
	atomic_store_##ta(&atom, val1, ATOMIC_RELAXED);			\
	val = atomic_fetch_sub_##ta(&atom, val2, ATOMIC_RELAXED);	\
	assert_##ta##_eq(val1, val,					\
	    "Fetch-sub should return previous value");			\
	val = atomic_load_##ta(&atom, ATOMIC_RELAXED);			\
	assert_##ta##_eq(val1 - val2, val,				\
	    "Fetch-sub should update atomic");				\
									\
	/* Fetch-and. */						\
	atomic_store_##ta(&atom, val1, ATOMIC_RELAXED);			\
	val = atomic_fetch_and_##ta(&atom, val2, ATOMIC_RELAXED);	\
	assert_##ta##_eq(val1, val,					\
	    "Fetch-and should return previous value");			\
	val = atomic_load_##ta(&atom, ATOMIC_RELAXED);			\
	assert_##ta##_eq(val1 & val2, val,				\
	    "Fetch-and should update atomic");				\
									\
	/* Fetch-or. */							\
	atomic_store_##ta(&atom, val1, ATOMIC_RELAXED);			\
	val = atomic_fetch_or_##ta(&atom, val2, ATOMIC_RELAXED);	\
	assert_##ta##_eq(val1, val,					\
	    "Fetch-or should return previous value");			\
	val = atomic_load_##ta(&atom, ATOMIC_RELAXED);			\
	assert_##ta##_eq(val1 | val2, val,				\
	    "Fetch-or should update atomic");				\
									\
	/* Fetch-xor. */						\
	atomic_store_##ta(&atom, val1, ATOMIC_RELAXED);			\
	val = atomic_fetch_xor_##ta(&atom, val2, ATOMIC_RELAXED);	\
	assert_##ta##_eq(val1, val,					\
	    "Fetch-xor should return previous value");			\
	val = atomic_load_##ta(&atom, ATOMIC_RELAXED);			\
	assert_##ta##_eq(val1 ^ val2, val,				\
	    "Fetch-xor should update atomic");				\
} while (0)

#define TEST_STRUCT(t, ta)						\
typedef struct {							\
	t val1;								\
	t val2;								\
	t val3;								\
} ta##_test_t;

#define TEST_CASES(t) {							\
	{(t)-1, (t)-1, (t)-2},						\
	{(t)-1, (t) 0, (t)-2},						\
	{(t)-1, (t) 1, (t)-2},						\
									\
	{(t) 0, (t)-1, (t)-2},						\
	{(t) 0, (t) 0, (t)-2},						\
	{(t) 0, (t) 1, (t)-2},						\
									\
	{(t) 1, (t)-1, (t)-2},						\
	{(t) 1, (t) 0, (t)-2},						\
	{(t) 1, (t) 1, (t)-2},						\
									\
	{(t)0, (t)-(1 << 22), (t)-2},					\
	{(t)0, (t)(1 << 22), (t)-2},					\
	{(t)(1 << 22), (t)-(1 << 22), (t)-2},				\
	{(t)(1 << 22), (t)(1 << 22), (t)-2}				\
}

#define TEST_BODY(t, ta) do {						\
	const ta##_test_t tests[] = TEST_CASES(t);			\
	for (unsigned i = 0; i < sizeof(tests)/sizeof(tests[0]); i++) {	\
		ta##_test_t test = tests[i];				\
		DO_TESTS(t, ta, test.val1, test.val2, test.val3);	\
	}								\
} while (0)

#define INTEGER_TEST_BODY(t, ta) do {					\
	const ta##_test_t tests[] = TEST_CASES(t);			\
	for (unsigned i = 0; i < sizeof(tests)/sizeof(tests[0]); i++) {	\
		ta##_test_t test = tests[i];				\
		DO_TESTS(t, ta, test.val1, test.val2, test.val3);	\
		DO_INTEGER_TESTS(t, ta, test.val1, test.val2);		\
	}								\
} while (0)

TEST_STRUCT(uint64_t, u64);
TEST_BEGIN(test_atomic_u64) {
#if !(LG_SIZEOF_PTR == 3 || LG_SIZEOF_INT == 3)
	test_skip("64-bit atomic operations not supported");
#else
	INTEGER_TEST_BODY(uint64_t, u64);
#endif
}
TEST_END


TEST_STRUCT(uint32_t, u32);
TEST_BEGIN(test_atomic_u32) {
	INTEGER_TEST_BODY(uint32_t, u32);
}
TEST_END

TEST_STRUCT(void *, p);
TEST_BEGIN(test_atomic_p) {
	TEST_BODY(void *, p);
}
TEST_END

TEST_STRUCT(size_t, zu);
TEST_BEGIN(test_atomic_zu) {
	INTEGER_TEST_BODY(size_t, zu);
}
TEST_END

TEST_STRUCT(ssize_t, zd);
TEST_BEGIN(test_atomic_zd) {
	INTEGER_TEST_BODY(ssize_t, zd);
}
TEST_END


TEST_STRUCT(unsigned, u);
TEST_BEGIN(test_atomic_u) {
	INTEGER_TEST_BODY(unsigned, u);
}
TEST_END

int
main(void) {
	return test(
	    test_atomic_u64,
	    test_atomic_u32,
	    test_atomic_p,
	    test_atomic_zu,
	    test_atomic_zd,
	    test_atomic_u);
}

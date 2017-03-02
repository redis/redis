#include "test/jemalloc_test.h"

static witness_lock_error_t *witness_lock_error_orig;
static witness_owner_error_t *witness_owner_error_orig;
static witness_not_owner_error_t *witness_not_owner_error_orig;
static witness_lockless_error_t *witness_lockless_error_orig;

static bool saw_lock_error;
static bool saw_owner_error;
static bool saw_not_owner_error;
static bool saw_lockless_error;

static void
witness_lock_error_intercept(const witness_list_t *witnesses,
    const witness_t *witness)
{

	saw_lock_error = true;
}

static void
witness_owner_error_intercept(const witness_t *witness)
{

	saw_owner_error = true;
}

static void
witness_not_owner_error_intercept(const witness_t *witness)
{

	saw_not_owner_error = true;
}

static void
witness_lockless_error_intercept(const witness_list_t *witnesses)
{

	saw_lockless_error = true;
}

static int
witness_comp(const witness_t *a, const witness_t *b)
{

	assert_u_eq(a->rank, b->rank, "Witnesses should have equal rank");

	return (strcmp(a->name, b->name));
}

static int
witness_comp_reverse(const witness_t *a, const witness_t *b)
{

	assert_u_eq(a->rank, b->rank, "Witnesses should have equal rank");

	return (-strcmp(a->name, b->name));
}

TEST_BEGIN(test_witness)
{
	witness_t a, b;
	tsdn_t *tsdn;

	test_skip_if(!config_debug);

	tsdn = tsdn_fetch();

	witness_assert_lockless(tsdn);

	witness_init(&a, "a", 1, NULL);
	witness_assert_not_owner(tsdn, &a);
	witness_lock(tsdn, &a);
	witness_assert_owner(tsdn, &a);

	witness_init(&b, "b", 2, NULL);
	witness_assert_not_owner(tsdn, &b);
	witness_lock(tsdn, &b);
	witness_assert_owner(tsdn, &b);

	witness_unlock(tsdn, &a);
	witness_unlock(tsdn, &b);

	witness_assert_lockless(tsdn);
}
TEST_END

TEST_BEGIN(test_witness_comp)
{
	witness_t a, b, c, d;
	tsdn_t *tsdn;

	test_skip_if(!config_debug);

	tsdn = tsdn_fetch();

	witness_assert_lockless(tsdn);

	witness_init(&a, "a", 1, witness_comp);
	witness_assert_not_owner(tsdn, &a);
	witness_lock(tsdn, &a);
	witness_assert_owner(tsdn, &a);

	witness_init(&b, "b", 1, witness_comp);
	witness_assert_not_owner(tsdn, &b);
	witness_lock(tsdn, &b);
	witness_assert_owner(tsdn, &b);
	witness_unlock(tsdn, &b);

	witness_lock_error_orig = witness_lock_error;
	witness_lock_error = witness_lock_error_intercept;
	saw_lock_error = false;

	witness_init(&c, "c", 1, witness_comp_reverse);
	witness_assert_not_owner(tsdn, &c);
	assert_false(saw_lock_error, "Unexpected witness lock error");
	witness_lock(tsdn, &c);
	assert_true(saw_lock_error, "Expected witness lock error");
	witness_unlock(tsdn, &c);

	saw_lock_error = false;

	witness_init(&d, "d", 1, NULL);
	witness_assert_not_owner(tsdn, &d);
	assert_false(saw_lock_error, "Unexpected witness lock error");
	witness_lock(tsdn, &d);
	assert_true(saw_lock_error, "Expected witness lock error");
	witness_unlock(tsdn, &d);

	witness_unlock(tsdn, &a);

	witness_assert_lockless(tsdn);

	witness_lock_error = witness_lock_error_orig;
}
TEST_END

TEST_BEGIN(test_witness_reversal)
{
	witness_t a, b;
	tsdn_t *tsdn;

	test_skip_if(!config_debug);

	witness_lock_error_orig = witness_lock_error;
	witness_lock_error = witness_lock_error_intercept;
	saw_lock_error = false;

	tsdn = tsdn_fetch();

	witness_assert_lockless(tsdn);

	witness_init(&a, "a", 1, NULL);
	witness_init(&b, "b", 2, NULL);

	witness_lock(tsdn, &b);
	assert_false(saw_lock_error, "Unexpected witness lock error");
	witness_lock(tsdn, &a);
	assert_true(saw_lock_error, "Expected witness lock error");

	witness_unlock(tsdn, &a);
	witness_unlock(tsdn, &b);

	witness_assert_lockless(tsdn);

	witness_lock_error = witness_lock_error_orig;
}
TEST_END

TEST_BEGIN(test_witness_recursive)
{
	witness_t a;
	tsdn_t *tsdn;

	test_skip_if(!config_debug);

	witness_not_owner_error_orig = witness_not_owner_error;
	witness_not_owner_error = witness_not_owner_error_intercept;
	saw_not_owner_error = false;

	witness_lock_error_orig = witness_lock_error;
	witness_lock_error = witness_lock_error_intercept;
	saw_lock_error = false;

	tsdn = tsdn_fetch();

	witness_assert_lockless(tsdn);

	witness_init(&a, "a", 1, NULL);

	witness_lock(tsdn, &a);
	assert_false(saw_lock_error, "Unexpected witness lock error");
	assert_false(saw_not_owner_error, "Unexpected witness not owner error");
	witness_lock(tsdn, &a);
	assert_true(saw_lock_error, "Expected witness lock error");
	assert_true(saw_not_owner_error, "Expected witness not owner error");

	witness_unlock(tsdn, &a);

	witness_assert_lockless(tsdn);

	witness_owner_error = witness_owner_error_orig;
	witness_lock_error = witness_lock_error_orig;

}
TEST_END

TEST_BEGIN(test_witness_unlock_not_owned)
{
	witness_t a;
	tsdn_t *tsdn;

	test_skip_if(!config_debug);

	witness_owner_error_orig = witness_owner_error;
	witness_owner_error = witness_owner_error_intercept;
	saw_owner_error = false;

	tsdn = tsdn_fetch();

	witness_assert_lockless(tsdn);

	witness_init(&a, "a", 1, NULL);

	assert_false(saw_owner_error, "Unexpected owner error");
	witness_unlock(tsdn, &a);
	assert_true(saw_owner_error, "Expected owner error");

	witness_assert_lockless(tsdn);

	witness_owner_error = witness_owner_error_orig;
}
TEST_END

TEST_BEGIN(test_witness_lockful)
{
	witness_t a;
	tsdn_t *tsdn;

	test_skip_if(!config_debug);

	witness_lockless_error_orig = witness_lockless_error;
	witness_lockless_error = witness_lockless_error_intercept;
	saw_lockless_error = false;

	tsdn = tsdn_fetch();

	witness_assert_lockless(tsdn);

	witness_init(&a, "a", 1, NULL);

	assert_false(saw_lockless_error, "Unexpected lockless error");
	witness_assert_lockless(tsdn);

	witness_lock(tsdn, &a);
	witness_assert_lockless(tsdn);
	assert_true(saw_lockless_error, "Expected lockless error");

	witness_unlock(tsdn, &a);

	witness_assert_lockless(tsdn);

	witness_lockless_error = witness_lockless_error_orig;
}
TEST_END

int
main(void)
{

	return (test(
	    test_witness,
	    test_witness_comp,
	    test_witness_reversal,
	    test_witness_recursive,
	    test_witness_unlock_not_owned,
	    test_witness_lockful));
}

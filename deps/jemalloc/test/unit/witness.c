#include "test/jemalloc_test.h"

static witness_lock_error_t *witness_lock_error_orig;
static witness_owner_error_t *witness_owner_error_orig;
static witness_not_owner_error_t *witness_not_owner_error_orig;
static witness_depth_error_t *witness_depth_error_orig;

static bool saw_lock_error;
static bool saw_owner_error;
static bool saw_not_owner_error;
static bool saw_depth_error;

static void
witness_lock_error_intercept(const witness_list_t *witnesses,
    const witness_t *witness) {
	saw_lock_error = true;
}

static void
witness_owner_error_intercept(const witness_t *witness) {
	saw_owner_error = true;
}

static void
witness_not_owner_error_intercept(const witness_t *witness) {
	saw_not_owner_error = true;
}

static void
witness_depth_error_intercept(const witness_list_t *witnesses,
    witness_rank_t rank_inclusive, unsigned depth) {
	saw_depth_error = true;
}

static int
witness_comp(const witness_t *a, void *oa, const witness_t *b, void *ob) {
	assert_u_eq(a->rank, b->rank, "Witnesses should have equal rank");

	assert(oa == (void *)a);
	assert(ob == (void *)b);

	return strcmp(a->name, b->name);
}

static int
witness_comp_reverse(const witness_t *a, void *oa, const witness_t *b,
    void *ob) {
	assert_u_eq(a->rank, b->rank, "Witnesses should have equal rank");

	assert(oa == (void *)a);
	assert(ob == (void *)b);

	return -strcmp(a->name, b->name);
}

TEST_BEGIN(test_witness) {
	witness_t a, b;
	witness_tsdn_t witness_tsdn = { WITNESS_TSD_INITIALIZER };

	test_skip_if(!config_debug);

	witness_assert_lockless(&witness_tsdn);
	witness_assert_depth(&witness_tsdn, 0);
	witness_assert_depth_to_rank(&witness_tsdn, (witness_rank_t)1U, 0);

	witness_init(&a, "a", 1, NULL, NULL);
	witness_assert_not_owner(&witness_tsdn, &a);
	witness_lock(&witness_tsdn, &a);
	witness_assert_owner(&witness_tsdn, &a);
	witness_assert_depth(&witness_tsdn, 1);
	witness_assert_depth_to_rank(&witness_tsdn, (witness_rank_t)1U, 1);
	witness_assert_depth_to_rank(&witness_tsdn, (witness_rank_t)2U, 0);

	witness_init(&b, "b", 2, NULL, NULL);
	witness_assert_not_owner(&witness_tsdn, &b);
	witness_lock(&witness_tsdn, &b);
	witness_assert_owner(&witness_tsdn, &b);
	witness_assert_depth(&witness_tsdn, 2);
	witness_assert_depth_to_rank(&witness_tsdn, (witness_rank_t)1U, 2);
	witness_assert_depth_to_rank(&witness_tsdn, (witness_rank_t)2U, 1);
	witness_assert_depth_to_rank(&witness_tsdn, (witness_rank_t)3U, 0);

	witness_unlock(&witness_tsdn, &a);
	witness_assert_depth(&witness_tsdn, 1);
	witness_assert_depth_to_rank(&witness_tsdn, (witness_rank_t)1U, 1);
	witness_assert_depth_to_rank(&witness_tsdn, (witness_rank_t)2U, 1);
	witness_assert_depth_to_rank(&witness_tsdn, (witness_rank_t)3U, 0);
	witness_unlock(&witness_tsdn, &b);

	witness_assert_lockless(&witness_tsdn);
	witness_assert_depth(&witness_tsdn, 0);
	witness_assert_depth_to_rank(&witness_tsdn, (witness_rank_t)1U, 0);
}
TEST_END

TEST_BEGIN(test_witness_comp) {
	witness_t a, b, c, d;
	witness_tsdn_t witness_tsdn = { WITNESS_TSD_INITIALIZER };

	test_skip_if(!config_debug);

	witness_assert_lockless(&witness_tsdn);

	witness_init(&a, "a", 1, witness_comp, &a);
	witness_assert_not_owner(&witness_tsdn, &a);
	witness_lock(&witness_tsdn, &a);
	witness_assert_owner(&witness_tsdn, &a);
	witness_assert_depth(&witness_tsdn, 1);

	witness_init(&b, "b", 1, witness_comp, &b);
	witness_assert_not_owner(&witness_tsdn, &b);
	witness_lock(&witness_tsdn, &b);
	witness_assert_owner(&witness_tsdn, &b);
	witness_assert_depth(&witness_tsdn, 2);
	witness_unlock(&witness_tsdn, &b);
	witness_assert_depth(&witness_tsdn, 1);

	witness_lock_error_orig = witness_lock_error;
	witness_lock_error = witness_lock_error_intercept;
	saw_lock_error = false;

	witness_init(&c, "c", 1, witness_comp_reverse, &c);
	witness_assert_not_owner(&witness_tsdn, &c);
	assert_false(saw_lock_error, "Unexpected witness lock error");
	witness_lock(&witness_tsdn, &c);
	assert_true(saw_lock_error, "Expected witness lock error");
	witness_unlock(&witness_tsdn, &c);
	witness_assert_depth(&witness_tsdn, 1);

	saw_lock_error = false;

	witness_init(&d, "d", 1, NULL, NULL);
	witness_assert_not_owner(&witness_tsdn, &d);
	assert_false(saw_lock_error, "Unexpected witness lock error");
	witness_lock(&witness_tsdn, &d);
	assert_true(saw_lock_error, "Expected witness lock error");
	witness_unlock(&witness_tsdn, &d);
	witness_assert_depth(&witness_tsdn, 1);

	witness_unlock(&witness_tsdn, &a);

	witness_assert_lockless(&witness_tsdn);

	witness_lock_error = witness_lock_error_orig;
}
TEST_END

TEST_BEGIN(test_witness_reversal) {
	witness_t a, b;
	witness_tsdn_t witness_tsdn = { WITNESS_TSD_INITIALIZER };

	test_skip_if(!config_debug);

	witness_lock_error_orig = witness_lock_error;
	witness_lock_error = witness_lock_error_intercept;
	saw_lock_error = false;

	witness_assert_lockless(&witness_tsdn);

	witness_init(&a, "a", 1, NULL, NULL);
	witness_init(&b, "b", 2, NULL, NULL);

	witness_lock(&witness_tsdn, &b);
	witness_assert_depth(&witness_tsdn, 1);
	assert_false(saw_lock_error, "Unexpected witness lock error");
	witness_lock(&witness_tsdn, &a);
	assert_true(saw_lock_error, "Expected witness lock error");

	witness_unlock(&witness_tsdn, &a);
	witness_assert_depth(&witness_tsdn, 1);
	witness_unlock(&witness_tsdn, &b);

	witness_assert_lockless(&witness_tsdn);

	witness_lock_error = witness_lock_error_orig;
}
TEST_END

TEST_BEGIN(test_witness_recursive) {
	witness_t a;
	witness_tsdn_t witness_tsdn = { WITNESS_TSD_INITIALIZER };

	test_skip_if(!config_debug);

	witness_not_owner_error_orig = witness_not_owner_error;
	witness_not_owner_error = witness_not_owner_error_intercept;
	saw_not_owner_error = false;

	witness_lock_error_orig = witness_lock_error;
	witness_lock_error = witness_lock_error_intercept;
	saw_lock_error = false;

	witness_assert_lockless(&witness_tsdn);

	witness_init(&a, "a", 1, NULL, NULL);

	witness_lock(&witness_tsdn, &a);
	assert_false(saw_lock_error, "Unexpected witness lock error");
	assert_false(saw_not_owner_error, "Unexpected witness not owner error");
	witness_lock(&witness_tsdn, &a);
	assert_true(saw_lock_error, "Expected witness lock error");
	assert_true(saw_not_owner_error, "Expected witness not owner error");

	witness_unlock(&witness_tsdn, &a);

	witness_assert_lockless(&witness_tsdn);

	witness_owner_error = witness_owner_error_orig;
	witness_lock_error = witness_lock_error_orig;

}
TEST_END

TEST_BEGIN(test_witness_unlock_not_owned) {
	witness_t a;
	witness_tsdn_t witness_tsdn = { WITNESS_TSD_INITIALIZER };

	test_skip_if(!config_debug);

	witness_owner_error_orig = witness_owner_error;
	witness_owner_error = witness_owner_error_intercept;
	saw_owner_error = false;

	witness_assert_lockless(&witness_tsdn);

	witness_init(&a, "a", 1, NULL, NULL);

	assert_false(saw_owner_error, "Unexpected owner error");
	witness_unlock(&witness_tsdn, &a);
	assert_true(saw_owner_error, "Expected owner error");

	witness_assert_lockless(&witness_tsdn);

	witness_owner_error = witness_owner_error_orig;
}
TEST_END

TEST_BEGIN(test_witness_depth) {
	witness_t a;
	witness_tsdn_t witness_tsdn = { WITNESS_TSD_INITIALIZER };

	test_skip_if(!config_debug);

	witness_depth_error_orig = witness_depth_error;
	witness_depth_error = witness_depth_error_intercept;
	saw_depth_error = false;

	witness_assert_lockless(&witness_tsdn);
	witness_assert_depth(&witness_tsdn, 0);

	witness_init(&a, "a", 1, NULL, NULL);

	assert_false(saw_depth_error, "Unexpected depth error");
	witness_assert_lockless(&witness_tsdn);
	witness_assert_depth(&witness_tsdn, 0);

	witness_lock(&witness_tsdn, &a);
	witness_assert_lockless(&witness_tsdn);
	witness_assert_depth(&witness_tsdn, 0);
	assert_true(saw_depth_error, "Expected depth error");

	witness_unlock(&witness_tsdn, &a);

	witness_assert_lockless(&witness_tsdn);
	witness_assert_depth(&witness_tsdn, 0);

	witness_depth_error = witness_depth_error_orig;
}
TEST_END

int
main(void) {
	return test(
	    test_witness,
	    test_witness_comp,
	    test_witness_reversal,
	    test_witness_recursive,
	    test_witness_unlock_not_owned,
	    test_witness_depth);
}

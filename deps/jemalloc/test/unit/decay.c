#include "test/jemalloc_test.h"

#include "jemalloc/internal/decay.h"

TEST_BEGIN(test_decay_init) {
	decay_t decay;
	memset(&decay, 0, sizeof(decay));

	nstime_t curtime;
	nstime_init(&curtime, 0);

	ssize_t decay_ms = 1000;
	assert_true(decay_ms_valid(decay_ms), "");

	expect_false(decay_init(&decay, &curtime, decay_ms),
	    "Failed to initialize decay");
	expect_zd_eq(decay_ms_read(&decay), decay_ms,
	    "Decay_ms was initialized incorrectly");
	expect_u64_ne(decay_epoch_duration_ns(&decay), 0,
	    "Epoch duration was initialized incorrectly");
}
TEST_END

TEST_BEGIN(test_decay_ms_valid) {
	expect_false(decay_ms_valid(-7),
	    "Misclassified negative decay as valid");
	expect_true(decay_ms_valid(-1),
	    "Misclassified -1 (never decay) as invalid decay");
	expect_true(decay_ms_valid(8943),
	    "Misclassified valid decay");
	if (SSIZE_MAX > NSTIME_SEC_MAX) {
		expect_false(
		    decay_ms_valid((ssize_t)(NSTIME_SEC_MAX * KQU(1000) + 39)),
		    "Misclassified too large decay");
	}
}
TEST_END

TEST_BEGIN(test_decay_npages_purge_in) {
	decay_t decay;
	memset(&decay, 0, sizeof(decay));

	nstime_t curtime;
	nstime_init(&curtime, 0);

	uint64_t decay_ms = 1000;
	nstime_t decay_nstime;
	nstime_init(&decay_nstime, decay_ms * 1000 * 1000);
	expect_false(decay_init(&decay, &curtime, (ssize_t)decay_ms),
	    "Failed to initialize decay");

	size_t new_pages = 100;

	nstime_t time;
	nstime_copy(&time, &decay_nstime);
	expect_u64_eq(decay_npages_purge_in(&decay, &time, new_pages),
	    new_pages, "Not all pages are expected to decay in decay_ms");

	nstime_init(&time, 0);
	expect_u64_eq(decay_npages_purge_in(&decay, &time, new_pages), 0,
	    "More than zero pages are expected to instantly decay");

	nstime_copy(&time, &decay_nstime);
	nstime_idivide(&time, 2);
	expect_u64_eq(decay_npages_purge_in(&decay, &time, new_pages),
	    new_pages / 2, "Not half of pages decay in half the decay period");
}
TEST_END

TEST_BEGIN(test_decay_maybe_advance_epoch) {
	decay_t decay;
	memset(&decay, 0, sizeof(decay));

	nstime_t curtime;
	nstime_init(&curtime, 0);

	uint64_t decay_ms = 1000;

	bool err = decay_init(&decay, &curtime, (ssize_t)decay_ms);
	expect_false(err, "");

	bool advanced;
	advanced = decay_maybe_advance_epoch(&decay, &curtime, 0);
	expect_false(advanced, "Epoch advanced while time didn't");

	nstime_t interval;
	nstime_init(&interval, decay_epoch_duration_ns(&decay));

	nstime_add(&curtime, &interval);
	advanced = decay_maybe_advance_epoch(&decay, &curtime, 0);
	expect_false(advanced, "Epoch advanced after first interval");

	nstime_add(&curtime, &interval);
	advanced = decay_maybe_advance_epoch(&decay, &curtime, 0);
	expect_true(advanced, "Epoch didn't advance after two intervals");
}
TEST_END

TEST_BEGIN(test_decay_empty) {
	/* If we never have any decaying pages, npages_limit should be 0. */
	decay_t decay;
	memset(&decay, 0, sizeof(decay));

	nstime_t curtime;
	nstime_init(&curtime, 0);

	uint64_t decay_ms = 1000;
	uint64_t decay_ns = decay_ms * 1000 * 1000;

	bool err = decay_init(&decay, &curtime, (ssize_t)decay_ms);
	assert_false(err, "");

	uint64_t time_between_calls = decay_epoch_duration_ns(&decay) / 5;
	int nepochs = 0;
	for (uint64_t i = 0; i < decay_ns / time_between_calls * 10; i++) {
		size_t dirty_pages = 0;
		nstime_init(&curtime, i * time_between_calls);
		bool epoch_advanced = decay_maybe_advance_epoch(&decay,
		    &curtime, dirty_pages);
		if (epoch_advanced) {
			nepochs++;
			expect_zu_eq(decay_npages_limit_get(&decay), 0,
			    "Unexpectedly increased npages_limit");
		}
	}
	expect_d_gt(nepochs, 0, "Epochs never advanced");
}
TEST_END

/*
 * Verify that npages_limit correctly decays as the time goes.
 *
 * During first 'nepoch_init' epochs, add new dirty pages.
 * After that, let them decay and verify npages_limit decreases.
 * Then proceed with another 'nepoch_init' epochs and check that
 * all dirty pages are flushed out of backlog, bringing npages_limit
 * down to zero.
 */
TEST_BEGIN(test_decay) {
	const uint64_t nepoch_init = 10;

	decay_t decay;
	memset(&decay, 0, sizeof(decay));

	nstime_t curtime;
	nstime_init(&curtime, 0);

	uint64_t decay_ms = 1000;
	uint64_t decay_ns = decay_ms * 1000 * 1000;

	bool err = decay_init(&decay, &curtime, (ssize_t)decay_ms);
	assert_false(err, "");

	expect_zu_eq(decay_npages_limit_get(&decay), 0,
	    "Empty decay returned nonzero npages_limit");

	nstime_t epochtime;
	nstime_init(&epochtime, decay_epoch_duration_ns(&decay));

	const size_t dirty_pages_per_epoch = 1000;
	size_t dirty_pages = 0;
	uint64_t epoch_ns = decay_epoch_duration_ns(&decay);
	bool epoch_advanced = false;

	/* Populate backlog with some dirty pages */
	for (uint64_t i = 0; i < nepoch_init; i++) {
		nstime_add(&curtime, &epochtime);
		dirty_pages += dirty_pages_per_epoch;
		epoch_advanced |= decay_maybe_advance_epoch(&decay, &curtime,
		    dirty_pages);
	}
	expect_true(epoch_advanced, "Epoch never advanced");

	size_t npages_limit = decay_npages_limit_get(&decay);
	expect_zu_gt(npages_limit, 0, "npages_limit is incorrectly equal "
	    "to zero after dirty pages have been added");

	/* Keep dirty pages unchanged and verify that npages_limit decreases */
	for (uint64_t i = nepoch_init; i * epoch_ns < decay_ns; ++i) {
		nstime_add(&curtime, &epochtime);
		epoch_advanced = decay_maybe_advance_epoch(&decay, &curtime,
				    dirty_pages);
		if (epoch_advanced) {
			size_t npages_limit_new = decay_npages_limit_get(&decay);
			expect_zu_lt(npages_limit_new, npages_limit,
			    "napges_limit failed to decay");

			npages_limit = npages_limit_new;
		}
	}

	expect_zu_gt(npages_limit, 0, "npages_limit decayed to zero earlier "
	    "than decay_ms since last dirty page was added");

	/* Completely push all dirty pages out of the backlog */
	epoch_advanced = false;
	for (uint64_t i = 0; i < nepoch_init; i++) {
		nstime_add(&curtime, &epochtime);
		epoch_advanced |= decay_maybe_advance_epoch(&decay, &curtime,
		    dirty_pages);
	}
	expect_true(epoch_advanced, "Epoch never advanced");

	npages_limit = decay_npages_limit_get(&decay);
	expect_zu_eq(npages_limit, 0, "npages_limit didn't decay to 0 after "
	    "decay_ms since last bump in dirty pages");
}
TEST_END

TEST_BEGIN(test_decay_ns_until_purge) {
	const uint64_t nepoch_init = 10;

	decay_t decay;
	memset(&decay, 0, sizeof(decay));

	nstime_t curtime;
	nstime_init(&curtime, 0);

	uint64_t decay_ms = 1000;
	uint64_t decay_ns = decay_ms * 1000 * 1000;

	bool err = decay_init(&decay, &curtime, (ssize_t)decay_ms);
	assert_false(err, "");

	nstime_t epochtime;
	nstime_init(&epochtime, decay_epoch_duration_ns(&decay));

	uint64_t ns_until_purge_empty = decay_ns_until_purge(&decay, 0, 0);
	expect_u64_eq(ns_until_purge_empty, DECAY_UNBOUNDED_TIME_TO_PURGE,
	    "Failed to return unbounded wait time for zero threshold");

	const size_t dirty_pages_per_epoch = 1000;
	size_t dirty_pages = 0;
	bool epoch_advanced = false;
	for (uint64_t i = 0; i < nepoch_init; i++) {
		nstime_add(&curtime, &epochtime);
		dirty_pages += dirty_pages_per_epoch;
		epoch_advanced |= decay_maybe_advance_epoch(&decay, &curtime,
		    dirty_pages);
	}
	expect_true(epoch_advanced, "Epoch never advanced");

	uint64_t ns_until_purge_all = decay_ns_until_purge(&decay,
	    dirty_pages, dirty_pages);
	expect_u64_ge(ns_until_purge_all, decay_ns,
	    "Incorrectly calculated time to purge all pages");

	uint64_t ns_until_purge_none = decay_ns_until_purge(&decay,
	    dirty_pages, 0);
	expect_u64_eq(ns_until_purge_none, decay_epoch_duration_ns(&decay) * 2,
	    "Incorrectly calculated time to purge 0 pages");

	uint64_t npages_threshold = dirty_pages / 2;
	uint64_t ns_until_purge_half = decay_ns_until_purge(&decay,
	    dirty_pages, npages_threshold);

	nstime_t waittime;
	nstime_init(&waittime, ns_until_purge_half);
	nstime_add(&curtime, &waittime);

	decay_maybe_advance_epoch(&decay, &curtime, dirty_pages);
	size_t npages_limit = decay_npages_limit_get(&decay);
	expect_zu_lt(npages_limit, dirty_pages,
	    "npages_limit failed to decrease after waiting");
	size_t expected = dirty_pages - npages_limit;
	int deviation = abs((int)expected - (int)(npages_threshold));
	expect_d_lt(deviation, (int)(npages_threshold / 2),
	    "After waiting, number of pages is out of the expected interval "
	    "[0.5 * npages_threshold .. 1.5 * npages_threshold]");
}
TEST_END

int
main(void) {
	return test(
	    test_decay_init,
	    test_decay_ms_valid,
	    test_decay_npages_purge_in,
	    test_decay_maybe_advance_epoch,
	    test_decay_empty,
	    test_decay,
	    test_decay_ns_until_purge);
}

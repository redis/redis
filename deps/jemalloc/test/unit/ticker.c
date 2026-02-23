#include "test/jemalloc_test.h"

#include "jemalloc/internal/ticker.h"

TEST_BEGIN(test_ticker_tick) {
#define NREPS 2
#define NTICKS 3
	ticker_t ticker;
	int32_t i, j;

	ticker_init(&ticker, NTICKS);
	for (i = 0; i < NREPS; i++) {
		for (j = 0; j < NTICKS; j++) {
			expect_u_eq(ticker_read(&ticker), NTICKS - j,
			    "Unexpected ticker value (i=%d, j=%d)", i, j);
			expect_false(ticker_tick(&ticker),
			    "Unexpected ticker fire (i=%d, j=%d)", i, j);
		}
		expect_u32_eq(ticker_read(&ticker), 0,
		    "Expected ticker depletion");
		expect_true(ticker_tick(&ticker),
		    "Expected ticker fire (i=%d)", i);
		expect_u32_eq(ticker_read(&ticker), NTICKS,
		    "Expected ticker reset");
	}
#undef NTICKS
}
TEST_END

TEST_BEGIN(test_ticker_ticks) {
#define NTICKS 3
	ticker_t ticker;

	ticker_init(&ticker, NTICKS);

	expect_u_eq(ticker_read(&ticker), NTICKS, "Unexpected ticker value");
	expect_false(ticker_ticks(&ticker, NTICKS), "Unexpected ticker fire");
	expect_u_eq(ticker_read(&ticker), 0, "Unexpected ticker value");
	expect_true(ticker_ticks(&ticker, NTICKS), "Expected ticker fire");
	expect_u_eq(ticker_read(&ticker), NTICKS, "Unexpected ticker value");

	expect_true(ticker_ticks(&ticker, NTICKS + 1), "Expected ticker fire");
	expect_u_eq(ticker_read(&ticker), NTICKS, "Unexpected ticker value");
#undef NTICKS
}
TEST_END

TEST_BEGIN(test_ticker_copy) {
#define NTICKS 3
	ticker_t ta, tb;

	ticker_init(&ta, NTICKS);
	ticker_copy(&tb, &ta);
	expect_u_eq(ticker_read(&tb), NTICKS, "Unexpected ticker value");
	expect_true(ticker_ticks(&tb, NTICKS + 1), "Expected ticker fire");
	expect_u_eq(ticker_read(&tb), NTICKS, "Unexpected ticker value");

	ticker_tick(&ta);
	ticker_copy(&tb, &ta);
	expect_u_eq(ticker_read(&tb), NTICKS - 1, "Unexpected ticker value");
	expect_true(ticker_ticks(&tb, NTICKS), "Expected ticker fire");
	expect_u_eq(ticker_read(&tb), NTICKS, "Unexpected ticker value");
#undef NTICKS
}
TEST_END

TEST_BEGIN(test_ticker_geom) {
	const int32_t ticks = 100;
	const uint64_t niters = 100 * 1000;

	ticker_geom_t ticker;
	ticker_geom_init(&ticker, ticks);
	uint64_t total_ticks = 0;
	/* Just some random constant. */
	uint64_t prng_state = 0x343219f93496db9fULL;
	for (uint64_t i = 0; i < niters; i++) {
		while(!ticker_geom_tick(&ticker, &prng_state)) {
			total_ticks++;
		}
	}
	/*
	 * In fact, with this choice of random seed and the PRNG implementation
	 * used at the time this was tested, total_ticks is 95.1% of the
	 * expected ticks.
	 */
	expect_u64_ge(total_ticks , niters * ticks * 9 / 10,
	    "Mean off by > 10%%");
	expect_u64_le(total_ticks , niters * ticks * 11 / 10,
	    "Mean off by > 10%%");
}
TEST_END

int
main(void) {
	return test(
	    test_ticker_tick,
	    test_ticker_ticks,
	    test_ticker_copy,
	    test_ticker_geom);
}

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
			assert_u_eq(ticker_read(&ticker), NTICKS - j,
			    "Unexpected ticker value (i=%d, j=%d)", i, j);
			assert_false(ticker_tick(&ticker),
			    "Unexpected ticker fire (i=%d, j=%d)", i, j);
		}
		assert_u32_eq(ticker_read(&ticker), 0,
		    "Expected ticker depletion");
		assert_true(ticker_tick(&ticker),
		    "Expected ticker fire (i=%d)", i);
		assert_u32_eq(ticker_read(&ticker), NTICKS,
		    "Expected ticker reset");
	}
#undef NTICKS
}
TEST_END

TEST_BEGIN(test_ticker_ticks) {
#define NTICKS 3
	ticker_t ticker;

	ticker_init(&ticker, NTICKS);

	assert_u_eq(ticker_read(&ticker), NTICKS, "Unexpected ticker value");
	assert_false(ticker_ticks(&ticker, NTICKS), "Unexpected ticker fire");
	assert_u_eq(ticker_read(&ticker), 0, "Unexpected ticker value");
	assert_true(ticker_ticks(&ticker, NTICKS), "Expected ticker fire");
	assert_u_eq(ticker_read(&ticker), NTICKS, "Unexpected ticker value");

	assert_true(ticker_ticks(&ticker, NTICKS + 1), "Expected ticker fire");
	assert_u_eq(ticker_read(&ticker), NTICKS, "Unexpected ticker value");
#undef NTICKS
}
TEST_END

TEST_BEGIN(test_ticker_copy) {
#define NTICKS 3
	ticker_t ta, tb;

	ticker_init(&ta, NTICKS);
	ticker_copy(&tb, &ta);
	assert_u_eq(ticker_read(&tb), NTICKS, "Unexpected ticker value");
	assert_true(ticker_ticks(&tb, NTICKS + 1), "Expected ticker fire");
	assert_u_eq(ticker_read(&tb), NTICKS, "Unexpected ticker value");

	ticker_tick(&ta);
	ticker_copy(&tb, &ta);
	assert_u_eq(ticker_read(&tb), NTICKS - 1, "Unexpected ticker value");
	assert_true(ticker_ticks(&tb, NTICKS), "Expected ticker fire");
	assert_u_eq(ticker_read(&tb), NTICKS, "Unexpected ticker value");
#undef NTICKS
}
TEST_END

int
main(void) {
	return test(
	    test_ticker_tick,
	    test_ticker_ticks,
	    test_ticker_copy);
}

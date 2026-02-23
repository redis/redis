#include "test/jemalloc_test.h"

#include "jemalloc/internal/peak.h"

TEST_BEGIN(test_peak) {
	peak_t peak = PEAK_INITIALIZER;
	expect_u64_eq(0, peak_max(&peak),
	    "Peak should be zero at initialization");
	peak_update(&peak, 100, 50);
	expect_u64_eq(50, peak_max(&peak),
	    "Missed update");
	peak_update(&peak, 100, 100);
	expect_u64_eq(50, peak_max(&peak), "Dallocs shouldn't change peak");
	peak_update(&peak, 100, 200);
	expect_u64_eq(50, peak_max(&peak), "Dallocs shouldn't change peak");
	peak_update(&peak, 200, 200);
	expect_u64_eq(50, peak_max(&peak), "Haven't reached peak again");
	peak_update(&peak, 300, 200);
	expect_u64_eq(100, peak_max(&peak), "Missed an update.");
	peak_set_zero(&peak, 300, 200);
	expect_u64_eq(0, peak_max(&peak), "No effect from zeroing");
	peak_update(&peak, 300, 300);
	expect_u64_eq(0, peak_max(&peak), "Dalloc shouldn't change peak");
	peak_update(&peak, 400, 300);
	expect_u64_eq(0, peak_max(&peak), "Should still be net negative");
	peak_update(&peak, 500, 300);
	expect_u64_eq(100, peak_max(&peak), "Missed an update.");
	/*
	 * Above, we set to zero while a net allocator; let's try as a
	 * net-deallocator.
	 */
	peak_set_zero(&peak, 600, 700);
	expect_u64_eq(0, peak_max(&peak), "No effect from zeroing.");
	peak_update(&peak, 600, 800);
	expect_u64_eq(0, peak_max(&peak), "Dalloc shouldn't change peak.");
	peak_update(&peak, 700, 800);
	expect_u64_eq(0, peak_max(&peak), "Should still be net negative.");
	peak_update(&peak, 800, 800);
	expect_u64_eq(100, peak_max(&peak), "Missed an update.");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_peak);
}

#include "test/jemalloc_test.h"

#include "jemalloc/internal/spin.h"

TEST_BEGIN(test_spin) {
	spin_t spinner = SPIN_INITIALIZER;

	for (unsigned i = 0; i < 100; i++) {
		spin_adaptive(&spinner);
	}
}
TEST_END

int
main(void) {
	return test(
	    test_spin);
}

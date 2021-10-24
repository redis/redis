#include "test/jemalloc_test.h"

TEST_BEGIN(test_update_slab_size) {
	sc_data_t data;
	memset(&data, 0, sizeof(data));
	sc_data_init(&data);
	sc_t *tiny = &data.sc[0];
	size_t tiny_size = (ZU(1) << tiny->lg_base)
	    + (ZU(tiny->ndelta) << tiny->lg_delta);
	size_t pgs_too_big = (tiny_size * BITMAP_MAXBITS + PAGE - 1) / PAGE + 1;
	sc_data_update_slab_size(&data, tiny_size, tiny_size, (int)pgs_too_big);
	assert_zu_lt((size_t)tiny->pgs, pgs_too_big, "Allowed excessive pages");

	sc_data_update_slab_size(&data, 1, 10 * PAGE, 1);
	for (int i = 0; i < data.nbins; i++) {
		sc_t *sc = &data.sc[i];
		size_t reg_size = (ZU(1) << sc->lg_base)
		    + (ZU(sc->ndelta) << sc->lg_delta);
		if (reg_size <= PAGE) {
			assert_d_eq(sc->pgs, 1, "Ignored valid page size hint");
		} else {
			assert_d_gt(sc->pgs, 1,
			    "Allowed invalid page size hint");
		}
	}
}
TEST_END

int
main(void) {
	return test(
	    test_update_slab_size);
}

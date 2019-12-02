#include "test/jemalloc_test.h"

TEST_BEGIN(test_arena_slab_regind) {
	szind_t binind;

	for (binind = 0; binind < NBINS; binind++) {
		size_t regind;
		extent_t slab;
		const bin_info_t *bin_info = &bin_infos[binind];
		extent_init(&slab, NULL, mallocx(bin_info->slab_size,
		    MALLOCX_LG_ALIGN(LG_PAGE)), bin_info->slab_size, true,
		    binind, 0, extent_state_active, false, true, true);
		assert_ptr_not_null(extent_addr_get(&slab),
		    "Unexpected malloc() failure");
		for (regind = 0; regind < bin_info->nregs; regind++) {
			void *reg = (void *)((uintptr_t)extent_addr_get(&slab) +
			    (bin_info->reg_size * regind));
			assert_zu_eq(arena_slab_regind(&slab, binind, reg),
			    regind,
			    "Incorrect region index computed for size %zu",
			    bin_info->reg_size);
		}
		free(extent_addr_get(&slab));
	}
}
TEST_END

int
main(void) {
	return test(
	    test_arena_slab_regind);
}

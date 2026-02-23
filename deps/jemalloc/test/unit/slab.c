#include "test/jemalloc_test.h"

#define INVALID_ARENA_IND ((1U << MALLOCX_ARENA_BITS) - 1)

TEST_BEGIN(test_arena_slab_regind) {
	szind_t binind;

	for (binind = 0; binind < SC_NBINS; binind++) {
		size_t regind;
		edata_t slab;
		const bin_info_t *bin_info = &bin_infos[binind];
		edata_init(&slab, INVALID_ARENA_IND,
		    mallocx(bin_info->slab_size, MALLOCX_LG_ALIGN(LG_PAGE)),
		    bin_info->slab_size, true,
		    binind, 0, extent_state_active, false, true, EXTENT_PAI_PAC,
		    EXTENT_NOT_HEAD);
		expect_ptr_not_null(edata_addr_get(&slab),
		    "Unexpected malloc() failure");
		arena_dalloc_bin_locked_info_t dalloc_info;
		arena_dalloc_bin_locked_begin(&dalloc_info, binind);
		for (regind = 0; regind < bin_info->nregs; regind++) {
			void *reg = (void *)((uintptr_t)edata_addr_get(&slab) +
			    (bin_info->reg_size * regind));
			expect_zu_eq(arena_slab_regind(&dalloc_info, binind,
			    &slab, reg),
			    regind,
			    "Incorrect region index computed for size %zu",
			    bin_info->reg_size);
		}
		free(edata_addr_get(&slab));
	}
}
TEST_END

int
main(void) {
	return test(
	    test_arena_slab_regind);
}

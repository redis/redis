#include "test/jemalloc_test.h"

#define BATCH_MAX ((1U << 16) + 1024)
static void *global_ptrs[BATCH_MAX];

#define PAGE_ALIGNED(ptr) (((uintptr_t)ptr & PAGE_MASK) == 0)

static void
verify_batch_basic(tsd_t *tsd, void **ptrs, size_t batch, size_t usize,
    bool zero) {
	for (size_t i = 0; i < batch; ++i) {
		void *p = ptrs[i];
		expect_zu_eq(isalloc(tsd_tsdn(tsd), p), usize, "");
		if (zero) {
			for (size_t k = 0; k < usize; ++k) {
				expect_true(*((unsigned char *)p + k) == 0, "");
			}
		}
	}
}

static void
verify_batch_locality(tsd_t *tsd, void **ptrs, size_t batch, size_t usize,
    arena_t *arena, unsigned nregs) {
	if (config_prof && opt_prof) {
		/*
		 * Checking batch locality when prof is on is feasible but
		 * complicated, while checking the non-prof case suffices for
		 * unit-test purpose.
		 */
		return;
	}
	for (size_t i = 0, j = 0; i < batch; ++i, ++j) {
		if (j == nregs) {
			j = 0;
		}
		if (j == 0 && batch - i < nregs) {
			break;
		}
		void *p = ptrs[i];
		expect_ptr_eq(iaalloc(tsd_tsdn(tsd), p), arena, "");
		if (j == 0) {
			expect_true(PAGE_ALIGNED(p), "");
			continue;
		}
		assert(i > 0);
		void *q = ptrs[i - 1];
		expect_true((uintptr_t)p > (uintptr_t)q
		    && (size_t)((uintptr_t)p - (uintptr_t)q) == usize, "");
	}
}

static void
release_batch(void **ptrs, size_t batch, size_t size) {
	for (size_t i = 0; i < batch; ++i) {
		sdallocx(ptrs[i], size, 0);
	}
}

typedef struct batch_alloc_packet_s batch_alloc_packet_t;
struct batch_alloc_packet_s {
	void **ptrs;
	size_t num;
	size_t size;
	int flags;
};

static size_t
batch_alloc_wrapper(void **ptrs, size_t num, size_t size, int flags) {
	batch_alloc_packet_t batch_alloc_packet = {ptrs, num, size, flags};
	size_t filled;
	size_t len = sizeof(size_t);
	assert_d_eq(mallctl("experimental.batch_alloc", &filled, &len,
	    &batch_alloc_packet, sizeof(batch_alloc_packet)), 0, "");
	return filled;
}

static void
test_wrapper(size_t size, size_t alignment, bool zero, unsigned arena_flag) {
	tsd_t *tsd = tsd_fetch();
	assert(tsd != NULL);
	const size_t usize =
	    (alignment != 0 ? sz_sa2u(size, alignment) : sz_s2u(size));
	const szind_t ind = sz_size2index(usize);
	const bin_info_t *bin_info = &bin_infos[ind];
	const unsigned nregs = bin_info->nregs;
	assert(nregs > 0);
	arena_t *arena;
	if (arena_flag != 0) {
		arena = arena_get(tsd_tsdn(tsd), MALLOCX_ARENA_GET(arena_flag),
		    false);
	} else {
		arena = arena_choose(tsd, NULL);
	}
	assert(arena != NULL);
	int flags = arena_flag;
	if (alignment != 0) {
		flags |= MALLOCX_ALIGN(alignment);
	}
	if (zero) {
		flags |= MALLOCX_ZERO;
	}

	/*
	 * Allocate for the purpose of bootstrapping arena_tdata, so that the
	 * change in bin stats won't contaminate the stats to be verified below.
	 */
	void *p = mallocx(size, flags | MALLOCX_TCACHE_NONE);

	for (size_t i = 0; i < 4; ++i) {
		size_t base = 0;
		if (i == 1) {
			base = nregs;
		} else if (i == 2) {
			base = nregs * 2;
		} else if (i == 3) {
			base = (1 << 16);
		}
		for (int j = -1; j <= 1; ++j) {
			if (base == 0 && j == -1) {
				continue;
			}
			size_t batch = base + (size_t)j;
			assert(batch < BATCH_MAX);
			size_t filled = batch_alloc_wrapper(global_ptrs, batch,
			    size, flags);
			assert_zu_eq(filled, batch, "");
			verify_batch_basic(tsd, global_ptrs, batch, usize,
			    zero);
			verify_batch_locality(tsd, global_ptrs, batch, usize,
			    arena, nregs);
			release_batch(global_ptrs, batch, usize);
		}
	}

	free(p);
}

TEST_BEGIN(test_batch_alloc) {
	test_wrapper(11, 0, false, 0);
}
TEST_END

TEST_BEGIN(test_batch_alloc_zero) {
	test_wrapper(11, 0, true, 0);
}
TEST_END

TEST_BEGIN(test_batch_alloc_aligned) {
	test_wrapper(7, 16, false, 0);
}
TEST_END

TEST_BEGIN(test_batch_alloc_manual_arena) {
	unsigned arena_ind;
	size_t len_unsigned = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.create", &arena_ind, &len_unsigned, NULL,
	    0), 0, "");
	test_wrapper(11, 0, false, MALLOCX_ARENA(arena_ind));
}
TEST_END

TEST_BEGIN(test_batch_alloc_large) {
	size_t size = SC_LARGE_MINCLASS;
	for (size_t batch = 0; batch < 4; ++batch) {
		assert(batch < BATCH_MAX);
		size_t filled = batch_alloc(global_ptrs, batch, size, 0);
		assert_zu_eq(filled, batch, "");
		release_batch(global_ptrs, batch, size);
	}
	size = tcache_maxclass + 1;
	for (size_t batch = 0; batch < 4; ++batch) {
		assert(batch < BATCH_MAX);
		size_t filled = batch_alloc(global_ptrs, batch, size, 0);
		assert_zu_eq(filled, batch, "");
		release_batch(global_ptrs, batch, size);
	}
}
TEST_END

int
main(void) {
	return test(
	    test_batch_alloc,
	    test_batch_alloc_zero,
	    test_batch_alloc_aligned,
	    test_batch_alloc_manual_arena,
	    test_batch_alloc_large);
}

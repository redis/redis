#include "test/jemalloc_test.h"

/* Config -- "narenas:1,bin_shards:1-160:16|129-512:4|256-256:8" */

#define NTHREADS 16
#define REMOTE_NALLOC 256

static void *
thd_producer(void *varg) {
	void **mem = varg;
	unsigned arena, i;
	size_t sz;

	sz = sizeof(arena);
	/* Remote arena. */
	expect_d_eq(mallctl("arenas.create", (void *)&arena, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	for (i = 0; i < REMOTE_NALLOC / 2; i++) {
		mem[i] = mallocx(1, MALLOCX_TCACHE_NONE | MALLOCX_ARENA(arena));
	}

	/* Remote bin. */
	for (; i < REMOTE_NALLOC; i++) {
		mem[i] = mallocx(1, MALLOCX_TCACHE_NONE | MALLOCX_ARENA(0));
	}

	return NULL;
}

TEST_BEGIN(test_producer_consumer) {
	thd_t thds[NTHREADS];
	void *mem[NTHREADS][REMOTE_NALLOC];
	unsigned i;

	/* Create producer threads to allocate. */
	for (i = 0; i < NTHREADS; i++) {
		thd_create(&thds[i], thd_producer, mem[i]);
	}
	for (i = 0; i < NTHREADS; i++) {
		thd_join(thds[i], NULL);
	}
	/* Remote deallocation by the current thread. */
	for (i = 0; i < NTHREADS; i++) {
		for (unsigned j = 0; j < REMOTE_NALLOC; j++) {
			expect_ptr_not_null(mem[i][j],
			    "Unexpected remote allocation failure");
			dallocx(mem[i][j], 0);
		}
	}
}
TEST_END

static void *
thd_start(void *varg) {
	void *ptr, *ptr2;
	edata_t *edata;
	unsigned shard1, shard2;

	tsdn_t *tsdn = tsdn_fetch();
	/* Try triggering allocations from sharded bins. */
	for (unsigned i = 0; i < 1024; i++) {
		ptr = mallocx(1, MALLOCX_TCACHE_NONE);
		ptr2 = mallocx(129, MALLOCX_TCACHE_NONE);

		edata = emap_edata_lookup(tsdn, &arena_emap_global, ptr);
		shard1 = edata_binshard_get(edata);
		dallocx(ptr, 0);
		expect_u_lt(shard1, 16, "Unexpected bin shard used");

		edata = emap_edata_lookup(tsdn, &arena_emap_global, ptr2);
		shard2 = edata_binshard_get(edata);
		dallocx(ptr2, 0);
		expect_u_lt(shard2, 4, "Unexpected bin shard used");

		if (shard1 > 0 || shard2 > 0) {
			/* Triggered sharded bin usage. */
			return (void *)(uintptr_t)shard1;
		}
	}

	return NULL;
}

TEST_BEGIN(test_bin_shard_mt) {
	test_skip_if(have_percpu_arena &&
	    PERCPU_ARENA_ENABLED(opt_percpu_arena));

	thd_t thds[NTHREADS];
	unsigned i;
	for (i = 0; i < NTHREADS; i++) {
		thd_create(&thds[i], thd_start, NULL);
	}
	bool sharded = false;
	for (i = 0; i < NTHREADS; i++) {
		void *ret;
		thd_join(thds[i], &ret);
		if (ret != NULL) {
			sharded = true;
		}
	}
	expect_b_eq(sharded, true, "Did not find sharded bins");
}
TEST_END

TEST_BEGIN(test_bin_shard) {
	unsigned nbins, i;
	size_t mib[4], mib2[4];
	size_t miblen, miblen2, len;

	len = sizeof(nbins);
	expect_d_eq(mallctl("arenas.nbins", (void *)&nbins, &len, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	miblen = 4;
	expect_d_eq(mallctlnametomib("arenas.bin.0.nshards", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	miblen2 = 4;
	expect_d_eq(mallctlnametomib("arenas.bin.0.size", mib2, &miblen2), 0,
	    "Unexpected mallctlnametomib() failure");

	for (i = 0; i < nbins; i++) {
		uint32_t nshards;
		size_t size, sz1, sz2;

		mib[2] = i;
		sz1 = sizeof(nshards);
		expect_d_eq(mallctlbymib(mib, miblen, (void *)&nshards, &sz1,
		    NULL, 0), 0, "Unexpected mallctlbymib() failure");

		mib2[2] = i;
		sz2 = sizeof(size);
		expect_d_eq(mallctlbymib(mib2, miblen2, (void *)&size, &sz2,
		    NULL, 0), 0, "Unexpected mallctlbymib() failure");

		if (size >= 1 && size <= 128) {
			expect_u_eq(nshards, 16, "Unexpected nshards");
		} else if (size == 256) {
			expect_u_eq(nshards, 8, "Unexpected nshards");
		} else if (size > 128 && size <= 512) {
			expect_u_eq(nshards, 4, "Unexpected nshards");
		} else {
			expect_u_eq(nshards, 1, "Unexpected nshards");
		}
	}
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_bin_shard,
	    test_bin_shard_mt,
	    test_producer_consumer);
}

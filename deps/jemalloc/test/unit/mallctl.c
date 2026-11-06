#include "test/jemalloc_test.h"

#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/hook.h"
#include "jemalloc/internal/util.h"

TEST_BEGIN(test_mallctl_errors) {
	uint64_t epoch;
	size_t sz;

	expect_d_eq(mallctl("no_such_name", NULL, NULL, NULL, 0), ENOENT,
	    "mallctl() should return ENOENT for non-existent names");

	expect_d_eq(mallctl("version", NULL, NULL, "0.0.0", strlen("0.0.0")),
	    EPERM, "mallctl() should return EPERM on attempt to write "
	    "read-only value");

	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch,
	    sizeof(epoch)-1), EINVAL,
	    "mallctl() should return EINVAL for input size mismatch");
	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch,
	    sizeof(epoch)+1), EINVAL,
	    "mallctl() should return EINVAL for input size mismatch");

	sz = sizeof(epoch)-1;
	expect_d_eq(mallctl("epoch", (void *)&epoch, &sz, NULL, 0), EINVAL,
	    "mallctl() should return EINVAL for output size mismatch");
	sz = sizeof(epoch)+1;
	expect_d_eq(mallctl("epoch", (void *)&epoch, &sz, NULL, 0), EINVAL,
	    "mallctl() should return EINVAL for output size mismatch");
}
TEST_END

TEST_BEGIN(test_mallctlnametomib_errors) {
	size_t mib[1];
	size_t miblen;

	miblen = sizeof(mib)/sizeof(size_t);
	expect_d_eq(mallctlnametomib("no_such_name", mib, &miblen), ENOENT,
	    "mallctlnametomib() should return ENOENT for non-existent names");
}
TEST_END

TEST_BEGIN(test_mallctlbymib_errors) {
	uint64_t epoch;
	size_t sz;
	size_t mib[1];
	size_t miblen;

	miblen = sizeof(mib)/sizeof(size_t);
	expect_d_eq(mallctlnametomib("version", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");

	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, "0.0.0",
	    strlen("0.0.0")), EPERM, "mallctl() should return EPERM on "
	    "attempt to write read-only value");

	miblen = sizeof(mib)/sizeof(size_t);
	expect_d_eq(mallctlnametomib("epoch", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");

	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, (void *)&epoch,
	    sizeof(epoch)-1), EINVAL,
	    "mallctlbymib() should return EINVAL for input size mismatch");
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, (void *)&epoch,
	    sizeof(epoch)+1), EINVAL,
	    "mallctlbymib() should return EINVAL for input size mismatch");

	sz = sizeof(epoch)-1;
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&epoch, &sz, NULL, 0),
	    EINVAL,
	    "mallctlbymib() should return EINVAL for output size mismatch");
	sz = sizeof(epoch)+1;
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&epoch, &sz, NULL, 0),
	    EINVAL,
	    "mallctlbymib() should return EINVAL for output size mismatch");
}
TEST_END

TEST_BEGIN(test_mallctl_read_write) {
	uint64_t old_epoch, new_epoch;
	size_t sz = sizeof(old_epoch);

	/* Blind. */
	expect_d_eq(mallctl("epoch", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_zu_eq(sz, sizeof(old_epoch), "Unexpected output size");

	/* Read. */
	expect_d_eq(mallctl("epoch", (void *)&old_epoch, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_zu_eq(sz, sizeof(old_epoch), "Unexpected output size");

	/* Write. */
	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&new_epoch,
	    sizeof(new_epoch)), 0, "Unexpected mallctl() failure");
	expect_zu_eq(sz, sizeof(old_epoch), "Unexpected output size");

	/* Read+write. */
	expect_d_eq(mallctl("epoch", (void *)&old_epoch, &sz,
	    (void *)&new_epoch, sizeof(new_epoch)), 0,
	    "Unexpected mallctl() failure");
	expect_zu_eq(sz, sizeof(old_epoch), "Unexpected output size");
}
TEST_END

TEST_BEGIN(test_mallctlnametomib_short_mib) {
	size_t mib[4];
	size_t miblen;

	miblen = 3;
	mib[3] = 42;
	expect_d_eq(mallctlnametomib("arenas.bin.0.nregs", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	expect_zu_eq(miblen, 3, "Unexpected mib output length");
	expect_zu_eq(mib[3], 42,
	    "mallctlnametomib() wrote past the end of the input mib");
}
TEST_END

TEST_BEGIN(test_mallctlnametomib_short_name) {
	size_t mib[4];
	size_t miblen;

	miblen = 4;
	mib[3] = 42;
	expect_d_eq(mallctlnametomib("arenas.bin.0", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	expect_zu_eq(miblen, 3, "Unexpected mib output length");
	expect_zu_eq(mib[3], 42,
	    "mallctlnametomib() wrote past the end of the input mib");
}
TEST_END

TEST_BEGIN(test_mallctlmibnametomib) {
	size_t mib[4];
	size_t miblen = 4;
	uint32_t result, result_ref;
	size_t len_result = sizeof(uint32_t);

	tsd_t *tsd = tsd_fetch();

	/* Error cases */
	assert_d_eq(ctl_mibnametomib(tsd, mib, 0, "bob", &miblen), ENOENT, "");
	assert_zu_eq(miblen, 4, "");
	assert_d_eq(ctl_mibnametomib(tsd, mib, 0, "9999", &miblen), ENOENT, "");
	assert_zu_eq(miblen, 4, "");

	/* Valid case. */
	assert_d_eq(ctl_mibnametomib(tsd, mib, 0, "arenas", &miblen), 0, "");
	assert_zu_eq(miblen, 1, "");
	miblen = 4;
	assert_d_eq(ctl_mibnametomib(tsd, mib, 1, "bin", &miblen), 0, "");
	assert_zu_eq(miblen, 2, "");
	expect_d_eq(mallctlbymib(mib, miblen, &result, &len_result, NULL, 0),
	    ENOENT, "mallctlbymib() should fail on partial path");

	/* Error cases. */
	miblen = 4;
	assert_d_eq(ctl_mibnametomib(tsd, mib, 2, "bob", &miblen), ENOENT, "");
	assert_zu_eq(miblen, 4, "");
	assert_d_eq(ctl_mibnametomib(tsd, mib, 2, "9999", &miblen), ENOENT, "");
	assert_zu_eq(miblen, 4, "");

	/* Valid case. */
	assert_d_eq(ctl_mibnametomib(tsd, mib, 2, "0", &miblen), 0, "");
	assert_zu_eq(miblen, 3, "");
	expect_d_eq(mallctlbymib(mib, miblen, &result, &len_result, NULL, 0),
	    ENOENT, "mallctlbymib() should fail on partial path");

	/* Error cases. */
	miblen = 4;
	assert_d_eq(ctl_mibnametomib(tsd, mib, 3, "bob", &miblen), ENOENT, "");
	assert_zu_eq(miblen, 4, "");
	assert_d_eq(ctl_mibnametomib(tsd, mib, 3, "9999", &miblen), ENOENT, "");
	assert_zu_eq(miblen, 4, "");

	/* Valid case. */
	assert_d_eq(ctl_mibnametomib(tsd, mib, 3, "nregs", &miblen), 0, "");
	assert_zu_eq(miblen, 4, "");
	assert_d_eq(mallctlbymib(mib, miblen, &result, &len_result, NULL, 0),
	    0, "Unexpected mallctlbymib() failure");
	assert_d_eq(mallctl("arenas.bin.0.nregs", &result_ref, &len_result,
	    NULL, 0), 0, "Unexpected mallctl() failure");
	expect_zu_eq(result, result_ref,
	    "mallctlbymib() and mallctl() returned different result");
}
TEST_END

TEST_BEGIN(test_mallctlbymibname) {
	size_t mib[4];
	size_t miblen = 4;
	uint32_t result, result_ref;
	size_t len_result = sizeof(uint32_t);

	tsd_t *tsd = tsd_fetch();

	/* Error cases. */

	assert_d_eq(mallctlnametomib("arenas", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	assert_zu_eq(miblen, 1, "");

	miblen = 4;
	assert_d_eq(ctl_bymibname(tsd, mib, 1, "bin.0", &miblen,
	    &result, &len_result, NULL, 0), ENOENT, "");
	miblen = 4;
	assert_d_eq(ctl_bymibname(tsd, mib, 1, "bin.0.bob", &miblen,
	    &result, &len_result, NULL, 0), ENOENT, "");
	assert_zu_eq(miblen, 4, "");

	/* Valid cases. */

	assert_d_eq(mallctl("arenas.bin.0.nregs", &result_ref, &len_result,
	    NULL, 0), 0, "Unexpected mallctl() failure");
	miblen = 4;

	assert_d_eq(ctl_bymibname(tsd, mib, 0, "arenas.bin.0.nregs", &miblen,
	    &result, &len_result, NULL, 0), 0, "");
	assert_zu_eq(miblen, 4, "");
	expect_zu_eq(result, result_ref, "Unexpected result");

	assert_d_eq(ctl_bymibname(tsd, mib, 1, "bin.0.nregs", &miblen, &result,
	    &len_result, NULL, 0), 0, "");
	assert_zu_eq(miblen, 4, "");
	expect_zu_eq(result, result_ref, "Unexpected result");

	assert_d_eq(ctl_bymibname(tsd, mib, 2, "0.nregs", &miblen, &result,
	    &len_result, NULL, 0), 0, "");
	assert_zu_eq(miblen, 4, "");
	expect_zu_eq(result, result_ref, "Unexpected result");

	assert_d_eq(ctl_bymibname(tsd, mib, 3, "nregs", &miblen, &result,
	    &len_result, NULL, 0), 0, "");
	assert_zu_eq(miblen, 4, "");
	expect_zu_eq(result, result_ref, "Unexpected result");
}
TEST_END

TEST_BEGIN(test_mallctl_config) {
#define TEST_MALLCTL_CONFIG(config, t) do {				\
	t oldval;							\
	size_t sz = sizeof(oldval);					\
	expect_d_eq(mallctl("config."#config, (void *)&oldval, &sz,	\
	    NULL, 0), 0, "Unexpected mallctl() failure");		\
	expect_b_eq(oldval, config_##config, "Incorrect config value");	\
	expect_zu_eq(sz, sizeof(oldval), "Unexpected output size");	\
} while (0)

	TEST_MALLCTL_CONFIG(cache_oblivious, bool);
	TEST_MALLCTL_CONFIG(debug, bool);
	TEST_MALLCTL_CONFIG(fill, bool);
	TEST_MALLCTL_CONFIG(lazy_lock, bool);
	TEST_MALLCTL_CONFIG(malloc_conf, const char *);
	TEST_MALLCTL_CONFIG(prof, bool);
	TEST_MALLCTL_CONFIG(prof_libgcc, bool);
	TEST_MALLCTL_CONFIG(prof_libunwind, bool);
	TEST_MALLCTL_CONFIG(stats, bool);
	TEST_MALLCTL_CONFIG(utrace, bool);
	TEST_MALLCTL_CONFIG(xmalloc, bool);

#undef TEST_MALLCTL_CONFIG
}
TEST_END

TEST_BEGIN(test_mallctl_opt) {
	bool config_always = true;

#define TEST_MALLCTL_OPT(t, opt, config) do {				\
	t oldval;							\
	size_t sz = sizeof(oldval);					\
	int expected = config_##config ? 0 : ENOENT;			\
	int result = mallctl("opt."#opt, (void *)&oldval, &sz, NULL,	\
	    0);								\
	expect_d_eq(result, expected,					\
	    "Unexpected mallctl() result for opt."#opt);		\
	expect_zu_eq(sz, sizeof(oldval), "Unexpected output size");	\
} while (0)

	TEST_MALLCTL_OPT(bool, abort, always);
	TEST_MALLCTL_OPT(bool, abort_conf, always);
	TEST_MALLCTL_OPT(bool, cache_oblivious, always);
	TEST_MALLCTL_OPT(bool, trust_madvise, always);
	TEST_MALLCTL_OPT(bool, confirm_conf, always);
	TEST_MALLCTL_OPT(const char *, metadata_thp, always);
	TEST_MALLCTL_OPT(bool, retain, always);
	TEST_MALLCTL_OPT(const char *, dss, always);
	TEST_MALLCTL_OPT(bool, hpa, always);
	TEST_MALLCTL_OPT(size_t, hpa_slab_max_alloc, always);
	TEST_MALLCTL_OPT(size_t, hpa_sec_nshards, always);
	TEST_MALLCTL_OPT(size_t, hpa_sec_max_alloc, always);
	TEST_MALLCTL_OPT(size_t, hpa_sec_max_bytes, always);
	TEST_MALLCTL_OPT(size_t, hpa_sec_bytes_after_flush, always);
	TEST_MALLCTL_OPT(size_t, hpa_sec_batch_fill_extra, always);
	TEST_MALLCTL_OPT(unsigned, narenas, always);
	TEST_MALLCTL_OPT(const char *, percpu_arena, always);
	TEST_MALLCTL_OPT(size_t, oversize_threshold, always);
	TEST_MALLCTL_OPT(bool, background_thread, always);
	TEST_MALLCTL_OPT(ssize_t, dirty_decay_ms, always);
	TEST_MALLCTL_OPT(ssize_t, muzzy_decay_ms, always);
	TEST_MALLCTL_OPT(bool, stats_print, always);
	TEST_MALLCTL_OPT(const char *, stats_print_opts, always);
	TEST_MALLCTL_OPT(int64_t, stats_interval, always);
	TEST_MALLCTL_OPT(const char *, stats_interval_opts, always);
	TEST_MALLCTL_OPT(const char *, junk, fill);
	TEST_MALLCTL_OPT(bool, zero, fill);
	TEST_MALLCTL_OPT(bool, utrace, utrace);
	TEST_MALLCTL_OPT(bool, xmalloc, xmalloc);
	TEST_MALLCTL_OPT(bool, tcache, always);
	TEST_MALLCTL_OPT(size_t, lg_extent_max_active_fit, always);
	TEST_MALLCTL_OPT(size_t, tcache_max, always);
	TEST_MALLCTL_OPT(const char *, thp, always);
	TEST_MALLCTL_OPT(const char *, zero_realloc, always);
	TEST_MALLCTL_OPT(bool, prof, prof);
	TEST_MALLCTL_OPT(const char *, prof_prefix, prof);
	TEST_MALLCTL_OPT(bool, prof_active, prof);
	TEST_MALLCTL_OPT(ssize_t, lg_prof_sample, prof);
	TEST_MALLCTL_OPT(bool, prof_accum, prof);
	TEST_MALLCTL_OPT(ssize_t, lg_prof_interval, prof);
	TEST_MALLCTL_OPT(bool, prof_gdump, prof);
	TEST_MALLCTL_OPT(bool, prof_final, prof);
	TEST_MALLCTL_OPT(bool, prof_leak, prof);
	TEST_MALLCTL_OPT(bool, prof_leak_error, prof);
	TEST_MALLCTL_OPT(ssize_t, prof_recent_alloc_max, prof);
	TEST_MALLCTL_OPT(bool, prof_stats, prof);
	TEST_MALLCTL_OPT(bool, prof_sys_thread_name, prof);
	TEST_MALLCTL_OPT(ssize_t, lg_san_uaf_align, uaf_detection);

#undef TEST_MALLCTL_OPT
}
TEST_END

TEST_BEGIN(test_manpage_example) {
	unsigned nbins, i;
	size_t mib[4];
	size_t len, miblen;

	len = sizeof(nbins);
	expect_d_eq(mallctl("arenas.nbins", (void *)&nbins, &len, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	miblen = 4;
	expect_d_eq(mallctlnametomib("arenas.bin.0.size", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	for (i = 0; i < nbins; i++) {
		size_t bin_size;

		mib[2] = i;
		len = sizeof(bin_size);
		expect_d_eq(mallctlbymib(mib, miblen, (void *)&bin_size, &len,
		    NULL, 0), 0, "Unexpected mallctlbymib() failure");
		/* Do something with bin_size... */
	}
}
TEST_END

TEST_BEGIN(test_tcache_none) {
	test_skip_if(!opt_tcache);

	/* Allocate p and q. */
	void *p0 = mallocx(42, 0);
	expect_ptr_not_null(p0, "Unexpected mallocx() failure");
	void *q = mallocx(42, 0);
	expect_ptr_not_null(q, "Unexpected mallocx() failure");

	/* Deallocate p and q, but bypass the tcache for q. */
	dallocx(p0, 0);
	dallocx(q, MALLOCX_TCACHE_NONE);

	/* Make sure that tcache-based allocation returns p, not q. */
	void *p1 = mallocx(42, 0);
	expect_ptr_not_null(p1, "Unexpected mallocx() failure");
	if (!opt_prof && !san_uaf_detection_enabled()) {
		expect_ptr_eq(p0, p1,
		    "Expected tcache to allocate cached region");
	}

	/* Clean up. */
	dallocx(p1, MALLOCX_TCACHE_NONE);
}
TEST_END

TEST_BEGIN(test_tcache) {
#define NTCACHES	10
	unsigned tis[NTCACHES];
	void *ps[NTCACHES];
	void *qs[NTCACHES];
	unsigned i;
	size_t sz, psz, qsz;

	psz = 42;
	qsz = nallocx(psz, 0) + 1;

	/* Create tcaches. */
	for (i = 0; i < NTCACHES; i++) {
		sz = sizeof(unsigned);
		expect_d_eq(mallctl("tcache.create", (void *)&tis[i], &sz, NULL,
		    0), 0, "Unexpected mallctl() failure, i=%u", i);
	}

	/* Exercise tcache ID recycling. */
	for (i = 0; i < NTCACHES; i++) {
		expect_d_eq(mallctl("tcache.destroy", NULL, NULL,
		    (void *)&tis[i], sizeof(unsigned)), 0,
		    "Unexpected mallctl() failure, i=%u", i);
	}
	for (i = 0; i < NTCACHES; i++) {
		sz = sizeof(unsigned);
		expect_d_eq(mallctl("tcache.create", (void *)&tis[i], &sz, NULL,
		    0), 0, "Unexpected mallctl() failure, i=%u", i);
	}

	/* Flush empty tcaches. */
	for (i = 0; i < NTCACHES; i++) {
		expect_d_eq(mallctl("tcache.flush", NULL, NULL, (void *)&tis[i],
		    sizeof(unsigned)), 0, "Unexpected mallctl() failure, i=%u",
		    i);
	}

	/* Cache some allocations. */
	for (i = 0; i < NTCACHES; i++) {
		ps[i] = mallocx(psz, MALLOCX_TCACHE(tis[i]));
		expect_ptr_not_null(ps[i], "Unexpected mallocx() failure, i=%u",
		    i);
		dallocx(ps[i], MALLOCX_TCACHE(tis[i]));

		qs[i] = mallocx(qsz, MALLOCX_TCACHE(tis[i]));
		expect_ptr_not_null(qs[i], "Unexpected mallocx() failure, i=%u",
		    i);
		dallocx(qs[i], MALLOCX_TCACHE(tis[i]));
	}

	/* Verify that tcaches allocate cached regions. */
	for (i = 0; i < NTCACHES; i++) {
		void *p0 = ps[i];
		ps[i] = mallocx(psz, MALLOCX_TCACHE(tis[i]));
		expect_ptr_not_null(ps[i], "Unexpected mallocx() failure, i=%u",
		    i);
		if (!san_uaf_detection_enabled()) {
			expect_ptr_eq(ps[i], p0, "Expected mallocx() to "
			    "allocate cached region, i=%u", i);
		}
	}

	/* Verify that reallocation uses cached regions. */
	for (i = 0; i < NTCACHES; i++) {
		void *q0 = qs[i];
		qs[i] = rallocx(ps[i], qsz, MALLOCX_TCACHE(tis[i]));
		expect_ptr_not_null(qs[i], "Unexpected rallocx() failure, i=%u",
		    i);
		if (!san_uaf_detection_enabled()) {
			expect_ptr_eq(qs[i], q0, "Expected rallocx() to "
			    "allocate cached region, i=%u", i);
		}
		/* Avoid undefined behavior in case of test failure. */
		if (qs[i] == NULL) {
			qs[i] = ps[i];
		}
	}
	for (i = 0; i < NTCACHES; i++) {
		dallocx(qs[i], MALLOCX_TCACHE(tis[i]));
	}

	/* Flush some non-empty tcaches. */
	for (i = 0; i < NTCACHES/2; i++) {
		expect_d_eq(mallctl("tcache.flush", NULL, NULL, (void *)&tis[i],
		    sizeof(unsigned)), 0, "Unexpected mallctl() failure, i=%u",
		    i);
	}

	/* Destroy tcaches. */
	for (i = 0; i < NTCACHES; i++) {
		expect_d_eq(mallctl("tcache.destroy", NULL, NULL,
		    (void *)&tis[i], sizeof(unsigned)), 0,
		    "Unexpected mallctl() failure, i=%u", i);
	}
}
TEST_END

TEST_BEGIN(test_thread_arena) {
	unsigned old_arena_ind, new_arena_ind, narenas;

	const char *opa;
	size_t sz = sizeof(opa);
	expect_d_eq(mallctl("opt.percpu_arena", (void *)&opa, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	sz = sizeof(unsigned);
	expect_d_eq(mallctl("arenas.narenas", (void *)&narenas, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	if (opt_oversize_threshold != 0) {
		narenas--;
	}
	expect_u_eq(narenas, opt_narenas, "Number of arenas incorrect");

	if (strcmp(opa, "disabled") == 0) {
		new_arena_ind = narenas - 1;
		expect_d_eq(mallctl("thread.arena", (void *)&old_arena_ind, &sz,
		    (void *)&new_arena_ind, sizeof(unsigned)), 0,
		    "Unexpected mallctl() failure");
		new_arena_ind = 0;
		expect_d_eq(mallctl("thread.arena", (void *)&old_arena_ind, &sz,
		    (void *)&new_arena_ind, sizeof(unsigned)), 0,
		    "Unexpected mallctl() failure");
	} else {
		expect_d_eq(mallctl("thread.arena", (void *)&old_arena_ind, &sz,
		    NULL, 0), 0, "Unexpected mallctl() failure");
		new_arena_ind = percpu_arena_ind_limit(opt_percpu_arena) - 1;
		if (old_arena_ind != new_arena_ind) {
			expect_d_eq(mallctl("thread.arena",
			    (void *)&old_arena_ind, &sz, (void *)&new_arena_ind,
			    sizeof(unsigned)), EPERM, "thread.arena ctl "
			    "should not be allowed with percpu arena");
		}
	}
}
TEST_END

TEST_BEGIN(test_arena_i_initialized) {
	unsigned narenas, i;
	size_t sz;
	size_t mib[3];
	size_t miblen = sizeof(mib) / sizeof(size_t);
	bool initialized;

	sz = sizeof(narenas);
	expect_d_eq(mallctl("arenas.narenas", (void *)&narenas, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");

	expect_d_eq(mallctlnametomib("arena.0.initialized", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	for (i = 0; i < narenas; i++) {
		mib[1] = i;
		sz = sizeof(initialized);
		expect_d_eq(mallctlbymib(mib, miblen, &initialized, &sz, NULL,
		    0), 0, "Unexpected mallctl() failure");
	}

	mib[1] = MALLCTL_ARENAS_ALL;
	sz = sizeof(initialized);
	expect_d_eq(mallctlbymib(mib, miblen, &initialized, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_true(initialized,
	    "Merged arena statistics should always be initialized");

	/* Equivalent to the above but using mallctl() directly. */
	sz = sizeof(initialized);
	expect_d_eq(mallctl(
	    "arena." STRINGIFY(MALLCTL_ARENAS_ALL) ".initialized",
	    (void *)&initialized, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_true(initialized,
	    "Merged arena statistics should always be initialized");
}
TEST_END

TEST_BEGIN(test_arena_i_dirty_decay_ms) {
	ssize_t dirty_decay_ms, orig_dirty_decay_ms, prev_dirty_decay_ms;
	size_t sz = sizeof(ssize_t);

	expect_d_eq(mallctl("arena.0.dirty_decay_ms",
	    (void *)&orig_dirty_decay_ms, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	dirty_decay_ms = -2;
	expect_d_eq(mallctl("arena.0.dirty_decay_ms", NULL, NULL,
	    (void *)&dirty_decay_ms, sizeof(ssize_t)), EFAULT,
	    "Unexpected mallctl() success");

	dirty_decay_ms = 0x7fffffff;
	expect_d_eq(mallctl("arena.0.dirty_decay_ms", NULL, NULL,
	    (void *)&dirty_decay_ms, sizeof(ssize_t)), 0,
	    "Unexpected mallctl() failure");

	for (prev_dirty_decay_ms = dirty_decay_ms, dirty_decay_ms = -1;
	    dirty_decay_ms < 20; prev_dirty_decay_ms = dirty_decay_ms,
	    dirty_decay_ms++) {
		ssize_t old_dirty_decay_ms;

		expect_d_eq(mallctl("arena.0.dirty_decay_ms",
		    (void *)&old_dirty_decay_ms, &sz, (void *)&dirty_decay_ms,
		    sizeof(ssize_t)), 0, "Unexpected mallctl() failure");
		expect_zd_eq(old_dirty_decay_ms, prev_dirty_decay_ms,
		    "Unexpected old arena.0.dirty_decay_ms");
	}
}
TEST_END

TEST_BEGIN(test_arena_i_muzzy_decay_ms) {
	ssize_t muzzy_decay_ms, orig_muzzy_decay_ms, prev_muzzy_decay_ms;
	size_t sz = sizeof(ssize_t);

	expect_d_eq(mallctl("arena.0.muzzy_decay_ms",
	    (void *)&orig_muzzy_decay_ms, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	muzzy_decay_ms = -2;
	expect_d_eq(mallctl("arena.0.muzzy_decay_ms", NULL, NULL,
	    (void *)&muzzy_decay_ms, sizeof(ssize_t)), EFAULT,
	    "Unexpected mallctl() success");

	muzzy_decay_ms = 0x7fffffff;
	expect_d_eq(mallctl("arena.0.muzzy_decay_ms", NULL, NULL,
	    (void *)&muzzy_decay_ms, sizeof(ssize_t)), 0,
	    "Unexpected mallctl() failure");

	for (prev_muzzy_decay_ms = muzzy_decay_ms, muzzy_decay_ms = -1;
	    muzzy_decay_ms < 20; prev_muzzy_decay_ms = muzzy_decay_ms,
	    muzzy_decay_ms++) {
		ssize_t old_muzzy_decay_ms;

		expect_d_eq(mallctl("arena.0.muzzy_decay_ms",
		    (void *)&old_muzzy_decay_ms, &sz, (void *)&muzzy_decay_ms,
		    sizeof(ssize_t)), 0, "Unexpected mallctl() failure");
		expect_zd_eq(old_muzzy_decay_ms, prev_muzzy_decay_ms,
		    "Unexpected old arena.0.muzzy_decay_ms");
	}
}
TEST_END

TEST_BEGIN(test_arena_i_purge) {
	unsigned narenas;
	size_t sz = sizeof(unsigned);
	size_t mib[3];
	size_t miblen = 3;

	expect_d_eq(mallctl("arena.0.purge", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	expect_d_eq(mallctl("arenas.narenas", (void *)&narenas, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	expect_d_eq(mallctlnametomib("arena.0.purge", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = narenas;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");

	mib[1] = MALLCTL_ARENAS_ALL;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
}
TEST_END

TEST_BEGIN(test_arena_i_decay) {
	unsigned narenas;
	size_t sz = sizeof(unsigned);
	size_t mib[3];
	size_t miblen = 3;

	expect_d_eq(mallctl("arena.0.decay", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	expect_d_eq(mallctl("arenas.narenas", (void *)&narenas, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	expect_d_eq(mallctlnametomib("arena.0.decay", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = narenas;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");

	mib[1] = MALLCTL_ARENAS_ALL;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
}
TEST_END

TEST_BEGIN(test_arena_i_dss) {
	const char *dss_prec_old, *dss_prec_new;
	size_t sz = sizeof(dss_prec_old);
	size_t mib[3];
	size_t miblen;

	miblen = sizeof(mib)/sizeof(size_t);
	expect_d_eq(mallctlnametomib("arena.0.dss", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() error");

	dss_prec_new = "disabled";
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_old, &sz,
	    (void *)&dss_prec_new, sizeof(dss_prec_new)), 0,
	    "Unexpected mallctl() failure");
	expect_str_ne(dss_prec_old, "primary",
	    "Unexpected default for dss precedence");

	expect_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_new, &sz,
	    (void *)&dss_prec_old, sizeof(dss_prec_old)), 0,
	    "Unexpected mallctl() failure");

	expect_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_old, &sz, NULL,
	    0), 0, "Unexpected mallctl() failure");
	expect_str_ne(dss_prec_old, "primary",
	    "Unexpected value for dss precedence");

	mib[1] = narenas_total_get();
	dss_prec_new = "disabled";
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_old, &sz,
	    (void *)&dss_prec_new, sizeof(dss_prec_new)), 0,
	    "Unexpected mallctl() failure");
	expect_str_ne(dss_prec_old, "primary",
	    "Unexpected default for dss precedence");

	expect_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_new, &sz,
	    (void *)&dss_prec_old, sizeof(dss_prec_new)), 0,
	    "Unexpected mallctl() failure");

	expect_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_old, &sz, NULL,
	    0), 0, "Unexpected mallctl() failure");
	expect_str_ne(dss_prec_old, "primary",
	    "Unexpected value for dss precedence");
}
TEST_END

TEST_BEGIN(test_arena_i_retain_grow_limit) {
	size_t old_limit, new_limit, default_limit;
	size_t mib[3];
	size_t miblen;

	bool retain_enabled;
	size_t sz = sizeof(retain_enabled);
	expect_d_eq(mallctl("opt.retain", &retain_enabled, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	test_skip_if(!retain_enabled);

	sz = sizeof(default_limit);
	miblen = sizeof(mib)/sizeof(size_t);
	expect_d_eq(mallctlnametomib("arena.0.retain_grow_limit", mib, &miblen),
	    0, "Unexpected mallctlnametomib() error");

	expect_d_eq(mallctlbymib(mib, miblen, &default_limit, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_zu_eq(default_limit, SC_LARGE_MAXCLASS,
	    "Unexpected default for retain_grow_limit");

	new_limit = PAGE - 1;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, &new_limit,
	    sizeof(new_limit)), EFAULT, "Unexpected mallctl() success");

	new_limit = PAGE + 1;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, &new_limit,
	    sizeof(new_limit)), 0, "Unexpected mallctl() failure");
	expect_d_eq(mallctlbymib(mib, miblen, &old_limit, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_zu_eq(old_limit, PAGE,
	    "Unexpected value for retain_grow_limit");

	/* Expect grow less than psize class 10. */
	new_limit = sz_pind2sz(10) - 1;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, &new_limit,
	    sizeof(new_limit)), 0, "Unexpected mallctl() failure");
	expect_d_eq(mallctlbymib(mib, miblen, &old_limit, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_zu_eq(old_limit, sz_pind2sz(9),
	    "Unexpected value for retain_grow_limit");

	/* Restore to default. */
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, &default_limit,
	    sizeof(default_limit)), 0, "Unexpected mallctl() failure");
}
TEST_END

TEST_BEGIN(test_arenas_dirty_decay_ms) {
	ssize_t dirty_decay_ms, orig_dirty_decay_ms, prev_dirty_decay_ms;
	size_t sz = sizeof(ssize_t);

	expect_d_eq(mallctl("arenas.dirty_decay_ms",
	    (void *)&orig_dirty_decay_ms, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	dirty_decay_ms = -2;
	expect_d_eq(mallctl("arenas.dirty_decay_ms", NULL, NULL,
	    (void *)&dirty_decay_ms, sizeof(ssize_t)), EFAULT,
	    "Unexpected mallctl() success");

	dirty_decay_ms = 0x7fffffff;
	expect_d_eq(mallctl("arenas.dirty_decay_ms", NULL, NULL,
	    (void *)&dirty_decay_ms, sizeof(ssize_t)), 0,
	    "Expected mallctl() failure");

	for (prev_dirty_decay_ms = dirty_decay_ms, dirty_decay_ms = -1;
	    dirty_decay_ms < 20; prev_dirty_decay_ms = dirty_decay_ms,
	    dirty_decay_ms++) {
		ssize_t old_dirty_decay_ms;

		expect_d_eq(mallctl("arenas.dirty_decay_ms",
		    (void *)&old_dirty_decay_ms, &sz, (void *)&dirty_decay_ms,
		    sizeof(ssize_t)), 0, "Unexpected mallctl() failure");
		expect_zd_eq(old_dirty_decay_ms, prev_dirty_decay_ms,
		    "Unexpected old arenas.dirty_decay_ms");
	}
}
TEST_END

TEST_BEGIN(test_arenas_muzzy_decay_ms) {
	ssize_t muzzy_decay_ms, orig_muzzy_decay_ms, prev_muzzy_decay_ms;
	size_t sz = sizeof(ssize_t);

	expect_d_eq(mallctl("arenas.muzzy_decay_ms",
	    (void *)&orig_muzzy_decay_ms, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	muzzy_decay_ms = -2;
	expect_d_eq(mallctl("arenas.muzzy_decay_ms", NULL, NULL,
	    (void *)&muzzy_decay_ms, sizeof(ssize_t)), EFAULT,
	    "Unexpected mallctl() success");

	muzzy_decay_ms = 0x7fffffff;
	expect_d_eq(mallctl("arenas.muzzy_decay_ms", NULL, NULL,
	    (void *)&muzzy_decay_ms, sizeof(ssize_t)), 0,
	    "Expected mallctl() failure");

	for (prev_muzzy_decay_ms = muzzy_decay_ms, muzzy_decay_ms = -1;
	    muzzy_decay_ms < 20; prev_muzzy_decay_ms = muzzy_decay_ms,
	    muzzy_decay_ms++) {
		ssize_t old_muzzy_decay_ms;

		expect_d_eq(mallctl("arenas.muzzy_decay_ms",
		    (void *)&old_muzzy_decay_ms, &sz, (void *)&muzzy_decay_ms,
		    sizeof(ssize_t)), 0, "Unexpected mallctl() failure");
		expect_zd_eq(old_muzzy_decay_ms, prev_muzzy_decay_ms,
		    "Unexpected old arenas.muzzy_decay_ms");
	}
}
TEST_END

TEST_BEGIN(test_arenas_constants) {
#define TEST_ARENAS_CONSTANT(t, name, expected) do {			\
	t name;								\
	size_t sz = sizeof(t);						\
	expect_d_eq(mallctl("arenas."#name, (void *)&name, &sz, NULL,	\
	    0), 0, "Unexpected mallctl() failure");			\
	expect_zu_eq(name, expected, "Incorrect "#name" size");		\
} while (0)

	TEST_ARENAS_CONSTANT(size_t, quantum, QUANTUM);
	TEST_ARENAS_CONSTANT(size_t, page, PAGE);
	TEST_ARENAS_CONSTANT(unsigned, nbins, SC_NBINS);
	TEST_ARENAS_CONSTANT(unsigned, nlextents, SC_NSIZES - SC_NBINS);

#undef TEST_ARENAS_CONSTANT
}
TEST_END

TEST_BEGIN(test_arenas_bin_constants) {
#define TEST_ARENAS_BIN_CONSTANT(t, name, expected) do {		\
	t name;								\
	size_t sz = sizeof(t);						\
	expect_d_eq(mallctl("arenas.bin.0."#name, (void *)&name, &sz,	\
	    NULL, 0), 0, "Unexpected mallctl() failure");		\
	expect_zu_eq(name, expected, "Incorrect "#name" size");		\
} while (0)

	TEST_ARENAS_BIN_CONSTANT(size_t, size, bin_infos[0].reg_size);
	TEST_ARENAS_BIN_CONSTANT(uint32_t, nregs, bin_infos[0].nregs);
	TEST_ARENAS_BIN_CONSTANT(size_t, slab_size,
	    bin_infos[0].slab_size);
	TEST_ARENAS_BIN_CONSTANT(uint32_t, nshards, bin_infos[0].n_shards);

#undef TEST_ARENAS_BIN_CONSTANT
}
TEST_END

TEST_BEGIN(test_arenas_lextent_constants) {
#define TEST_ARENAS_LEXTENT_CONSTANT(t, name, expected) do {		\
	t name;								\
	size_t sz = sizeof(t);						\
	expect_d_eq(mallctl("arenas.lextent.0."#name, (void *)&name,	\
	    &sz, NULL, 0), 0, "Unexpected mallctl() failure");		\
	expect_zu_eq(name, expected, "Incorrect "#name" size");		\
} while (0)

	TEST_ARENAS_LEXTENT_CONSTANT(size_t, size,
	    SC_LARGE_MINCLASS);

#undef TEST_ARENAS_LEXTENT_CONSTANT
}
TEST_END

TEST_BEGIN(test_arenas_create) {
	unsigned narenas_before, arena, narenas_after;
	size_t sz = sizeof(unsigned);

	expect_d_eq(mallctl("arenas.narenas", (void *)&narenas_before, &sz,
	    NULL, 0), 0, "Unexpected mallctl() failure");
	expect_d_eq(mallctl("arenas.create", (void *)&arena, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_d_eq(mallctl("arenas.narenas", (void *)&narenas_after, &sz, NULL,
	    0), 0, "Unexpected mallctl() failure");

	expect_u_eq(narenas_before+1, narenas_after,
	    "Unexpected number of arenas before versus after extension");
	expect_u_eq(arena, narenas_after-1, "Unexpected arena index");
}
TEST_END

TEST_BEGIN(test_arenas_lookup) {
	unsigned arena, arena1;
	void *ptr;
	size_t sz = sizeof(unsigned);

	expect_d_eq(mallctl("arenas.create", (void *)&arena, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	ptr = mallocx(42, MALLOCX_ARENA(arena) | MALLOCX_TCACHE_NONE);
	expect_ptr_not_null(ptr, "Unexpected mallocx() failure");
	expect_d_eq(mallctl("arenas.lookup", &arena1, &sz, &ptr, sizeof(ptr)),
	    0, "Unexpected mallctl() failure");
	expect_u_eq(arena, arena1, "Unexpected arena index");
	dallocx(ptr, 0);
}
TEST_END

TEST_BEGIN(test_prof_active) {
	/*
	 * If config_prof is off, then the test for prof_active in
	 * test_mallctl_opt was already enough.
	 */
	test_skip_if(!config_prof);
	test_skip_if(opt_prof);

	bool active, old;
	size_t len = sizeof(bool);

	active = true;
	expect_d_eq(mallctl("prof.active", NULL, NULL, &active, len), ENOENT,
	    "Setting prof_active to true should fail when opt_prof is off");
	old = true;
	expect_d_eq(mallctl("prof.active", &old, &len, &active, len), ENOENT,
	    "Setting prof_active to true should fail when opt_prof is off");
	expect_true(old, "old value should not be touched when mallctl fails");
	active = false;
	expect_d_eq(mallctl("prof.active", NULL, NULL, &active, len), 0,
	    "Setting prof_active to false should succeed when opt_prof is off");
	expect_d_eq(mallctl("prof.active", &old, &len, &active, len), 0,
	    "Setting prof_active to false should succeed when opt_prof is off");
	expect_false(old, "prof_active should be false when opt_prof is off");
}
TEST_END

TEST_BEGIN(test_stats_arenas) {
#define TEST_STATS_ARENAS(t, name) do {					\
	t name;								\
	size_t sz = sizeof(t);						\
	expect_d_eq(mallctl("stats.arenas.0."#name, (void *)&name, &sz,	\
	    NULL, 0), 0, "Unexpected mallctl() failure");		\
} while (0)

	TEST_STATS_ARENAS(unsigned, nthreads);
	TEST_STATS_ARENAS(const char *, dss);
	TEST_STATS_ARENAS(ssize_t, dirty_decay_ms);
	TEST_STATS_ARENAS(ssize_t, muzzy_decay_ms);
	TEST_STATS_ARENAS(size_t, pactive);
	TEST_STATS_ARENAS(size_t, pdirty);

#undef TEST_STATS_ARENAS
}
TEST_END

static void
alloc_hook(void *extra, UNUSED hook_alloc_t type, UNUSED void *result,
    UNUSED uintptr_t result_raw, UNUSED uintptr_t args_raw[3]) {
	*(bool *)extra = true;
}

static void
dalloc_hook(void *extra, UNUSED hook_dalloc_t type,
    UNUSED void *address, UNUSED uintptr_t args_raw[3]) {
	*(bool *)extra = true;
}

TEST_BEGIN(test_hooks) {
	bool hook_called = false;
	hooks_t hooks = {&alloc_hook, &dalloc_hook, NULL, &hook_called};
	void *handle = NULL;
	size_t sz = sizeof(handle);
	int err = mallctl("experimental.hooks.install", &handle, &sz, &hooks,
	    sizeof(hooks));
	expect_d_eq(err, 0, "Hook installation failed");
	expect_ptr_ne(handle, NULL, "Hook installation gave null handle");
	void *ptr = mallocx(1, 0);
	expect_true(hook_called, "Alloc hook not called");
	hook_called = false;
	free(ptr);
	expect_true(hook_called, "Free hook not called");

	err = mallctl("experimental.hooks.remove", NULL, NULL, &handle,
	    sizeof(handle));
	expect_d_eq(err, 0, "Hook removal failed");
	hook_called = false;
	ptr = mallocx(1, 0);
	free(ptr);
	expect_false(hook_called, "Hook called after removal");
}
TEST_END

TEST_BEGIN(test_hooks_exhaustion) {
	bool hook_called = false;
	hooks_t hooks = {&alloc_hook, &dalloc_hook, NULL, &hook_called};

	void *handle;
	void *handles[HOOK_MAX];
	size_t sz = sizeof(handle);
	int err;
	for (int i = 0; i < HOOK_MAX; i++) {
		handle = NULL;
		err = mallctl("experimental.hooks.install", &handle, &sz,
		    &hooks, sizeof(hooks));
		expect_d_eq(err, 0, "Error installation hooks");
		expect_ptr_ne(handle, NULL, "Got NULL handle");
		handles[i] = handle;
	}
	err = mallctl("experimental.hooks.install", &handle, &sz, &hooks,
	    sizeof(hooks));
	expect_d_eq(err, EAGAIN, "Should have failed hook installation");
	for (int i = 0; i < HOOK_MAX; i++) {
		err = mallctl("experimental.hooks.remove", NULL, NULL,
		    &handles[i], sizeof(handles[i]));
		expect_d_eq(err, 0, "Hook removal failed");
	}
	/* Insertion failed, but then we removed some; it should work now. */
	handle = NULL;
	err = mallctl("experimental.hooks.install", &handle, &sz, &hooks,
	    sizeof(hooks));
	expect_d_eq(err, 0, "Hook insertion failed");
	expect_ptr_ne(handle, NULL, "Got NULL handle");
	err = mallctl("experimental.hooks.remove", NULL, NULL, &handle,
	    sizeof(handle));
	expect_d_eq(err, 0, "Hook removal failed");
}
TEST_END

TEST_BEGIN(test_thread_idle) {
	/*
	 * We're cheating a little bit in this test, and inferring things about
	 * implementation internals (like tcache details).  We have to;
	 * thread.idle has no guaranteed effects.  We need stats to make these
	 * inferences.
	 */
	test_skip_if(!config_stats);

	int err;
	size_t sz;
	size_t miblen;

	bool tcache_enabled = false;
	sz = sizeof(tcache_enabled);
	err = mallctl("thread.tcache.enabled", &tcache_enabled, &sz, NULL, 0);
	expect_d_eq(err, 0, "");
	test_skip_if(!tcache_enabled);

	size_t tcache_max;
	sz = sizeof(tcache_max);
	err = mallctl("arenas.tcache_max", &tcache_max, &sz, NULL, 0);
	expect_d_eq(err, 0, "");
	test_skip_if(tcache_max == 0);

	unsigned arena_ind;
	sz = sizeof(arena_ind);
	err = mallctl("thread.arena", &arena_ind, &sz, NULL, 0);
	expect_d_eq(err, 0, "");

	/* We're going to do an allocation of size 1, which we know is small. */
	size_t mib[5];
	miblen = sizeof(mib)/sizeof(mib[0]);
	err = mallctlnametomib("stats.arenas.0.small.ndalloc", mib, &miblen);
	expect_d_eq(err, 0, "");
	mib[2] = arena_ind;

	/*
	 * This alloc and dalloc should leave something in the tcache, in a
	 * small size's cache bin.
	 */
	void *ptr = mallocx(1, 0);
	dallocx(ptr, 0);

	uint64_t epoch;
	err = mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch));
	expect_d_eq(err, 0, "");

	uint64_t small_dalloc_pre_idle;
	sz = sizeof(small_dalloc_pre_idle);
	err = mallctlbymib(mib, miblen, &small_dalloc_pre_idle, &sz, NULL, 0);
	expect_d_eq(err, 0, "");

	err = mallctl("thread.idle", NULL, NULL, NULL, 0);
	expect_d_eq(err, 0, "");

	err = mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch));
	expect_d_eq(err, 0, "");

	uint64_t small_dalloc_post_idle;
	sz = sizeof(small_dalloc_post_idle);
	err = mallctlbymib(mib, miblen, &small_dalloc_post_idle, &sz, NULL, 0);
	expect_d_eq(err, 0, "");

	expect_u64_lt(small_dalloc_pre_idle, small_dalloc_post_idle,
	    "Purge didn't flush the tcache");
}
TEST_END

TEST_BEGIN(test_thread_peak) {
	test_skip_if(!config_stats);

	/*
	 * We don't commit to any stable amount of accuracy for peak tracking
	 * (in practice, when this test was written, we made sure to be within
	 * 100k).  But 10MB is big for more or less any definition of big.
	 */
	size_t big_size = 10 * 1024 * 1024;
	size_t small_size = 256;

	void *ptr;
	int err;
	size_t sz;
	uint64_t peak;
	sz = sizeof(uint64_t);

	err = mallctl("thread.peak.reset", NULL, NULL, NULL, 0);
	expect_d_eq(err, 0, "");
	ptr = mallocx(SC_SMALL_MAXCLASS, 0);
	err = mallctl("thread.peak.read", &peak, &sz, NULL, 0);
	expect_d_eq(err, 0, "");
	expect_u64_eq(peak, SC_SMALL_MAXCLASS, "Missed an update");
	free(ptr);
	err = mallctl("thread.peak.read", &peak, &sz, NULL, 0);
	expect_d_eq(err, 0, "");
	expect_u64_eq(peak, SC_SMALL_MAXCLASS, "Freeing changed peak");
	ptr = mallocx(big_size, 0);
	free(ptr);
	/*
	 * The peak should have hit big_size in the last two lines, even though
	 * the net allocated bytes has since dropped back down to zero.  We
	 * should have noticed the peak change without having down any mallctl
	 * calls while net allocated bytes was high.
	 */
	err = mallctl("thread.peak.read", &peak, &sz, NULL, 0);
	expect_d_eq(err, 0, "");
	expect_u64_ge(peak, big_size, "Missed a peak change.");

	/* Allocate big_size, but using small allocations. */
	size_t nallocs = big_size / small_size;
	void **ptrs = calloc(nallocs, sizeof(void *));
	err = mallctl("thread.peak.reset", NULL, NULL, NULL, 0);
	expect_d_eq(err, 0, "");
	err = mallctl("thread.peak.read", &peak, &sz, NULL, 0);
	expect_d_eq(err, 0, "");
	expect_u64_eq(0, peak, "Missed a reset.");
	for (size_t i = 0; i < nallocs; i++) {
		ptrs[i] = mallocx(small_size, 0);
	}
	for (size_t i = 0; i < nallocs; i++) {
		free(ptrs[i]);
	}
	err = mallctl("thread.peak.read", &peak, &sz, NULL, 0);
	expect_d_eq(err, 0, "");
	/*
	 * We don't guarantee exactness; make sure we're within 10% of the peak,
	 * though.
	 */
	expect_u64_ge(peak, nallocx(small_size, 0) * nallocs * 9 / 10,
	    "Missed some peak changes.");
	expect_u64_le(peak, nallocx(small_size, 0) * nallocs * 11 / 10,
	    "Overcounted peak changes.");
	free(ptrs);
}
TEST_END

typedef struct activity_test_data_s activity_test_data_t;
struct activity_test_data_s {
	uint64_t obtained_alloc;
	uint64_t obtained_dalloc;
};

static void
activity_test_callback(void *uctx, uint64_t alloc, uint64_t dalloc) {
	activity_test_data_t *test_data = (activity_test_data_t *)uctx;
	test_data->obtained_alloc = alloc;
	test_data->obtained_dalloc = dalloc;
}

TEST_BEGIN(test_thread_activity_callback) {
	test_skip_if(!config_stats);

	const size_t big_size = 10 * 1024 * 1024;
	void *ptr;
	int err;
	size_t sz;

	uint64_t *allocatedp;
	uint64_t *deallocatedp;
	sz = sizeof(allocatedp);
	err = mallctl("thread.allocatedp", &allocatedp, &sz, NULL, 0);
	assert_d_eq(0, err, "");
	err = mallctl("thread.deallocatedp", &deallocatedp, &sz, NULL, 0);
	assert_d_eq(0, err, "");

	activity_callback_thunk_t old_thunk = {(activity_callback_t)111,
		(void *)222};

	activity_test_data_t test_data = {333, 444};
	activity_callback_thunk_t new_thunk =
	    {&activity_test_callback, &test_data};

	sz = sizeof(old_thunk);
	err = mallctl("experimental.thread.activity_callback", &old_thunk, &sz,
	    &new_thunk, sizeof(new_thunk));
	assert_d_eq(0, err, "");

	expect_true(old_thunk.callback == NULL, "Callback already installed");
	expect_true(old_thunk.uctx == NULL, "Callback data already installed");

	ptr = mallocx(big_size, 0);
	expect_u64_eq(test_data.obtained_alloc, *allocatedp, "");
	expect_u64_eq(test_data.obtained_dalloc, *deallocatedp, "");

	free(ptr);
	expect_u64_eq(test_data.obtained_alloc, *allocatedp, "");
	expect_u64_eq(test_data.obtained_dalloc, *deallocatedp, "");

	sz = sizeof(old_thunk);
	new_thunk = (activity_callback_thunk_t){ NULL, NULL };
	err = mallctl("experimental.thread.activity_callback", &old_thunk, &sz,
	    &new_thunk, sizeof(new_thunk));
	assert_d_eq(0, err, "");

	expect_true(old_thunk.callback == &activity_test_callback, "");
	expect_true(old_thunk.uctx == &test_data, "");

	/* Inserting NULL should have turned off tracking. */
	test_data.obtained_alloc = 333;
	test_data.obtained_dalloc = 444;
	ptr = mallocx(big_size, 0);
	free(ptr);
	expect_u64_eq(333, test_data.obtained_alloc, "");
	expect_u64_eq(444, test_data.obtained_dalloc, "");
}
TEST_END

int
main(void) {
	return test(
	    test_mallctl_errors,
	    test_mallctlnametomib_errors,
	    test_mallctlbymib_errors,
	    test_mallctl_read_write,
	    test_mallctlnametomib_short_mib,
	    test_mallctlnametomib_short_name,
	    test_mallctlmibnametomib,
	    test_mallctlbymibname,
	    test_mallctl_config,
	    test_mallctl_opt,
	    test_manpage_example,
	    test_tcache_none,
	    test_tcache,
	    test_thread_arena,
	    test_arena_i_initialized,
	    test_arena_i_dirty_decay_ms,
	    test_arena_i_muzzy_decay_ms,
	    test_arena_i_purge,
	    test_arena_i_decay,
	    test_arena_i_dss,
	    test_arena_i_retain_grow_limit,
	    test_arenas_dirty_decay_ms,
	    test_arenas_muzzy_decay_ms,
	    test_arenas_constants,
	    test_arenas_bin_constants,
	    test_arenas_lextent_constants,
	    test_arenas_create,
	    test_arenas_lookup,
	    test_prof_active,
	    test_stats_arenas,
	    test_hooks,
	    test_hooks_exhaustion,
	    test_thread_idle,
	    test_thread_peak,
	    test_thread_activity_callback);
}

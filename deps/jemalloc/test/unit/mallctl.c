#include "test/jemalloc_test.h"

#include "jemalloc/internal/util.h"

TEST_BEGIN(test_mallctl_errors) {
	uint64_t epoch;
	size_t sz;

	assert_d_eq(mallctl("no_such_name", NULL, NULL, NULL, 0), ENOENT,
	    "mallctl() should return ENOENT for non-existent names");

	assert_d_eq(mallctl("version", NULL, NULL, "0.0.0", strlen("0.0.0")),
	    EPERM, "mallctl() should return EPERM on attempt to write "
	    "read-only value");

	assert_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch,
	    sizeof(epoch)-1), EINVAL,
	    "mallctl() should return EINVAL for input size mismatch");
	assert_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch,
	    sizeof(epoch)+1), EINVAL,
	    "mallctl() should return EINVAL for input size mismatch");

	sz = sizeof(epoch)-1;
	assert_d_eq(mallctl("epoch", (void *)&epoch, &sz, NULL, 0), EINVAL,
	    "mallctl() should return EINVAL for output size mismatch");
	sz = sizeof(epoch)+1;
	assert_d_eq(mallctl("epoch", (void *)&epoch, &sz, NULL, 0), EINVAL,
	    "mallctl() should return EINVAL for output size mismatch");
}
TEST_END

TEST_BEGIN(test_mallctlnametomib_errors) {
	size_t mib[1];
	size_t miblen;

	miblen = sizeof(mib)/sizeof(size_t);
	assert_d_eq(mallctlnametomib("no_such_name", mib, &miblen), ENOENT,
	    "mallctlnametomib() should return ENOENT for non-existent names");
}
TEST_END

TEST_BEGIN(test_mallctlbymib_errors) {
	uint64_t epoch;
	size_t sz;
	size_t mib[1];
	size_t miblen;

	miblen = sizeof(mib)/sizeof(size_t);
	assert_d_eq(mallctlnametomib("version", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");

	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, "0.0.0",
	    strlen("0.0.0")), EPERM, "mallctl() should return EPERM on "
	    "attempt to write read-only value");

	miblen = sizeof(mib)/sizeof(size_t);
	assert_d_eq(mallctlnametomib("epoch", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");

	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, (void *)&epoch,
	    sizeof(epoch)-1), EINVAL,
	    "mallctlbymib() should return EINVAL for input size mismatch");
	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, (void *)&epoch,
	    sizeof(epoch)+1), EINVAL,
	    "mallctlbymib() should return EINVAL for input size mismatch");

	sz = sizeof(epoch)-1;
	assert_d_eq(mallctlbymib(mib, miblen, (void *)&epoch, &sz, NULL, 0),
	    EINVAL,
	    "mallctlbymib() should return EINVAL for output size mismatch");
	sz = sizeof(epoch)+1;
	assert_d_eq(mallctlbymib(mib, miblen, (void *)&epoch, &sz, NULL, 0),
	    EINVAL,
	    "mallctlbymib() should return EINVAL for output size mismatch");
}
TEST_END

TEST_BEGIN(test_mallctl_read_write) {
	uint64_t old_epoch, new_epoch;
	size_t sz = sizeof(old_epoch);

	/* Blind. */
	assert_d_eq(mallctl("epoch", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	assert_zu_eq(sz, sizeof(old_epoch), "Unexpected output size");

	/* Read. */
	assert_d_eq(mallctl("epoch", (void *)&old_epoch, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	assert_zu_eq(sz, sizeof(old_epoch), "Unexpected output size");

	/* Write. */
	assert_d_eq(mallctl("epoch", NULL, NULL, (void *)&new_epoch,
	    sizeof(new_epoch)), 0, "Unexpected mallctl() failure");
	assert_zu_eq(sz, sizeof(old_epoch), "Unexpected output size");

	/* Read+write. */
	assert_d_eq(mallctl("epoch", (void *)&old_epoch, &sz,
	    (void *)&new_epoch, sizeof(new_epoch)), 0,
	    "Unexpected mallctl() failure");
	assert_zu_eq(sz, sizeof(old_epoch), "Unexpected output size");
}
TEST_END

TEST_BEGIN(test_mallctlnametomib_short_mib) {
	size_t mib[4];
	size_t miblen;

	miblen = 3;
	mib[3] = 42;
	assert_d_eq(mallctlnametomib("arenas.bin.0.nregs", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	assert_zu_eq(miblen, 3, "Unexpected mib output length");
	assert_zu_eq(mib[3], 42,
	    "mallctlnametomib() wrote past the end of the input mib");
}
TEST_END

TEST_BEGIN(test_mallctl_config) {
#define TEST_MALLCTL_CONFIG(config, t) do {				\
	t oldval;							\
	size_t sz = sizeof(oldval);					\
	assert_d_eq(mallctl("config."#config, (void *)&oldval, &sz,	\
	    NULL, 0), 0, "Unexpected mallctl() failure");		\
	assert_b_eq(oldval, config_##config, "Incorrect config value");	\
	assert_zu_eq(sz, sizeof(oldval), "Unexpected output size");	\
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
	assert_d_eq(result, expected,					\
	    "Unexpected mallctl() result for opt."#opt);		\
	assert_zu_eq(sz, sizeof(oldval), "Unexpected output size");	\
} while (0)

	TEST_MALLCTL_OPT(bool, abort, always);
	TEST_MALLCTL_OPT(bool, abort_conf, always);
	TEST_MALLCTL_OPT(const char *, metadata_thp, always);
	TEST_MALLCTL_OPT(bool, retain, always);
	TEST_MALLCTL_OPT(const char *, dss, always);
	TEST_MALLCTL_OPT(unsigned, narenas, always);
	TEST_MALLCTL_OPT(const char *, percpu_arena, always);
	TEST_MALLCTL_OPT(bool, background_thread, always);
	TEST_MALLCTL_OPT(ssize_t, dirty_decay_ms, always);
	TEST_MALLCTL_OPT(ssize_t, muzzy_decay_ms, always);
	TEST_MALLCTL_OPT(bool, stats_print, always);
	TEST_MALLCTL_OPT(const char *, junk, fill);
	TEST_MALLCTL_OPT(bool, zero, fill);
	TEST_MALLCTL_OPT(bool, utrace, utrace);
	TEST_MALLCTL_OPT(bool, xmalloc, xmalloc);
	TEST_MALLCTL_OPT(bool, tcache, always);
	TEST_MALLCTL_OPT(size_t, lg_extent_max_active_fit, always);
	TEST_MALLCTL_OPT(size_t, lg_tcache_max, always);
	TEST_MALLCTL_OPT(const char *, thp, always);
	TEST_MALLCTL_OPT(bool, prof, prof);
	TEST_MALLCTL_OPT(const char *, prof_prefix, prof);
	TEST_MALLCTL_OPT(bool, prof_active, prof);
	TEST_MALLCTL_OPT(ssize_t, lg_prof_sample, prof);
	TEST_MALLCTL_OPT(bool, prof_accum, prof);
	TEST_MALLCTL_OPT(ssize_t, lg_prof_interval, prof);
	TEST_MALLCTL_OPT(bool, prof_gdump, prof);
	TEST_MALLCTL_OPT(bool, prof_final, prof);
	TEST_MALLCTL_OPT(bool, prof_leak, prof);

#undef TEST_MALLCTL_OPT
}
TEST_END

TEST_BEGIN(test_manpage_example) {
	unsigned nbins, i;
	size_t mib[4];
	size_t len, miblen;

	len = sizeof(nbins);
	assert_d_eq(mallctl("arenas.nbins", (void *)&nbins, &len, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	miblen = 4;
	assert_d_eq(mallctlnametomib("arenas.bin.0.size", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	for (i = 0; i < nbins; i++) {
		size_t bin_size;

		mib[2] = i;
		len = sizeof(bin_size);
		assert_d_eq(mallctlbymib(mib, miblen, (void *)&bin_size, &len,
		    NULL, 0), 0, "Unexpected mallctlbymib() failure");
		/* Do something with bin_size... */
	}
}
TEST_END

TEST_BEGIN(test_tcache_none) {
	test_skip_if(!opt_tcache);

	/* Allocate p and q. */
	void *p0 = mallocx(42, 0);
	assert_ptr_not_null(p0, "Unexpected mallocx() failure");
	void *q = mallocx(42, 0);
	assert_ptr_not_null(q, "Unexpected mallocx() failure");

	/* Deallocate p and q, but bypass the tcache for q. */
	dallocx(p0, 0);
	dallocx(q, MALLOCX_TCACHE_NONE);

	/* Make sure that tcache-based allocation returns p, not q. */
	void *p1 = mallocx(42, 0);
	assert_ptr_not_null(p1, "Unexpected mallocx() failure");
	assert_ptr_eq(p0, p1, "Expected tcache to allocate cached region");

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
		assert_d_eq(mallctl("tcache.create", (void *)&tis[i], &sz, NULL,
		    0), 0, "Unexpected mallctl() failure, i=%u", i);
	}

	/* Exercise tcache ID recycling. */
	for (i = 0; i < NTCACHES; i++) {
		assert_d_eq(mallctl("tcache.destroy", NULL, NULL,
		    (void *)&tis[i], sizeof(unsigned)), 0,
		    "Unexpected mallctl() failure, i=%u", i);
	}
	for (i = 0; i < NTCACHES; i++) {
		sz = sizeof(unsigned);
		assert_d_eq(mallctl("tcache.create", (void *)&tis[i], &sz, NULL,
		    0), 0, "Unexpected mallctl() failure, i=%u", i);
	}

	/* Flush empty tcaches. */
	for (i = 0; i < NTCACHES; i++) {
		assert_d_eq(mallctl("tcache.flush", NULL, NULL, (void *)&tis[i],
		    sizeof(unsigned)), 0, "Unexpected mallctl() failure, i=%u",
		    i);
	}

	/* Cache some allocations. */
	for (i = 0; i < NTCACHES; i++) {
		ps[i] = mallocx(psz, MALLOCX_TCACHE(tis[i]));
		assert_ptr_not_null(ps[i], "Unexpected mallocx() failure, i=%u",
		    i);
		dallocx(ps[i], MALLOCX_TCACHE(tis[i]));

		qs[i] = mallocx(qsz, MALLOCX_TCACHE(tis[i]));
		assert_ptr_not_null(qs[i], "Unexpected mallocx() failure, i=%u",
		    i);
		dallocx(qs[i], MALLOCX_TCACHE(tis[i]));
	}

	/* Verify that tcaches allocate cached regions. */
	for (i = 0; i < NTCACHES; i++) {
		void *p0 = ps[i];
		ps[i] = mallocx(psz, MALLOCX_TCACHE(tis[i]));
		assert_ptr_not_null(ps[i], "Unexpected mallocx() failure, i=%u",
		    i);
		assert_ptr_eq(ps[i], p0,
		    "Expected mallocx() to allocate cached region, i=%u", i);
	}

	/* Verify that reallocation uses cached regions. */
	for (i = 0; i < NTCACHES; i++) {
		void *q0 = qs[i];
		qs[i] = rallocx(ps[i], qsz, MALLOCX_TCACHE(tis[i]));
		assert_ptr_not_null(qs[i], "Unexpected rallocx() failure, i=%u",
		    i);
		assert_ptr_eq(qs[i], q0,
		    "Expected rallocx() to allocate cached region, i=%u", i);
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
		assert_d_eq(mallctl("tcache.flush", NULL, NULL, (void *)&tis[i],
		    sizeof(unsigned)), 0, "Unexpected mallctl() failure, i=%u",
		    i);
	}

	/* Destroy tcaches. */
	for (i = 0; i < NTCACHES; i++) {
		assert_d_eq(mallctl("tcache.destroy", NULL, NULL,
		    (void *)&tis[i], sizeof(unsigned)), 0,
		    "Unexpected mallctl() failure, i=%u", i);
	}
}
TEST_END

TEST_BEGIN(test_thread_arena) {
	unsigned old_arena_ind, new_arena_ind, narenas;

	const char *opa;
	size_t sz = sizeof(opa);
	assert_d_eq(mallctl("opt.percpu_arena", (void *)&opa, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	sz = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.narenas", (void *)&narenas, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	assert_u_eq(narenas, opt_narenas, "Number of arenas incorrect");

	if (strcmp(opa, "disabled") == 0) {
		new_arena_ind = narenas - 1;
		assert_d_eq(mallctl("thread.arena", (void *)&old_arena_ind, &sz,
		    (void *)&new_arena_ind, sizeof(unsigned)), 0,
		    "Unexpected mallctl() failure");
		new_arena_ind = 0;
		assert_d_eq(mallctl("thread.arena", (void *)&old_arena_ind, &sz,
		    (void *)&new_arena_ind, sizeof(unsigned)), 0,
		    "Unexpected mallctl() failure");
	} else {
		assert_d_eq(mallctl("thread.arena", (void *)&old_arena_ind, &sz,
		    NULL, 0), 0, "Unexpected mallctl() failure");
		new_arena_ind = percpu_arena_ind_limit(opt_percpu_arena) - 1;
		if (old_arena_ind != new_arena_ind) {
			assert_d_eq(mallctl("thread.arena",
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
	assert_d_eq(mallctl("arenas.narenas", (void *)&narenas, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");

	assert_d_eq(mallctlnametomib("arena.0.initialized", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	for (i = 0; i < narenas; i++) {
		mib[1] = i;
		sz = sizeof(initialized);
		assert_d_eq(mallctlbymib(mib, miblen, &initialized, &sz, NULL,
		    0), 0, "Unexpected mallctl() failure");
	}

	mib[1] = MALLCTL_ARENAS_ALL;
	sz = sizeof(initialized);
	assert_d_eq(mallctlbymib(mib, miblen, &initialized, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	assert_true(initialized,
	    "Merged arena statistics should always be initialized");

	/* Equivalent to the above but using mallctl() directly. */
	sz = sizeof(initialized);
	assert_d_eq(mallctl(
	    "arena." STRINGIFY(MALLCTL_ARENAS_ALL) ".initialized",
	    (void *)&initialized, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	assert_true(initialized,
	    "Merged arena statistics should always be initialized");
}
TEST_END

TEST_BEGIN(test_arena_i_dirty_decay_ms) {
	ssize_t dirty_decay_ms, orig_dirty_decay_ms, prev_dirty_decay_ms;
	size_t sz = sizeof(ssize_t);

	assert_d_eq(mallctl("arena.0.dirty_decay_ms",
	    (void *)&orig_dirty_decay_ms, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	dirty_decay_ms = -2;
	assert_d_eq(mallctl("arena.0.dirty_decay_ms", NULL, NULL,
	    (void *)&dirty_decay_ms, sizeof(ssize_t)), EFAULT,
	    "Unexpected mallctl() success");

	dirty_decay_ms = 0x7fffffff;
	assert_d_eq(mallctl("arena.0.dirty_decay_ms", NULL, NULL,
	    (void *)&dirty_decay_ms, sizeof(ssize_t)), 0,
	    "Unexpected mallctl() failure");

	for (prev_dirty_decay_ms = dirty_decay_ms, dirty_decay_ms = -1;
	    dirty_decay_ms < 20; prev_dirty_decay_ms = dirty_decay_ms,
	    dirty_decay_ms++) {
		ssize_t old_dirty_decay_ms;

		assert_d_eq(mallctl("arena.0.dirty_decay_ms",
		    (void *)&old_dirty_decay_ms, &sz, (void *)&dirty_decay_ms,
		    sizeof(ssize_t)), 0, "Unexpected mallctl() failure");
		assert_zd_eq(old_dirty_decay_ms, prev_dirty_decay_ms,
		    "Unexpected old arena.0.dirty_decay_ms");
	}
}
TEST_END

TEST_BEGIN(test_arena_i_muzzy_decay_ms) {
	ssize_t muzzy_decay_ms, orig_muzzy_decay_ms, prev_muzzy_decay_ms;
	size_t sz = sizeof(ssize_t);

	assert_d_eq(mallctl("arena.0.muzzy_decay_ms",
	    (void *)&orig_muzzy_decay_ms, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	muzzy_decay_ms = -2;
	assert_d_eq(mallctl("arena.0.muzzy_decay_ms", NULL, NULL,
	    (void *)&muzzy_decay_ms, sizeof(ssize_t)), EFAULT,
	    "Unexpected mallctl() success");

	muzzy_decay_ms = 0x7fffffff;
	assert_d_eq(mallctl("arena.0.muzzy_decay_ms", NULL, NULL,
	    (void *)&muzzy_decay_ms, sizeof(ssize_t)), 0,
	    "Unexpected mallctl() failure");

	for (prev_muzzy_decay_ms = muzzy_decay_ms, muzzy_decay_ms = -1;
	    muzzy_decay_ms < 20; prev_muzzy_decay_ms = muzzy_decay_ms,
	    muzzy_decay_ms++) {
		ssize_t old_muzzy_decay_ms;

		assert_d_eq(mallctl("arena.0.muzzy_decay_ms",
		    (void *)&old_muzzy_decay_ms, &sz, (void *)&muzzy_decay_ms,
		    sizeof(ssize_t)), 0, "Unexpected mallctl() failure");
		assert_zd_eq(old_muzzy_decay_ms, prev_muzzy_decay_ms,
		    "Unexpected old arena.0.muzzy_decay_ms");
	}
}
TEST_END

TEST_BEGIN(test_arena_i_purge) {
	unsigned narenas;
	size_t sz = sizeof(unsigned);
	size_t mib[3];
	size_t miblen = 3;

	assert_d_eq(mallctl("arena.0.purge", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	assert_d_eq(mallctl("arenas.narenas", (void *)&narenas, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	assert_d_eq(mallctlnametomib("arena.0.purge", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = narenas;
	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");

	mib[1] = MALLCTL_ARENAS_ALL;
	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
}
TEST_END

TEST_BEGIN(test_arena_i_decay) {
	unsigned narenas;
	size_t sz = sizeof(unsigned);
	size_t mib[3];
	size_t miblen = 3;

	assert_d_eq(mallctl("arena.0.decay", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	assert_d_eq(mallctl("arenas.narenas", (void *)&narenas, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	assert_d_eq(mallctlnametomib("arena.0.decay", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = narenas;
	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");

	mib[1] = MALLCTL_ARENAS_ALL;
	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
}
TEST_END

TEST_BEGIN(test_arena_i_dss) {
	const char *dss_prec_old, *dss_prec_new;
	size_t sz = sizeof(dss_prec_old);
	size_t mib[3];
	size_t miblen;

	miblen = sizeof(mib)/sizeof(size_t);
	assert_d_eq(mallctlnametomib("arena.0.dss", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() error");

	dss_prec_new = "disabled";
	assert_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_old, &sz,
	    (void *)&dss_prec_new, sizeof(dss_prec_new)), 0,
	    "Unexpected mallctl() failure");
	assert_str_ne(dss_prec_old, "primary",
	    "Unexpected default for dss precedence");

	assert_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_new, &sz,
	    (void *)&dss_prec_old, sizeof(dss_prec_old)), 0,
	    "Unexpected mallctl() failure");

	assert_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_old, &sz, NULL,
	    0), 0, "Unexpected mallctl() failure");
	assert_str_ne(dss_prec_old, "primary",
	    "Unexpected value for dss precedence");

	mib[1] = narenas_total_get();
	dss_prec_new = "disabled";
	assert_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_old, &sz,
	    (void *)&dss_prec_new, sizeof(dss_prec_new)), 0,
	    "Unexpected mallctl() failure");
	assert_str_ne(dss_prec_old, "primary",
	    "Unexpected default for dss precedence");

	assert_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_new, &sz,
	    (void *)&dss_prec_old, sizeof(dss_prec_new)), 0,
	    "Unexpected mallctl() failure");

	assert_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_old, &sz, NULL,
	    0), 0, "Unexpected mallctl() failure");
	assert_str_ne(dss_prec_old, "primary",
	    "Unexpected value for dss precedence");
}
TEST_END

TEST_BEGIN(test_arena_i_retain_grow_limit) {
	size_t old_limit, new_limit, default_limit;
	size_t mib[3];
	size_t miblen;

	bool retain_enabled;
	size_t sz = sizeof(retain_enabled);
	assert_d_eq(mallctl("opt.retain", &retain_enabled, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	test_skip_if(!retain_enabled);

	sz = sizeof(default_limit);
	miblen = sizeof(mib)/sizeof(size_t);
	assert_d_eq(mallctlnametomib("arena.0.retain_grow_limit", mib, &miblen),
	    0, "Unexpected mallctlnametomib() error");

	assert_d_eq(mallctlbymib(mib, miblen, &default_limit, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	assert_zu_eq(default_limit, sz_pind2sz(EXTENT_GROW_MAX_PIND),
	    "Unexpected default for retain_grow_limit");

	new_limit = PAGE - 1;
	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, &new_limit,
	    sizeof(new_limit)), EFAULT, "Unexpected mallctl() success");

	new_limit = PAGE + 1;
	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, &new_limit,
	    sizeof(new_limit)), 0, "Unexpected mallctl() failure");
	assert_d_eq(mallctlbymib(mib, miblen, &old_limit, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	assert_zu_eq(old_limit, PAGE,
	    "Unexpected value for retain_grow_limit");

	/* Expect grow less than psize class 10. */
	new_limit = sz_pind2sz(10) - 1;
	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, &new_limit,
	    sizeof(new_limit)), 0, "Unexpected mallctl() failure");
	assert_d_eq(mallctlbymib(mib, miblen, &old_limit, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	assert_zu_eq(old_limit, sz_pind2sz(9),
	    "Unexpected value for retain_grow_limit");

	/* Restore to default. */
	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, &default_limit,
	    sizeof(default_limit)), 0, "Unexpected mallctl() failure");
}
TEST_END

TEST_BEGIN(test_arenas_dirty_decay_ms) {
	ssize_t dirty_decay_ms, orig_dirty_decay_ms, prev_dirty_decay_ms;
	size_t sz = sizeof(ssize_t);

	assert_d_eq(mallctl("arenas.dirty_decay_ms",
	    (void *)&orig_dirty_decay_ms, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	dirty_decay_ms = -2;
	assert_d_eq(mallctl("arenas.dirty_decay_ms", NULL, NULL,
	    (void *)&dirty_decay_ms, sizeof(ssize_t)), EFAULT,
	    "Unexpected mallctl() success");

	dirty_decay_ms = 0x7fffffff;
	assert_d_eq(mallctl("arenas.dirty_decay_ms", NULL, NULL,
	    (void *)&dirty_decay_ms, sizeof(ssize_t)), 0,
	    "Expected mallctl() failure");

	for (prev_dirty_decay_ms = dirty_decay_ms, dirty_decay_ms = -1;
	    dirty_decay_ms < 20; prev_dirty_decay_ms = dirty_decay_ms,
	    dirty_decay_ms++) {
		ssize_t old_dirty_decay_ms;

		assert_d_eq(mallctl("arenas.dirty_decay_ms",
		    (void *)&old_dirty_decay_ms, &sz, (void *)&dirty_decay_ms,
		    sizeof(ssize_t)), 0, "Unexpected mallctl() failure");
		assert_zd_eq(old_dirty_decay_ms, prev_dirty_decay_ms,
		    "Unexpected old arenas.dirty_decay_ms");
	}
}
TEST_END

TEST_BEGIN(test_arenas_muzzy_decay_ms) {
	ssize_t muzzy_decay_ms, orig_muzzy_decay_ms, prev_muzzy_decay_ms;
	size_t sz = sizeof(ssize_t);

	assert_d_eq(mallctl("arenas.muzzy_decay_ms",
	    (void *)&orig_muzzy_decay_ms, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	muzzy_decay_ms = -2;
	assert_d_eq(mallctl("arenas.muzzy_decay_ms", NULL, NULL,
	    (void *)&muzzy_decay_ms, sizeof(ssize_t)), EFAULT,
	    "Unexpected mallctl() success");

	muzzy_decay_ms = 0x7fffffff;
	assert_d_eq(mallctl("arenas.muzzy_decay_ms", NULL, NULL,
	    (void *)&muzzy_decay_ms, sizeof(ssize_t)), 0,
	    "Expected mallctl() failure");

	for (prev_muzzy_decay_ms = muzzy_decay_ms, muzzy_decay_ms = -1;
	    muzzy_decay_ms < 20; prev_muzzy_decay_ms = muzzy_decay_ms,
	    muzzy_decay_ms++) {
		ssize_t old_muzzy_decay_ms;

		assert_d_eq(mallctl("arenas.muzzy_decay_ms",
		    (void *)&old_muzzy_decay_ms, &sz, (void *)&muzzy_decay_ms,
		    sizeof(ssize_t)), 0, "Unexpected mallctl() failure");
		assert_zd_eq(old_muzzy_decay_ms, prev_muzzy_decay_ms,
		    "Unexpected old arenas.muzzy_decay_ms");
	}
}
TEST_END

TEST_BEGIN(test_arenas_constants) {
#define TEST_ARENAS_CONSTANT(t, name, expected) do {			\
	t name;								\
	size_t sz = sizeof(t);						\
	assert_d_eq(mallctl("arenas."#name, (void *)&name, &sz, NULL,	\
	    0), 0, "Unexpected mallctl() failure");			\
	assert_zu_eq(name, expected, "Incorrect "#name" size");		\
} while (0)

	TEST_ARENAS_CONSTANT(size_t, quantum, QUANTUM);
	TEST_ARENAS_CONSTANT(size_t, page, PAGE);
	TEST_ARENAS_CONSTANT(unsigned, nbins, NBINS);
	TEST_ARENAS_CONSTANT(unsigned, nlextents, NSIZES - NBINS);

#undef TEST_ARENAS_CONSTANT
}
TEST_END

TEST_BEGIN(test_arenas_bin_constants) {
#define TEST_ARENAS_BIN_CONSTANT(t, name, expected) do {		\
	t name;								\
	size_t sz = sizeof(t);						\
	assert_d_eq(mallctl("arenas.bin.0."#name, (void *)&name, &sz,	\
	    NULL, 0), 0, "Unexpected mallctl() failure");		\
	assert_zu_eq(name, expected, "Incorrect "#name" size");		\
} while (0)

	TEST_ARENAS_BIN_CONSTANT(size_t, size, bin_infos[0].reg_size);
	TEST_ARENAS_BIN_CONSTANT(uint32_t, nregs, bin_infos[0].nregs);
	TEST_ARENAS_BIN_CONSTANT(size_t, slab_size,
	    bin_infos[0].slab_size);

#undef TEST_ARENAS_BIN_CONSTANT
}
TEST_END

TEST_BEGIN(test_arenas_lextent_constants) {
#define TEST_ARENAS_LEXTENT_CONSTANT(t, name, expected) do {		\
	t name;								\
	size_t sz = sizeof(t);						\
	assert_d_eq(mallctl("arenas.lextent.0."#name, (void *)&name,	\
	    &sz, NULL, 0), 0, "Unexpected mallctl() failure");		\
	assert_zu_eq(name, expected, "Incorrect "#name" size");		\
} while (0)

	TEST_ARENAS_LEXTENT_CONSTANT(size_t, size, LARGE_MINCLASS);

#undef TEST_ARENAS_LEXTENT_CONSTANT
}
TEST_END

TEST_BEGIN(test_arenas_create) {
	unsigned narenas_before, arena, narenas_after;
	size_t sz = sizeof(unsigned);

	assert_d_eq(mallctl("arenas.narenas", (void *)&narenas_before, &sz,
	    NULL, 0), 0, "Unexpected mallctl() failure");
	assert_d_eq(mallctl("arenas.create", (void *)&arena, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	assert_d_eq(mallctl("arenas.narenas", (void *)&narenas_after, &sz, NULL,
	    0), 0, "Unexpected mallctl() failure");

	assert_u_eq(narenas_before+1, narenas_after,
	    "Unexpected number of arenas before versus after extension");
	assert_u_eq(arena, narenas_after-1, "Unexpected arena index");
}
TEST_END

TEST_BEGIN(test_arenas_lookup) {
	unsigned arena, arena1;
	void *ptr;
	size_t sz = sizeof(unsigned);

	assert_d_eq(mallctl("arenas.create", (void *)&arena, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	ptr = mallocx(42, MALLOCX_ARENA(arena) | MALLOCX_TCACHE_NONE);
	assert_ptr_not_null(ptr, "Unexpected mallocx() failure");
	assert_d_eq(mallctl("arenas.lookup", &arena1, &sz, &ptr, sizeof(ptr)),
	    0, "Unexpected mallctl() failure");
	assert_u_eq(arena, arena1, "Unexpected arena index");
	dallocx(ptr, 0);
}
TEST_END

TEST_BEGIN(test_stats_arenas) {
#define TEST_STATS_ARENAS(t, name) do {					\
	t name;								\
	size_t sz = sizeof(t);						\
	assert_d_eq(mallctl("stats.arenas.0."#name, (void *)&name, &sz,	\
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

int
main(void) {
	return test(
	    test_mallctl_errors,
	    test_mallctlnametomib_errors,
	    test_mallctlbymib_errors,
	    test_mallctl_read_write,
	    test_mallctlnametomib_short_mib,
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
	    test_stats_arenas);
}

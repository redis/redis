static inline unsigned
do_arena_create(ssize_t dirty_decay_ms, ssize_t muzzy_decay_ms) {
	unsigned arena_ind;
	size_t sz = sizeof(unsigned);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	size_t mib[3];
	size_t miblen = sizeof(mib)/sizeof(size_t);

	expect_d_eq(mallctlnametomib("arena.0.dirty_decay_ms", mib, &miblen),
	    0, "Unexpected mallctlnametomib() failure");
	mib[1] = (size_t)arena_ind;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL,
	    (void *)&dirty_decay_ms, sizeof(dirty_decay_ms)), 0,
	    "Unexpected mallctlbymib() failure");

	expect_d_eq(mallctlnametomib("arena.0.muzzy_decay_ms", mib, &miblen),
	    0, "Unexpected mallctlnametomib() failure");
	mib[1] = (size_t)arena_ind;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL,
	    (void *)&muzzy_decay_ms, sizeof(muzzy_decay_ms)), 0,
	    "Unexpected mallctlbymib() failure");

	return arena_ind;
}

static inline void
do_arena_destroy(unsigned arena_ind) {
	/* 
	 * For convenience, flush tcache in case there are cached items.
	 * However not assert success since the tcache may be disabled.
	 */
	mallctl("thread.tcache.flush", NULL, NULL, NULL, 0);

	size_t mib[3];
	size_t miblen = sizeof(mib)/sizeof(size_t);
	expect_d_eq(mallctlnametomib("arena.0.destroy", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = (size_t)arena_ind;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
}

static inline void
do_epoch(void) {
	uint64_t epoch = 1;
	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");
}

static inline void
do_purge(unsigned arena_ind) {
	size_t mib[3];
	size_t miblen = sizeof(mib)/sizeof(size_t);
	expect_d_eq(mallctlnametomib("arena.0.purge", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = (size_t)arena_ind;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
}

static inline void
do_decay(unsigned arena_ind) {
	size_t mib[3];
	size_t miblen = sizeof(mib)/sizeof(size_t);
	expect_d_eq(mallctlnametomib("arena.0.decay", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = (size_t)arena_ind;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
}

static inline uint64_t
get_arena_npurge_impl(const char *mibname, unsigned arena_ind) {
	size_t mib[4];
	size_t miblen = sizeof(mib)/sizeof(size_t);
	expect_d_eq(mallctlnametomib(mibname, mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[2] = (size_t)arena_ind;
	uint64_t npurge = 0;
	size_t sz = sizeof(npurge);
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&npurge, &sz, NULL, 0),
	    config_stats ? 0 : ENOENT, "Unexpected mallctlbymib() failure");
	return npurge;
}

static inline uint64_t
get_arena_dirty_npurge(unsigned arena_ind) {
	do_epoch();
	return get_arena_npurge_impl("stats.arenas.0.dirty_npurge", arena_ind);
}

static inline uint64_t
get_arena_dirty_purged(unsigned arena_ind) {
	do_epoch();
	return get_arena_npurge_impl("stats.arenas.0.dirty_purged", arena_ind);
}

static inline uint64_t
get_arena_muzzy_npurge(unsigned arena_ind) {
	do_epoch();
	return get_arena_npurge_impl("stats.arenas.0.muzzy_npurge", arena_ind);
}

static inline uint64_t
get_arena_npurge(unsigned arena_ind) {
	do_epoch();
	return get_arena_npurge_impl("stats.arenas.0.dirty_npurge", arena_ind) +
	    get_arena_npurge_impl("stats.arenas.0.muzzy_npurge", arena_ind);
}

static inline size_t
get_arena_pdirty(unsigned arena_ind) {
	do_epoch();
	size_t mib[4];
	size_t miblen = sizeof(mib)/sizeof(size_t);
	expect_d_eq(mallctlnametomib("stats.arenas.0.pdirty", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[2] = (size_t)arena_ind;
	size_t pdirty;
	size_t sz = sizeof(pdirty);
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&pdirty, &sz, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
	return pdirty;
}

static inline size_t
get_arena_pmuzzy(unsigned arena_ind) {
	do_epoch();
	size_t mib[4];
	size_t miblen = sizeof(mib)/sizeof(size_t);
	expect_d_eq(mallctlnametomib("stats.arenas.0.pmuzzy", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[2] = (size_t)arena_ind;
	size_t pmuzzy;
	size_t sz = sizeof(pmuzzy);
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&pmuzzy, &sz, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
	return pmuzzy;
}

static inline void *
do_mallocx(size_t size, int flags) {
	void *p = mallocx(size, flags);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	return p;
}

static inline void
generate_dirty(unsigned arena_ind, size_t size) {
	int flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;
	void *p = do_mallocx(size, flags);
	dallocx(p, flags);
}


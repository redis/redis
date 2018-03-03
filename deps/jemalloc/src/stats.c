#define	JEMALLOC_STATS_C_
#include "jemalloc/internal/jemalloc_internal.h"

#define	CTL_GET(n, v, t) do {						\
	size_t sz = sizeof(t);						\
	xmallctl(n, v, &sz, NULL, 0);					\
} while (0)

#define	CTL_M2_GET(n, i, v, t) do {					\
	size_t mib[6];							\
	size_t miblen = sizeof(mib) / sizeof(size_t);			\
	size_t sz = sizeof(t);						\
	xmallctlnametomib(n, mib, &miblen);				\
	mib[2] = (i);							\
	xmallctlbymib(mib, miblen, v, &sz, NULL, 0);			\
} while (0)

#define	CTL_M2_M4_GET(n, i, j, v, t) do {				\
	size_t mib[6];							\
	size_t miblen = sizeof(mib) / sizeof(size_t);			\
	size_t sz = sizeof(t);						\
	xmallctlnametomib(n, mib, &miblen);				\
	mib[2] = (i);							\
	mib[4] = (j);							\
	xmallctlbymib(mib, miblen, v, &sz, NULL, 0);			\
} while (0)

/******************************************************************************/
/* Data. */

bool	opt_stats_print = false;

size_t	stats_cactive = 0;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	stats_arena_bins_print(void (*write_cb)(void *, const char *),
    void *cbopaque, unsigned i);
static void	stats_arena_lruns_print(void (*write_cb)(void *, const char *),
    void *cbopaque, unsigned i);
static void	stats_arena_hchunks_print(
    void (*write_cb)(void *, const char *), void *cbopaque, unsigned i);
static void	stats_arena_print(void (*write_cb)(void *, const char *),
    void *cbopaque, unsigned i, bool bins, bool large, bool huge);

/******************************************************************************/

static void
stats_arena_bins_print(void (*write_cb)(void *, const char *), void *cbopaque,
    unsigned i)
{
	size_t page;
	bool config_tcache, in_gap;
	unsigned nbins, j;

	CTL_GET("arenas.page", &page, size_t);

	CTL_GET("config.tcache", &config_tcache, bool);
	if (config_tcache) {
		malloc_cprintf(write_cb, cbopaque,
		    "bins:           size ind    allocated      nmalloc"
		    "      ndalloc    nrequests      curregs      curruns regs"
		    " pgs  util       nfills     nflushes      newruns"
		    "       reruns\n");
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "bins:           size ind    allocated      nmalloc"
		    "      ndalloc    nrequests      curregs      curruns regs"
		    " pgs  util      newruns       reruns\n");
	}
	CTL_GET("arenas.nbins", &nbins, unsigned);
	for (j = 0, in_gap = false; j < nbins; j++) {
		uint64_t nruns;

		CTL_M2_M4_GET("stats.arenas.0.bins.0.nruns", i, j, &nruns,
		    uint64_t);
		if (nruns == 0)
			in_gap = true;
		else {
			size_t reg_size, run_size, curregs, availregs, milli;
			size_t curruns;
			uint32_t nregs;
			uint64_t nmalloc, ndalloc, nrequests, nfills, nflushes;
			uint64_t reruns;
			char util[6]; /* "x.yyy". */

			if (in_gap) {
				malloc_cprintf(write_cb, cbopaque,
				    "                     ---\n");
				in_gap = false;
			}
			CTL_M2_GET("arenas.bin.0.size", j, &reg_size, size_t);
			CTL_M2_GET("arenas.bin.0.nregs", j, &nregs, uint32_t);
			CTL_M2_GET("arenas.bin.0.run_size", j, &run_size,
			    size_t);
			CTL_M2_M4_GET("stats.arenas.0.bins.0.nmalloc", i, j,
			    &nmalloc, uint64_t);
			CTL_M2_M4_GET("stats.arenas.0.bins.0.ndalloc", i, j,
			    &ndalloc, uint64_t);
			CTL_M2_M4_GET("stats.arenas.0.bins.0.curregs", i, j,
			    &curregs, size_t);
			CTL_M2_M4_GET("stats.arenas.0.bins.0.nrequests", i, j,
			    &nrequests, uint64_t);
			if (config_tcache) {
				CTL_M2_M4_GET("stats.arenas.0.bins.0.nfills", i,
				    j, &nfills, uint64_t);
				CTL_M2_M4_GET("stats.arenas.0.bins.0.nflushes",
				    i, j, &nflushes, uint64_t);
			}
			CTL_M2_M4_GET("stats.arenas.0.bins.0.nreruns", i, j,
			    &reruns, uint64_t);
			CTL_M2_M4_GET("stats.arenas.0.bins.0.curruns", i, j,
			    &curruns, size_t);

			availregs = nregs * curruns;
			milli = (availregs != 0) ? (1000 * curregs) / availregs
			    : 1000;
			assert(milli <= 1000);
			if (milli < 10) {
				malloc_snprintf(util, sizeof(util),
				    "0.00%zu", milli);
			} else if (milli < 100) {
				malloc_snprintf(util, sizeof(util), "0.0%zu",
				    milli);
			} else if (milli < 1000) {
				malloc_snprintf(util, sizeof(util), "0.%zu",
				    milli);
			} else
				malloc_snprintf(util, sizeof(util), "1");

			if (config_tcache) {
				malloc_cprintf(write_cb, cbopaque,
				    "%20zu %3u %12zu %12"FMTu64
				    " %12"FMTu64" %12"FMTu64" %12zu"
				    " %12zu %4u %3zu %-5s %12"FMTu64
				    " %12"FMTu64" %12"FMTu64" %12"FMTu64"\n",
				    reg_size, j, curregs * reg_size, nmalloc,
				    ndalloc, nrequests, curregs, curruns, nregs,
				    run_size / page, util, nfills, nflushes,
				    nruns, reruns);
			} else {
				malloc_cprintf(write_cb, cbopaque,
				    "%20zu %3u %12zu %12"FMTu64
				    " %12"FMTu64" %12"FMTu64" %12zu"
				    " %12zu %4u %3zu %-5s %12"FMTu64
				    " %12"FMTu64"\n",
				    reg_size, j, curregs * reg_size, nmalloc,
				    ndalloc, nrequests, curregs, curruns, nregs,
				    run_size / page, util, nruns, reruns);
			}
		}
	}
	if (in_gap) {
		malloc_cprintf(write_cb, cbopaque,
		    "                     ---\n");
	}
}

static void
stats_arena_lruns_print(void (*write_cb)(void *, const char *), void *cbopaque,
    unsigned i)
{
	unsigned nbins, nlruns, j;
	bool in_gap;

	malloc_cprintf(write_cb, cbopaque,
	    "large:          size ind    allocated      nmalloc      ndalloc"
	    "    nrequests      curruns\n");
	CTL_GET("arenas.nbins", &nbins, unsigned);
	CTL_GET("arenas.nlruns", &nlruns, unsigned);
	for (j = 0, in_gap = false; j < nlruns; j++) {
		uint64_t nmalloc, ndalloc, nrequests;
		size_t run_size, curruns;

		CTL_M2_M4_GET("stats.arenas.0.lruns.0.nmalloc", i, j, &nmalloc,
		    uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.lruns.0.ndalloc", i, j, &ndalloc,
		    uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.lruns.0.nrequests", i, j,
		    &nrequests, uint64_t);
		if (nrequests == 0)
			in_gap = true;
		else {
			CTL_M2_GET("arenas.lrun.0.size", j, &run_size, size_t);
			CTL_M2_M4_GET("stats.arenas.0.lruns.0.curruns", i, j,
			    &curruns, size_t);
			if (in_gap) {
				malloc_cprintf(write_cb, cbopaque,
				    "                     ---\n");
				in_gap = false;
			}
			malloc_cprintf(write_cb, cbopaque,
			    "%20zu %3u %12zu %12"FMTu64" %12"FMTu64
			    " %12"FMTu64" %12zu\n",
			    run_size, nbins + j, curruns * run_size, nmalloc,
			    ndalloc, nrequests, curruns);
		}
	}
	if (in_gap) {
		malloc_cprintf(write_cb, cbopaque,
		    "                     ---\n");
	}
}

static void
stats_arena_hchunks_print(void (*write_cb)(void *, const char *),
    void *cbopaque, unsigned i)
{
	unsigned nbins, nlruns, nhchunks, j;
	bool in_gap;

	malloc_cprintf(write_cb, cbopaque,
	    "huge:           size ind    allocated      nmalloc      ndalloc"
	    "    nrequests   curhchunks\n");
	CTL_GET("arenas.nbins", &nbins, unsigned);
	CTL_GET("arenas.nlruns", &nlruns, unsigned);
	CTL_GET("arenas.nhchunks", &nhchunks, unsigned);
	for (j = 0, in_gap = false; j < nhchunks; j++) {
		uint64_t nmalloc, ndalloc, nrequests;
		size_t hchunk_size, curhchunks;

		CTL_M2_M4_GET("stats.arenas.0.hchunks.0.nmalloc", i, j,
		    &nmalloc, uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.hchunks.0.ndalloc", i, j,
		    &ndalloc, uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.hchunks.0.nrequests", i, j,
		    &nrequests, uint64_t);
		if (nrequests == 0)
			in_gap = true;
		else {
			CTL_M2_GET("arenas.hchunk.0.size", j, &hchunk_size,
			    size_t);
			CTL_M2_M4_GET("stats.arenas.0.hchunks.0.curhchunks", i,
			    j, &curhchunks, size_t);
			if (in_gap) {
				malloc_cprintf(write_cb, cbopaque,
				    "                     ---\n");
				in_gap = false;
			}
			malloc_cprintf(write_cb, cbopaque,
			    "%20zu %3u %12zu %12"FMTu64" %12"FMTu64
			    " %12"FMTu64" %12zu\n",
			    hchunk_size, nbins + nlruns + j,
			    curhchunks * hchunk_size, nmalloc, ndalloc,
			    nrequests, curhchunks);
		}
	}
	if (in_gap) {
		malloc_cprintf(write_cb, cbopaque,
		    "                     ---\n");
	}
}

static void
stats_arena_print(void (*write_cb)(void *, const char *), void *cbopaque,
    unsigned i, bool bins, bool large, bool huge)
{
	unsigned nthreads;
	const char *dss;
	ssize_t lg_dirty_mult;
	size_t page, pactive, pdirty, mapped;
	size_t metadata_mapped, metadata_allocated;
	uint64_t npurge, nmadvise, purged;
	size_t small_allocated;
	uint64_t small_nmalloc, small_ndalloc, small_nrequests;
	size_t large_allocated;
	uint64_t large_nmalloc, large_ndalloc, large_nrequests;
	size_t huge_allocated;
	uint64_t huge_nmalloc, huge_ndalloc, huge_nrequests;

	CTL_GET("arenas.page", &page, size_t);

	CTL_M2_GET("stats.arenas.0.nthreads", i, &nthreads, unsigned);
	malloc_cprintf(write_cb, cbopaque,
	    "assigned threads: %u\n", nthreads);
	CTL_M2_GET("stats.arenas.0.dss", i, &dss, const char *);
	malloc_cprintf(write_cb, cbopaque, "dss allocation precedence: %s\n",
	    dss);
	CTL_M2_GET("stats.arenas.0.lg_dirty_mult", i, &lg_dirty_mult, ssize_t);
	if (lg_dirty_mult >= 0) {
		malloc_cprintf(write_cb, cbopaque,
		    "min active:dirty page ratio: %u:1\n",
		    (1U << lg_dirty_mult));
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "min active:dirty page ratio: N/A\n");
	}
	CTL_M2_GET("stats.arenas.0.pactive", i, &pactive, size_t);
	CTL_M2_GET("stats.arenas.0.pdirty", i, &pdirty, size_t);
	CTL_M2_GET("stats.arenas.0.npurge", i, &npurge, uint64_t);
	CTL_M2_GET("stats.arenas.0.nmadvise", i, &nmadvise, uint64_t);
	CTL_M2_GET("stats.arenas.0.purged", i, &purged, uint64_t);
	malloc_cprintf(write_cb, cbopaque,
	    "dirty pages: %zu:%zu active:dirty, %"FMTu64" sweep%s, %"FMTu64
	    " madvise%s, %"FMTu64" purged\n", pactive, pdirty, npurge, npurge ==
	    1 ? "" : "s", nmadvise, nmadvise == 1 ? "" : "s", purged);

	malloc_cprintf(write_cb, cbopaque,
	    "                            allocated      nmalloc      ndalloc"
	    "    nrequests\n");
	CTL_M2_GET("stats.arenas.0.small.allocated", i, &small_allocated,
	    size_t);
	CTL_M2_GET("stats.arenas.0.small.nmalloc", i, &small_nmalloc, uint64_t);
	CTL_M2_GET("stats.arenas.0.small.ndalloc", i, &small_ndalloc, uint64_t);
	CTL_M2_GET("stats.arenas.0.small.nrequests", i, &small_nrequests,
	    uint64_t);
	malloc_cprintf(write_cb, cbopaque,
	    "small:                   %12zu %12"FMTu64" %12"FMTu64
	    " %12"FMTu64"\n",
	    small_allocated, small_nmalloc, small_ndalloc, small_nrequests);
	CTL_M2_GET("stats.arenas.0.large.allocated", i, &large_allocated,
	    size_t);
	CTL_M2_GET("stats.arenas.0.large.nmalloc", i, &large_nmalloc, uint64_t);
	CTL_M2_GET("stats.arenas.0.large.ndalloc", i, &large_ndalloc, uint64_t);
	CTL_M2_GET("stats.arenas.0.large.nrequests", i, &large_nrequests,
	    uint64_t);
	malloc_cprintf(write_cb, cbopaque,
	    "large:                   %12zu %12"FMTu64" %12"FMTu64
	    " %12"FMTu64"\n",
	    large_allocated, large_nmalloc, large_ndalloc, large_nrequests);
	CTL_M2_GET("stats.arenas.0.huge.allocated", i, &huge_allocated, size_t);
	CTL_M2_GET("stats.arenas.0.huge.nmalloc", i, &huge_nmalloc, uint64_t);
	CTL_M2_GET("stats.arenas.0.huge.ndalloc", i, &huge_ndalloc, uint64_t);
	CTL_M2_GET("stats.arenas.0.huge.nrequests", i, &huge_nrequests,
	    uint64_t);
	malloc_cprintf(write_cb, cbopaque,
	    "huge:                    %12zu %12"FMTu64" %12"FMTu64
	    " %12"FMTu64"\n",
	    huge_allocated, huge_nmalloc, huge_ndalloc, huge_nrequests);
	malloc_cprintf(write_cb, cbopaque,
	    "total:                   %12zu %12"FMTu64" %12"FMTu64
	    " %12"FMTu64"\n",
	    small_allocated + large_allocated + huge_allocated,
	    small_nmalloc + large_nmalloc + huge_nmalloc,
	    small_ndalloc + large_ndalloc + huge_ndalloc,
	    small_nrequests + large_nrequests + huge_nrequests);
	malloc_cprintf(write_cb, cbopaque,
	    "active:                  %12zu\n", pactive * page);
	CTL_M2_GET("stats.arenas.0.mapped", i, &mapped, size_t);
	malloc_cprintf(write_cb, cbopaque,
	    "mapped:                  %12zu\n", mapped);
	CTL_M2_GET("stats.arenas.0.metadata.mapped", i, &metadata_mapped,
	    size_t);
	CTL_M2_GET("stats.arenas.0.metadata.allocated", i, &metadata_allocated,
	    size_t);
	malloc_cprintf(write_cb, cbopaque,
	    "metadata: mapped: %zu, allocated: %zu\n",
	    metadata_mapped, metadata_allocated);

	if (bins)
		stats_arena_bins_print(write_cb, cbopaque, i);
	if (large)
		stats_arena_lruns_print(write_cb, cbopaque, i);
	if (huge)
		stats_arena_hchunks_print(write_cb, cbopaque, i);
}

void
stats_print(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *opts)
{
	int err;
	uint64_t epoch;
	size_t u64sz;
	bool general = true;
	bool merged = true;
	bool unmerged = true;
	bool bins = true;
	bool large = true;
	bool huge = true;

	/*
	 * Refresh stats, in case mallctl() was called by the application.
	 *
	 * Check for OOM here, since refreshing the ctl cache can trigger
	 * allocation.  In practice, none of the subsequent mallctl()-related
	 * calls in this function will cause OOM if this one succeeds.
	 * */
	epoch = 1;
	u64sz = sizeof(uint64_t);
	err = je_mallctl("epoch", &epoch, &u64sz, &epoch, sizeof(uint64_t));
	if (err != 0) {
		if (err == EAGAIN) {
			malloc_write("<jemalloc>: Memory allocation failure in "
			    "mallctl(\"epoch\", ...)\n");
			return;
		}
		malloc_write("<jemalloc>: Failure in mallctl(\"epoch\", "
		    "...)\n");
		abort();
	}

	if (opts != NULL) {
		unsigned i;

		for (i = 0; opts[i] != '\0'; i++) {
			switch (opts[i]) {
			case 'g':
				general = false;
				break;
			case 'm':
				merged = false;
				break;
			case 'a':
				unmerged = false;
				break;
			case 'b':
				bins = false;
				break;
			case 'l':
				large = false;
				break;
			case 'h':
				huge = false;
				break;
			default:;
			}
		}
	}

	malloc_cprintf(write_cb, cbopaque,
	    "___ Begin jemalloc statistics ___\n");
	if (general) {
		const char *cpv;
		bool bv;
		unsigned uv;
		ssize_t ssv;
		size_t sv, bsz, ssz, sssz, cpsz;

		bsz = sizeof(bool);
		ssz = sizeof(size_t);
		sssz = sizeof(ssize_t);
		cpsz = sizeof(const char *);

		CTL_GET("version", &cpv, const char *);
		malloc_cprintf(write_cb, cbopaque, "Version: %s\n", cpv);
		CTL_GET("config.debug", &bv, bool);
		malloc_cprintf(write_cb, cbopaque, "Assertions %s\n",
		    bv ? "enabled" : "disabled");

#define	OPT_WRITE_BOOL(n)						\
		if (je_mallctl("opt."#n, &bv, &bsz, NULL, 0) == 0) {	\
			malloc_cprintf(write_cb, cbopaque,		\
			    "  opt."#n": %s\n", bv ? "true" : "false");	\
		}
#define	OPT_WRITE_BOOL_MUTABLE(n, m) {					\
		bool bv2;						\
		if (je_mallctl("opt."#n, &bv, &bsz, NULL, 0) == 0 &&	\
		    je_mallctl(#m, &bv2, &bsz, NULL, 0) == 0) {		\
			malloc_cprintf(write_cb, cbopaque,		\
			    "  opt."#n": %s ("#m": %s)\n", bv ? "true"	\
			    : "false", bv2 ? "true" : "false");		\
		}							\
}
#define	OPT_WRITE_SIZE_T(n)						\
		if (je_mallctl("opt."#n, &sv, &ssz, NULL, 0) == 0) {	\
			malloc_cprintf(write_cb, cbopaque,		\
			"  opt."#n": %zu\n", sv);			\
		}
#define	OPT_WRITE_SSIZE_T(n)						\
		if (je_mallctl("opt."#n, &ssv, &sssz, NULL, 0) == 0) {	\
			malloc_cprintf(write_cb, cbopaque,		\
			    "  opt."#n": %zd\n", ssv);			\
		}
#define	OPT_WRITE_SSIZE_T_MUTABLE(n, m) {				\
		ssize_t ssv2;						\
		if (je_mallctl("opt."#n, &ssv, &sssz, NULL, 0) == 0 &&	\
		    je_mallctl(#m, &ssv2, &sssz, NULL, 0) == 0) {	\
			malloc_cprintf(write_cb, cbopaque,		\
			    "  opt."#n": %zd ("#m": %zd)\n",		\
			    ssv, ssv2);					\
		}							\
}
#define	OPT_WRITE_CHAR_P(n)						\
		if (je_mallctl("opt."#n, &cpv, &cpsz, NULL, 0) == 0) {	\
			malloc_cprintf(write_cb, cbopaque,		\
			    "  opt."#n": \"%s\"\n", cpv);		\
		}

		malloc_cprintf(write_cb, cbopaque,
		    "Run-time option settings:\n");
		OPT_WRITE_BOOL(abort)
		OPT_WRITE_SIZE_T(lg_chunk)
		OPT_WRITE_CHAR_P(dss)
		OPT_WRITE_SIZE_T(narenas)
		OPT_WRITE_SSIZE_T_MUTABLE(lg_dirty_mult, arenas.lg_dirty_mult)
		OPT_WRITE_BOOL(stats_print)
		OPT_WRITE_CHAR_P(junk)
		OPT_WRITE_SIZE_T(quarantine)
		OPT_WRITE_BOOL(redzone)
		OPT_WRITE_BOOL(zero)
		OPT_WRITE_BOOL(utrace)
		OPT_WRITE_BOOL(valgrind)
		OPT_WRITE_BOOL(xmalloc)
		OPT_WRITE_BOOL(tcache)
		OPT_WRITE_SSIZE_T(lg_tcache_max)
		OPT_WRITE_BOOL(prof)
		OPT_WRITE_CHAR_P(prof_prefix)
		OPT_WRITE_BOOL_MUTABLE(prof_active, prof.active)
		OPT_WRITE_BOOL_MUTABLE(prof_thread_active_init,
		    prof.thread_active_init)
		OPT_WRITE_SSIZE_T(lg_prof_sample)
		OPT_WRITE_BOOL(prof_accum)
		OPT_WRITE_SSIZE_T(lg_prof_interval)
		OPT_WRITE_BOOL(prof_gdump)
		OPT_WRITE_BOOL(prof_final)
		OPT_WRITE_BOOL(prof_leak)

#undef OPT_WRITE_BOOL
#undef OPT_WRITE_BOOL_MUTABLE
#undef OPT_WRITE_SIZE_T
#undef OPT_WRITE_SSIZE_T
#undef OPT_WRITE_CHAR_P

		malloc_cprintf(write_cb, cbopaque, "CPUs: %u\n", ncpus);

		CTL_GET("arenas.narenas", &uv, unsigned);
		malloc_cprintf(write_cb, cbopaque, "Arenas: %u\n", uv);

		malloc_cprintf(write_cb, cbopaque, "Pointer size: %zu\n",
		    sizeof(void *));

		CTL_GET("arenas.quantum", &sv, size_t);
		malloc_cprintf(write_cb, cbopaque, "Quantum size: %zu\n",
		    sv);

		CTL_GET("arenas.page", &sv, size_t);
		malloc_cprintf(write_cb, cbopaque, "Page size: %zu\n", sv);

		CTL_GET("arenas.lg_dirty_mult", &ssv, ssize_t);
		if (ssv >= 0) {
			malloc_cprintf(write_cb, cbopaque,
			    "Min active:dirty page ratio per arena: %u:1\n",
			    (1U << ssv));
		} else {
			malloc_cprintf(write_cb, cbopaque,
			    "Min active:dirty page ratio per arena: N/A\n");
		}
		if (je_mallctl("arenas.tcache_max", &sv, &ssz, NULL, 0) == 0) {
			malloc_cprintf(write_cb, cbopaque,
			    "Maximum thread-cached size class: %zu\n", sv);
		}
		if (je_mallctl("opt.prof", &bv, &bsz, NULL, 0) == 0 && bv) {
			CTL_GET("prof.lg_sample", &sv, size_t);
			malloc_cprintf(write_cb, cbopaque,
			    "Average profile sample interval: %"FMTu64
			    " (2^%zu)\n", (((uint64_t)1U) << sv), sv);

			CTL_GET("opt.lg_prof_interval", &ssv, ssize_t);
			if (ssv >= 0) {
				malloc_cprintf(write_cb, cbopaque,
				    "Average profile dump interval: %"FMTu64
				    " (2^%zd)\n",
				    (((uint64_t)1U) << ssv), ssv);
			} else {
				malloc_cprintf(write_cb, cbopaque,
				    "Average profile dump interval: N/A\n");
			}
		}
		CTL_GET("opt.lg_chunk", &sv, size_t);
		malloc_cprintf(write_cb, cbopaque,
		    "Chunk size: %zu (2^%zu)\n", (ZU(1) << sv), sv);
	}

	if (config_stats) {
		size_t *cactive;
		size_t allocated, active, metadata, resident, mapped;

		CTL_GET("stats.cactive", &cactive, size_t *);
		CTL_GET("stats.allocated", &allocated, size_t);
		CTL_GET("stats.active", &active, size_t);
		CTL_GET("stats.metadata", &metadata, size_t);
		CTL_GET("stats.resident", &resident, size_t);
		CTL_GET("stats.mapped", &mapped, size_t);
		malloc_cprintf(write_cb, cbopaque,
		    "Allocated: %zu, active: %zu, metadata: %zu,"
		    " resident: %zu, mapped: %zu\n",
		    allocated, active, metadata, resident, mapped);
		malloc_cprintf(write_cb, cbopaque,
		    "Current active ceiling: %zu\n",
		    atomic_read_z(cactive));

		if (merged) {
			unsigned narenas;

			CTL_GET("arenas.narenas", &narenas, unsigned);
			{
				VARIABLE_ARRAY(bool, initialized, narenas);
				size_t isz;
				unsigned i, ninitialized;

				isz = sizeof(bool) * narenas;
				xmallctl("arenas.initialized", initialized,
				    &isz, NULL, 0);
				for (i = ninitialized = 0; i < narenas; i++) {
					if (initialized[i])
						ninitialized++;
				}

				if (ninitialized > 1 || !unmerged) {
					/* Print merged arena stats. */
					malloc_cprintf(write_cb, cbopaque,
					    "\nMerged arenas stats:\n");
					stats_arena_print(write_cb, cbopaque,
					    narenas, bins, large, huge);
				}
			}
		}

		if (unmerged) {
			unsigned narenas;

			/* Print stats for each arena. */

			CTL_GET("arenas.narenas", &narenas, unsigned);
			{
				VARIABLE_ARRAY(bool, initialized, narenas);
				size_t isz;
				unsigned i;

				isz = sizeof(bool) * narenas;
				xmallctl("arenas.initialized", initialized,
				    &isz, NULL, 0);

				for (i = 0; i < narenas; i++) {
					if (initialized[i]) {
						malloc_cprintf(write_cb,
						    cbopaque,
						    "\narenas[%u]:\n", i);
						stats_arena_print(write_cb,
						    cbopaque, i, bins, large,
						    huge);
					}
				}
			}
		}
	}
	malloc_cprintf(write_cb, cbopaque, "--- End jemalloc statistics ---\n");
}

#define	JEMALLOC_STATS_C_
#include "jemalloc/internal/jemalloc_internal.h"

#define	CTL_GET(n, v, t) do {						\
	size_t sz = sizeof(t);						\
	xmallctl(n, v, &sz, NULL, 0);					\
} while (0)

#define	CTL_I_GET(n, v, t) do {						\
	size_t mib[6];							\
	size_t miblen = sizeof(mib) / sizeof(size_t);			\
	size_t sz = sizeof(t);						\
	xmallctlnametomib(n, mib, &miblen);				\
	mib[2] = i;							\
	xmallctlbymib(mib, miblen, v, &sz, NULL, 0);			\
} while (0)

#define	CTL_J_GET(n, v, t) do {						\
	size_t mib[6];							\
	size_t miblen = sizeof(mib) / sizeof(size_t);			\
	size_t sz = sizeof(t);						\
	xmallctlnametomib(n, mib, &miblen);				\
	mib[2] = j;							\
	xmallctlbymib(mib, miblen, v, &sz, NULL, 0);			\
} while (0)

#define	CTL_IJ_GET(n, v, t) do {					\
	size_t mib[6];							\
	size_t miblen = sizeof(mib) / sizeof(size_t);			\
	size_t sz = sizeof(t);						\
	xmallctlnametomib(n, mib, &miblen);				\
	mib[2] = i;							\
	mib[4] = j;							\
	xmallctlbymib(mib, miblen, v, &sz, NULL, 0);			\
} while (0)

/******************************************************************************/
/* Data. */

bool	opt_stats_print = false;

#ifdef JEMALLOC_STATS
size_t	stats_cactive = 0;
#endif

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

#ifdef JEMALLOC_STATS
static void	malloc_vcprintf(void (*write_cb)(void *, const char *),
    void *cbopaque, const char *format, va_list ap);
static void	stats_arena_bins_print(void (*write_cb)(void *, const char *),
    void *cbopaque, unsigned i);
static void	stats_arena_lruns_print(void (*write_cb)(void *, const char *),
    void *cbopaque, unsigned i);
static void	stats_arena_print(void (*write_cb)(void *, const char *),
    void *cbopaque, unsigned i);
#endif

/******************************************************************************/

/*
 * We don't want to depend on vsnprintf() for production builds, since that can
 * cause unnecessary bloat for static binaries.  u2s() provides minimal integer
 * printing functionality, so that malloc_printf() use can be limited to
 * JEMALLOC_STATS code.
 */
char *
u2s(uint64_t x, unsigned base, char *s)
{
	unsigned i;

	i = UMAX2S_BUFSIZE - 1;
	s[i] = '\0';
	switch (base) {
	case 10:
		do {
			i--;
			s[i] = "0123456789"[x % (uint64_t)10];
			x /= (uint64_t)10;
		} while (x > 0);
		break;
	case 16:
		do {
			i--;
			s[i] = "0123456789abcdef"[x & 0xf];
			x >>= 4;
		} while (x > 0);
		break;
	default:
		do {
			i--;
			s[i] = "0123456789abcdefghijklmnopqrstuvwxyz"[x %
			    (uint64_t)base];
			x /= (uint64_t)base;
		} while (x > 0);
	}

	return (&s[i]);
}

#ifdef JEMALLOC_STATS
static void
malloc_vcprintf(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *format, va_list ap)
{
	char buf[4096];

	if (write_cb == NULL) {
		/*
		 * The caller did not provide an alternate write_cb callback
		 * function, so use the default one.  malloc_write() is an
		 * inline function, so use malloc_message() directly here.
		 */
		write_cb = JEMALLOC_P(malloc_message);
		cbopaque = NULL;
	}

	vsnprintf(buf, sizeof(buf), format, ap);
	write_cb(cbopaque, buf);
}

/*
 * Print to a callback function in such a way as to (hopefully) avoid memory
 * allocation.
 */
JEMALLOC_ATTR(format(printf, 3, 4))
void
malloc_cprintf(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	malloc_vcprintf(write_cb, cbopaque, format, ap);
	va_end(ap);
}

/*
 * Print to stderr in such a way as to (hopefully) avoid memory allocation.
 */
JEMALLOC_ATTR(format(printf, 1, 2))
void
malloc_printf(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	malloc_vcprintf(NULL, NULL, format, ap);
	va_end(ap);
}
#endif

#ifdef JEMALLOC_STATS
static void
stats_arena_bins_print(void (*write_cb)(void *, const char *), void *cbopaque,
    unsigned i)
{
	size_t pagesize;
	bool config_tcache;
	unsigned nbins, j, gap_start;

	CTL_GET("arenas.pagesize", &pagesize, size_t);

	CTL_GET("config.tcache", &config_tcache, bool);
	if (config_tcache) {
		malloc_cprintf(write_cb, cbopaque,
		    "bins:     bin    size regs pgs    allocated      nmalloc"
		    "      ndalloc    nrequests       nfills     nflushes"
		    "      newruns       reruns      maxruns      curruns\n");
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "bins:     bin    size regs pgs    allocated      nmalloc"
		    "      ndalloc      newruns       reruns      maxruns"
		    "      curruns\n");
	}
	CTL_GET("arenas.nbins", &nbins, unsigned);
	for (j = 0, gap_start = UINT_MAX; j < nbins; j++) {
		uint64_t nruns;

		CTL_IJ_GET("stats.arenas.0.bins.0.nruns", &nruns, uint64_t);
		if (nruns == 0) {
			if (gap_start == UINT_MAX)
				gap_start = j;
		} else {
			unsigned ntbins_, nqbins, ncbins, nsbins;
			size_t reg_size, run_size, allocated;
			uint32_t nregs;
			uint64_t nmalloc, ndalloc, nrequests, nfills, nflushes;
			uint64_t reruns;
			size_t highruns, curruns;

			if (gap_start != UINT_MAX) {
				if (j > gap_start + 1) {
					/* Gap of more than one size class. */
					malloc_cprintf(write_cb, cbopaque,
					    "[%u..%u]\n", gap_start,
					    j - 1);
				} else {
					/* Gap of one size class. */
					malloc_cprintf(write_cb, cbopaque,
					    "[%u]\n", gap_start);
				}
				gap_start = UINT_MAX;
			}
			CTL_GET("arenas.ntbins", &ntbins_, unsigned);
			CTL_GET("arenas.nqbins", &nqbins, unsigned);
			CTL_GET("arenas.ncbins", &ncbins, unsigned);
			CTL_GET("arenas.nsbins", &nsbins, unsigned);
			CTL_J_GET("arenas.bin.0.size", &reg_size, size_t);
			CTL_J_GET("arenas.bin.0.nregs", &nregs, uint32_t);
			CTL_J_GET("arenas.bin.0.run_size", &run_size, size_t);
			CTL_IJ_GET("stats.arenas.0.bins.0.allocated",
			    &allocated, size_t);
			CTL_IJ_GET("stats.arenas.0.bins.0.nmalloc",
			    &nmalloc, uint64_t);
			CTL_IJ_GET("stats.arenas.0.bins.0.ndalloc",
			    &ndalloc, uint64_t);
			if (config_tcache) {
				CTL_IJ_GET("stats.arenas.0.bins.0.nrequests",
				    &nrequests, uint64_t);
				CTL_IJ_GET("stats.arenas.0.bins.0.nfills",
				    &nfills, uint64_t);
				CTL_IJ_GET("stats.arenas.0.bins.0.nflushes",
				    &nflushes, uint64_t);
			}
			CTL_IJ_GET("stats.arenas.0.bins.0.nreruns", &reruns,
			    uint64_t);
			CTL_IJ_GET("stats.arenas.0.bins.0.highruns", &highruns,
			    size_t);
			CTL_IJ_GET("stats.arenas.0.bins.0.curruns", &curruns,
			    size_t);
			if (config_tcache) {
				malloc_cprintf(write_cb, cbopaque,
				    "%13u %1s %5zu %4u %3zu %12zu %12"PRIu64
				    " %12"PRIu64" %12"PRIu64" %12"PRIu64
				    " %12"PRIu64" %12"PRIu64" %12"PRIu64
				    " %12zu %12zu\n",
				    j,
				    j < ntbins_ ? "T" : j < ntbins_ + nqbins ?
				    "Q" : j < ntbins_ + nqbins + ncbins ? "C" :
				    "S",
				    reg_size, nregs, run_size / pagesize,
				    allocated, nmalloc, ndalloc, nrequests,
				    nfills, nflushes, nruns, reruns, highruns,
				    curruns);
			} else {
				malloc_cprintf(write_cb, cbopaque,
				    "%13u %1s %5zu %4u %3zu %12zu %12"PRIu64
				    " %12"PRIu64" %12"PRIu64" %12"PRIu64
				    " %12zu %12zu\n",
				    j,
				    j < ntbins_ ? "T" : j < ntbins_ + nqbins ?
				    "Q" : j < ntbins_ + nqbins + ncbins ? "C" :
				    "S",
				    reg_size, nregs, run_size / pagesize,
				    allocated, nmalloc, ndalloc, nruns, reruns,
				    highruns, curruns);
			}
		}
	}
	if (gap_start != UINT_MAX) {
		if (j > gap_start + 1) {
			/* Gap of more than one size class. */
			malloc_cprintf(write_cb, cbopaque, "[%u..%u]\n",
			    gap_start, j - 1);
		} else {
			/* Gap of one size class. */
			malloc_cprintf(write_cb, cbopaque, "[%u]\n", gap_start);
		}
	}
}

static void
stats_arena_lruns_print(void (*write_cb)(void *, const char *), void *cbopaque,
    unsigned i)
{
	size_t pagesize, nlruns, j;
	ssize_t gap_start;

	CTL_GET("arenas.pagesize", &pagesize, size_t);

	malloc_cprintf(write_cb, cbopaque,
	    "large:   size pages      nmalloc      ndalloc    nrequests"
	    "      maxruns      curruns\n");
	CTL_GET("arenas.nlruns", &nlruns, size_t);
	for (j = 0, gap_start = -1; j < nlruns; j++) {
		uint64_t nmalloc, ndalloc, nrequests;
		size_t run_size, highruns, curruns;

		CTL_IJ_GET("stats.arenas.0.lruns.0.nmalloc", &nmalloc,
		    uint64_t);
		CTL_IJ_GET("stats.arenas.0.lruns.0.ndalloc", &ndalloc,
		    uint64_t);
		CTL_IJ_GET("stats.arenas.0.lruns.0.nrequests", &nrequests,
		    uint64_t);
		if (nrequests == 0) {
			if (gap_start == -1)
				gap_start = j;
		} else {
			CTL_J_GET("arenas.lrun.0.size", &run_size, size_t);
			CTL_IJ_GET("stats.arenas.0.lruns.0.highruns", &highruns,
			    size_t);
			CTL_IJ_GET("stats.arenas.0.lruns.0.curruns", &curruns,
			    size_t);
			if (gap_start != -1) {
				malloc_cprintf(write_cb, cbopaque, "[%zu]\n",
				    j - gap_start);
				gap_start = -1;
			}
			malloc_cprintf(write_cb, cbopaque,
			    "%13zu %5zu %12"PRIu64" %12"PRIu64" %12"PRIu64
			    " %12zu %12zu\n",
			    run_size, run_size / pagesize, nmalloc, ndalloc,
			    nrequests, highruns, curruns);
		}
	}
	if (gap_start != -1)
		malloc_cprintf(write_cb, cbopaque, "[%zu]\n", j - gap_start);
}

static void
stats_arena_print(void (*write_cb)(void *, const char *), void *cbopaque,
    unsigned i)
{
	unsigned nthreads;
	size_t pagesize, pactive, pdirty, mapped;
	uint64_t npurge, nmadvise, purged;
	size_t small_allocated;
	uint64_t small_nmalloc, small_ndalloc, small_nrequests;
	size_t large_allocated;
	uint64_t large_nmalloc, large_ndalloc, large_nrequests;

	CTL_GET("arenas.pagesize", &pagesize, size_t);

	CTL_I_GET("stats.arenas.0.nthreads", &nthreads, unsigned);
	malloc_cprintf(write_cb, cbopaque,
	    "assigned threads: %u\n", nthreads);
	CTL_I_GET("stats.arenas.0.pactive", &pactive, size_t);
	CTL_I_GET("stats.arenas.0.pdirty", &pdirty, size_t);
	CTL_I_GET("stats.arenas.0.npurge", &npurge, uint64_t);
	CTL_I_GET("stats.arenas.0.nmadvise", &nmadvise, uint64_t);
	CTL_I_GET("stats.arenas.0.purged", &purged, uint64_t);
	malloc_cprintf(write_cb, cbopaque,
	    "dirty pages: %zu:%zu active:dirty, %"PRIu64" sweep%s,"
	    " %"PRIu64" madvise%s, %"PRIu64" purged\n",
	    pactive, pdirty, npurge, npurge == 1 ? "" : "s",
	    nmadvise, nmadvise == 1 ? "" : "s", purged);

	malloc_cprintf(write_cb, cbopaque,
	    "            allocated      nmalloc      ndalloc    nrequests\n");
	CTL_I_GET("stats.arenas.0.small.allocated", &small_allocated, size_t);
	CTL_I_GET("stats.arenas.0.small.nmalloc", &small_nmalloc, uint64_t);
	CTL_I_GET("stats.arenas.0.small.ndalloc", &small_ndalloc, uint64_t);
	CTL_I_GET("stats.arenas.0.small.nrequests", &small_nrequests, uint64_t);
	malloc_cprintf(write_cb, cbopaque,
	    "small:   %12zu %12"PRIu64" %12"PRIu64" %12"PRIu64"\n",
	    small_allocated, small_nmalloc, small_ndalloc, small_nrequests);
	CTL_I_GET("stats.arenas.0.large.allocated", &large_allocated, size_t);
	CTL_I_GET("stats.arenas.0.large.nmalloc", &large_nmalloc, uint64_t);
	CTL_I_GET("stats.arenas.0.large.ndalloc", &large_ndalloc, uint64_t);
	CTL_I_GET("stats.arenas.0.large.nrequests", &large_nrequests, uint64_t);
	malloc_cprintf(write_cb, cbopaque,
	    "large:   %12zu %12"PRIu64" %12"PRIu64" %12"PRIu64"\n",
	    large_allocated, large_nmalloc, large_ndalloc, large_nrequests);
	malloc_cprintf(write_cb, cbopaque,
	    "total:   %12zu %12"PRIu64" %12"PRIu64" %12"PRIu64"\n",
	    small_allocated + large_allocated,
	    small_nmalloc + large_nmalloc,
	    small_ndalloc + large_ndalloc,
	    small_nrequests + large_nrequests);
	malloc_cprintf(write_cb, cbopaque, "active:  %12zu\n",
	    pactive * pagesize );
	CTL_I_GET("stats.arenas.0.mapped", &mapped, size_t);
	malloc_cprintf(write_cb, cbopaque, "mapped:  %12zu\n", mapped);

	stats_arena_bins_print(write_cb, cbopaque, i);
	stats_arena_lruns_print(write_cb, cbopaque, i);
}
#endif

void
stats_print(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *opts)
{
	int err;
	uint64_t epoch;
	size_t u64sz;
	char s[UMAX2S_BUFSIZE];
	bool general = true;
	bool merged = true;
	bool unmerged = true;
	bool bins = true;
	bool large = true;

	/*
	 * Refresh stats, in case mallctl() was called by the application.
	 *
	 * Check for OOM here, since refreshing the ctl cache can trigger
	 * allocation.  In practice, none of the subsequent mallctl()-related
	 * calls in this function will cause OOM if this one succeeds.
	 * */
	epoch = 1;
	u64sz = sizeof(uint64_t);
	err = JEMALLOC_P(mallctl)("epoch", &epoch, &u64sz, &epoch,
	    sizeof(uint64_t));
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

	if (write_cb == NULL) {
		/*
		 * The caller did not provide an alternate write_cb callback
		 * function, so use the default one.  malloc_write() is an
		 * inline function, so use malloc_message() directly here.
		 */
		write_cb = JEMALLOC_P(malloc_message);
		cbopaque = NULL;
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
				default:;
			}
		}
	}

	write_cb(cbopaque, "___ Begin jemalloc statistics ___\n");
	if (general) {
		int err;
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
		write_cb(cbopaque, "Version: ");
		write_cb(cbopaque, cpv);
		write_cb(cbopaque, "\n");
		CTL_GET("config.debug", &bv, bool);
		write_cb(cbopaque, "Assertions ");
		write_cb(cbopaque, bv ? "enabled" : "disabled");
		write_cb(cbopaque, "\n");

#define OPT_WRITE_BOOL(n)						\
		if ((err = JEMALLOC_P(mallctl)("opt."#n, &bv, &bsz,	\
		    NULL, 0)) == 0) {					\
			write_cb(cbopaque, "  opt."#n": ");		\
			write_cb(cbopaque, bv ? "true" : "false");	\
			write_cb(cbopaque, "\n");			\
		}
#define OPT_WRITE_SIZE_T(n)						\
		if ((err = JEMALLOC_P(mallctl)("opt."#n, &sv, &ssz,	\
		    NULL, 0)) == 0) {					\
			write_cb(cbopaque, "  opt."#n": ");		\
			write_cb(cbopaque, u2s(sv, 10, s));		\
			write_cb(cbopaque, "\n");			\
		}
#define OPT_WRITE_SSIZE_T(n)						\
		if ((err = JEMALLOC_P(mallctl)("opt."#n, &ssv, &sssz,	\
		    NULL, 0)) == 0) {					\
			if (ssv >= 0) {					\
				write_cb(cbopaque, "  opt."#n": ");	\
				write_cb(cbopaque, u2s(ssv, 10, s));	\
			} else {					\
				write_cb(cbopaque, "  opt."#n": -");	\
				write_cb(cbopaque, u2s(-ssv, 10, s));	\
			}						\
			write_cb(cbopaque, "\n");			\
		}
#define OPT_WRITE_CHAR_P(n)						\
		if ((err = JEMALLOC_P(mallctl)("opt."#n, &cpv, &cpsz,	\
		    NULL, 0)) == 0) {					\
			write_cb(cbopaque, "  opt."#n": \"");		\
			write_cb(cbopaque, cpv);			\
			write_cb(cbopaque, "\"\n");			\
		}

		write_cb(cbopaque, "Run-time option settings:\n");
		OPT_WRITE_BOOL(abort)
		OPT_WRITE_SIZE_T(lg_qspace_max)
		OPT_WRITE_SIZE_T(lg_cspace_max)
		OPT_WRITE_SIZE_T(lg_chunk)
		OPT_WRITE_SIZE_T(narenas)
		OPT_WRITE_SSIZE_T(lg_dirty_mult)
		OPT_WRITE_BOOL(stats_print)
		OPT_WRITE_BOOL(junk)
		OPT_WRITE_BOOL(zero)
		OPT_WRITE_BOOL(sysv)
		OPT_WRITE_BOOL(xmalloc)
		OPT_WRITE_BOOL(tcache)
		OPT_WRITE_SSIZE_T(lg_tcache_gc_sweep)
		OPT_WRITE_SSIZE_T(lg_tcache_max)
		OPT_WRITE_BOOL(prof)
		OPT_WRITE_CHAR_P(prof_prefix)
		OPT_WRITE_SIZE_T(lg_prof_bt_max)
		OPT_WRITE_BOOL(prof_active)
		OPT_WRITE_SSIZE_T(lg_prof_sample)
		OPT_WRITE_BOOL(prof_accum)
		OPT_WRITE_SSIZE_T(lg_prof_tcmax)
		OPT_WRITE_SSIZE_T(lg_prof_interval)
		OPT_WRITE_BOOL(prof_gdump)
		OPT_WRITE_BOOL(prof_leak)
		OPT_WRITE_BOOL(overcommit)

#undef OPT_WRITE_BOOL
#undef OPT_WRITE_SIZE_T
#undef OPT_WRITE_SSIZE_T
#undef OPT_WRITE_CHAR_P

		write_cb(cbopaque, "CPUs: ");
		write_cb(cbopaque, u2s(ncpus, 10, s));
		write_cb(cbopaque, "\n");

		CTL_GET("arenas.narenas", &uv, unsigned);
		write_cb(cbopaque, "Max arenas: ");
		write_cb(cbopaque, u2s(uv, 10, s));
		write_cb(cbopaque, "\n");

		write_cb(cbopaque, "Pointer size: ");
		write_cb(cbopaque, u2s(sizeof(void *), 10, s));
		write_cb(cbopaque, "\n");

		CTL_GET("arenas.quantum", &sv, size_t);
		write_cb(cbopaque, "Quantum size: ");
		write_cb(cbopaque, u2s(sv, 10, s));
		write_cb(cbopaque, "\n");

		CTL_GET("arenas.cacheline", &sv, size_t);
		write_cb(cbopaque, "Cacheline size (assumed): ");
		write_cb(cbopaque, u2s(sv, 10, s));
		write_cb(cbopaque, "\n");

		CTL_GET("arenas.subpage", &sv, size_t);
		write_cb(cbopaque, "Subpage spacing: ");
		write_cb(cbopaque, u2s(sv, 10, s));
		write_cb(cbopaque, "\n");

		if ((err = JEMALLOC_P(mallctl)("arenas.tspace_min", &sv, &ssz,
		    NULL, 0)) == 0) {
			write_cb(cbopaque, "Tiny 2^n-spaced sizes: [");
			write_cb(cbopaque, u2s(sv, 10, s));
			write_cb(cbopaque, "..");

			CTL_GET("arenas.tspace_max", &sv, size_t);
			write_cb(cbopaque, u2s(sv, 10, s));
			write_cb(cbopaque, "]\n");
		}

		CTL_GET("arenas.qspace_min", &sv, size_t);
		write_cb(cbopaque, "Quantum-spaced sizes: [");
		write_cb(cbopaque, u2s(sv, 10, s));
		write_cb(cbopaque, "..");
		CTL_GET("arenas.qspace_max", &sv, size_t);
		write_cb(cbopaque, u2s(sv, 10, s));
		write_cb(cbopaque, "]\n");

		CTL_GET("arenas.cspace_min", &sv, size_t);
		write_cb(cbopaque, "Cacheline-spaced sizes: [");
		write_cb(cbopaque, u2s(sv, 10, s));
		write_cb(cbopaque, "..");
		CTL_GET("arenas.cspace_max", &sv, size_t);
		write_cb(cbopaque, u2s(sv, 10, s));
		write_cb(cbopaque, "]\n");

		CTL_GET("arenas.sspace_min", &sv, size_t);
		write_cb(cbopaque, "Subpage-spaced sizes: [");
		write_cb(cbopaque, u2s(sv, 10, s));
		write_cb(cbopaque, "..");
		CTL_GET("arenas.sspace_max", &sv, size_t);
		write_cb(cbopaque, u2s(sv, 10, s));
		write_cb(cbopaque, "]\n");

		CTL_GET("opt.lg_dirty_mult", &ssv, ssize_t);
		if (ssv >= 0) {
			write_cb(cbopaque,
			    "Min active:dirty page ratio per arena: ");
			write_cb(cbopaque, u2s((1U << ssv), 10, s));
			write_cb(cbopaque, ":1\n");
		} else {
			write_cb(cbopaque,
			    "Min active:dirty page ratio per arena: N/A\n");
		}
		if ((err = JEMALLOC_P(mallctl)("arenas.tcache_max", &sv,
		    &ssz, NULL, 0)) == 0) {
			write_cb(cbopaque,
			    "Maximum thread-cached size class: ");
			write_cb(cbopaque, u2s(sv, 10, s));
			write_cb(cbopaque, "\n");
		}
		if ((err = JEMALLOC_P(mallctl)("opt.lg_tcache_gc_sweep", &ssv,
		    &ssz, NULL, 0)) == 0) {
			size_t tcache_gc_sweep = (1U << ssv);
			bool tcache_enabled;
			CTL_GET("opt.tcache", &tcache_enabled, bool);
			write_cb(cbopaque, "Thread cache GC sweep interval: ");
			write_cb(cbopaque, tcache_enabled && ssv >= 0 ?
			    u2s(tcache_gc_sweep, 10, s) : "N/A");
			write_cb(cbopaque, "\n");
		}
		if ((err = JEMALLOC_P(mallctl)("opt.prof", &bv, &bsz, NULL, 0))
		   == 0 && bv) {
			CTL_GET("opt.lg_prof_bt_max", &sv, size_t);
			write_cb(cbopaque, "Maximum profile backtrace depth: ");
			write_cb(cbopaque, u2s((1U << sv), 10, s));
			write_cb(cbopaque, "\n");

			CTL_GET("opt.lg_prof_tcmax", &ssv, ssize_t);
			write_cb(cbopaque,
			    "Maximum per thread backtrace cache: ");
			if (ssv >= 0) {
				write_cb(cbopaque, u2s((1U << ssv), 10, s));
				write_cb(cbopaque, " (2^");
				write_cb(cbopaque, u2s(ssv, 10, s));
				write_cb(cbopaque, ")\n");
			} else
				write_cb(cbopaque, "N/A\n");

			CTL_GET("opt.lg_prof_sample", &sv, size_t);
			write_cb(cbopaque, "Average profile sample interval: ");
			write_cb(cbopaque, u2s((((uint64_t)1U) << sv), 10, s));
			write_cb(cbopaque, " (2^");
			write_cb(cbopaque, u2s(sv, 10, s));
			write_cb(cbopaque, ")\n");

			CTL_GET("opt.lg_prof_interval", &ssv, ssize_t);
			write_cb(cbopaque, "Average profile dump interval: ");
			if (ssv >= 0) {
				write_cb(cbopaque, u2s((((uint64_t)1U) << ssv),
				    10, s));
				write_cb(cbopaque, " (2^");
				write_cb(cbopaque, u2s(ssv, 10, s));
				write_cb(cbopaque, ")\n");
			} else
				write_cb(cbopaque, "N/A\n");
		}
		CTL_GET("arenas.chunksize", &sv, size_t);
		write_cb(cbopaque, "Chunk size: ");
		write_cb(cbopaque, u2s(sv, 10, s));
		CTL_GET("opt.lg_chunk", &sv, size_t);
		write_cb(cbopaque, " (2^");
		write_cb(cbopaque, u2s(sv, 10, s));
		write_cb(cbopaque, ")\n");
	}

#ifdef JEMALLOC_STATS
	{
		int err;
		size_t sszp, ssz;
		size_t *cactive;
		size_t allocated, active, mapped;
		size_t chunks_current, chunks_high, swap_avail;
		uint64_t chunks_total;
		size_t huge_allocated;
		uint64_t huge_nmalloc, huge_ndalloc;

		sszp = sizeof(size_t *);
		ssz = sizeof(size_t);

		CTL_GET("stats.cactive", &cactive, size_t *);
		CTL_GET("stats.allocated", &allocated, size_t);
		CTL_GET("stats.active", &active, size_t);
		CTL_GET("stats.mapped", &mapped, size_t);
		malloc_cprintf(write_cb, cbopaque,
		    "Allocated: %zu, active: %zu, mapped: %zu\n",
		    allocated, active, mapped);
		malloc_cprintf(write_cb, cbopaque,
		    "Current active ceiling: %zu\n", atomic_read_z(cactive));

		/* Print chunk stats. */
		CTL_GET("stats.chunks.total", &chunks_total, uint64_t);
		CTL_GET("stats.chunks.high", &chunks_high, size_t);
		CTL_GET("stats.chunks.current", &chunks_current, size_t);
		if ((err = JEMALLOC_P(mallctl)("swap.avail", &swap_avail, &ssz,
		    NULL, 0)) == 0) {
			size_t lg_chunk;

			malloc_cprintf(write_cb, cbopaque, "chunks: nchunks   "
			    "highchunks    curchunks   swap_avail\n");
			CTL_GET("opt.lg_chunk", &lg_chunk, size_t);
			malloc_cprintf(write_cb, cbopaque,
			    "  %13"PRIu64"%13zu%13zu%13zu\n",
			    chunks_total, chunks_high, chunks_current,
			    swap_avail << lg_chunk);
		} else {
			malloc_cprintf(write_cb, cbopaque, "chunks: nchunks   "
			    "highchunks    curchunks\n");
			malloc_cprintf(write_cb, cbopaque,
			    "  %13"PRIu64"%13zu%13zu\n",
			    chunks_total, chunks_high, chunks_current);
		}

		/* Print huge stats. */
		CTL_GET("stats.huge.nmalloc", &huge_nmalloc, uint64_t);
		CTL_GET("stats.huge.ndalloc", &huge_ndalloc, uint64_t);
		CTL_GET("stats.huge.allocated", &huge_allocated, size_t);
		malloc_cprintf(write_cb, cbopaque,
		    "huge: nmalloc      ndalloc    allocated\n");
		malloc_cprintf(write_cb, cbopaque,
		    " %12"PRIu64" %12"PRIu64" %12zu\n",
		    huge_nmalloc, huge_ndalloc, huge_allocated);

		if (merged) {
			unsigned narenas;

			CTL_GET("arenas.narenas", &narenas, unsigned);
			{
				bool initialized[narenas];
				size_t isz;
				unsigned i, ninitialized;

				isz = sizeof(initialized);
				xmallctl("arenas.initialized", initialized,
				    &isz, NULL, 0);
				for (i = ninitialized = 0; i < narenas; i++) {
					if (initialized[i])
						ninitialized++;
				}

				if (ninitialized > 1) {
					/* Print merged arena stats. */
					malloc_cprintf(write_cb, cbopaque,
					    "\nMerged arenas stats:\n");
					stats_arena_print(write_cb, cbopaque,
					    narenas);
				}
			}
		}

		if (unmerged) {
			unsigned narenas;

			/* Print stats for each arena. */

			CTL_GET("arenas.narenas", &narenas, unsigned);
			{
				bool initialized[narenas];
				size_t isz;
				unsigned i;

				isz = sizeof(initialized);
				xmallctl("arenas.initialized", initialized,
				    &isz, NULL, 0);

				for (i = 0; i < narenas; i++) {
					if (initialized[i]) {
						malloc_cprintf(write_cb,
						    cbopaque,
						    "\narenas[%u]:\n", i);
						stats_arena_print(write_cb,
						    cbopaque, i);
					}
				}
			}
		}
	}
#endif /* #ifdef JEMALLOC_STATS */
	write_cb(cbopaque, "--- End jemalloc statistics ---\n");
}

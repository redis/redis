#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/ckh.h"
#include "jemalloc/internal/hash.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/prof_data.h"

/*
 * This file defines and manages the core profiling data structures.
 *
 * Conceptually, profiling data can be imagined as a table with three columns:
 * thread, stack trace, and current allocation size.  (When prof_accum is on,
 * there's one additional column which is the cumulative allocation size.)
 *
 * Implementation wise, each thread maintains a hash recording the stack trace
 * to allocation size correspondences, which are basically the individual rows
 * in the table.  In addition, two global "indices" are built to make data
 * aggregation efficient (for dumping): bt2gctx and tdatas, which are basically
 * the "grouped by stack trace" and "grouped by thread" views of the same table,
 * respectively.  Note that the allocation size is only aggregated to the two
 * indices at dumping time, so as to optimize for performance.
 */

/******************************************************************************/

malloc_mutex_t bt2gctx_mtx;
malloc_mutex_t tdatas_mtx;
malloc_mutex_t prof_dump_mtx;

/*
 * Table of mutexes that are shared among gctx's.  These are leaf locks, so
 * there is no problem with using them for more than one gctx at the same time.
 * The primary motivation for this sharing though is that gctx's are ephemeral,
 * and destroying mutexes causes complications for systems that allocate when
 * creating/destroying mutexes.
 */
malloc_mutex_t *gctx_locks;
static atomic_u_t cum_gctxs; /* Atomic counter. */

/*
 * Table of mutexes that are shared among tdata's.  No operations require
 * holding multiple tdata locks, so there is no problem with using them for more
 * than one tdata at the same time, even though a gctx lock may be acquired
 * while holding a tdata lock.
 */
malloc_mutex_t *tdata_locks;

/*
 * Global hash of (prof_bt_t *)-->(prof_gctx_t *).  This is the master data
 * structure that knows about all backtraces currently captured.
 */
static ckh_t bt2gctx;

/*
 * Tree of all extant prof_tdata_t structures, regardless of state,
 * {attached,detached,expired}.
 */
static prof_tdata_tree_t tdatas;

size_t prof_unbiased_sz[PROF_SC_NSIZES];
size_t prof_shifted_unbiased_cnt[PROF_SC_NSIZES];

/******************************************************************************/
/* Red-black trees. */

static int
prof_tctx_comp(const prof_tctx_t *a, const prof_tctx_t *b) {
	uint64_t a_thr_uid = a->thr_uid;
	uint64_t b_thr_uid = b->thr_uid;
	int ret = (a_thr_uid > b_thr_uid) - (a_thr_uid < b_thr_uid);
	if (ret == 0) {
		uint64_t a_thr_discrim = a->thr_discrim;
		uint64_t b_thr_discrim = b->thr_discrim;
		ret = (a_thr_discrim > b_thr_discrim) - (a_thr_discrim <
		    b_thr_discrim);
		if (ret == 0) {
			uint64_t a_tctx_uid = a->tctx_uid;
			uint64_t b_tctx_uid = b->tctx_uid;
			ret = (a_tctx_uid > b_tctx_uid) - (a_tctx_uid <
			    b_tctx_uid);
		}
	}
	return ret;
}

rb_gen(static UNUSED, tctx_tree_, prof_tctx_tree_t, prof_tctx_t,
    tctx_link, prof_tctx_comp)

static int
prof_gctx_comp(const prof_gctx_t *a, const prof_gctx_t *b) {
	unsigned a_len = a->bt.len;
	unsigned b_len = b->bt.len;
	unsigned comp_len = (a_len < b_len) ? a_len : b_len;
	int ret = memcmp(a->bt.vec, b->bt.vec, comp_len * sizeof(void *));
	if (ret == 0) {
		ret = (a_len > b_len) - (a_len < b_len);
	}
	return ret;
}

rb_gen(static UNUSED, gctx_tree_, prof_gctx_tree_t, prof_gctx_t, dump_link,
    prof_gctx_comp)

static int
prof_tdata_comp(const prof_tdata_t *a, const prof_tdata_t *b) {
	int ret;
	uint64_t a_uid = a->thr_uid;
	uint64_t b_uid = b->thr_uid;

	ret = ((a_uid > b_uid) - (a_uid < b_uid));
	if (ret == 0) {
		uint64_t a_discrim = a->thr_discrim;
		uint64_t b_discrim = b->thr_discrim;

		ret = ((a_discrim > b_discrim) - (a_discrim < b_discrim));
	}
	return ret;
}

rb_gen(static UNUSED, tdata_tree_, prof_tdata_tree_t, prof_tdata_t, tdata_link,
    prof_tdata_comp)

/******************************************************************************/

static malloc_mutex_t *
prof_gctx_mutex_choose(void) {
	unsigned ngctxs = atomic_fetch_add_u(&cum_gctxs, 1, ATOMIC_RELAXED);

	return &gctx_locks[(ngctxs - 1) % PROF_NCTX_LOCKS];
}

static malloc_mutex_t *
prof_tdata_mutex_choose(uint64_t thr_uid) {
	return &tdata_locks[thr_uid % PROF_NTDATA_LOCKS];
}

bool
prof_data_init(tsd_t *tsd) {
	tdata_tree_new(&tdatas);
	return ckh_new(tsd, &bt2gctx, PROF_CKH_MINITEMS,
	    prof_bt_hash, prof_bt_keycomp);
}

static void
prof_enter(tsd_t *tsd, prof_tdata_t *tdata) {
	cassert(config_prof);
	assert(tdata == prof_tdata_get(tsd, false));

	if (tdata != NULL) {
		assert(!tdata->enq);
		tdata->enq = true;
	}

	malloc_mutex_lock(tsd_tsdn(tsd), &bt2gctx_mtx);
}

static void
prof_leave(tsd_t *tsd, prof_tdata_t *tdata) {
	cassert(config_prof);
	assert(tdata == prof_tdata_get(tsd, false));

	malloc_mutex_unlock(tsd_tsdn(tsd), &bt2gctx_mtx);

	if (tdata != NULL) {
		bool idump, gdump;

		assert(tdata->enq);
		tdata->enq = false;
		idump = tdata->enq_idump;
		tdata->enq_idump = false;
		gdump = tdata->enq_gdump;
		tdata->enq_gdump = false;

		if (idump) {
			prof_idump(tsd_tsdn(tsd));
		}
		if (gdump) {
			prof_gdump(tsd_tsdn(tsd));
		}
	}
}

static prof_gctx_t *
prof_gctx_create(tsdn_t *tsdn, prof_bt_t *bt) {
	/*
	 * Create a single allocation that has space for vec of length bt->len.
	 */
	size_t size = offsetof(prof_gctx_t, vec) + (bt->len * sizeof(void *));
	prof_gctx_t *gctx = (prof_gctx_t *)iallocztm(tsdn, size,
	    sz_size2index(size), false, NULL, true, arena_get(TSDN_NULL, 0, true),
	    true);
	if (gctx == NULL) {
		return NULL;
	}
	gctx->lock = prof_gctx_mutex_choose();
	/*
	 * Set nlimbo to 1, in order to avoid a race condition with
	 * prof_tctx_destroy()/prof_gctx_try_destroy().
	 */
	gctx->nlimbo = 1;
	tctx_tree_new(&gctx->tctxs);
	/* Duplicate bt. */
	memcpy(gctx->vec, bt->vec, bt->len * sizeof(void *));
	gctx->bt.vec = gctx->vec;
	gctx->bt.len = bt->len;
	return gctx;
}

static void
prof_gctx_try_destroy(tsd_t *tsd, prof_tdata_t *tdata_self,
    prof_gctx_t *gctx) {
	cassert(config_prof);

	/*
	 * Check that gctx is still unused by any thread cache before destroying
	 * it.  prof_lookup() increments gctx->nlimbo in order to avoid a race
	 * condition with this function, as does prof_tctx_destroy() in order to
	 * avoid a race between the main body of prof_tctx_destroy() and entry
	 * into this function.
	 */
	prof_enter(tsd, tdata_self);
	malloc_mutex_lock(tsd_tsdn(tsd), gctx->lock);
	assert(gctx->nlimbo != 0);
	if (tctx_tree_empty(&gctx->tctxs) && gctx->nlimbo == 1) {
		/* Remove gctx from bt2gctx. */
		if (ckh_remove(tsd, &bt2gctx, &gctx->bt, NULL, NULL)) {
			not_reached();
		}
		prof_leave(tsd, tdata_self);
		/* Destroy gctx. */
		malloc_mutex_unlock(tsd_tsdn(tsd), gctx->lock);
		idalloctm(tsd_tsdn(tsd), gctx, NULL, NULL, true, true);
	} else {
		/*
		 * Compensate for increment in prof_tctx_destroy() or
		 * prof_lookup().
		 */
		gctx->nlimbo--;
		malloc_mutex_unlock(tsd_tsdn(tsd), gctx->lock);
		prof_leave(tsd, tdata_self);
	}
}

static bool
prof_gctx_should_destroy(prof_gctx_t *gctx) {
	if (opt_prof_accum) {
		return false;
	}
	if (!tctx_tree_empty(&gctx->tctxs)) {
		return false;
	}
	if (gctx->nlimbo != 0) {
		return false;
	}
	return true;
}

static bool
prof_lookup_global(tsd_t *tsd, prof_bt_t *bt, prof_tdata_t *tdata,
    void **p_btkey, prof_gctx_t **p_gctx, bool *p_new_gctx) {
	union {
		prof_gctx_t	*p;
		void		*v;
	} gctx, tgctx;
	union {
		prof_bt_t	*p;
		void		*v;
	} btkey;
	bool new_gctx;

	prof_enter(tsd, tdata);
	if (ckh_search(&bt2gctx, bt, &btkey.v, &gctx.v)) {
		/* bt has never been seen before.  Insert it. */
		prof_leave(tsd, tdata);
		tgctx.p = prof_gctx_create(tsd_tsdn(tsd), bt);
		if (tgctx.v == NULL) {
			return true;
		}
		prof_enter(tsd, tdata);
		if (ckh_search(&bt2gctx, bt, &btkey.v, &gctx.v)) {
			gctx.p = tgctx.p;
			btkey.p = &gctx.p->bt;
			if (ckh_insert(tsd, &bt2gctx, btkey.v, gctx.v)) {
				/* OOM. */
				prof_leave(tsd, tdata);
				idalloctm(tsd_tsdn(tsd), gctx.v, NULL, NULL,
				    true, true);
				return true;
			}
			new_gctx = true;
		} else {
			new_gctx = false;
		}
	} else {
		tgctx.v = NULL;
		new_gctx = false;
	}

	if (!new_gctx) {
		/*
		 * Increment nlimbo, in order to avoid a race condition with
		 * prof_tctx_destroy()/prof_gctx_try_destroy().
		 */
		malloc_mutex_lock(tsd_tsdn(tsd), gctx.p->lock);
		gctx.p->nlimbo++;
		malloc_mutex_unlock(tsd_tsdn(tsd), gctx.p->lock);
		new_gctx = false;

		if (tgctx.v != NULL) {
			/* Lost race to insert. */
			idalloctm(tsd_tsdn(tsd), tgctx.v, NULL, NULL, true,
			    true);
		}
	}
	prof_leave(tsd, tdata);

	*p_btkey = btkey.v;
	*p_gctx = gctx.p;
	*p_new_gctx = new_gctx;
	return false;
}

prof_tctx_t *
prof_lookup(tsd_t *tsd, prof_bt_t *bt) {
	union {
		prof_tctx_t	*p;
		void		*v;
	} ret;
	prof_tdata_t *tdata;
	bool not_found;

	cassert(config_prof);

	tdata = prof_tdata_get(tsd, false);
	assert(tdata != NULL);

	malloc_mutex_lock(tsd_tsdn(tsd), tdata->lock);
	not_found = ckh_search(&tdata->bt2tctx, bt, NULL, &ret.v);
	if (!not_found) { /* Note double negative! */
		ret.p->prepared = true;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), tdata->lock);
	if (not_found) {
		void *btkey;
		prof_gctx_t *gctx;
		bool new_gctx, error;

		/*
		 * This thread's cache lacks bt.  Look for it in the global
		 * cache.
		 */
		if (prof_lookup_global(tsd, bt, tdata, &btkey, &gctx,
		    &new_gctx)) {
			return NULL;
		}

		/* Link a prof_tctx_t into gctx for this thread. */
		ret.v = iallocztm(tsd_tsdn(tsd), sizeof(prof_tctx_t),
		    sz_size2index(sizeof(prof_tctx_t)), false, NULL, true,
		    arena_ichoose(tsd, NULL), true);
		if (ret.p == NULL) {
			if (new_gctx) {
				prof_gctx_try_destroy(tsd, tdata, gctx);
			}
			return NULL;
		}
		ret.p->tdata = tdata;
		ret.p->thr_uid = tdata->thr_uid;
		ret.p->thr_discrim = tdata->thr_discrim;
		ret.p->recent_count = 0;
		memset(&ret.p->cnts, 0, sizeof(prof_cnt_t));
		ret.p->gctx = gctx;
		ret.p->tctx_uid = tdata->tctx_uid_next++;
		ret.p->prepared = true;
		ret.p->state = prof_tctx_state_initializing;
		malloc_mutex_lock(tsd_tsdn(tsd), tdata->lock);
		error = ckh_insert(tsd, &tdata->bt2tctx, btkey, ret.v);
		malloc_mutex_unlock(tsd_tsdn(tsd), tdata->lock);
		if (error) {
			if (new_gctx) {
				prof_gctx_try_destroy(tsd, tdata, gctx);
			}
			idalloctm(tsd_tsdn(tsd), ret.v, NULL, NULL, true, true);
			return NULL;
		}
		malloc_mutex_lock(tsd_tsdn(tsd), gctx->lock);
		ret.p->state = prof_tctx_state_nominal;
		tctx_tree_insert(&gctx->tctxs, ret.p);
		gctx->nlimbo--;
		malloc_mutex_unlock(tsd_tsdn(tsd), gctx->lock);
	}

	return ret.p;
}

/* Used in unit tests. */
static prof_tdata_t *
prof_tdata_count_iter(prof_tdata_tree_t *tdatas_ptr, prof_tdata_t *tdata,
    void *arg) {
	size_t *tdata_count = (size_t *)arg;

	(*tdata_count)++;

	return NULL;
}

/* Used in unit tests. */
size_t
prof_tdata_count(void) {
	size_t tdata_count = 0;
	tsdn_t *tsdn;

	tsdn = tsdn_fetch();
	malloc_mutex_lock(tsdn, &tdatas_mtx);
	tdata_tree_iter(&tdatas, NULL, prof_tdata_count_iter,
	    (void *)&tdata_count);
	malloc_mutex_unlock(tsdn, &tdatas_mtx);

	return tdata_count;
}

/* Used in unit tests. */
size_t
prof_bt_count(void) {
	size_t bt_count;
	tsd_t *tsd;
	prof_tdata_t *tdata;

	tsd = tsd_fetch();
	tdata = prof_tdata_get(tsd, false);
	if (tdata == NULL) {
		return 0;
	}

	malloc_mutex_lock(tsd_tsdn(tsd), &bt2gctx_mtx);
	bt_count = ckh_count(&bt2gctx);
	malloc_mutex_unlock(tsd_tsdn(tsd), &bt2gctx_mtx);

	return bt_count;
}

char *
prof_thread_name_alloc(tsd_t *tsd, const char *thread_name) {
	char *ret;
	size_t size;

	if (thread_name == NULL) {
		return NULL;
	}

	size = strlen(thread_name) + 1;
	if (size == 1) {
		return "";
	}

	ret = iallocztm(tsd_tsdn(tsd), size, sz_size2index(size), false, NULL,
	    true, arena_get(TSDN_NULL, 0, true), true);
	if (ret == NULL) {
		return NULL;
	}
	memcpy(ret, thread_name, size);
	return ret;
}

int
prof_thread_name_set_impl(tsd_t *tsd, const char *thread_name) {
	assert(tsd_reentrancy_level_get(tsd) == 0);

	prof_tdata_t *tdata;
	unsigned i;
	char *s;

	tdata = prof_tdata_get(tsd, true);
	if (tdata == NULL) {
		return EAGAIN;
	}

	/* Validate input. */
	if (thread_name == NULL) {
		return EFAULT;
	}
	for (i = 0; thread_name[i] != '\0'; i++) {
		char c = thread_name[i];
		if (!isgraph(c) && !isblank(c)) {
			return EFAULT;
		}
	}

	s = prof_thread_name_alloc(tsd, thread_name);
	if (s == NULL) {
		return EAGAIN;
	}

	if (tdata->thread_name != NULL) {
		idalloctm(tsd_tsdn(tsd), tdata->thread_name, NULL, NULL, true,
		    true);
		tdata->thread_name = NULL;
	}
	if (strlen(s) > 0) {
		tdata->thread_name = s;
	}
	return 0;
}

JEMALLOC_FORMAT_PRINTF(3, 4)
static void
prof_dump_printf(write_cb_t *prof_dump_write, void *cbopaque,
    const char *format, ...) {
	va_list ap;
	char buf[PROF_PRINTF_BUFSIZE];

	va_start(ap, format);
	malloc_vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
	prof_dump_write(cbopaque, buf);
}

/*
 * Casting a double to a uint64_t may not necessarily be in range; this can be
 * UB.  I don't think this is practically possible with the cur counters, but
 * plausibly could be with the accum counters.
 */
#ifdef JEMALLOC_PROF
static uint64_t
prof_double_uint64_cast(double d) {
	/*
	 * Note: UINT64_MAX + 1 is exactly representable as a double on all
	 * reasonable platforms (certainly those we'll support).  Writing this
	 * as !(a < b) instead of (a >= b) means that we're NaN-safe.
	 */
	double rounded = round(d);
	if (!(rounded < (double)UINT64_MAX)) {
		return UINT64_MAX;
	}
	return (uint64_t)rounded;
}
#endif

void prof_unbias_map_init() {
	/* See the comment in prof_sample_new_event_wait */
#ifdef JEMALLOC_PROF
	for (szind_t i = 0; i < SC_NSIZES; i++) {
		double sz = (double)sz_index2size(i);
		double rate = (double)(ZU(1) << lg_prof_sample);
		double div_val = 1.0 - exp(-sz / rate);
		double unbiased_sz = sz / div_val;
		/*
		 * The "true" right value for the unbiased count is
		 * 1.0/(1 - exp(-sz/rate)).  The problem is, we keep the counts
		 * as integers (for a variety of reasons -- rounding errors
		 * could trigger asserts, and not all libcs can properly handle
		 * floating point arithmetic during malloc calls inside libc).
		 * Rounding to an integer, though, can lead to rounding errors
		 * of over 30% for sizes close to the sampling rate.  So
		 * instead, we multiply by a constant, dividing the maximum
		 * possible roundoff error by that constant.  To avoid overflow
		 * in summing up size_t values, the largest safe constant we can
		 * pick is the size of the smallest allocation.
		 */
		double cnt_shift = (double)(ZU(1) << SC_LG_TINY_MIN);
		double shifted_unbiased_cnt = cnt_shift / div_val;
		prof_unbiased_sz[i] = (size_t)round(unbiased_sz);
		prof_shifted_unbiased_cnt[i] = (size_t)round(
		    shifted_unbiased_cnt);
	}
#else
	unreachable();
#endif
}

/*
 * The unbiasing story is long.  The jeprof unbiasing logic was copied from
 * pprof.  Both shared an issue: they unbiased using the average size of the
 * allocations at a particular stack trace.  This can work out OK if allocations
 * are mostly of the same size given some stack, but not otherwise.  We now
 * internally track what the unbiased results ought to be.  We can't just report
 * them as they are though; they'll still go through the jeprof unbiasing
 * process.  Instead, we figure out what values we can feed *into* jeprof's
 * unbiasing mechanism that will lead to getting the right values out.
 *
 * It'll unbias count and aggregate size as:
 *
 *   c_out = c_in * 1/(1-exp(-s_in/c_in/R)
 *   s_out = s_in * 1/(1-exp(-s_in/c_in/R)
 *
 * We want to solve for the values of c_in and s_in that will
 * give the c_out and s_out that we've computed internally.
 *
 * Let's do a change of variables (both to make the math easier and to make it
 * easier to write):
 *   x = s_in / c_in
 *   y = s_in
 *   k = 1/R.
 *
 * Then
 *   c_out = y/x * 1/(1-exp(-k*x))
 *   s_out = y * 1/(1-exp(-k*x))
 *
 * The first equation gives:
 *   y = x * c_out * (1-exp(-k*x))
 * The second gives:
 *   y = s_out * (1-exp(-k*x))
 * So we have
 *   x = s_out / c_out.
 * And all the other values fall out from that.
 *
 * This is all a fair bit of work.  The thing we get out of it is that we don't
 * break backwards compatibility with jeprof (and the various tools that have
 * copied its unbiasing logic).  Eventually, we anticipate a v3 heap profile
 * dump format based on JSON, at which point I think much of this logic can get
 * cleaned up (since we'll be taking a compatibility break there anyways).
 */
static void
prof_do_unbias(uint64_t c_out_shifted_i, uint64_t s_out_i, uint64_t *r_c_in,
    uint64_t *r_s_in) {
#ifdef JEMALLOC_PROF
	if (c_out_shifted_i == 0 || s_out_i == 0) {
		*r_c_in = 0;
		*r_s_in = 0;
		return;
	}
	/*
	 * See the note in prof_unbias_map_init() to see why we take c_out in a
	 * shifted form.
	 */
	double c_out = (double)c_out_shifted_i
	    / (double)(ZU(1) << SC_LG_TINY_MIN);
	double s_out = (double)s_out_i;
	double R = (double)(ZU(1) << lg_prof_sample);

	double x = s_out / c_out;
	double y = s_out * (1.0 - exp(-x / R));

	double c_in = y / x;
	double s_in = y;

	*r_c_in = prof_double_uint64_cast(c_in);
	*r_s_in = prof_double_uint64_cast(s_in);
#else
	unreachable();
#endif
}

static void
prof_dump_print_cnts(write_cb_t *prof_dump_write, void *cbopaque,
    const prof_cnt_t *cnts) {
	uint64_t curobjs;
	uint64_t curbytes;
	uint64_t accumobjs;
	uint64_t accumbytes;
	if (opt_prof_unbias) {
		prof_do_unbias(cnts->curobjs_shifted_unbiased,
		    cnts->curbytes_unbiased, &curobjs, &curbytes);
		prof_do_unbias(cnts->accumobjs_shifted_unbiased,
		    cnts->accumbytes_unbiased, &accumobjs, &accumbytes);
	} else {
		curobjs = cnts->curobjs;
		curbytes = cnts->curbytes;
		accumobjs = cnts->accumobjs;
		accumbytes = cnts->accumbytes;
	}
	prof_dump_printf(prof_dump_write, cbopaque,
	    "%"FMTu64": %"FMTu64" [%"FMTu64": %"FMTu64"]",
	    curobjs, curbytes, accumobjs, accumbytes);
}

static void
prof_tctx_merge_tdata(tsdn_t *tsdn, prof_tctx_t *tctx, prof_tdata_t *tdata) {
	malloc_mutex_assert_owner(tsdn, tctx->tdata->lock);

	malloc_mutex_lock(tsdn, tctx->gctx->lock);

	switch (tctx->state) {
	case prof_tctx_state_initializing:
		malloc_mutex_unlock(tsdn, tctx->gctx->lock);
		return;
	case prof_tctx_state_nominal:
		tctx->state = prof_tctx_state_dumping;
		malloc_mutex_unlock(tsdn, tctx->gctx->lock);

		memcpy(&tctx->dump_cnts, &tctx->cnts, sizeof(prof_cnt_t));

		tdata->cnt_summed.curobjs += tctx->dump_cnts.curobjs;
		tdata->cnt_summed.curobjs_shifted_unbiased
		    += tctx->dump_cnts.curobjs_shifted_unbiased;
		tdata->cnt_summed.curbytes += tctx->dump_cnts.curbytes;
		tdata->cnt_summed.curbytes_unbiased
		    += tctx->dump_cnts.curbytes_unbiased;
		if (opt_prof_accum) {
			tdata->cnt_summed.accumobjs +=
			    tctx->dump_cnts.accumobjs;
			tdata->cnt_summed.accumobjs_shifted_unbiased +=
			    tctx->dump_cnts.accumobjs_shifted_unbiased;
			tdata->cnt_summed.accumbytes +=
			    tctx->dump_cnts.accumbytes;
			tdata->cnt_summed.accumbytes_unbiased +=
			    tctx->dump_cnts.accumbytes_unbiased;
		}
		break;
	case prof_tctx_state_dumping:
	case prof_tctx_state_purgatory:
		not_reached();
	}
}

static void
prof_tctx_merge_gctx(tsdn_t *tsdn, prof_tctx_t *tctx, prof_gctx_t *gctx) {
	malloc_mutex_assert_owner(tsdn, gctx->lock);

	gctx->cnt_summed.curobjs += tctx->dump_cnts.curobjs;
	gctx->cnt_summed.curobjs_shifted_unbiased
	    += tctx->dump_cnts.curobjs_shifted_unbiased;
	gctx->cnt_summed.curbytes += tctx->dump_cnts.curbytes;
	gctx->cnt_summed.curbytes_unbiased += tctx->dump_cnts.curbytes_unbiased;
	if (opt_prof_accum) {
		gctx->cnt_summed.accumobjs += tctx->dump_cnts.accumobjs;
		gctx->cnt_summed.accumobjs_shifted_unbiased
		    += tctx->dump_cnts.accumobjs_shifted_unbiased;
		gctx->cnt_summed.accumbytes += tctx->dump_cnts.accumbytes;
		gctx->cnt_summed.accumbytes_unbiased
		    += tctx->dump_cnts.accumbytes_unbiased;
	}
}

static prof_tctx_t *
prof_tctx_merge_iter(prof_tctx_tree_t *tctxs, prof_tctx_t *tctx, void *arg) {
	tsdn_t *tsdn = (tsdn_t *)arg;

	malloc_mutex_assert_owner(tsdn, tctx->gctx->lock);

	switch (tctx->state) {
	case prof_tctx_state_nominal:
		/* New since dumping started; ignore. */
		break;
	case prof_tctx_state_dumping:
	case prof_tctx_state_purgatory:
		prof_tctx_merge_gctx(tsdn, tctx, tctx->gctx);
		break;
	default:
		not_reached();
	}

	return NULL;
}

typedef struct prof_dump_iter_arg_s prof_dump_iter_arg_t;
struct prof_dump_iter_arg_s {
	tsdn_t *tsdn;
	write_cb_t *prof_dump_write;
	void *cbopaque;
};

static prof_tctx_t *
prof_tctx_dump_iter(prof_tctx_tree_t *tctxs, prof_tctx_t *tctx, void *opaque) {
	prof_dump_iter_arg_t *arg = (prof_dump_iter_arg_t *)opaque;
	malloc_mutex_assert_owner(arg->tsdn, tctx->gctx->lock);

	switch (tctx->state) {
	case prof_tctx_state_initializing:
	case prof_tctx_state_nominal:
		/* Not captured by this dump. */
		break;
	case prof_tctx_state_dumping:
	case prof_tctx_state_purgatory:
		prof_dump_printf(arg->prof_dump_write, arg->cbopaque,
		    "  t%"FMTu64": ", tctx->thr_uid);
		prof_dump_print_cnts(arg->prof_dump_write, arg->cbopaque,
		    &tctx->dump_cnts);
		arg->prof_dump_write(arg->cbopaque, "\n");
		break;
	default:
		not_reached();
	}
	return NULL;
}

static prof_tctx_t *
prof_tctx_finish_iter(prof_tctx_tree_t *tctxs, prof_tctx_t *tctx, void *arg) {
	tsdn_t *tsdn = (tsdn_t *)arg;
	prof_tctx_t *ret;

	malloc_mutex_assert_owner(tsdn, tctx->gctx->lock);

	switch (tctx->state) {
	case prof_tctx_state_nominal:
		/* New since dumping started; ignore. */
		break;
	case prof_tctx_state_dumping:
		tctx->state = prof_tctx_state_nominal;
		break;
	case prof_tctx_state_purgatory:
		ret = tctx;
		goto label_return;
	default:
		not_reached();
	}

	ret = NULL;
label_return:
	return ret;
}

static void
prof_dump_gctx_prep(tsdn_t *tsdn, prof_gctx_t *gctx, prof_gctx_tree_t *gctxs) {
	cassert(config_prof);

	malloc_mutex_lock(tsdn, gctx->lock);

	/*
	 * Increment nlimbo so that gctx won't go away before dump.
	 * Additionally, link gctx into the dump list so that it is included in
	 * prof_dump()'s second pass.
	 */
	gctx->nlimbo++;
	gctx_tree_insert(gctxs, gctx);

	memset(&gctx->cnt_summed, 0, sizeof(prof_cnt_t));

	malloc_mutex_unlock(tsdn, gctx->lock);
}

typedef struct prof_gctx_merge_iter_arg_s prof_gctx_merge_iter_arg_t;
struct prof_gctx_merge_iter_arg_s {
	tsdn_t *tsdn;
	size_t *leak_ngctx;
};

static prof_gctx_t *
prof_gctx_merge_iter(prof_gctx_tree_t *gctxs, prof_gctx_t *gctx, void *opaque) {
	prof_gctx_merge_iter_arg_t *arg = (prof_gctx_merge_iter_arg_t *)opaque;

	malloc_mutex_lock(arg->tsdn, gctx->lock);
	tctx_tree_iter(&gctx->tctxs, NULL, prof_tctx_merge_iter,
	    (void *)arg->tsdn);
	if (gctx->cnt_summed.curobjs != 0) {
		(*arg->leak_ngctx)++;
	}
	malloc_mutex_unlock(arg->tsdn, gctx->lock);

	return NULL;
}

static void
prof_gctx_finish(tsd_t *tsd, prof_gctx_tree_t *gctxs) {
	prof_tdata_t *tdata = prof_tdata_get(tsd, false);
	prof_gctx_t *gctx;

	/*
	 * Standard tree iteration won't work here, because as soon as we
	 * decrement gctx->nlimbo and unlock gctx, another thread can
	 * concurrently destroy it, which will corrupt the tree.  Therefore,
	 * tear down the tree one node at a time during iteration.
	 */
	while ((gctx = gctx_tree_first(gctxs)) != NULL) {
		gctx_tree_remove(gctxs, gctx);
		malloc_mutex_lock(tsd_tsdn(tsd), gctx->lock);
		{
			prof_tctx_t *next;

			next = NULL;
			do {
				prof_tctx_t *to_destroy =
				    tctx_tree_iter(&gctx->tctxs, next,
				    prof_tctx_finish_iter,
				    (void *)tsd_tsdn(tsd));
				if (to_destroy != NULL) {
					next = tctx_tree_next(&gctx->tctxs,
					    to_destroy);
					tctx_tree_remove(&gctx->tctxs,
					    to_destroy);
					idalloctm(tsd_tsdn(tsd), to_destroy,
					    NULL, NULL, true, true);
				} else {
					next = NULL;
				}
			} while (next != NULL);
		}
		gctx->nlimbo--;
		if (prof_gctx_should_destroy(gctx)) {
			gctx->nlimbo++;
			malloc_mutex_unlock(tsd_tsdn(tsd), gctx->lock);
			prof_gctx_try_destroy(tsd, tdata, gctx);
		} else {
			malloc_mutex_unlock(tsd_tsdn(tsd), gctx->lock);
		}
	}
}

typedef struct prof_tdata_merge_iter_arg_s prof_tdata_merge_iter_arg_t;
struct prof_tdata_merge_iter_arg_s {
	tsdn_t *tsdn;
	prof_cnt_t *cnt_all;
};

static prof_tdata_t *
prof_tdata_merge_iter(prof_tdata_tree_t *tdatas_ptr, prof_tdata_t *tdata,
    void *opaque) {
	prof_tdata_merge_iter_arg_t *arg =
	    (prof_tdata_merge_iter_arg_t *)opaque;

	malloc_mutex_lock(arg->tsdn, tdata->lock);
	if (!tdata->expired) {
		size_t tabind;
		union {
			prof_tctx_t	*p;
			void		*v;
		} tctx;

		tdata->dumping = true;
		memset(&tdata->cnt_summed, 0, sizeof(prof_cnt_t));
		for (tabind = 0; !ckh_iter(&tdata->bt2tctx, &tabind, NULL,
		    &tctx.v);) {
			prof_tctx_merge_tdata(arg->tsdn, tctx.p, tdata);
		}

		arg->cnt_all->curobjs += tdata->cnt_summed.curobjs;
		arg->cnt_all->curobjs_shifted_unbiased
		    += tdata->cnt_summed.curobjs_shifted_unbiased;
		arg->cnt_all->curbytes += tdata->cnt_summed.curbytes;
		arg->cnt_all->curbytes_unbiased
		    += tdata->cnt_summed.curbytes_unbiased;
		if (opt_prof_accum) {
			arg->cnt_all->accumobjs += tdata->cnt_summed.accumobjs;
			arg->cnt_all->accumobjs_shifted_unbiased
			    += tdata->cnt_summed.accumobjs_shifted_unbiased;
			arg->cnt_all->accumbytes +=
			    tdata->cnt_summed.accumbytes;
			arg->cnt_all->accumbytes_unbiased +=
			    tdata->cnt_summed.accumbytes_unbiased;
		}
	} else {
		tdata->dumping = false;
	}
	malloc_mutex_unlock(arg->tsdn, tdata->lock);

	return NULL;
}

static prof_tdata_t *
prof_tdata_dump_iter(prof_tdata_tree_t *tdatas_ptr, prof_tdata_t *tdata,
    void *opaque) {
	if (!tdata->dumping) {
		return NULL;
	}

	prof_dump_iter_arg_t *arg = (prof_dump_iter_arg_t *)opaque;
	prof_dump_printf(arg->prof_dump_write, arg->cbopaque, "  t%"FMTu64": ",
	    tdata->thr_uid);
	prof_dump_print_cnts(arg->prof_dump_write, arg->cbopaque,
	    &tdata->cnt_summed);
	if (tdata->thread_name != NULL) {
		arg->prof_dump_write(arg->cbopaque, " ");
		arg->prof_dump_write(arg->cbopaque, tdata->thread_name);
	}
	arg->prof_dump_write(arg->cbopaque, "\n");
	return NULL;
}

static void
prof_dump_header(prof_dump_iter_arg_t *arg, const prof_cnt_t *cnt_all) {
	prof_dump_printf(arg->prof_dump_write, arg->cbopaque,
	    "heap_v2/%"FMTu64"\n  t*: ", ((uint64_t)1U << lg_prof_sample));
	prof_dump_print_cnts(arg->prof_dump_write, arg->cbopaque, cnt_all);
	arg->prof_dump_write(arg->cbopaque, "\n");

	malloc_mutex_lock(arg->tsdn, &tdatas_mtx);
	tdata_tree_iter(&tdatas, NULL, prof_tdata_dump_iter, arg);
	malloc_mutex_unlock(arg->tsdn, &tdatas_mtx);
}

static void
prof_dump_gctx(prof_dump_iter_arg_t *arg, prof_gctx_t *gctx,
    const prof_bt_t *bt, prof_gctx_tree_t *gctxs) {
	cassert(config_prof);
	malloc_mutex_assert_owner(arg->tsdn, gctx->lock);

	/* Avoid dumping such gctx's that have no useful data. */
	if ((!opt_prof_accum && gctx->cnt_summed.curobjs == 0) ||
	    (opt_prof_accum && gctx->cnt_summed.accumobjs == 0)) {
		assert(gctx->cnt_summed.curobjs == 0);
		assert(gctx->cnt_summed.curbytes == 0);
		/*
		 * These asserts would not be correct -- see the comment on races
		 * in prof.c
		 * assert(gctx->cnt_summed.curobjs_unbiased == 0);
		 * assert(gctx->cnt_summed.curbytes_unbiased == 0);
		*/
		assert(gctx->cnt_summed.accumobjs == 0);
		assert(gctx->cnt_summed.accumobjs_shifted_unbiased == 0);
		assert(gctx->cnt_summed.accumbytes == 0);
		assert(gctx->cnt_summed.accumbytes_unbiased == 0);
		return;
	}

	arg->prof_dump_write(arg->cbopaque, "@");
	for (unsigned i = 0; i < bt->len; i++) {
		prof_dump_printf(arg->prof_dump_write, arg->cbopaque,
		    " %#"FMTxPTR, (uintptr_t)bt->vec[i]);
	}

	arg->prof_dump_write(arg->cbopaque, "\n  t*: ");
	prof_dump_print_cnts(arg->prof_dump_write, arg->cbopaque,
	    &gctx->cnt_summed);
	arg->prof_dump_write(arg->cbopaque, "\n");

	tctx_tree_iter(&gctx->tctxs, NULL, prof_tctx_dump_iter, arg);
}

/*
 * See prof_sample_new_event_wait() comment for why the body of this function
 * is conditionally compiled.
 */
static void
prof_leakcheck(const prof_cnt_t *cnt_all, size_t leak_ngctx) {
#ifdef JEMALLOC_PROF
	/*
	 * Scaling is equivalent AdjustSamples() in jeprof, but the result may
	 * differ slightly from what jeprof reports, because here we scale the
	 * summary values, whereas jeprof scales each context individually and
	 * reports the sums of the scaled values.
	 */
	if (cnt_all->curbytes != 0) {
		double sample_period = (double)((uint64_t)1 << lg_prof_sample);
		double ratio = (((double)cnt_all->curbytes) /
		    (double)cnt_all->curobjs) / sample_period;
		double scale_factor = 1.0 / (1.0 - exp(-ratio));
		uint64_t curbytes = (uint64_t)round(((double)cnt_all->curbytes)
		    * scale_factor);
		uint64_t curobjs = (uint64_t)round(((double)cnt_all->curobjs) *
		    scale_factor);

		malloc_printf("<jemalloc>: Leak approximation summary: ~%"FMTu64
		    " byte%s, ~%"FMTu64" object%s, >= %zu context%s\n",
		    curbytes, (curbytes != 1) ? "s" : "", curobjs, (curobjs !=
		    1) ? "s" : "", leak_ngctx, (leak_ngctx != 1) ? "s" : "");
		malloc_printf(
		    "<jemalloc>: Run jeprof on dump output for leak detail\n");
		if (opt_prof_leak_error) {
			malloc_printf(
			    "<jemalloc>: Exiting with error code because memory"
			    " leaks were detected\n");
			/*
			 * Use _exit() with underscore to avoid calling atexit()
			 * and entering endless cycle.
			 */
			_exit(1);
		}
	}
#endif
}

static prof_gctx_t *
prof_gctx_dump_iter(prof_gctx_tree_t *gctxs, prof_gctx_t *gctx, void *opaque) {
	prof_dump_iter_arg_t *arg = (prof_dump_iter_arg_t *)opaque;
	malloc_mutex_lock(arg->tsdn, gctx->lock);
	prof_dump_gctx(arg, gctx, &gctx->bt, gctxs);
	malloc_mutex_unlock(arg->tsdn, gctx->lock);
	return NULL;
}

static void
prof_dump_prep(tsd_t *tsd, prof_tdata_t *tdata, prof_cnt_t *cnt_all,
    size_t *leak_ngctx, prof_gctx_tree_t *gctxs) {
	size_t tabind;
	union {
		prof_gctx_t	*p;
		void		*v;
	} gctx;

	prof_enter(tsd, tdata);

	/*
	 * Put gctx's in limbo and clear their counters in preparation for
	 * summing.
	 */
	gctx_tree_new(gctxs);
	for (tabind = 0; !ckh_iter(&bt2gctx, &tabind, NULL, &gctx.v);) {
		prof_dump_gctx_prep(tsd_tsdn(tsd), gctx.p, gctxs);
	}

	/*
	 * Iterate over tdatas, and for the non-expired ones snapshot their tctx
	 * stats and merge them into the associated gctx's.
	 */
	memset(cnt_all, 0, sizeof(prof_cnt_t));
	prof_tdata_merge_iter_arg_t prof_tdata_merge_iter_arg = {tsd_tsdn(tsd),
	    cnt_all};
	malloc_mutex_lock(tsd_tsdn(tsd), &tdatas_mtx);
	tdata_tree_iter(&tdatas, NULL, prof_tdata_merge_iter,
	    &prof_tdata_merge_iter_arg);
	malloc_mutex_unlock(tsd_tsdn(tsd), &tdatas_mtx);

	/* Merge tctx stats into gctx's. */
	*leak_ngctx = 0;
	prof_gctx_merge_iter_arg_t prof_gctx_merge_iter_arg = {tsd_tsdn(tsd),
	    leak_ngctx};
	gctx_tree_iter(gctxs, NULL, prof_gctx_merge_iter,
	    &prof_gctx_merge_iter_arg);

	prof_leave(tsd, tdata);
}

void
prof_dump_impl(tsd_t *tsd, write_cb_t *prof_dump_write, void *cbopaque,
    prof_tdata_t *tdata, bool leakcheck) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_dump_mtx);
	prof_cnt_t cnt_all;
	size_t leak_ngctx;
	prof_gctx_tree_t gctxs;
	prof_dump_prep(tsd, tdata, &cnt_all, &leak_ngctx, &gctxs);
	prof_dump_iter_arg_t prof_dump_iter_arg = {tsd_tsdn(tsd),
	    prof_dump_write, cbopaque};
	prof_dump_header(&prof_dump_iter_arg, &cnt_all);
	gctx_tree_iter(&gctxs, NULL, prof_gctx_dump_iter, &prof_dump_iter_arg);
	prof_gctx_finish(tsd, &gctxs);
	if (leakcheck) {
		prof_leakcheck(&cnt_all, leak_ngctx);
	}
}

/* Used in unit tests. */
void
prof_cnt_all(prof_cnt_t *cnt_all) {
	tsd_t *tsd = tsd_fetch();
	prof_tdata_t *tdata = prof_tdata_get(tsd, false);
	if (tdata == NULL) {
		memset(cnt_all, 0, sizeof(prof_cnt_t));
	} else {
		size_t leak_ngctx;
		prof_gctx_tree_t gctxs;
		prof_dump_prep(tsd, tdata, cnt_all, &leak_ngctx, &gctxs);
		prof_gctx_finish(tsd, &gctxs);
	}
}

void
prof_bt_hash(const void *key, size_t r_hash[2]) {
	prof_bt_t *bt = (prof_bt_t *)key;

	cassert(config_prof);

	hash(bt->vec, bt->len * sizeof(void *), 0x94122f33U, r_hash);
}

bool
prof_bt_keycomp(const void *k1, const void *k2) {
	const prof_bt_t *bt1 = (prof_bt_t *)k1;
	const prof_bt_t *bt2 = (prof_bt_t *)k2;

	cassert(config_prof);

	if (bt1->len != bt2->len) {
		return false;
	}
	return (memcmp(bt1->vec, bt2->vec, bt1->len * sizeof(void *)) == 0);
}

prof_tdata_t *
prof_tdata_init_impl(tsd_t *tsd, uint64_t thr_uid, uint64_t thr_discrim,
    char *thread_name, bool active) {
	assert(tsd_reentrancy_level_get(tsd) == 0);

	prof_tdata_t *tdata;

	cassert(config_prof);

	/* Initialize an empty cache for this thread. */
	tdata = (prof_tdata_t *)iallocztm(tsd_tsdn(tsd), sizeof(prof_tdata_t),
	    sz_size2index(sizeof(prof_tdata_t)), false, NULL, true,
	    arena_get(TSDN_NULL, 0, true), true);
	if (tdata == NULL) {
		return NULL;
	}

	tdata->lock = prof_tdata_mutex_choose(thr_uid);
	tdata->thr_uid = thr_uid;
	tdata->thr_discrim = thr_discrim;
	tdata->thread_name = thread_name;
	tdata->attached = true;
	tdata->expired = false;
	tdata->tctx_uid_next = 0;

	if (ckh_new(tsd, &tdata->bt2tctx, PROF_CKH_MINITEMS, prof_bt_hash,
	    prof_bt_keycomp)) {
		idalloctm(tsd_tsdn(tsd), tdata, NULL, NULL, true, true);
		return NULL;
	}

	tdata->enq = false;
	tdata->enq_idump = false;
	tdata->enq_gdump = false;

	tdata->dumping = false;
	tdata->active = active;

	malloc_mutex_lock(tsd_tsdn(tsd), &tdatas_mtx);
	tdata_tree_insert(&tdatas, tdata);
	malloc_mutex_unlock(tsd_tsdn(tsd), &tdatas_mtx);

	return tdata;
}

static bool
prof_tdata_should_destroy_unlocked(prof_tdata_t *tdata, bool even_if_attached) {
	if (tdata->attached && !even_if_attached) {
		return false;
	}
	if (ckh_count(&tdata->bt2tctx) != 0) {
		return false;
	}
	return true;
}

static bool
prof_tdata_should_destroy(tsdn_t *tsdn, prof_tdata_t *tdata,
    bool even_if_attached) {
	malloc_mutex_assert_owner(tsdn, tdata->lock);

	return prof_tdata_should_destroy_unlocked(tdata, even_if_attached);
}

static void
prof_tdata_destroy_locked(tsd_t *tsd, prof_tdata_t *tdata,
    bool even_if_attached) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &tdatas_mtx);
	malloc_mutex_assert_not_owner(tsd_tsdn(tsd), tdata->lock);

	tdata_tree_remove(&tdatas, tdata);

	assert(prof_tdata_should_destroy_unlocked(tdata, even_if_attached));

	if (tdata->thread_name != NULL) {
		idalloctm(tsd_tsdn(tsd), tdata->thread_name, NULL, NULL, true,
		    true);
	}
	ckh_delete(tsd, &tdata->bt2tctx);
	idalloctm(tsd_tsdn(tsd), tdata, NULL, NULL, true, true);
}

static void
prof_tdata_destroy(tsd_t *tsd, prof_tdata_t *tdata, bool even_if_attached) {
	malloc_mutex_lock(tsd_tsdn(tsd), &tdatas_mtx);
	prof_tdata_destroy_locked(tsd, tdata, even_if_attached);
	malloc_mutex_unlock(tsd_tsdn(tsd), &tdatas_mtx);
}

void
prof_tdata_detach(tsd_t *tsd, prof_tdata_t *tdata) {
	bool destroy_tdata;

	malloc_mutex_lock(tsd_tsdn(tsd), tdata->lock);
	if (tdata->attached) {
		destroy_tdata = prof_tdata_should_destroy(tsd_tsdn(tsd), tdata,
		    true);
		/*
		 * Only detach if !destroy_tdata, because detaching would allow
		 * another thread to win the race to destroy tdata.
		 */
		if (!destroy_tdata) {
			tdata->attached = false;
		}
		tsd_prof_tdata_set(tsd, NULL);
	} else {
		destroy_tdata = false;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), tdata->lock);
	if (destroy_tdata) {
		prof_tdata_destroy(tsd, tdata, true);
	}
}

static bool
prof_tdata_expire(tsdn_t *tsdn, prof_tdata_t *tdata) {
	bool destroy_tdata;

	malloc_mutex_lock(tsdn, tdata->lock);
	if (!tdata->expired) {
		tdata->expired = true;
		destroy_tdata = prof_tdata_should_destroy(tsdn, tdata, false);
	} else {
		destroy_tdata = false;
	}
	malloc_mutex_unlock(tsdn, tdata->lock);

	return destroy_tdata;
}

static prof_tdata_t *
prof_tdata_reset_iter(prof_tdata_tree_t *tdatas_ptr, prof_tdata_t *tdata,
    void *arg) {
	tsdn_t *tsdn = (tsdn_t *)arg;

	return (prof_tdata_expire(tsdn, tdata) ? tdata : NULL);
}

void
prof_reset(tsd_t *tsd, size_t lg_sample) {
	prof_tdata_t *next;

	assert(lg_sample < (sizeof(uint64_t) << 3));

	malloc_mutex_lock(tsd_tsdn(tsd), &prof_dump_mtx);
	malloc_mutex_lock(tsd_tsdn(tsd), &tdatas_mtx);

	lg_prof_sample = lg_sample;
	prof_unbias_map_init();

	next = NULL;
	do {
		prof_tdata_t *to_destroy = tdata_tree_iter(&tdatas, next,
		    prof_tdata_reset_iter, (void *)tsd);
		if (to_destroy != NULL) {
			next = tdata_tree_next(&tdatas, to_destroy);
			prof_tdata_destroy_locked(tsd, to_destroy, false);
		} else {
			next = NULL;
		}
	} while (next != NULL);

	malloc_mutex_unlock(tsd_tsdn(tsd), &tdatas_mtx);
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_dump_mtx);
}

static bool
prof_tctx_should_destroy(tsd_t *tsd, prof_tctx_t *tctx) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), tctx->tdata->lock);

	if (opt_prof_accum) {
		return false;
	}
	if (tctx->cnts.curobjs != 0) {
		return false;
	}
	if (tctx->prepared) {
		return false;
	}
	if (tctx->recent_count != 0) {
		return false;
	}
	return true;
}

static void
prof_tctx_destroy(tsd_t *tsd, prof_tctx_t *tctx) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), tctx->tdata->lock);

	assert(tctx->cnts.curobjs == 0);
	assert(tctx->cnts.curbytes == 0);
	/*
	 * These asserts are not correct -- see the comment about races in
	 * prof.c
	 *
	 * assert(tctx->cnts.curobjs_shifted_unbiased == 0);
	 * assert(tctx->cnts.curbytes_unbiased == 0);
	 */
	assert(!opt_prof_accum);
	assert(tctx->cnts.accumobjs == 0);
	assert(tctx->cnts.accumbytes == 0);
	/*
	 * These ones are, since accumbyte counts never go down.  Either
	 * prof_accum is off (in which case these should never have changed from
	 * their initial value of zero), or it's on (in which case we shouldn't
	 * be destroying this tctx).
	 */
	assert(tctx->cnts.accumobjs_shifted_unbiased == 0);
	assert(tctx->cnts.accumbytes_unbiased == 0);

	prof_gctx_t *gctx = tctx->gctx;

	{
		prof_tdata_t *tdata = tctx->tdata;
		tctx->tdata = NULL;
		ckh_remove(tsd, &tdata->bt2tctx, &gctx->bt, NULL, NULL);
		bool destroy_tdata = prof_tdata_should_destroy(tsd_tsdn(tsd),
		    tdata, false);
		malloc_mutex_unlock(tsd_tsdn(tsd), tdata->lock);
		if (destroy_tdata) {
			prof_tdata_destroy(tsd, tdata, false);
		}
	}

	bool destroy_tctx, destroy_gctx;

	malloc_mutex_lock(tsd_tsdn(tsd), gctx->lock);
	switch (tctx->state) {
	case prof_tctx_state_nominal:
		tctx_tree_remove(&gctx->tctxs, tctx);
		destroy_tctx = true;
		if (prof_gctx_should_destroy(gctx)) {
			/*
			 * Increment gctx->nlimbo in order to keep another
			 * thread from winning the race to destroy gctx while
			 * this one has gctx->lock dropped.  Without this, it
			 * would be possible for another thread to:
			 *
			 * 1) Sample an allocation associated with gctx.
			 * 2) Deallocate the sampled object.
			 * 3) Successfully prof_gctx_try_destroy(gctx).
			 *
			 * The result would be that gctx no longer exists by the
			 * time this thread accesses it in
			 * prof_gctx_try_destroy().
			 */
			gctx->nlimbo++;
			destroy_gctx = true;
		} else {
			destroy_gctx = false;
		}
		break;
	case prof_tctx_state_dumping:
		/*
		 * A dumping thread needs tctx to remain valid until dumping
		 * has finished.  Change state such that the dumping thread will
		 * complete destruction during a late dump iteration phase.
		 */
		tctx->state = prof_tctx_state_purgatory;
		destroy_tctx = false;
		destroy_gctx = false;
		break;
	default:
		not_reached();
		destroy_tctx = false;
		destroy_gctx = false;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), gctx->lock);
	if (destroy_gctx) {
		prof_gctx_try_destroy(tsd, prof_tdata_get(tsd, false), gctx);
	}
	if (destroy_tctx) {
		idalloctm(tsd_tsdn(tsd), tctx, NULL, NULL, true, true);
	}
}

void
prof_tctx_try_destroy(tsd_t *tsd, prof_tctx_t *tctx) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), tctx->tdata->lock);
	if (prof_tctx_should_destroy(tsd, tctx)) {
		/* tctx->tdata->lock will be released in prof_tctx_destroy(). */
		prof_tctx_destroy(tsd, tctx);
	} else {
		malloc_mutex_unlock(tsd_tsdn(tsd), tctx->tdata->lock);
	}
}

/******************************************************************************/

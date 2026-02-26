#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/counter.h"
#include "jemalloc/internal/prof_data.h"
#include "jemalloc/internal/prof_log.h"
#include "jemalloc/internal/prof_recent.h"
#include "jemalloc/internal/prof_stats.h"
#include "jemalloc/internal/prof_sys.h"
#include "jemalloc/internal/prof_hook.h"
#include "jemalloc/internal/thread_event.h"

/*
 * This file implements the profiling "APIs" needed by other parts of jemalloc,
 * and also manages the relevant "operational" data, mainly options and mutexes;
 * the core profiling data structures are encapsulated in prof_data.c.
 */

/******************************************************************************/

/* Data. */

bool opt_prof = false;
bool opt_prof_active = true;
bool opt_prof_thread_active_init = true;
size_t opt_lg_prof_sample = LG_PROF_SAMPLE_DEFAULT;
ssize_t opt_lg_prof_interval = LG_PROF_INTERVAL_DEFAULT;
bool opt_prof_gdump = false;
bool opt_prof_final = false;
bool opt_prof_leak = false;
bool opt_prof_leak_error = false;
bool opt_prof_accum = false;
char opt_prof_prefix[PROF_DUMP_FILENAME_LEN];
bool opt_prof_sys_thread_name = false;
bool opt_prof_unbias = true;

/* Accessed via prof_sample_event_handler(). */
static counter_accum_t prof_idump_accumulated;

/*
 * Initialized as opt_prof_active, and accessed via
 * prof_active_[gs]et{_unlocked,}().
 */
bool prof_active_state;
static malloc_mutex_t prof_active_mtx;

/*
 * Initialized as opt_prof_thread_active_init, and accessed via
 * prof_thread_active_init_[gs]et().
 */
static bool prof_thread_active_init;
static malloc_mutex_t prof_thread_active_init_mtx;

/*
 * Initialized as opt_prof_gdump, and accessed via
 * prof_gdump_[gs]et{_unlocked,}().
 */
bool prof_gdump_val;
static malloc_mutex_t prof_gdump_mtx;

uint64_t prof_interval = 0;

size_t lg_prof_sample;

static uint64_t next_thr_uid;
static malloc_mutex_t next_thr_uid_mtx;

/* Do not dump any profiles until bootstrapping is complete. */
bool prof_booted = false;

/* Logically a prof_backtrace_hook_t. */
atomic_p_t prof_backtrace_hook;

/* Logically a prof_dump_hook_t. */
atomic_p_t prof_dump_hook;

/******************************************************************************/

void
prof_alloc_rollback(tsd_t *tsd, prof_tctx_t *tctx) {
	cassert(config_prof);

	if (tsd_reentrancy_level_get(tsd) > 0) {
		assert((uintptr_t)tctx == (uintptr_t)1U);
		return;
	}

	if ((uintptr_t)tctx > (uintptr_t)1U) {
		malloc_mutex_lock(tsd_tsdn(tsd), tctx->tdata->lock);
		tctx->prepared = false;
		prof_tctx_try_destroy(tsd, tctx);
	}
}

void
prof_malloc_sample_object(tsd_t *tsd, const void *ptr, size_t size,
    size_t usize, prof_tctx_t *tctx) {
	cassert(config_prof);

	if (opt_prof_sys_thread_name) {
		prof_sys_thread_name_fetch(tsd);
	}

	edata_t *edata = emap_edata_lookup(tsd_tsdn(tsd), &arena_emap_global,
	    ptr);
	prof_info_set(tsd, edata, tctx, size);

	szind_t szind = sz_size2index(usize);

	malloc_mutex_lock(tsd_tsdn(tsd), tctx->tdata->lock);
	/*
	 * We need to do these map lookups while holding the lock, to avoid the
	 * possibility of races with prof_reset calls, which update the map and
	 * then acquire the lock.  This actually still leaves a data race on the
	 * contents of the unbias map, but we have not yet gone through and
	 * atomic-ified the prof module, and compilers are not yet causing us
	 * issues.  The key thing is to make sure that, if we read garbage data,
	 * the prof_reset call is about to mark our tctx as expired before any
	 * dumping of our corrupted output is attempted.
	 */
	size_t shifted_unbiased_cnt = prof_shifted_unbiased_cnt[szind];
	size_t unbiased_bytes = prof_unbiased_sz[szind];
	tctx->cnts.curobjs++;
	tctx->cnts.curobjs_shifted_unbiased += shifted_unbiased_cnt;
	tctx->cnts.curbytes += usize;
	tctx->cnts.curbytes_unbiased += unbiased_bytes;
	if (opt_prof_accum) {
		tctx->cnts.accumobjs++;
		tctx->cnts.accumobjs_shifted_unbiased += shifted_unbiased_cnt;
		tctx->cnts.accumbytes += usize;
		tctx->cnts.accumbytes_unbiased += unbiased_bytes;
	}
	bool record_recent = prof_recent_alloc_prepare(tsd, tctx);
	tctx->prepared = false;
	malloc_mutex_unlock(tsd_tsdn(tsd), tctx->tdata->lock);
	if (record_recent) {
		assert(tctx == edata_prof_tctx_get(edata));
		prof_recent_alloc(tsd, edata, size, usize);
	}

	if (opt_prof_stats) {
		prof_stats_inc(tsd, szind, size);
	}
}

void
prof_free_sampled_object(tsd_t *tsd, size_t usize, prof_info_t *prof_info) {
	cassert(config_prof);

	assert(prof_info != NULL);
	prof_tctx_t *tctx = prof_info->alloc_tctx;
	assert((uintptr_t)tctx > (uintptr_t)1U);

	szind_t szind = sz_size2index(usize);
	malloc_mutex_lock(tsd_tsdn(tsd), tctx->tdata->lock);

	assert(tctx->cnts.curobjs > 0);
	assert(tctx->cnts.curbytes >= usize);
	/*
	 * It's not correct to do equivalent asserts for unbiased bytes, because
	 * of the potential for races with prof.reset calls.  The map contents
	 * should really be atomic, but we have not atomic-ified the prof module
	 * yet.
	 */
	tctx->cnts.curobjs--;
	tctx->cnts.curobjs_shifted_unbiased -= prof_shifted_unbiased_cnt[szind];
	tctx->cnts.curbytes -= usize;
	tctx->cnts.curbytes_unbiased -= prof_unbiased_sz[szind];

	prof_try_log(tsd, usize, prof_info);

	prof_tctx_try_destroy(tsd, tctx);

	if (opt_prof_stats) {
		prof_stats_dec(tsd, szind, prof_info->alloc_size);
	}
}

prof_tctx_t *
prof_tctx_create(tsd_t *tsd) {
	if (!tsd_nominal(tsd) || tsd_reentrancy_level_get(tsd) > 0) {
		return NULL;
	}

	prof_tdata_t *tdata = prof_tdata_get(tsd, true);
	if (tdata == NULL) {
		return NULL;
	}

	prof_bt_t bt;
	bt_init(&bt, tdata->vec);
	prof_backtrace(tsd, &bt);
	return prof_lookup(tsd, &bt);
}

/*
 * The bodies of this function and prof_leakcheck() are compiled out unless heap
 * profiling is enabled, so that it is possible to compile jemalloc with
 * floating point support completely disabled.  Avoiding floating point code is
 * important on memory-constrained systems, but it also enables a workaround for
 * versions of glibc that don't properly save/restore floating point registers
 * during dynamic lazy symbol loading (which internally calls into whatever
 * malloc implementation happens to be integrated into the application).  Note
 * that some compilers (e.g.  gcc 4.8) may use floating point registers for fast
 * memory moves, so jemalloc must be compiled with such optimizations disabled
 * (e.g.
 * -mno-sse) in order for the workaround to be complete.
 */
uint64_t
prof_sample_new_event_wait(tsd_t *tsd) {
#ifdef JEMALLOC_PROF
	if (lg_prof_sample == 0) {
		return TE_MIN_START_WAIT;
	}

	/*
	 * Compute sample interval as a geometrically distributed random
	 * variable with mean (2^lg_prof_sample).
	 *
	 *                      __        __
	 *                      |  log(u)  |                     1
	 * bytes_until_sample = | -------- |, where p = ---------------
	 *                      | log(1-p) |             lg_prof_sample
	 *                                              2
	 *
	 * For more information on the math, see:
	 *
	 *   Non-Uniform Random Variate Generation
	 *   Luc Devroye
	 *   Springer-Verlag, New York, 1986
	 *   pp 500
	 *   (http://luc.devroye.org/rnbookindex.html)
	 *
	 * In the actual computation, there's a non-zero probability that our
	 * pseudo random number generator generates an exact 0, and to avoid
	 * log(0), we set u to 1.0 in case r is 0.  Therefore u effectively is
	 * uniformly distributed in (0, 1] instead of [0, 1).  Further, rather
	 * than taking the ceiling, we take the floor and then add 1, since
	 * otherwise bytes_until_sample would be 0 if u is exactly 1.0.
	 */
	uint64_t r = prng_lg_range_u64(tsd_prng_statep_get(tsd), 53);
	double u = (r == 0U) ? 1.0 : (double)r * (1.0/9007199254740992.0L);
	return (uint64_t)(log(u) /
	    log(1.0 - (1.0 / (double)((uint64_t)1U << lg_prof_sample))))
	    + (uint64_t)1U;
#else
	not_reached();
	return TE_MAX_START_WAIT;
#endif
}

uint64_t
prof_sample_postponed_event_wait(tsd_t *tsd) {
	/*
	 * The postponed wait time for prof sample event is computed as if we
	 * want a new wait time (i.e. as if the event were triggered).  If we
	 * instead postpone to the immediate next allocation, like how we're
	 * handling the other events, then we can have sampling bias, if e.g.
	 * the allocation immediately following a reentrancy always comes from
	 * the same stack trace.
	 */
	return prof_sample_new_event_wait(tsd);
}

void
prof_sample_event_handler(tsd_t *tsd, uint64_t elapsed) {
	cassert(config_prof);
	assert(elapsed > 0 && elapsed != TE_INVALID_ELAPSED);
	if (prof_interval == 0 || !prof_active_get_unlocked()) {
		return;
	}
	if (counter_accum(tsd_tsdn(tsd), &prof_idump_accumulated, elapsed)) {
		prof_idump(tsd_tsdn(tsd));
	}
}

static void
prof_fdump(void) {
	tsd_t *tsd;

	cassert(config_prof);
	assert(opt_prof_final);

	if (!prof_booted) {
		return;
	}
	tsd = tsd_fetch();
	assert(tsd_reentrancy_level_get(tsd) == 0);

	prof_fdump_impl(tsd);
}

static bool
prof_idump_accum_init(void) {
	cassert(config_prof);

	return counter_accum_init(&prof_idump_accumulated, prof_interval);
}

void
prof_idump(tsdn_t *tsdn) {
	tsd_t *tsd;
	prof_tdata_t *tdata;

	cassert(config_prof);

	if (!prof_booted || tsdn_null(tsdn) || !prof_active_get_unlocked()) {
		return;
	}
	tsd = tsdn_tsd(tsdn);
	if (tsd_reentrancy_level_get(tsd) > 0) {
		return;
	}

	tdata = prof_tdata_get(tsd, true);
	if (tdata == NULL) {
		return;
	}
	if (tdata->enq) {
		tdata->enq_idump = true;
		return;
	}

	prof_idump_impl(tsd);
}

bool
prof_mdump(tsd_t *tsd, const char *filename) {
	cassert(config_prof);
	assert(tsd_reentrancy_level_get(tsd) == 0);

	if (!opt_prof || !prof_booted) {
		return true;
	}

	return prof_mdump_impl(tsd, filename);
}

void
prof_gdump(tsdn_t *tsdn) {
	tsd_t *tsd;
	prof_tdata_t *tdata;

	cassert(config_prof);

	if (!prof_booted || tsdn_null(tsdn) || !prof_active_get_unlocked()) {
		return;
	}
	tsd = tsdn_tsd(tsdn);
	if (tsd_reentrancy_level_get(tsd) > 0) {
		return;
	}

	tdata = prof_tdata_get(tsd, false);
	if (tdata == NULL) {
		return;
	}
	if (tdata->enq) {
		tdata->enq_gdump = true;
		return;
	}

	prof_gdump_impl(tsd);
}

static uint64_t
prof_thr_uid_alloc(tsdn_t *tsdn) {
	uint64_t thr_uid;

	malloc_mutex_lock(tsdn, &next_thr_uid_mtx);
	thr_uid = next_thr_uid;
	next_thr_uid++;
	malloc_mutex_unlock(tsdn, &next_thr_uid_mtx);

	return thr_uid;
}

prof_tdata_t *
prof_tdata_init(tsd_t *tsd) {
	return prof_tdata_init_impl(tsd, prof_thr_uid_alloc(tsd_tsdn(tsd)), 0,
	    NULL, prof_thread_active_init_get(tsd_tsdn(tsd)));
}

prof_tdata_t *
prof_tdata_reinit(tsd_t *tsd, prof_tdata_t *tdata) {
	uint64_t thr_uid = tdata->thr_uid;
	uint64_t thr_discrim = tdata->thr_discrim + 1;
	char *thread_name = (tdata->thread_name != NULL) ?
	    prof_thread_name_alloc(tsd, tdata->thread_name) : NULL;
	bool active = tdata->active;

	prof_tdata_detach(tsd, tdata);
	return prof_tdata_init_impl(tsd, thr_uid, thr_discrim, thread_name,
	    active);
}

void
prof_tdata_cleanup(tsd_t *tsd) {
	prof_tdata_t *tdata;

	if (!config_prof) {
		return;
	}

	tdata = tsd_prof_tdata_get(tsd);
	if (tdata != NULL) {
		prof_tdata_detach(tsd, tdata);
	}
}

bool
prof_active_get(tsdn_t *tsdn) {
	bool prof_active_current;

	prof_active_assert();
	malloc_mutex_lock(tsdn, &prof_active_mtx);
	prof_active_current = prof_active_state;
	malloc_mutex_unlock(tsdn, &prof_active_mtx);
	return prof_active_current;
}

bool
prof_active_set(tsdn_t *tsdn, bool active) {
	bool prof_active_old;

	prof_active_assert();
	malloc_mutex_lock(tsdn, &prof_active_mtx);
	prof_active_old = prof_active_state;
	prof_active_state = active;
	malloc_mutex_unlock(tsdn, &prof_active_mtx);
	prof_active_assert();
	return prof_active_old;
}

const char *
prof_thread_name_get(tsd_t *tsd) {
	assert(tsd_reentrancy_level_get(tsd) == 0);

	prof_tdata_t *tdata;

	tdata = prof_tdata_get(tsd, true);
	if (tdata == NULL) {
		return "";
	}
	return (tdata->thread_name != NULL ? tdata->thread_name : "");
}

int
prof_thread_name_set(tsd_t *tsd, const char *thread_name) {
	if (opt_prof_sys_thread_name) {
		return ENOENT;
	} else {
		return prof_thread_name_set_impl(tsd, thread_name);
	}
}

bool
prof_thread_active_get(tsd_t *tsd) {
	assert(tsd_reentrancy_level_get(tsd) == 0);

	prof_tdata_t *tdata;

	tdata = prof_tdata_get(tsd, true);
	if (tdata == NULL) {
		return false;
	}
	return tdata->active;
}

bool
prof_thread_active_set(tsd_t *tsd, bool active) {
	assert(tsd_reentrancy_level_get(tsd) == 0);

	prof_tdata_t *tdata;

	tdata = prof_tdata_get(tsd, true);
	if (tdata == NULL) {
		return true;
	}
	tdata->active = active;
	return false;
}

bool
prof_thread_active_init_get(tsdn_t *tsdn) {
	bool active_init;

	malloc_mutex_lock(tsdn, &prof_thread_active_init_mtx);
	active_init = prof_thread_active_init;
	malloc_mutex_unlock(tsdn, &prof_thread_active_init_mtx);
	return active_init;
}

bool
prof_thread_active_init_set(tsdn_t *tsdn, bool active_init) {
	bool active_init_old;

	malloc_mutex_lock(tsdn, &prof_thread_active_init_mtx);
	active_init_old = prof_thread_active_init;
	prof_thread_active_init = active_init;
	malloc_mutex_unlock(tsdn, &prof_thread_active_init_mtx);
	return active_init_old;
}

bool
prof_gdump_get(tsdn_t *tsdn) {
	bool prof_gdump_current;

	malloc_mutex_lock(tsdn, &prof_gdump_mtx);
	prof_gdump_current = prof_gdump_val;
	malloc_mutex_unlock(tsdn, &prof_gdump_mtx);
	return prof_gdump_current;
}

bool
prof_gdump_set(tsdn_t *tsdn, bool gdump) {
	bool prof_gdump_old;

	malloc_mutex_lock(tsdn, &prof_gdump_mtx);
	prof_gdump_old = prof_gdump_val;
	prof_gdump_val = gdump;
	malloc_mutex_unlock(tsdn, &prof_gdump_mtx);
	return prof_gdump_old;
}

void
prof_backtrace_hook_set(prof_backtrace_hook_t hook) {
	atomic_store_p(&prof_backtrace_hook, hook, ATOMIC_RELEASE);
}

prof_backtrace_hook_t
prof_backtrace_hook_get() {
	return (prof_backtrace_hook_t)atomic_load_p(&prof_backtrace_hook,
	    ATOMIC_ACQUIRE);
}

void
prof_dump_hook_set(prof_dump_hook_t hook) {
	atomic_store_p(&prof_dump_hook, hook, ATOMIC_RELEASE);
}

prof_dump_hook_t
prof_dump_hook_get() {
	return (prof_dump_hook_t)atomic_load_p(&prof_dump_hook,
	    ATOMIC_ACQUIRE);
}

void
prof_boot0(void) {
	cassert(config_prof);

	memcpy(opt_prof_prefix, PROF_PREFIX_DEFAULT,
	    sizeof(PROF_PREFIX_DEFAULT));
}

void
prof_boot1(void) {
	cassert(config_prof);

	/*
	 * opt_prof must be in its final state before any arenas are
	 * initialized, so this function must be executed early.
	 */
	if (opt_prof_leak_error && !opt_prof_leak) {
		opt_prof_leak = true;
	}

	if (opt_prof_leak && !opt_prof) {
		/*
		 * Enable opt_prof, but in such a way that profiles are never
		 * automatically dumped.
		 */
		opt_prof = true;
		opt_prof_gdump = false;
	} else if (opt_prof) {
		if (opt_lg_prof_interval >= 0) {
			prof_interval = (((uint64_t)1U) <<
			    opt_lg_prof_interval);
		}
	}
}

bool
prof_boot2(tsd_t *tsd, base_t *base) {
	cassert(config_prof);

	/*
	 * Initialize the global mutexes unconditionally to maintain correct
	 * stats when opt_prof is false.
	 */
	if (malloc_mutex_init(&prof_active_mtx, "prof_active",
	    WITNESS_RANK_PROF_ACTIVE, malloc_mutex_rank_exclusive)) {
		return true;
	}
	if (malloc_mutex_init(&prof_gdump_mtx, "prof_gdump",
	    WITNESS_RANK_PROF_GDUMP, malloc_mutex_rank_exclusive)) {
		return true;
	}
	if (malloc_mutex_init(&prof_thread_active_init_mtx,
	    "prof_thread_active_init", WITNESS_RANK_PROF_THREAD_ACTIVE_INIT,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}
	if (malloc_mutex_init(&bt2gctx_mtx, "prof_bt2gctx",
	    WITNESS_RANK_PROF_BT2GCTX, malloc_mutex_rank_exclusive)) {
		return true;
	}
	if (malloc_mutex_init(&tdatas_mtx, "prof_tdatas",
	    WITNESS_RANK_PROF_TDATAS, malloc_mutex_rank_exclusive)) {
		return true;
	}
	if (malloc_mutex_init(&next_thr_uid_mtx, "prof_next_thr_uid",
	    WITNESS_RANK_PROF_NEXT_THR_UID, malloc_mutex_rank_exclusive)) {
		return true;
	}
	if (malloc_mutex_init(&prof_stats_mtx, "prof_stats",
	    WITNESS_RANK_PROF_STATS, malloc_mutex_rank_exclusive)) {
		return true;
	}
	if (malloc_mutex_init(&prof_dump_filename_mtx,
	    "prof_dump_filename", WITNESS_RANK_PROF_DUMP_FILENAME,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}
	if (malloc_mutex_init(&prof_dump_mtx, "prof_dump",
	    WITNESS_RANK_PROF_DUMP, malloc_mutex_rank_exclusive)) {
		return true;
	}

	if (opt_prof) {
		lg_prof_sample = opt_lg_prof_sample;
		prof_unbias_map_init();
		prof_active_state = opt_prof_active;
		prof_gdump_val = opt_prof_gdump;
		prof_thread_active_init = opt_prof_thread_active_init;

		if (prof_data_init(tsd)) {
			return true;
		}

		next_thr_uid = 0;
		if (prof_idump_accum_init()) {
			return true;
		}

		if (opt_prof_final && opt_prof_prefix[0] != '\0' &&
		    atexit(prof_fdump) != 0) {
			malloc_write("<jemalloc>: Error in atexit()\n");
			if (opt_abort) {
				abort();
			}
		}

		if (prof_log_init(tsd)) {
			return true;
		}

		if (prof_recent_init()) {
			return true;
		}

		prof_base = base;

		gctx_locks = (malloc_mutex_t *)base_alloc(tsd_tsdn(tsd), base,
		    PROF_NCTX_LOCKS * sizeof(malloc_mutex_t), CACHELINE);
		if (gctx_locks == NULL) {
			return true;
		}
		for (unsigned i = 0; i < PROF_NCTX_LOCKS; i++) {
			if (malloc_mutex_init(&gctx_locks[i], "prof_gctx",
			    WITNESS_RANK_PROF_GCTX,
			    malloc_mutex_rank_exclusive)) {
				return true;
			}
		}

		tdata_locks = (malloc_mutex_t *)base_alloc(tsd_tsdn(tsd), base,
		    PROF_NTDATA_LOCKS * sizeof(malloc_mutex_t), CACHELINE);
		if (tdata_locks == NULL) {
			return true;
		}
		for (unsigned i = 0; i < PROF_NTDATA_LOCKS; i++) {
			if (malloc_mutex_init(&tdata_locks[i], "prof_tdata",
			    WITNESS_RANK_PROF_TDATA,
			    malloc_mutex_rank_exclusive)) {
				return true;
			}
		}

		prof_unwind_init();
		prof_hooks_init();
	}
	prof_booted = true;

	return false;
}

void
prof_prefork0(tsdn_t *tsdn) {
	if (config_prof && opt_prof) {
		unsigned i;

		malloc_mutex_prefork(tsdn, &prof_dump_mtx);
		malloc_mutex_prefork(tsdn, &bt2gctx_mtx);
		malloc_mutex_prefork(tsdn, &tdatas_mtx);
		for (i = 0; i < PROF_NTDATA_LOCKS; i++) {
			malloc_mutex_prefork(tsdn, &tdata_locks[i]);
		}
		malloc_mutex_prefork(tsdn, &log_mtx);
		for (i = 0; i < PROF_NCTX_LOCKS; i++) {
			malloc_mutex_prefork(tsdn, &gctx_locks[i]);
		}
		malloc_mutex_prefork(tsdn, &prof_recent_dump_mtx);
	}
}

void
prof_prefork1(tsdn_t *tsdn) {
	if (config_prof && opt_prof) {
		counter_prefork(tsdn, &prof_idump_accumulated);
		malloc_mutex_prefork(tsdn, &prof_active_mtx);
		malloc_mutex_prefork(tsdn, &prof_dump_filename_mtx);
		malloc_mutex_prefork(tsdn, &prof_gdump_mtx);
		malloc_mutex_prefork(tsdn, &prof_recent_alloc_mtx);
		malloc_mutex_prefork(tsdn, &prof_stats_mtx);
		malloc_mutex_prefork(tsdn, &next_thr_uid_mtx);
		malloc_mutex_prefork(tsdn, &prof_thread_active_init_mtx);
	}
}

void
prof_postfork_parent(tsdn_t *tsdn) {
	if (config_prof && opt_prof) {
		unsigned i;

		malloc_mutex_postfork_parent(tsdn,
		    &prof_thread_active_init_mtx);
		malloc_mutex_postfork_parent(tsdn, &next_thr_uid_mtx);
		malloc_mutex_postfork_parent(tsdn, &prof_stats_mtx);
		malloc_mutex_postfork_parent(tsdn, &prof_recent_alloc_mtx);
		malloc_mutex_postfork_parent(tsdn, &prof_gdump_mtx);
		malloc_mutex_postfork_parent(tsdn, &prof_dump_filename_mtx);
		malloc_mutex_postfork_parent(tsdn, &prof_active_mtx);
		counter_postfork_parent(tsdn, &prof_idump_accumulated);
		malloc_mutex_postfork_parent(tsdn, &prof_recent_dump_mtx);
		for (i = 0; i < PROF_NCTX_LOCKS; i++) {
			malloc_mutex_postfork_parent(tsdn, &gctx_locks[i]);
		}
		malloc_mutex_postfork_parent(tsdn, &log_mtx);
		for (i = 0; i < PROF_NTDATA_LOCKS; i++) {
			malloc_mutex_postfork_parent(tsdn, &tdata_locks[i]);
		}
		malloc_mutex_postfork_parent(tsdn, &tdatas_mtx);
		malloc_mutex_postfork_parent(tsdn, &bt2gctx_mtx);
		malloc_mutex_postfork_parent(tsdn, &prof_dump_mtx);
	}
}

void
prof_postfork_child(tsdn_t *tsdn) {
	if (config_prof && opt_prof) {
		unsigned i;

		malloc_mutex_postfork_child(tsdn, &prof_thread_active_init_mtx);
		malloc_mutex_postfork_child(tsdn, &next_thr_uid_mtx);
		malloc_mutex_postfork_child(tsdn, &prof_stats_mtx);
		malloc_mutex_postfork_child(tsdn, &prof_recent_alloc_mtx);
		malloc_mutex_postfork_child(tsdn, &prof_gdump_mtx);
		malloc_mutex_postfork_child(tsdn, &prof_dump_filename_mtx);
		malloc_mutex_postfork_child(tsdn, &prof_active_mtx);
		counter_postfork_child(tsdn, &prof_idump_accumulated);
		malloc_mutex_postfork_child(tsdn, &prof_recent_dump_mtx);
		for (i = 0; i < PROF_NCTX_LOCKS; i++) {
			malloc_mutex_postfork_child(tsdn, &gctx_locks[i]);
		}
		malloc_mutex_postfork_child(tsdn, &log_mtx);
		for (i = 0; i < PROF_NTDATA_LOCKS; i++) {
			malloc_mutex_postfork_child(tsdn, &tdata_locks[i]);
		}
		malloc_mutex_postfork_child(tsdn, &tdatas_mtx);
		malloc_mutex_postfork_child(tsdn, &bt2gctx_mtx);
		malloc_mutex_postfork_child(tsdn, &prof_dump_mtx);
	}
}

/******************************************************************************/

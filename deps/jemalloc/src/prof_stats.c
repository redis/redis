#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/prof_stats.h"

bool opt_prof_stats = false;
malloc_mutex_t prof_stats_mtx;
static prof_stats_t prof_stats_live[PROF_SC_NSIZES];
static prof_stats_t prof_stats_accum[PROF_SC_NSIZES];

static void
prof_stats_enter(tsd_t *tsd, szind_t ind) {
	assert(opt_prof && opt_prof_stats);
	assert(ind < SC_NSIZES);
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_stats_mtx);
}

static void
prof_stats_leave(tsd_t *tsd) {
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_stats_mtx);
}

void
prof_stats_inc(tsd_t *tsd, szind_t ind, size_t size) {
	cassert(config_prof);
	prof_stats_enter(tsd, ind);
	prof_stats_live[ind].req_sum += size;
	prof_stats_live[ind].count++;
	prof_stats_accum[ind].req_sum += size;
	prof_stats_accum[ind].count++;
	prof_stats_leave(tsd);
}

void
prof_stats_dec(tsd_t *tsd, szind_t ind, size_t size) {
	cassert(config_prof);
	prof_stats_enter(tsd, ind);
	prof_stats_live[ind].req_sum -= size;
	prof_stats_live[ind].count--;
	prof_stats_leave(tsd);
}

void
prof_stats_get_live(tsd_t *tsd, szind_t ind, prof_stats_t *stats) {
	cassert(config_prof);
	prof_stats_enter(tsd, ind);
	memcpy(stats, &prof_stats_live[ind], sizeof(prof_stats_t));
	prof_stats_leave(tsd);
}

void
prof_stats_get_accum(tsd_t *tsd, szind_t ind, prof_stats_t *stats) {
	cassert(config_prof);
	prof_stats_enter(tsd, ind);
	memcpy(stats, &prof_stats_accum[ind], sizeof(prof_stats_t));
	prof_stats_leave(tsd);
}

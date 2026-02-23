#ifndef JEMALLOC_INTERNAL_PROF_STATS_H
#define JEMALLOC_INTERNAL_PROF_STATS_H

typedef struct prof_stats_s prof_stats_t;
struct prof_stats_s {
	uint64_t req_sum;
	uint64_t count;
};

extern malloc_mutex_t prof_stats_mtx;

void prof_stats_inc(tsd_t *tsd, szind_t ind, size_t size);
void prof_stats_dec(tsd_t *tsd, szind_t ind, size_t size);
void prof_stats_get_live(tsd_t *tsd, szind_t ind, prof_stats_t *stats);
void prof_stats_get_accum(tsd_t *tsd, szind_t ind, prof_stats_t *stats);

#endif /* JEMALLOC_INTERNAL_PROF_STATS_H */

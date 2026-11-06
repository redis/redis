#ifndef JEMALLOC_INTERNAL_TCACHE_EXTERNS_H
#define JEMALLOC_INTERNAL_TCACHE_EXTERNS_H

extern bool opt_tcache;
extern size_t opt_tcache_max;
extern ssize_t	opt_lg_tcache_nslots_mul;
extern unsigned opt_tcache_nslots_small_min;
extern unsigned opt_tcache_nslots_small_max;
extern unsigned opt_tcache_nslots_large;
extern ssize_t opt_lg_tcache_shift;
extern size_t opt_tcache_gc_incr_bytes;
extern size_t opt_tcache_gc_delay_bytes;
extern unsigned opt_lg_tcache_flush_small_div;
extern unsigned opt_lg_tcache_flush_large_div;

/*
 * Number of tcache bins.  There are SC_NBINS small-object bins, plus 0 or more
 * large-object bins.
 */
extern unsigned	nhbins;

/* Maximum cached size class. */
extern size_t	tcache_maxclass;

extern cache_bin_info_t *tcache_bin_info;

/*
 * Explicit tcaches, managed via the tcache.{create,flush,destroy} mallctls and
 * usable via the MALLOCX_TCACHE() flag.  The automatic per thread tcaches are
 * completely disjoint from this data structure.  tcaches starts off as a sparse
 * array, so it has no physical memory footprint until individual pages are
 * touched.  This allows the entire array to be allocated the first time an
 * explicit tcache is created without a disproportionate impact on memory usage.
 */
extern tcaches_t	*tcaches;

size_t tcache_salloc(tsdn_t *tsdn, const void *ptr);
void *tcache_alloc_small_hard(tsdn_t *tsdn, arena_t *arena, tcache_t *tcache,
    cache_bin_t *tbin, szind_t binind, bool *tcache_success);

void tcache_bin_flush_small(tsd_t *tsd, tcache_t *tcache, cache_bin_t *tbin,
    szind_t binind, unsigned rem);
void tcache_bin_flush_large(tsd_t *tsd, tcache_t *tcache, cache_bin_t *tbin,
    szind_t binind, unsigned rem);
void tcache_bin_flush_stashed(tsd_t *tsd, tcache_t *tcache, cache_bin_t *bin,
    szind_t binind, bool is_small);
void tcache_arena_reassociate(tsdn_t *tsdn, tcache_slow_t *tcache_slow,
    tcache_t *tcache, arena_t *arena);
tcache_t *tcache_create_explicit(tsd_t *tsd);
void tcache_cleanup(tsd_t *tsd);
void tcache_stats_merge(tsdn_t *tsdn, tcache_t *tcache, arena_t *arena);
bool tcaches_create(tsd_t *tsd, base_t *base, unsigned *r_ind);
void tcaches_flush(tsd_t *tsd, unsigned ind);
void tcaches_destroy(tsd_t *tsd, unsigned ind);
bool tcache_boot(tsdn_t *tsdn, base_t *base);
void tcache_arena_associate(tsdn_t *tsdn, tcache_slow_t *tcache_slow,
    tcache_t *tcache, arena_t *arena);
void tcache_prefork(tsdn_t *tsdn);
void tcache_postfork_parent(tsdn_t *tsdn);
void tcache_postfork_child(tsdn_t *tsdn);
void tcache_flush(tsd_t *tsd);
bool tsd_tcache_data_init(tsd_t *tsd);
bool tsd_tcache_enabled_data_init(tsd_t *tsd);

void tcache_assert_initialized(tcache_t *tcache);

/* Only accessed by thread event. */
uint64_t tcache_gc_new_event_wait(tsd_t *tsd);
uint64_t tcache_gc_postponed_event_wait(tsd_t *tsd);
void tcache_gc_event_handler(tsd_t *tsd, uint64_t elapsed);
uint64_t tcache_gc_dalloc_new_event_wait(tsd_t *tsd);
uint64_t tcache_gc_dalloc_postponed_event_wait(tsd_t *tsd);
void tcache_gc_dalloc_event_handler(tsd_t *tsd, uint64_t elapsed);

#endif /* JEMALLOC_INTERNAL_TCACHE_EXTERNS_H */

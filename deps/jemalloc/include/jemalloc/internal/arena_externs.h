#ifndef JEMALLOC_INTERNAL_ARENA_EXTERNS_H
#define JEMALLOC_INTERNAL_ARENA_EXTERNS_H

#include "jemalloc/internal/bin.h"
#include "jemalloc/internal/div.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/hook.h"
#include "jemalloc/internal/pages.h"
#include "jemalloc/internal/stats.h"

/*
 * When the amount of pages to be purged exceeds this amount, deferred purge
 * should happen.
 */
#define ARENA_DEFERRED_PURGE_NPAGES_THRESHOLD UINT64_C(1024)

extern ssize_t opt_dirty_decay_ms;
extern ssize_t opt_muzzy_decay_ms;

extern percpu_arena_mode_t opt_percpu_arena;
extern const char *percpu_arena_mode_names[];

extern div_info_t arena_binind_div_info[SC_NBINS];

extern malloc_mutex_t arenas_lock;
extern emap_t arena_emap_global;

extern size_t opt_oversize_threshold;
extern size_t oversize_threshold;

/*
 * arena_bin_offsets[binind] is the offset of the first bin shard for size class
 * binind.
 */
extern uint32_t arena_bin_offsets[SC_NBINS];

void arena_basic_stats_merge(tsdn_t *tsdn, arena_t *arena,
    unsigned *nthreads, const char **dss, ssize_t *dirty_decay_ms,
    ssize_t *muzzy_decay_ms, size_t *nactive, size_t *ndirty, size_t *nmuzzy);
void arena_stats_merge(tsdn_t *tsdn, arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *dirty_decay_ms, ssize_t *muzzy_decay_ms,
    size_t *nactive, size_t *ndirty, size_t *nmuzzy, arena_stats_t *astats,
    bin_stats_data_t *bstats, arena_stats_large_t *lstats,
    pac_estats_t *estats, hpa_shard_stats_t *hpastats, sec_stats_t *secstats);
void arena_handle_deferred_work(tsdn_t *tsdn, arena_t *arena);
edata_t *arena_extent_alloc_large(tsdn_t *tsdn, arena_t *arena,
    size_t usize, size_t alignment, bool zero);
void arena_extent_dalloc_large_prep(tsdn_t *tsdn, arena_t *arena,
    edata_t *edata);
void arena_extent_ralloc_large_shrink(tsdn_t *tsdn, arena_t *arena,
    edata_t *edata, size_t oldsize);
void arena_extent_ralloc_large_expand(tsdn_t *tsdn, arena_t *arena,
    edata_t *edata, size_t oldsize);
bool arena_decay_ms_set(tsdn_t *tsdn, arena_t *arena, extent_state_t state,
    ssize_t decay_ms);
ssize_t arena_decay_ms_get(arena_t *arena, extent_state_t state);
void arena_decay(tsdn_t *tsdn, arena_t *arena, bool is_background_thread,
    bool all);
uint64_t arena_time_until_deferred(tsdn_t *tsdn, arena_t *arena);
void arena_do_deferred_work(tsdn_t *tsdn, arena_t *arena);
void arena_reset(tsd_t *tsd, arena_t *arena);
void arena_destroy(tsd_t *tsd, arena_t *arena);
void arena_cache_bin_fill_small(tsdn_t *tsdn, arena_t *arena,
    cache_bin_t *cache_bin, cache_bin_info_t *cache_bin_info, szind_t binind,
    const unsigned nfill);

void *arena_malloc_hard(tsdn_t *tsdn, arena_t *arena, size_t size,
    szind_t ind, bool zero);
void *arena_palloc(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool zero, tcache_t *tcache);
void arena_prof_promote(tsdn_t *tsdn, void *ptr, size_t usize);
void arena_dalloc_promoted(tsdn_t *tsdn, void *ptr, tcache_t *tcache,
    bool slow_path);
void arena_slab_dalloc(tsdn_t *tsdn, arena_t *arena, edata_t *slab);

void arena_dalloc_bin_locked_handle_newly_empty(tsdn_t *tsdn, arena_t *arena,
    edata_t *slab, bin_t *bin);
void arena_dalloc_bin_locked_handle_newly_nonempty(tsdn_t *tsdn, arena_t *arena,
    edata_t *slab, bin_t *bin);
void arena_dalloc_small(tsdn_t *tsdn, void *ptr);
bool arena_ralloc_no_move(tsdn_t *tsdn, void *ptr, size_t oldsize, size_t size,
    size_t extra, bool zero, size_t *newsize);
void *arena_ralloc(tsdn_t *tsdn, arena_t *arena, void *ptr, size_t oldsize,
    size_t size, size_t alignment, bool zero, tcache_t *tcache,
    hook_ralloc_args_t *hook_args);
dss_prec_t arena_dss_prec_get(arena_t *arena);
ehooks_t *arena_get_ehooks(arena_t *arena);
extent_hooks_t *arena_set_extent_hooks(tsd_t *tsd, arena_t *arena,
    extent_hooks_t *extent_hooks);
bool arena_dss_prec_set(arena_t *arena, dss_prec_t dss_prec);
ssize_t arena_dirty_decay_ms_default_get(void);
bool arena_dirty_decay_ms_default_set(ssize_t decay_ms);
ssize_t arena_muzzy_decay_ms_default_get(void);
bool arena_muzzy_decay_ms_default_set(ssize_t decay_ms);
bool arena_retain_grow_limit_get_set(tsd_t *tsd, arena_t *arena,
    size_t *old_limit, size_t *new_limit);
unsigned arena_nthreads_get(arena_t *arena, bool internal);
void arena_nthreads_inc(arena_t *arena, bool internal);
void arena_nthreads_dec(arena_t *arena, bool internal);
arena_t *arena_new(tsdn_t *tsdn, unsigned ind, const arena_config_t *config);
bool arena_init_huge(void);
bool arena_is_huge(unsigned arena_ind);
arena_t *arena_choose_huge(tsd_t *tsd);
bin_t *arena_bin_choose(tsdn_t *tsdn, arena_t *arena, szind_t binind,
    unsigned *binshard);
size_t arena_fill_small_fresh(tsdn_t *tsdn, arena_t *arena, szind_t binind,
    void **ptrs, size_t nfill, bool zero);
bool arena_boot(sc_data_t *sc_data, base_t *base, bool hpa);
void arena_prefork0(tsdn_t *tsdn, arena_t *arena);
void arena_prefork1(tsdn_t *tsdn, arena_t *arena);
void arena_prefork2(tsdn_t *tsdn, arena_t *arena);
void arena_prefork3(tsdn_t *tsdn, arena_t *arena);
void arena_prefork4(tsdn_t *tsdn, arena_t *arena);
void arena_prefork5(tsdn_t *tsdn, arena_t *arena);
void arena_prefork6(tsdn_t *tsdn, arena_t *arena);
void arena_prefork7(tsdn_t *tsdn, arena_t *arena);
void arena_prefork8(tsdn_t *tsdn, arena_t *arena);
void arena_postfork_parent(tsdn_t *tsdn, arena_t *arena);
void arena_postfork_child(tsdn_t *tsdn, arena_t *arena);

#endif /* JEMALLOC_INTERNAL_ARENA_EXTERNS_H */

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/inspect.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/nstime.h"
#include "jemalloc/internal/peak_event.h"
#include "jemalloc/internal/prof_data.h"
#include "jemalloc/internal/prof_log.h"
#include "jemalloc/internal/prof_recent.h"
#include "jemalloc/internal/prof_stats.h"
#include "jemalloc/internal/prof_sys.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/util.h"

/******************************************************************************/
/* Data. */

/*
 * ctl_mtx protects the following:
 * - ctl_stats->*
 */
static malloc_mutex_t	ctl_mtx;
static bool		ctl_initialized;
static ctl_stats_t	*ctl_stats;
static ctl_arenas_t	*ctl_arenas;

/******************************************************************************/
/* Helpers for named and indexed nodes. */

static const ctl_named_node_t *
ctl_named_node(const ctl_node_t *node) {
	return ((node->named) ? (const ctl_named_node_t *)node : NULL);
}

static const ctl_named_node_t *
ctl_named_children(const ctl_named_node_t *node, size_t index) {
	const ctl_named_node_t *children = ctl_named_node(node->children);

	return (children ? &children[index] : NULL);
}

static const ctl_indexed_node_t *
ctl_indexed_node(const ctl_node_t *node) {
	return (!node->named ? (const ctl_indexed_node_t *)node : NULL);
}

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

#define CTL_PROTO(n)							\
static int	n##_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,	\
    void *oldp, size_t *oldlenp, void *newp, size_t newlen);

#define INDEX_PROTO(n)							\
static const ctl_named_node_t	*n##_index(tsdn_t *tsdn,		\
    const size_t *mib, size_t miblen, size_t i);

CTL_PROTO(version)
CTL_PROTO(epoch)
CTL_PROTO(background_thread)
CTL_PROTO(max_background_threads)
CTL_PROTO(thread_tcache_enabled)
CTL_PROTO(thread_tcache_flush)
CTL_PROTO(thread_peak_read)
CTL_PROTO(thread_peak_reset)
CTL_PROTO(thread_prof_name)
CTL_PROTO(thread_prof_active)
CTL_PROTO(thread_arena)
CTL_PROTO(thread_allocated)
CTL_PROTO(thread_allocatedp)
CTL_PROTO(thread_deallocated)
CTL_PROTO(thread_deallocatedp)
CTL_PROTO(thread_idle)
CTL_PROTO(config_cache_oblivious)
CTL_PROTO(config_debug)
CTL_PROTO(config_fill)
CTL_PROTO(config_lazy_lock)
CTL_PROTO(config_malloc_conf)
CTL_PROTO(config_opt_safety_checks)
CTL_PROTO(config_prof)
CTL_PROTO(config_prof_libgcc)
CTL_PROTO(config_prof_libunwind)
CTL_PROTO(config_stats)
CTL_PROTO(config_utrace)
CTL_PROTO(config_xmalloc)
CTL_PROTO(opt_abort)
CTL_PROTO(opt_abort_conf)
CTL_PROTO(opt_cache_oblivious)
CTL_PROTO(opt_trust_madvise)
CTL_PROTO(opt_confirm_conf)
CTL_PROTO(opt_hpa)
CTL_PROTO(opt_hpa_slab_max_alloc)
CTL_PROTO(opt_hpa_hugification_threshold)
CTL_PROTO(opt_hpa_hugify_delay_ms)
CTL_PROTO(opt_hpa_min_purge_interval_ms)
CTL_PROTO(opt_hpa_dirty_mult)
CTL_PROTO(opt_hpa_sec_nshards)
CTL_PROTO(opt_hpa_sec_max_alloc)
CTL_PROTO(opt_hpa_sec_max_bytes)
CTL_PROTO(opt_hpa_sec_bytes_after_flush)
CTL_PROTO(opt_hpa_sec_batch_fill_extra)
CTL_PROTO(opt_metadata_thp)
CTL_PROTO(opt_retain)
CTL_PROTO(opt_dss)
CTL_PROTO(opt_narenas)
CTL_PROTO(opt_percpu_arena)
CTL_PROTO(opt_oversize_threshold)
CTL_PROTO(opt_background_thread)
CTL_PROTO(opt_mutex_max_spin)
CTL_PROTO(opt_max_background_threads)
CTL_PROTO(opt_dirty_decay_ms)
CTL_PROTO(opt_muzzy_decay_ms)
CTL_PROTO(opt_stats_print)
CTL_PROTO(opt_stats_print_opts)
CTL_PROTO(opt_stats_interval)
CTL_PROTO(opt_stats_interval_opts)
CTL_PROTO(opt_junk)
CTL_PROTO(opt_zero)
CTL_PROTO(opt_utrace)
CTL_PROTO(opt_xmalloc)
CTL_PROTO(opt_experimental_infallible_new)
CTL_PROTO(opt_tcache)
CTL_PROTO(opt_tcache_max)
CTL_PROTO(opt_tcache_nslots_small_min)
CTL_PROTO(opt_tcache_nslots_small_max)
CTL_PROTO(opt_tcache_nslots_large)
CTL_PROTO(opt_lg_tcache_nslots_mul)
CTL_PROTO(opt_tcache_gc_incr_bytes)
CTL_PROTO(opt_tcache_gc_delay_bytes)
CTL_PROTO(opt_lg_tcache_flush_small_div)
CTL_PROTO(opt_lg_tcache_flush_large_div)
CTL_PROTO(opt_thp)
CTL_PROTO(opt_lg_extent_max_active_fit)
CTL_PROTO(opt_prof)
CTL_PROTO(opt_prof_prefix)
CTL_PROTO(opt_prof_active)
CTL_PROTO(opt_prof_thread_active_init)
CTL_PROTO(opt_lg_prof_sample)
CTL_PROTO(opt_lg_prof_interval)
CTL_PROTO(opt_prof_gdump)
CTL_PROTO(opt_prof_final)
CTL_PROTO(opt_prof_leak)
CTL_PROTO(opt_prof_leak_error)
CTL_PROTO(opt_prof_accum)
CTL_PROTO(opt_prof_recent_alloc_max)
CTL_PROTO(opt_prof_stats)
CTL_PROTO(opt_prof_sys_thread_name)
CTL_PROTO(opt_prof_time_res)
CTL_PROTO(opt_lg_san_uaf_align)
CTL_PROTO(opt_zero_realloc)
CTL_PROTO(tcache_create)
CTL_PROTO(tcache_flush)
CTL_PROTO(tcache_destroy)
CTL_PROTO(arena_i_initialized)
CTL_PROTO(arena_i_decay)
CTL_PROTO(arena_i_purge)
CTL_PROTO(arena_i_reset)
CTL_PROTO(arena_i_destroy)
CTL_PROTO(arena_i_dss)
CTL_PROTO(arena_i_oversize_threshold)
CTL_PROTO(arena_i_dirty_decay_ms)
CTL_PROTO(arena_i_muzzy_decay_ms)
CTL_PROTO(arena_i_extent_hooks)
CTL_PROTO(arena_i_retain_grow_limit)
INDEX_PROTO(arena_i)
CTL_PROTO(arenas_bin_i_size)
CTL_PROTO(arenas_bin_i_nregs)
CTL_PROTO(arenas_bin_i_slab_size)
CTL_PROTO(arenas_bin_i_nshards)
INDEX_PROTO(arenas_bin_i)
CTL_PROTO(arenas_lextent_i_size)
INDEX_PROTO(arenas_lextent_i)
CTL_PROTO(arenas_narenas)
CTL_PROTO(arenas_dirty_decay_ms)
CTL_PROTO(arenas_muzzy_decay_ms)
CTL_PROTO(arenas_quantum)
CTL_PROTO(arenas_page)
CTL_PROTO(arenas_tcache_max)
CTL_PROTO(arenas_nbins)
CTL_PROTO(arenas_nhbins)
CTL_PROTO(arenas_nlextents)
CTL_PROTO(arenas_create)
CTL_PROTO(arenas_lookup)
CTL_PROTO(prof_thread_active_init)
CTL_PROTO(prof_active)
CTL_PROTO(prof_dump)
CTL_PROTO(prof_gdump)
CTL_PROTO(prof_prefix)
CTL_PROTO(prof_reset)
CTL_PROTO(prof_interval)
CTL_PROTO(lg_prof_sample)
CTL_PROTO(prof_log_start)
CTL_PROTO(prof_log_stop)
CTL_PROTO(prof_stats_bins_i_live)
CTL_PROTO(prof_stats_bins_i_accum)
INDEX_PROTO(prof_stats_bins_i)
CTL_PROTO(prof_stats_lextents_i_live)
CTL_PROTO(prof_stats_lextents_i_accum)
INDEX_PROTO(prof_stats_lextents_i)
CTL_PROTO(stats_arenas_i_small_allocated)
CTL_PROTO(stats_arenas_i_small_nmalloc)
CTL_PROTO(stats_arenas_i_small_ndalloc)
CTL_PROTO(stats_arenas_i_small_nrequests)
CTL_PROTO(stats_arenas_i_small_nfills)
CTL_PROTO(stats_arenas_i_small_nflushes)
CTL_PROTO(stats_arenas_i_large_allocated)
CTL_PROTO(stats_arenas_i_large_nmalloc)
CTL_PROTO(stats_arenas_i_large_ndalloc)
CTL_PROTO(stats_arenas_i_large_nrequests)
CTL_PROTO(stats_arenas_i_large_nfills)
CTL_PROTO(stats_arenas_i_large_nflushes)
CTL_PROTO(stats_arenas_i_bins_j_nmalloc)
CTL_PROTO(stats_arenas_i_bins_j_ndalloc)
CTL_PROTO(stats_arenas_i_bins_j_nrequests)
CTL_PROTO(stats_arenas_i_bins_j_curregs)
CTL_PROTO(stats_arenas_i_bins_j_nfills)
CTL_PROTO(stats_arenas_i_bins_j_nflushes)
CTL_PROTO(stats_arenas_i_bins_j_nslabs)
CTL_PROTO(stats_arenas_i_bins_j_nreslabs)
CTL_PROTO(stats_arenas_i_bins_j_curslabs)
CTL_PROTO(stats_arenas_i_bins_j_nonfull_slabs)
INDEX_PROTO(stats_arenas_i_bins_j)
CTL_PROTO(stats_arenas_i_lextents_j_nmalloc)
CTL_PROTO(stats_arenas_i_lextents_j_ndalloc)
CTL_PROTO(stats_arenas_i_lextents_j_nrequests)
CTL_PROTO(stats_arenas_i_lextents_j_curlextents)
INDEX_PROTO(stats_arenas_i_lextents_j)
CTL_PROTO(stats_arenas_i_extents_j_ndirty)
CTL_PROTO(stats_arenas_i_extents_j_nmuzzy)
CTL_PROTO(stats_arenas_i_extents_j_nretained)
CTL_PROTO(stats_arenas_i_extents_j_dirty_bytes)
CTL_PROTO(stats_arenas_i_extents_j_muzzy_bytes)
CTL_PROTO(stats_arenas_i_extents_j_retained_bytes)
INDEX_PROTO(stats_arenas_i_extents_j)
CTL_PROTO(stats_arenas_i_hpa_shard_npurge_passes)
CTL_PROTO(stats_arenas_i_hpa_shard_npurges)
CTL_PROTO(stats_arenas_i_hpa_shard_nhugifies)
CTL_PROTO(stats_arenas_i_hpa_shard_ndehugifies)

/* We have a set of stats for full slabs. */
CTL_PROTO(stats_arenas_i_hpa_shard_full_slabs_npageslabs_nonhuge)
CTL_PROTO(stats_arenas_i_hpa_shard_full_slabs_npageslabs_huge)
CTL_PROTO(stats_arenas_i_hpa_shard_full_slabs_nactive_nonhuge)
CTL_PROTO(stats_arenas_i_hpa_shard_full_slabs_nactive_huge)
CTL_PROTO(stats_arenas_i_hpa_shard_full_slabs_ndirty_nonhuge)
CTL_PROTO(stats_arenas_i_hpa_shard_full_slabs_ndirty_huge)

/* A parallel set for the empty slabs. */
CTL_PROTO(stats_arenas_i_hpa_shard_empty_slabs_npageslabs_nonhuge)
CTL_PROTO(stats_arenas_i_hpa_shard_empty_slabs_npageslabs_huge)
CTL_PROTO(stats_arenas_i_hpa_shard_empty_slabs_nactive_nonhuge)
CTL_PROTO(stats_arenas_i_hpa_shard_empty_slabs_nactive_huge)
CTL_PROTO(stats_arenas_i_hpa_shard_empty_slabs_ndirty_nonhuge)
CTL_PROTO(stats_arenas_i_hpa_shard_empty_slabs_ndirty_huge)

/*
 * And one for the slabs that are neither empty nor full, but indexed by how
 * full they are.
 */
CTL_PROTO(stats_arenas_i_hpa_shard_nonfull_slabs_j_npageslabs_nonhuge)
CTL_PROTO(stats_arenas_i_hpa_shard_nonfull_slabs_j_npageslabs_huge)
CTL_PROTO(stats_arenas_i_hpa_shard_nonfull_slabs_j_nactive_nonhuge)
CTL_PROTO(stats_arenas_i_hpa_shard_nonfull_slabs_j_nactive_huge)
CTL_PROTO(stats_arenas_i_hpa_shard_nonfull_slabs_j_ndirty_nonhuge)
CTL_PROTO(stats_arenas_i_hpa_shard_nonfull_slabs_j_ndirty_huge)

INDEX_PROTO(stats_arenas_i_hpa_shard_nonfull_slabs_j)
CTL_PROTO(stats_arenas_i_nthreads)
CTL_PROTO(stats_arenas_i_uptime)
CTL_PROTO(stats_arenas_i_dss)
CTL_PROTO(stats_arenas_i_dirty_decay_ms)
CTL_PROTO(stats_arenas_i_muzzy_decay_ms)
CTL_PROTO(stats_arenas_i_pactive)
CTL_PROTO(stats_arenas_i_pdirty)
CTL_PROTO(stats_arenas_i_pmuzzy)
CTL_PROTO(stats_arenas_i_mapped)
CTL_PROTO(stats_arenas_i_retained)
CTL_PROTO(stats_arenas_i_extent_avail)
CTL_PROTO(stats_arenas_i_dirty_npurge)
CTL_PROTO(stats_arenas_i_dirty_nmadvise)
CTL_PROTO(stats_arenas_i_dirty_purged)
CTL_PROTO(stats_arenas_i_muzzy_npurge)
CTL_PROTO(stats_arenas_i_muzzy_nmadvise)
CTL_PROTO(stats_arenas_i_muzzy_purged)
CTL_PROTO(stats_arenas_i_base)
CTL_PROTO(stats_arenas_i_internal)
CTL_PROTO(stats_arenas_i_metadata_thp)
CTL_PROTO(stats_arenas_i_tcache_bytes)
CTL_PROTO(stats_arenas_i_tcache_stashed_bytes)
CTL_PROTO(stats_arenas_i_resident)
CTL_PROTO(stats_arenas_i_abandoned_vm)
CTL_PROTO(stats_arenas_i_hpa_sec_bytes)
INDEX_PROTO(stats_arenas_i)
CTL_PROTO(stats_allocated)
CTL_PROTO(stats_active)
CTL_PROTO(stats_background_thread_num_threads)
CTL_PROTO(stats_background_thread_num_runs)
CTL_PROTO(stats_background_thread_run_interval)
CTL_PROTO(stats_metadata)
CTL_PROTO(stats_metadata_thp)
CTL_PROTO(stats_resident)
CTL_PROTO(stats_mapped)
CTL_PROTO(stats_retained)
CTL_PROTO(stats_zero_reallocs)
CTL_PROTO(experimental_hooks_install)
CTL_PROTO(experimental_hooks_remove)
CTL_PROTO(experimental_hooks_prof_backtrace)
CTL_PROTO(experimental_hooks_prof_dump)
CTL_PROTO(experimental_hooks_safety_check_abort)
CTL_PROTO(experimental_thread_activity_callback)
CTL_PROTO(experimental_utilization_query)
CTL_PROTO(experimental_utilization_batch_query)
CTL_PROTO(experimental_arenas_i_pactivep)
INDEX_PROTO(experimental_arenas_i)
CTL_PROTO(experimental_prof_recent_alloc_max)
CTL_PROTO(experimental_prof_recent_alloc_dump)
CTL_PROTO(experimental_batch_alloc)
CTL_PROTO(experimental_arenas_create_ext)

#define MUTEX_STATS_CTL_PROTO_GEN(n)					\
CTL_PROTO(stats_##n##_num_ops)						\
CTL_PROTO(stats_##n##_num_wait)						\
CTL_PROTO(stats_##n##_num_spin_acq)					\
CTL_PROTO(stats_##n##_num_owner_switch)					\
CTL_PROTO(stats_##n##_total_wait_time)					\
CTL_PROTO(stats_##n##_max_wait_time)					\
CTL_PROTO(stats_##n##_max_num_thds)

/* Global mutexes. */
#define OP(mtx) MUTEX_STATS_CTL_PROTO_GEN(mutexes_##mtx)
MUTEX_PROF_GLOBAL_MUTEXES
#undef OP

/* Per arena mutexes. */
#define OP(mtx) MUTEX_STATS_CTL_PROTO_GEN(arenas_i_mutexes_##mtx)
MUTEX_PROF_ARENA_MUTEXES
#undef OP

/* Arena bin mutexes. */
MUTEX_STATS_CTL_PROTO_GEN(arenas_i_bins_j_mutex)
#undef MUTEX_STATS_CTL_PROTO_GEN

CTL_PROTO(stats_mutexes_reset)

/******************************************************************************/
/* mallctl tree. */

#define NAME(n)	{true},	n
#define CHILD(t, c)							\
	sizeof(c##_node) / sizeof(ctl_##t##_node_t),			\
	(ctl_node_t *)c##_node,						\
	NULL
#define CTL(c)	0, NULL, c##_ctl

/*
 * Only handles internal indexed nodes, since there are currently no external
 * ones.
 */
#define INDEX(i)	{false},	i##_index

static const ctl_named_node_t	thread_tcache_node[] = {
	{NAME("enabled"),	CTL(thread_tcache_enabled)},
	{NAME("flush"),		CTL(thread_tcache_flush)}
};

static const ctl_named_node_t	thread_peak_node[] = {
	{NAME("read"),		CTL(thread_peak_read)},
	{NAME("reset"),		CTL(thread_peak_reset)},
};

static const ctl_named_node_t	thread_prof_node[] = {
	{NAME("name"),		CTL(thread_prof_name)},
	{NAME("active"),	CTL(thread_prof_active)}
};

static const ctl_named_node_t	thread_node[] = {
	{NAME("arena"),		CTL(thread_arena)},
	{NAME("allocated"),	CTL(thread_allocated)},
	{NAME("allocatedp"),	CTL(thread_allocatedp)},
	{NAME("deallocated"),	CTL(thread_deallocated)},
	{NAME("deallocatedp"),	CTL(thread_deallocatedp)},
	{NAME("tcache"),	CHILD(named, thread_tcache)},
	{NAME("peak"),		CHILD(named, thread_peak)},
	{NAME("prof"),		CHILD(named, thread_prof)},
	{NAME("idle"),		CTL(thread_idle)}
};

static const ctl_named_node_t	config_node[] = {
	{NAME("cache_oblivious"), CTL(config_cache_oblivious)},
	{NAME("debug"),		CTL(config_debug)},
	{NAME("fill"),		CTL(config_fill)},
	{NAME("lazy_lock"),	CTL(config_lazy_lock)},
	{NAME("malloc_conf"),	CTL(config_malloc_conf)},
	{NAME("opt_safety_checks"),	CTL(config_opt_safety_checks)},
	{NAME("prof"),		CTL(config_prof)},
	{NAME("prof_libgcc"),	CTL(config_prof_libgcc)},
	{NAME("prof_libunwind"), CTL(config_prof_libunwind)},
	{NAME("stats"),		CTL(config_stats)},
	{NAME("utrace"),	CTL(config_utrace)},
	{NAME("xmalloc"),	CTL(config_xmalloc)}
};

static const ctl_named_node_t opt_node[] = {
	{NAME("abort"),		CTL(opt_abort)},
	{NAME("abort_conf"),	CTL(opt_abort_conf)},
	{NAME("cache_oblivious"),	CTL(opt_cache_oblivious)},
	{NAME("trust_madvise"),	CTL(opt_trust_madvise)},
	{NAME("confirm_conf"),	CTL(opt_confirm_conf)},
	{NAME("hpa"),		CTL(opt_hpa)},
	{NAME("hpa_slab_max_alloc"),	CTL(opt_hpa_slab_max_alloc)},
	{NAME("hpa_hugification_threshold"),
		CTL(opt_hpa_hugification_threshold)},
	{NAME("hpa_hugify_delay_ms"), CTL(opt_hpa_hugify_delay_ms)},
	{NAME("hpa_min_purge_interval_ms"), CTL(opt_hpa_min_purge_interval_ms)},
	{NAME("hpa_dirty_mult"), CTL(opt_hpa_dirty_mult)},
	{NAME("hpa_sec_nshards"),	CTL(opt_hpa_sec_nshards)},
	{NAME("hpa_sec_max_alloc"),	CTL(opt_hpa_sec_max_alloc)},
	{NAME("hpa_sec_max_bytes"),	CTL(opt_hpa_sec_max_bytes)},
	{NAME("hpa_sec_bytes_after_flush"),
		CTL(opt_hpa_sec_bytes_after_flush)},
	{NAME("hpa_sec_batch_fill_extra"),
		CTL(opt_hpa_sec_batch_fill_extra)},
	{NAME("metadata_thp"),	CTL(opt_metadata_thp)},
	{NAME("retain"),	CTL(opt_retain)},
	{NAME("dss"),		CTL(opt_dss)},
	{NAME("narenas"),	CTL(opt_narenas)},
	{NAME("percpu_arena"),	CTL(opt_percpu_arena)},
	{NAME("oversize_threshold"),	CTL(opt_oversize_threshold)},
	{NAME("mutex_max_spin"),	CTL(opt_mutex_max_spin)},
	{NAME("background_thread"),	CTL(opt_background_thread)},
	{NAME("max_background_threads"),	CTL(opt_max_background_threads)},
	{NAME("dirty_decay_ms"), CTL(opt_dirty_decay_ms)},
	{NAME("muzzy_decay_ms"), CTL(opt_muzzy_decay_ms)},
	{NAME("stats_print"),	CTL(opt_stats_print)},
	{NAME("stats_print_opts"),	CTL(opt_stats_print_opts)},
	{NAME("stats_interval"),	CTL(opt_stats_interval)},
	{NAME("stats_interval_opts"),	CTL(opt_stats_interval_opts)},
	{NAME("junk"),		CTL(opt_junk)},
	{NAME("zero"),		CTL(opt_zero)},
	{NAME("utrace"),	CTL(opt_utrace)},
	{NAME("xmalloc"),	CTL(opt_xmalloc)},
	{NAME("experimental_infallible_new"),
		CTL(opt_experimental_infallible_new)},
	{NAME("tcache"),	CTL(opt_tcache)},
	{NAME("tcache_max"),	CTL(opt_tcache_max)},
	{NAME("tcache_nslots_small_min"),
		CTL(opt_tcache_nslots_small_min)},
	{NAME("tcache_nslots_small_max"),
		CTL(opt_tcache_nslots_small_max)},
	{NAME("tcache_nslots_large"),	CTL(opt_tcache_nslots_large)},
	{NAME("lg_tcache_nslots_mul"),	CTL(opt_lg_tcache_nslots_mul)},
	{NAME("tcache_gc_incr_bytes"),	CTL(opt_tcache_gc_incr_bytes)},
	{NAME("tcache_gc_delay_bytes"),	CTL(opt_tcache_gc_delay_bytes)},
	{NAME("lg_tcache_flush_small_div"),
		CTL(opt_lg_tcache_flush_small_div)},
	{NAME("lg_tcache_flush_large_div"),
		CTL(opt_lg_tcache_flush_large_div)},
	{NAME("thp"),		CTL(opt_thp)},
	{NAME("lg_extent_max_active_fit"), CTL(opt_lg_extent_max_active_fit)},
	{NAME("prof"),		CTL(opt_prof)},
	{NAME("prof_prefix"),	CTL(opt_prof_prefix)},
	{NAME("prof_active"),	CTL(opt_prof_active)},
	{NAME("prof_thread_active_init"), CTL(opt_prof_thread_active_init)},
	{NAME("lg_prof_sample"), CTL(opt_lg_prof_sample)},
	{NAME("lg_prof_interval"), CTL(opt_lg_prof_interval)},
	{NAME("prof_gdump"),	CTL(opt_prof_gdump)},
	{NAME("prof_final"),	CTL(opt_prof_final)},
	{NAME("prof_leak"),	CTL(opt_prof_leak)},
	{NAME("prof_leak_error"),	CTL(opt_prof_leak_error)},
	{NAME("prof_accum"),	CTL(opt_prof_accum)},
	{NAME("prof_recent_alloc_max"),	CTL(opt_prof_recent_alloc_max)},
	{NAME("prof_stats"),	CTL(opt_prof_stats)},
	{NAME("prof_sys_thread_name"),	CTL(opt_prof_sys_thread_name)},
	{NAME("prof_time_resolution"),	CTL(opt_prof_time_res)},
	{NAME("lg_san_uaf_align"),	CTL(opt_lg_san_uaf_align)},
	{NAME("zero_realloc"),	CTL(opt_zero_realloc)}
};

static const ctl_named_node_t	tcache_node[] = {
	{NAME("create"),	CTL(tcache_create)},
	{NAME("flush"),		CTL(tcache_flush)},
	{NAME("destroy"),	CTL(tcache_destroy)}
};

static const ctl_named_node_t arena_i_node[] = {
	{NAME("initialized"),	CTL(arena_i_initialized)},
	{NAME("decay"),		CTL(arena_i_decay)},
	{NAME("purge"),		CTL(arena_i_purge)},
	{NAME("reset"),		CTL(arena_i_reset)},
	{NAME("destroy"),	CTL(arena_i_destroy)},
	{NAME("dss"),		CTL(arena_i_dss)},
	/*
	 * Undocumented for now, since we anticipate an arena API in flux after
	 * we cut the last 5-series release.
	 */
	{NAME("oversize_threshold"), CTL(arena_i_oversize_threshold)},
	{NAME("dirty_decay_ms"), CTL(arena_i_dirty_decay_ms)},
	{NAME("muzzy_decay_ms"), CTL(arena_i_muzzy_decay_ms)},
	{NAME("extent_hooks"),	CTL(arena_i_extent_hooks)},
	{NAME("retain_grow_limit"),	CTL(arena_i_retain_grow_limit)}
};
static const ctl_named_node_t super_arena_i_node[] = {
	{NAME(""),		CHILD(named, arena_i)}
};

static const ctl_indexed_node_t arena_node[] = {
	{INDEX(arena_i)}
};

static const ctl_named_node_t arenas_bin_i_node[] = {
	{NAME("size"),		CTL(arenas_bin_i_size)},
	{NAME("nregs"),		CTL(arenas_bin_i_nregs)},
	{NAME("slab_size"),	CTL(arenas_bin_i_slab_size)},
	{NAME("nshards"),	CTL(arenas_bin_i_nshards)}
};
static const ctl_named_node_t super_arenas_bin_i_node[] = {
	{NAME(""),		CHILD(named, arenas_bin_i)}
};

static const ctl_indexed_node_t arenas_bin_node[] = {
	{INDEX(arenas_bin_i)}
};

static const ctl_named_node_t arenas_lextent_i_node[] = {
	{NAME("size"),		CTL(arenas_lextent_i_size)}
};
static const ctl_named_node_t super_arenas_lextent_i_node[] = {
	{NAME(""),		CHILD(named, arenas_lextent_i)}
};

static const ctl_indexed_node_t arenas_lextent_node[] = {
	{INDEX(arenas_lextent_i)}
};

static const ctl_named_node_t arenas_node[] = {
	{NAME("narenas"),	CTL(arenas_narenas)},
	{NAME("dirty_decay_ms"), CTL(arenas_dirty_decay_ms)},
	{NAME("muzzy_decay_ms"), CTL(arenas_muzzy_decay_ms)},
	{NAME("quantum"),	CTL(arenas_quantum)},
	{NAME("page"),		CTL(arenas_page)},
	{NAME("tcache_max"),	CTL(arenas_tcache_max)},
	{NAME("nbins"),		CTL(arenas_nbins)},
	{NAME("nhbins"),	CTL(arenas_nhbins)},
	{NAME("bin"),		CHILD(indexed, arenas_bin)},
	{NAME("nlextents"),	CTL(arenas_nlextents)},
	{NAME("lextent"),	CHILD(indexed, arenas_lextent)},
	{NAME("create"),	CTL(arenas_create)},
	{NAME("lookup"),	CTL(arenas_lookup)}
};

static const ctl_named_node_t prof_stats_bins_i_node[] = {
	{NAME("live"),		CTL(prof_stats_bins_i_live)},
	{NAME("accum"),		CTL(prof_stats_bins_i_accum)}
};

static const ctl_named_node_t super_prof_stats_bins_i_node[] = {
	{NAME(""),		CHILD(named, prof_stats_bins_i)}
};

static const ctl_indexed_node_t prof_stats_bins_node[] = {
	{INDEX(prof_stats_bins_i)}
};

static const ctl_named_node_t prof_stats_lextents_i_node[] = {
	{NAME("live"),		CTL(prof_stats_lextents_i_live)},
	{NAME("accum"),		CTL(prof_stats_lextents_i_accum)}
};

static const ctl_named_node_t super_prof_stats_lextents_i_node[] = {
	{NAME(""),		CHILD(named, prof_stats_lextents_i)}
};

static const ctl_indexed_node_t prof_stats_lextents_node[] = {
	{INDEX(prof_stats_lextents_i)}
};

static const ctl_named_node_t	prof_stats_node[] = {
	{NAME("bins"),		CHILD(indexed, prof_stats_bins)},
	{NAME("lextents"),	CHILD(indexed, prof_stats_lextents)},
};

static const ctl_named_node_t	prof_node[] = {
	{NAME("thread_active_init"), CTL(prof_thread_active_init)},
	{NAME("active"),	CTL(prof_active)},
	{NAME("dump"),		CTL(prof_dump)},
	{NAME("gdump"),		CTL(prof_gdump)},
	{NAME("prefix"),	CTL(prof_prefix)},
	{NAME("reset"),		CTL(prof_reset)},
	{NAME("interval"),	CTL(prof_interval)},
	{NAME("lg_sample"),	CTL(lg_prof_sample)},
	{NAME("log_start"),	CTL(prof_log_start)},
	{NAME("log_stop"),	CTL(prof_log_stop)},
	{NAME("stats"),		CHILD(named, prof_stats)}
};

static const ctl_named_node_t stats_arenas_i_small_node[] = {
	{NAME("allocated"),	CTL(stats_arenas_i_small_allocated)},
	{NAME("nmalloc"),	CTL(stats_arenas_i_small_nmalloc)},
	{NAME("ndalloc"),	CTL(stats_arenas_i_small_ndalloc)},
	{NAME("nrequests"),	CTL(stats_arenas_i_small_nrequests)},
	{NAME("nfills"),	CTL(stats_arenas_i_small_nfills)},
	{NAME("nflushes"),	CTL(stats_arenas_i_small_nflushes)}
};

static const ctl_named_node_t stats_arenas_i_large_node[] = {
	{NAME("allocated"),	CTL(stats_arenas_i_large_allocated)},
	{NAME("nmalloc"),	CTL(stats_arenas_i_large_nmalloc)},
	{NAME("ndalloc"),	CTL(stats_arenas_i_large_ndalloc)},
	{NAME("nrequests"),	CTL(stats_arenas_i_large_nrequests)},
	{NAME("nfills"),	CTL(stats_arenas_i_large_nfills)},
	{NAME("nflushes"),	CTL(stats_arenas_i_large_nflushes)}
};

#define MUTEX_PROF_DATA_NODE(prefix)					\
static const ctl_named_node_t stats_##prefix##_node[] = {		\
	{NAME("num_ops"),						\
	 CTL(stats_##prefix##_num_ops)},				\
	{NAME("num_wait"),						\
	 CTL(stats_##prefix##_num_wait)},				\
	{NAME("num_spin_acq"),						\
	 CTL(stats_##prefix##_num_spin_acq)},				\
	{NAME("num_owner_switch"),					\
	 CTL(stats_##prefix##_num_owner_switch)},			\
	{NAME("total_wait_time"),					\
	 CTL(stats_##prefix##_total_wait_time)},			\
	{NAME("max_wait_time"),						\
	 CTL(stats_##prefix##_max_wait_time)},				\
	{NAME("max_num_thds"),						\
	 CTL(stats_##prefix##_max_num_thds)}				\
	/* Note that # of current waiting thread not provided. */	\
};

MUTEX_PROF_DATA_NODE(arenas_i_bins_j_mutex)

static const ctl_named_node_t stats_arenas_i_bins_j_node[] = {
	{NAME("nmalloc"),	CTL(stats_arenas_i_bins_j_nmalloc)},
	{NAME("ndalloc"),	CTL(stats_arenas_i_bins_j_ndalloc)},
	{NAME("nrequests"),	CTL(stats_arenas_i_bins_j_nrequests)},
	{NAME("curregs"),	CTL(stats_arenas_i_bins_j_curregs)},
	{NAME("nfills"),	CTL(stats_arenas_i_bins_j_nfills)},
	{NAME("nflushes"),	CTL(stats_arenas_i_bins_j_nflushes)},
	{NAME("nslabs"),	CTL(stats_arenas_i_bins_j_nslabs)},
	{NAME("nreslabs"),	CTL(stats_arenas_i_bins_j_nreslabs)},
	{NAME("curslabs"),	CTL(stats_arenas_i_bins_j_curslabs)},
	{NAME("nonfull_slabs"),	CTL(stats_arenas_i_bins_j_nonfull_slabs)},
	{NAME("mutex"),		CHILD(named, stats_arenas_i_bins_j_mutex)}
};

static const ctl_named_node_t super_stats_arenas_i_bins_j_node[] = {
	{NAME(""),		CHILD(named, stats_arenas_i_bins_j)}
};

static const ctl_indexed_node_t stats_arenas_i_bins_node[] = {
	{INDEX(stats_arenas_i_bins_j)}
};

static const ctl_named_node_t stats_arenas_i_lextents_j_node[] = {
	{NAME("nmalloc"),	CTL(stats_arenas_i_lextents_j_nmalloc)},
	{NAME("ndalloc"),	CTL(stats_arenas_i_lextents_j_ndalloc)},
	{NAME("nrequests"),	CTL(stats_arenas_i_lextents_j_nrequests)},
	{NAME("curlextents"),	CTL(stats_arenas_i_lextents_j_curlextents)}
};
static const ctl_named_node_t super_stats_arenas_i_lextents_j_node[] = {
	{NAME(""),		CHILD(named, stats_arenas_i_lextents_j)}
};

static const ctl_indexed_node_t stats_arenas_i_lextents_node[] = {
	{INDEX(stats_arenas_i_lextents_j)}
};

static const ctl_named_node_t stats_arenas_i_extents_j_node[] = {
	{NAME("ndirty"),	CTL(stats_arenas_i_extents_j_ndirty)},
	{NAME("nmuzzy"),	CTL(stats_arenas_i_extents_j_nmuzzy)},
	{NAME("nretained"),	CTL(stats_arenas_i_extents_j_nretained)},
	{NAME("dirty_bytes"),	CTL(stats_arenas_i_extents_j_dirty_bytes)},
	{NAME("muzzy_bytes"),	CTL(stats_arenas_i_extents_j_muzzy_bytes)},
	{NAME("retained_bytes"), CTL(stats_arenas_i_extents_j_retained_bytes)}
};

static const ctl_named_node_t super_stats_arenas_i_extents_j_node[] = {
	{NAME(""),		CHILD(named, stats_arenas_i_extents_j)}
};

static const ctl_indexed_node_t stats_arenas_i_extents_node[] = {
	{INDEX(stats_arenas_i_extents_j)}
};

#define OP(mtx)  MUTEX_PROF_DATA_NODE(arenas_i_mutexes_##mtx)
MUTEX_PROF_ARENA_MUTEXES
#undef OP

static const ctl_named_node_t stats_arenas_i_mutexes_node[] = {
#define OP(mtx) {NAME(#mtx), CHILD(named, stats_arenas_i_mutexes_##mtx)},
MUTEX_PROF_ARENA_MUTEXES
#undef OP
};

static const ctl_named_node_t stats_arenas_i_hpa_shard_full_slabs_node[] = {
	{NAME("npageslabs_nonhuge"),
		CTL(stats_arenas_i_hpa_shard_full_slabs_npageslabs_nonhuge)},
	{NAME("npageslabs_huge"),
		CTL(stats_arenas_i_hpa_shard_full_slabs_npageslabs_huge)},
	{NAME("nactive_nonhuge"),
		CTL(stats_arenas_i_hpa_shard_full_slabs_nactive_nonhuge)},
	{NAME("nactive_huge"),
		CTL(stats_arenas_i_hpa_shard_full_slabs_nactive_huge)},
	{NAME("ndirty_nonhuge"),
		CTL(stats_arenas_i_hpa_shard_full_slabs_ndirty_nonhuge)},
	{NAME("ndirty_huge"),
		CTL(stats_arenas_i_hpa_shard_full_slabs_ndirty_huge)}
};

static const ctl_named_node_t stats_arenas_i_hpa_shard_empty_slabs_node[] = {
	{NAME("npageslabs_nonhuge"),
		CTL(stats_arenas_i_hpa_shard_empty_slabs_npageslabs_nonhuge)},
	{NAME("npageslabs_huge"),
		CTL(stats_arenas_i_hpa_shard_empty_slabs_npageslabs_huge)},
	{NAME("nactive_nonhuge"),
		CTL(stats_arenas_i_hpa_shard_empty_slabs_nactive_nonhuge)},
	{NAME("nactive_huge"),
		CTL(stats_arenas_i_hpa_shard_empty_slabs_nactive_huge)},
	{NAME("ndirty_nonhuge"),
		CTL(stats_arenas_i_hpa_shard_empty_slabs_ndirty_nonhuge)},
	{NAME("ndirty_huge"),
		CTL(stats_arenas_i_hpa_shard_empty_slabs_ndirty_huge)}
};

static const ctl_named_node_t stats_arenas_i_hpa_shard_nonfull_slabs_j_node[] = {
	{NAME("npageslabs_nonhuge"),
		CTL(stats_arenas_i_hpa_shard_nonfull_slabs_j_npageslabs_nonhuge)},
	{NAME("npageslabs_huge"),
		CTL(stats_arenas_i_hpa_shard_nonfull_slabs_j_npageslabs_huge)},
	{NAME("nactive_nonhuge"),
		CTL(stats_arenas_i_hpa_shard_nonfull_slabs_j_nactive_nonhuge)},
	{NAME("nactive_huge"),
		CTL(stats_arenas_i_hpa_shard_nonfull_slabs_j_nactive_huge)},
	{NAME("ndirty_nonhuge"),
		CTL(stats_arenas_i_hpa_shard_nonfull_slabs_j_ndirty_nonhuge)},
	{NAME("ndirty_huge"),
		CTL(stats_arenas_i_hpa_shard_nonfull_slabs_j_ndirty_huge)}
};

static const ctl_named_node_t super_stats_arenas_i_hpa_shard_nonfull_slabs_j_node[] = {
	{NAME(""),
		CHILD(named, stats_arenas_i_hpa_shard_nonfull_slabs_j)}
};

static const ctl_indexed_node_t stats_arenas_i_hpa_shard_nonfull_slabs_node[] =
{
	{INDEX(stats_arenas_i_hpa_shard_nonfull_slabs_j)}
};

static const ctl_named_node_t stats_arenas_i_hpa_shard_node[] = {
	{NAME("full_slabs"),	CHILD(named,
	    stats_arenas_i_hpa_shard_full_slabs)},
	{NAME("empty_slabs"),	CHILD(named,
	    stats_arenas_i_hpa_shard_empty_slabs)},
	{NAME("nonfull_slabs"),	CHILD(indexed,
	    stats_arenas_i_hpa_shard_nonfull_slabs)},

	{NAME("npurge_passes"),	CTL(stats_arenas_i_hpa_shard_npurge_passes)},
	{NAME("npurges"),	CTL(stats_arenas_i_hpa_shard_npurges)},
	{NAME("nhugifies"),	CTL(stats_arenas_i_hpa_shard_nhugifies)},
	{NAME("ndehugifies"),	CTL(stats_arenas_i_hpa_shard_ndehugifies)}
};

static const ctl_named_node_t stats_arenas_i_node[] = {
	{NAME("nthreads"),	CTL(stats_arenas_i_nthreads)},
	{NAME("uptime"),	CTL(stats_arenas_i_uptime)},
	{NAME("dss"),		CTL(stats_arenas_i_dss)},
	{NAME("dirty_decay_ms"), CTL(stats_arenas_i_dirty_decay_ms)},
	{NAME("muzzy_decay_ms"), CTL(stats_arenas_i_muzzy_decay_ms)},
	{NAME("pactive"),	CTL(stats_arenas_i_pactive)},
	{NAME("pdirty"),	CTL(stats_arenas_i_pdirty)},
	{NAME("pmuzzy"),	CTL(stats_arenas_i_pmuzzy)},
	{NAME("mapped"),	CTL(stats_arenas_i_mapped)},
	{NAME("retained"),	CTL(stats_arenas_i_retained)},
	{NAME("extent_avail"),	CTL(stats_arenas_i_extent_avail)},
	{NAME("dirty_npurge"),	CTL(stats_arenas_i_dirty_npurge)},
	{NAME("dirty_nmadvise"), CTL(stats_arenas_i_dirty_nmadvise)},
	{NAME("dirty_purged"),	CTL(stats_arenas_i_dirty_purged)},
	{NAME("muzzy_npurge"),	CTL(stats_arenas_i_muzzy_npurge)},
	{NAME("muzzy_nmadvise"), CTL(stats_arenas_i_muzzy_nmadvise)},
	{NAME("muzzy_purged"),	CTL(stats_arenas_i_muzzy_purged)},
	{NAME("base"),		CTL(stats_arenas_i_base)},
	{NAME("internal"),	CTL(stats_arenas_i_internal)},
	{NAME("metadata_thp"),	CTL(stats_arenas_i_metadata_thp)},
	{NAME("tcache_bytes"),	CTL(stats_arenas_i_tcache_bytes)},
	{NAME("tcache_stashed_bytes"),
	    CTL(stats_arenas_i_tcache_stashed_bytes)},
	{NAME("resident"),	CTL(stats_arenas_i_resident)},
	{NAME("abandoned_vm"),	CTL(stats_arenas_i_abandoned_vm)},
	{NAME("hpa_sec_bytes"),	CTL(stats_arenas_i_hpa_sec_bytes)},
	{NAME("small"),		CHILD(named, stats_arenas_i_small)},
	{NAME("large"),		CHILD(named, stats_arenas_i_large)},
	{NAME("bins"),		CHILD(indexed, stats_arenas_i_bins)},
	{NAME("lextents"),	CHILD(indexed, stats_arenas_i_lextents)},
	{NAME("extents"),	CHILD(indexed, stats_arenas_i_extents)},
	{NAME("mutexes"),	CHILD(named, stats_arenas_i_mutexes)},
	{NAME("hpa_shard"),	CHILD(named, stats_arenas_i_hpa_shard)}
};
static const ctl_named_node_t super_stats_arenas_i_node[] = {
	{NAME(""),		CHILD(named, stats_arenas_i)}
};

static const ctl_indexed_node_t stats_arenas_node[] = {
	{INDEX(stats_arenas_i)}
};

static const ctl_named_node_t stats_background_thread_node[] = {
	{NAME("num_threads"),	CTL(stats_background_thread_num_threads)},
	{NAME("num_runs"),	CTL(stats_background_thread_num_runs)},
	{NAME("run_interval"),	CTL(stats_background_thread_run_interval)}
};

#define OP(mtx) MUTEX_PROF_DATA_NODE(mutexes_##mtx)
MUTEX_PROF_GLOBAL_MUTEXES
#undef OP

static const ctl_named_node_t stats_mutexes_node[] = {
#define OP(mtx) {NAME(#mtx), CHILD(named, stats_mutexes_##mtx)},
MUTEX_PROF_GLOBAL_MUTEXES
#undef OP
	{NAME("reset"),		CTL(stats_mutexes_reset)}
};
#undef MUTEX_PROF_DATA_NODE

static const ctl_named_node_t stats_node[] = {
	{NAME("allocated"),	CTL(stats_allocated)},
	{NAME("active"),	CTL(stats_active)},
	{NAME("metadata"),	CTL(stats_metadata)},
	{NAME("metadata_thp"),	CTL(stats_metadata_thp)},
	{NAME("resident"),	CTL(stats_resident)},
	{NAME("mapped"),	CTL(stats_mapped)},
	{NAME("retained"),	CTL(stats_retained)},
	{NAME("background_thread"),
	 CHILD(named, stats_background_thread)},
	{NAME("mutexes"),	CHILD(named, stats_mutexes)},
	{NAME("arenas"),	CHILD(indexed, stats_arenas)},
	{NAME("zero_reallocs"),	CTL(stats_zero_reallocs)},
};

static const ctl_named_node_t experimental_hooks_node[] = {
	{NAME("install"),	CTL(experimental_hooks_install)},
	{NAME("remove"),	CTL(experimental_hooks_remove)},
	{NAME("prof_backtrace"),	CTL(experimental_hooks_prof_backtrace)},
	{NAME("prof_dump"),	CTL(experimental_hooks_prof_dump)},
	{NAME("safety_check_abort"),	CTL(experimental_hooks_safety_check_abort)},
};

static const ctl_named_node_t experimental_thread_node[] = {
	{NAME("activity_callback"),
		CTL(experimental_thread_activity_callback)}
};

static const ctl_named_node_t experimental_utilization_node[] = {
	{NAME("query"),		CTL(experimental_utilization_query)},
	{NAME("batch_query"),	CTL(experimental_utilization_batch_query)}
};

static const ctl_named_node_t experimental_arenas_i_node[] = {
	{NAME("pactivep"),	CTL(experimental_arenas_i_pactivep)}
};
static const ctl_named_node_t super_experimental_arenas_i_node[] = {
	{NAME(""),		CHILD(named, experimental_arenas_i)}
};

static const ctl_indexed_node_t experimental_arenas_node[] = {
	{INDEX(experimental_arenas_i)}
};

static const ctl_named_node_t experimental_prof_recent_node[] = {
	{NAME("alloc_max"),	CTL(experimental_prof_recent_alloc_max)},
	{NAME("alloc_dump"),	CTL(experimental_prof_recent_alloc_dump)},
};

static const ctl_named_node_t experimental_node[] = {
	{NAME("hooks"),		CHILD(named, experimental_hooks)},
	{NAME("utilization"),	CHILD(named, experimental_utilization)},
	{NAME("arenas"),	CHILD(indexed, experimental_arenas)},
	{NAME("arenas_create_ext"),	CTL(experimental_arenas_create_ext)},
	{NAME("prof_recent"),	CHILD(named, experimental_prof_recent)},
	{NAME("batch_alloc"),	CTL(experimental_batch_alloc)},
	{NAME("thread"),	CHILD(named, experimental_thread)}
};

static const ctl_named_node_t	root_node[] = {
	{NAME("version"),	CTL(version)},
	{NAME("epoch"),		CTL(epoch)},
	{NAME("background_thread"),	CTL(background_thread)},
	{NAME("max_background_threads"),	CTL(max_background_threads)},
	{NAME("thread"),	CHILD(named, thread)},
	{NAME("config"),	CHILD(named, config)},
	{NAME("opt"),		CHILD(named, opt)},
	{NAME("tcache"),	CHILD(named, tcache)},
	{NAME("arena"),		CHILD(indexed, arena)},
	{NAME("arenas"),	CHILD(named, arenas)},
	{NAME("prof"),		CHILD(named, prof)},
	{NAME("stats"),		CHILD(named, stats)},
	{NAME("experimental"),	CHILD(named, experimental)}
};
static const ctl_named_node_t super_root_node[] = {
	{NAME(""),		CHILD(named, root)}
};

#undef NAME
#undef CHILD
#undef CTL
#undef INDEX

/******************************************************************************/

/*
 * Sets *dst + *src non-atomically.  This is safe, since everything is
 * synchronized by the ctl mutex.
 */
static void
ctl_accum_locked_u64(locked_u64_t *dst, locked_u64_t *src) {
	locked_inc_u64_unsynchronized(dst,
	    locked_read_u64_unsynchronized(src));
}

static void
ctl_accum_atomic_zu(atomic_zu_t *dst, atomic_zu_t *src) {
	size_t cur_dst = atomic_load_zu(dst, ATOMIC_RELAXED);
	size_t cur_src = atomic_load_zu(src, ATOMIC_RELAXED);
	atomic_store_zu(dst, cur_dst + cur_src, ATOMIC_RELAXED);
}

/******************************************************************************/

static unsigned
arenas_i2a_impl(size_t i, bool compat, bool validate) {
	unsigned a;

	switch (i) {
	case MALLCTL_ARENAS_ALL:
		a = 0;
		break;
	case MALLCTL_ARENAS_DESTROYED:
		a = 1;
		break;
	default:
		if (compat && i == ctl_arenas->narenas) {
			/*
			 * Provide deprecated backward compatibility for
			 * accessing the merged stats at index narenas rather
			 * than via MALLCTL_ARENAS_ALL.  This is scheduled for
			 * removal in 6.0.0.
			 */
			a = 0;
		} else if (validate && i >= ctl_arenas->narenas) {
			a = UINT_MAX;
		} else {
			/*
			 * This function should never be called for an index
			 * more than one past the range of indices that have
			 * initialized ctl data.
			 */
			assert(i < ctl_arenas->narenas || (!validate && i ==
			    ctl_arenas->narenas));
			a = (unsigned)i + 2;
		}
		break;
	}

	return a;
}

static unsigned
arenas_i2a(size_t i) {
	return arenas_i2a_impl(i, true, false);
}

static ctl_arena_t *
arenas_i_impl(tsd_t *tsd, size_t i, bool compat, bool init) {
	ctl_arena_t *ret;

	assert(!compat || !init);

	ret = ctl_arenas->arenas[arenas_i2a_impl(i, compat, false)];
	if (init && ret == NULL) {
		if (config_stats) {
			struct container_s {
				ctl_arena_t		ctl_arena;
				ctl_arena_stats_t	astats;
			};
			struct container_s *cont =
			    (struct container_s *)base_alloc(tsd_tsdn(tsd),
			    b0get(), sizeof(struct container_s), QUANTUM);
			if (cont == NULL) {
				return NULL;
			}
			ret = &cont->ctl_arena;
			ret->astats = &cont->astats;
		} else {
			ret = (ctl_arena_t *)base_alloc(tsd_tsdn(tsd), b0get(),
			    sizeof(ctl_arena_t), QUANTUM);
			if (ret == NULL) {
				return NULL;
			}
		}
		ret->arena_ind = (unsigned)i;
		ctl_arenas->arenas[arenas_i2a_impl(i, compat, false)] = ret;
	}

	assert(ret == NULL || arenas_i2a(ret->arena_ind) == arenas_i2a(i));
	return ret;
}

static ctl_arena_t *
arenas_i(size_t i) {
	ctl_arena_t *ret = arenas_i_impl(tsd_fetch(), i, true, false);
	assert(ret != NULL);
	return ret;
}

static void
ctl_arena_clear(ctl_arena_t *ctl_arena) {
	ctl_arena->nthreads = 0;
	ctl_arena->dss = dss_prec_names[dss_prec_limit];
	ctl_arena->dirty_decay_ms = -1;
	ctl_arena->muzzy_decay_ms = -1;
	ctl_arena->pactive = 0;
	ctl_arena->pdirty = 0;
	ctl_arena->pmuzzy = 0;
	if (config_stats) {
		memset(&ctl_arena->astats->astats, 0, sizeof(arena_stats_t));
		ctl_arena->astats->allocated_small = 0;
		ctl_arena->astats->nmalloc_small = 0;
		ctl_arena->astats->ndalloc_small = 0;
		ctl_arena->astats->nrequests_small = 0;
		ctl_arena->astats->nfills_small = 0;
		ctl_arena->astats->nflushes_small = 0;
		memset(ctl_arena->astats->bstats, 0, SC_NBINS *
		    sizeof(bin_stats_data_t));
		memset(ctl_arena->astats->lstats, 0, (SC_NSIZES - SC_NBINS) *
		    sizeof(arena_stats_large_t));
		memset(ctl_arena->astats->estats, 0, SC_NPSIZES *
		    sizeof(pac_estats_t));
		memset(&ctl_arena->astats->hpastats, 0,
		    sizeof(hpa_shard_stats_t));
		memset(&ctl_arena->astats->secstats, 0,
		    sizeof(sec_stats_t));
	}
}

static void
ctl_arena_stats_amerge(tsdn_t *tsdn, ctl_arena_t *ctl_arena, arena_t *arena) {
	unsigned i;

	if (config_stats) {
		arena_stats_merge(tsdn, arena, &ctl_arena->nthreads,
		    &ctl_arena->dss, &ctl_arena->dirty_decay_ms,
		    &ctl_arena->muzzy_decay_ms, &ctl_arena->pactive,
		    &ctl_arena->pdirty, &ctl_arena->pmuzzy,
		    &ctl_arena->astats->astats, ctl_arena->astats->bstats,
		    ctl_arena->astats->lstats, ctl_arena->astats->estats,
		    &ctl_arena->astats->hpastats, &ctl_arena->astats->secstats);

		for (i = 0; i < SC_NBINS; i++) {
			bin_stats_t *bstats =
			    &ctl_arena->astats->bstats[i].stats_data;
			ctl_arena->astats->allocated_small += bstats->curregs *
			    sz_index2size(i);
			ctl_arena->astats->nmalloc_small += bstats->nmalloc;
			ctl_arena->astats->ndalloc_small += bstats->ndalloc;
			ctl_arena->astats->nrequests_small += bstats->nrequests;
			ctl_arena->astats->nfills_small += bstats->nfills;
			ctl_arena->astats->nflushes_small += bstats->nflushes;
		}
	} else {
		arena_basic_stats_merge(tsdn, arena, &ctl_arena->nthreads,
		    &ctl_arena->dss, &ctl_arena->dirty_decay_ms,
		    &ctl_arena->muzzy_decay_ms, &ctl_arena->pactive,
		    &ctl_arena->pdirty, &ctl_arena->pmuzzy);
	}
}

static void
ctl_arena_stats_sdmerge(ctl_arena_t *ctl_sdarena, ctl_arena_t *ctl_arena,
    bool destroyed) {
	unsigned i;

	if (!destroyed) {
		ctl_sdarena->nthreads += ctl_arena->nthreads;
		ctl_sdarena->pactive += ctl_arena->pactive;
		ctl_sdarena->pdirty += ctl_arena->pdirty;
		ctl_sdarena->pmuzzy += ctl_arena->pmuzzy;
	} else {
		assert(ctl_arena->nthreads == 0);
		assert(ctl_arena->pactive == 0);
		assert(ctl_arena->pdirty == 0);
		assert(ctl_arena->pmuzzy == 0);
	}

	if (config_stats) {
		ctl_arena_stats_t *sdstats = ctl_sdarena->astats;
		ctl_arena_stats_t *astats = ctl_arena->astats;

		if (!destroyed) {
			sdstats->astats.mapped += astats->astats.mapped;
			sdstats->astats.pa_shard_stats.pac_stats.retained
			    += astats->astats.pa_shard_stats.pac_stats.retained;
			sdstats->astats.pa_shard_stats.edata_avail
			    += astats->astats.pa_shard_stats.edata_avail;
		}

		ctl_accum_locked_u64(
		    &sdstats->astats.pa_shard_stats.pac_stats.decay_dirty.npurge,
		    &astats->astats.pa_shard_stats.pac_stats.decay_dirty.npurge);
		ctl_accum_locked_u64(
		    &sdstats->astats.pa_shard_stats.pac_stats.decay_dirty.nmadvise,
		    &astats->astats.pa_shard_stats.pac_stats.decay_dirty.nmadvise);
		ctl_accum_locked_u64(
		    &sdstats->astats.pa_shard_stats.pac_stats.decay_dirty.purged,
		    &astats->astats.pa_shard_stats.pac_stats.decay_dirty.purged);

		ctl_accum_locked_u64(
		    &sdstats->astats.pa_shard_stats.pac_stats.decay_muzzy.npurge,
		    &astats->astats.pa_shard_stats.pac_stats.decay_muzzy.npurge);
		ctl_accum_locked_u64(
		    &sdstats->astats.pa_shard_stats.pac_stats.decay_muzzy.nmadvise,
		    &astats->astats.pa_shard_stats.pac_stats.decay_muzzy.nmadvise);
		ctl_accum_locked_u64(
		    &sdstats->astats.pa_shard_stats.pac_stats.decay_muzzy.purged,
		    &astats->astats.pa_shard_stats.pac_stats.decay_muzzy.purged);

#define OP(mtx) malloc_mutex_prof_merge(				\
		    &(sdstats->astats.mutex_prof_data[			\
		        arena_prof_mutex_##mtx]),			\
		    &(astats->astats.mutex_prof_data[			\
		        arena_prof_mutex_##mtx]));
MUTEX_PROF_ARENA_MUTEXES
#undef OP
		if (!destroyed) {
			sdstats->astats.base += astats->astats.base;
			sdstats->astats.resident += astats->astats.resident;
			sdstats->astats.metadata_thp += astats->astats.metadata_thp;
			ctl_accum_atomic_zu(&sdstats->astats.internal,
			    &astats->astats.internal);
		} else {
			assert(atomic_load_zu(
			    &astats->astats.internal, ATOMIC_RELAXED) == 0);
		}

		if (!destroyed) {
			sdstats->allocated_small += astats->allocated_small;
		} else {
			assert(astats->allocated_small == 0);
		}
		sdstats->nmalloc_small += astats->nmalloc_small;
		sdstats->ndalloc_small += astats->ndalloc_small;
		sdstats->nrequests_small += astats->nrequests_small;
		sdstats->nfills_small += astats->nfills_small;
		sdstats->nflushes_small += astats->nflushes_small;

		if (!destroyed) {
			sdstats->astats.allocated_large +=
			    astats->astats.allocated_large;
		} else {
			assert(astats->astats.allocated_large == 0);
		}
		sdstats->astats.nmalloc_large += astats->astats.nmalloc_large;
		sdstats->astats.ndalloc_large += astats->astats.ndalloc_large;
		sdstats->astats.nrequests_large
		    += astats->astats.nrequests_large;
		sdstats->astats.nflushes_large += astats->astats.nflushes_large;
		ctl_accum_atomic_zu(
		    &sdstats->astats.pa_shard_stats.pac_stats.abandoned_vm,
		    &astats->astats.pa_shard_stats.pac_stats.abandoned_vm);

		sdstats->astats.tcache_bytes += astats->astats.tcache_bytes;
		sdstats->astats.tcache_stashed_bytes +=
		    astats->astats.tcache_stashed_bytes;

		if (ctl_arena->arena_ind == 0) {
			sdstats->astats.uptime = astats->astats.uptime;
		}

		/* Merge bin stats. */
		for (i = 0; i < SC_NBINS; i++) {
			bin_stats_t *bstats = &astats->bstats[i].stats_data;
			bin_stats_t *merged = &sdstats->bstats[i].stats_data;
			merged->nmalloc += bstats->nmalloc;
			merged->ndalloc += bstats->ndalloc;
			merged->nrequests += bstats->nrequests;
			if (!destroyed) {
				merged->curregs += bstats->curregs;
			} else {
				assert(bstats->curregs == 0);
			}
			merged->nfills += bstats->nfills;
			merged->nflushes += bstats->nflushes;
			merged->nslabs += bstats->nslabs;
			merged->reslabs += bstats->reslabs;
			if (!destroyed) {
				merged->curslabs += bstats->curslabs;
				merged->nonfull_slabs += bstats->nonfull_slabs;
			} else {
				assert(bstats->curslabs == 0);
				assert(bstats->nonfull_slabs == 0);
			}
			malloc_mutex_prof_merge(&sdstats->bstats[i].mutex_data,
			    &astats->bstats[i].mutex_data);
		}

		/* Merge stats for large allocations. */
		for (i = 0; i < SC_NSIZES - SC_NBINS; i++) {
			ctl_accum_locked_u64(&sdstats->lstats[i].nmalloc,
			    &astats->lstats[i].nmalloc);
			ctl_accum_locked_u64(&sdstats->lstats[i].ndalloc,
			    &astats->lstats[i].ndalloc);
			ctl_accum_locked_u64(&sdstats->lstats[i].nrequests,
			    &astats->lstats[i].nrequests);
			if (!destroyed) {
				sdstats->lstats[i].curlextents +=
				    astats->lstats[i].curlextents;
			} else {
				assert(astats->lstats[i].curlextents == 0);
			}
		}

		/* Merge extents stats. */
		for (i = 0; i < SC_NPSIZES; i++) {
			sdstats->estats[i].ndirty += astats->estats[i].ndirty;
			sdstats->estats[i].nmuzzy += astats->estats[i].nmuzzy;
			sdstats->estats[i].nretained
			    += astats->estats[i].nretained;
			sdstats->estats[i].dirty_bytes
			    += astats->estats[i].dirty_bytes;
			sdstats->estats[i].muzzy_bytes
			    += astats->estats[i].muzzy_bytes;
			sdstats->estats[i].retained_bytes
			    += astats->estats[i].retained_bytes;
		}

		/* Merge HPA stats. */
		hpa_shard_stats_accum(&sdstats->hpastats, &astats->hpastats);
		sec_stats_accum(&sdstats->secstats, &astats->secstats);
	}
}

static void
ctl_arena_refresh(tsdn_t *tsdn, arena_t *arena, ctl_arena_t *ctl_sdarena,
    unsigned i, bool destroyed) {
	ctl_arena_t *ctl_arena = arenas_i(i);

	ctl_arena_clear(ctl_arena);
	ctl_arena_stats_amerge(tsdn, ctl_arena, arena);
	/* Merge into sum stats as well. */
	ctl_arena_stats_sdmerge(ctl_sdarena, ctl_arena, destroyed);
}

static unsigned
ctl_arena_init(tsd_t *tsd, const arena_config_t *config) {
	unsigned arena_ind;
	ctl_arena_t *ctl_arena;

	if ((ctl_arena = ql_last(&ctl_arenas->destroyed, destroyed_link)) !=
	    NULL) {
		ql_remove(&ctl_arenas->destroyed, ctl_arena, destroyed_link);
		arena_ind = ctl_arena->arena_ind;
	} else {
		arena_ind = ctl_arenas->narenas;
	}

	/* Trigger stats allocation. */
	if (arenas_i_impl(tsd, arena_ind, false, true) == NULL) {
		return UINT_MAX;
	}

	/* Initialize new arena. */
	if (arena_init(tsd_tsdn(tsd), arena_ind, config) == NULL) {
		return UINT_MAX;
	}

	if (arena_ind == ctl_arenas->narenas) {
		ctl_arenas->narenas++;
	}

	return arena_ind;
}

static void
ctl_background_thread_stats_read(tsdn_t *tsdn) {
	background_thread_stats_t *stats = &ctl_stats->background_thread;
	if (!have_background_thread ||
	    background_thread_stats_read(tsdn, stats)) {
		memset(stats, 0, sizeof(background_thread_stats_t));
		nstime_init_zero(&stats->run_interval);
	}
	malloc_mutex_prof_copy(
	    &ctl_stats->mutex_prof_data[global_prof_mutex_max_per_bg_thd],
	    &stats->max_counter_per_bg_thd);
}

static void
ctl_refresh(tsdn_t *tsdn) {
	unsigned i;
	ctl_arena_t *ctl_sarena = arenas_i(MALLCTL_ARENAS_ALL);
	VARIABLE_ARRAY(arena_t *, tarenas, ctl_arenas->narenas);

	/*
	 * Clear sum stats, since they will be merged into by
	 * ctl_arena_refresh().
	 */
	ctl_arena_clear(ctl_sarena);

	for (i = 0; i < ctl_arenas->narenas; i++) {
		tarenas[i] = arena_get(tsdn, i, false);
	}

	for (i = 0; i < ctl_arenas->narenas; i++) {
		ctl_arena_t *ctl_arena = arenas_i(i);
		bool initialized = (tarenas[i] != NULL);

		ctl_arena->initialized = initialized;
		if (initialized) {
			ctl_arena_refresh(tsdn, tarenas[i], ctl_sarena, i,
			    false);
		}
	}

	if (config_stats) {
		ctl_stats->allocated = ctl_sarena->astats->allocated_small +
		    ctl_sarena->astats->astats.allocated_large;
		ctl_stats->active = (ctl_sarena->pactive << LG_PAGE);
		ctl_stats->metadata = ctl_sarena->astats->astats.base +
		    atomic_load_zu(&ctl_sarena->astats->astats.internal,
			ATOMIC_RELAXED);
		ctl_stats->resident = ctl_sarena->astats->astats.resident;
		ctl_stats->metadata_thp =
		    ctl_sarena->astats->astats.metadata_thp;
		ctl_stats->mapped = ctl_sarena->astats->astats.mapped;
		ctl_stats->retained = ctl_sarena->astats->astats
		    .pa_shard_stats.pac_stats.retained;

		ctl_background_thread_stats_read(tsdn);

#define READ_GLOBAL_MUTEX_PROF_DATA(i, mtx)				\
    malloc_mutex_lock(tsdn, &mtx);					\
    malloc_mutex_prof_read(tsdn, &ctl_stats->mutex_prof_data[i], &mtx);	\
    malloc_mutex_unlock(tsdn, &mtx);

		if (config_prof && opt_prof) {
			READ_GLOBAL_MUTEX_PROF_DATA(
			    global_prof_mutex_prof, bt2gctx_mtx);
			READ_GLOBAL_MUTEX_PROF_DATA(
			    global_prof_mutex_prof_thds_data, tdatas_mtx);
			READ_GLOBAL_MUTEX_PROF_DATA(
			    global_prof_mutex_prof_dump, prof_dump_mtx);
			READ_GLOBAL_MUTEX_PROF_DATA(
			    global_prof_mutex_prof_recent_alloc,
			    prof_recent_alloc_mtx);
			READ_GLOBAL_MUTEX_PROF_DATA(
			    global_prof_mutex_prof_recent_dump,
			    prof_recent_dump_mtx);
			READ_GLOBAL_MUTEX_PROF_DATA(
			    global_prof_mutex_prof_stats, prof_stats_mtx);
		}
		if (have_background_thread) {
			READ_GLOBAL_MUTEX_PROF_DATA(
			    global_prof_mutex_background_thread,
			    background_thread_lock);
		} else {
			memset(&ctl_stats->mutex_prof_data[
			    global_prof_mutex_background_thread], 0,
			    sizeof(mutex_prof_data_t));
		}
		/* We own ctl mutex already. */
		malloc_mutex_prof_read(tsdn,
		    &ctl_stats->mutex_prof_data[global_prof_mutex_ctl],
		    &ctl_mtx);
#undef READ_GLOBAL_MUTEX_PROF_DATA
	}
	ctl_arenas->epoch++;
}

static bool
ctl_init(tsd_t *tsd) {
	bool ret;
	tsdn_t *tsdn = tsd_tsdn(tsd);

	malloc_mutex_lock(tsdn, &ctl_mtx);
	if (!ctl_initialized) {
		ctl_arena_t *ctl_sarena, *ctl_darena;
		unsigned i;

		/*
		 * Allocate demand-zeroed space for pointers to the full
		 * range of supported arena indices.
		 */
		if (ctl_arenas == NULL) {
			ctl_arenas = (ctl_arenas_t *)base_alloc(tsdn,
			    b0get(), sizeof(ctl_arenas_t), QUANTUM);
			if (ctl_arenas == NULL) {
				ret = true;
				goto label_return;
			}
		}

		if (config_stats && ctl_stats == NULL) {
			ctl_stats = (ctl_stats_t *)base_alloc(tsdn, b0get(),
			    sizeof(ctl_stats_t), QUANTUM);
			if (ctl_stats == NULL) {
				ret = true;
				goto label_return;
			}
		}

		/*
		 * Allocate space for the current full range of arenas
		 * here rather than doing it lazily elsewhere, in order
		 * to limit when OOM-caused errors can occur.
		 */
		if ((ctl_sarena = arenas_i_impl(tsd, MALLCTL_ARENAS_ALL, false,
		    true)) == NULL) {
			ret = true;
			goto label_return;
		}
		ctl_sarena->initialized = true;

		if ((ctl_darena = arenas_i_impl(tsd, MALLCTL_ARENAS_DESTROYED,
		    false, true)) == NULL) {
			ret = true;
			goto label_return;
		}
		ctl_arena_clear(ctl_darena);
		/*
		 * Don't toggle ctl_darena to initialized until an arena is
		 * actually destroyed, so that arena.<i>.initialized can be used
		 * to query whether the stats are relevant.
		 */

		ctl_arenas->narenas = narenas_total_get();
		for (i = 0; i < ctl_arenas->narenas; i++) {
			if (arenas_i_impl(tsd, i, false, true) == NULL) {
				ret = true;
				goto label_return;
			}
		}

		ql_new(&ctl_arenas->destroyed);
		ctl_refresh(tsdn);

		ctl_initialized = true;
	}

	ret = false;
label_return:
	malloc_mutex_unlock(tsdn, &ctl_mtx);
	return ret;
}

static int
ctl_lookup(tsdn_t *tsdn, const ctl_named_node_t *starting_node,
    const char *name, const ctl_named_node_t **ending_nodep, size_t *mibp,
    size_t *depthp) {
	int ret;
	const char *elm, *tdot, *dot;
	size_t elen, i, j;
	const ctl_named_node_t *node;

	elm = name;
	/* Equivalent to strchrnul(). */
	dot = ((tdot = strchr(elm, '.')) != NULL) ? tdot : strchr(elm, '\0');
	elen = (size_t)((uintptr_t)dot - (uintptr_t)elm);
	if (elen == 0) {
		ret = ENOENT;
		goto label_return;
	}
	node = starting_node;
	for (i = 0; i < *depthp; i++) {
		assert(node);
		assert(node->nchildren > 0);
		if (ctl_named_node(node->children) != NULL) {
			const ctl_named_node_t *pnode = node;

			/* Children are named. */
			for (j = 0; j < node->nchildren; j++) {
				const ctl_named_node_t *child =
				    ctl_named_children(node, j);
				if (strlen(child->name) == elen &&
				    strncmp(elm, child->name, elen) == 0) {
					node = child;
					mibp[i] = j;
					break;
				}
			}
			if (node == pnode) {
				ret = ENOENT;
				goto label_return;
			}
		} else {
			uintmax_t index;
			const ctl_indexed_node_t *inode;

			/* Children are indexed. */
			index = malloc_strtoumax(elm, NULL, 10);
			if (index == UINTMAX_MAX || index > SIZE_T_MAX) {
				ret = ENOENT;
				goto label_return;
			}

			inode = ctl_indexed_node(node->children);
			node = inode->index(tsdn, mibp, *depthp, (size_t)index);
			if (node == NULL) {
				ret = ENOENT;
				goto label_return;
			}

			mibp[i] = (size_t)index;
		}

		/* Reached the end? */
		if (node->ctl != NULL || *dot == '\0') {
			/* Terminal node. */
			if (*dot != '\0') {
				/*
				 * The name contains more elements than are
				 * in this path through the tree.
				 */
				ret = ENOENT;
				goto label_return;
			}
			/* Complete lookup successful. */
			*depthp = i + 1;
			break;
		}

		/* Update elm. */
		elm = &dot[1];
		dot = ((tdot = strchr(elm, '.')) != NULL) ? tdot :
		    strchr(elm, '\0');
		elen = (size_t)((uintptr_t)dot - (uintptr_t)elm);
	}
	if (ending_nodep != NULL) {
		*ending_nodep = node;
	}

	ret = 0;
label_return:
	return ret;
}

int
ctl_byname(tsd_t *tsd, const char *name, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen) {
	int ret;
	size_t depth;
	size_t mib[CTL_MAX_DEPTH];
	const ctl_named_node_t *node;

	if (!ctl_initialized && ctl_init(tsd)) {
		ret = EAGAIN;
		goto label_return;
	}

	depth = CTL_MAX_DEPTH;
	ret = ctl_lookup(tsd_tsdn(tsd), super_root_node, name, &node, mib,
	    &depth);
	if (ret != 0) {
		goto label_return;
	}

	if (node != NULL && node->ctl) {
		ret = node->ctl(tsd, mib, depth, oldp, oldlenp, newp, newlen);
	} else {
		/* The name refers to a partial path through the ctl tree. */
		ret = ENOENT;
	}

label_return:
	return(ret);
}

int
ctl_nametomib(tsd_t *tsd, const char *name, size_t *mibp, size_t *miblenp) {
	int ret;

	if (!ctl_initialized && ctl_init(tsd)) {
		ret = EAGAIN;
		goto label_return;
	}

	ret = ctl_lookup(tsd_tsdn(tsd), super_root_node, name, NULL, mibp,
	    miblenp);
label_return:
	return(ret);
}

static int
ctl_lookupbymib(tsdn_t *tsdn, const ctl_named_node_t **ending_nodep,
    const size_t *mib, size_t miblen) {
	int ret;

	const ctl_named_node_t *node = super_root_node;
	for (size_t i = 0; i < miblen; i++) {
		assert(node);
		assert(node->nchildren > 0);
		if (ctl_named_node(node->children) != NULL) {
			/* Children are named. */
			if (node->nchildren <= mib[i]) {
				ret = ENOENT;
				goto label_return;
			}
			node = ctl_named_children(node, mib[i]);
		} else {
			const ctl_indexed_node_t *inode;

			/* Indexed element. */
			inode = ctl_indexed_node(node->children);
			node = inode->index(tsdn, mib, miblen, mib[i]);
			if (node == NULL) {
				ret = ENOENT;
				goto label_return;
			}
		}
	}
	assert(ending_nodep != NULL);
	*ending_nodep = node;
	ret = 0;

label_return:
	return(ret);
}

int
ctl_bymib(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	const ctl_named_node_t *node;

	if (!ctl_initialized && ctl_init(tsd)) {
		ret = EAGAIN;
		goto label_return;
	}

	ret = ctl_lookupbymib(tsd_tsdn(tsd), &node, mib, miblen);
	if (ret != 0) {
		goto label_return;
	}

	/* Call the ctl function. */
	if (node && node->ctl) {
		ret = node->ctl(tsd, mib, miblen, oldp, oldlenp, newp, newlen);
	} else {
		/* Partial MIB. */
		ret = ENOENT;
	}

label_return:
	return(ret);
}

int
ctl_mibnametomib(tsd_t *tsd, size_t *mib, size_t miblen, const char *name,
    size_t *miblenp) {
	int ret;
	const ctl_named_node_t *node;

	if (!ctl_initialized && ctl_init(tsd)) {
		ret = EAGAIN;
		goto label_return;
	}

	ret = ctl_lookupbymib(tsd_tsdn(tsd), &node, mib, miblen);
	if (ret != 0) {
		goto label_return;
	}
	if (node == NULL || node->ctl != NULL) {
		ret = ENOENT;
		goto label_return;
	}

	assert(miblenp != NULL);
	assert(*miblenp >= miblen);
	*miblenp -= miblen;
	ret = ctl_lookup(tsd_tsdn(tsd), node, name, NULL, mib + miblen,
	    miblenp);
	*miblenp += miblen;
label_return:
	return(ret);
}

int
ctl_bymibname(tsd_t *tsd, size_t *mib, size_t miblen, const char *name,
    size_t *miblenp, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	const ctl_named_node_t *node;

	if (!ctl_initialized && ctl_init(tsd)) {
		ret = EAGAIN;
		goto label_return;
	}

	ret = ctl_lookupbymib(tsd_tsdn(tsd), &node, mib, miblen);
	if (ret != 0) {
		goto label_return;
	}
	if (node == NULL || node->ctl != NULL) {
		ret = ENOENT;
		goto label_return;
	}

	assert(miblenp != NULL);
	assert(*miblenp >= miblen);
	*miblenp -= miblen;
	/*
	 * The same node supplies the starting node and stores the ending node.
	 */
	ret = ctl_lookup(tsd_tsdn(tsd), node, name, &node, mib + miblen,
	    miblenp);
	*miblenp += miblen;
	if (ret != 0) {
		goto label_return;
	}

	if (node != NULL && node->ctl) {
		ret = node->ctl(tsd, mib, *miblenp, oldp, oldlenp, newp,
		    newlen);
	} else {
		/* The name refers to a partial path through the ctl tree. */
		ret = ENOENT;
	}

label_return:
	return(ret);
}

bool
ctl_boot(void) {
	if (malloc_mutex_init(&ctl_mtx, "ctl", WITNESS_RANK_CTL,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}

	ctl_initialized = false;

	return false;
}

void
ctl_prefork(tsdn_t *tsdn) {
	malloc_mutex_prefork(tsdn, &ctl_mtx);
}

void
ctl_postfork_parent(tsdn_t *tsdn) {
	malloc_mutex_postfork_parent(tsdn, &ctl_mtx);
}

void
ctl_postfork_child(tsdn_t *tsdn) {
	malloc_mutex_postfork_child(tsdn, &ctl_mtx);
}

void
ctl_mtx_assert_held(tsdn_t *tsdn) {
	malloc_mutex_assert_owner(tsdn, &ctl_mtx);
}

/******************************************************************************/
/* *_ctl() functions. */

#define READONLY()	do {						\
	if (newp != NULL || newlen != 0) {				\
		ret = EPERM;						\
		goto label_return;					\
	}								\
} while (0)

#define WRITEONLY()	do {						\
	if (oldp != NULL || oldlenp != NULL) {				\
		ret = EPERM;						\
		goto label_return;					\
	}								\
} while (0)

/* Can read or write, but not both. */
#define READ_XOR_WRITE()	do {					\
	if ((oldp != NULL && oldlenp != NULL) && (newp != NULL ||	\
	    newlen != 0)) {						\
		ret = EPERM;						\
		goto label_return;					\
	}								\
} while (0)

/* Can neither read nor write. */
#define NEITHER_READ_NOR_WRITE()	do {				\
	if (oldp != NULL || oldlenp != NULL || newp != NULL ||		\
	    newlen != 0) {						\
		ret = EPERM;						\
		goto label_return;					\
	}								\
} while (0)

/* Verify that the space provided is enough. */
#define VERIFY_READ(t)	do {						\
	if (oldp == NULL || oldlenp == NULL || *oldlenp != sizeof(t)) {	\
		*oldlenp = 0;						\
		ret = EINVAL;						\
		goto label_return;					\
	}								\
} while (0)

#define READ(v, t)	do {						\
	if (oldp != NULL && oldlenp != NULL) {				\
		if (*oldlenp != sizeof(t)) {				\
			size_t	copylen = (sizeof(t) <= *oldlenp)	\
			    ? sizeof(t) : *oldlenp;			\
			memcpy(oldp, (void *)&(v), copylen);		\
			*oldlenp = copylen;				\
			ret = EINVAL;					\
			goto label_return;				\
		}							\
		*(t *)oldp = (v);					\
	}								\
} while (0)

#define WRITE(v, t)	do {						\
	if (newp != NULL) {						\
		if (newlen != sizeof(t)) {				\
			ret = EINVAL;					\
			goto label_return;				\
		}							\
		(v) = *(t *)newp;					\
	}								\
} while (0)

#define ASSURED_WRITE(v, t)	do {					\
	if (newp == NULL || newlen != sizeof(t)) {			\
		ret = EINVAL;						\
		goto label_return;					\
	}								\
	(v) = *(t *)newp;						\
} while (0)

#define MIB_UNSIGNED(v, i) do {						\
	if (mib[i] > UINT_MAX) {					\
		ret = EFAULT;						\
		goto label_return;					\
	}								\
	v = (unsigned)mib[i];						\
} while (0)

/*
 * There's a lot of code duplication in the following macros due to limitations
 * in how nested cpp macros are expanded.
 */
#define CTL_RO_CLGEN(c, l, n, v, t)					\
static int								\
n##_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,	\
    size_t *oldlenp, void *newp, size_t newlen) {			\
	int ret;							\
	t oldval;							\
									\
	if (!(c)) {							\
		return ENOENT;						\
	}								\
	if (l) {							\
		malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);		\
	}								\
	READONLY();							\
	oldval = (v);							\
	READ(oldval, t);						\
									\
	ret = 0;							\
label_return:								\
	if (l) {							\
		malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);		\
	}								\
	return ret;							\
}

#define CTL_RO_CGEN(c, n, v, t)						\
static int								\
n##_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,			\
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {		\
	int ret;							\
	t oldval;							\
									\
	if (!(c)) {							\
		return ENOENT;						\
	}								\
	malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);			\
	READONLY();							\
	oldval = (v);							\
	READ(oldval, t);						\
									\
	ret = 0;							\
label_return:								\
	malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);			\
	return ret;							\
}

#define CTL_RO_GEN(n, v, t)						\
static int								\
n##_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,	\
    size_t *oldlenp, void *newp, size_t newlen) {			\
	int ret;							\
	t oldval;							\
									\
	malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);			\
	READONLY();							\
	oldval = (v);							\
	READ(oldval, t);						\
									\
	ret = 0;							\
label_return:								\
	malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);			\
	return ret;							\
}

/*
 * ctl_mtx is not acquired, under the assumption that no pertinent data will
 * mutate during the call.
 */
#define CTL_RO_NL_CGEN(c, n, v, t)					\
static int								\
n##_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,			\
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {		\
	int ret;							\
	t oldval;							\
									\
	if (!(c)) {							\
		return ENOENT;						\
	}								\
	READONLY();							\
	oldval = (v);							\
	READ(oldval, t);						\
									\
	ret = 0;							\
label_return:								\
	return ret;							\
}

#define CTL_RO_NL_GEN(n, v, t)						\
static int								\
n##_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,			\
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {		\
	int ret;							\
	t oldval;							\
									\
	READONLY();							\
	oldval = (v);							\
	READ(oldval, t);						\
									\
	ret = 0;							\
label_return:								\
	return ret;							\
}

#define CTL_RO_CONFIG_GEN(n, t)						\
static int								\
n##_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,			\
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {		\
	int ret;							\
	t oldval;							\
									\
	READONLY();							\
	oldval = n;							\
	READ(oldval, t);						\
									\
	ret = 0;							\
label_return:								\
	return ret;							\
}

/******************************************************************************/

CTL_RO_NL_GEN(version, JEMALLOC_VERSION, const char *)

static int
epoch_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	UNUSED uint64_t newval;

	malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);
	WRITE(newval, uint64_t);
	if (newp != NULL) {
		ctl_refresh(tsd_tsdn(tsd));
	}
	READ(ctl_arenas->epoch, uint64_t);

	ret = 0;
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);
	return ret;
}

static int
background_thread_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen) {
	int ret;
	bool oldval;

	if (!have_background_thread) {
		return ENOENT;
	}
	background_thread_ctl_init(tsd_tsdn(tsd));

	malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);
	malloc_mutex_lock(tsd_tsdn(tsd), &background_thread_lock);
	if (newp == NULL) {
		oldval = background_thread_enabled();
		READ(oldval, bool);
	} else {
		if (newlen != sizeof(bool)) {
			ret = EINVAL;
			goto label_return;
		}
		oldval = background_thread_enabled();
		READ(oldval, bool);

		bool newval = *(bool *)newp;
		if (newval == oldval) {
			ret = 0;
			goto label_return;
		}

		background_thread_enabled_set(tsd_tsdn(tsd), newval);
		if (newval) {
			if (background_threads_enable(tsd)) {
				ret = EFAULT;
				goto label_return;
			}
		} else {
			if (background_threads_disable(tsd)) {
				ret = EFAULT;
				goto label_return;
			}
		}
	}
	ret = 0;
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &background_thread_lock);
	malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);

	return ret;
}

static int
max_background_threads_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen) {
	int ret;
	size_t oldval;

	if (!have_background_thread) {
		return ENOENT;
	}
	background_thread_ctl_init(tsd_tsdn(tsd));

	malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);
	malloc_mutex_lock(tsd_tsdn(tsd), &background_thread_lock);
	if (newp == NULL) {
		oldval = max_background_threads;
		READ(oldval, size_t);
	} else {
		if (newlen != sizeof(size_t)) {
			ret = EINVAL;
			goto label_return;
		}
		oldval = max_background_threads;
		READ(oldval, size_t);

		size_t newval = *(size_t *)newp;
		if (newval == oldval) {
			ret = 0;
			goto label_return;
		}
		if (newval > opt_max_background_threads) {
			ret = EINVAL;
			goto label_return;
		}

		if (background_thread_enabled()) {
			background_thread_enabled_set(tsd_tsdn(tsd), false);
			if (background_threads_disable(tsd)) {
				ret = EFAULT;
				goto label_return;
			}
			max_background_threads = newval;
			background_thread_enabled_set(tsd_tsdn(tsd), true);
			if (background_threads_enable(tsd)) {
				ret = EFAULT;
				goto label_return;
			}
		} else {
			max_background_threads = newval;
		}
	}
	ret = 0;
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &background_thread_lock);
	malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);

	return ret;
}

/******************************************************************************/

CTL_RO_CONFIG_GEN(config_cache_oblivious, bool)
CTL_RO_CONFIG_GEN(config_debug, bool)
CTL_RO_CONFIG_GEN(config_fill, bool)
CTL_RO_CONFIG_GEN(config_lazy_lock, bool)
CTL_RO_CONFIG_GEN(config_malloc_conf, const char *)
CTL_RO_CONFIG_GEN(config_opt_safety_checks, bool)
CTL_RO_CONFIG_GEN(config_prof, bool)
CTL_RO_CONFIG_GEN(config_prof_libgcc, bool)
CTL_RO_CONFIG_GEN(config_prof_libunwind, bool)
CTL_RO_CONFIG_GEN(config_stats, bool)
CTL_RO_CONFIG_GEN(config_utrace, bool)
CTL_RO_CONFIG_GEN(config_xmalloc, bool)

/******************************************************************************/

CTL_RO_NL_GEN(opt_abort, opt_abort, bool)
CTL_RO_NL_GEN(opt_abort_conf, opt_abort_conf, bool)
CTL_RO_NL_GEN(opt_cache_oblivious, opt_cache_oblivious, bool)
CTL_RO_NL_GEN(opt_trust_madvise, opt_trust_madvise, bool)
CTL_RO_NL_GEN(opt_confirm_conf, opt_confirm_conf, bool)

/* HPA options. */
CTL_RO_NL_GEN(opt_hpa, opt_hpa, bool)
CTL_RO_NL_GEN(opt_hpa_hugification_threshold,
    opt_hpa_opts.hugification_threshold, size_t)
CTL_RO_NL_GEN(opt_hpa_hugify_delay_ms, opt_hpa_opts.hugify_delay_ms, uint64_t)
CTL_RO_NL_GEN(opt_hpa_min_purge_interval_ms, opt_hpa_opts.min_purge_interval_ms,
    uint64_t)

/*
 * This will have to change before we publicly document this option; fxp_t and
 * its representation are internal implementation details.
 */
CTL_RO_NL_GEN(opt_hpa_dirty_mult, opt_hpa_opts.dirty_mult, fxp_t)
CTL_RO_NL_GEN(opt_hpa_slab_max_alloc, opt_hpa_opts.slab_max_alloc, size_t)

/* HPA SEC options */
CTL_RO_NL_GEN(opt_hpa_sec_nshards, opt_hpa_sec_opts.nshards, size_t)
CTL_RO_NL_GEN(opt_hpa_sec_max_alloc, opt_hpa_sec_opts.max_alloc, size_t)
CTL_RO_NL_GEN(opt_hpa_sec_max_bytes, opt_hpa_sec_opts.max_bytes, size_t)
CTL_RO_NL_GEN(opt_hpa_sec_bytes_after_flush, opt_hpa_sec_opts.bytes_after_flush,
    size_t)
CTL_RO_NL_GEN(opt_hpa_sec_batch_fill_extra, opt_hpa_sec_opts.batch_fill_extra,
    size_t)

CTL_RO_NL_GEN(opt_metadata_thp, metadata_thp_mode_names[opt_metadata_thp],
    const char *)
CTL_RO_NL_GEN(opt_retain, opt_retain, bool)
CTL_RO_NL_GEN(opt_dss, opt_dss, const char *)
CTL_RO_NL_GEN(opt_narenas, opt_narenas, unsigned)
CTL_RO_NL_GEN(opt_percpu_arena, percpu_arena_mode_names[opt_percpu_arena],
    const char *)
CTL_RO_NL_GEN(opt_mutex_max_spin, opt_mutex_max_spin, int64_t)
CTL_RO_NL_GEN(opt_oversize_threshold, opt_oversize_threshold, size_t)
CTL_RO_NL_GEN(opt_background_thread, opt_background_thread, bool)
CTL_RO_NL_GEN(opt_max_background_threads, opt_max_background_threads, size_t)
CTL_RO_NL_GEN(opt_dirty_decay_ms, opt_dirty_decay_ms, ssize_t)
CTL_RO_NL_GEN(opt_muzzy_decay_ms, opt_muzzy_decay_ms, ssize_t)
CTL_RO_NL_GEN(opt_stats_print, opt_stats_print, bool)
CTL_RO_NL_GEN(opt_stats_print_opts, opt_stats_print_opts, const char *)
CTL_RO_NL_GEN(opt_stats_interval, opt_stats_interval, int64_t)
CTL_RO_NL_GEN(opt_stats_interval_opts, opt_stats_interval_opts, const char *)
CTL_RO_NL_CGEN(config_fill, opt_junk, opt_junk, const char *)
CTL_RO_NL_CGEN(config_fill, opt_zero, opt_zero, bool)
CTL_RO_NL_CGEN(config_utrace, opt_utrace, opt_utrace, bool)
CTL_RO_NL_CGEN(config_xmalloc, opt_xmalloc, opt_xmalloc, bool)
CTL_RO_NL_CGEN(config_enable_cxx, opt_experimental_infallible_new,
    opt_experimental_infallible_new, bool)
CTL_RO_NL_GEN(opt_tcache, opt_tcache, bool)
CTL_RO_NL_GEN(opt_tcache_max, opt_tcache_max, size_t)
CTL_RO_NL_GEN(opt_tcache_nslots_small_min, opt_tcache_nslots_small_min,
    unsigned)
CTL_RO_NL_GEN(opt_tcache_nslots_small_max, opt_tcache_nslots_small_max,
    unsigned)
CTL_RO_NL_GEN(opt_tcache_nslots_large, opt_tcache_nslots_large, unsigned)
CTL_RO_NL_GEN(opt_lg_tcache_nslots_mul, opt_lg_tcache_nslots_mul, ssize_t)
CTL_RO_NL_GEN(opt_tcache_gc_incr_bytes, opt_tcache_gc_incr_bytes, size_t)
CTL_RO_NL_GEN(opt_tcache_gc_delay_bytes, opt_tcache_gc_delay_bytes, size_t)
CTL_RO_NL_GEN(opt_lg_tcache_flush_small_div, opt_lg_tcache_flush_small_div,
    unsigned)
CTL_RO_NL_GEN(opt_lg_tcache_flush_large_div, opt_lg_tcache_flush_large_div,
    unsigned)
CTL_RO_NL_GEN(opt_thp, thp_mode_names[opt_thp], const char *)
CTL_RO_NL_GEN(opt_lg_extent_max_active_fit, opt_lg_extent_max_active_fit,
    size_t)
CTL_RO_NL_CGEN(config_prof, opt_prof, opt_prof, bool)
CTL_RO_NL_CGEN(config_prof, opt_prof_prefix, opt_prof_prefix, const char *)
CTL_RO_NL_CGEN(config_prof, opt_prof_active, opt_prof_active, bool)
CTL_RO_NL_CGEN(config_prof, opt_prof_thread_active_init,
    opt_prof_thread_active_init, bool)
CTL_RO_NL_CGEN(config_prof, opt_lg_prof_sample, opt_lg_prof_sample, size_t)
CTL_RO_NL_CGEN(config_prof, opt_prof_accum, opt_prof_accum, bool)
CTL_RO_NL_CGEN(config_prof, opt_lg_prof_interval, opt_lg_prof_interval, ssize_t)
CTL_RO_NL_CGEN(config_prof, opt_prof_gdump, opt_prof_gdump, bool)
CTL_RO_NL_CGEN(config_prof, opt_prof_final, opt_prof_final, bool)
CTL_RO_NL_CGEN(config_prof, opt_prof_leak, opt_prof_leak, bool)
CTL_RO_NL_CGEN(config_prof, opt_prof_leak_error, opt_prof_leak_error, bool)
CTL_RO_NL_CGEN(config_prof, opt_prof_recent_alloc_max,
    opt_prof_recent_alloc_max, ssize_t)
CTL_RO_NL_CGEN(config_prof, opt_prof_stats, opt_prof_stats, bool)
CTL_RO_NL_CGEN(config_prof, opt_prof_sys_thread_name, opt_prof_sys_thread_name,
    bool)
CTL_RO_NL_CGEN(config_prof, opt_prof_time_res,
    prof_time_res_mode_names[opt_prof_time_res], const char *)
CTL_RO_NL_CGEN(config_uaf_detection, opt_lg_san_uaf_align,
    opt_lg_san_uaf_align, ssize_t)
CTL_RO_NL_GEN(opt_zero_realloc,
    zero_realloc_mode_names[opt_zero_realloc_action], const char *)

/******************************************************************************/

static int
thread_arena_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	arena_t *oldarena;
	unsigned newind, oldind;

	oldarena = arena_choose(tsd, NULL);
	if (oldarena == NULL) {
		return EAGAIN;
	}
	newind = oldind = arena_ind_get(oldarena);
	WRITE(newind, unsigned);
	READ(oldind, unsigned);

	if (newind != oldind) {
		arena_t *newarena;

		if (newind >= narenas_total_get()) {
			/* New arena index is out of range. */
			ret = EFAULT;
			goto label_return;
		}

		if (have_percpu_arena &&
		    PERCPU_ARENA_ENABLED(opt_percpu_arena)) {
			if (newind < percpu_arena_ind_limit(opt_percpu_arena)) {
				/*
				 * If perCPU arena is enabled, thread_arena
				 * control is not allowed for the auto arena
				 * range.
				 */
				ret = EPERM;
				goto label_return;
			}
		}

		/* Initialize arena if necessary. */
		newarena = arena_get(tsd_tsdn(tsd), newind, true);
		if (newarena == NULL) {
			ret = EAGAIN;
			goto label_return;
		}
		/* Set new arena/tcache associations. */
		arena_migrate(tsd, oldarena, newarena);
		if (tcache_available(tsd)) {
			tcache_arena_reassociate(tsd_tsdn(tsd),
			    tsd_tcache_slowp_get(tsd), tsd_tcachep_get(tsd),
			    newarena);
		}
	}

	ret = 0;
label_return:
	return ret;
}

CTL_RO_NL_GEN(thread_allocated, tsd_thread_allocated_get(tsd), uint64_t)
CTL_RO_NL_GEN(thread_allocatedp, tsd_thread_allocatedp_get(tsd), uint64_t *)
CTL_RO_NL_GEN(thread_deallocated, tsd_thread_deallocated_get(tsd), uint64_t)
CTL_RO_NL_GEN(thread_deallocatedp, tsd_thread_deallocatedp_get(tsd), uint64_t *)

static int
thread_tcache_enabled_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen) {
	int ret;
	bool oldval;

	oldval = tcache_enabled_get(tsd);
	if (newp != NULL) {
		if (newlen != sizeof(bool)) {
			ret = EINVAL;
			goto label_return;
		}
		tcache_enabled_set(tsd, *(bool *)newp);
	}
	READ(oldval, bool);

	ret = 0;
label_return:
	return ret;
}

static int
thread_tcache_flush_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen) {
	int ret;

	if (!tcache_available(tsd)) {
		ret = EFAULT;
		goto label_return;
	}

	NEITHER_READ_NOR_WRITE();

	tcache_flush(tsd);

	ret = 0;
label_return:
	return ret;
}

static int
thread_peak_read_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen) {
	int ret;
	if (!config_stats) {
		return ENOENT;
	}
	READONLY();
	peak_event_update(tsd);
	uint64_t result = peak_event_max(tsd);
	READ(result, uint64_t);
	ret = 0;
label_return:
	return ret;
}

static int
thread_peak_reset_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen) {
	int ret;
	if (!config_stats) {
		return ENOENT;
	}
	NEITHER_READ_NOR_WRITE();
	peak_event_zero(tsd);
	ret = 0;
label_return:
	return ret;
}

static int
thread_prof_name_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen) {
	int ret;

	if (!config_prof || !opt_prof) {
		return ENOENT;
	}

	READ_XOR_WRITE();

	if (newp != NULL) {
		if (newlen != sizeof(const char *)) {
			ret = EINVAL;
			goto label_return;
		}

		if ((ret = prof_thread_name_set(tsd, *(const char **)newp)) !=
		    0) {
			goto label_return;
		}
	} else {
		const char *oldname = prof_thread_name_get(tsd);
		READ(oldname, const char *);
	}

	ret = 0;
label_return:
	return ret;
}

static int
thread_prof_active_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen) {
	int ret;
	bool oldval;

	if (!config_prof) {
		return ENOENT;
	}

	oldval = opt_prof ? prof_thread_active_get(tsd) : false;
	if (newp != NULL) {
		if (!opt_prof) {
			ret = ENOENT;
			goto label_return;
		}
		if (newlen != sizeof(bool)) {
			ret = EINVAL;
			goto label_return;
		}
		if (prof_thread_active_set(tsd, *(bool *)newp)) {
			ret = EAGAIN;
			goto label_return;
		}
	}
	READ(oldval, bool);

	ret = 0;
label_return:
	return ret;
}

static int
thread_idle_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen) {
	int ret;

	NEITHER_READ_NOR_WRITE();

	if (tcache_available(tsd)) {
		tcache_flush(tsd);
	}
	/*
	 * This heuristic is perhaps not the most well-considered.  But it
	 * matches the only idling policy we have experience with in the status
	 * quo.  Over time we should investigate more principled approaches.
	 */
	if (opt_narenas > ncpus * 2) {
		arena_t *arena = arena_choose(tsd, NULL);
		if (arena != NULL) {
			arena_decay(tsd_tsdn(tsd), arena, false, true);
		}
		/*
		 * The missing arena case is not actually an error; a thread
		 * might be idle before it associates itself to one.  This is
		 * unusual, but not wrong.
		 */
	}

	ret = 0;
label_return:
	return ret;
}

/******************************************************************************/

static int
tcache_create_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	unsigned tcache_ind;

	READONLY();
	VERIFY_READ(unsigned);
	if (tcaches_create(tsd, b0get(), &tcache_ind)) {
		ret = EFAULT;
		goto label_return;
	}
	READ(tcache_ind, unsigned);

	ret = 0;
label_return:
	return ret;
}

static int
tcache_flush_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	unsigned tcache_ind;

	WRITEONLY();
	ASSURED_WRITE(tcache_ind, unsigned);
	tcaches_flush(tsd, tcache_ind);

	ret = 0;
label_return:
	return ret;
}

static int
tcache_destroy_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	unsigned tcache_ind;

	WRITEONLY();
	ASSURED_WRITE(tcache_ind, unsigned);
	tcaches_destroy(tsd, tcache_ind);

	ret = 0;
label_return:
	return ret;
}

/******************************************************************************/

static int
arena_i_initialized_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	tsdn_t *tsdn = tsd_tsdn(tsd);
	unsigned arena_ind;
	bool initialized;

	READONLY();
	MIB_UNSIGNED(arena_ind, 1);

	malloc_mutex_lock(tsdn, &ctl_mtx);
	initialized = arenas_i(arena_ind)->initialized;
	malloc_mutex_unlock(tsdn, &ctl_mtx);

	READ(initialized, bool);

	ret = 0;
label_return:
	return ret;
}

static void
arena_i_decay(tsdn_t *tsdn, unsigned arena_ind, bool all) {
	malloc_mutex_lock(tsdn, &ctl_mtx);
	{
		unsigned narenas = ctl_arenas->narenas;

		/*
		 * Access via index narenas is deprecated, and scheduled for
		 * removal in 6.0.0.
		 */
		if (arena_ind == MALLCTL_ARENAS_ALL || arena_ind == narenas) {
			unsigned i;
			VARIABLE_ARRAY(arena_t *, tarenas, narenas);

			for (i = 0; i < narenas; i++) {
				tarenas[i] = arena_get(tsdn, i, false);
			}

			/*
			 * No further need to hold ctl_mtx, since narenas and
			 * tarenas contain everything needed below.
			 */
			malloc_mutex_unlock(tsdn, &ctl_mtx);

			for (i = 0; i < narenas; i++) {
				if (tarenas[i] != NULL) {
					arena_decay(tsdn, tarenas[i], false,
					    all);
				}
			}
		} else {
			arena_t *tarena;

			assert(arena_ind < narenas);

			tarena = arena_get(tsdn, arena_ind, false);

			/* No further need to hold ctl_mtx. */
			malloc_mutex_unlock(tsdn, &ctl_mtx);

			if (tarena != NULL) {
				arena_decay(tsdn, tarena, false, all);
			}
		}
	}
}

static int
arena_i_decay_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	unsigned arena_ind;

	NEITHER_READ_NOR_WRITE();
	MIB_UNSIGNED(arena_ind, 1);
	arena_i_decay(tsd_tsdn(tsd), arena_ind, false);

	ret = 0;
label_return:
	return ret;
}

static int
arena_i_purge_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	unsigned arena_ind;

	NEITHER_READ_NOR_WRITE();
	MIB_UNSIGNED(arena_ind, 1);
	arena_i_decay(tsd_tsdn(tsd), arena_ind, true);

	ret = 0;
label_return:
	return ret;
}

static int
arena_i_reset_destroy_helper(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen, unsigned *arena_ind,
    arena_t **arena) {
	int ret;

	NEITHER_READ_NOR_WRITE();
	MIB_UNSIGNED(*arena_ind, 1);

	*arena = arena_get(tsd_tsdn(tsd), *arena_ind, false);
	if (*arena == NULL || arena_is_auto(*arena)) {
		ret = EFAULT;
		goto label_return;
	}

	ret = 0;
label_return:
	return ret;
}

static void
arena_reset_prepare_background_thread(tsd_t *tsd, unsigned arena_ind) {
	/* Temporarily disable the background thread during arena reset. */
	if (have_background_thread) {
		malloc_mutex_lock(tsd_tsdn(tsd), &background_thread_lock);
		if (background_thread_enabled()) {
			background_thread_info_t *info =
			    background_thread_info_get(arena_ind);
			assert(info->state == background_thread_started);
			malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
			info->state = background_thread_paused;
			malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
		}
	}
}

static void
arena_reset_finish_background_thread(tsd_t *tsd, unsigned arena_ind) {
	if (have_background_thread) {
		if (background_thread_enabled()) {
			background_thread_info_t *info =
			    background_thread_info_get(arena_ind);
			assert(info->state == background_thread_paused);
			malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
			info->state = background_thread_started;
			malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &background_thread_lock);
	}
}

static int
arena_i_reset_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	unsigned arena_ind;
	arena_t *arena;

	ret = arena_i_reset_destroy_helper(tsd, mib, miblen, oldp, oldlenp,
	    newp, newlen, &arena_ind, &arena);
	if (ret != 0) {
		return ret;
	}

	arena_reset_prepare_background_thread(tsd, arena_ind);
	arena_reset(tsd, arena);
	arena_reset_finish_background_thread(tsd, arena_ind);

	return ret;
}

static int
arena_i_destroy_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	unsigned arena_ind;
	arena_t *arena;
	ctl_arena_t *ctl_darena, *ctl_arena;

	malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);

	ret = arena_i_reset_destroy_helper(tsd, mib, miblen, oldp, oldlenp,
	    newp, newlen, &arena_ind, &arena);
	if (ret != 0) {
		goto label_return;
	}

	if (arena_nthreads_get(arena, false) != 0 || arena_nthreads_get(arena,
	    true) != 0) {
		ret = EFAULT;
		goto label_return;
	}

	arena_reset_prepare_background_thread(tsd, arena_ind);
	/* Merge stats after resetting and purging arena. */
	arena_reset(tsd, arena);
	arena_decay(tsd_tsdn(tsd), arena, false, true);
	ctl_darena = arenas_i(MALLCTL_ARENAS_DESTROYED);
	ctl_darena->initialized = true;
	ctl_arena_refresh(tsd_tsdn(tsd), arena, ctl_darena, arena_ind, true);
	/* Destroy arena. */
	arena_destroy(tsd, arena);
	ctl_arena = arenas_i(arena_ind);
	ctl_arena->initialized = false;
	/* Record arena index for later recycling via arenas.create. */
	ql_elm_new(ctl_arena, destroyed_link);
	ql_tail_insert(&ctl_arenas->destroyed, ctl_arena, destroyed_link);
	arena_reset_finish_background_thread(tsd, arena_ind);

	assert(ret == 0);
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);

	return ret;
}

static int
arena_i_dss_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	const char *dss = NULL;
	unsigned arena_ind;
	dss_prec_t dss_prec_old = dss_prec_limit;
	dss_prec_t dss_prec = dss_prec_limit;

	malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);
	WRITE(dss, const char *);
	MIB_UNSIGNED(arena_ind, 1);
	if (dss != NULL) {
		int i;
		bool match = false;

		for (i = 0; i < dss_prec_limit; i++) {
			if (strcmp(dss_prec_names[i], dss) == 0) {
				dss_prec = i;
				match = true;
				break;
			}
		}

		if (!match) {
			ret = EINVAL;
			goto label_return;
		}
	}

	/*
	 * Access via index narenas is deprecated, and scheduled for removal in
	 * 6.0.0.
	 */
	if (arena_ind == MALLCTL_ARENAS_ALL || arena_ind ==
	    ctl_arenas->narenas) {
		if (dss_prec != dss_prec_limit &&
		    extent_dss_prec_set(dss_prec)) {
			ret = EFAULT;
			goto label_return;
		}
		dss_prec_old = extent_dss_prec_get();
	} else {
		arena_t *arena = arena_get(tsd_tsdn(tsd), arena_ind, false);
		if (arena == NULL || (dss_prec != dss_prec_limit &&
		    arena_dss_prec_set(arena, dss_prec))) {
			ret = EFAULT;
			goto label_return;
		}
		dss_prec_old = arena_dss_prec_get(arena);
	}

	dss = dss_prec_names[dss_prec_old];
	READ(dss, const char *);

	ret = 0;
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);
	return ret;
}

static int
arena_i_oversize_threshold_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;

	unsigned arena_ind;
	MIB_UNSIGNED(arena_ind, 1);

	arena_t *arena = arena_get(tsd_tsdn(tsd), arena_ind, false);
	if (arena == NULL) {
		ret = EFAULT;
		goto label_return;
	}

	if (oldp != NULL && oldlenp != NULL) {
		size_t oldval = atomic_load_zu(
		    &arena->pa_shard.pac.oversize_threshold, ATOMIC_RELAXED);
		READ(oldval, size_t);
	}
	if (newp != NULL) {
		if (newlen != sizeof(size_t)) {
			ret = EINVAL;
			goto label_return;
		}
		atomic_store_zu(&arena->pa_shard.pac.oversize_threshold,
		    *(size_t *)newp, ATOMIC_RELAXED);
	}
	ret = 0;
label_return:
	return ret;
}

static int
arena_i_decay_ms_ctl_impl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen, bool dirty) {
	int ret;
	unsigned arena_ind;
	arena_t *arena;

	MIB_UNSIGNED(arena_ind, 1);
	arena = arena_get(tsd_tsdn(tsd), arena_ind, false);
	if (arena == NULL) {
		ret = EFAULT;
		goto label_return;
	}
	extent_state_t state = dirty ? extent_state_dirty : extent_state_muzzy;

	if (oldp != NULL && oldlenp != NULL) {
		size_t oldval = arena_decay_ms_get(arena, state);
		READ(oldval, ssize_t);
	}
	if (newp != NULL) {
		if (newlen != sizeof(ssize_t)) {
			ret = EINVAL;
			goto label_return;
		}
		if (arena_is_huge(arena_ind) && *(ssize_t *)newp > 0) {
			/*
			 * By default the huge arena purges eagerly.  If it is
			 * set to non-zero decay time afterwards, background
			 * thread might be needed.
			 */
			if (background_thread_create(tsd, arena_ind)) {
				ret = EFAULT;
				goto label_return;
			}
		}

		if (arena_decay_ms_set(tsd_tsdn(tsd), arena, state,
		    *(ssize_t *)newp)) {
			ret = EFAULT;
			goto label_return;
		}
	}

	ret = 0;
label_return:
	return ret;
}

static int
arena_i_dirty_decay_ms_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	return arena_i_decay_ms_ctl_impl(tsd, mib, miblen, oldp, oldlenp, newp,
	    newlen, true);
}

static int
arena_i_muzzy_decay_ms_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	return arena_i_decay_ms_ctl_impl(tsd, mib, miblen, oldp, oldlenp, newp,
	    newlen, false);
}

static int
arena_i_extent_hooks_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	unsigned arena_ind;
	arena_t *arena;

	malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);
	MIB_UNSIGNED(arena_ind, 1);
	if (arena_ind < narenas_total_get()) {
		extent_hooks_t *old_extent_hooks;
		arena = arena_get(tsd_tsdn(tsd), arena_ind, false);
		if (arena == NULL) {
			if (arena_ind >= narenas_auto) {
				ret = EFAULT;
				goto label_return;
			}
			old_extent_hooks =
			    (extent_hooks_t *)&ehooks_default_extent_hooks;
			READ(old_extent_hooks, extent_hooks_t *);
			if (newp != NULL) {
				/* Initialize a new arena as a side effect. */
				extent_hooks_t *new_extent_hooks
				    JEMALLOC_CC_SILENCE_INIT(NULL);
				WRITE(new_extent_hooks, extent_hooks_t *);
				arena_config_t config = arena_config_default;
				config.extent_hooks = new_extent_hooks;

				arena = arena_init(tsd_tsdn(tsd), arena_ind,
				    &config);
				if (arena == NULL) {
					ret = EFAULT;
					goto label_return;
				}
			}
		} else {
			if (newp != NULL) {
				extent_hooks_t *new_extent_hooks
				    JEMALLOC_CC_SILENCE_INIT(NULL);
				WRITE(new_extent_hooks, extent_hooks_t *);
				old_extent_hooks = arena_set_extent_hooks(tsd,
				    arena, new_extent_hooks);
				READ(old_extent_hooks, extent_hooks_t *);
			} else {
				old_extent_hooks =
				    ehooks_get_extent_hooks_ptr(
					arena_get_ehooks(arena));
				READ(old_extent_hooks, extent_hooks_t *);
			}
		}
	} else {
		ret = EFAULT;
		goto label_return;
	}
	ret = 0;
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);
	return ret;
}

static int
arena_i_retain_grow_limit_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen) {
	int ret;
	unsigned arena_ind;
	arena_t *arena;

	if (!opt_retain) {
		/* Only relevant when retain is enabled. */
		return ENOENT;
	}

	malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);
	MIB_UNSIGNED(arena_ind, 1);
	if (arena_ind < narenas_total_get() && (arena =
	    arena_get(tsd_tsdn(tsd), arena_ind, false)) != NULL) {
		size_t old_limit, new_limit;
		if (newp != NULL) {
			WRITE(new_limit, size_t);
		}
		bool err = arena_retain_grow_limit_get_set(tsd, arena,
		    &old_limit, newp != NULL ? &new_limit : NULL);
		if (!err) {
			READ(old_limit, size_t);
			ret = 0;
		} else {
			ret = EFAULT;
		}
	} else {
		ret = EFAULT;
	}
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);
	return ret;
}

static const ctl_named_node_t *
arena_i_index(tsdn_t *tsdn, const size_t *mib, size_t miblen,
    size_t i) {
	const ctl_named_node_t *ret;

	malloc_mutex_lock(tsdn, &ctl_mtx);
	switch (i) {
	case MALLCTL_ARENAS_ALL:
	case MALLCTL_ARENAS_DESTROYED:
		break;
	default:
		if (i > ctl_arenas->narenas) {
			ret = NULL;
			goto label_return;
		}
		break;
	}

	ret = super_arena_i_node;
label_return:
	malloc_mutex_unlock(tsdn, &ctl_mtx);
	return ret;
}

/******************************************************************************/

static int
arenas_narenas_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	unsigned narenas;

	malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);
	READONLY();
	narenas = ctl_arenas->narenas;
	READ(narenas, unsigned);

	ret = 0;
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);
	return ret;
}

static int
arenas_decay_ms_ctl_impl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen, bool dirty) {
	int ret;

	if (oldp != NULL && oldlenp != NULL) {
		size_t oldval = (dirty ? arena_dirty_decay_ms_default_get() :
		    arena_muzzy_decay_ms_default_get());
		READ(oldval, ssize_t);
	}
	if (newp != NULL) {
		if (newlen != sizeof(ssize_t)) {
			ret = EINVAL;
			goto label_return;
		}
		if (dirty ? arena_dirty_decay_ms_default_set(*(ssize_t *)newp)
		    : arena_muzzy_decay_ms_default_set(*(ssize_t *)newp)) {
			ret = EFAULT;
			goto label_return;
		}
	}

	ret = 0;
label_return:
	return ret;
}

static int
arenas_dirty_decay_ms_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	return arenas_decay_ms_ctl_impl(tsd, mib, miblen, oldp, oldlenp, newp,
	    newlen, true);
}

static int
arenas_muzzy_decay_ms_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	return arenas_decay_ms_ctl_impl(tsd, mib, miblen, oldp, oldlenp, newp,
	    newlen, false);
}

CTL_RO_NL_GEN(arenas_quantum, QUANTUM, size_t)
CTL_RO_NL_GEN(arenas_page, PAGE, size_t)
CTL_RO_NL_GEN(arenas_tcache_max, tcache_maxclass, size_t)
CTL_RO_NL_GEN(arenas_nbins, SC_NBINS, unsigned)
CTL_RO_NL_GEN(arenas_nhbins, nhbins, unsigned)
CTL_RO_NL_GEN(arenas_bin_i_size, bin_infos[mib[2]].reg_size, size_t)
CTL_RO_NL_GEN(arenas_bin_i_nregs, bin_infos[mib[2]].nregs, uint32_t)
CTL_RO_NL_GEN(arenas_bin_i_slab_size, bin_infos[mib[2]].slab_size, size_t)
CTL_RO_NL_GEN(arenas_bin_i_nshards, bin_infos[mib[2]].n_shards, uint32_t)
static const ctl_named_node_t *
arenas_bin_i_index(tsdn_t *tsdn, const size_t *mib,
    size_t miblen, size_t i) {
	if (i > SC_NBINS) {
		return NULL;
	}
	return super_arenas_bin_i_node;
}

CTL_RO_NL_GEN(arenas_nlextents, SC_NSIZES - SC_NBINS, unsigned)
CTL_RO_NL_GEN(arenas_lextent_i_size, sz_index2size(SC_NBINS+(szind_t)mib[2]),
    size_t)
static const ctl_named_node_t *
arenas_lextent_i_index(tsdn_t *tsdn, const size_t *mib,
    size_t miblen, size_t i) {
	if (i > SC_NSIZES - SC_NBINS) {
		return NULL;
	}
	return super_arenas_lextent_i_node;
}

static int
arenas_create_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	unsigned arena_ind;

	malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);

	VERIFY_READ(unsigned);
	arena_config_t config = arena_config_default;
	WRITE(config.extent_hooks, extent_hooks_t *);
	if ((arena_ind = ctl_arena_init(tsd, &config)) == UINT_MAX) {
		ret = EAGAIN;
		goto label_return;
	}
	READ(arena_ind, unsigned);

	ret = 0;
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);
	return ret;
}

static int
experimental_arenas_create_ext_ctl(tsd_t *tsd,
    const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	unsigned arena_ind;

	malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);

	arena_config_t config = arena_config_default;
	VERIFY_READ(unsigned);
	WRITE(config, arena_config_t);

	if ((arena_ind = ctl_arena_init(tsd, &config)) == UINT_MAX) {
		ret = EAGAIN;
		goto label_return;
	}
	READ(arena_ind, unsigned);
	ret = 0;
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);
	return ret;
}

static int
arenas_lookup_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen) {
	int ret;
	unsigned arena_ind;
	void *ptr;
	edata_t *edata;
	arena_t *arena;

	ptr = NULL;
	ret = EINVAL;
	malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);
	WRITE(ptr, void *);
	edata = emap_edata_lookup(tsd_tsdn(tsd), &arena_emap_global, ptr);
	if (edata == NULL) {
		goto label_return;
	}

	arena = arena_get_from_edata(edata);
	if (arena == NULL) {
		goto label_return;
	}

	arena_ind = arena_ind_get(arena);
	READ(arena_ind, unsigned);

	ret = 0;
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);
	return ret;
}

/******************************************************************************/

static int
prof_thread_active_init_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen) {
	int ret;
	bool oldval;

	if (!config_prof) {
		return ENOENT;
	}

	if (newp != NULL) {
		if (!opt_prof) {
			ret = ENOENT;
			goto label_return;
		}
		if (newlen != sizeof(bool)) {
			ret = EINVAL;
			goto label_return;
		}
		oldval = prof_thread_active_init_set(tsd_tsdn(tsd),
		    *(bool *)newp);
	} else {
		oldval = opt_prof ? prof_thread_active_init_get(tsd_tsdn(tsd)) :
		    false;
	}
	READ(oldval, bool);

	ret = 0;
label_return:
	return ret;
}

static int
prof_active_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	bool oldval;

	if (!config_prof) {
		ret = ENOENT;
		goto label_return;
	}

	if (newp != NULL) {
		if (newlen != sizeof(bool)) {
			ret = EINVAL;
			goto label_return;
		}
		bool val = *(bool *)newp;
		if (!opt_prof) {
			if (val) {
				ret = ENOENT;
				goto label_return;
			} else {
				/* No change needed (already off). */
				oldval = false;
			}
		} else {
			oldval = prof_active_set(tsd_tsdn(tsd), val);
		}
	} else {
		oldval = opt_prof ? prof_active_get(tsd_tsdn(tsd)) : false;
	}
	READ(oldval, bool);

	ret = 0;
label_return:
	return ret;
}

static int
prof_dump_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	const char *filename = NULL;

	if (!config_prof || !opt_prof) {
		return ENOENT;
	}

	WRITEONLY();
	WRITE(filename, const char *);

	if (prof_mdump(tsd, filename)) {
		ret = EFAULT;
		goto label_return;
	}

	ret = 0;
label_return:
	return ret;
}

static int
prof_gdump_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	bool oldval;

	if (!config_prof) {
		return ENOENT;
	}

	if (newp != NULL) {
		if (!opt_prof) {
			ret = ENOENT;
			goto label_return;
		}
		if (newlen != sizeof(bool)) {
			ret = EINVAL;
			goto label_return;
		}
		oldval = prof_gdump_set(tsd_tsdn(tsd), *(bool *)newp);
	} else {
		oldval = opt_prof ? prof_gdump_get(tsd_tsdn(tsd)) : false;
	}
	READ(oldval, bool);

	ret = 0;
label_return:
	return ret;
}

static int
prof_prefix_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	const char *prefix = NULL;

	if (!config_prof || !opt_prof) {
		return ENOENT;
	}

	malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);
	WRITEONLY();
	WRITE(prefix, const char *);

	ret = prof_prefix_set(tsd_tsdn(tsd), prefix) ? EFAULT : 0;
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);
	return ret;
}

static int
prof_reset_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	size_t lg_sample = lg_prof_sample;

	if (!config_prof || !opt_prof) {
		return ENOENT;
	}

	WRITEONLY();
	WRITE(lg_sample, size_t);
	if (lg_sample >= (sizeof(uint64_t) << 3)) {
		lg_sample = (sizeof(uint64_t) << 3) - 1;
	}

	prof_reset(tsd, lg_sample);

	ret = 0;
label_return:
	return ret;
}

CTL_RO_NL_CGEN(config_prof, prof_interval, prof_interval, uint64_t)
CTL_RO_NL_CGEN(config_prof, lg_prof_sample, lg_prof_sample, size_t)

static int
prof_log_start_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	int ret;

	const char *filename = NULL;

	if (!config_prof || !opt_prof) {
		return ENOENT;
	}

	WRITEONLY();
	WRITE(filename, const char *);

	if (prof_log_start(tsd_tsdn(tsd), filename)) {
		ret = EFAULT;
		goto label_return;
	}

	ret = 0;
label_return:
	return ret;
}

static int
prof_log_stop_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	if (!config_prof || !opt_prof) {
		return ENOENT;
	}

	if (prof_log_stop(tsd_tsdn(tsd))) {
		return EFAULT;
	}

	return 0;
}

static int
experimental_hooks_prof_backtrace_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;

	if (oldp == NULL && newp == NULL) {
		ret = EINVAL;
		goto label_return;
	}
	if (oldp != NULL) {
		prof_backtrace_hook_t old_hook =
		    prof_backtrace_hook_get();
		READ(old_hook, prof_backtrace_hook_t);
	}
	if (newp != NULL) {
		if (!opt_prof) {
			ret = ENOENT;
			goto label_return;
		}
		prof_backtrace_hook_t new_hook JEMALLOC_CC_SILENCE_INIT(NULL);
		WRITE(new_hook, prof_backtrace_hook_t);
		if (new_hook == NULL) {
			ret = EINVAL;
			goto label_return;
		}
		prof_backtrace_hook_set(new_hook);
	}
	ret = 0;
label_return:
	return ret;
}

static int
experimental_hooks_prof_dump_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;

	if (oldp == NULL && newp == NULL) {
		ret = EINVAL;
		goto label_return;
	}
	if (oldp != NULL) {
		prof_dump_hook_t old_hook =
		    prof_dump_hook_get();
		READ(old_hook, prof_dump_hook_t);
	}
	if (newp != NULL) {
		if (!opt_prof) {
			ret = ENOENT;
			goto label_return;
		}
		prof_dump_hook_t new_hook JEMALLOC_CC_SILENCE_INIT(NULL);
		WRITE(new_hook, prof_dump_hook_t);
		prof_dump_hook_set(new_hook);
	}
	ret = 0;
label_return:
	return ret;
}

/* For integration test purpose only.  No plan to move out of experimental. */
static int
experimental_hooks_safety_check_abort_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;

	WRITEONLY();
	if (newp != NULL) {
		if (newlen != sizeof(safety_check_abort_hook_t)) {
			ret = EINVAL;
			goto label_return;
		}
		safety_check_abort_hook_t hook JEMALLOC_CC_SILENCE_INIT(NULL);
		WRITE(hook, safety_check_abort_hook_t);
		safety_check_set_abort(hook);
	}
	ret = 0;
label_return:
	return ret;
}

/******************************************************************************/

CTL_RO_CGEN(config_stats, stats_allocated, ctl_stats->allocated, size_t)
CTL_RO_CGEN(config_stats, stats_active, ctl_stats->active, size_t)
CTL_RO_CGEN(config_stats, stats_metadata, ctl_stats->metadata, size_t)
CTL_RO_CGEN(config_stats, stats_metadata_thp, ctl_stats->metadata_thp, size_t)
CTL_RO_CGEN(config_stats, stats_resident, ctl_stats->resident, size_t)
CTL_RO_CGEN(config_stats, stats_mapped, ctl_stats->mapped, size_t)
CTL_RO_CGEN(config_stats, stats_retained, ctl_stats->retained, size_t)

CTL_RO_CGEN(config_stats, stats_background_thread_num_threads,
    ctl_stats->background_thread.num_threads, size_t)
CTL_RO_CGEN(config_stats, stats_background_thread_num_runs,
    ctl_stats->background_thread.num_runs, uint64_t)
CTL_RO_CGEN(config_stats, stats_background_thread_run_interval,
    nstime_ns(&ctl_stats->background_thread.run_interval), uint64_t)

CTL_RO_CGEN(config_stats, stats_zero_reallocs,
    atomic_load_zu(&zero_realloc_count, ATOMIC_RELAXED), size_t)

CTL_RO_GEN(stats_arenas_i_dss, arenas_i(mib[2])->dss, const char *)
CTL_RO_GEN(stats_arenas_i_dirty_decay_ms, arenas_i(mib[2])->dirty_decay_ms,
    ssize_t)
CTL_RO_GEN(stats_arenas_i_muzzy_decay_ms, arenas_i(mib[2])->muzzy_decay_ms,
    ssize_t)
CTL_RO_GEN(stats_arenas_i_nthreads, arenas_i(mib[2])->nthreads, unsigned)
CTL_RO_GEN(stats_arenas_i_uptime,
    nstime_ns(&arenas_i(mib[2])->astats->astats.uptime), uint64_t)
CTL_RO_GEN(stats_arenas_i_pactive, arenas_i(mib[2])->pactive, size_t)
CTL_RO_GEN(stats_arenas_i_pdirty, arenas_i(mib[2])->pdirty, size_t)
CTL_RO_GEN(stats_arenas_i_pmuzzy, arenas_i(mib[2])->pmuzzy, size_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_mapped,
    arenas_i(mib[2])->astats->astats.mapped, size_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_retained,
    arenas_i(mib[2])->astats->astats.pa_shard_stats.pac_stats.retained, size_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_extent_avail,
    arenas_i(mib[2])->astats->astats.pa_shard_stats.edata_avail, size_t)

CTL_RO_CGEN(config_stats, stats_arenas_i_dirty_npurge,
    locked_read_u64_unsynchronized(
    &arenas_i(mib[2])->astats->astats.pa_shard_stats.pac_stats.decay_dirty.npurge),
    uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_dirty_nmadvise,
    locked_read_u64_unsynchronized(
    &arenas_i(mib[2])->astats->astats.pa_shard_stats.pac_stats.decay_dirty.nmadvise),
    uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_dirty_purged,
    locked_read_u64_unsynchronized(
    &arenas_i(mib[2])->astats->astats.pa_shard_stats.pac_stats.decay_dirty.purged),
    uint64_t)

CTL_RO_CGEN(config_stats, stats_arenas_i_muzzy_npurge,
    locked_read_u64_unsynchronized(
    &arenas_i(mib[2])->astats->astats.pa_shard_stats.pac_stats.decay_muzzy.npurge),
    uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_muzzy_nmadvise,
    locked_read_u64_unsynchronized(
    &arenas_i(mib[2])->astats->astats.pa_shard_stats.pac_stats.decay_muzzy.nmadvise),
    uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_muzzy_purged,
    locked_read_u64_unsynchronized(
    &arenas_i(mib[2])->astats->astats.pa_shard_stats.pac_stats.decay_muzzy.purged),
    uint64_t)

CTL_RO_CGEN(config_stats, stats_arenas_i_base,
    arenas_i(mib[2])->astats->astats.base,
    size_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_internal,
    atomic_load_zu(&arenas_i(mib[2])->astats->astats.internal, ATOMIC_RELAXED),
    size_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_metadata_thp,
    arenas_i(mib[2])->astats->astats.metadata_thp, size_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_tcache_bytes,
    arenas_i(mib[2])->astats->astats.tcache_bytes, size_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_tcache_stashed_bytes,
    arenas_i(mib[2])->astats->astats.tcache_stashed_bytes, size_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_resident,
    arenas_i(mib[2])->astats->astats.resident,
    size_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_abandoned_vm,
    atomic_load_zu(
    &arenas_i(mib[2])->astats->astats.pa_shard_stats.pac_stats.abandoned_vm,
    ATOMIC_RELAXED), size_t)

CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_sec_bytes,
    arenas_i(mib[2])->astats->secstats.bytes, size_t)

CTL_RO_CGEN(config_stats, stats_arenas_i_small_allocated,
    arenas_i(mib[2])->astats->allocated_small, size_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_small_nmalloc,
    arenas_i(mib[2])->astats->nmalloc_small, uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_small_ndalloc,
    arenas_i(mib[2])->astats->ndalloc_small, uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_small_nrequests,
    arenas_i(mib[2])->astats->nrequests_small, uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_small_nfills,
    arenas_i(mib[2])->astats->nfills_small, uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_small_nflushes,
    arenas_i(mib[2])->astats->nflushes_small, uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_large_allocated,
    arenas_i(mib[2])->astats->astats.allocated_large, size_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_large_nmalloc,
    arenas_i(mib[2])->astats->astats.nmalloc_large, uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_large_ndalloc,
    arenas_i(mib[2])->astats->astats.ndalloc_large, uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_large_nrequests,
    arenas_i(mib[2])->astats->astats.nrequests_large, uint64_t)
/*
 * Note: "nmalloc_large" here instead of "nfills" in the read.  This is
 * intentional (large has no batch fill).
 */
CTL_RO_CGEN(config_stats, stats_arenas_i_large_nfills,
    arenas_i(mib[2])->astats->astats.nmalloc_large, uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_large_nflushes,
    arenas_i(mib[2])->astats->astats.nflushes_large, uint64_t)

/* Lock profiling related APIs below. */
#define RO_MUTEX_CTL_GEN(n, l)						\
CTL_RO_CGEN(config_stats, stats_##n##_num_ops,				\
    l.n_lock_ops, uint64_t)						\
CTL_RO_CGEN(config_stats, stats_##n##_num_wait,				\
    l.n_wait_times, uint64_t)						\
CTL_RO_CGEN(config_stats, stats_##n##_num_spin_acq,			\
    l.n_spin_acquired, uint64_t)					\
CTL_RO_CGEN(config_stats, stats_##n##_num_owner_switch,			\
    l.n_owner_switches, uint64_t) 					\
CTL_RO_CGEN(config_stats, stats_##n##_total_wait_time,			\
    nstime_ns(&l.tot_wait_time), uint64_t)				\
CTL_RO_CGEN(config_stats, stats_##n##_max_wait_time,			\
    nstime_ns(&l.max_wait_time), uint64_t)				\
CTL_RO_CGEN(config_stats, stats_##n##_max_num_thds,			\
    l.max_n_thds, uint32_t)

/* Global mutexes. */
#define OP(mtx)								\
    RO_MUTEX_CTL_GEN(mutexes_##mtx,					\
        ctl_stats->mutex_prof_data[global_prof_mutex_##mtx])
MUTEX_PROF_GLOBAL_MUTEXES
#undef OP

/* Per arena mutexes */
#define OP(mtx) RO_MUTEX_CTL_GEN(arenas_i_mutexes_##mtx,		\
    arenas_i(mib[2])->astats->astats.mutex_prof_data[arena_prof_mutex_##mtx])
MUTEX_PROF_ARENA_MUTEXES
#undef OP

/* tcache bin mutex */
RO_MUTEX_CTL_GEN(arenas_i_bins_j_mutex,
    arenas_i(mib[2])->astats->bstats[mib[4]].mutex_data)
#undef RO_MUTEX_CTL_GEN

/* Resets all mutex stats, including global, arena and bin mutexes. */
static int
stats_mutexes_reset_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen) {
	if (!config_stats) {
		return ENOENT;
	}

	tsdn_t *tsdn = tsd_tsdn(tsd);

#define MUTEX_PROF_RESET(mtx)						\
    malloc_mutex_lock(tsdn, &mtx);					\
    malloc_mutex_prof_data_reset(tsdn, &mtx);				\
    malloc_mutex_unlock(tsdn, &mtx);

	/* Global mutexes: ctl and prof. */
	MUTEX_PROF_RESET(ctl_mtx);
	if (have_background_thread) {
		MUTEX_PROF_RESET(background_thread_lock);
	}
	if (config_prof && opt_prof) {
		MUTEX_PROF_RESET(bt2gctx_mtx);
		MUTEX_PROF_RESET(tdatas_mtx);
		MUTEX_PROF_RESET(prof_dump_mtx);
		MUTEX_PROF_RESET(prof_recent_alloc_mtx);
		MUTEX_PROF_RESET(prof_recent_dump_mtx);
		MUTEX_PROF_RESET(prof_stats_mtx);
	}

	/* Per arena mutexes. */
	unsigned n = narenas_total_get();

	for (unsigned i = 0; i < n; i++) {
		arena_t *arena = arena_get(tsdn, i, false);
		if (!arena) {
			continue;
		}
		MUTEX_PROF_RESET(arena->large_mtx);
		MUTEX_PROF_RESET(arena->pa_shard.edata_cache.mtx);
		MUTEX_PROF_RESET(arena->pa_shard.pac.ecache_dirty.mtx);
		MUTEX_PROF_RESET(arena->pa_shard.pac.ecache_muzzy.mtx);
		MUTEX_PROF_RESET(arena->pa_shard.pac.ecache_retained.mtx);
		MUTEX_PROF_RESET(arena->pa_shard.pac.decay_dirty.mtx);
		MUTEX_PROF_RESET(arena->pa_shard.pac.decay_muzzy.mtx);
		MUTEX_PROF_RESET(arena->tcache_ql_mtx);
		MUTEX_PROF_RESET(arena->base->mtx);

		for (szind_t j = 0; j < SC_NBINS; j++) {
			for (unsigned k = 0; k < bin_infos[j].n_shards; k++) {
				bin_t *bin = arena_get_bin(arena, j, k);
				MUTEX_PROF_RESET(bin->lock);
			}
		}
	}
#undef MUTEX_PROF_RESET
	return 0;
}

CTL_RO_CGEN(config_stats, stats_arenas_i_bins_j_nmalloc,
    arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.nmalloc, uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_bins_j_ndalloc,
    arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.ndalloc, uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_bins_j_nrequests,
    arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.nrequests, uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_bins_j_curregs,
    arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.curregs, size_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_bins_j_nfills,
    arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.nfills, uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_bins_j_nflushes,
    arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.nflushes, uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_bins_j_nslabs,
    arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.nslabs, uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_bins_j_nreslabs,
    arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.reslabs, uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_bins_j_curslabs,
    arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.curslabs, size_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_bins_j_nonfull_slabs,
    arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.nonfull_slabs, size_t)

static const ctl_named_node_t *
stats_arenas_i_bins_j_index(tsdn_t *tsdn, const size_t *mib,
    size_t miblen, size_t j) {
	if (j > SC_NBINS) {
		return NULL;
	}
	return super_stats_arenas_i_bins_j_node;
}

CTL_RO_CGEN(config_stats, stats_arenas_i_lextents_j_nmalloc,
    locked_read_u64_unsynchronized(
    &arenas_i(mib[2])->astats->lstats[mib[4]].nmalloc), uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_lextents_j_ndalloc,
    locked_read_u64_unsynchronized(
    &arenas_i(mib[2])->astats->lstats[mib[4]].ndalloc), uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_lextents_j_nrequests,
    locked_read_u64_unsynchronized(
    &arenas_i(mib[2])->astats->lstats[mib[4]].nrequests), uint64_t)
CTL_RO_CGEN(config_stats, stats_arenas_i_lextents_j_curlextents,
    arenas_i(mib[2])->astats->lstats[mib[4]].curlextents, size_t)

static const ctl_named_node_t *
stats_arenas_i_lextents_j_index(tsdn_t *tsdn, const size_t *mib,
    size_t miblen, size_t j) {
	if (j > SC_NSIZES - SC_NBINS) {
		return NULL;
	}
	return super_stats_arenas_i_lextents_j_node;
}

CTL_RO_CGEN(config_stats, stats_arenas_i_extents_j_ndirty,
        arenas_i(mib[2])->astats->estats[mib[4]].ndirty, size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_extents_j_nmuzzy,
        arenas_i(mib[2])->astats->estats[mib[4]].nmuzzy, size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_extents_j_nretained,
        arenas_i(mib[2])->astats->estats[mib[4]].nretained, size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_extents_j_dirty_bytes,
        arenas_i(mib[2])->astats->estats[mib[4]].dirty_bytes, size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_extents_j_muzzy_bytes,
        arenas_i(mib[2])->astats->estats[mib[4]].muzzy_bytes, size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_extents_j_retained_bytes,
        arenas_i(mib[2])->astats->estats[mib[4]].retained_bytes, size_t);

static const ctl_named_node_t *
stats_arenas_i_extents_j_index(tsdn_t *tsdn, const size_t *mib,
    size_t miblen, size_t j) {
	if (j >= SC_NPSIZES) {
		return NULL;
	}
	return super_stats_arenas_i_extents_j_node;
}

CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_npurge_passes,
    arenas_i(mib[2])->astats->hpastats.nonderived_stats.npurge_passes, uint64_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_npurges,
    arenas_i(mib[2])->astats->hpastats.nonderived_stats.npurges, uint64_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_nhugifies,
    arenas_i(mib[2])->astats->hpastats.nonderived_stats.nhugifies, uint64_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_ndehugifies,
    arenas_i(mib[2])->astats->hpastats.nonderived_stats.ndehugifies, uint64_t);

/* Full, nonhuge */
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_full_slabs_npageslabs_nonhuge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.full_slabs[0].npageslabs,
    size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_full_slabs_nactive_nonhuge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.full_slabs[0].nactive, size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_full_slabs_ndirty_nonhuge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.full_slabs[0].ndirty, size_t);

/* Full, huge */
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_full_slabs_npageslabs_huge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.full_slabs[1].npageslabs,
    size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_full_slabs_nactive_huge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.full_slabs[1].nactive, size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_full_slabs_ndirty_huge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.full_slabs[1].ndirty, size_t);

/* Empty, nonhuge */
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_empty_slabs_npageslabs_nonhuge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.empty_slabs[0].npageslabs,
    size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_empty_slabs_nactive_nonhuge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.empty_slabs[0].nactive, size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_empty_slabs_ndirty_nonhuge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.empty_slabs[0].ndirty, size_t);

/* Empty, huge */
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_empty_slabs_npageslabs_huge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.empty_slabs[1].npageslabs,
    size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_empty_slabs_nactive_huge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.empty_slabs[1].nactive, size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_empty_slabs_ndirty_huge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.empty_slabs[1].ndirty, size_t);

/* Nonfull, nonhuge */
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_nonfull_slabs_j_npageslabs_nonhuge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.nonfull_slabs[mib[5]][0].npageslabs,
    size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_nonfull_slabs_j_nactive_nonhuge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.nonfull_slabs[mib[5]][0].nactive,
    size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_nonfull_slabs_j_ndirty_nonhuge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.nonfull_slabs[mib[5]][0].ndirty,
    size_t);

/* Nonfull, huge */
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_nonfull_slabs_j_npageslabs_huge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.nonfull_slabs[mib[5]][1].npageslabs,
    size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_nonfull_slabs_j_nactive_huge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.nonfull_slabs[mib[5]][1].nactive,
    size_t);
CTL_RO_CGEN(config_stats, stats_arenas_i_hpa_shard_nonfull_slabs_j_ndirty_huge,
    arenas_i(mib[2])->astats->hpastats.psset_stats.nonfull_slabs[mib[5]][1].ndirty,
    size_t);

static const ctl_named_node_t *
stats_arenas_i_hpa_shard_nonfull_slabs_j_index(tsdn_t *tsdn, const size_t *mib,
    size_t miblen, size_t j) {
	if (j >= PSSET_NPSIZES) {
		return NULL;
	}
	return super_stats_arenas_i_hpa_shard_nonfull_slabs_j_node;
}

static bool
ctl_arenas_i_verify(size_t i) {
	size_t a = arenas_i2a_impl(i, true, true);
	if (a == UINT_MAX || !ctl_arenas->arenas[a]->initialized) {
		return true;
	}

	return false;
}

static const ctl_named_node_t *
stats_arenas_i_index(tsdn_t *tsdn, const size_t *mib,
    size_t miblen, size_t i) {
	const ctl_named_node_t *ret;

	malloc_mutex_lock(tsdn, &ctl_mtx);
	if (ctl_arenas_i_verify(i)) {
		ret = NULL;
		goto label_return;
	}

	ret = super_stats_arenas_i_node;
label_return:
	malloc_mutex_unlock(tsdn, &ctl_mtx);
	return ret;
}

static int
experimental_hooks_install_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	if (oldp == NULL || oldlenp == NULL|| newp == NULL) {
		ret = EINVAL;
		goto label_return;
	}
	/*
	 * Note: this is a *private* struct.  This is an experimental interface;
	 * forcing the user to know the jemalloc internals well enough to
	 * extract the ABI hopefully ensures nobody gets too comfortable with
	 * this API, which can change at a moment's notice.
	 */
	hooks_t hooks;
	WRITE(hooks, hooks_t);
	void *handle = hook_install(tsd_tsdn(tsd), &hooks);
	if (handle == NULL) {
		ret = EAGAIN;
		goto label_return;
	}
	READ(handle, void *);

	ret = 0;
label_return:
	return ret;
}

static int
experimental_hooks_remove_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	WRITEONLY();
	void *handle = NULL;
	WRITE(handle, void *);
	if (handle == NULL) {
		ret = EINVAL;
		goto label_return;
	}
	hook_remove(tsd_tsdn(tsd), handle);
	ret = 0;
label_return:
	return ret;
}

static int
experimental_thread_activity_callback_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;

	if (!config_stats) {
		return ENOENT;
	}

	activity_callback_thunk_t t_old = tsd_activity_callback_thunk_get(tsd);
	READ(t_old, activity_callback_thunk_t);

	if (newp != NULL) {
		/*
		 * This initialization is unnecessary.  If it's omitted, though,
		 * clang gets confused and warns on the subsequent use of t_new.
		 */
		activity_callback_thunk_t t_new = {NULL, NULL};
		WRITE(t_new, activity_callback_thunk_t);
		tsd_activity_callback_thunk_set(tsd, t_new);
	}
	ret = 0;
label_return:
	return ret;
}

/*
 * Output six memory utilization entries for an input pointer, the first one of
 * type (void *) and the remaining five of type size_t, describing the following
 * (in the same order):
 *
 * (a) memory address of the extent a potential reallocation would go into,
 * == the five fields below describe about the extent the pointer resides in ==
 * (b) number of free regions in the extent,
 * (c) number of regions in the extent,
 * (d) size of the extent in terms of bytes,
 * (e) total number of free regions in the bin the extent belongs to, and
 * (f) total number of regions in the bin the extent belongs to.
 *
 * Note that "(e)" and "(f)" are only available when stats are enabled;
 * otherwise their values are undefined.
 *
 * This API is mainly intended for small class allocations, where extents are
 * used as slab.  Note that if the bin the extent belongs to is completely
 * full, "(a)" will be NULL.
 *
 * In case of large class allocations, "(a)" will be NULL, and "(e)" and "(f)"
 * will be zero (if stats are enabled; otherwise undefined).  The other three
 * fields will be properly set though the values are trivial: "(b)" will be 0,
 * "(c)" will be 1, and "(d)" will be the usable size.
 *
 * The input pointer and size are respectively passed in by newp and newlen,
 * and the output fields and size are respectively oldp and *oldlenp.
 *
 * It can be beneficial to define the following macros to make it easier to
 * access the output:
 *
 * #define SLABCUR_READ(out) (*(void **)out)
 * #define COUNTS(out) ((size_t *)((void **)out + 1))
 * #define NFREE_READ(out) COUNTS(out)[0]
 * #define NREGS_READ(out) COUNTS(out)[1]
 * #define SIZE_READ(out) COUNTS(out)[2]
 * #define BIN_NFREE_READ(out) COUNTS(out)[3]
 * #define BIN_NREGS_READ(out) COUNTS(out)[4]
 *
 * and then write e.g. NFREE_READ(oldp) to fetch the output.  See the unit test
 * test_query in test/unit/extent_util.c for an example.
 *
 * For a typical defragmentation workflow making use of this API for
 * understanding the fragmentation level, please refer to the comment for
 * experimental_utilization_batch_query_ctl.
 *
 * It's up to the application how to determine the significance of
 * fragmentation relying on the outputs returned.  Possible choices are:
 *
 * (a) if extent utilization ratio is below certain threshold,
 * (b) if extent memory consumption is above certain threshold,
 * (c) if extent utilization ratio is significantly below bin utilization ratio,
 * (d) if input pointer deviates a lot from potential reallocation address, or
 * (e) some selection/combination of the above.
 *
 * The caller needs to make sure that the input/output arguments are valid,
 * in particular, that the size of the output is correct, i.e.:
 *
 *     *oldlenp = sizeof(void *) + sizeof(size_t) * 5
 *
 * Otherwise, the function immediately returns EINVAL without touching anything.
 *
 * In the rare case where there's no associated extent found for the input
 * pointer, the function zeros out all output fields and return.  Please refer
 * to the comment for experimental_utilization_batch_query_ctl to understand the
 * motivation from C++.
 */
static int
experimental_utilization_query_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;

	assert(sizeof(inspect_extent_util_stats_verbose_t)
	    == sizeof(void *) + sizeof(size_t) * 5);

	if (oldp == NULL || oldlenp == NULL
	    || *oldlenp != sizeof(inspect_extent_util_stats_verbose_t)
	    || newp == NULL) {
		ret = EINVAL;
		goto label_return;
	}

	void *ptr = NULL;
	WRITE(ptr, void *);
	inspect_extent_util_stats_verbose_t *util_stats
	    = (inspect_extent_util_stats_verbose_t *)oldp;
	inspect_extent_util_stats_verbose_get(tsd_tsdn(tsd), ptr,
	    &util_stats->nfree, &util_stats->nregs, &util_stats->size,
	    &util_stats->bin_nfree, &util_stats->bin_nregs,
	    &util_stats->slabcur_addr);
	ret = 0;

label_return:
	return ret;
}

/*
 * Given an input array of pointers, output three memory utilization entries of
 * type size_t for each input pointer about the extent it resides in:
 *
 * (a) number of free regions in the extent,
 * (b) number of regions in the extent, and
 * (c) size of the extent in terms of bytes.
 *
 * This API is mainly intended for small class allocations, where extents are
 * used as slab.  In case of large class allocations, the outputs are trivial:
 * "(a)" will be 0, "(b)" will be 1, and "(c)" will be the usable size.
 *
 * Note that multiple input pointers may reside on a same extent so the output
 * fields may contain duplicates.
 *
 * The format of the input/output looks like:
 *
 * input[0]:  1st_pointer_to_query	|  output[0]: 1st_extent_n_free_regions
 *					|  output[1]: 1st_extent_n_regions
 *					|  output[2]: 1st_extent_size
 * input[1]:  2nd_pointer_to_query	|  output[3]: 2nd_extent_n_free_regions
 *					|  output[4]: 2nd_extent_n_regions
 *					|  output[5]: 2nd_extent_size
 * ...					|  ...
 *
 * The input array and size are respectively passed in by newp and newlen, and
 * the output array and size are respectively oldp and *oldlenp.
 *
 * It can be beneficial to define the following macros to make it easier to
 * access the output:
 *
 * #define NFREE_READ(out, i) out[(i) * 3]
 * #define NREGS_READ(out, i) out[(i) * 3 + 1]
 * #define SIZE_READ(out, i) out[(i) * 3 + 2]
 *
 * and then write e.g. NFREE_READ(oldp, i) to fetch the output.  See the unit
 * test test_batch in test/unit/extent_util.c for a concrete example.
 *
 * A typical workflow would be composed of the following steps:
 *
 * (1) flush tcache: mallctl("thread.tcache.flush", ...)
 * (2) initialize input array of pointers to query fragmentation
 * (3) allocate output array to hold utilization statistics
 * (4) query utilization: mallctl("experimental.utilization.batch_query", ...)
 * (5) (optional) decide if it's worthwhile to defragment; otherwise stop here
 * (6) disable tcache: mallctl("thread.tcache.enabled", ...)
 * (7) defragment allocations with significant fragmentation, e.g.:
 *         for each allocation {
 *             if it's fragmented {
 *                 malloc(...);
 *                 memcpy(...);
 *                 free(...);
 *             }
 *         }
 * (8) enable tcache: mallctl("thread.tcache.enabled", ...)
 *
 * The application can determine the significance of fragmentation themselves
 * relying on the statistics returned, both at the overall level i.e. step "(5)"
 * and at individual allocation level i.e. within step "(7)".  Possible choices
 * are:
 *
 * (a) whether memory utilization ratio is below certain threshold,
 * (b) whether memory consumption is above certain threshold, or
 * (c) some combination of the two.
 *
 * The caller needs to make sure that the input/output arrays are valid and
 * their sizes are proper as well as matched, meaning:
 *
 * (a) newlen = n_pointers * sizeof(const void *)
 * (b) *oldlenp = n_pointers * sizeof(size_t) * 3
 * (c) n_pointers > 0
 *
 * Otherwise, the function immediately returns EINVAL without touching anything.
 *
 * In the rare case where there's no associated extent found for some pointers,
 * rather than immediately terminating the computation and raising an error,
 * the function simply zeros out the corresponding output fields and continues
 * the computation until all input pointers are handled.  The motivations of
 * such a design are as follows:
 *
 * (a) The function always either processes nothing or processes everything, and
 * never leaves the output half touched and half untouched.
 *
 * (b) It facilitates usage needs especially common in C++.  A vast variety of
 * C++ objects are instantiated with multiple dynamic memory allocations.  For
 * example, std::string and std::vector typically use at least two allocations,
 * one for the metadata and one for the actual content.  Other types may use
 * even more allocations.  When inquiring about utilization statistics, the
 * caller often wants to examine into all such allocations, especially internal
 * one(s), rather than just the topmost one.  The issue comes when some
 * implementations do certain optimizations to reduce/aggregate some internal
 * allocations, e.g. putting short strings directly into the metadata, and such
 * decisions are not known to the caller.  Therefore, we permit pointers to
 * memory usages that may not be returned by previous malloc calls, and we
 * provide the caller a convenient way to identify such cases.
 */
static int
experimental_utilization_batch_query_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;

	assert(sizeof(inspect_extent_util_stats_t) == sizeof(size_t) * 3);

	const size_t len = newlen / sizeof(const void *);
	if (oldp == NULL || oldlenp == NULL || newp == NULL || newlen == 0
	    || newlen != len * sizeof(const void *)
	    || *oldlenp != len * sizeof(inspect_extent_util_stats_t)) {
		ret = EINVAL;
		goto label_return;
	}

	void **ptrs = (void **)newp;
	inspect_extent_util_stats_t *util_stats =
	    (inspect_extent_util_stats_t *)oldp;
	size_t i;
	for (i = 0; i < len; ++i) {
		inspect_extent_util_stats_get(tsd_tsdn(tsd), ptrs[i],
		    &util_stats[i].nfree, &util_stats[i].nregs,
		    &util_stats[i].size);
	}
	ret = 0;

label_return:
	return ret;
}

static const ctl_named_node_t *
experimental_arenas_i_index(tsdn_t *tsdn, const size_t *mib,
    size_t miblen, size_t i) {
	const ctl_named_node_t *ret;

	malloc_mutex_lock(tsdn, &ctl_mtx);
	if (ctl_arenas_i_verify(i)) {
		ret = NULL;
		goto label_return;
	}
	ret = super_experimental_arenas_i_node;
label_return:
	malloc_mutex_unlock(tsdn, &ctl_mtx);
	return ret;
}

static int
experimental_arenas_i_pactivep_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	if (!config_stats) {
		return ENOENT;
	}
	if (oldp == NULL || oldlenp == NULL || *oldlenp != sizeof(size_t *)) {
		return EINVAL;
	}

	unsigned arena_ind;
	arena_t *arena;
	int ret;
	size_t *pactivep;

	malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);
	READONLY();
	MIB_UNSIGNED(arena_ind, 2);
	if (arena_ind < narenas_total_get() && (arena =
	    arena_get(tsd_tsdn(tsd), arena_ind, false)) != NULL) {
#if defined(JEMALLOC_GCC_ATOMIC_ATOMICS) ||				\
    defined(JEMALLOC_GCC_SYNC_ATOMICS) || defined(_MSC_VER)
		/* Expose the underlying counter for fast read. */
		pactivep = (size_t *)&(arena->pa_shard.nactive.repr);
		READ(pactivep, size_t *);
		ret = 0;
#else
		ret = EFAULT;
#endif
	} else {
		ret = EFAULT;
	}
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);
	return ret;
}

static int
experimental_prof_recent_alloc_max_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;

	if (!(config_prof && opt_prof)) {
		ret = ENOENT;
		goto label_return;
	}

	ssize_t old_max;
	if (newp != NULL) {
		if (newlen != sizeof(ssize_t)) {
			ret = EINVAL;
			goto label_return;
		}
		ssize_t max = *(ssize_t *)newp;
		if (max < -1) {
			ret = EINVAL;
			goto label_return;
		}
		old_max = prof_recent_alloc_max_ctl_write(tsd, max);
	} else {
		old_max = prof_recent_alloc_max_ctl_read();
	}
	READ(old_max, ssize_t);

	ret = 0;

label_return:
	return ret;
}

typedef struct write_cb_packet_s write_cb_packet_t;
struct write_cb_packet_s {
	write_cb_t *write_cb;
	void *cbopaque;
};

static int
experimental_prof_recent_alloc_dump_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;

	if (!(config_prof && opt_prof)) {
		ret = ENOENT;
		goto label_return;
	}

	assert(sizeof(write_cb_packet_t) == sizeof(void *) * 2);

	WRITEONLY();
	write_cb_packet_t write_cb_packet;
	ASSURED_WRITE(write_cb_packet, write_cb_packet_t);

	prof_recent_alloc_dump(tsd, write_cb_packet.write_cb,
	    write_cb_packet.cbopaque);

	ret = 0;

label_return:
	return ret;
}

typedef struct batch_alloc_packet_s batch_alloc_packet_t;
struct batch_alloc_packet_s {
	void **ptrs;
	size_t num;
	size_t size;
	int flags;
};

static int
experimental_batch_alloc_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;

	VERIFY_READ(size_t);

	batch_alloc_packet_t batch_alloc_packet;
	ASSURED_WRITE(batch_alloc_packet, batch_alloc_packet_t);
	size_t filled = batch_alloc(batch_alloc_packet.ptrs,
	    batch_alloc_packet.num, batch_alloc_packet.size,
	    batch_alloc_packet.flags);
	READ(filled, size_t);

	ret = 0;

label_return:
	return ret;
}

static int
prof_stats_bins_i_live_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	unsigned binind;
	prof_stats_t stats;

	if (!(config_prof && opt_prof && opt_prof_stats)) {
		ret = ENOENT;
		goto label_return;
	}

	READONLY();
	MIB_UNSIGNED(binind, 3);
	if (binind >= SC_NBINS) {
		ret = EINVAL;
		goto label_return;
	}
	prof_stats_get_live(tsd, (szind_t)binind, &stats);
	READ(stats, prof_stats_t);

	ret = 0;
label_return:
	return ret;
}

static int
prof_stats_bins_i_accum_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	unsigned binind;
	prof_stats_t stats;

	if (!(config_prof && opt_prof && opt_prof_stats)) {
		ret = ENOENT;
		goto label_return;
	}

	READONLY();
	MIB_UNSIGNED(binind, 3);
	if (binind >= SC_NBINS) {
		ret = EINVAL;
		goto label_return;
	}
	prof_stats_get_accum(tsd, (szind_t)binind, &stats);
	READ(stats, prof_stats_t);

	ret = 0;
label_return:
	return ret;
}

static const ctl_named_node_t *
prof_stats_bins_i_index(tsdn_t *tsdn, const size_t *mib, size_t miblen,
    size_t i) {
	if (!(config_prof && opt_prof && opt_prof_stats)) {
		return NULL;
	}
	if (i >= SC_NBINS) {
		return NULL;
	}
	return super_prof_stats_bins_i_node;
}

static int
prof_stats_lextents_i_live_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	unsigned lextent_ind;
	prof_stats_t stats;

	if (!(config_prof && opt_prof && opt_prof_stats)) {
		ret = ENOENT;
		goto label_return;
	}

	READONLY();
	MIB_UNSIGNED(lextent_ind, 3);
	if (lextent_ind >= SC_NSIZES - SC_NBINS) {
		ret = EINVAL;
		goto label_return;
	}
	prof_stats_get_live(tsd, (szind_t)(lextent_ind + SC_NBINS), &stats);
	READ(stats, prof_stats_t);

	ret = 0;
label_return:
	return ret;
}

static int
prof_stats_lextents_i_accum_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;
	unsigned lextent_ind;
	prof_stats_t stats;

	if (!(config_prof && opt_prof && opt_prof_stats)) {
		ret = ENOENT;
		goto label_return;
	}

	READONLY();
	MIB_UNSIGNED(lextent_ind, 3);
	if (lextent_ind >= SC_NSIZES - SC_NBINS) {
		ret = EINVAL;
		goto label_return;
	}
	prof_stats_get_accum(tsd, (szind_t)(lextent_ind + SC_NBINS), &stats);
	READ(stats, prof_stats_t);

	ret = 0;
label_return:
	return ret;
}

static const ctl_named_node_t *
prof_stats_lextents_i_index(tsdn_t *tsdn, const size_t *mib, size_t miblen,
    size_t i) {
	if (!(config_prof && opt_prof && opt_prof_stats)) {
		return NULL;
	}
	if (i >= SC_NSIZES - SC_NBINS) {
		return NULL;
	}
	return super_prof_stats_lextents_i_node;
}

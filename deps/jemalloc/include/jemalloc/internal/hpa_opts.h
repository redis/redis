#ifndef JEMALLOC_INTERNAL_HPA_OPTS_H
#define JEMALLOC_INTERNAL_HPA_OPTS_H

#include "jemalloc/internal/fxp.h"

/*
 * This file is morally part of hpa.h, but is split out for header-ordering
 * reasons.
 */

typedef struct hpa_shard_opts_s hpa_shard_opts_t;
struct hpa_shard_opts_s {
	/*
	 * The largest size we'll allocate out of the shard.  For those
	 * allocations refused, the caller (in practice, the PA module) will
	 * fall back to the more general (for now) PAC, which can always handle
	 * any allocation request.
	 */
	size_t slab_max_alloc;

	/*
	 * When the number of active bytes in a hugepage is >=
	 * hugification_threshold, we force hugify it.
	 */
	size_t hugification_threshold;

	/*
	 * The HPA purges whenever the number of pages exceeds dirty_mult *
	 * active_pages.  This may be set to (fxp_t)-1 to disable purging.
	 */
	fxp_t dirty_mult;

	/*
	 * Whether or not the PAI methods are allowed to defer work to a
	 * subsequent hpa_shard_do_deferred_work() call.  Practically, this
	 * corresponds to background threads being enabled.  We track this
	 * ourselves for encapsulation purposes.
	 */
	bool deferral_allowed;

	/*
	 * How long a hugepage has to be a hugification candidate before it will
	 * actually get hugified.
	 */
	uint64_t hugify_delay_ms;

	/*
	 * Minimum amount of time between purges.
	 */
	uint64_t min_purge_interval_ms;
};

#define HPA_SHARD_OPTS_DEFAULT {					\
	/* slab_max_alloc */						\
	64 * 1024,							\
	/* hugification_threshold */					\
	HUGEPAGE * 95 / 100,						\
	/* dirty_mult */						\
	FXP_INIT_PERCENT(25),						\
	/*								\
	 * deferral_allowed						\
	 * 								\
	 * Really, this is always set by the arena during creation	\
	 * or by an hpa_shard_set_deferral_allowed call, so the value	\
	 * we put here doesn't matter.					\
	 */								\
	false,								\
	/* hugify_delay_ms */						\
	10 * 1000,							\
	/* min_purge_interval_ms */					\
	5 * 1000							\
}

#endif /* JEMALLOC_INTERNAL_HPA_OPTS_H */

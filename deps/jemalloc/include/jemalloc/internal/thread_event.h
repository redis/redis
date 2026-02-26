#ifndef JEMALLOC_INTERNAL_THREAD_EVENT_H
#define JEMALLOC_INTERNAL_THREAD_EVENT_H

#include "jemalloc/internal/tsd.h"

/* "te" is short for "thread_event" */

/*
 * TE_MIN_START_WAIT should not exceed the minimal allocation usize.
 */
#define TE_MIN_START_WAIT ((uint64_t)1U)
#define TE_MAX_START_WAIT UINT64_MAX

/*
 * Maximum threshold on thread_(de)allocated_next_event_fast, so that there is
 * no need to check overflow in malloc fast path. (The allocation size in malloc
 * fast path never exceeds SC_LOOKUP_MAXCLASS.)
 */
#define TE_NEXT_EVENT_FAST_MAX (UINT64_MAX - SC_LOOKUP_MAXCLASS + 1U)

/*
 * The max interval helps make sure that malloc stays on the fast path in the
 * common case, i.e. thread_allocated < thread_allocated_next_event_fast.  When
 * thread_allocated is within an event's distance to TE_NEXT_EVENT_FAST_MAX
 * above, thread_allocated_next_event_fast is wrapped around and we fall back to
 * the medium-fast path. The max interval makes sure that we're not staying on
 * the fallback case for too long, even if there's no active event or if all
 * active events have long wait times.
 */
#define TE_MAX_INTERVAL ((uint64_t)(4U << 20))

/*
 * Invalid elapsed time, for situations where elapsed time is not needed.  See
 * comments in thread_event.c for more info.
 */
#define TE_INVALID_ELAPSED UINT64_MAX

typedef struct te_ctx_s {
	bool is_alloc;
	uint64_t *current;
	uint64_t *last_event;
	uint64_t *next_event;
	uint64_t *next_event_fast;
} te_ctx_t;

void te_assert_invariants_debug(tsd_t *tsd);
void te_event_trigger(tsd_t *tsd, te_ctx_t *ctx);
void te_recompute_fast_threshold(tsd_t *tsd);
void tsd_te_init(tsd_t *tsd);

/*
 * List of all events, in the following format:
 *  E(event,		(condition), is_alloc_event)
 */
#define ITERATE_OVER_ALL_EVENTS						\
    E(tcache_gc,		(opt_tcache_gc_incr_bytes > 0), true)	\
    E(prof_sample,		(config_prof && opt_prof), true)  	\
    E(stats_interval,		(opt_stats_interval >= 0), true)   	\
    E(tcache_gc_dalloc,		(opt_tcache_gc_incr_bytes > 0), false)	\
    E(peak_alloc,		config_stats, true)			\
    E(peak_dalloc,		config_stats, false)

#define E(event, condition_unused, is_alloc_event_unused)		\
    C(event##_event_wait)

/* List of all thread event counters. */
#define ITERATE_OVER_ALL_COUNTERS					\
    C(thread_allocated)							\
    C(thread_allocated_last_event)					\
    ITERATE_OVER_ALL_EVENTS						\
    C(prof_sample_last_event)						\
    C(stats_interval_last_event)

/* Getters directly wrap TSD getters. */
#define C(counter)							\
JEMALLOC_ALWAYS_INLINE uint64_t						\
counter##_get(tsd_t *tsd) {						\
	return tsd_##counter##_get(tsd);				\
}

ITERATE_OVER_ALL_COUNTERS
#undef C

/*
 * Setters call the TSD pointer getters rather than the TSD setters, so that
 * the counters can be modified even when TSD state is reincarnated or
 * minimal_initialized: if an event is triggered in such cases, we will
 * temporarily delay the event and let it be immediately triggered at the next
 * allocation call.
 */
#define C(counter)							\
JEMALLOC_ALWAYS_INLINE void						\
counter##_set(tsd_t *tsd, uint64_t v) {					\
	*tsd_##counter##p_get(tsd) = v;					\
}

ITERATE_OVER_ALL_COUNTERS
#undef C

/*
 * For generating _event_wait getter / setter functions for each individual
 * event.
 */
#undef E

/*
 * The malloc and free fastpath getters -- use the unsafe getters since tsd may
 * be non-nominal, in which case the fast_threshold will be set to 0.  This
 * allows checking for events and tsd non-nominal in a single branch.
 *
 * Note that these can only be used on the fastpath.
 */
JEMALLOC_ALWAYS_INLINE void
te_malloc_fastpath_ctx(tsd_t *tsd, uint64_t *allocated, uint64_t *threshold) {
	*allocated = *tsd_thread_allocatedp_get_unsafe(tsd);
	*threshold = *tsd_thread_allocated_next_event_fastp_get_unsafe(tsd);
	assert(*threshold <= TE_NEXT_EVENT_FAST_MAX);
}

JEMALLOC_ALWAYS_INLINE void
te_free_fastpath_ctx(tsd_t *tsd, uint64_t *deallocated, uint64_t *threshold) {
	/* Unsafe getters since this may happen before tsd_init. */
	*deallocated = *tsd_thread_deallocatedp_get_unsafe(tsd);
	*threshold = *tsd_thread_deallocated_next_event_fastp_get_unsafe(tsd);
	assert(*threshold <= TE_NEXT_EVENT_FAST_MAX);
}

JEMALLOC_ALWAYS_INLINE bool
te_ctx_is_alloc(te_ctx_t *ctx) {
	return ctx->is_alloc;
}

JEMALLOC_ALWAYS_INLINE uint64_t
te_ctx_current_bytes_get(te_ctx_t *ctx) {
	return *ctx->current;
}

JEMALLOC_ALWAYS_INLINE void
te_ctx_current_bytes_set(te_ctx_t *ctx, uint64_t v) {
	*ctx->current = v;
}

JEMALLOC_ALWAYS_INLINE uint64_t
te_ctx_last_event_get(te_ctx_t *ctx) {
	return *ctx->last_event;
}

JEMALLOC_ALWAYS_INLINE void
te_ctx_last_event_set(te_ctx_t *ctx, uint64_t v) {
	*ctx->last_event = v;
}

/* Below 3 for next_event_fast. */
JEMALLOC_ALWAYS_INLINE uint64_t
te_ctx_next_event_fast_get(te_ctx_t *ctx) {
	uint64_t v = *ctx->next_event_fast;
	assert(v <= TE_NEXT_EVENT_FAST_MAX);
	return v;
}

JEMALLOC_ALWAYS_INLINE void
te_ctx_next_event_fast_set(te_ctx_t *ctx, uint64_t v) {
	assert(v <= TE_NEXT_EVENT_FAST_MAX);
	*ctx->next_event_fast = v;
}

JEMALLOC_ALWAYS_INLINE void
te_next_event_fast_set_non_nominal(tsd_t *tsd) {
	/*
	 * Set the fast thresholds to zero when tsd is non-nominal.  Use the
	 * unsafe getter as this may get called during tsd init and clean up.
	 */
	*tsd_thread_allocated_next_event_fastp_get_unsafe(tsd) = 0;
	*tsd_thread_deallocated_next_event_fastp_get_unsafe(tsd) = 0;
}

/* For next_event.  Setter also updates the fast threshold. */
JEMALLOC_ALWAYS_INLINE uint64_t
te_ctx_next_event_get(te_ctx_t *ctx) {
	return *ctx->next_event;
}

JEMALLOC_ALWAYS_INLINE void
te_ctx_next_event_set(tsd_t *tsd, te_ctx_t *ctx, uint64_t v) {
	*ctx->next_event = v;
	te_recompute_fast_threshold(tsd);
}

/*
 * The function checks in debug mode whether the thread event counters are in
 * a consistent state, which forms the invariants before and after each round
 * of thread event handling that we can rely on and need to promise.
 * The invariants are only temporarily violated in the middle of
 * te_event_advance() if an event is triggered (the te_event_trigger() call at
 * the end will restore the invariants).
 */
JEMALLOC_ALWAYS_INLINE void
te_assert_invariants(tsd_t *tsd) {
	if (config_debug) {
		te_assert_invariants_debug(tsd);
	}
}

JEMALLOC_ALWAYS_INLINE void
te_ctx_get(tsd_t *tsd, te_ctx_t *ctx, bool is_alloc) {
	ctx->is_alloc = is_alloc;
	if (is_alloc) {
		ctx->current = tsd_thread_allocatedp_get(tsd);
		ctx->last_event = tsd_thread_allocated_last_eventp_get(tsd);
		ctx->next_event = tsd_thread_allocated_next_eventp_get(tsd);
		ctx->next_event_fast =
		    tsd_thread_allocated_next_event_fastp_get(tsd);
	} else {
		ctx->current = tsd_thread_deallocatedp_get(tsd);
		ctx->last_event = tsd_thread_deallocated_last_eventp_get(tsd);
		ctx->next_event = tsd_thread_deallocated_next_eventp_get(tsd);
		ctx->next_event_fast =
		    tsd_thread_deallocated_next_event_fastp_get(tsd);
	}
}

/*
 * The lookahead functionality facilitates events to be able to lookahead, i.e.
 * without touching the event counters, to determine whether an event would be
 * triggered.  The event counters are not advanced until the end of the
 * allocation / deallocation calls, so the lookahead can be useful if some
 * preparation work for some event must be done early in the allocation /
 * deallocation calls.
 *
 * Currently only the profiling sampling event needs the lookahead
 * functionality, so we don't yet define general purpose lookahead functions.
 *
 * Surplus is a terminology referring to the amount of bytes beyond what's
 * needed for triggering an event, which can be a useful quantity to have in
 * general when lookahead is being called.
 */

JEMALLOC_ALWAYS_INLINE bool
te_prof_sample_event_lookahead_surplus(tsd_t *tsd, size_t usize,
    size_t *surplus) {
	if (surplus != NULL) {
		/*
		 * This is a dead store: the surplus will be overwritten before
		 * any read.  The initialization suppresses compiler warnings.
		 * Meanwhile, using SIZE_MAX to initialize is good for
		 * debugging purpose, because a valid surplus value is strictly
		 * less than usize, which is at most SIZE_MAX.
		 */
		*surplus = SIZE_MAX;
	}
	if (unlikely(!tsd_nominal(tsd) || tsd_reentrancy_level_get(tsd) > 0)) {
		return false;
	}
	/* The subtraction is intentionally susceptible to underflow. */
	uint64_t accumbytes = tsd_thread_allocated_get(tsd) + usize -
	    tsd_thread_allocated_last_event_get(tsd);
	uint64_t sample_wait = tsd_prof_sample_event_wait_get(tsd);
	if (accumbytes < sample_wait) {
		return false;
	}
	assert(accumbytes - sample_wait < (uint64_t)usize);
	if (surplus != NULL) {
		*surplus = (size_t)(accumbytes - sample_wait);
	}
	return true;
}

JEMALLOC_ALWAYS_INLINE bool
te_prof_sample_event_lookahead(tsd_t *tsd, size_t usize) {
	return te_prof_sample_event_lookahead_surplus(tsd, usize, NULL);
}

JEMALLOC_ALWAYS_INLINE void
te_event_advance(tsd_t *tsd, size_t usize, bool is_alloc) {
	te_assert_invariants(tsd);

	te_ctx_t ctx;
	te_ctx_get(tsd, &ctx, is_alloc);

	uint64_t bytes_before = te_ctx_current_bytes_get(&ctx);
	te_ctx_current_bytes_set(&ctx, bytes_before + usize);

	/* The subtraction is intentionally susceptible to underflow. */
	if (likely(usize < te_ctx_next_event_get(&ctx) - bytes_before)) {
		te_assert_invariants(tsd);
	} else {
		te_event_trigger(tsd, &ctx);
	}
}

JEMALLOC_ALWAYS_INLINE void
thread_dalloc_event(tsd_t *tsd, size_t usize) {
	te_event_advance(tsd, usize, false);
}

JEMALLOC_ALWAYS_INLINE void
thread_alloc_event(tsd_t *tsd, size_t usize) {
	te_event_advance(tsd, usize, true);
}

#endif /* JEMALLOC_INTERNAL_THREAD_EVENT_H */

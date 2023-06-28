#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/thread_event.h"

/*
 * Signatures for event specific functions.  These functions should be defined
 * by the modules owning each event.  The signatures here verify that the
 * definitions follow the right format.
 *
 * The first two are functions computing new / postponed event wait time.  New
 * event wait time is the time till the next event if an event is currently
 * being triggered; postponed event wait time is the time till the next event
 * if an event should be triggered but needs to be postponed, e.g. when the TSD
 * is not nominal or during reentrancy.
 *
 * The third is the event handler function, which is called whenever an event
 * is triggered.  The parameter is the elapsed time since the last time an
 * event of the same type was triggered.
 */
#define E(event, condition_unused, is_alloc_event_unused)		\
uint64_t event##_new_event_wait(tsd_t *tsd);				\
uint64_t event##_postponed_event_wait(tsd_t *tsd);			\
void event##_event_handler(tsd_t *tsd, uint64_t elapsed);

ITERATE_OVER_ALL_EVENTS
#undef E

/* Signatures for internal functions fetching elapsed time. */
#define E(event, condition_unused, is_alloc_event_unused)		\
static uint64_t event##_fetch_elapsed(tsd_t *tsd);

ITERATE_OVER_ALL_EVENTS
#undef E

static uint64_t
tcache_gc_fetch_elapsed(tsd_t *tsd) {
	return TE_INVALID_ELAPSED;
}

static uint64_t
tcache_gc_dalloc_fetch_elapsed(tsd_t *tsd) {
	return TE_INVALID_ELAPSED;
}

static uint64_t
prof_sample_fetch_elapsed(tsd_t *tsd) {
	uint64_t last_event = thread_allocated_last_event_get(tsd);
	uint64_t last_sample_event = prof_sample_last_event_get(tsd);
	prof_sample_last_event_set(tsd, last_event);
	return last_event - last_sample_event;
}

static uint64_t
stats_interval_fetch_elapsed(tsd_t *tsd) {
	uint64_t last_event = thread_allocated_last_event_get(tsd);
	uint64_t last_stats_event = stats_interval_last_event_get(tsd);
	stats_interval_last_event_set(tsd, last_event);
	return last_event - last_stats_event;
}

static uint64_t
peak_alloc_fetch_elapsed(tsd_t *tsd) {
	return TE_INVALID_ELAPSED;
}

static uint64_t
peak_dalloc_fetch_elapsed(tsd_t *tsd) {
	return TE_INVALID_ELAPSED;
}

/* Per event facilities done. */

static bool
te_ctx_has_active_events(te_ctx_t *ctx) {
	assert(config_debug);
#define E(event, condition, alloc_event)			       \
	if (condition && alloc_event == ctx->is_alloc) {	       \
		return true;					       \
	}
	ITERATE_OVER_ALL_EVENTS
#undef E
	return false;
}

static uint64_t
te_next_event_compute(tsd_t *tsd, bool is_alloc) {
	uint64_t wait = TE_MAX_START_WAIT;
#define E(event, condition, alloc_event)				\
	if (is_alloc == alloc_event && condition) {			\
		uint64_t event_wait =					\
		    event##_event_wait_get(tsd);			\
		assert(event_wait <= TE_MAX_START_WAIT);		\
		if (event_wait > 0U && event_wait < wait) {		\
			wait = event_wait;				\
		}							\
	}

	ITERATE_OVER_ALL_EVENTS
#undef E
	assert(wait <= TE_MAX_START_WAIT);
	return wait;
}

static void
te_assert_invariants_impl(tsd_t *tsd, te_ctx_t *ctx) {
	uint64_t current_bytes = te_ctx_current_bytes_get(ctx);
	uint64_t last_event = te_ctx_last_event_get(ctx);
	uint64_t next_event = te_ctx_next_event_get(ctx);
	uint64_t next_event_fast = te_ctx_next_event_fast_get(ctx);

	assert(last_event != next_event);
	if (next_event > TE_NEXT_EVENT_FAST_MAX || !tsd_fast(tsd)) {
		assert(next_event_fast == 0U);
	} else {
		assert(next_event_fast == next_event);
	}

	/* The subtraction is intentionally susceptible to underflow. */
	uint64_t interval = next_event - last_event;

	/* The subtraction is intentionally susceptible to underflow. */
	assert(current_bytes - last_event < interval);
	uint64_t min_wait = te_next_event_compute(tsd, te_ctx_is_alloc(ctx));
	/*
	 * next_event should have been pushed up only except when no event is
	 * on and the TSD is just initialized.  The last_event == 0U guard
	 * below is stronger than needed, but having an exactly accurate guard
	 * is more complicated to implement.
	 */
	assert((!te_ctx_has_active_events(ctx) && last_event == 0U) ||
	    interval == min_wait ||
	    (interval < min_wait && interval == TE_MAX_INTERVAL));
}

void
te_assert_invariants_debug(tsd_t *tsd) {
	te_ctx_t ctx;
	te_ctx_get(tsd, &ctx, true);
	te_assert_invariants_impl(tsd, &ctx);

	te_ctx_get(tsd, &ctx, false);
	te_assert_invariants_impl(tsd, &ctx);
}

/*
 * Synchronization around the fast threshold in tsd --
 * There are two threads to consider in the synchronization here:
 * - The owner of the tsd being updated by a slow path change
 * - The remote thread, doing that slow path change.
 *
 * As a design constraint, we want to ensure that a slow-path transition cannot
 * be ignored for arbitrarily long, and that if the remote thread causes a
 * slow-path transition and then communicates with the owner thread that it has
 * occurred, then the owner will go down the slow path on the next allocator
 * operation (so that we don't want to just wait until the owner hits its slow
 * path reset condition on its own).
 *
 * Here's our strategy to do that:
 *
 * The remote thread will update the slow-path stores to TSD variables, issue a
 * SEQ_CST fence, and then update the TSD next_event_fast counter. The owner
 * thread will update next_event_fast, issue an SEQ_CST fence, and then check
 * its TSD to see if it's on the slow path.

 * This is fairly straightforward when 64-bit atomics are supported. Assume that
 * the remote fence is sandwiched between two owner fences in the reset pathway.
 * The case where there is no preceding or trailing owner fence (i.e. because
 * the owner thread is near the beginning or end of its life) can be analyzed
 * similarly. The owner store to next_event_fast preceding the earlier owner
 * fence will be earlier in coherence order than the remote store to it, so that
 * the owner thread will go down the slow path once the store becomes visible to
 * it, which is no later than the time of the second fence.

 * The case where we don't support 64-bit atomics is trickier, since word
 * tearing is possible. We'll repeat the same analysis, and look at the two
 * owner fences sandwiching the remote fence. The next_event_fast stores done
 * alongside the earlier owner fence cannot overwrite any of the remote stores
 * (since they precede the earlier owner fence in sb, which precedes the remote
 * fence in sc, which precedes the remote stores in sb). After the second owner
 * fence there will be a re-check of the slow-path variables anyways, so the
 * "owner will notice that it's on the slow path eventually" guarantee is
 * satisfied. To make sure that the out-of-band-messaging constraint is as well,
 * note that either the message passing is sequenced before the second owner
 * fence (in which case the remote stores happen before the second set of owner
 * stores, so malloc sees a value of zero for next_event_fast and goes down the
 * slow path), or it is not (in which case the owner sees the tsd slow-path
 * writes on its previous update). This leaves open the possibility that the
 * remote thread will (at some arbitrary point in the future) zero out one half
 * of the owner thread's next_event_fast, but that's always safe (it just sends
 * it down the slow path earlier).
 */
static void
te_ctx_next_event_fast_update(te_ctx_t *ctx) {
	uint64_t next_event = te_ctx_next_event_get(ctx);
	uint64_t next_event_fast = (next_event <= TE_NEXT_EVENT_FAST_MAX) ?
	    next_event : 0U;
	te_ctx_next_event_fast_set(ctx, next_event_fast);
}

void
te_recompute_fast_threshold(tsd_t *tsd) {
	if (tsd_state_get(tsd) != tsd_state_nominal) {
		/* Check first because this is also called on purgatory. */
		te_next_event_fast_set_non_nominal(tsd);
		return;
	}

	te_ctx_t ctx;
	te_ctx_get(tsd, &ctx, true);
	te_ctx_next_event_fast_update(&ctx);
	te_ctx_get(tsd, &ctx, false);
	te_ctx_next_event_fast_update(&ctx);

	atomic_fence(ATOMIC_SEQ_CST);
	if (tsd_state_get(tsd) != tsd_state_nominal) {
		te_next_event_fast_set_non_nominal(tsd);
	}
}

static void
te_adjust_thresholds_helper(tsd_t *tsd, te_ctx_t *ctx,
    uint64_t wait) {
	/*
	 * The next threshold based on future events can only be adjusted after
	 * progressing the last_event counter (which is set to current).
	 */
	assert(te_ctx_current_bytes_get(ctx) == te_ctx_last_event_get(ctx));
	assert(wait <= TE_MAX_START_WAIT);

	uint64_t next_event = te_ctx_last_event_get(ctx) + (wait <=
	    TE_MAX_INTERVAL ? wait : TE_MAX_INTERVAL);
	te_ctx_next_event_set(tsd, ctx, next_event);
}

static uint64_t
te_clip_event_wait(uint64_t event_wait) {
	assert(event_wait > 0U);
	if (TE_MIN_START_WAIT > 1U &&
	    unlikely(event_wait < TE_MIN_START_WAIT)) {
		event_wait = TE_MIN_START_WAIT;
	}
	if (TE_MAX_START_WAIT < UINT64_MAX &&
	    unlikely(event_wait > TE_MAX_START_WAIT)) {
		event_wait = TE_MAX_START_WAIT;
	}
	return event_wait;
}

void
te_event_trigger(tsd_t *tsd, te_ctx_t *ctx) {
	/* usize has already been added to thread_allocated. */
	uint64_t bytes_after = te_ctx_current_bytes_get(ctx);
	/* The subtraction is intentionally susceptible to underflow. */
	uint64_t accumbytes = bytes_after - te_ctx_last_event_get(ctx);

	te_ctx_last_event_set(ctx, bytes_after);

	bool allow_event_trigger = tsd_nominal(tsd) &&
	    tsd_reentrancy_level_get(tsd) == 0;
	bool is_alloc = ctx->is_alloc;
	uint64_t wait = TE_MAX_START_WAIT;

#define E(event, condition, alloc_event)				\
	bool is_##event##_triggered = false;				\
	if (is_alloc == alloc_event && condition) {			\
		uint64_t event_wait = event##_event_wait_get(tsd);	\
		assert(event_wait <= TE_MAX_START_WAIT);		\
		if (event_wait > accumbytes) {				\
			event_wait -= accumbytes;			\
		} else if (!allow_event_trigger) {			\
			event_wait = event##_postponed_event_wait(tsd);	\
		} else {						\
			is_##event##_triggered = true;			\
			event_wait = event##_new_event_wait(tsd);	\
		}							\
		event_wait = te_clip_event_wait(event_wait);		\
		event##_event_wait_set(tsd, event_wait);		\
		if (event_wait < wait) {				\
			wait = event_wait;				\
		}							\
	}

	ITERATE_OVER_ALL_EVENTS
#undef E

	assert(wait <= TE_MAX_START_WAIT);
	te_adjust_thresholds_helper(tsd, ctx, wait);
	te_assert_invariants(tsd);

#define E(event, condition, alloc_event)				\
	if (is_alloc == alloc_event && condition &&			\
	    is_##event##_triggered) {					\
		assert(allow_event_trigger);				\
		uint64_t elapsed = event##_fetch_elapsed(tsd);		\
		event##_event_handler(tsd, elapsed);			\
	}

	ITERATE_OVER_ALL_EVENTS
#undef E

	te_assert_invariants(tsd);
}

static void
te_init(tsd_t *tsd, bool is_alloc) {
	te_ctx_t ctx;
	te_ctx_get(tsd, &ctx, is_alloc);
	/*
	 * Reset the last event to current, which starts the events from a clean
	 * state.  This is necessary when re-init the tsd event counters.
	 *
	 * The event counters maintain a relationship with the current bytes:
	 * last_event <= current < next_event.  When a reinit happens (e.g.
	 * reincarnated tsd), the last event needs progressing because all
	 * events start fresh from the current bytes.
	 */
	te_ctx_last_event_set(&ctx, te_ctx_current_bytes_get(&ctx));

	uint64_t wait = TE_MAX_START_WAIT;
#define E(event, condition, alloc_event)				\
	if (is_alloc == alloc_event && condition) {			\
		uint64_t event_wait = event##_new_event_wait(tsd);	\
		event_wait = te_clip_event_wait(event_wait);		\
		event##_event_wait_set(tsd, event_wait);		\
		if (event_wait < wait) {				\
			wait = event_wait;				\
		}							\
	}

	ITERATE_OVER_ALL_EVENTS
#undef E
	te_adjust_thresholds_helper(tsd, &ctx, wait);
}

void
tsd_te_init(tsd_t *tsd) {
	/* Make sure no overflow for the bytes accumulated on event_trigger. */
	assert(TE_MAX_INTERVAL <= UINT64_MAX - SC_LARGE_MAXCLASS + 1);
	te_init(tsd, true);
	te_init(tsd, false);
	te_assert_invariants(tsd);
}

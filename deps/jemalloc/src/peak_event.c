#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/peak_event.h"

#include "jemalloc/internal/activity_callback.h"
#include "jemalloc/internal/peak.h"

/*
 * Update every 64K by default.  We're not exposing this as a configuration
 * option for now; we don't want to bind ourselves too tightly to any particular
 * performance requirements for small values, or guarantee that we'll even be
 * able to provide fine-grained accuracy.
 */
#define PEAK_EVENT_WAIT (64 * 1024)

/* Update the peak with current tsd state. */
void
peak_event_update(tsd_t *tsd) {
	uint64_t alloc = tsd_thread_allocated_get(tsd);
	uint64_t dalloc = tsd_thread_deallocated_get(tsd);
	peak_t *peak = tsd_peakp_get(tsd);
	peak_update(peak, alloc, dalloc);
}

static void
peak_event_activity_callback(tsd_t *tsd) {
	activity_callback_thunk_t *thunk = tsd_activity_callback_thunkp_get(
	    tsd);
	uint64_t alloc = tsd_thread_allocated_get(tsd);
	uint64_t dalloc = tsd_thread_deallocated_get(tsd);
	if (thunk->callback != NULL) {
		thunk->callback(thunk->uctx, alloc, dalloc);
	}
}

/* Set current state to zero. */
void
peak_event_zero(tsd_t *tsd) {
	uint64_t alloc = tsd_thread_allocated_get(tsd);
	uint64_t dalloc = tsd_thread_deallocated_get(tsd);
	peak_t *peak = tsd_peakp_get(tsd);
	peak_set_zero(peak, alloc, dalloc);
}

uint64_t
peak_event_max(tsd_t *tsd) {
	peak_t *peak = tsd_peakp_get(tsd);
	return peak_max(peak);
}

uint64_t
peak_alloc_new_event_wait(tsd_t *tsd) {
	return PEAK_EVENT_WAIT;
}

uint64_t
peak_alloc_postponed_event_wait(tsd_t *tsd) {
	return TE_MIN_START_WAIT;
}

void
peak_alloc_event_handler(tsd_t *tsd, uint64_t elapsed) {
	peak_event_update(tsd);
	peak_event_activity_callback(tsd);
}

uint64_t
peak_dalloc_new_event_wait(tsd_t *tsd) {
	return PEAK_EVENT_WAIT;
}

uint64_t
peak_dalloc_postponed_event_wait(tsd_t *tsd) {
	return TE_MIN_START_WAIT;
}

void
peak_dalloc_event_handler(tsd_t *tsd, uint64_t elapsed) {
	peak_event_update(tsd);
	peak_event_activity_callback(tsd);
}

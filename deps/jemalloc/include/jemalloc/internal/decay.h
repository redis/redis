#ifndef JEMALLOC_INTERNAL_DECAY_H
#define JEMALLOC_INTERNAL_DECAY_H

#include "jemalloc/internal/smoothstep.h"

#define DECAY_UNBOUNDED_TIME_TO_PURGE ((uint64_t)-1)

/*
 * The decay_t computes the number of pages we should purge at any given time.
 * Page allocators inform a decay object when pages enter a decay-able state
 * (i.e. dirty or muzzy), and query it to determine how many pages should be
 * purged at any given time.
 *
 * This is mostly a single-threaded data structure and doesn't care about
 * synchronization at all; it's the caller's responsibility to manage their
 * synchronization on their own.  There are two exceptions:
 * 1) It's OK to racily call decay_ms_read (i.e. just the simplest state query).
 * 2) The mtx and purging fields live (and are initialized) here, but are
 *    logically owned by the page allocator.  This is just a convenience (since
 *    those fields would be duplicated for both the dirty and muzzy states
 *    otherwise).
 */
typedef struct decay_s decay_t;
struct decay_s {
	/* Synchronizes all non-atomic fields. */
	malloc_mutex_t mtx;
	/*
	 * True if a thread is currently purging the extents associated with
	 * this decay structure.
	 */
	bool purging;
	/*
	 * Approximate time in milliseconds from the creation of a set of unused
	 * dirty pages until an equivalent set of unused dirty pages is purged
	 * and/or reused.
	 */
	atomic_zd_t time_ms;
	/* time / SMOOTHSTEP_NSTEPS. */
	nstime_t interval;
	/*
	 * Time at which the current decay interval logically started.  We do
	 * not actually advance to a new epoch until sometime after it starts
	 * because of scheduling and computation delays, and it is even possible
	 * to completely skip epochs.  In all cases, during epoch advancement we
	 * merge all relevant activity into the most recently recorded epoch.
	 */
	nstime_t epoch;
	/* Deadline randomness generator. */
	uint64_t jitter_state;
	/*
	 * Deadline for current epoch.  This is the sum of interval and per
	 * epoch jitter which is a uniform random variable in [0..interval).
	 * Epochs always advance by precise multiples of interval, but we
	 * randomize the deadline to reduce the likelihood of arenas purging in
	 * lockstep.
	 */
	nstime_t deadline;
	/*
	 * The number of pages we cap ourselves at in the current epoch, per
	 * decay policies.  Updated on an epoch change.  After an epoch change,
	 * the caller should take steps to try to purge down to this amount.
	 */
	size_t npages_limit;
	/*
	 * Number of unpurged pages at beginning of current epoch.  During epoch
	 * advancement we use the delta between arena->decay_*.nunpurged and
	 * ecache_npages_get(&arena->ecache_*) to determine how many dirty pages,
	 * if any, were generated.
	 */
	size_t nunpurged;
	/*
	 * Trailing log of how many unused dirty pages were generated during
	 * each of the past SMOOTHSTEP_NSTEPS decay epochs, where the last
	 * element is the most recent epoch.  Corresponding epoch times are
	 * relative to epoch.
	 *
	 * Updated only on epoch advance, triggered by
	 * decay_maybe_advance_epoch, below.
	 */
	size_t backlog[SMOOTHSTEP_NSTEPS];

	/* Peak number of pages in associated extents.  Used for debug only. */
	uint64_t ceil_npages;
};

/*
 * The current decay time setting.  This is the only public access to a decay_t
 * that's allowed without holding mtx.
 */
static inline ssize_t
decay_ms_read(const decay_t *decay) {
	return atomic_load_zd(&decay->time_ms, ATOMIC_RELAXED);
}

/*
 * See the comment on the struct field -- the limit on pages we should allow in
 * this decay state this epoch.
 */
static inline size_t
decay_npages_limit_get(const decay_t *decay) {
	return decay->npages_limit;
}

/* How many unused dirty pages were generated during the last epoch. */
static inline size_t
decay_epoch_npages_delta(const decay_t *decay) {
	return decay->backlog[SMOOTHSTEP_NSTEPS - 1];
}

/*
 * Current epoch duration, in nanoseconds.  Given that new epochs are started
 * somewhat haphazardly, this is not necessarily exactly the time between any
 * two calls to decay_maybe_advance_epoch; see the comments on fields in the
 * decay_t.
 */
static inline uint64_t
decay_epoch_duration_ns(const decay_t *decay) {
	return nstime_ns(&decay->interval);
}

static inline bool
decay_immediately(const decay_t *decay) {
	ssize_t decay_ms = decay_ms_read(decay);
	return decay_ms == 0;
}

static inline bool
decay_disabled(const decay_t *decay) {
	ssize_t decay_ms = decay_ms_read(decay);
	return decay_ms < 0;
}

/* Returns true if decay is enabled and done gradually. */
static inline bool
decay_gradually(const decay_t *decay) {
	ssize_t decay_ms = decay_ms_read(decay);
	return decay_ms > 0;
}

/*
 * Returns true if the passed in decay time setting is valid.
 * < -1 : invalid
 * -1   : never decay
 *  0   : decay immediately
 *  > 0 : some positive decay time, up to a maximum allowed value of
 *  NSTIME_SEC_MAX * 1000, which corresponds to decaying somewhere in the early
 *  27th century.  By that time, we expect to have implemented alternate purging
 *  strategies.
 */
bool decay_ms_valid(ssize_t decay_ms);

/*
 * As a precondition, the decay_t must be zeroed out (as if with memset).
 *
 * Returns true on error.
 */
bool decay_init(decay_t *decay, nstime_t *cur_time, ssize_t decay_ms);

/*
 * Given an already-initialized decay_t, reinitialize it with the given decay
 * time.  The decay_t must have previously been initialized (and should not then
 * be zeroed).
 */
void decay_reinit(decay_t *decay, nstime_t *cur_time, ssize_t decay_ms);

/*
 * Compute how many of 'npages_new' pages we would need to purge in 'time'.
 */
uint64_t decay_npages_purge_in(decay_t *decay, nstime_t *time,
    size_t npages_new);

/* Returns true if the epoch advanced and there are pages to purge. */
bool decay_maybe_advance_epoch(decay_t *decay, nstime_t *new_time,
    size_t current_npages);

/*
 * Calculates wait time until a number of pages in the interval
 * [0.5 * npages_threshold .. 1.5 * npages_threshold] should be purged.
 *
 * Returns number of nanoseconds or DECAY_UNBOUNDED_TIME_TO_PURGE in case of
 * indefinite wait.
 */
uint64_t decay_ns_until_purge(decay_t *decay, size_t npages_current,
    uint64_t npages_threshold);

#endif /* JEMALLOC_INTERNAL_DECAY_H */

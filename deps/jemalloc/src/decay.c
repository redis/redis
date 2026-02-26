#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/decay.h"

static const uint64_t h_steps[SMOOTHSTEP_NSTEPS] = {
#define STEP(step, h, x, y)			\
		h,
		SMOOTHSTEP
#undef STEP
};

/*
 * Generate a new deadline that is uniformly random within the next epoch after
 * the current one.
 */
void
decay_deadline_init(decay_t *decay) {
	nstime_copy(&decay->deadline, &decay->epoch);
	nstime_add(&decay->deadline, &decay->interval);
	if (decay_ms_read(decay) > 0) {
		nstime_t jitter;

		nstime_init(&jitter, prng_range_u64(&decay->jitter_state,
		    nstime_ns(&decay->interval)));
		nstime_add(&decay->deadline, &jitter);
	}
}

void
decay_reinit(decay_t *decay, nstime_t *cur_time, ssize_t decay_ms) {
	atomic_store_zd(&decay->time_ms, decay_ms, ATOMIC_RELAXED);
	if (decay_ms > 0) {
		nstime_init(&decay->interval, (uint64_t)decay_ms *
		    KQU(1000000));
		nstime_idivide(&decay->interval, SMOOTHSTEP_NSTEPS);
	}

	nstime_copy(&decay->epoch, cur_time);
	decay->jitter_state = (uint64_t)(uintptr_t)decay;
	decay_deadline_init(decay);
	decay->nunpurged = 0;
	memset(decay->backlog, 0, SMOOTHSTEP_NSTEPS * sizeof(size_t));
}

bool
decay_init(decay_t *decay, nstime_t *cur_time, ssize_t decay_ms) {
	if (config_debug) {
		for (size_t i = 0; i < sizeof(decay_t); i++) {
			assert(((char *)decay)[i] == 0);
		}
		decay->ceil_npages = 0;
	}
	if (malloc_mutex_init(&decay->mtx, "decay", WITNESS_RANK_DECAY,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}
	decay->purging = false;
	decay_reinit(decay, cur_time, decay_ms);
	return false;
}

bool
decay_ms_valid(ssize_t decay_ms) {
	if (decay_ms < -1) {
		return false;
	}
	if (decay_ms == -1 || (uint64_t)decay_ms <= NSTIME_SEC_MAX *
	    KQU(1000)) {
		return true;
	}
	return false;
}

static void
decay_maybe_update_time(decay_t *decay, nstime_t *new_time) {
	if (unlikely(!nstime_monotonic() && nstime_compare(&decay->epoch,
	    new_time) > 0)) {
		/*
		 * Time went backwards.  Move the epoch back in time and
		 * generate a new deadline, with the expectation that time
		 * typically flows forward for long enough periods of time that
		 * epochs complete.  Unfortunately, this strategy is susceptible
		 * to clock jitter triggering premature epoch advances, but
		 * clock jitter estimation and compensation isn't feasible here
		 * because calls into this code are event-driven.
		 */
		nstime_copy(&decay->epoch, new_time);
		decay_deadline_init(decay);
	} else {
		/* Verify that time does not go backwards. */
		assert(nstime_compare(&decay->epoch, new_time) <= 0);
	}
}

static size_t
decay_backlog_npages_limit(const decay_t *decay) {
	/*
	 * For each element of decay_backlog, multiply by the corresponding
	 * fixed-point smoothstep decay factor.  Sum the products, then divide
	 * to round down to the nearest whole number of pages.
	 */
	uint64_t sum = 0;
	for (unsigned i = 0; i < SMOOTHSTEP_NSTEPS; i++) {
		sum += decay->backlog[i] * h_steps[i];
	}
	size_t npages_limit_backlog = (size_t)(sum >> SMOOTHSTEP_BFP);

	return npages_limit_backlog;
}

/*
 * Update backlog, assuming that 'nadvance_u64' time intervals have passed.
 * Trailing 'nadvance_u64' records should be erased and 'current_npages' is
 * placed as the newest record.
 */
static void
decay_backlog_update(decay_t *decay, uint64_t nadvance_u64,
    size_t current_npages) {
	if (nadvance_u64 >= SMOOTHSTEP_NSTEPS) {
		memset(decay->backlog, 0, (SMOOTHSTEP_NSTEPS-1) *
		    sizeof(size_t));
	} else {
		size_t nadvance_z = (size_t)nadvance_u64;

		assert((uint64_t)nadvance_z == nadvance_u64);

		memmove(decay->backlog, &decay->backlog[nadvance_z],
		    (SMOOTHSTEP_NSTEPS - nadvance_z) * sizeof(size_t));
		if (nadvance_z > 1) {
			memset(&decay->backlog[SMOOTHSTEP_NSTEPS -
			    nadvance_z], 0, (nadvance_z-1) * sizeof(size_t));
		}
	}

	size_t npages_delta = (current_npages > decay->nunpurged) ?
	    current_npages - decay->nunpurged : 0;
	decay->backlog[SMOOTHSTEP_NSTEPS-1] = npages_delta;

	if (config_debug) {
		if (current_npages > decay->ceil_npages) {
			decay->ceil_npages = current_npages;
		}
		size_t npages_limit = decay_backlog_npages_limit(decay);
		assert(decay->ceil_npages >= npages_limit);
		if (decay->ceil_npages > npages_limit) {
			decay->ceil_npages = npages_limit;
		}
	}
}

static inline bool
decay_deadline_reached(const decay_t *decay, const nstime_t *time) {
	return (nstime_compare(&decay->deadline, time) <= 0);
}

uint64_t
decay_npages_purge_in(decay_t *decay, nstime_t *time, size_t npages_new) {
	uint64_t decay_interval_ns = decay_epoch_duration_ns(decay);
	size_t n_epoch = (size_t)(nstime_ns(time) / decay_interval_ns);

	uint64_t npages_purge;
	if (n_epoch >= SMOOTHSTEP_NSTEPS) {
		npages_purge = npages_new;
	} else {
		uint64_t h_steps_max = h_steps[SMOOTHSTEP_NSTEPS - 1];
		assert(h_steps_max >=
		    h_steps[SMOOTHSTEP_NSTEPS - 1 - n_epoch]);
		npages_purge = npages_new * (h_steps_max -
		    h_steps[SMOOTHSTEP_NSTEPS - 1 - n_epoch]);
		npages_purge >>= SMOOTHSTEP_BFP;
	}
	return npages_purge;
}

bool
decay_maybe_advance_epoch(decay_t *decay, nstime_t *new_time,
    size_t npages_current) {
	/* Handle possible non-monotonicity of time. */
	decay_maybe_update_time(decay, new_time);

	if (!decay_deadline_reached(decay, new_time)) {
		return false;
	}
	nstime_t delta;
	nstime_copy(&delta, new_time);
	nstime_subtract(&delta, &decay->epoch);

	uint64_t nadvance_u64 = nstime_divide(&delta, &decay->interval);
	assert(nadvance_u64 > 0);

	/* Add nadvance_u64 decay intervals to epoch. */
	nstime_copy(&delta, &decay->interval);
	nstime_imultiply(&delta, nadvance_u64);
	nstime_add(&decay->epoch, &delta);

	/* Set a new deadline. */
	decay_deadline_init(decay);

	/* Update the backlog. */
	decay_backlog_update(decay, nadvance_u64, npages_current);

	decay->npages_limit = decay_backlog_npages_limit(decay);
	decay->nunpurged = (decay->npages_limit > npages_current) ?
	    decay->npages_limit : npages_current;

	return true;
}

/*
 * Calculate how many pages should be purged after 'interval'.
 *
 * First, calculate how many pages should remain at the moment, then subtract
 * the number of pages that should remain after 'interval'. The difference is
 * how many pages should be purged until then.
 *
 * The number of pages that should remain at a specific moment is calculated
 * like this: pages(now) = sum(backlog[i] * h_steps[i]). After 'interval'
 * passes, backlog would shift 'interval' positions to the left and sigmoid
 * curve would be applied starting with backlog[interval].
 *
 * The implementation doesn't directly map to the description, but it's
 * essentially the same calculation, optimized to avoid iterating over
 * [interval..SMOOTHSTEP_NSTEPS) twice.
 */
static inline size_t
decay_npurge_after_interval(decay_t *decay, size_t interval) {
	size_t i;
	uint64_t sum = 0;
	for (i = 0; i < interval; i++) {
		sum += decay->backlog[i] * h_steps[i];
	}
	for (; i < SMOOTHSTEP_NSTEPS; i++) {
		sum += decay->backlog[i] *
		    (h_steps[i] - h_steps[i - interval]);
	}

	return (size_t)(sum >> SMOOTHSTEP_BFP);
}

uint64_t decay_ns_until_purge(decay_t *decay, size_t npages_current,
    uint64_t npages_threshold) {
	if (!decay_gradually(decay)) {
		return DECAY_UNBOUNDED_TIME_TO_PURGE;
	}
	uint64_t decay_interval_ns = decay_epoch_duration_ns(decay);
	assert(decay_interval_ns > 0);
	if (npages_current == 0) {
		unsigned i;
		for (i = 0; i < SMOOTHSTEP_NSTEPS; i++) {
			if (decay->backlog[i] > 0) {
				break;
			}
		}
		if (i == SMOOTHSTEP_NSTEPS) {
			/* No dirty pages recorded.  Sleep indefinitely. */
			return DECAY_UNBOUNDED_TIME_TO_PURGE;
		}
	}
	if (npages_current <= npages_threshold) {
		/* Use max interval. */
		return decay_interval_ns * SMOOTHSTEP_NSTEPS;
	}

	/* Minimal 2 intervals to ensure reaching next epoch deadline. */
	size_t lb = 2;
	size_t ub = SMOOTHSTEP_NSTEPS;

	size_t npurge_lb, npurge_ub;
	npurge_lb = decay_npurge_after_interval(decay, lb);
	if (npurge_lb > npages_threshold) {
		return decay_interval_ns * lb;
	}
	npurge_ub = decay_npurge_after_interval(decay, ub);
	if (npurge_ub < npages_threshold) {
		return decay_interval_ns * ub;
	}

	unsigned n_search = 0;
	size_t target, npurge;
	while ((npurge_lb + npages_threshold < npurge_ub) && (lb + 2 < ub)) {
		target = (lb + ub) / 2;
		npurge = decay_npurge_after_interval(decay, target);
		if (npurge > npages_threshold) {
			ub = target;
			npurge_ub = npurge;
		} else {
			lb = target;
			npurge_lb = npurge;
		}
		assert(n_search < lg_floor(SMOOTHSTEP_NSTEPS) + 1);
		++n_search;
	}
	return decay_interval_ns * (ub + lb) / 2;
}

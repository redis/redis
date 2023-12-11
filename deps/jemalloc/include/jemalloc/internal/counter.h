#ifndef JEMALLOC_INTERNAL_COUNTER_H
#define JEMALLOC_INTERNAL_COUNTER_H

#include "jemalloc/internal/mutex.h"

typedef struct counter_accum_s {
	LOCKEDINT_MTX_DECLARE(mtx)
	locked_u64_t accumbytes;
	uint64_t interval;
} counter_accum_t;

JEMALLOC_ALWAYS_INLINE bool
counter_accum(tsdn_t *tsdn, counter_accum_t *counter, uint64_t bytes) {
	uint64_t interval = counter->interval;
	assert(interval > 0);
	LOCKEDINT_MTX_LOCK(tsdn, counter->mtx);
	/*
	 * If the event moves fast enough (and/or if the event handling is slow
	 * enough), extreme overflow can cause counter trigger coalescing.
	 * This is an intentional mechanism that avoids rate-limiting
	 * allocation.
	 */
	bool overflow = locked_inc_mod_u64(tsdn, LOCKEDINT_MTX(counter->mtx),
	    &counter->accumbytes, bytes, interval);
	LOCKEDINT_MTX_UNLOCK(tsdn, counter->mtx);
	return overflow;
}

bool counter_accum_init(counter_accum_t *counter, uint64_t interval);
void counter_prefork(tsdn_t *tsdn, counter_accum_t *counter);
void counter_postfork_parent(tsdn_t *tsdn, counter_accum_t *counter);
void counter_postfork_child(tsdn_t *tsdn, counter_accum_t *counter);

#endif /* JEMALLOC_INTERNAL_COUNTER_H */

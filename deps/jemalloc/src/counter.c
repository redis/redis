#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/counter.h"

bool
counter_accum_init(counter_accum_t *counter, uint64_t interval) {
	if (LOCKEDINT_MTX_INIT(counter->mtx, "counter_accum",
	    WITNESS_RANK_COUNTER_ACCUM, malloc_mutex_rank_exclusive)) {
		return true;
	}
	locked_init_u64_unsynchronized(&counter->accumbytes, 0);
	counter->interval = interval;
	return false;
}

void
counter_prefork(tsdn_t *tsdn, counter_accum_t *counter) {
	LOCKEDINT_MTX_PREFORK(tsdn, counter->mtx);
}

void
counter_postfork_parent(tsdn_t *tsdn, counter_accum_t *counter) {
	LOCKEDINT_MTX_POSTFORK_PARENT(tsdn, counter->mtx);
}

void
counter_postfork_child(tsdn_t *tsdn, counter_accum_t *counter) {
	LOCKEDINT_MTX_POSTFORK_CHILD(tsdn, counter->mtx);
}

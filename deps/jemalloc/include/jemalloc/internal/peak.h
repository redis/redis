#ifndef JEMALLOC_INTERNAL_PEAK_H
#define JEMALLOC_INTERNAL_PEAK_H

typedef struct peak_s peak_t;
struct peak_s {
	/* The highest recorded peak value, after adjustment (see below). */
	uint64_t cur_max;
	/*
	 * The difference between alloc and dalloc at the last set_zero call;
	 * this lets us cancel out the appropriate amount of excess.
	 */
	uint64_t adjustment;
};

#define PEAK_INITIALIZER {0, 0}

static inline uint64_t
peak_max(peak_t *peak) {
	return peak->cur_max;
}

static inline void
peak_update(peak_t *peak, uint64_t alloc, uint64_t dalloc) {
	int64_t candidate_max = (int64_t)(alloc - dalloc - peak->adjustment);
	if (candidate_max > (int64_t)peak->cur_max) {
		peak->cur_max = candidate_max;
	}
}

/* Resets the counter to zero; all peaks are now relative to this point. */
static inline void
peak_set_zero(peak_t *peak, uint64_t alloc, uint64_t dalloc) {
	peak->cur_max = 0;
	peak->adjustment = alloc - dalloc;
}

#endif /* JEMALLOC_INTERNAL_PEAK_H */

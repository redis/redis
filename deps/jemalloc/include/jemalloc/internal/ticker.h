#ifndef JEMALLOC_INTERNAL_TICKER_H
#define JEMALLOC_INTERNAL_TICKER_H

#include "jemalloc/internal/prng.h"
#include "jemalloc/internal/util.h"

/**
 * A ticker makes it easy to count-down events until some limit.  You
 * ticker_init the ticker to trigger every nticks events.  You then notify it
 * that an event has occurred with calls to ticker_tick (or that nticks events
 * have occurred with a call to ticker_ticks), which will return true (and reset
 * the counter) if the countdown hit zero.
 */
typedef struct ticker_s ticker_t;
struct ticker_s {
	int32_t tick;
	int32_t nticks;
};

static inline void
ticker_init(ticker_t *ticker, int32_t nticks) {
	ticker->tick = nticks;
	ticker->nticks = nticks;
}

static inline void
ticker_copy(ticker_t *ticker, const ticker_t *other) {
	*ticker = *other;
}

static inline int32_t
ticker_read(const ticker_t *ticker) {
	return ticker->tick;
}

/*
 * Not intended to be a public API.  Unfortunately, on x86, neither gcc nor
 * clang seems smart enough to turn
 *   ticker->tick -= nticks;
 *   if (unlikely(ticker->tick < 0)) {
 *     fixup ticker
 *     return true;
 *   }
 *   return false;
 * into
 *   subq %nticks_reg, (%ticker_reg)
 *   js fixup ticker
 *
 * unless we force "fixup ticker" out of line.  In that case, gcc gets it right,
 * but clang now does worse than before.  So, on x86 with gcc, we force it out
 * of line, but otherwise let the inlining occur.  Ordinarily this wouldn't be
 * worth the hassle, but this is on the fast path of both malloc and free (via
 * tcache_event).
 */
#if defined(__GNUC__) && !defined(__clang__)				\
    && (defined(__x86_64__) || defined(__i386__))
JEMALLOC_NOINLINE
#endif
static bool
ticker_fixup(ticker_t *ticker) {
	ticker->tick = ticker->nticks;
	return true;
}

static inline bool
ticker_ticks(ticker_t *ticker, int32_t nticks) {
	ticker->tick -= nticks;
	if (unlikely(ticker->tick < 0)) {
		return ticker_fixup(ticker);
	}
	return false;
}

static inline bool
ticker_tick(ticker_t *ticker) {
	return ticker_ticks(ticker, 1);
}

/*
 * Try to tick.  If ticker would fire, return true, but rely on
 * slowpath to reset ticker.
 */
static inline bool
ticker_trytick(ticker_t *ticker) {
	--ticker->tick;
	if (unlikely(ticker->tick < 0)) {
		return true;
	}
	return false;
}

/*
 * The ticker_geom_t is much like the ticker_t, except that instead of ticker
 * having a constant countdown, it has an approximate one; each tick has
 * approximately a 1/nticks chance of triggering the count.
 *
 * The motivation is in triggering arena decay.  With a naive strategy, each
 * thread would maintain a ticker per arena, and check if decay is necessary
 * each time that the arena's ticker fires.  This has two costs:
 * - Since under reasonable assumptions both threads and arenas can scale
 *   linearly with the number of CPUs, maintaining per-arena data in each thread
 *   scales quadratically with the number of CPUs.
 * - These tickers are often a cache miss down tcache flush pathways.
 *
 * By giving each tick a 1/nticks chance of firing, we still maintain the same
 * average number of ticks-until-firing per arena, with only a single ticker's
 * worth of metadata.
 */

/* See ticker.c for an explanation of these constants. */
#define TICKER_GEOM_NBITS 6
#define TICKER_GEOM_MUL 61
extern const uint8_t ticker_geom_table[1 << TICKER_GEOM_NBITS];

/* Not actually any different from ticker_t; just for type safety. */
typedef struct ticker_geom_s ticker_geom_t;
struct ticker_geom_s {
	int32_t tick;
	int32_t nticks;
};

/*
 * Just pick the average delay for the first counter.  We're more concerned with
 * the behavior over long periods of time rather than the exact timing of the
 * initial ticks.
 */
#define TICKER_GEOM_INIT(nticks) {nticks, nticks}

static inline void
ticker_geom_init(ticker_geom_t *ticker, int32_t nticks) {
	/*
	 * Make sure there's no overflow possible.  This shouldn't really be a
	 * problem for reasonable nticks choices, which are all static and
	 * relatively small.
	 */
	assert((uint64_t)nticks * (uint64_t)255 / (uint64_t)TICKER_GEOM_MUL
	    <= (uint64_t)INT32_MAX);
	ticker->tick = nticks;
	ticker->nticks = nticks;
}

static inline int32_t
ticker_geom_read(const ticker_geom_t *ticker) {
	return ticker->tick;
}

/* Same deal as above. */
#if defined(__GNUC__) && !defined(__clang__)				\
    && (defined(__x86_64__) || defined(__i386__))
JEMALLOC_NOINLINE
#endif
static bool
ticker_geom_fixup(ticker_geom_t *ticker, uint64_t *prng_state) {
	uint64_t idx = prng_lg_range_u64(prng_state, TICKER_GEOM_NBITS);
	ticker->tick = (uint32_t)(
	    (uint64_t)ticker->nticks * (uint64_t)ticker_geom_table[idx]
	    / (uint64_t)TICKER_GEOM_MUL);
	return true;
}

static inline bool
ticker_geom_ticks(ticker_geom_t *ticker, uint64_t *prng_state, int32_t nticks) {
	ticker->tick -= nticks;
	if (unlikely(ticker->tick < 0)) {
		return ticker_geom_fixup(ticker, prng_state);
	}
	return false;
}

static inline bool
ticker_geom_tick(ticker_geom_t *ticker, uint64_t *prng_state) {
	return ticker_geom_ticks(ticker, prng_state, 1);
}

#endif /* JEMALLOC_INTERNAL_TICKER_H */

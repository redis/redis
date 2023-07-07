#ifndef JEMALLOC_INTERNAL_INLINES_B_H
#define JEMALLOC_INTERNAL_INLINES_B_H

#include "jemalloc/internal/extent.h"

static inline void
percpu_arena_update(tsd_t *tsd, unsigned cpu) {
	assert(have_percpu_arena);
	arena_t *oldarena = tsd_arena_get(tsd);
	assert(oldarena != NULL);
	unsigned oldind = arena_ind_get(oldarena);

	if (oldind != cpu) {
		unsigned newind = cpu;
		arena_t *newarena = arena_get(tsd_tsdn(tsd), newind, true);
		assert(newarena != NULL);

		/* Set new arena/tcache associations. */
		arena_migrate(tsd, oldarena, newarena);
		tcache_t *tcache = tcache_get(tsd);
		if (tcache != NULL) {
			tcache_slow_t *tcache_slow = tsd_tcache_slowp_get(tsd);
			tcache_arena_reassociate(tsd_tsdn(tsd), tcache_slow,
			    tcache, newarena);
		}
	}
}


/* Choose an arena based on a per-thread value. */
static inline arena_t *
arena_choose_impl(tsd_t *tsd, arena_t *arena, bool internal) {
	arena_t *ret;

	if (arena != NULL) {
		return arena;
	}

	/* During reentrancy, arena 0 is the safest bet. */
	if (unlikely(tsd_reentrancy_level_get(tsd) > 0)) {
		return arena_get(tsd_tsdn(tsd), 0, true);
	}

	ret = internal ? tsd_iarena_get(tsd) : tsd_arena_get(tsd);
	if (unlikely(ret == NULL)) {
		ret = arena_choose_hard(tsd, internal);
		assert(ret);
		if (tcache_available(tsd)) {
			tcache_slow_t *tcache_slow = tsd_tcache_slowp_get(tsd);
			tcache_t *tcache = tsd_tcachep_get(tsd);
			if (tcache_slow->arena != NULL) {
				/* See comments in tsd_tcache_data_init().*/
				assert(tcache_slow->arena ==
				    arena_get(tsd_tsdn(tsd), 0, false));
				if (tcache_slow->arena != ret) {
					tcache_arena_reassociate(tsd_tsdn(tsd),
					    tcache_slow, tcache, ret);
				}
			} else {
				tcache_arena_associate(tsd_tsdn(tsd),
				    tcache_slow, tcache, ret);
			}
		}
	}

	/*
	 * Note that for percpu arena, if the current arena is outside of the
	 * auto percpu arena range, (i.e. thread is assigned to a manually
	 * managed arena), then percpu arena is skipped.
	 */
	if (have_percpu_arena && PERCPU_ARENA_ENABLED(opt_percpu_arena) &&
	    !internal && (arena_ind_get(ret) <
	    percpu_arena_ind_limit(opt_percpu_arena)) && (ret->last_thd !=
	    tsd_tsdn(tsd))) {
		unsigned ind = percpu_arena_choose();
		if (arena_ind_get(ret) != ind) {
			percpu_arena_update(tsd, ind);
			ret = tsd_arena_get(tsd);
		}
		ret->last_thd = tsd_tsdn(tsd);
	}

	return ret;
}

static inline arena_t *
arena_choose(tsd_t *tsd, arena_t *arena) {
	return arena_choose_impl(tsd, arena, false);
}

static inline arena_t *
arena_ichoose(tsd_t *tsd, arena_t *arena) {
	return arena_choose_impl(tsd, arena, true);
}

static inline bool
arena_is_auto(arena_t *arena) {
	assert(narenas_auto > 0);

	return (arena_ind_get(arena) < manual_arena_base);
}

#endif /* JEMALLOC_INTERNAL_INLINES_B_H */

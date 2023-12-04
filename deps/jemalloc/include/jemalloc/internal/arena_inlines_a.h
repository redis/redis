#ifndef JEMALLOC_INTERNAL_ARENA_INLINES_A_H
#define JEMALLOC_INTERNAL_ARENA_INLINES_A_H

static inline unsigned
arena_ind_get(const arena_t *arena) {
	return arena->ind;
}

static inline void
arena_internal_add(arena_t *arena, size_t size) {
	atomic_fetch_add_zu(&arena->stats.internal, size, ATOMIC_RELAXED);
}

static inline void
arena_internal_sub(arena_t *arena, size_t size) {
	atomic_fetch_sub_zu(&arena->stats.internal, size, ATOMIC_RELAXED);
}

static inline size_t
arena_internal_get(arena_t *arena) {
	return atomic_load_zu(&arena->stats.internal, ATOMIC_RELAXED);
}

#endif /* JEMALLOC_INTERNAL_ARENA_INLINES_A_H */

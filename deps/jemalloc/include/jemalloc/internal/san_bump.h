#ifndef JEMALLOC_INTERNAL_SAN_BUMP_H
#define JEMALLOC_INTERNAL_SAN_BUMP_H

#include "jemalloc/internal/edata.h"
#include "jemalloc/internal/exp_grow.h"
#include "jemalloc/internal/mutex.h"

#define SBA_RETAINED_ALLOC_SIZE ((size_t)4 << 20)

extern bool opt_retain;

typedef struct ehooks_s ehooks_t;
typedef struct pac_s pac_t;

typedef struct san_bump_alloc_s san_bump_alloc_t;
struct san_bump_alloc_s {
	malloc_mutex_t mtx;

	edata_t *curr_reg;
};

static inline bool
san_bump_enabled() {
	/*
	 * We enable san_bump allocator only when it's possible to break up a
	 * mapping and unmap a part of it (maps_coalesce). This is needed to
	 * ensure the arena destruction process can destroy all retained guarded
	 * extents one by one and to unmap a trailing part of a retained guarded
	 * region when it's too small to fit a pending allocation.
	 * opt_retain is required, because this allocator retains a large
	 * virtual memory mapping and returns smaller parts of it.
	 */
	return maps_coalesce && opt_retain;
}

static inline bool
san_bump_alloc_init(san_bump_alloc_t* sba) {
	bool err = malloc_mutex_init(&sba->mtx, "sanitizer_bump_allocator",
	    WITNESS_RANK_SAN_BUMP_ALLOC, malloc_mutex_rank_exclusive);
	if (err) {
		return true;
	}
	sba->curr_reg = NULL;

	return false;
}

edata_t *
san_bump_alloc(tsdn_t *tsdn, san_bump_alloc_t* sba, pac_t *pac, ehooks_t *ehooks,
    size_t size, bool zero);

#endif /* JEMALLOC_INTERNAL_SAN_BUMP_H */

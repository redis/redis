#ifndef JEMALLOC_INTERNAL_EDATA_CACHE_H
#define JEMALLOC_INTERNAL_EDATA_CACHE_H

#include "jemalloc/internal/base.h"

/* For tests only. */
#define EDATA_CACHE_FAST_FILL 4

/*
 * A cache of edata_t structures allocated via base_alloc_edata (as opposed to
 * the underlying extents they describe).  The contents of returned edata_t
 * objects are garbage and cannot be relied upon.
 */

typedef struct edata_cache_s edata_cache_t;
struct edata_cache_s {
	edata_avail_t avail;
	atomic_zu_t count;
	malloc_mutex_t mtx;
	base_t *base;
};

bool edata_cache_init(edata_cache_t *edata_cache, base_t *base);
edata_t *edata_cache_get(tsdn_t *tsdn, edata_cache_t *edata_cache);
void edata_cache_put(tsdn_t *tsdn, edata_cache_t *edata_cache, edata_t *edata);

void edata_cache_prefork(tsdn_t *tsdn, edata_cache_t *edata_cache);
void edata_cache_postfork_parent(tsdn_t *tsdn, edata_cache_t *edata_cache);
void edata_cache_postfork_child(tsdn_t *tsdn, edata_cache_t *edata_cache);

/*
 * An edata_cache_small is like an edata_cache, but it relies on external
 * synchronization and avoids first-fit strategies.
 */

typedef struct edata_cache_fast_s edata_cache_fast_t;
struct edata_cache_fast_s {
	edata_list_inactive_t list;
	edata_cache_t *fallback;
	bool disabled;
};

void edata_cache_fast_init(edata_cache_fast_t *ecs, edata_cache_t *fallback);
edata_t *edata_cache_fast_get(tsdn_t *tsdn, edata_cache_fast_t *ecs);
void edata_cache_fast_put(tsdn_t *tsdn, edata_cache_fast_t *ecs,
    edata_t *edata);
void edata_cache_fast_disable(tsdn_t *tsdn, edata_cache_fast_t *ecs);

#endif /* JEMALLOC_INTERNAL_EDATA_CACHE_H */

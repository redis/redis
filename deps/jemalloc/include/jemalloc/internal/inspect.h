#ifndef JEMALLOC_INTERNAL_INSPECT_H
#define JEMALLOC_INTERNAL_INSPECT_H

/*
 * This module contains the heap introspection capabilities.  For now they are
 * exposed purely through mallctl APIs in the experimental namespace, but this
 * may change over time.
 */

/*
 * The following two structs are for experimental purposes. See
 * experimental_utilization_query_ctl and
 * experimental_utilization_batch_query_ctl in src/ctl.c.
 */
typedef struct inspect_extent_util_stats_s inspect_extent_util_stats_t;
struct inspect_extent_util_stats_s {
	size_t nfree;
	size_t nregs;
	size_t size;
};

typedef struct inspect_extent_util_stats_verbose_s
    inspect_extent_util_stats_verbose_t;

struct inspect_extent_util_stats_verbose_s {
	void *slabcur_addr;
	size_t nfree;
	size_t nregs;
	size_t size;
	size_t bin_nfree;
	size_t bin_nregs;
};

void inspect_extent_util_stats_get(tsdn_t *tsdn, const void *ptr,
    size_t *nfree, size_t *nregs, size_t *size);
void inspect_extent_util_stats_verbose_get(tsdn_t *tsdn, const void *ptr,
    size_t *nfree, size_t *nregs, size_t *size,
    size_t *bin_nfree, size_t *bin_nregs, void **slabcur_addr);

#endif /* JEMALLOC_INTERNAL_INSPECT_H */

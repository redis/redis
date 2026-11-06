#include "test/jemalloc_test.h"

#include <stdio.h>

/*
 * Print the sizes of various important core data structures.  OK, I guess this
 * isn't really a "stress" test, but it does give useful information about
 * low-level performance characteristics, as the other things in this directory
 * do.
 */

static void
do_print(const char *name, size_t sz_bytes) {
	const char *sizes[] = {"bytes", "KB", "MB", "GB", "TB", "PB", "EB",
		"ZB"};
	size_t sizes_max = sizeof(sizes)/sizeof(sizes[0]);

	size_t ind = 0;
	double sz = sz_bytes;
	while (sz >= 1024 && ind < sizes_max - 1) {
		sz /= 1024;
		ind++;
	}
	if (ind == 0) {
		printf("%-20s: %zu bytes\n", name, sz_bytes);
	} else {
		printf("%-20s: %f %s\n", name, sz, sizes[ind]);
	}
}

int
main() {
#define P(type)								\
	do_print(#type, sizeof(type))
	P(arena_t);
	P(arena_stats_t);
	P(base_t);
	P(decay_t);
	P(edata_t);
	P(ecache_t);
	P(eset_t);
	P(malloc_mutex_t);
	P(prof_tctx_t);
	P(prof_gctx_t);
	P(prof_tdata_t);
	P(rtree_t);
	P(rtree_leaf_elm_t);
	P(slab_data_t);
	P(tcache_t);
	P(tcache_slow_t);
	P(tsd_t);
#undef P
}

#ifndef JEMALLOC_INTERNAL_BIN_INFO_H
#define JEMALLOC_INTERNAL_BIN_INFO_H

#include "jemalloc/internal/bitmap.h"

/*
 * Read-only information associated with each element of arena_t's bins array
 * is stored separately, partly to reduce memory usage (only one copy, rather
 * than one per arena), but mainly to avoid false cacheline sharing.
 *
 * Each slab has the following layout:
 *
 *   /--------------------\
 *   | region 0           |
 *   |--------------------|
 *   | region 1           |
 *   |--------------------|
 *   | ...                |
 *   | ...                |
 *   | ...                |
 *   |--------------------|
 *   | region nregs-1     |
 *   \--------------------/
 */
typedef struct bin_info_s bin_info_t;
struct bin_info_s {
	/* Size of regions in a slab for this bin's size class. */
	size_t			reg_size;

	/* Total size of a slab for this bin's size class. */
	size_t			slab_size;

	/* Total number of regions in a slab for this bin's size class. */
	uint32_t		nregs;

	/* Number of sharded bins in each arena for this size class. */
	uint32_t		n_shards;

	/*
	 * Metadata used to manipulate bitmaps for slabs associated with this
	 * bin.
	 */
	bitmap_info_t		bitmap_info;
};

extern bin_info_t bin_infos[SC_NBINS];

void bin_info_boot(sc_data_t *sc_data, unsigned bin_shard_sizes[SC_NBINS]);

#endif /* JEMALLOC_INTERNAL_BIN_INFO_H */

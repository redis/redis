#ifndef JEMALLOC_INTERNAL_SLAB_DATA_H
#define JEMALLOC_INTERNAL_SLAB_DATA_H

#include "jemalloc/internal/bitmap.h"

typedef struct slab_data_s slab_data_t;
struct slab_data_s {
	/* Per region allocated/deallocated bitmap. */
	bitmap_t bitmap[BITMAP_GROUPS_MAX];
};

#endif /* JEMALLOC_INTERNAL_SLAB_DATA_H */

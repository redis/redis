#ifndef JEMALLOC_INTERNAL_BIN_TYPES_H
#define JEMALLOC_INTERNAL_BIN_TYPES_H

#include "jemalloc/internal/sc.h"

#define BIN_SHARDS_MAX (1 << EDATA_BITS_BINSHARD_WIDTH)
#define N_BIN_SHARDS_DEFAULT 1

/* Used in TSD static initializer only. Real init in arena_bind(). */
#define TSD_BINSHARDS_ZERO_INITIALIZER {{UINT8_MAX}}

typedef struct tsd_binshards_s tsd_binshards_t;
struct tsd_binshards_s {
	uint8_t binshard[SC_NBINS];
};

#endif /* JEMALLOC_INTERNAL_BIN_TYPES_H */

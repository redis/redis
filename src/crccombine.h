
#include <stdint.h>


#define USE_STATIC_COMBINE_CACHE 1
// mask types
typedef unsigned long long v2uq __attribute__ ((vector_size (16)));

// final implementation
uint64_t gf2_matrix_times_vec2(uint64_t *mat, uint64_t vec);


#if USE_STATIC_COMBINE_CACHE
void init_combine_cache(uint64_t poly, uint8_t dim);
#endif

uint64_t crc64_combine(uint64_t crc1, uint64_t crc2, uintmax_t len2, uint64_t poly, uint8_t dim);

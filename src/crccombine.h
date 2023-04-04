
#include <stdint.h>

// vector combine gets 4-10x improvement on CRC combine by itself
#define USE_VECTOR_COMBINE_INNER 1
#define USE_SWITCH_IF_NO_VECTOR 1
// consider testing for your platform, slower for me
#define HAVE_AVX2_AND_WANT_USE_AVX2 0
// Without vector combine, this nets ~50-200x improvement by itself
#define USE_STATIC_COMBINE_CACHE 1
// With USE_VECTOR_COMBINE_INNER and USE_STATIC_VECTOR_COMBINE_CACHE:
// While we expect 200x-2000x improvement, we are >10,000x faster for the
// CRC combine operation. The combination is SUPER EFFECTIVE!

// mask types
typedef unsigned long long v8uq __attribute__ ((vector_size (64)));
typedef unsigned long long v4uq __attribute__ ((vector_size (32)));
typedef unsigned long long v2uq __attribute__ ((vector_size (16)));

// used when !USE_VECTOR_COMBINE_INNER and !USE_SWITCH_IF_NO_VECTOR
uint64_t gf2_matrix_times_original(uint64_t *mat, uint64_t vec);

// used when !USE_VECTOR_COMBINE_INNER and USE_SWITCH_IF_NO_VECTOR
uint64_t gf2_matrix_times_switch(uint64_t *mat, uint64_t vec);

// used when USE_VECTOR_COMBINE_INNER and HAVE_AVX2_AND_WANT_USE_AVX2
uint64_t gf2_matrix_times_vec_avx2(uint64_t *mat, uint64_t vec);

// used by USE_VECTOR_COMBINE_INNER and !HAVE_AVX2_AND_WANT_USE_AVX2
uint64_t gf2_matrix_times_vec2(uint64_t *mat, uint64_t vec);

// not used, but the cleanest inner macro of them all, almost as fast
// as gf2_matrix_times_vec2
uint64_t gf2_matrix_times_vec(uint64_t *mat, uint64_t vec);

// not used, seems to do a lot of register shuffling
uint64_t gf2_matrix_times_vec8(uint64_t *mat, uint64_t vec);


#if USE_STATIC_COMBINE_CACHE
void init_combine_cache(uint64_t poly, uint8_t dim);
#endif

uint64_t crc64_combine(uint64_t crc1, uint64_t crc2, uintmax_t len2, uint64_t poly, uint8_t dim);

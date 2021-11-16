#ifndef JEMALLOC_INTERNAL_BIT_UTIL_H
#define JEMALLOC_INTERNAL_BIT_UTIL_H

#include "jemalloc/internal/assert.h"

#define BIT_UTIL_INLINE static inline

/* Sanity check. */
#if !defined(JEMALLOC_INTERNAL_FFSLL) || !defined(JEMALLOC_INTERNAL_FFSL) \
    || !defined(JEMALLOC_INTERNAL_FFS)
#  error JEMALLOC_INTERNAL_FFS{,L,LL} should have been defined by configure
#endif


BIT_UTIL_INLINE unsigned
ffs_llu(unsigned long long bitmap) {
	return JEMALLOC_INTERNAL_FFSLL(bitmap);
}

BIT_UTIL_INLINE unsigned
ffs_lu(unsigned long bitmap) {
	return JEMALLOC_INTERNAL_FFSL(bitmap);
}

BIT_UTIL_INLINE unsigned
ffs_u(unsigned bitmap) {
	return JEMALLOC_INTERNAL_FFS(bitmap);
}

#ifdef JEMALLOC_INTERNAL_POPCOUNTL
BIT_UTIL_INLINE unsigned
popcount_lu(unsigned long bitmap) {
  return JEMALLOC_INTERNAL_POPCOUNTL(bitmap);
}
#endif

/*
 * Clears first unset bit in bitmap, and returns
 * place of bit.  bitmap *must not* be 0.
 */

BIT_UTIL_INLINE size_t
cfs_lu(unsigned long* bitmap) {
	size_t bit = ffs_lu(*bitmap) - 1;
	*bitmap ^= ZU(1) << bit;
	return bit;
}

BIT_UTIL_INLINE unsigned
ffs_zu(size_t bitmap) {
#if LG_SIZEOF_PTR == LG_SIZEOF_INT
	return ffs_u(bitmap);
#elif LG_SIZEOF_PTR == LG_SIZEOF_LONG
	return ffs_lu(bitmap);
#elif LG_SIZEOF_PTR == LG_SIZEOF_LONG_LONG
	return ffs_llu(bitmap);
#else
#error No implementation for size_t ffs()
#endif
}

BIT_UTIL_INLINE unsigned
ffs_u64(uint64_t bitmap) {
#if LG_SIZEOF_LONG == 3
	return ffs_lu(bitmap);
#elif LG_SIZEOF_LONG_LONG == 3
	return ffs_llu(bitmap);
#else
#error No implementation for 64-bit ffs()
#endif
}

BIT_UTIL_INLINE unsigned
ffs_u32(uint32_t bitmap) {
#if LG_SIZEOF_INT == 2
	return ffs_u(bitmap);
#else
#error No implementation for 32-bit ffs()
#endif
	return ffs_u(bitmap);
}

BIT_UTIL_INLINE uint64_t
pow2_ceil_u64(uint64_t x) {
#if (defined(__amd64__) || defined(__x86_64__) || defined(JEMALLOC_HAVE_BUILTIN_CLZ))
	if(unlikely(x <= 1)) {
		return x;
	}
	size_t msb_on_index;
#if (defined(__amd64__) || defined(__x86_64__))
	asm ("bsrq %1, %0"
			: "=r"(msb_on_index) // Outputs.
			: "r"(x-1)           // Inputs.
		);
#elif (defined(JEMALLOC_HAVE_BUILTIN_CLZ))
	msb_on_index = (63 ^ __builtin_clzll(x - 1));
#endif
	assert(msb_on_index < 63);
	return 1ULL << (msb_on_index + 1);
#else
	x--;
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
	x |= x >> 32;
	x++;
	return x;
#endif
}

BIT_UTIL_INLINE uint32_t
pow2_ceil_u32(uint32_t x) {
#if ((defined(__i386__) || defined(JEMALLOC_HAVE_BUILTIN_CLZ)) && (!defined(__s390__)))
	if(unlikely(x <= 1)) {
		return x;
	}
	size_t msb_on_index;
#if (defined(__i386__))
	asm ("bsr %1, %0"
			: "=r"(msb_on_index) // Outputs.
			: "r"(x-1)           // Inputs.
		);
#elif (defined(JEMALLOC_HAVE_BUILTIN_CLZ))
	msb_on_index = (31 ^ __builtin_clz(x - 1));
#endif
	assert(msb_on_index < 31);
	return 1U << (msb_on_index + 1);
#else
	x--;
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
	x++;
	return x;
#endif
}

/* Compute the smallest power of 2 that is >= x. */
BIT_UTIL_INLINE size_t
pow2_ceil_zu(size_t x) {
#if (LG_SIZEOF_PTR == 3)
	return pow2_ceil_u64(x);
#else
	return pow2_ceil_u32(x);
#endif
}

#if (defined(__i386__) || defined(__amd64__) || defined(__x86_64__))
BIT_UTIL_INLINE unsigned
lg_floor(size_t x) {
	size_t ret;
	assert(x != 0);

	asm ("bsr %1, %0"
	    : "=r"(ret) // Outputs.
	    : "r"(x)    // Inputs.
	    );
	assert(ret < UINT_MAX);
	return (unsigned)ret;
}
#elif (defined(_MSC_VER))
BIT_UTIL_INLINE unsigned
lg_floor(size_t x) {
	unsigned long ret;

	assert(x != 0);

#if (LG_SIZEOF_PTR == 3)
	_BitScanReverse64(&ret, x);
#elif (LG_SIZEOF_PTR == 2)
	_BitScanReverse(&ret, x);
#else
#  error "Unsupported type size for lg_floor()"
#endif
	assert(ret < UINT_MAX);
	return (unsigned)ret;
}
#elif (defined(JEMALLOC_HAVE_BUILTIN_CLZ))
BIT_UTIL_INLINE unsigned
lg_floor(size_t x) {
	assert(x != 0);

#if (LG_SIZEOF_PTR == LG_SIZEOF_INT)
	return ((8 << LG_SIZEOF_PTR) - 1) - __builtin_clz(x);
#elif (LG_SIZEOF_PTR == LG_SIZEOF_LONG)
	return ((8 << LG_SIZEOF_PTR) - 1) - __builtin_clzl(x);
#else
#  error "Unsupported type size for lg_floor()"
#endif
}
#else
BIT_UTIL_INLINE unsigned
lg_floor(size_t x) {
	assert(x != 0);

	x |= (x >> 1);
	x |= (x >> 2);
	x |= (x >> 4);
	x |= (x >> 8);
	x |= (x >> 16);
#if (LG_SIZEOF_PTR == 3)
	x |= (x >> 32);
#endif
	if (x == SIZE_T_MAX) {
		return (8 << LG_SIZEOF_PTR) - 1;
	}
	x++;
	return ffs_zu(x) - 2;
}
#endif

BIT_UTIL_INLINE unsigned
lg_ceil(size_t x) {
	return lg_floor(x) + ((x & (x - 1)) == 0 ? 0 : 1);
}

#undef BIT_UTIL_INLINE

/* A compile-time version of lg_floor and lg_ceil. */
#define LG_FLOOR_1(x) 0
#define LG_FLOOR_2(x) (x < (1ULL << 1) ? LG_FLOOR_1(x) : 1 + LG_FLOOR_1(x >> 1))
#define LG_FLOOR_4(x) (x < (1ULL << 2) ? LG_FLOOR_2(x) : 2 + LG_FLOOR_2(x >> 2))
#define LG_FLOOR_8(x) (x < (1ULL << 4) ? LG_FLOOR_4(x) : 4 + LG_FLOOR_4(x >> 4))
#define LG_FLOOR_16(x) (x < (1ULL << 8) ? LG_FLOOR_8(x) : 8 + LG_FLOOR_8(x >> 8))
#define LG_FLOOR_32(x) (x < (1ULL << 16) ? LG_FLOOR_16(x) : 16 + LG_FLOOR_16(x >> 16))
#define LG_FLOOR_64(x) (x < (1ULL << 32) ? LG_FLOOR_32(x) : 32 + LG_FLOOR_32(x >> 32))
#if LG_SIZEOF_PTR == 2
#  define LG_FLOOR(x) LG_FLOOR_32((x))
#else
#  define LG_FLOOR(x) LG_FLOOR_64((x))
#endif

#define LG_CEIL(x) (LG_FLOOR(x) + (((x) & ((x) - 1)) == 0 ? 0 : 1))

#endif /* JEMALLOC_INTERNAL_BIT_UTIL_H */

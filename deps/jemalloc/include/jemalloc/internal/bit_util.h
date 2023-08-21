#ifndef JEMALLOC_INTERNAL_BIT_UTIL_H
#define JEMALLOC_INTERNAL_BIT_UTIL_H

#include "jemalloc/internal/assert.h"

/* Sanity check. */
#if !defined(JEMALLOC_INTERNAL_FFSLL) || !defined(JEMALLOC_INTERNAL_FFSL) \
    || !defined(JEMALLOC_INTERNAL_FFS)
#  error JEMALLOC_INTERNAL_FFS{,L,LL} should have been defined by configure
#endif

/*
 * Unlike the builtins and posix ffs functions, our ffs requires a non-zero
 * input, and returns the position of the lowest bit set (as opposed to the
 * posix versions, which return 1 larger than that position and use a return
 * value of zero as a sentinel.  This tends to simplify logic in callers, and
 * allows for consistency with the builtins we build fls on top of.
 */
static inline unsigned
ffs_llu(unsigned long long x) {
	util_assume(x != 0);
	return JEMALLOC_INTERNAL_FFSLL(x) - 1;
}

static inline unsigned
ffs_lu(unsigned long x) {
	util_assume(x != 0);
	return JEMALLOC_INTERNAL_FFSL(x) - 1;
}

static inline unsigned
ffs_u(unsigned x) {
	util_assume(x != 0);
	return JEMALLOC_INTERNAL_FFS(x) - 1;
}

#define DO_FLS_SLOW(x, suffix) do {					\
	util_assume(x != 0);						\
	x |= (x >> 1);							\
	x |= (x >> 2);							\
	x |= (x >> 4);							\
	x |= (x >> 8);							\
	x |= (x >> 16);							\
	if (sizeof(x) > 4) {						\
		/*							\
		 * If sizeof(x) is 4, then the expression "x >> 32"	\
		 * will generate compiler warnings even if the code	\
		 * never executes.  This circumvents the warning, and	\
		 * gets compiled out in optimized builds.		\
		 */							\
		int constant_32 = sizeof(x) * 4;			\
		x |= (x >> constant_32);				\
	}								\
	x++;								\
	if (x == 0) {							\
		return 8 * sizeof(x) - 1;				\
	}								\
	return ffs_##suffix(x) - 1;					\
} while(0)

static inline unsigned
fls_llu_slow(unsigned long long x) {
	DO_FLS_SLOW(x, llu);
}

static inline unsigned
fls_lu_slow(unsigned long x) {
	DO_FLS_SLOW(x, lu);
}

static inline unsigned
fls_u_slow(unsigned x) {
	DO_FLS_SLOW(x, u);
}

#undef DO_FLS_SLOW

#ifdef JEMALLOC_HAVE_BUILTIN_CLZ
static inline unsigned
fls_llu(unsigned long long x) {
	util_assume(x != 0);
	/*
	 * Note that the xor here is more naturally written as subtraction; the
	 * last bit set is the number of bits in the type minus the number of
	 * leading zero bits.  But GCC implements that as:
	 *    bsr     edi, edi
	 *    mov     eax, 31
	 *    xor     edi, 31
	 *    sub     eax, edi
	 * If we write it as xor instead, then we get
	 *    bsr     eax, edi
	 * as desired.
	 */
	return (8 * sizeof(x) - 1) ^ __builtin_clzll(x);
}

static inline unsigned
fls_lu(unsigned long x) {
	util_assume(x != 0);
	return (8 * sizeof(x) - 1) ^ __builtin_clzl(x);
}

static inline unsigned
fls_u(unsigned x) {
	util_assume(x != 0);
	return (8 * sizeof(x) - 1) ^ __builtin_clz(x);
}
#elif defined(_MSC_VER)

#if LG_SIZEOF_PTR == 3
#define DO_BSR64(bit, x) _BitScanReverse64(&bit, x)
#else
/*
 * This never actually runs; we're just dodging a compiler error for the
 * never-taken branch where sizeof(void *) == 8.
 */
#define DO_BSR64(bit, x) bit = 0; unreachable()
#endif

#define DO_FLS(x) do {							\
	if (x == 0) {							\
		return 8 * sizeof(x);					\
	}								\
	unsigned long bit;						\
	if (sizeof(x) == 4) {						\
		_BitScanReverse(&bit, (unsigned)x);			\
		return (unsigned)bit;					\
	}								\
	if (sizeof(x) == 8 && sizeof(void *) == 8) {			\
		DO_BSR64(bit, x);					\
		return (unsigned)bit;					\
	}								\
	if (sizeof(x) == 8 && sizeof(void *) == 4) {			\
		/* Dodge a compiler warning, as above. */		\
		int constant_32 = sizeof(x) * 4;			\
		if (_BitScanReverse(&bit,				\
		    (unsigned)(x >> constant_32))) {			\
			return 32 + (unsigned)bit;			\
		} else {						\
			_BitScanReverse(&bit, (unsigned)x);		\
			return (unsigned)bit;				\
		}							\
	}								\
	unreachable();							\
} while (0)

static inline unsigned
fls_llu(unsigned long long x) {
	DO_FLS(x);
}

static inline unsigned
fls_lu(unsigned long x) {
	DO_FLS(x);
}

static inline unsigned
fls_u(unsigned x) {
	DO_FLS(x);
}

#undef DO_FLS
#undef DO_BSR64
#else

static inline unsigned
fls_llu(unsigned long long x) {
	return fls_llu_slow(x);
}

static inline unsigned
fls_lu(unsigned long x) {
	return fls_lu_slow(x);
}

static inline unsigned
fls_u(unsigned x) {
	return fls_u_slow(x);
}
#endif

#if LG_SIZEOF_LONG_LONG > 3
#  error "Haven't implemented popcount for 16-byte ints."
#endif

#define DO_POPCOUNT(x, type) do {					\
	/*								\
	 * Algorithm from an old AMD optimization reference manual.	\
	 * We're putting a little bit more work than you might expect	\
	 * into the no-instrinsic case, since we only support the	\
	 * GCC intrinsics spelling of popcount (for now).  Detecting	\
	 * whether or not the popcount builtin is actually useable in	\
	 * MSVC is nontrivial.						\
	 */								\
									\
	type bmul = (type)0x0101010101010101ULL;			\
									\
	/*								\
	 * Replace each 2 bits with the sideways sum of the original	\
	 * values.  0x5 = 0b0101.					\
	 *								\
	 * You might expect this to be:					\
	 *   x = (x & 0x55...) + ((x >> 1) & 0x55...).			\
	 * That costs an extra mask relative to this, though.		\
	 */								\
	x = x - ((x >> 1) & (0x55U * bmul));				\
	/* Replace each 4 bits with their sideays sum.  0x3 = 0b0011. */\
	x = (x & (bmul * 0x33U)) + ((x >> 2) & (bmul * 0x33U));		\
	/*								\
	 * Replace each 8 bits with their sideways sum.  Note that we	\
	 * can't overflow within each 4-bit sum here, so we can skip	\
	 * the initial mask.						\
	 */								\
	x = (x + (x >> 4)) & (bmul * 0x0FU);				\
	/*								\
	 * None of the partial sums in this multiplication (viewed in	\
	 * base-256) can overflow into the next digit.  So the least	\
	 * significant byte of the product will be the least		\
	 * significant byte of the original value, the second least	\
	 * significant byte will be the sum of the two least		\
	 * significant bytes of the original value, and so on.		\
	 * Importantly, the high byte will be the byte-wise sum of all	\
	 * the bytes of the original value.				\
	 */								\
	x = x * bmul;							\
	x >>= ((sizeof(x) - 1) * 8);					\
	return (unsigned)x;						\
} while(0)

static inline unsigned
popcount_u_slow(unsigned bitmap) {
	DO_POPCOUNT(bitmap, unsigned);
}

static inline unsigned
popcount_lu_slow(unsigned long bitmap) {
	DO_POPCOUNT(bitmap, unsigned long);
}

static inline unsigned
popcount_llu_slow(unsigned long long bitmap) {
	DO_POPCOUNT(bitmap, unsigned long long);
}

#undef DO_POPCOUNT

static inline unsigned
popcount_u(unsigned bitmap) {
#ifdef JEMALLOC_INTERNAL_POPCOUNT
	return JEMALLOC_INTERNAL_POPCOUNT(bitmap);
#else
	return popcount_u_slow(bitmap);
#endif
}

static inline unsigned
popcount_lu(unsigned long bitmap) {
#ifdef JEMALLOC_INTERNAL_POPCOUNTL
	return JEMALLOC_INTERNAL_POPCOUNTL(bitmap);
#else
	return popcount_lu_slow(bitmap);
#endif
}

static inline unsigned
popcount_llu(unsigned long long bitmap) {
#ifdef JEMALLOC_INTERNAL_POPCOUNTLL
	return JEMALLOC_INTERNAL_POPCOUNTLL(bitmap);
#else
	return popcount_llu_slow(bitmap);
#endif
}

/*
 * Clears first unset bit in bitmap, and returns
 * place of bit.  bitmap *must not* be 0.
 */

static inline size_t
cfs_lu(unsigned long* bitmap) {
	util_assume(*bitmap != 0);
	size_t bit = ffs_lu(*bitmap);
	*bitmap ^= ZU(1) << bit;
	return bit;
}

static inline unsigned
ffs_zu(size_t x) {
#if LG_SIZEOF_PTR == LG_SIZEOF_INT
	return ffs_u(x);
#elif LG_SIZEOF_PTR == LG_SIZEOF_LONG
	return ffs_lu(x);
#elif LG_SIZEOF_PTR == LG_SIZEOF_LONG_LONG
	return ffs_llu(x);
#else
#error No implementation for size_t ffs()
#endif
}

static inline unsigned
fls_zu(size_t x) {
#if LG_SIZEOF_PTR == LG_SIZEOF_INT
	return fls_u(x);
#elif LG_SIZEOF_PTR == LG_SIZEOF_LONG
	return fls_lu(x);
#elif LG_SIZEOF_PTR == LG_SIZEOF_LONG_LONG
	return fls_llu(x);
#else
#error No implementation for size_t fls()
#endif
}


static inline unsigned
ffs_u64(uint64_t x) {
#if LG_SIZEOF_LONG == 3
	return ffs_lu(x);
#elif LG_SIZEOF_LONG_LONG == 3
	return ffs_llu(x);
#else
#error No implementation for 64-bit ffs()
#endif
}

static inline unsigned
fls_u64(uint64_t x) {
#if LG_SIZEOF_LONG == 3
	return fls_lu(x);
#elif LG_SIZEOF_LONG_LONG == 3
	return fls_llu(x);
#else
#error No implementation for 64-bit fls()
#endif
}

static inline unsigned
ffs_u32(uint32_t x) {
#if LG_SIZEOF_INT == 2
	return ffs_u(x);
#else
#error No implementation for 32-bit ffs()
#endif
	return ffs_u(x);
}

static inline unsigned
fls_u32(uint32_t x) {
#if LG_SIZEOF_INT == 2
	return fls_u(x);
#else
#error No implementation for 32-bit fls()
#endif
	return fls_u(x);
}

static inline uint64_t
pow2_ceil_u64(uint64_t x) {
	if (unlikely(x <= 1)) {
		return x;
	}
	size_t msb_on_index = fls_u64(x - 1);
	/*
	 * Range-check; it's on the callers to ensure that the result of this
	 * call won't overflow.
	 */
	assert(msb_on_index < 63);
	return 1ULL << (msb_on_index + 1);
}

static inline uint32_t
pow2_ceil_u32(uint32_t x) {
	if (unlikely(x <= 1)) {
	    return x;
	}
	size_t msb_on_index = fls_u32(x - 1);
	/* As above. */
	assert(msb_on_index < 31);
	return 1U << (msb_on_index + 1);
}

/* Compute the smallest power of 2 that is >= x. */
static inline size_t
pow2_ceil_zu(size_t x) {
#if (LG_SIZEOF_PTR == 3)
	return pow2_ceil_u64(x);
#else
	return pow2_ceil_u32(x);
#endif
}

static inline unsigned
lg_floor(size_t x) {
	util_assume(x != 0);
#if (LG_SIZEOF_PTR == 3)
	return fls_u64(x);
#else
	return fls_u32(x);
#endif
}

static inline unsigned
lg_ceil(size_t x) {
	return lg_floor(x) + ((x & (x - 1)) == 0 ? 0 : 1);
}

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

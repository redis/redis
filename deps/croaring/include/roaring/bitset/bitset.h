/*
 * bitset.h
 *
 * This bitset is a general-purpose dynamic bitmap storing bits in a contiguous
 * array of 64-bit words. The array field points to the word buffer, arraysize
 * records how many words are currently in use, and capacity records how many
 * words are allocated.
 *
 * Unlike the fixed 16-bit-domain container bitset, this structure can grow to
 * cover an arbitrary number of bit positions. It is useful when callers need a
 * resizable bitmap with efficient bitwise operations, scans, and shifts over a
 * larger or runtime-defined domain.
 */
#ifndef CROARING_CBITSET_BITSET_H
#define CROARING_CBITSET_BITSET_H

// For compatibility with MSVC with the use of `restrict`
#ifdef __cplusplus
#define CROARING_CBITSET_RESTRICT
#elif (__STDC_VERSION__ >= 199901L) || \
    (defined(__GNUC__) && defined(__STDC_VERSION__))
#define CROARING_CBITSET_RESTRICT restrict
#else
#define CROARING_CBITSET_RESTRICT
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <roaring/portability.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace api {
#endif

struct bitset_s {
    uint64_t *CROARING_CBITSET_RESTRICT array;
    /* For simplicity and performance, we prefer to have a size and a capacity
     * that is a multiple of 64 bits. Thus we only track the size and the
     * capacity in terms of 64-bit words allocated */
    size_t arraysize;
    size_t capacity;
};

typedef struct bitset_s bitset_t;

/* Create a new bitset. Return NULL in case of failure. */
bitset_t *bitset_create(void);

/* Create a new bitset able to contain size bits. Return NULL in case of
 * failure. */
bitset_t *bitset_create_with_capacity(size_t size);

/* Free memory. */
void bitset_free(bitset_t *bitset);

/* Set all bits to zero. */
void bitset_clear(bitset_t *bitset);

/* Set all bits to one. */
void bitset_fill(bitset_t *bitset);

/* Create a copy */
bitset_t *bitset_copy(const bitset_t *bitset);

/* For advanced users: Resize the bitset so that it can support newarraysize *
 * 64 bits. Return true in case of success, false for failure. Pad with zeroes
 * new buffer areas if requested. */
bool bitset_resize(bitset_t *bitset, size_t newarraysize, bool padwithzeroes);

/* returns how many bytes of memory the backend buffer uses */
inline size_t bitset_size_in_bytes(const bitset_t *bitset) {
    return bitset->arraysize * sizeof(uint64_t);
}

/* returns how many bits can be accessed */
inline size_t bitset_size_in_bits(const bitset_t *bitset) {
    return bitset->arraysize * 64;
}

/* returns how many words (64-bit) of memory the backend buffer uses */
inline size_t bitset_size_in_words(const bitset_t *bitset) {
    return bitset->arraysize;
}

/* For advanced users: Grow the bitset so that it can support newarraysize * 64
 * bits with padding. Return true in case of success, false for failure. */
bool bitset_grow(bitset_t *bitset, size_t newarraysize);

/* attempts to recover unused memory, return false in case of
 * roaring_reallocation failure */
bool bitset_trim(bitset_t *bitset);

/* shifts all bits by 's' positions so that the bitset representing values
 * 1,2,10 would represent values 1+s, 2+s, 10+s */
void bitset_shift_left(bitset_t *bitset, size_t s);

/* shifts all bits by 's' positions so that the bitset representing values
 * 1,2,10 would represent values 1-s, 2-s, 10-s, negative values are deleted */
void bitset_shift_right(bitset_t *bitset, size_t s);

/* Set the ith bit. Attempts to resize the bitset if needed (may silently fail)
 */
inline void bitset_set(bitset_t *bitset, size_t i) {
    size_t shiftedi = i / 64;
    if (shiftedi >= bitset->arraysize) {
        if (!bitset_grow(bitset, shiftedi + 1)) {
            return;
        }
    }
    bitset->array[shiftedi] |= ((uint64_t)1) << (i % 64);
}

/* Set the ith bit to the specified value. Attempts to resize the bitset if
 * needed (may silently fail) */
inline void bitset_set_to_value(bitset_t *bitset, size_t i, bool flag) {
    size_t shiftedi = i / 64;
    uint64_t mask = ((uint64_t)1) << (i % 64);
    uint64_t dynmask = ((uint64_t)flag) << (i % 64);
    if (shiftedi >= bitset->arraysize) {
        if (!bitset_grow(bitset, shiftedi + 1)) {
            return;
        }
    }
    uint64_t w = bitset->array[shiftedi];
    w &= ~mask;
    w |= dynmask;
    bitset->array[shiftedi] = w;
}

/* Get the value of the ith bit.  */
inline bool bitset_get(const bitset_t *bitset, size_t i) {
    size_t shiftedi = i / 64;
    if (shiftedi >= bitset->arraysize) {
        return false;
    }
    return (bitset->array[shiftedi] & (((uint64_t)1) << (i % 64))) != 0;
}

/* Count number of bits set.  */
size_t bitset_count(const bitset_t *bitset);

/* Returns true if no bit is set.  */
bool bitset_empty(const bitset_t *bitset);

/* Find the index of the first bit set. Or SIZE_MAX if the bitset is empty.  */
size_t bitset_minimum(const bitset_t *bitset);

/* Find the index of the last bit set. Or zero if the bitset is empty.  */
size_t bitset_maximum(const bitset_t *bitset);

/* compute the union in-place (to b1), returns true if successful, to generate a
 * new bitset first call bitset_copy */
bool bitset_inplace_union(bitset_t *CROARING_CBITSET_RESTRICT b1,
                          const bitset_t *CROARING_CBITSET_RESTRICT b2);

/* report the size of the union (without materializing it) */
size_t bitset_union_count(const bitset_t *CROARING_CBITSET_RESTRICT b1,
                          const bitset_t *CROARING_CBITSET_RESTRICT b2);

/* compute the intersection in-place (to b1), to generate a new bitset first
 * call bitset_copy */
void bitset_inplace_intersection(bitset_t *CROARING_CBITSET_RESTRICT b1,
                                 const bitset_t *CROARING_CBITSET_RESTRICT b2);

/* report the size of the intersection (without materializing it) */
size_t bitset_intersection_count(const bitset_t *CROARING_CBITSET_RESTRICT b1,
                                 const bitset_t *CROARING_CBITSET_RESTRICT b2);

/* returns true if the bitsets contain no common elements */
bool bitsets_disjoint(const bitset_t *CROARING_CBITSET_RESTRICT b1,
                      const bitset_t *CROARING_CBITSET_RESTRICT b2);

/* returns true if the bitsets contain any common elements */
bool bitsets_intersect(const bitset_t *CROARING_CBITSET_RESTRICT b1,
                       const bitset_t *CROARING_CBITSET_RESTRICT b2);

/* returns true if b1 contains all of the set bits of b2 */
bool bitset_contains_all(const bitset_t *CROARING_CBITSET_RESTRICT b1,
                         const bitset_t *CROARING_CBITSET_RESTRICT b2);

/* compute the difference in-place (to b1), to generate a new bitset first call
 * bitset_copy */
void bitset_inplace_difference(bitset_t *CROARING_CBITSET_RESTRICT b1,
                               const bitset_t *CROARING_CBITSET_RESTRICT b2);

/* compute the size of the difference */
size_t bitset_difference_count(const bitset_t *CROARING_CBITSET_RESTRICT b1,
                               const bitset_t *CROARING_CBITSET_RESTRICT b2);

/* compute the symmetric difference in-place (to b1), return true if successful,
 * to generate a new bitset first call bitset_copy */
bool bitset_inplace_symmetric_difference(
    bitset_t *CROARING_CBITSET_RESTRICT b1,
    const bitset_t *CROARING_CBITSET_RESTRICT b2);

/* compute the size of the symmetric difference  */
size_t bitset_symmetric_difference_count(
    const bitset_t *CROARING_CBITSET_RESTRICT b1,
    const bitset_t *CROARING_CBITSET_RESTRICT b2);

/* iterate over the set bits
 like so :
  for(size_t i = 0; bitset_next_set_bit(b,&i) ; i++) {
    //.....
  }
  */
inline bool bitset_next_set_bit(const bitset_t *bitset, size_t *i) {
    size_t x = *i / 64;
    if (x >= bitset->arraysize) {
        return false;
    }
    uint64_t w = bitset->array[x];
    w >>= (*i & 63);
    if (w != 0) {
        *i += roaring_trailing_zeroes(w);
        return true;
    }
    x++;
    while (x < bitset->arraysize) {
        w = bitset->array[x];
        if (w != 0) {
            *i = x * 64 + roaring_trailing_zeroes(w);
            return true;
        }
        x++;
    }
    return false;
}

/* iterate over the set bits
 like so :
   size_t buffer[256];
   size_t howmany = 0;
  for(size_t startfrom = 0; (howmany = bitset_next_set_bits(b,buffer,256,
 &startfrom)) > 0 ; startfrom++) {
    //.....
  }
  */
inline size_t bitset_next_set_bits(const bitset_t *bitset, size_t *buffer,
                                   size_t capacity, size_t *startfrom) {
    if (capacity == 0) return 0;  // sanity check
    size_t x = *startfrom / 64;
    if (x >= bitset->arraysize) {
        return 0;  // nothing more to iterate over
    }
    uint64_t w = bitset->array[x];
    // unset low bits inside the word less than *startfrom
    w &= ~((UINT64_C(1) << (*startfrom & 63)) - 1);
    size_t howmany = 0;
    size_t base = x << 6;
    while (howmany < capacity) {
        while (w != 0) {
            uint64_t t = w & (~w + 1);
            int r = roaring_trailing_zeroes(w);
            buffer[howmany++] = r + base;
            if (howmany == capacity) goto end;
            w ^= t;
        }
        x += 1;
        if (x == bitset->arraysize) {
            break;
        }
        base += 64;
        w = bitset->array[x];
    }
end:
    if (howmany > 0) {
        *startfrom = buffer[howmany - 1];
    }
    return howmany;
}

typedef bool (*bitset_iterator)(size_t value, void *param);

// return true if uninterrupted
inline bool bitset_for_each(const bitset_t *b, bitset_iterator iterator,
                            void *ptr) {
    size_t base = 0;
    for (size_t i = 0; i < b->arraysize; ++i) {
        uint64_t w = b->array[i];
        while (w != 0) {
            uint64_t t = w & (~w + 1);
            int r = roaring_trailing_zeroes(w);
            if (!iterator(r + base, ptr)) return false;
            w ^= t;
        }
        base += 64;
    }
    return true;
}

inline void bitset_print(const bitset_t *b) {
    printf("{");
    for (size_t i = 0; bitset_next_set_bit(b, &i); i++) {
        printf("%zu, ", i);
    }
    printf("}");
}

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace api {
#endif

#endif

/*
 * bitset.h
 *
 * Bitset containers store a set of 16-bit integers as a fixed-size bitmap.
 * The words pointer references an array of 64-bit words covering the full
 * 16-bit domain, with one bit per possible value. The cardinality field tracks
 * the number of set bits; when it is BITSET_UNKNOWN_CARDINALITY, the count must
 * be recomputed from the bitmap contents.
 *
 * This representation is used for denser containers because membership tests,
 * set operations, and sequential scans can be implemented efficiently with
 * word-level bitwise operations.
 */

#ifndef INCLUDE_CONTAINERS_BITSET_H_
#define INCLUDE_CONTAINERS_BITSET_H_

#include <stdbool.h>
#include <stdint.h>

#include <roaring/roaring_types.h>  // roaring_iterator

// Include other headers after roaring_types.h
#include <roaring/containers/container_defs.h>  // container_t, perfparameters
#include <roaring/portability.h>
#include <roaring/roaring_types.h>  // roaring_iterator
#include <roaring/utilasm.h>        // ASM_XXX macros

#ifdef __cplusplus
extern "C" {
namespace roaring {

// Note: in pure C++ code, you should avoid putting `using` in header files
using api::roaring_iterator;
using api::roaring_iterator64;

namespace internal {
#endif

enum {
    BITSET_CONTAINER_SIZE_IN_WORDS = (1 << 16) / 64,
    BITSET_UNKNOWN_CARDINALITY = -1
};

STRUCT_CONTAINER(bitset_container_s) {
    int32_t cardinality;
    uint64_t *words;
};

typedef struct bitset_container_s bitset_container_t;

#define CAST_bitset(c) CAST(bitset_container_t *, c)  // safer downcast
#define const_CAST_bitset(c) CAST(const bitset_container_t *, c)
#define movable_CAST_bitset(c) movable_CAST(bitset_container_t **, c)

/* Create a new bitset. Return NULL in case of failure. */
bitset_container_t *bitset_container_create(void);

/* Free memory. */
void bitset_container_free(bitset_container_t *bitset);

/* Clear bitset (sets bits to 0). */
void bitset_container_clear(bitset_container_t *bitset);

/* Set all bits to 1. */
void bitset_container_set_all(bitset_container_t *bitset);

/* Duplicate bitset */
bitset_container_t *bitset_container_clone(const bitset_container_t *src);

/* Set the bit in [begin,end). WARNING: as of April 2016, this method is slow
 * and
 * should not be used in performance-sensitive code. Ever.  */
void bitset_container_set_range(bitset_container_t *bitset, uint32_t begin,
                                uint32_t end);

#if defined(CROARING_ASMBITMANIPOPTIMIZATION) && defined(__AVX2__)
/* Set the ith bit.  */
static inline void bitset_container_set(bitset_container_t *bitset,
                                        uint16_t pos) {
    uint64_t shift = 6;
    uint64_t offset;
    uint64_t p = pos;
    ASM_SHIFT_RIGHT(p, shift, offset);
    uint64_t load = bitset->words[offset];
    ASM_SET_BIT_INC_WAS_CLEAR(load, p, bitset->cardinality);
    bitset->words[offset] = load;
}

/* Unset the ith bit. Currently unused. Could be used for optimization. */
/*static inline void bitset_container_unset(bitset_container_t *bitset,
                                          uint16_t pos) {
    uint64_t shift = 6;
    uint64_t offset;
    uint64_t p = pos;
    ASM_SHIFT_RIGHT(p, shift, offset);
    uint64_t load = bitset->words[offset];
    ASM_CLEAR_BIT_DEC_WAS_SET(load, p, bitset->cardinality);
    bitset->words[offset] = load;
}*/

/* Add `pos' to `bitset'. Returns true if `pos' was not present. Might be slower
 * than bitset_container_set.  */
static inline bool bitset_container_add(bitset_container_t *bitset,
                                        uint16_t pos) {
    uint64_t shift = 6;
    uint64_t offset;
    uint64_t p = pos;
    ASM_SHIFT_RIGHT(p, shift, offset);
    uint64_t load = bitset->words[offset];
    // could be possibly slightly further optimized
    const int32_t oldcard = bitset->cardinality;
    ASM_SET_BIT_INC_WAS_CLEAR(load, p, bitset->cardinality);
    bitset->words[offset] = load;
    return bitset->cardinality - oldcard;
}

/* Remove `pos' from `bitset'. Returns true if `pos' was present.  Might be
 * slower than bitset_container_unset.  */
static inline bool bitset_container_remove(bitset_container_t *bitset,
                                           uint16_t pos) {
    uint64_t shift = 6;
    uint64_t offset;
    uint64_t p = pos;
    ASM_SHIFT_RIGHT(p, shift, offset);
    uint64_t load = bitset->words[offset];
    // could be possibly slightly further optimized
    const int32_t oldcard = bitset->cardinality;
    ASM_CLEAR_BIT_DEC_WAS_SET(load, p, bitset->cardinality);
    bitset->words[offset] = load;
    return oldcard - bitset->cardinality;
}

/* Get the value of the ith bit.  */
inline bool bitset_container_get(const bitset_container_t *bitset,
                                 uint16_t pos) {
    uint64_t word = bitset->words[pos >> 6];
    const uint64_t p = pos;
    ASM_INPLACESHIFT_RIGHT(word, p);
    return word & 1;
}

#else

/* Set the ith bit.  */
static inline void bitset_container_set(bitset_container_t *bitset,
                                        uint16_t pos) {
    const uint64_t old_word = bitset->words[pos >> 6];
    const int index = pos & 63;
    const uint64_t new_word = old_word | (UINT64_C(1) << index);
    bitset->cardinality += (uint32_t)((old_word ^ new_word) >> index);
    bitset->words[pos >> 6] = new_word;
}

/* Unset the ith bit. Currently unused.  */
/*static inline void bitset_container_unset(bitset_container_t *bitset,
                                          uint16_t pos) {
    const uint64_t old_word = bitset->words[pos >> 6];
    const int index = pos & 63;
    const uint64_t new_word = old_word & (~(UINT64_C(1) << index));
    bitset->cardinality -= (uint32_t)((old_word ^ new_word) >> index);
    bitset->words[pos >> 6] = new_word;
}*/

/* Add `pos' to `bitset'. Returns true if `pos' was not present. Might be slower
 * than bitset_container_set.  */
static inline bool bitset_container_add(bitset_container_t *bitset,
                                        uint16_t pos) {
    const uint64_t old_word = bitset->words[pos >> 6];
    const int index = pos & 63;
    const uint64_t new_word = old_word | (UINT64_C(1) << index);
    const uint64_t increment = (old_word ^ new_word) >> index;
    bitset->cardinality += (uint32_t)increment;
    bitset->words[pos >> 6] = new_word;
    return increment > 0;
}

/* Remove `pos' from `bitset'. Returns true if `pos' was present.  Might be
 * slower than bitset_container_unset.  */
static inline bool bitset_container_remove(bitset_container_t *bitset,
                                           uint16_t pos) {
    const uint64_t old_word = bitset->words[pos >> 6];
    const int index = pos & 63;
    const uint64_t new_word = old_word & (~(UINT64_C(1) << index));
    const uint64_t increment = (old_word ^ new_word) >> index;
    bitset->cardinality -= (uint32_t)increment;
    bitset->words[pos >> 6] = new_word;
    return increment > 0;
}

/* Get the value of the ith bit.  */
inline bool bitset_container_get(const bitset_container_t *bitset,
                                 uint16_t pos) {
    const uint64_t word = bitset->words[pos >> 6];
    return (word >> (pos & 63)) & 1;
}

#endif

/*
 * Check if all bits are set in a range of positions from pos_start (included)
 * to pos_end (excluded).
 */
static inline bool bitset_container_get_range(const bitset_container_t *bitset,
                                              uint32_t pos_start,
                                              uint32_t pos_end) {
    const uint32_t start = pos_start >> 6;
    const uint32_t end = pos_end >> 6;

    const uint64_t first = ~((1ULL << (pos_start & 0x3F)) - 1);
    const uint64_t last = (1ULL << (pos_end & 0x3F)) - 1;

    if (start == end)
        return ((bitset->words[end] & first & last) == (first & last));
    if ((bitset->words[start] & first) != first) return false;

    if ((end < BITSET_CONTAINER_SIZE_IN_WORDS) &&
        ((bitset->words[end] & last) != last)) {
        return false;
    }

    for (uint32_t i = start + 1;
         (i < BITSET_CONTAINER_SIZE_IN_WORDS) && (i < end); ++i) {
        if (bitset->words[i] != UINT64_C(0xFFFFFFFFFFFFFFFF)) return false;
    }

    return true;
}

/* Check whether `bitset' is present in `array'.  Calls bitset_container_get. */
inline bool bitset_container_contains(const bitset_container_t *bitset,
                                      uint16_t pos) {
    return bitset_container_get(bitset, pos);
}

/*
 * Check whether a range of bits from position `pos_start' (included) to
 * `pos_end' (excluded) is present in `bitset'.  Calls bitset_container_get_all.
 */
static inline bool bitset_container_contains_range(
    const bitset_container_t *bitset, uint32_t pos_start, uint32_t pos_end) {
    return bitset_container_get_range(bitset, pos_start, pos_end);
}

/* Get the number of bits set */
CROARING_ALLOW_UNALIGNED
static inline int bitset_container_cardinality(
    const bitset_container_t *bitset) {
    return bitset->cardinality;
}

/* Copy one container into another. We assume that they are distinct. */
void bitset_container_copy(const bitset_container_t *source,
                           bitset_container_t *dest);

/*  Add all the values [min,max) at a distance k*step from min: min,
 * min+step,.... */
void bitset_container_add_from_range(bitset_container_t *bitset, uint32_t min,
                                     uint32_t max, uint16_t step);

/* Get the number of bits set (force computation). This does not modify bitset.
 * To update the cardinality, you should do
 * bitset->cardinality =  bitset_container_compute_cardinality(bitset).*/
int bitset_container_compute_cardinality(const bitset_container_t *bitset);

/* Check whether this bitset is empty,
 *  it never modifies the bitset struct. */
static inline bool bitset_container_empty(const bitset_container_t *bitset) {
    if (bitset->cardinality == BITSET_UNKNOWN_CARDINALITY) {
        for (int i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i++) {
            if ((bitset->words[i]) != 0) return false;
        }
        return true;
    }
    return bitset->cardinality == 0;
}

/* Get whether there is at least one bit set  (see bitset_container_empty for
   the reverse), the bitset is never modified */
static inline bool bitset_container_const_nonzero_cardinality(
    const bitset_container_t *bitset) {
    return !bitset_container_empty(bitset);
}

/*
 * Check whether the two bitsets intersect
 */
bool bitset_container_intersect(const bitset_container_t *src_1,
                                const bitset_container_t *src_2);

/* Computes the union of bitsets `src_1' and `src_2' into `dst'  and return the
 * cardinality. */
int bitset_container_or(const bitset_container_t *src_1,
                        const bitset_container_t *src_2,
                        bitset_container_t *dst);

/* Computes the union of bitsets `src_1' and `src_2' and return the cardinality.
 */
int bitset_container_or_justcard(const bitset_container_t *src_1,
                                 const bitset_container_t *src_2);

/* Computes the union of bitsets `src_1' and `src_2' into `dst' and return the
 * cardinality. Same as bitset_container_or. */
int bitset_container_union(const bitset_container_t *src_1,
                           const bitset_container_t *src_2,
                           bitset_container_t *dst);

/* Computes the union of bitsets `src_1' and `src_2'  and return the
 * cardinality. Same as bitset_container_or_justcard. */
int bitset_container_union_justcard(const bitset_container_t *src_1,
                                    const bitset_container_t *src_2);

/* Computes the union of bitsets `src_1' and `src_2' into `dst', but does
 * not update the cardinality. Provided to optimize chained operations. */
int bitset_container_union_nocard(const bitset_container_t *src_1,
                                  const bitset_container_t *src_2,
                                  bitset_container_t *dst);

/* Computes the union of bitsets `src_1' and `src_2' into `dst', but does not
 * update the cardinality. Provided to optimize chained operations. */
int bitset_container_or_nocard(const bitset_container_t *src_1,
                               const bitset_container_t *src_2,
                               bitset_container_t *dst);

/* Computes the intersection of bitsets `src_1' and `src_2' into `dst' and
 * return the cardinality. */
int bitset_container_and(const bitset_container_t *src_1,
                         const bitset_container_t *src_2,
                         bitset_container_t *dst);

/* Computes the intersection of bitsets `src_1' and `src_2'  and return the
 * cardinality. */
int bitset_container_and_justcard(const bitset_container_t *src_1,
                                  const bitset_container_t *src_2);

/* Computes the intersection of bitsets `src_1' and `src_2' into `dst' and
 * return the cardinality. Same as bitset_container_and. */
int bitset_container_intersection(const bitset_container_t *src_1,
                                  const bitset_container_t *src_2,
                                  bitset_container_t *dst);

/* Computes the intersection of bitsets `src_1' and `src_2' and return the
 * cardinality. Same as bitset_container_and_justcard. */
int bitset_container_intersection_justcard(const bitset_container_t *src_1,
                                           const bitset_container_t *src_2);

/* Computes the intersection of bitsets `src_1' and `src_2' into `dst', but does
 * not update the cardinality. Provided to optimize chained operations. */
int bitset_container_intersection_nocard(const bitset_container_t *src_1,
                                         const bitset_container_t *src_2,
                                         bitset_container_t *dst);

/* Computes the intersection of bitsets `src_1' and `src_2' into `dst', but does
 * not update the cardinality. Provided to optimize chained operations. */
int bitset_container_and_nocard(const bitset_container_t *src_1,
                                const bitset_container_t *src_2,
                                bitset_container_t *dst);

/* Computes the exclusive or of bitsets `src_1' and `src_2' into `dst' and
 * return the cardinality. */
int bitset_container_xor(const bitset_container_t *src_1,
                         const bitset_container_t *src_2,
                         bitset_container_t *dst);

/* Computes the exclusive or of bitsets `src_1' and `src_2' and return the
 * cardinality. */
int bitset_container_xor_justcard(const bitset_container_t *src_1,
                                  const bitset_container_t *src_2);

/* Computes the exclusive or of bitsets `src_1' and `src_2' into `dst', but does
 * not update the cardinality. Provided to optimize chained operations. */
int bitset_container_xor_nocard(const bitset_container_t *src_1,
                                const bitset_container_t *src_2,
                                bitset_container_t *dst);

/* Computes the and not of bitsets `src_1' and `src_2' into `dst' and return the
 * cardinality. */
int bitset_container_andnot(const bitset_container_t *src_1,
                            const bitset_container_t *src_2,
                            bitset_container_t *dst);

/* Computes the and not of bitsets `src_1' and `src_2'  and return the
 * cardinality. */
int bitset_container_andnot_justcard(const bitset_container_t *src_1,
                                     const bitset_container_t *src_2);

/* Computes the and not or of bitsets `src_1' and `src_2' into `dst', but does
 * not update the cardinality. Provided to optimize chained operations. */
int bitset_container_andnot_nocard(const bitset_container_t *src_1,
                                   const bitset_container_t *src_2,
                                   bitset_container_t *dst);

void bitset_container_offset(const bitset_container_t *c, container_t **loc,
                             container_t **hic, uint16_t offset);
/*
 * Write out the 16-bit integers contained in this container as a list of 32-bit
 * integers using base
 * as the starting value (it might be expected that base has zeros in its 16
 * least significant bits).
 * The function returns the number of values written.
 * The caller is responsible for allocating enough memory in out.
 * The out pointer should point to enough memory (the cardinality times 32
 * bits).
 */
int bitset_container_to_uint32_array(uint32_t *out,
                                     const bitset_container_t *bc,
                                     uint32_t base);

/*
 * Print this container using printf (useful for debugging).
 */
void bitset_container_printf(const bitset_container_t *v);

/*
 * Print this container using printf as a comma-separated list of 32-bit
 * integers starting at base.
 */
void bitset_container_printf_as_uint32_array(const bitset_container_t *v,
                                             uint32_t base);

bool bitset_container_validate(const bitset_container_t *v,
                               const char **reason);

/**
 * Return the serialized size in bytes of a container.
 */
static inline int32_t bitset_container_serialized_size_in_bytes(void) {
    return BITSET_CONTAINER_SIZE_IN_WORDS * 8;
}

/**
 * Return the the number of runs.
 */
int bitset_container_number_of_runs(bitset_container_t *bc);

bool bitset_container_iterate(const bitset_container_t *cont, uint32_t base,
                              roaring_iterator iterator, void *ptr);
bool bitset_container_iterate64(const bitset_container_t *cont, uint32_t base,
                                roaring_iterator64 iterator, uint64_t high_bits,
                                void *ptr);

/**
 * Writes the underlying array to buf, outputs how many bytes were written.
 * This is meant to be byte-by-byte compatible with the Java and Go versions of
 * Roaring.
 * The number of bytes written should be
 * bitset_container_size_in_bytes(container).
 */
int32_t bitset_container_write(const bitset_container_t *container, char *buf);

/**
 * Reads the instance from buf, outputs how many bytes were read.
 * This is meant to be byte-by-byte compatible with the Java and Go versions of
 * Roaring.
 * The number of bytes read should be bitset_container_size_in_bytes(container).
 * You need to provide the (known) cardinality.
 */
int32_t bitset_container_read(int32_t cardinality,
                              bitset_container_t *container, const char *buf);
/**
 * Return the serialized size in bytes of a container (see
 * bitset_container_write).
 * This is meant to be compatible with the Java and Go versions of Roaring and
 * assumes
 * that the cardinality of the container is already known or can be computed.
 */
static inline int32_t bitset_container_size_in_bytes(
    const bitset_container_t *container) {
    (void)container;
    return BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t);
}

/**
 * Return true if the two containers have the same content.
 */
bool bitset_container_equals(const bitset_container_t *container1,
                             const bitset_container_t *container2);

/**
 * Return true if container1 is a subset of container2.
 */
bool bitset_container_is_subset(const bitset_container_t *container1,
                                const bitset_container_t *container2);

/**
 * If the element of given rank is in this container, supposing that the first
 * element has rank start_rank, then the function returns true and sets element
 * accordingly.
 * Otherwise, it returns false and update start_rank.
 */
bool bitset_container_select(const bitset_container_t *container,
                             uint32_t *start_rank, uint32_t rank,
                             uint32_t *element);

/* Returns the smallest value (assumes not empty) */
uint16_t bitset_container_minimum(const bitset_container_t *container);

/* Returns the largest value (assumes not empty) */
uint16_t bitset_container_maximum(const bitset_container_t *container);

/* Returns the number of values equal or smaller than x */
int bitset_container_rank(const bitset_container_t *container, uint16_t x);

/* bulk version of bitset_container_rank(); return number of consumed elements
 */
uint32_t bitset_container_rank_many(const bitset_container_t *container,
                                    uint64_t start_rank, const uint32_t *begin,
                                    const uint32_t *end, uint64_t *ans);

/* Returns the index of x , if not exsist return -1 */
int bitset_container_get_index(const bitset_container_t *container, uint16_t x);

/* Returns the index of the first value equal or larger than x, or -1 */
int bitset_container_index_equalorlarger(const bitset_container_t *container,
                                         uint16_t x);

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

#endif /* INCLUDE_CONTAINERS_BITSET_H_ */

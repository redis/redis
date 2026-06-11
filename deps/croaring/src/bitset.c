#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <roaring/bitset/bitset.h>
#include <roaring/memory.h>
#include <roaring/portability.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

extern inline void bitset_print(const bitset_t *b);
extern inline bool bitset_for_each(const bitset_t *b, bitset_iterator iterator,
                                   void *ptr);
extern inline size_t bitset_next_set_bits(const bitset_t *bitset,
                                          size_t *buffer, size_t capacity,
                                          size_t *startfrom);
extern inline void bitset_set_to_value(bitset_t *bitset, size_t i, bool flag);
extern inline bool bitset_next_set_bit(const bitset_t *bitset, size_t *i);
extern inline void bitset_set(bitset_t *bitset, size_t i);
extern inline bool bitset_get(const bitset_t *bitset, size_t i);
extern inline size_t bitset_size_in_words(const bitset_t *bitset);
extern inline size_t bitset_size_in_bits(const bitset_t *bitset);
extern inline size_t bitset_size_in_bytes(const bitset_t *bitset);

/* Create a new bitset. Return NULL in case of failure. */
bitset_t *bitset_create(void) {
    bitset_t *bitset = NULL;
    /* Allocate the bitset itself. */
    if ((bitset = (bitset_t *)roaring_malloc(sizeof(bitset_t))) == NULL) {
        return NULL;
    }
    bitset->array = NULL;
    bitset->arraysize = 0;
    bitset->capacity = 0;
    return bitset;
}

/* Create a new bitset able to contain size bits. Return NULL in case of
 * failure. */
bitset_t *bitset_create_with_capacity(size_t size) {
    bitset_t *bitset = NULL;
    /* Allocate the bitset itself. */
    if ((bitset = (bitset_t *)roaring_malloc(sizeof(bitset_t))) == NULL) {
        return NULL;
    }
    bitset->arraysize =
        (size + sizeof(uint64_t) * 8 - 1) / (sizeof(uint64_t) * 8);
    bitset->capacity = bitset->arraysize;
    if ((bitset->array = (uint64_t *)roaring_calloc(
             bitset->arraysize, sizeof(uint64_t))) == NULL) {
        roaring_free(bitset);
        return NULL;
    }
    return bitset;
}

/* Create a copy */
bitset_t *bitset_copy(const bitset_t *bitset) {
    bitset_t *copy = NULL;
    /* Allocate the bitset itself. */
    if ((copy = (bitset_t *)roaring_malloc(sizeof(bitset_t))) == NULL) {
        return NULL;
    }
    memcpy(copy, bitset, sizeof(bitset_t));
    copy->capacity = copy->arraysize;
    if ((copy->array = (uint64_t *)roaring_malloc(sizeof(uint64_t) *
                                                  bitset->arraysize)) == NULL) {
        roaring_free(copy);
        return NULL;
    }
    memcpy(copy->array, bitset->array, sizeof(uint64_t) * bitset->arraysize);
    return copy;
}

void bitset_clear(bitset_t *bitset) {
    memset(bitset->array, 0, sizeof(uint64_t) * bitset->arraysize);
}

void bitset_fill(bitset_t *bitset) {
    memset(bitset->array, 0xff, sizeof(uint64_t) * bitset->arraysize);
}

void bitset_shift_left(bitset_t *bitset, size_t s) {
    size_t extra_words = s / 64;
    int inword_shift = s % 64;
    size_t as = bitset->arraysize;
    if (inword_shift == 0) {
        bitset_resize(bitset, as + extra_words, false);
        // could be done with a memmove
        for (size_t i = as + extra_words; i > extra_words; i--) {
            bitset->array[i - 1] = bitset->array[i - 1 - extra_words];
        }
    } else {
        bitset_resize(bitset, as + extra_words + 1, true);
        bitset->array[as + extra_words] =
            bitset->array[as - 1] >> (64 - inword_shift);
        for (size_t i = as + extra_words; i >= extra_words + 2; i--) {
            bitset->array[i - 1] =
                (bitset->array[i - 1 - extra_words] << inword_shift) |
                (bitset->array[i - 2 - extra_words] >> (64 - inword_shift));
        }
        bitset->array[extra_words] = bitset->array[0] << inword_shift;
    }
    for (size_t i = 0; i < extra_words; i++) {
        bitset->array[i] = 0;
    }
}

void bitset_shift_right(bitset_t *bitset, size_t s) {
    size_t extra_words = s / 64;
    int inword_shift = s % 64;
    size_t as = bitset->arraysize;
    if (inword_shift == 0) {
        // could be done with a memmove
        for (size_t i = 0; i < as - extra_words; i++) {
            bitset->array[i] = bitset->array[i + extra_words];
        }
        bitset_resize(bitset, as - extra_words, false);

    } else {
        for (size_t i = 0; i + extra_words + 1 < as; i++) {
            bitset->array[i] =
                (bitset->array[i + extra_words] >> inword_shift) |
                (bitset->array[i + extra_words + 1] << (64 - inword_shift));
        }
        bitset->array[as - extra_words - 1] =
            (bitset->array[as - 1] >> inword_shift);
        bitset_resize(bitset, as - extra_words, false);
    }
}

/* Free memory. */
void bitset_free(bitset_t *bitset) {
    if (bitset == NULL) {
        return;
    }
    roaring_free(bitset->array);
    roaring_free(bitset);
}

/* Resize the bitset so that it can support newarraysize * 64 bits. Return true
 * in case of success, false for failure. */
bool bitset_resize(bitset_t *bitset, size_t newarraysize, bool padwithzeroes) {
    if (newarraysize > SIZE_MAX / 64) {
        return false;
    }
    size_t smallest =
        newarraysize < bitset->arraysize ? newarraysize : bitset->arraysize;
    if (bitset->capacity < newarraysize) {
        uint64_t *newarray;
        size_t newcapacity = bitset->capacity;
        if (newcapacity == 0) {
            newcapacity = 1;
        }
        while (newcapacity < newarraysize) {
            newcapacity *= 2;
        }
        if ((newarray = (uint64_t *)roaring_realloc(
                 bitset->array, sizeof(uint64_t) * newcapacity)) == NULL) {
            return false;
        }
        bitset->capacity = newcapacity;
        bitset->array = newarray;
    }
    if (padwithzeroes && (newarraysize > smallest))
        memset(bitset->array + smallest, 0,
               sizeof(uint64_t) * (newarraysize - smallest));
    bitset->arraysize = newarraysize;
    return true;  // success!
}

size_t bitset_count(const bitset_t *bitset) {
    size_t card = 0;
    size_t k = 0;
    for (; k + 7 < bitset->arraysize; k += 8) {
        card += roaring_hamming(bitset->array[k]);
        card += roaring_hamming(bitset->array[k + 1]);
        card += roaring_hamming(bitset->array[k + 2]);
        card += roaring_hamming(bitset->array[k + 3]);
        card += roaring_hamming(bitset->array[k + 4]);
        card += roaring_hamming(bitset->array[k + 5]);
        card += roaring_hamming(bitset->array[k + 6]);
        card += roaring_hamming(bitset->array[k + 7]);
    }
    for (; k + 3 < bitset->arraysize; k += 4) {
        card += roaring_hamming(bitset->array[k]);
        card += roaring_hamming(bitset->array[k + 1]);
        card += roaring_hamming(bitset->array[k + 2]);
        card += roaring_hamming(bitset->array[k + 3]);
    }
    for (; k < bitset->arraysize; k++) {
        card += roaring_hamming(bitset->array[k]);
    }
    return card;
}

bool bitset_inplace_union(bitset_t *CROARING_CBITSET_RESTRICT b1,
                          const bitset_t *CROARING_CBITSET_RESTRICT b2) {
    size_t minlength =
        b1->arraysize < b2->arraysize ? b1->arraysize : b2->arraysize;
    for (size_t k = 0; k < minlength; ++k) {
        b1->array[k] |= b2->array[k];
    }
    if (b2->arraysize > b1->arraysize) {
        size_t oldsize = b1->arraysize;
        if (!bitset_resize(b1, b2->arraysize, false)) return false;
        memcpy(b1->array + oldsize, b2->array + oldsize,
               (b2->arraysize - oldsize) * sizeof(uint64_t));
    }
    return true;
}

bool bitset_empty(const bitset_t *bitset) {
    for (size_t k = 0; k < bitset->arraysize; k++) {
        if (bitset->array[k] != 0) {
            return false;
        }
    }
    return true;
}

size_t bitset_minimum(const bitset_t *bitset) {
    for (size_t k = 0; k < bitset->arraysize; k++) {
        uint64_t w = bitset->array[k];
        if (w != 0) {
            return roaring_trailing_zeroes(w) + k * 64;
        }
    }
    return SIZE_MAX;
}

bool bitset_grow(bitset_t *bitset, size_t newarraysize) {
    if (newarraysize < bitset->arraysize) {
        return false;
    }
    if (newarraysize > SIZE_MAX / 64) {
        return false;
    }
    if (bitset->capacity < newarraysize) {
        uint64_t *newarray;
        size_t newcapacity = (UINT64_C(0xFFFFFFFFFFFFFFFF) >>
                              roaring_leading_zeroes(newarraysize)) +
                             1;
        while (newcapacity < newarraysize) {
            newcapacity *= 2;
        }
        if ((newarray = (uint64_t *)roaring_realloc(
                 bitset->array, sizeof(uint64_t) * newcapacity)) == NULL) {
            return false;
        }
        bitset->capacity = newcapacity;
        bitset->array = newarray;
    }
    memset(bitset->array + bitset->arraysize, 0,
           sizeof(uint64_t) * (newarraysize - bitset->arraysize));
    bitset->arraysize = newarraysize;
    return true;  // success!
}

size_t bitset_maximum(const bitset_t *bitset) {
    for (size_t k = bitset->arraysize; k > 0; k--) {
        uint64_t w = bitset->array[k - 1];
        if (w != 0) {
            return 63 - roaring_leading_zeroes(w) + (k - 1) * 64;
        }
    }
    return 0;
}

/* Returns true if bitsets share no common elements, false otherwise.
 *
 * Performs early-out if common element found. */
bool bitsets_disjoint(const bitset_t *CROARING_CBITSET_RESTRICT b1,
                      const bitset_t *CROARING_CBITSET_RESTRICT b2) {
    size_t minlength =
        b1->arraysize < b2->arraysize ? b1->arraysize : b2->arraysize;

    for (size_t k = 0; k < minlength; k++) {
        if ((b1->array[k] & b2->array[k]) != 0) return false;
    }
    return true;
}

/* Returns true if bitsets contain at least 1 common element, false if they are
 * disjoint.
 *
 * Performs early-out if common element found. */
bool bitsets_intersect(const bitset_t *CROARING_CBITSET_RESTRICT b1,
                       const bitset_t *CROARING_CBITSET_RESTRICT b2) {
    size_t minlength =
        b1->arraysize < b2->arraysize ? b1->arraysize : b2->arraysize;

    for (size_t k = 0; k < minlength; k++) {
        if ((b1->array[k] & b2->array[k]) != 0) return true;
    }
    return false;
}

/* Returns true if b has any bits set in or after b->array[starting_loc]. */
static bool any_bits_set(const bitset_t *b, size_t starting_loc) {
    if (starting_loc >= b->arraysize) {
        return false;
    }
    for (size_t k = starting_loc; k < b->arraysize; k++) {
        if (b->array[k] != 0) return true;
    }
    return false;
}

/* Returns true if b1 has all of b2's bits set.
 *
 * Performs early out if a bit is found in b2 that is not found in b1. */
bool bitset_contains_all(const bitset_t *CROARING_CBITSET_RESTRICT b1,
                         const bitset_t *CROARING_CBITSET_RESTRICT b2) {
    size_t min_size = b1->arraysize;
    if (b1->arraysize > b2->arraysize) {
        min_size = b2->arraysize;
    }
    for (size_t k = 0; k < min_size; k++) {
        if ((b1->array[k] & b2->array[k]) != b2->array[k]) {
            return false;
        }
    }
    if (b2->arraysize > b1->arraysize) {
        /* Need to check if b2 has any bits set beyond b1's array */
        return !any_bits_set(b2, b1->arraysize);
    }
    return true;
}

size_t bitset_union_count(const bitset_t *CROARING_CBITSET_RESTRICT b1,
                          const bitset_t *CROARING_CBITSET_RESTRICT b2) {
    size_t answer = 0;
    size_t minlength =
        b1->arraysize < b2->arraysize ? b1->arraysize : b2->arraysize;
    size_t k = 0;
    for (; k + 3 < minlength; k += 4) {
        answer += roaring_hamming(b1->array[k] | b2->array[k]);
        answer += roaring_hamming(b1->array[k + 1] | b2->array[k + 1]);
        answer += roaring_hamming(b1->array[k + 2] | b2->array[k + 2]);
        answer += roaring_hamming(b1->array[k + 3] | b2->array[k + 3]);
    }
    for (; k < minlength; ++k) {
        answer += roaring_hamming(b1->array[k] | b2->array[k]);
    }
    if (b2->arraysize > b1->arraysize) {
        // k is equal to b1->arraysize
        for (; k + 3 < b2->arraysize; k += 4) {
            answer += roaring_hamming(b2->array[k]);
            answer += roaring_hamming(b2->array[k + 1]);
            answer += roaring_hamming(b2->array[k + 2]);
            answer += roaring_hamming(b2->array[k + 3]);
        }
        for (; k < b2->arraysize; ++k) {
            answer += roaring_hamming(b2->array[k]);
        }
    } else {
        // k is equal to b2->arraysize
        for (; k + 3 < b1->arraysize; k += 4) {
            answer += roaring_hamming(b1->array[k]);
            answer += roaring_hamming(b1->array[k + 1]);
            answer += roaring_hamming(b1->array[k + 2]);
            answer += roaring_hamming(b1->array[k + 3]);
        }
        for (; k < b1->arraysize; ++k) {
            answer += roaring_hamming(b1->array[k]);
        }
    }
    return answer;
}

void bitset_inplace_intersection(bitset_t *CROARING_CBITSET_RESTRICT b1,
                                 const bitset_t *CROARING_CBITSET_RESTRICT b2) {
    size_t minlength =
        b1->arraysize < b2->arraysize ? b1->arraysize : b2->arraysize;
    size_t k = 0;
    for (; k < minlength; ++k) {
        b1->array[k] &= b2->array[k];
    }
    for (; k < b1->arraysize; ++k) {
        b1->array[k] = 0;  // memset could, maybe, be a tiny bit faster
    }
}

size_t bitset_intersection_count(const bitset_t *CROARING_CBITSET_RESTRICT b1,
                                 const bitset_t *CROARING_CBITSET_RESTRICT b2) {
    size_t answer = 0;
    size_t minlength =
        b1->arraysize < b2->arraysize ? b1->arraysize : b2->arraysize;
    for (size_t k = 0; k < minlength; ++k) {
        answer += roaring_hamming(b1->array[k] & b2->array[k]);
    }
    return answer;
}

void bitset_inplace_difference(bitset_t *CROARING_CBITSET_RESTRICT b1,
                               const bitset_t *CROARING_CBITSET_RESTRICT b2) {
    size_t minlength =
        b1->arraysize < b2->arraysize ? b1->arraysize : b2->arraysize;
    size_t k = 0;
    for (; k < minlength; ++k) {
        b1->array[k] &= ~(b2->array[k]);
    }
}

size_t bitset_difference_count(const bitset_t *CROARING_CBITSET_RESTRICT b1,
                               const bitset_t *CROARING_CBITSET_RESTRICT b2) {
    size_t minlength =
        b1->arraysize < b2->arraysize ? b1->arraysize : b2->arraysize;
    size_t k = 0;
    size_t answer = 0;
    for (; k < minlength; ++k) {
        answer += roaring_hamming(b1->array[k] & ~(b2->array[k]));
    }
    for (; k < b1->arraysize; ++k) {
        answer += roaring_hamming(b1->array[k]);
    }
    return answer;
}

bool bitset_inplace_symmetric_difference(
    bitset_t *CROARING_CBITSET_RESTRICT b1,
    const bitset_t *CROARING_CBITSET_RESTRICT b2) {
    size_t minlength =
        b1->arraysize < b2->arraysize ? b1->arraysize : b2->arraysize;
    size_t k = 0;
    for (; k < minlength; ++k) {
        b1->array[k] ^= b2->array[k];
    }
    if (b2->arraysize > b1->arraysize) {
        size_t oldsize = b1->arraysize;
        if (!bitset_resize(b1, b2->arraysize, false)) return false;
        memcpy(b1->array + oldsize, b2->array + oldsize,
               (b2->arraysize - oldsize) * sizeof(uint64_t));
    }
    return true;
}

size_t bitset_symmetric_difference_count(
    const bitset_t *CROARING_CBITSET_RESTRICT b1,
    const bitset_t *CROARING_CBITSET_RESTRICT b2) {
    size_t minlength =
        b1->arraysize < b2->arraysize ? b1->arraysize : b2->arraysize;
    size_t k = 0;
    size_t answer = 0;
    for (; k < minlength; ++k) {
        answer += roaring_hamming(b1->array[k] ^ b2->array[k]);
    }
    if (b2->arraysize > b1->arraysize) {
        for (; k < b2->arraysize; ++k) {
            answer += roaring_hamming(b2->array[k]);
        }
    } else {
        for (; k < b1->arraysize; ++k) {
            answer += roaring_hamming(b1->array[k]);
        }
    }
    return answer;
}

bool bitset_trim(bitset_t *bitset) {
    size_t newsize = bitset->arraysize;
    while (newsize > 0) {
        if (bitset->array[newsize - 1] == 0)
            newsize -= 1;
        else
            break;
    }
    if (bitset->capacity == newsize) return true;  // nothing to do
    uint64_t *newarray;
    if ((newarray = (uint64_t *)roaring_realloc(
             bitset->array, sizeof(uint64_t) * newsize)) == NULL) {
        return false;
    }
    bitset->array = newarray;
    bitset->capacity = newsize;
    bitset->arraysize = newsize;
    return true;
}

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

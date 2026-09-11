/*
 * array.h
 *
 * Array containers store a sparse set of 16-bit integers as a sorted dynamic
 * array. The cardinality field tracks how many values are present, capacity
 * tracks the allocated length, and array points to the sorted values.
 *
 * This representation is used for low-cardinality containers because it is
 * compact and supports fast iteration and binary-search-based membership
 * tests. When the number of stored values grows beyond DEFAULT_MAX_SIZE,
 * CRoaring will typically switch to a denser container representation.
 */

#ifndef INCLUDE_CONTAINERS_ARRAY_H_
#define INCLUDE_CONTAINERS_ARRAY_H_

#include <string.h>

#include <roaring/roaring_types.h>  // roaring_iterator

// Include other headers after roaring_types.h
#include <roaring/array_util.h>  // binarySearch()/memequals() for inlining
#include <roaring/containers/container_defs.h>  // container_t, perfparameters
#include <roaring/portability.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {

// Note: in pure C++ code, you should avoid putting `using` in header files
using api::roaring_iterator;
using api::roaring_iterator64;

namespace internal {
#endif

/* Containers with DEFAULT_MAX_SIZE or less integers should be arrays */
enum { DEFAULT_MAX_SIZE = 4096 };

/* struct array_container - sparse representation of a bitmap
 *
 * @cardinality: number of indices in `array` (and the bitmap)
 * @capacity:    allocated size of `array`
 * @array:       sorted list of integers
 */
STRUCT_CONTAINER(array_container_s) {
    int32_t cardinality;
    int32_t capacity;
    uint16_t *array;
};

typedef struct array_container_s array_container_t;

#define CAST_array(c) CAST(array_container_t *, c)  // safer downcast
#define const_CAST_array(c) CAST(const array_container_t *, c)
#define movable_CAST_array(c) movable_CAST(array_container_t **, c)

/* Create a new array with default. Return NULL in case of failure. See also
 * array_container_create_given_capacity. */
array_container_t *array_container_create(void);

/* Create a new array with a specified capacity size. Return NULL in case of
 * failure. */
array_container_t *array_container_create_given_capacity(int32_t size);

/* Create a new array containing all values in [min,max). */
array_container_t *array_container_create_range(uint32_t min, uint32_t max);

/*
 * Shrink the capacity to the actual size, return the number of bytes saved.
 */
int array_container_shrink_to_fit(array_container_t *src);

/* Free memory owned by `array'. */
void array_container_free(array_container_t *array);

/* Duplicate container */
array_container_t *array_container_clone(const array_container_t *src);

/* Get the cardinality of `array'. */
CROARING_ALLOW_UNALIGNED
static inline int array_container_cardinality(const array_container_t *array) {
    return array->cardinality;
}

static inline bool array_container_nonzero_cardinality(
    const array_container_t *array) {
    return array->cardinality > 0;
}

/* Copy one container into another. We assume that they are distinct. */
void array_container_copy(const array_container_t *src, array_container_t *dst);

/*  Add all the values in [min,max) (included) at a distance k*step from min.
    The container must have a size less or equal to DEFAULT_MAX_SIZE after this
   addition. */
void array_container_add_from_range(array_container_t *arr, uint32_t min,
                                    uint32_t max, uint16_t step);

static inline bool array_container_empty(const array_container_t *array) {
    return array->cardinality == 0;
}

/* check whether the cardinality is equal to the capacity (this does not mean
 * that it contains 1<<16 elements) */
static inline bool array_container_full(const array_container_t *array) {
    return array->cardinality == array->capacity;
}

/* Compute the union of `src_1' and `src_2' and write the result to `dst'
 * It is assumed that `dst' is distinct from both `src_1' and `src_2'. */
void array_container_union(const array_container_t *src_1,
                           const array_container_t *src_2,
                           array_container_t *dst);

/* symmetric difference, see array_container_union */
void array_container_xor(const array_container_t *array_1,
                         const array_container_t *array_2,
                         array_container_t *out);

/* Computes the intersection of src_1 and src_2 and write the result to
 * dst. It is assumed that dst is distinct from both src_1 and src_2. */
void array_container_intersection(const array_container_t *src_1,
                                  const array_container_t *src_2,
                                  array_container_t *dst);

/* Check whether src_1 and src_2 intersect. */
bool array_container_intersect(const array_container_t *src_1,
                               const array_container_t *src_2);

/* computers the size of the intersection between two arrays.
 */
int array_container_intersection_cardinality(const array_container_t *src_1,
                                             const array_container_t *src_2);

/* computes the intersection of array1 and array2 and write the result to
 * array1.
 * */
void array_container_intersection_inplace(array_container_t *src_1,
                                          const array_container_t *src_2);

/*
 * Write out the 16-bit integers contained in this container as a list of 32-bit
 * integers using base
 * as the starting value (it might be expected that base has zeros in its 16
 * least significant bits).
 * The function returns the number of values written.
 * The caller is responsible for allocating enough memory in out.
 */
int array_container_to_uint32_array(void *vout, const array_container_t *cont,
                                    uint32_t base);

/* Compute the number of runs */
int32_t array_container_number_of_runs(const array_container_t *ac);

/*
 * Print this container using printf (useful for debugging).
 */
void array_container_printf(const array_container_t *v);

/*
 * Print this container using printf as a comma-separated list of 32-bit
 * integers starting at base.
 */
void array_container_printf_as_uint32_array(const array_container_t *v,
                                            uint32_t base);

bool array_container_validate(const array_container_t *v, const char **reason);

/**
 * Return the serialized size in bytes of a container having cardinality "card".
 */
static inline int32_t array_container_serialized_size_in_bytes(int32_t card) {
    return card * sizeof(uint16_t);
}

/**
 * Increase capacity to at least min.
 * Whether the existing data needs to be copied over depends on the "preserve"
 * parameter. If preserve is false, then the new content will be uninitialized,
 * otherwise the old content is copied.
 */
void array_container_grow(array_container_t *container, int32_t min,
                          bool preserve);

bool array_container_iterate(const array_container_t *cont, uint32_t base,
                             roaring_iterator iterator, void *ptr);
bool array_container_iterate64(const array_container_t *cont, uint32_t base,
                               roaring_iterator64 iterator, uint64_t high_bits,
                               void *ptr);

/**
 * Writes the underlying array to buf, outputs how many bytes were written.
 * This is meant to be byte-by-byte compatible with the Java and Go versions of
 * Roaring.
 * The number of bytes written should be
 * array_container_size_in_bytes(container).
 *
 */
int32_t array_container_write(const array_container_t *container, char *buf);
/**
 * Reads the instance from buf, outputs how many bytes were read.
 * This is meant to be byte-by-byte compatible with the Java and Go versions of
 * Roaring.
 * The number of bytes read should be array_container_size_in_bytes(container).
 * You need to provide the (known) cardinality.
 */
int32_t array_container_read(int32_t cardinality, array_container_t *container,
                             const char *buf);

/**
 * Return the serialized size in bytes of a container (see
 * bitset_container_write)
 * This is meant to be compatible with the Java and Go versions of Roaring and
 * assumes
 * that the cardinality of the container is already known.
 *
 */
CROARING_ALLOW_UNALIGNED
static inline int32_t array_container_size_in_bytes(
    const array_container_t *container) {
    return container->cardinality * sizeof(uint16_t);
}

/**
 * Return true if the two arrays have the same content.
 */
CROARING_ALLOW_UNALIGNED
static inline bool array_container_equals(const array_container_t *container1,
                                          const array_container_t *container2) {
    if (container1->cardinality != container2->cardinality) {
        return false;
    }
    return memequals(container1->array, container2->array,
                     container1->cardinality * 2);
}

/**
 * Return true if container1 is a subset of container2.
 */
bool array_container_is_subset(const array_container_t *container1,
                               const array_container_t *container2);

/**
 * If the element of given rank is in this container, supposing that the first
 * element has rank start_rank, then the function returns true and sets element
 * accordingly.
 * Otherwise, it returns false and update start_rank.
 */
static inline bool array_container_select(const array_container_t *container,
                                          uint32_t *start_rank, uint32_t rank,
                                          uint32_t *element) {
    int card = array_container_cardinality(container);
    if (*start_rank + card <= rank) {
        *start_rank += card;
        return false;
    } else {
        *element = container->array[rank - *start_rank];
        return true;
    }
}

/* Computes the  difference of array1 and array2 and write the result
 * to array out.
 * Array out does not need to be distinct from array_1
 */
void array_container_andnot(const array_container_t *array_1,
                            const array_container_t *array_2,
                            array_container_t *out);

/* Append x to the set. Assumes that the value is larger than any preceding
 * values.  */
static inline void array_container_append(array_container_t *arr,
                                          uint16_t pos) {
    const int32_t capacity = arr->capacity;

    if (array_container_full(arr)) {
        array_container_grow(arr, capacity + 1, true);
    }

    arr->array[arr->cardinality++] = pos;
}

/**
 * Add value to the set if final cardinality doesn't exceed max_cardinality.
 * Return code:
 * 1  -- value was added
 * 0  -- value was already present
 * -1 -- value was not added because cardinality would exceed max_cardinality
 */
static inline int array_container_try_add(array_container_t *arr,
                                          uint16_t value,
                                          int32_t max_cardinality) {
    const int32_t cardinality = arr->cardinality;

    // best case, we can append.
    if ((array_container_empty(arr) || arr->array[cardinality - 1] < value) &&
        cardinality < max_cardinality) {
        array_container_append(arr, value);
        return 1;
    }

    const int32_t loc = binarySearch(arr->array, cardinality, value);

    if (loc >= 0) {
        return 0;
    } else if (cardinality < max_cardinality) {
        if (array_container_full(arr)) {
            array_container_grow(arr, arr->capacity + 1, true);
        }
        const int32_t insert_idx = -loc - 1;
        memmove(arr->array + insert_idx + 1, arr->array + insert_idx,
                (cardinality - insert_idx) * sizeof(uint16_t));
        arr->array[insert_idx] = value;
        arr->cardinality++;
        return 1;
    } else {
        return -1;
    }
}

/* Add value to the set. Returns true if x was not already present.  */
static inline bool array_container_add(array_container_t *arr, uint16_t value) {
    return array_container_try_add(arr, value, INT32_MAX) == 1;
}

/* Remove x from the set. Returns true if x was present.  */
static inline bool array_container_remove(array_container_t *arr,
                                          uint16_t pos) {
    const int32_t idx = binarySearch(arr->array, arr->cardinality, pos);
    const bool is_present = idx >= 0;
    if (is_present) {
        memmove(arr->array + idx, arr->array + idx + 1,
                (arr->cardinality - idx - 1) * sizeof(uint16_t));
        arr->cardinality--;
    }

    return is_present;
}

/* Check whether x is present.  */
inline bool array_container_contains(const array_container_t *arr,
                                     uint16_t pos) {
    /**
     * SIMD Quad algorithm
     * Daniel Lemire, "You can beat the binary search," in Daniel Lemire's blog,
     *  April 27, 2026,
     * https://lemire.me/blog/2026/04/27/you-can-beat-the-binary-search/.
     */
    const int32_t gap = 16;
    const uint16_t *carr = arr->array;
    int32_t cardinality = arr->cardinality;
    if (cardinality < gap) {
        for (int32_t j = 0; j < cardinality; j++) {
            if (carr[j] >= pos) return carr[j] == pos;
        }
        return false;
    }
    int32_t num_blocks = cardinality / gap;
    int32_t base = 0;
    int32_t n = num_blocks;
    while (n > 3) {
        int32_t quarter = n >> 2;

        int32_t k1 = carr[(base + quarter + 1) * gap - 1];
        int32_t k2 = carr[(base + 2 * quarter + 1) * gap - 1];
        int32_t k3 = carr[(base + 3 * quarter + 1) * gap - 1];

        int32_t c1 = (k1 < pos);
        int32_t c2 = (k2 < pos);
        int32_t c3 = (k3 < pos);

        base += (c1 + c2 + c3) * quarter;
        n -= 3 * quarter;
    }
    while (n > 1) {
        int32_t half = n >> 1;
        base = (carr[(base + half + 1) * gap - 1] < pos) ? base + half : base;
        n -= half;
    }
    int32_t lo = (carr[(base + 1) * gap - 1] < pos) ? base + 1 : base;

    if (lo < num_blocks) {
        const uint16_t *blk = carr + lo * gap;
#ifdef CROARING_USENEON
        uint16x8_t needle = vdupq_n_u16(pos);
        uint16x8_t v0 = vld1q_u16(blk);
        uint16x8_t v1 = vld1q_u16(blk + 8);
        uint16x8_t hit =
            vorrq_u16(vceqq_u16(v0, needle), vceqq_u16(v1, needle));
        return vmaxvq_u16(hit) != 0;
#elif defined(CROARING_IS_X64)
        __m128i needle = _mm_set1_epi16((short)pos);
        __m128i v0 = _mm_loadu_si128((const __m128i *)blk);
        __m128i v1 = _mm_loadu_si128((const __m128i *)(blk + 8));
        __m128i hit = _mm_or_si128(_mm_cmpeq_epi16(v0, needle),
                                   _mm_cmpeq_epi16(v1, needle));
        return _mm_movemask_epi8(hit) != 0;
#else
        for (int32_t j = 0; j < gap; j++) {
            if (blk[j] >= pos) return blk[j] == pos;
        }
        return false;
#endif
    }

    for (int32_t j = num_blocks * gap; j < cardinality; j++) {
        uint16_t v = carr[j];
        if (v >= pos) return (v == pos);
    }
    return false;
}

void array_container_offset(const array_container_t *c, container_t **loc,
                            container_t **hic, uint16_t offset);

//* Check whether a range of values from range_start (included) to range_end
//(excluded) is present. */
static inline bool array_container_contains_range(const array_container_t *arr,
                                                  uint32_t range_start,
                                                  uint32_t range_end) {
    const int32_t range_count = range_end - range_start;
    const uint16_t rs_included = (uint16_t)range_start;
    const uint16_t re_included = (uint16_t)(range_end - 1);

    // Empty range is always included
    if (range_count <= 0) {
        return true;
    }
    if (range_count > arr->cardinality) {
        return false;
    }

    const int32_t start =
        binarySearch(arr->array, arr->cardinality, rs_included);
    // If this sorted array contains all items in the range:
    // * the start item must be found
    // * the last item in range range_count must exist, and be the expected end
    // value
    return (start >= 0) && (arr->cardinality >= start + range_count) &&
           (arr->array[start + range_count - 1] == re_included);
}

/* Returns the smallest value (assumes not empty) */
inline uint16_t array_container_minimum(const array_container_t *arr) {
    if (arr->cardinality == 0) return 0;
    return arr->array[0];
}

/* Returns the largest value (assumes not empty) */
inline uint16_t array_container_maximum(const array_container_t *arr) {
    if (arr->cardinality == 0) return 0;
    return arr->array[arr->cardinality - 1];
}

/* Returns the number of values equal or smaller than x */
inline int array_container_rank(const array_container_t *arr, uint16_t x) {
    const int32_t idx = binarySearch(arr->array, arr->cardinality, x);
    const bool is_present = idx >= 0;
    if (is_present) {
        return idx + 1;
    } else {
        return -idx - 1;
    }
}

/*  bulk version of array_container_rank(); return number of consumed elements
 */
inline uint32_t array_container_rank_many(const array_container_t *arr,
                                          uint64_t start_rank,
                                          const uint32_t *begin,
                                          const uint32_t *end, uint64_t *ans) {
    const uint16_t high = (uint16_t)((*begin) >> 16);
    uint32_t pos = 0;
    const uint32_t *iter = begin;
    for (; iter != end; iter++) {
        uint32_t x = *iter;
        uint16_t xhigh = (uint16_t)(x >> 16);
        if (xhigh != high) return iter - begin;  // stop at next container

        const int32_t idx =
            binarySearch(arr->array + pos, arr->cardinality - pos, (uint16_t)x);
        const bool is_present = idx >= 0;
        if (is_present) {
            *(ans++) = start_rank + pos + (idx + 1);
            pos = idx + 1;
        } else {
            *(ans++) = start_rank + pos + (-idx - 1);
        }
    }
    return iter - begin;
}

/* Returns the index of x , if not exsist return -1 */
inline int array_container_get_index(const array_container_t *arr, uint16_t x) {
    const int32_t idx = binarySearch(arr->array, arr->cardinality, x);
    const bool is_present = idx >= 0;
    if (is_present) {
        return idx;
    } else {
        return -1;
    }
}

/* Returns the index of the first value equal or larger than x, or -1 */
inline int array_container_index_equalorlarger(const array_container_t *arr,
                                               uint16_t x) {
    const int32_t idx = binarySearch(arr->array, arr->cardinality, x);
    const bool is_present = idx >= 0;
    if (is_present) {
        return idx;
    } else {
        int32_t candidate = -idx - 1;
        if (candidate < arr->cardinality) return candidate;
        return -1;
    }
}

/*
 * Adds all values in range [min,max] using hint:
 *   nvals_less is the number of array values less than $min
 *   nvals_greater is the number of array values greater than $max
 */
static inline void array_container_add_range_nvals(array_container_t *array,
                                                   uint32_t min, uint32_t max,
                                                   int32_t nvals_less,
                                                   int32_t nvals_greater) {
    int32_t union_cardinality = nvals_less + (max - min + 1) + nvals_greater;
    if (union_cardinality > array->capacity) {
        array_container_grow(array, union_cardinality, true);
    }
    memmove(&(array->array[union_cardinality - nvals_greater]),
            &(array->array[array->cardinality - nvals_greater]),
            nvals_greater * sizeof(uint16_t));
    for (uint32_t i = 0; i <= max - min; i++) {
        array->array[nvals_less + i] = (uint16_t)(min + i);
    }
    array->cardinality = union_cardinality;
}

/**
 * Adds all values in range [min,max]. This function is currently unused
 * and left as a documentation.
 */
/*static inline void array_container_add_range(array_container_t *array,
                                             uint32_t min, uint32_t max) {
    int32_t nvals_greater = count_greater(array->array, array->cardinality,
max); int32_t nvals_less = count_less(array->array, array->cardinality -
nvals_greater, min); array_container_add_range_nvals(array, min, max,
nvals_less, nvals_greater);
}*/

/*
 * Removes all elements array[pos] .. array[pos+count-1]
 */
static inline void array_container_remove_range(array_container_t *array,
                                                uint32_t pos, uint32_t count) {
    if (count != 0) {
        memmove(&(array->array[pos]), &(array->array[pos + count]),
                (array->cardinality - pos - count) * sizeof(uint16_t));
        array->cardinality -= count;
    }
}

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

#endif /* INCLUDE_CONTAINERS_ARRAY_H_ */

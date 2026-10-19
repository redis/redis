/*
 * array.c
 *
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <roaring/containers/array.h>
#include <roaring/memory.h>

#if CROARING_IS_X64
#ifndef CROARING_COMPILER_SUPPORTS_AVX512
#error "CROARING_COMPILER_SUPPORTS_AVX512 needs to be defined."
#endif  // CROARING_COMPILER_SUPPORTS_AVX512
#endif

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

extern inline uint16_t array_container_minimum(const array_container_t *arr);
extern inline uint16_t array_container_maximum(const array_container_t *arr);
extern inline int array_container_index_equalorlarger(
    const array_container_t *arr, uint16_t x);

extern inline int array_container_rank(const array_container_t *arr,
                                       uint16_t x);
extern inline uint32_t array_container_rank_many(const array_container_t *arr,
                                                 uint64_t start_rank,
                                                 const uint32_t *begin,
                                                 const uint32_t *end,
                                                 uint64_t *ans);
extern inline int array_container_get_index(const array_container_t *arr,
                                            uint16_t x);
extern inline bool array_container_contains(const array_container_t *arr,
                                            uint16_t pos);
extern inline int array_container_cardinality(const array_container_t *array);
extern inline bool array_container_nonzero_cardinality(
    const array_container_t *array);
extern inline int32_t array_container_serialized_size_in_bytes(int32_t card);
extern inline bool array_container_empty(const array_container_t *array);
extern inline bool array_container_full(const array_container_t *array);

/* Create a new array with capacity size. Return NULL in case of failure. */
array_container_t *array_container_create_given_capacity(int32_t size) {
    array_container_t *container;

    if ((container = (array_container_t *)roaring_malloc(
             sizeof(array_container_t))) == NULL) {
        return NULL;
    }

    if (size <= 0) {  // we don't want to rely on malloc(0)
        container->array = NULL;
    } else if ((container->array = (uint16_t *)roaring_malloc(sizeof(uint16_t) *
                                                              size)) == NULL) {
        roaring_free(container);
        return NULL;
    }

    container->capacity = size;
    container->cardinality = 0;

    return container;
}

/* Create a new array. Return NULL in case of failure. */
array_container_t *array_container_create(void) {
    return array_container_create_given_capacity(ARRAY_DEFAULT_INIT_SIZE);
}

/* Create a new array containing all values in [min,max). */
array_container_t *array_container_create_range(uint32_t min, uint32_t max) {
    array_container_t *answer =
        array_container_create_given_capacity(max - min + 1);
    if (answer == NULL) return answer;
    answer->cardinality = 0;
    for (uint32_t k = min; k < max; k++) {
        answer->array[answer->cardinality++] = k;
    }
    return answer;
}

/* Duplicate container */
CROARING_ALLOW_UNALIGNED
array_container_t *array_container_clone(const array_container_t *src) {
    array_container_t *newcontainer =
        array_container_create_given_capacity(src->capacity);
    if (newcontainer == NULL) return NULL;

    newcontainer->cardinality = src->cardinality;

    memcpy(newcontainer->array, src->array,
           src->cardinality * sizeof(uint16_t));

    return newcontainer;
}

void array_container_offset(const array_container_t *c, container_t **loc,
                            container_t **hic, uint16_t offset) {
    array_container_t *lo = NULL, *hi = NULL;
    int top, lo_cap, hi_cap;

    top = (1 << 16) - offset;

    lo_cap = count_less(c->array, c->cardinality, top);
    if (loc && lo_cap) {
        lo = array_container_create_given_capacity(lo_cap);
        for (int i = 0; i < lo_cap; ++i) {
            lo->array[i] = c->array[i] + offset;
        }
        lo->cardinality = lo_cap;
        *loc = (container_t *)lo;
    }

    hi_cap = c->cardinality - lo_cap;
    if (hic && hi_cap) {
        hi = array_container_create_given_capacity(hi_cap);
        for (int i = 0; i < hi_cap; ++i) {
            hi->array[i] = c->array[lo_cap + i] + offset;
        }
        hi->cardinality = hi_cap;
        *hic = (container_t *)hi;
    }
}

int array_container_shrink_to_fit(array_container_t *src) {
    if (src->cardinality == src->capacity) return 0;  // nothing to do
    int savings = src->capacity - src->cardinality;
    src->capacity = src->cardinality;
    if (src->capacity ==
        0) {  // we do not want to rely on realloc for zero allocs
        roaring_free(src->array);
        src->array = NULL;
    } else {
        uint16_t *oldarray = src->array;
        src->array = (uint16_t *)roaring_realloc(
            oldarray, src->capacity * sizeof(uint16_t));
        if (src->array == NULL) roaring_free(oldarray);  // should never happen?
    }
    return savings;
}

/* Free memory. */
void array_container_free(array_container_t *arr) {
    if (arr == NULL) return;
    roaring_free(arr->array);
    roaring_free(arr);
}

static inline int32_t grow_capacity(int32_t capacity) {
    return (capacity <= 0)   ? ARRAY_DEFAULT_INIT_SIZE
           : capacity < 64   ? capacity * 2
           : capacity < 1024 ? capacity * 3 / 2
                             : capacity * 5 / 4;
}

static inline int32_t clamp(int32_t val, int32_t min, int32_t max) {
    return ((val < min) ? min : (val > max) ? max : val);
}

void array_container_grow(array_container_t *container, int32_t min,
                          bool preserve) {
    int32_t max = (min <= DEFAULT_MAX_SIZE ? DEFAULT_MAX_SIZE : 65536);
    int32_t new_capacity = clamp(grow_capacity(container->capacity), min, max);

    container->capacity = new_capacity;
    uint16_t *array = container->array;

    if (preserve) {
        container->array =
            (uint16_t *)roaring_realloc(array, new_capacity * sizeof(uint16_t));
        if (container->array == NULL) roaring_free(array);
    } else {
        roaring_free(array);
        container->array =
            (uint16_t *)roaring_malloc(new_capacity * sizeof(uint16_t));
    }

    // if realloc fails, we have container->array == NULL.
}

/* Copy one container into another. We assume that they are distinct. */
void array_container_copy(const array_container_t *src,
                          array_container_t *dst) {
    const int32_t cardinality = src->cardinality;
    if (cardinality > dst->capacity) {
        array_container_grow(dst, cardinality, false);
    }

    dst->cardinality = cardinality;
    memcpy(dst->array, src->array, cardinality * sizeof(uint16_t));
}

void array_container_add_from_range(array_container_t *arr, uint32_t min,
                                    uint32_t max, uint16_t step) {
    for (uint32_t value = min; value < max; value += step) {
        array_container_append(arr, value);
    }
}

/* Computes the union of array1 and array2 and write the result to arrayout.
 * It is assumed that arrayout is distinct from both array1 and array2.
 */
void array_container_union(const array_container_t *array_1,
                           const array_container_t *array_2,
                           array_container_t *out) {
    const int32_t card_1 = array_1->cardinality, card_2 = array_2->cardinality;
    const int32_t max_cardinality = card_1 + card_2;

    if (out->capacity < max_cardinality) {
        array_container_grow(out, max_cardinality, false);
    }
    out->cardinality = (int32_t)fast_union_uint16(
        array_1->array, card_1, array_2->array, card_2, out->array);
}

/* Computes the  difference of array1 and array2 and write the result
 * to array out.
 * Array out does not need to be distinct from array_1
 */
void array_container_andnot(const array_container_t *array_1,
                            const array_container_t *array_2,
                            array_container_t *out) {
    if (out->capacity < array_1->cardinality)
        array_container_grow(out, array_1->cardinality, false);
#if CROARING_IS_X64
    if ((croaring_hardware_support() & ROARING_SUPPORTS_AVX2) &&
        (out != array_1) && (out != array_2)) {
        out->cardinality = difference_vector16(
            array_1->array, array_1->cardinality, array_2->array,
            array_2->cardinality, out->array);
    } else {
        out->cardinality =
            difference_uint16(array_1->array, array_1->cardinality,
                              array_2->array, array_2->cardinality, out->array);
    }
#else
    out->cardinality =
        difference_uint16(array_1->array, array_1->cardinality, array_2->array,
                          array_2->cardinality, out->array);
#endif
}

/* Computes the symmetric difference of array1 and array2 and write the
 * result
 * to arrayout.
 * It is assumed that arrayout is distinct from both array1 and array2.
 */
void array_container_xor(const array_container_t *array_1,
                         const array_container_t *array_2,
                         array_container_t *out) {
    const int32_t card_1 = array_1->cardinality, card_2 = array_2->cardinality;
    const int32_t max_cardinality = card_1 + card_2;
    if (out->capacity < max_cardinality) {
        array_container_grow(out, max_cardinality, false);
    }

#if CROARING_IS_X64
    if (croaring_hardware_support() & ROARING_SUPPORTS_AVX2) {
        out->cardinality =
            xor_vector16(array_1->array, array_1->cardinality, array_2->array,
                         array_2->cardinality, out->array);
    } else {
        out->cardinality =
            xor_uint16(array_1->array, array_1->cardinality, array_2->array,
                       array_2->cardinality, out->array);
    }
#else
    out->cardinality =
        xor_uint16(array_1->array, array_1->cardinality, array_2->array,
                   array_2->cardinality, out->array);
#endif
}

static inline int32_t minimum_int32(int32_t a, int32_t b) {
    return (a < b) ? a : b;
}

/* computes the intersection of array1 and array2 and write the result to
 * arrayout.
 * It is assumed that arrayout is distinct from both array1 and array2.
 * */
void array_container_intersection(const array_container_t *array1,
                                  const array_container_t *array2,
                                  array_container_t *out) {
    int32_t card_1 = array1->cardinality, card_2 = array2->cardinality,
            min_card = minimum_int32(card_1, card_2);
    const int threshold = 64;  // subject to tuning
#if CROARING_IS_X64
    if (out->capacity < min_card) {
        array_container_grow(out, min_card + sizeof(__m128i) / sizeof(uint16_t),
                             false);
    }
#else
    if (out->capacity < min_card) {
        array_container_grow(out, min_card, false);
    }
#endif

    if (card_1 * threshold < card_2) {
        out->cardinality = intersect_skewed_uint16(
            array1->array, card_1, array2->array, card_2, out->array);
    } else if (card_2 * threshold < card_1) {
        out->cardinality = intersect_skewed_uint16(
            array2->array, card_2, array1->array, card_1, out->array);
    } else {
#if CROARING_IS_X64
        if (croaring_hardware_support() & ROARING_SUPPORTS_AVX2) {
            out->cardinality = intersect_vector16(
                array1->array, card_1, array2->array, card_2, out->array);
        } else {
            out->cardinality = intersect_uint16(
                array1->array, card_1, array2->array, card_2, out->array);
        }
#else
        out->cardinality = intersect_uint16(array1->array, card_1,
                                            array2->array, card_2, out->array);
#endif
    }
}

/* computes the size of the intersection of array1 and array2
 * */
int array_container_intersection_cardinality(const array_container_t *array1,
                                             const array_container_t *array2) {
    int32_t card_1 = array1->cardinality, card_2 = array2->cardinality;
    const int threshold = 64;  // subject to tuning
    if (card_1 * threshold < card_2) {
        return intersect_skewed_uint16_cardinality(array1->array, card_1,
                                                   array2->array, card_2);
    } else if (card_2 * threshold < card_1) {
        return intersect_skewed_uint16_cardinality(array2->array, card_2,
                                                   array1->array, card_1);
    } else {
#if CROARING_IS_X64
        if (croaring_hardware_support() & ROARING_SUPPORTS_AVX2) {
            return intersect_vector16_cardinality(array1->array, card_1,
                                                  array2->array, card_2);
        } else {
            return intersect_uint16_cardinality(array1->array, card_1,
                                                array2->array, card_2);
        }
#else
        return intersect_uint16_cardinality(array1->array, card_1,
                                            array2->array, card_2);
#endif
    }
}

bool array_container_intersect(const array_container_t *array1,
                               const array_container_t *array2) {
    int32_t card_1 = array1->cardinality, card_2 = array2->cardinality;
    const int threshold = 64;  // subject to tuning
    if (card_1 * threshold < card_2) {
        return intersect_skewed_uint16_nonempty(array1->array, card_1,
                                                array2->array, card_2);
    } else if (card_2 * threshold < card_1) {
        return intersect_skewed_uint16_nonempty(array2->array, card_2,
                                                array1->array, card_1);
    } else {
        // we do not bother vectorizing
        return intersect_uint16_nonempty(array1->array, card_1, array2->array,
                                         card_2);
    }
}

/* computes the intersection of array1 and array2 and write the result to
 * array1.
 * */
void array_container_intersection_inplace(array_container_t *src_1,
                                          const array_container_t *src_2) {
    int32_t card_1 = src_1->cardinality, card_2 = src_2->cardinality;
    const int threshold = 64;  // subject to tuning
    if (card_1 * threshold < card_2) {
        src_1->cardinality = intersect_skewed_uint16(
            src_1->array, card_1, src_2->array, card_2, src_1->array);
    } else if (card_2 * threshold < card_1) {
        src_1->cardinality = intersect_skewed_uint16(
            src_2->array, card_2, src_1->array, card_1, src_1->array);
    } else {
#if CROARING_IS_X64
        if (croaring_hardware_support() & ROARING_SUPPORTS_AVX2) {
            src_1->cardinality = intersect_vector16_inplace(
                src_1->array, card_1, src_2->array, card_2);
        } else {
            src_1->cardinality = intersect_uint16(
                src_1->array, card_1, src_2->array, card_2, src_1->array);
        }
#else
        src_1->cardinality = intersect_uint16(
            src_1->array, card_1, src_2->array, card_2, src_1->array);
#endif
    }
}

CROARING_ALLOW_UNALIGNED
int array_container_to_uint32_array(void *vout, const array_container_t *cont,
                                    uint32_t base) {
#if CROARING_IS_X64
    int support = croaring_hardware_support();
#if CROARING_COMPILER_SUPPORTS_AVX512
    if (support & ROARING_SUPPORTS_AVX512) {
        return avx512_array_container_to_uint32_array(vout, cont->array,
                                                      cont->cardinality, base);
    }
#endif
    if (support & ROARING_SUPPORTS_AVX2) {
        return array_container_to_uint32_array_vector16(
            vout, cont->array, cont->cardinality, base);
    }
#endif  // CROARING_IS_X64
    int outpos = 0;
    uint32_t *out = (uint32_t *)vout;
    size_t i = 0;
    for (; i < (size_t)cont->cardinality; ++i) {
        const uint32_t val = base + cont->array[i];
        memcpy(out + outpos, &val,
               sizeof(uint32_t));  // should be compiled as a MOV on x64
        outpos++;
    }
    return outpos;
}

void array_container_printf(const array_container_t *v) {
    if (v->cardinality == 0) {
        printf("{}");
        return;
    }
    printf("{");
    printf("%d", v->array[0]);
    for (int i = 1; i < v->cardinality; ++i) {
        printf(",%d", v->array[i]);
    }
    printf("}");
}

void array_container_printf_as_uint32_array(const array_container_t *v,
                                            uint32_t base) {
    if (v->cardinality == 0) {
        return;
    }
    printf("%u", v->array[0] + base);
    for (int i = 1; i < v->cardinality; ++i) {
        printf(",%u", v->array[i] + base);
    }
}

/*
 * Validate the container. Returns true if valid.
 */
bool array_container_validate(const array_container_t *v, const char **reason) {
    if (v->capacity < 0) {
        *reason = "negative capacity";
        return false;
    }
    if (v->cardinality < 0) {
        *reason = "negative cardinality";
        return false;
    }
    if (v->cardinality > v->capacity) {
        *reason = "cardinality exceeds capacity";
        return false;
    }
    if (v->cardinality > DEFAULT_MAX_SIZE) {
        *reason = "cardinality exceeds DEFAULT_MAX_SIZE";
        return false;
    }
    if (v->cardinality == 0) {
        *reason = "zero cardinality";
        return false;
    }

    if (v->array == NULL) {
        *reason = "NULL array pointer";
        return false;
    }
    uint16_t prev = v->array[0];
    for (int i = 1; i < v->cardinality; ++i) {
        if (v->array[i] <= prev) {
            *reason = "array elements not strictly increasing";
            return false;
        }
        prev = v->array[i];
    }

    return true;
}

/* Compute the number of runs */
int32_t array_container_number_of_runs(const array_container_t *ac) {
    // Can SIMD work here?
    int32_t nr_runs = 0;
    int32_t prev = -2;
    for (const uint16_t *p = ac->array; p != ac->array + ac->cardinality; ++p) {
        if (*p != prev + 1) nr_runs++;
        prev = *p;
    }
    return nr_runs;
}

/**
 * Writes the underlying array to buf, outputs how many bytes were written.
 * The number of bytes written should be
 * array_container_size_in_bytes(container).
 *
 */
int32_t array_container_write(const array_container_t *container, char *buf) {
#if CROARING_IS_BIG_ENDIAN
    for (int32_t i = 0; i < container->cardinality; ++i) {
        uint16_t v_le = croaring_htole16(container->array[i]);
        memcpy(buf + i * sizeof(uint16_t), &v_le, sizeof(uint16_t));
    }
#else
    memcpy(buf, container->array, container->cardinality * sizeof(uint16_t));
#endif
    return array_container_size_in_bytes(container);
}

bool array_container_is_subset(const array_container_t *container1,
                               const array_container_t *container2) {
    if (container1->cardinality > container2->cardinality) {
        return false;
    }
    int i1 = 0, i2 = 0;
    while (i1 < container1->cardinality && i2 < container2->cardinality) {
        if (container1->array[i1] == container2->array[i2]) {
            i1++;
            i2++;
        } else if (container1->array[i1] > container2->array[i2]) {
            i2++;
        } else {  // container1->array[i1] < container2->array[i2]
            return false;
        }
    }
    if (i1 == container1->cardinality) {
        return true;
    } else {
        return false;
    }
}

int32_t array_container_read(int32_t cardinality, array_container_t *container,
                             const char *buf) {
    if (container->capacity < cardinality) {
        array_container_grow(container, cardinality, false);
    }
    container->cardinality = cardinality;
#if CROARING_IS_BIG_ENDIAN
    for (int32_t i = 0; i < cardinality; ++i) {
        uint16_t v_le;
        memcpy(&v_le, buf + i * sizeof(uint16_t), sizeof(uint16_t));
        container->array[i] = croaring_letoh16(v_le);
    }
#else
    memcpy(container->array, buf, container->cardinality * sizeof(uint16_t));
#endif

    return array_container_size_in_bytes(container);
}

bool array_container_iterate(const array_container_t *cont, uint32_t base,
                             roaring_iterator iterator, void *ptr) {
    for (int i = 0; i < cont->cardinality; i++)
        if (!iterator(cont->array[i] + base, ptr)) return false;
    return true;
}

bool array_container_iterate64(const array_container_t *cont, uint32_t base,
                               roaring_iterator64 iterator, uint64_t high_bits,
                               void *ptr) {
    for (int i = 0; i < cont->cardinality; i++)
        if (!iterator(high_bits | (uint64_t)(cont->array[i] + base), ptr))
            return false;
    return true;
}

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

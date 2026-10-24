/*
 * mixed_intersection.c
 *
 */

#include <roaring/array_util.h>
#include <roaring/bitset_util.h>
#include <roaring/containers/convert.h>
#include <roaring/containers/mixed_intersection.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

/* Compute the intersection of src_1 and src_2 and write the result to
 * dst.  */
void array_bitset_container_intersection(const array_container_t *src_1,
                                         const bitset_container_t *src_2,
                                         array_container_t *dst) {
    if (dst->capacity < src_1->cardinality) {
        array_container_grow(dst, src_1->cardinality, false);
    }
    int32_t newcard = 0;  // dst could be src_1
    const int32_t origcard = src_1->cardinality;
    for (int i = 0; i < origcard; ++i) {
        uint16_t key = src_1->array[i];
        // this branchless approach is much faster...
        dst->array[newcard] = key;
        newcard += bitset_container_contains(src_2, key);
        /**
         * we could do it this way instead...
         * if (bitset_container_contains(src_2, key)) {
         * dst->array[newcard++] = key;
         * }
         * but if the result is unpredictible, the processor generates
         * many mispredicted branches.
         * Difference can be huge (from 3 cycles when predictible all the way
         * to 16 cycles when unpredictible.
         * See
         * https://github.com/lemire/Code-used-on-Daniel-Lemire-s-blog/blob/master/extra/bitset/c/arraybitsetintersection.c
         */
    }
    dst->cardinality = newcard;
}

/* Compute the size of the intersection of src_1 and src_2. */
int array_bitset_container_intersection_cardinality(
    const array_container_t *src_1, const bitset_container_t *src_2) {
    int32_t newcard = 0;
    const int32_t origcard = src_1->cardinality;
    for (int i = 0; i < origcard; ++i) {
        uint16_t key = src_1->array[i];
        newcard += bitset_container_contains(src_2, key);
    }
    return newcard;
}

bool array_bitset_container_intersect(const array_container_t *src_1,
                                      const bitset_container_t *src_2) {
    const int32_t origcard = src_1->cardinality;
    for (int i = 0; i < origcard; ++i) {
        uint16_t key = src_1->array[i];
        if (bitset_container_contains(src_2, key)) return true;
    }
    return false;
}

/* Compute the intersection of src_1 and src_2 and write the result to
 * dst. It is allowed for dst to be equal to src_1. We assume that dst is a
 * valid container. */
void array_run_container_intersection(const array_container_t *src_1,
                                      const run_container_t *src_2,
                                      array_container_t *dst) {
    if (run_container_is_full(src_2)) {
        if (dst != src_1) array_container_copy(src_1, dst);
        return;
    }
    if (dst->capacity < src_1->cardinality) {
        array_container_grow(dst, src_1->cardinality, false);
    }
    if (src_2->n_runs == 0) {
        return;
    }
    int32_t rlepos = 0;
    int32_t arraypos = 0;
    rle16_t rle = src_2->runs[rlepos];
    int32_t newcard = 0;
    while (arraypos < src_1->cardinality) {
        const uint16_t arrayval = src_1->array[arraypos];
        while (rle.value + rle.length <
               arrayval) {  // this will frequently be false
            ++rlepos;
            if (rlepos == src_2->n_runs) {
                dst->cardinality = newcard;
                return;  // we are done
            }
            rle = src_2->runs[rlepos];
        }
        if (rle.value > arrayval) {
            arraypos = advanceUntil(src_1->array, arraypos, src_1->cardinality,
                                    rle.value);
        } else {
            dst->array[newcard] = arrayval;
            newcard++;
            arraypos++;
        }
    }
    dst->cardinality = newcard;
}

/* Compute the intersection of src_1 and src_2 and write the result to
 * *dst. If the result is true then the result is a bitset_container_t
 * otherwise is a array_container_t. If *dst ==  src_2, an in-place processing
 * is attempted.*/
bool run_bitset_container_intersection(const run_container_t *src_1,
                                       const bitset_container_t *src_2,
                                       container_t **dst) {
    if (run_container_is_full(src_1)) {
        if (*dst != src_2) *dst = bitset_container_clone(src_2);
        return true;
    }
    int32_t card = run_container_cardinality(src_1);
    if (card <= DEFAULT_MAX_SIZE) {
        // result can only be an array (assuming that we never make a
        // RunContainer)
        if (card > src_2->cardinality) {
            card = src_2->cardinality;
        }
        array_container_t *answer = array_container_create_given_capacity(card);
        *dst = answer;
        if (*dst == NULL) {
            return false;
        }
        for (int32_t rlepos = 0; rlepos < src_1->n_runs; ++rlepos) {
            rle16_t rle = src_1->runs[rlepos];
            uint32_t endofrun = (uint32_t)rle.value + rle.length;
            for (uint32_t runValue = rle.value; runValue <= endofrun;
                 ++runValue) {
                answer->array[answer->cardinality] = (uint16_t)runValue;
                answer->cardinality +=
                    bitset_container_contains(src_2, runValue);
            }
        }
        return false;
    }
    if (*dst == src_2) {  // we attempt in-place
        bitset_container_t *answer = CAST_bitset(*dst);
        uint32_t start = 0;
        for (int32_t rlepos = 0; rlepos < src_1->n_runs; ++rlepos) {
            const rle16_t rle = src_1->runs[rlepos];
            uint32_t end = rle.value;
            bitset_reset_range(src_2->words, start, end);

            start = end + rle.length + 1;
        }
        bitset_reset_range(src_2->words, start, UINT32_C(1) << 16);
        answer->cardinality = bitset_container_compute_cardinality(answer);
        if (src_2->cardinality > DEFAULT_MAX_SIZE) {
            return true;
        } else {
            array_container_t *newanswer = array_container_from_bitset(src_2);
            if (newanswer == NULL) {
                *dst = NULL;
                return false;
            }
            *dst = newanswer;
            return false;
        }
    } else {  // no inplace
        // we expect the answer to be a bitmap (if we are lucky)
        bitset_container_t *answer = bitset_container_clone(src_2);

        *dst = answer;
        if (answer == NULL) {
            return true;
        }
        uint32_t start = 0;
        for (int32_t rlepos = 0; rlepos < src_1->n_runs; ++rlepos) {
            const rle16_t rle = src_1->runs[rlepos];
            uint32_t end = rle.value;
            bitset_reset_range(answer->words, start, end);
            start = end + rle.length + 1;
        }
        bitset_reset_range(answer->words, start, UINT32_C(1) << 16);
        answer->cardinality = bitset_container_compute_cardinality(answer);

        if (answer->cardinality > DEFAULT_MAX_SIZE) {
            return true;
        } else {
            array_container_t *newanswer = array_container_from_bitset(answer);
            bitset_container_free(CAST_bitset(*dst));
            if (newanswer == NULL) {
                *dst = NULL;
                return false;
            }
            *dst = newanswer;
            return false;
        }
    }
}

/* Compute the size of the intersection between src_1 and src_2 . */
int array_run_container_intersection_cardinality(const array_container_t *src_1,
                                                 const run_container_t *src_2) {
    if (run_container_is_full(src_2)) {
        return src_1->cardinality;
    }
    if (src_2->n_runs == 0) {
        return 0;
    }
    int32_t rlepos = 0;
    int32_t arraypos = 0;
    rle16_t rle = src_2->runs[rlepos];
    int32_t newcard = 0;
    while (arraypos < src_1->cardinality) {
        const uint16_t arrayval = src_1->array[arraypos];
        while (rle.value + rle.length <
               arrayval) {  // this will frequently be false
            ++rlepos;
            if (rlepos == src_2->n_runs) {
                return newcard;  // we are done
            }
            rle = src_2->runs[rlepos];
        }
        if (rle.value > arrayval) {
            arraypos = advanceUntil(src_1->array, arraypos, src_1->cardinality,
                                    rle.value);
        } else {
            newcard++;
            arraypos++;
        }
    }
    return newcard;
}

/* Compute the intersection  between src_1 and src_2
 **/
int run_bitset_container_intersection_cardinality(
    const run_container_t *src_1, const bitset_container_t *src_2) {
    if (run_container_is_full(src_1)) {
        return bitset_container_cardinality(src_2);
    }
    int answer = 0;
    for (int32_t rlepos = 0; rlepos < src_1->n_runs; ++rlepos) {
        rle16_t rle = src_1->runs[rlepos];
        answer +=
            bitset_lenrange_cardinality(src_2->words, rle.value, rle.length);
    }
    return answer;
}

bool array_run_container_intersect(const array_container_t *src_1,
                                   const run_container_t *src_2) {
    if (run_container_is_full(src_2)) {
        return !array_container_empty(src_1);
    }
    if (src_2->n_runs == 0) {
        return false;
    }
    int32_t rlepos = 0;
    int32_t arraypos = 0;
    rle16_t rle = src_2->runs[rlepos];
    while (arraypos < src_1->cardinality) {
        const uint16_t arrayval = src_1->array[arraypos];
        while (rle.value + rle.length <
               arrayval) {  // this will frequently be false
            ++rlepos;
            if (rlepos == src_2->n_runs) {
                return false;  // we are done
            }
            rle = src_2->runs[rlepos];
        }
        if (rle.value > arrayval) {
            arraypos = advanceUntil(src_1->array, arraypos, src_1->cardinality,
                                    rle.value);
        } else {
            return true;
        }
    }
    return false;
}

/* Compute the intersection  between src_1 and src_2
 **/
bool run_bitset_container_intersect(const run_container_t *src_1,
                                    const bitset_container_t *src_2) {
    if (run_container_is_full(src_1)) {
        return !bitset_container_empty(src_2);
    }
    for (int32_t rlepos = 0; rlepos < src_1->n_runs; ++rlepos) {
        rle16_t rle = src_1->runs[rlepos];
        if (!bitset_lenrange_empty(src_2->words, rle.value, rle.length))
            return true;
    }
    return false;
}

/*
 * Compute the intersection between src_1 and src_2 and write the result
 * to *dst. If the return function is true, the result is a bitset_container_t
 * otherwise is a array_container_t.
 */
bool bitset_bitset_container_intersection(const bitset_container_t *src_1,
                                          const bitset_container_t *src_2,
                                          container_t **dst) {
    const int newCardinality = bitset_container_and_justcard(src_1, src_2);
    if (newCardinality > DEFAULT_MAX_SIZE) {
        *dst = bitset_container_create();
        if (*dst != NULL) {
            bitset_container_and_nocard(src_1, src_2, CAST_bitset(*dst));
            CAST_bitset(*dst)->cardinality = newCardinality;
        }
        return true;  // it is a bitset
    }
    *dst = array_container_create_given_capacity(newCardinality);
    if (*dst != NULL) {
        CAST_array(*dst)->cardinality = newCardinality;
        bitset_extract_intersection_setbits_uint16(
            src_1->words, src_2->words, BITSET_CONTAINER_SIZE_IN_WORDS,
            CAST_array(*dst)->array, 0);
    }
    return false;  // not a bitset
}

bool bitset_bitset_container_intersection_inplace(
    bitset_container_t *src_1, const bitset_container_t *src_2,
    container_t **dst) {
    const int newCardinality = bitset_container_and_justcard(src_1, src_2);
    if (newCardinality > DEFAULT_MAX_SIZE) {
        *dst = src_1;
        bitset_container_and_nocard(src_1, src_2, src_1);
        CAST_bitset(*dst)->cardinality = newCardinality;
        return true;  // it is a bitset
    }
    *dst = array_container_create_given_capacity(newCardinality);
    if (*dst != NULL) {
        CAST_array(*dst)->cardinality = newCardinality;
        bitset_extract_intersection_setbits_uint16(
            src_1->words, src_2->words, BITSET_CONTAINER_SIZE_IN_WORDS,
            CAST_array(*dst)->array, 0);
    }
    return false;  // not a bitset
}

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

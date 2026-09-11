/*
 * mixed_negation.c
 *
 */

#include <assert.h>
#include <string.h>

#include <roaring/array_util.h>
#include <roaring/bitset_util.h>
#include <roaring/containers/containers.h>
#include <roaring/containers/convert.h>
#include <roaring/containers/mixed_negation.h>
#include <roaring/containers/run.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

// TODO: make simplified and optimized negation code across
// the full range.

/* Negation across the entire range of the container.
 * Compute the  negation of src  and write the result
 * to *dst. The complement of a
 * sufficiently sparse set will always be dense and a hence a bitmap
' * We assume that dst is pre-allocated and a valid bitset container
 * There can be no in-place version.
 */
void array_container_negation(const array_container_t *src,
                              bitset_container_t *dst) {
    uint64_t card = UINT64_C(1 << 16);
    bitset_container_set_all(dst);

    if (src->cardinality == 0) {
        return;
    }

    dst->cardinality = (int32_t)bitset_clear_list(dst->words, card, src->array,
                                                  (uint64_t)src->cardinality);
}

/* Negation across the entire range of the container
 * Compute the  negation of src  and write the result
 * to *dst.  A true return value indicates a bitset result,
 * otherwise the result is an array container.
 *  We assume that dst is not pre-allocated. In
 * case of failure, *dst will be NULL.
 */
bool bitset_container_negation(const bitset_container_t *src,
                               container_t **dst) {
    return bitset_container_negation_range(src, 0, (1 << 16), dst);
}

/* inplace version */
/*
 * Same as bitset_container_negation except that if the output is to
 * be a
 * bitset_container_t, then src is modified and no allocation is made.
 * If the output is to be an array_container_t, then caller is responsible
 * to free the container.
 * In all cases, the result is in *dst.
 */
bool bitset_container_negation_inplace(bitset_container_t *src,
                                       container_t **dst) {
    return bitset_container_negation_range_inplace(src, 0, (1 << 16), dst);
}

/* Negation across the entire range of container
 * Compute the  negation of src  and write the result
 * to *dst.  Return values are the *_TYPECODES as defined * in containers.h
 *  We assume that dst is not pre-allocated. In
 * case of failure, *dst will be NULL.
 */
int run_container_negation(const run_container_t *src, container_t **dst) {
    return run_container_negation_range(src, 0, (1 << 16), dst);
}

/*
 * Same as run_container_negation except that if the output is to
 * be a
 * run_container_t, and has the capacity to hold the result,
 * then src is modified and no allocation is made.
 * In all cases, the result is in *dst.
 */
int run_container_negation_inplace(run_container_t *src, container_t **dst) {
    return run_container_negation_range_inplace(src, 0, (1 << 16), dst);
}

/* Negation across a range of the container.
 * Compute the  negation of src  and write the result
 * to *dst. Returns true if the result is a bitset container
 * and false for an array container.  *dst is not preallocated.
 */
bool array_container_negation_range(const array_container_t *src,
                                    const int range_start, const int range_end,
                                    container_t **dst) {
    /* close port of the Java implementation */
    if (range_start >= range_end) {
        *dst = array_container_clone(src);
        return false;
    }

    int32_t start_index =
        binarySearch(src->array, src->cardinality, (uint16_t)range_start);
    if (start_index < 0) start_index = -start_index - 1;

    int32_t last_index =
        binarySearch(src->array, src->cardinality, (uint16_t)(range_end - 1));
    if (last_index < 0) last_index = -last_index - 2;

    const int32_t current_values_in_range = last_index - start_index + 1;
    const int32_t span_to_be_flipped = range_end - range_start;
    const int32_t new_values_in_range =
        span_to_be_flipped - current_values_in_range;
    const int32_t cardinality_change =
        new_values_in_range - current_values_in_range;
    const int32_t new_cardinality = src->cardinality + cardinality_change;

    if (new_cardinality > DEFAULT_MAX_SIZE) {
        bitset_container_t *temp = bitset_container_from_array(src);
        bitset_flip_range(temp->words, (uint32_t)range_start,
                          (uint32_t)range_end);
        temp->cardinality = new_cardinality;
        *dst = temp;
        return true;
    }

    array_container_t *arr =
        array_container_create_given_capacity(new_cardinality);
    *dst = (container_t *)arr;
    if (new_cardinality == 0) {
        arr->cardinality = new_cardinality;
        return false;  // we are done.
    }
    // copy stuff before the active area
    memcpy(arr->array, src->array, start_index * sizeof(uint16_t));

    // work on the range
    int32_t out_pos = start_index, in_pos = start_index;
    int32_t val_in_range = range_start;
    for (; val_in_range < range_end && in_pos <= last_index; ++val_in_range) {
        if ((uint16_t)val_in_range != src->array[in_pos]) {
            arr->array[out_pos++] = (uint16_t)val_in_range;
        } else {
            ++in_pos;
        }
    }
    for (; val_in_range < range_end; ++val_in_range)
        arr->array[out_pos++] = (uint16_t)val_in_range;

    // content after the active range
    memcpy(arr->array + out_pos, src->array + (last_index + 1),
           (src->cardinality - (last_index + 1)) * sizeof(uint16_t));
    arr->cardinality = new_cardinality;
    return false;
}

/* Even when the result would fit, it is unclear how to make an
 * inplace version without inefficient copying.
 */

bool array_container_negation_range_inplace(array_container_t *src,
                                            const int range_start,
                                            const int range_end,
                                            container_t **dst) {
    bool ans = array_container_negation_range(src, range_start, range_end, dst);
    // TODO : try a real inplace version
    array_container_free(src);
    return ans;
}

/* Negation across a range of the container
 * Compute the  negation of src  and write the result
 * to *dst.  A true return value indicates a bitset result,
 * otherwise the result is an array container.
 *  We assume that dst is not pre-allocated. In
 * case of failure, *dst will be NULL.
 */
bool bitset_container_negation_range(const bitset_container_t *src,
                                     const int range_start, const int range_end,
                                     container_t **dst) {
    // TODO maybe consider density-based estimate
    // and sometimes build result directly as array, with
    // conversion back to bitset if wrong.  Or determine
    // actual result cardinality, then go directly for the known final cont.

    // keep computation using bitsets as long as possible.
    bitset_container_t *t = bitset_container_clone(src);
    bitset_flip_range(t->words, (uint32_t)range_start, (uint32_t)range_end);
    t->cardinality = bitset_container_compute_cardinality(t);

    if (t->cardinality > DEFAULT_MAX_SIZE) {
        *dst = t;
        return true;
    } else {
        *dst = array_container_from_bitset(t);
        bitset_container_free(t);
        return false;
    }
}

/* inplace version */
/*
 * Same as bitset_container_negation except that if the output is to
 * be a
 * bitset_container_t, then src is modified and no allocation is made.
 * If the output is to be an array_container_t, then caller is responsible
 * to free the container.
 * In all cases, the result is in *dst.
 */
bool bitset_container_negation_range_inplace(bitset_container_t *src,
                                             const int range_start,
                                             const int range_end,
                                             container_t **dst) {
    bitset_flip_range(src->words, (uint32_t)range_start, (uint32_t)range_end);
    src->cardinality = bitset_container_compute_cardinality(src);
    if (src->cardinality > DEFAULT_MAX_SIZE) {
        *dst = src;
        return true;
    }
    *dst = array_container_from_bitset(src);
    bitset_container_free(src);
    return false;
}

/* Negation across a range of container
 * Compute the  negation of src  and write the result
 * to *dst. Return values are the *_TYPECODES as defined * in containers.h
 *  We assume that dst is not pre-allocated. In
 * case of failure, *dst will be NULL.
 */
int run_container_negation_range(const run_container_t *src,
                                 const int range_start, const int range_end,
                                 container_t **dst) {
    uint8_t return_typecode;

    // follows the Java implementation
    if (range_end <= range_start) {
        *dst = run_container_clone(src);
        return RUN_CONTAINER_TYPE;
    }

    run_container_t *ans = run_container_create_given_capacity(
        src->n_runs + 1);  // src->n_runs + 1);
    int k = 0;
    for (; k < src->n_runs && src->runs[k].value < range_start; ++k) {
        ans->runs[k] = src->runs[k];
        ans->n_runs++;
    }

    run_container_smart_append_exclusive(
        ans, (uint16_t)range_start, (uint16_t)(range_end - range_start - 1));

    for (; k < src->n_runs; ++k) {
        run_container_smart_append_exclusive(ans, src->runs[k].value,
                                             src->runs[k].length);
    }

    *dst = convert_run_to_efficient_container(ans, &return_typecode);
    if (return_typecode != RUN_CONTAINER_TYPE) run_container_free(ans);

    return return_typecode;
}

/*
 * Same as run_container_negation except that if the output is to
 * be a
 * run_container_t, and has the capacity to hold the result,
 * then src is modified and no allocation is made.
 * In all cases, the result is in *dst.
 */
int run_container_negation_range_inplace(run_container_t *src,
                                         const int range_start,
                                         const int range_end,
                                         container_t **dst) {
    uint8_t return_typecode;

    if (range_end <= range_start) {
        *dst = src;
        return RUN_CONTAINER_TYPE;
    }

    // TODO: efficient special case when range is 0 to 65535 inclusive

    if (src->capacity == src->n_runs) {
        // no excess room.  More checking to see if result can fit
        bool last_val_before_range = false;
        bool first_val_in_range = false;
        bool last_val_in_range = false;
        bool first_val_past_range = false;

        if (range_start > 0)
            last_val_before_range =
                run_container_contains(src, (uint16_t)(range_start - 1));
        first_val_in_range = run_container_contains(src, (uint16_t)range_start);

        if (last_val_before_range == first_val_in_range) {
            last_val_in_range =
                run_container_contains(src, (uint16_t)(range_end - 1));
            if (range_end != 0x10000)
                first_val_past_range =
                    run_container_contains(src, (uint16_t)range_end);

            if (last_val_in_range ==
                first_val_past_range) {  // no space for inplace
                int ans = run_container_negation_range(src, range_start,
                                                       range_end, dst);
                run_container_free(src);
                return ans;
            }
        }
    }
    // all other cases: result will fit

    run_container_t *ans = src;
    int my_nbr_runs = src->n_runs;

    ans->n_runs = 0;
    int k = 0;
    for (; (k < my_nbr_runs) && (src->runs[k].value < range_start); ++k) {
        // ans->runs[k] = src->runs[k]; (would be self-copy)
        ans->n_runs++;
    }

    // as with Java implementation, use locals to give self a buffer of depth 1
    rle16_t buffered = CROARING_MAKE_RLE16(0, 0);
    rle16_t next = buffered;
    if (k < my_nbr_runs) buffered = src->runs[k];

    run_container_smart_append_exclusive(
        ans, (uint16_t)range_start, (uint16_t)(range_end - range_start - 1));

    for (; k < my_nbr_runs; ++k) {
        if (k + 1 < my_nbr_runs) next = src->runs[k + 1];

        run_container_smart_append_exclusive(ans, buffered.value,
                                             buffered.length);
        buffered = next;
    }

    *dst = convert_run_to_efficient_container(ans, &return_typecode);
    if (return_typecode != RUN_CONTAINER_TYPE) run_container_free(ans);

    return return_typecode;
}

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

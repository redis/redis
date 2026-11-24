/*
 * mixed_andnot.c.  More methods since operation is not symmetric,
 * except no "wide" andnot , so no lazy options motivated.
 */

#include <assert.h>
#include <string.h>

#include <roaring/array_util.h>
#include <roaring/bitset_util.h>
#include <roaring/containers/containers.h>
#include <roaring/containers/convert.h>
#include <roaring/containers/mixed_andnot.h>
#include <roaring/containers/perfparameters.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst, a valid array container that could be the same as dst.*/
void array_bitset_container_andnot(const array_container_t *src_1,
                                   const bitset_container_t *src_2,
                                   array_container_t *dst) {
    // follows Java implementation as of June 2016
    if (dst->capacity < src_1->cardinality) {
        array_container_grow(dst, src_1->cardinality, false);
    }
    int32_t newcard = 0;
    const int32_t origcard = src_1->cardinality;
    for (int i = 0; i < origcard; ++i) {
        uint16_t key = src_1->array[i];
        dst->array[newcard] = key;
        newcard += 1 - bitset_container_contains(src_2, key);
    }
    dst->cardinality = newcard;
}

/* Compute the andnot of src_1 and src_2 and write the result to
 * src_1 */

void array_bitset_container_iandnot(array_container_t *src_1,
                                    const bitset_container_t *src_2) {
    array_bitset_container_andnot(src_1, src_2, src_1);
}

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst, which does not initially have a valid container.
 * Return true for a bitset result; false for array
 */

bool bitset_array_container_andnot(const bitset_container_t *src_1,
                                   const array_container_t *src_2,
                                   container_t **dst) {
    // Java did this directly, but we have option of asm or avx
    bitset_container_t *result = bitset_container_create();
    bitset_container_copy(src_1, result);
    result->cardinality =
        (int32_t)bitset_clear_list(result->words, (uint64_t)result->cardinality,
                                   src_2->array, (uint64_t)src_2->cardinality);

    // do required type conversions.
    if (result->cardinality <= DEFAULT_MAX_SIZE) {
        *dst = array_container_from_bitset(result);
        bitset_container_free(result);
        return false;
    }
    *dst = result;
    return true;
}

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst (which has no container initially).  It will modify src_1
 * to be dst if the result is a bitset.  Otherwise, it will
 * free src_1 and dst will be a new array container.  In both
 * cases, the caller is responsible for deallocating dst.
 * Returns true iff dst is a bitset  */

bool bitset_array_container_iandnot(bitset_container_t *src_1,
                                    const array_container_t *src_2,
                                    container_t **dst) {
    *dst = src_1;
    src_1->cardinality =
        (int32_t)bitset_clear_list(src_1->words, (uint64_t)src_1->cardinality,
                                   src_2->array, (uint64_t)src_2->cardinality);

    if (src_1->cardinality <= DEFAULT_MAX_SIZE) {
        *dst = array_container_from_bitset(src_1);
        bitset_container_free(src_1);
        return false;  // not bitset
    } else
        return true;
}

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst. Result may be either a bitset or an array container
 * (returns "result is bitset"). dst does not initially have
 * any container, but becomes either a bitset container (return
 * result true) or an array container.
 */

bool run_bitset_container_andnot(const run_container_t *src_1,
                                 const bitset_container_t *src_2,
                                 container_t **dst) {
    // follows the Java implementation as of June 2016
    int card = run_container_cardinality(src_1);
    if (card <= DEFAULT_MAX_SIZE) {
        // must be an array
        array_container_t *answer = array_container_create_given_capacity(card);
        answer->cardinality = 0;
        for (int32_t rlepos = 0; rlepos < src_1->n_runs; ++rlepos) {
            rle16_t rle = src_1->runs[rlepos];
            for (int run_value = rle.value; run_value <= rle.value + rle.length;
                 ++run_value) {
                if (!bitset_container_get(src_2, (uint16_t)run_value)) {
                    answer->array[answer->cardinality++] = (uint16_t)run_value;
                }
            }
        }
        *dst = answer;
        return false;
    } else {  // we guess it will be a bitset, though have to check guess when
              // done
        bitset_container_t *answer = bitset_container_clone(src_2);

        uint32_t last_pos = 0;
        for (int32_t rlepos = 0; rlepos < src_1->n_runs; ++rlepos) {
            rle16_t rle = src_1->runs[rlepos];

            uint32_t start = rle.value;
            uint32_t end = start + rle.length + 1;
            bitset_reset_range(answer->words, last_pos, start);
            bitset_flip_range(answer->words, start, end);
            last_pos = end;
        }
        bitset_reset_range(answer->words, last_pos, (uint32_t)(1 << 16));

        answer->cardinality = bitset_container_compute_cardinality(answer);

        if (answer->cardinality <= DEFAULT_MAX_SIZE) {
            *dst = array_container_from_bitset(answer);
            bitset_container_free(answer);
            return false;  // not bitset
        }
        *dst = answer;
        return true;  // bitset
    }
}

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst. Result may be either a bitset or an array container
 * (returns "result is bitset"). dst does not initially have
 * any container, but becomes either a bitset container (return
 * result true) or an array container.
 */

bool run_bitset_container_iandnot(run_container_t *src_1,
                                  const bitset_container_t *src_2,
                                  container_t **dst) {
    // dummy implementation
    bool ans = run_bitset_container_andnot(src_1, src_2, dst);
    run_container_free(src_1);
    return ans;
}

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst. Result may be either a bitset or an array container
 * (returns "result is bitset").  dst does not initially have
 * any container, but becomes either a bitset container (return
 * result true) or an array container.
 */

bool bitset_run_container_andnot(const bitset_container_t *src_1,
                                 const run_container_t *src_2,
                                 container_t **dst) {
    // follows Java implementation
    bitset_container_t *result = bitset_container_create();

    bitset_container_copy(src_1, result);
    for (int32_t rlepos = 0; rlepos < src_2->n_runs; ++rlepos) {
        rle16_t rle = src_2->runs[rlepos];
        bitset_reset_range(result->words, rle.value,
                           rle.value + rle.length + UINT32_C(1));
    }
    result->cardinality = bitset_container_compute_cardinality(result);

    if (result->cardinality <= DEFAULT_MAX_SIZE) {
        *dst = array_container_from_bitset(result);
        bitset_container_free(result);
        return false;  // not bitset
    }
    *dst = result;
    return true;  // bitset
}

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst (which has no container initially).  It will modify src_1
 * to be dst if the result is a bitset.  Otherwise, it will
 * free src_1 and dst will be a new array container.  In both
 * cases, the caller is responsible for deallocating dst.
 * Returns true iff dst is a bitset  */

bool bitset_run_container_iandnot(bitset_container_t *src_1,
                                  const run_container_t *src_2,
                                  container_t **dst) {
    *dst = src_1;

    for (int32_t rlepos = 0; rlepos < src_2->n_runs; ++rlepos) {
        rle16_t rle = src_2->runs[rlepos];
        bitset_reset_range(src_1->words, rle.value,
                           rle.value + rle.length + UINT32_C(1));
    }
    src_1->cardinality = bitset_container_compute_cardinality(src_1);

    if (src_1->cardinality <= DEFAULT_MAX_SIZE) {
        *dst = array_container_from_bitset(src_1);
        bitset_container_free(src_1);
        return false;  // not bitset
    } else
        return true;
}

/* helper. a_out must be a valid array container with adequate capacity.
 * Returns the cardinality of the output container. Partly Based on Java
 * implementation Util.unsignedDifference.
 *
 * TODO: Util.unsignedDifference does not use advanceUntil.  Is it cheaper
 * to avoid advanceUntil?
 */

static int run_array_array_subtract(const run_container_t *rc,
                                    const array_container_t *a_in,
                                    array_container_t *a_out) {
    int out_card = 0;
    int32_t in_array_pos =
        -1;  // since advanceUntil always assumes we start the search AFTER this

    for (int rlepos = 0; rlepos < rc->n_runs; rlepos++) {
        int32_t start = rc->runs[rlepos].value;
        int32_t end = start + rc->runs[rlepos].length + 1;

        in_array_pos = advanceUntil(a_in->array, in_array_pos,
                                    a_in->cardinality, (uint16_t)start);

        if (in_array_pos >= a_in->cardinality) {  // run has no items subtracted
            for (int32_t i = start; i < end; ++i)
                a_out->array[out_card++] = (uint16_t)i;
        } else {
            uint16_t next_nonincluded = a_in->array[in_array_pos];
            if (next_nonincluded >= end) {
                // another case when run goes unaltered
                for (int32_t i = start; i < end; ++i)
                    a_out->array[out_card++] = (uint16_t)i;
                in_array_pos--;  // ensure we see this item again if necessary
            } else {
                for (int32_t i = start; i < end; ++i)
                    if (i != next_nonincluded)
                        a_out->array[out_card++] = (uint16_t)i;
                    else  // 0 should ensure  we don't match
                        next_nonincluded =
                            (in_array_pos + 1 >= a_in->cardinality)
                                ? 0
                                : a_in->array[++in_array_pos];
                in_array_pos--;  // see again
            }
        }
    }
    return out_card;
}

/* dst does not indicate a valid container initially.  Eventually it
 * can become any type of container.
 */

int run_array_container_andnot(const run_container_t *src_1,
                               const array_container_t *src_2,
                               container_t **dst) {
    // follows the Java impl as of June 2016

    int card = run_container_cardinality(src_1);
    const int arbitrary_threshold = 32;

    if (card <= arbitrary_threshold) {
        if (src_2->cardinality == 0) {
            *dst = run_container_clone(src_1);
            return RUN_CONTAINER_TYPE;
        }
        // Java's "lazyandNot.toEfficientContainer" thing
        run_container_t *answer = run_container_create_given_capacity(
            card + array_container_cardinality(src_2));

        int rlepos = 0;
        int xrlepos = 0;  // "x" is src_2
        rle16_t rle = src_1->runs[rlepos];
        int32_t start = rle.value;
        int32_t end = start + rle.length + 1;
        int32_t xstart = src_2->array[xrlepos];

        while ((rlepos < src_1->n_runs) && (xrlepos < src_2->cardinality)) {
            if (end <= xstart) {
                // output the first run
                answer->runs[answer->n_runs++] =
                    CROARING_MAKE_RLE16(start, end - start - 1);
                rlepos++;
                if (rlepos < src_1->n_runs) {
                    start = src_1->runs[rlepos].value;
                    end = start + src_1->runs[rlepos].length + 1;
                }
            } else if (xstart + 1 <= start) {
                // exit the second run
                xrlepos++;
                if (xrlepos < src_2->cardinality) {
                    xstart = src_2->array[xrlepos];
                }
            } else {
                if (start < xstart) {
                    answer->runs[answer->n_runs++] =
                        CROARING_MAKE_RLE16(start, xstart - start - 1);
                }
                if (xstart + 1 < end) {
                    start = xstart + 1;
                } else {
                    rlepos++;
                    if (rlepos < src_1->n_runs) {
                        start = src_1->runs[rlepos].value;
                        end = start + src_1->runs[rlepos].length + 1;
                    }
                }
            }
        }
        if (rlepos < src_1->n_runs) {
            answer->runs[answer->n_runs++] =
                CROARING_MAKE_RLE16(start, end - start - 1);
            rlepos++;
            if (rlepos < src_1->n_runs) {
                memcpy(answer->runs + answer->n_runs, src_1->runs + rlepos,
                       (src_1->n_runs - rlepos) * sizeof(rle16_t));
                answer->n_runs += (src_1->n_runs - rlepos);
            }
        }
        uint8_t return_type;
        *dst = convert_run_to_efficient_container(answer, &return_type);
        if (answer != *dst) run_container_free(answer);
        return return_type;
    }
    // else it's a bitmap or array

    if (card <= DEFAULT_MAX_SIZE) {
        array_container_t *ac = array_container_create_given_capacity(card);
        // nb Java code used a generic iterator-based merge to compute
        // difference
        ac->cardinality = run_array_array_subtract(src_1, src_2, ac);
        *dst = ac;
        return ARRAY_CONTAINER_TYPE;
    }
    bitset_container_t *ans = bitset_container_from_run(src_1);
    bool result_is_bitset = bitset_array_container_iandnot(ans, src_2, dst);
    return (result_is_bitset ? BITSET_CONTAINER_TYPE : ARRAY_CONTAINER_TYPE);
}

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst (which has no container initially).  It will modify src_1
 * to be dst if the result is a bitset.  Otherwise, it will
 * free src_1 and dst will be a new array container.  In both
 * cases, the caller is responsible for deallocating dst.
 * Returns true iff dst is a bitset  */

int run_array_container_iandnot(run_container_t *src_1,
                                const array_container_t *src_2,
                                container_t **dst) {
    // dummy implementation same as June 2016 Java
    int ans = run_array_container_andnot(src_1, src_2, dst);
    run_container_free(src_1);
    return ans;
}

/* dst must be a valid array container, allowed to be src_1 */

void array_run_container_andnot(const array_container_t *src_1,
                                const run_container_t *src_2,
                                array_container_t *dst) {
    // basically following Java impl as of June 2016
    if (src_1->cardinality > dst->capacity) {
        array_container_grow(dst, src_1->cardinality, false);
    }

    if (src_2->n_runs == 0) {
        memmove(dst->array, src_1->array,
                sizeof(uint16_t) * src_1->cardinality);
        dst->cardinality = src_1->cardinality;
        return;
    }
    int32_t run_start = src_2->runs[0].value;
    int32_t run_end = run_start + src_2->runs[0].length;
    int which_run = 0;

    uint16_t val = 0;
    int dest_card = 0;
    for (int i = 0; i < src_1->cardinality; ++i) {
        val = src_1->array[i];
        if (val < run_start)
            dst->array[dest_card++] = val;
        else if (val <= run_end) {
            ;  // omitted item
        } else {
            do {
                if (which_run + 1 < src_2->n_runs) {
                    ++which_run;
                    run_start = src_2->runs[which_run].value;
                    run_end = run_start + src_2->runs[which_run].length;

                } else
                    run_start = run_end = (1 << 16) + 1;
            } while (val > run_end);
            --i;
        }
    }
    dst->cardinality = dest_card;
}

/* dst does not indicate a valid container initially.  Eventually it
 * can become any kind of container.
 */

void array_run_container_iandnot(array_container_t *src_1,
                                 const run_container_t *src_2) {
    array_run_container_andnot(src_1, src_2, src_1);
}

/* dst does not indicate a valid container initially.  Eventually it
 * can become any kind of container.
 */

int run_run_container_andnot(const run_container_t *src_1,
                             const run_container_t *src_2, container_t **dst) {
    run_container_t *ans = run_container_create();
    run_container_andnot(src_1, src_2, ans);
    uint8_t typecode_after;
    *dst = convert_run_to_efficient_container_and_free(ans, &typecode_after);
    return typecode_after;
}

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst (which has no container initially).  It will modify src_1
 * to be dst if the result is a bitset.  Otherwise, it will
 * free src_1 and dst will be a new array container.  In both
 * cases, the caller is responsible for deallocating dst.
 * Returns true iff dst is a bitset  */

int run_run_container_iandnot(run_container_t *src_1,
                              const run_container_t *src_2, container_t **dst) {
    // following Java impl as of June 2016 (dummy)
    int ans = run_run_container_andnot(src_1, src_2, dst);
    run_container_free(src_1);
    return ans;
}

/*
 * dst is a valid array container and may be the same as src_1
 */

void array_array_container_andnot(const array_container_t *src_1,
                                  const array_container_t *src_2,
                                  array_container_t *dst) {
    array_container_andnot(src_1, src_2, dst);
}

/* inplace array-array andnot will always be able to reuse the space of
 * src_1 */
void array_array_container_iandnot(array_container_t *src_1,
                                   const array_container_t *src_2) {
    array_container_andnot(src_1, src_2, src_1);
}

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst (which has no container initially). Return value is
 * "dst is a bitset"
 */

bool bitset_bitset_container_andnot(const bitset_container_t *src_1,
                                    const bitset_container_t *src_2,
                                    container_t **dst) {
    bitset_container_t *ans = bitset_container_create();
    int card = bitset_container_andnot(src_1, src_2, ans);
    if (card <= DEFAULT_MAX_SIZE) {
        *dst = array_container_from_bitset(ans);
        bitset_container_free(ans);
        return false;  // not bitset
    } else {
        *dst = ans;
        return true;
    }
}

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst (which has no container initially).  It will modify src_1
 * to be dst if the result is a bitset.  Otherwise, it will
 * free src_1 and dst will be a new array container.  In both
 * cases, the caller is responsible for deallocating dst.
 * Returns true iff dst is a bitset  */

bool bitset_bitset_container_iandnot(bitset_container_t *src_1,
                                     const bitset_container_t *src_2,
                                     container_t **dst) {
    int card = bitset_container_andnot(src_1, src_2, src_1);
    if (card <= DEFAULT_MAX_SIZE) {
        *dst = array_container_from_bitset(src_1);
        bitset_container_free(src_1);
        return false;  // not bitset
    } else {
        *dst = src_1;
        return true;
    }
}

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

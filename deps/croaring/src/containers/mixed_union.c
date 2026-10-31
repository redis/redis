/*
 * mixed_union.c
 *
 */

#include <assert.h>
#include <string.h>

#include <roaring/bitset_util.h>
#include <roaring/containers/convert.h>
#include <roaring/containers/mixed_union.h>
#include <roaring/containers/perfparameters.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

/* Compute the union of src_1 and src_2 and write the result to
 * dst.  */
void array_bitset_container_union(const array_container_t *src_1,
                                  const bitset_container_t *src_2,
                                  bitset_container_t *dst) {
    if (src_2 != dst) bitset_container_copy(src_2, dst);
    dst->cardinality = (int32_t)bitset_set_list_withcard(
        dst->words, dst->cardinality, src_1->array, src_1->cardinality);
}

/* Compute the union of src_1 and src_2 and write the result to
 * dst. It is allowed for src_2 to be dst.  This version does not
 * update the cardinality of dst (it is set to BITSET_UNKNOWN_CARDINALITY). */
void array_bitset_container_lazy_union(const array_container_t *src_1,
                                       const bitset_container_t *src_2,
                                       bitset_container_t *dst) {
    if (src_2 != dst) bitset_container_copy(src_2, dst);
    bitset_set_list(dst->words, src_1->array, src_1->cardinality);
    dst->cardinality = BITSET_UNKNOWN_CARDINALITY;
}

void run_bitset_container_union(const run_container_t *src_1,
                                const bitset_container_t *src_2,
                                bitset_container_t *dst) {
    assert(!run_container_is_full(src_1));  // catch this case upstream
    if (src_2 != dst) bitset_container_copy(src_2, dst);
    for (int32_t rlepos = 0; rlepos < src_1->n_runs; ++rlepos) {
        rle16_t rle = src_1->runs[rlepos];
        bitset_set_lenrange(dst->words, rle.value, rle.length);
    }
    dst->cardinality = bitset_container_compute_cardinality(dst);
}

void run_bitset_container_lazy_union(const run_container_t *src_1,
                                     const bitset_container_t *src_2,
                                     bitset_container_t *dst) {
    assert(!run_container_is_full(src_1));  // catch this case upstream
    if (src_2 != dst) bitset_container_copy(src_2, dst);
    for (int32_t rlepos = 0; rlepos < src_1->n_runs; ++rlepos) {
        rle16_t rle = src_1->runs[rlepos];
        bitset_set_lenrange(dst->words, rle.value, rle.length);
    }
    dst->cardinality = BITSET_UNKNOWN_CARDINALITY;
}

// why do we leave the result as a run container??
void array_run_container_union(const array_container_t *src_1,
                               const run_container_t *src_2,
                               run_container_t *dst) {
    if (run_container_is_full(src_2)) {
        run_container_copy(src_2, dst);
        return;
    }
    // TODO: see whether the "2*" is spurious
    run_container_grow(dst, 2 * (src_1->cardinality + src_2->n_runs), false);
    int32_t rlepos = 0;
    int32_t arraypos = 0;
    rle16_t previousrle;
    if (src_2->runs[rlepos].value <= src_1->array[arraypos]) {
        previousrle = run_container_append_first(dst, src_2->runs[rlepos]);
        rlepos++;
    } else {
        previousrle =
            run_container_append_value_first(dst, src_1->array[arraypos]);
        arraypos++;
    }
    while ((rlepos < src_2->n_runs) && (arraypos < src_1->cardinality)) {
        if (src_2->runs[rlepos].value <= src_1->array[arraypos]) {
            run_container_append(dst, src_2->runs[rlepos], &previousrle);
            rlepos++;
        } else {
            run_container_append_value(dst, src_1->array[arraypos],
                                       &previousrle);
            arraypos++;
        }
    }
    if (arraypos < src_1->cardinality) {
        while (arraypos < src_1->cardinality) {
            run_container_append_value(dst, src_1->array[arraypos],
                                       &previousrle);
            arraypos++;
        }
    } else {
        while (rlepos < src_2->n_runs) {
            run_container_append(dst, src_2->runs[rlepos], &previousrle);
            rlepos++;
        }
    }
}

void array_run_container_inplace_union(const array_container_t *src_1,
                                       run_container_t *src_2) {
    if (run_container_is_full(src_2)) {
        return;
    }
    const int32_t maxoutput = src_1->cardinality + src_2->n_runs;
    const int32_t neededcapacity = maxoutput + src_2->n_runs;
    if (src_2->capacity < neededcapacity)
        run_container_grow(src_2, neededcapacity, true);
    memmove(src_2->runs + maxoutput, src_2->runs,
            src_2->n_runs * sizeof(rle16_t));
    rle16_t *inputsrc2 = src_2->runs + maxoutput;
    int32_t rlepos = 0;
    int32_t arraypos = 0;
    int src2nruns = src_2->n_runs;
    src_2->n_runs = 0;

    rle16_t previousrle;

    if (inputsrc2[rlepos].value <= src_1->array[arraypos]) {
        previousrle = run_container_append_first(src_2, inputsrc2[rlepos]);
        rlepos++;
    } else {
        previousrle =
            run_container_append_value_first(src_2, src_1->array[arraypos]);
        arraypos++;
    }

    while ((rlepos < src2nruns) && (arraypos < src_1->cardinality)) {
        if (inputsrc2[rlepos].value <= src_1->array[arraypos]) {
            run_container_append(src_2, inputsrc2[rlepos], &previousrle);
            rlepos++;
        } else {
            run_container_append_value(src_2, src_1->array[arraypos],
                                       &previousrle);
            arraypos++;
        }
    }
    if (arraypos < src_1->cardinality) {
        while (arraypos < src_1->cardinality) {
            run_container_append_value(src_2, src_1->array[arraypos],
                                       &previousrle);
            arraypos++;
        }
    } else {
        while (rlepos < src2nruns) {
            run_container_append(src_2, inputsrc2[rlepos], &previousrle);
            rlepos++;
        }
    }
}

bool array_array_container_union(const array_container_t *src_1,
                                 const array_container_t *src_2,
                                 container_t **dst) {
    int totalCardinality = src_1->cardinality + src_2->cardinality;
    if (totalCardinality <= DEFAULT_MAX_SIZE) {
        *dst = array_container_create_given_capacity(totalCardinality);
        if (*dst != NULL) {
            array_container_union(src_1, src_2, CAST_array(*dst));
        } else {
            return true;  // otherwise failure won't be caught
        }
        return false;  // not a bitset
    }
    *dst = bitset_container_create();
    bool returnval = true;  // expect a bitset
    if (*dst != NULL) {
        bitset_container_t *ourbitset = CAST_bitset(*dst);
        bitset_set_list(ourbitset->words, src_1->array, src_1->cardinality);
        ourbitset->cardinality = (int32_t)bitset_set_list_withcard(
            ourbitset->words, src_1->cardinality, src_2->array,
            src_2->cardinality);
        if (ourbitset->cardinality <= DEFAULT_MAX_SIZE) {
            // need to convert!
            *dst = array_container_from_bitset(ourbitset);
            bitset_container_free(ourbitset);
            returnval = false;  // not going to be a bitset
        }
    }
    return returnval;
}

bool array_array_container_inplace_union(array_container_t *src_1,
                                         const array_container_t *src_2,
                                         container_t **dst) {
    int totalCardinality = src_1->cardinality + src_2->cardinality;
    *dst = NULL;
    if (totalCardinality <= DEFAULT_MAX_SIZE) {
        if (src_1->capacity < totalCardinality) {
            *dst = array_container_create_given_capacity(
                2 * totalCardinality);  // be purposefully generous
            if (*dst != NULL) {
                array_container_union(src_1, src_2, CAST_array(*dst));
            } else {
                return true;  // otherwise failure won't be caught
            }
            return false;  // not a bitset
        } else {
            memmove(src_1->array + src_2->cardinality, src_1->array,
                    src_1->cardinality * sizeof(uint16_t));
            // In theory, we could use fast_union_uint16, but it is unsafe. It
            // fails with Intel compilers in particular.
            // https://github.com/RoaringBitmap/CRoaring/pull/452
            // See report https://github.com/RoaringBitmap/CRoaring/issues/476
            src_1->cardinality = (int32_t)union_uint16(
                src_1->array + src_2->cardinality, src_1->cardinality,
                src_2->array, src_2->cardinality, src_1->array);
            return false;  // not a bitset
        }
    }
    *dst = bitset_container_create();
    bool returnval = true;  // expect a bitset
    if (*dst != NULL) {
        bitset_container_t *ourbitset = CAST_bitset(*dst);
        bitset_set_list(ourbitset->words, src_1->array, src_1->cardinality);
        ourbitset->cardinality = (int32_t)bitset_set_list_withcard(
            ourbitset->words, src_1->cardinality, src_2->array,
            src_2->cardinality);
        if (ourbitset->cardinality <= DEFAULT_MAX_SIZE) {
            // need to convert!
            if (src_1->capacity < ourbitset->cardinality) {
                array_container_grow(src_1, ourbitset->cardinality, false);
            }

            bitset_extract_setbits_uint16(ourbitset->words,
                                          BITSET_CONTAINER_SIZE_IN_WORDS,
                                          src_1->array, 0);
            src_1->cardinality = ourbitset->cardinality;
            *dst = src_1;
            bitset_container_free(ourbitset);
            returnval = false;  // not going to be a bitset
        }
    }
    return returnval;
}

bool array_array_container_lazy_union(const array_container_t *src_1,
                                      const array_container_t *src_2,
                                      container_t **dst) {
    int totalCardinality = src_1->cardinality + src_2->cardinality;
    //
    // We assume that operations involving bitset containers will be faster than
    // operations involving solely array containers, except maybe when array
    // containers are small. Indeed, for example, it is cheap to compute the
    // union between an array and a bitset container, generally more so than
    // between a large array and another array. So it is advantageous to favour
    // bitset containers during the computation. Of course, if we convert array
    // containers eagerly to bitset containers, we may later need to revert the
    // bitset containers to array containerr to satisfy the Roaring format
    // requirements, but such one-time conversions at the end may not be overly
    // expensive. We arrived to this design based on extensive benchmarking.
    //
    if (totalCardinality <= ARRAY_LAZY_LOWERBOUND) {
        *dst = array_container_create_given_capacity(totalCardinality);
        if (*dst != NULL) {
            array_container_union(src_1, src_2, CAST_array(*dst));
        } else {
            return true;  // otherwise failure won't be caught
        }
        return false;  // not a bitset
    }
    *dst = bitset_container_create();
    bool returnval = true;  // expect a bitset
    if (*dst != NULL) {
        bitset_container_t *ourbitset = CAST_bitset(*dst);
        bitset_set_list(ourbitset->words, src_1->array, src_1->cardinality);
        bitset_set_list(ourbitset->words, src_2->array, src_2->cardinality);
        ourbitset->cardinality = BITSET_UNKNOWN_CARDINALITY;
    }
    return returnval;
}

bool array_array_container_lazy_inplace_union(array_container_t *src_1,
                                              const array_container_t *src_2,
                                              container_t **dst) {
    int totalCardinality = src_1->cardinality + src_2->cardinality;
    *dst = NULL;
    //
    // We assume that operations involving bitset containers will be faster than
    // operations involving solely array containers, except maybe when array
    // containers are small. Indeed, for example, it is cheap to compute the
    // union between an array and a bitset container, generally more so than
    // between a large array and another array. So it is advantageous to favour
    // bitset containers during the computation. Of course, if we convert array
    // containers eagerly to bitset containers, we may later need to revert the
    // bitset containers to array containerr to satisfy the Roaring format
    // requirements, but such one-time conversions at the end may not be overly
    // expensive. We arrived to this design based on extensive benchmarking.
    //
    if (totalCardinality <= ARRAY_LAZY_LOWERBOUND) {
        if (src_1->capacity < totalCardinality) {
            *dst = array_container_create_given_capacity(
                2 * totalCardinality);  // be purposefully generous
            if (*dst != NULL) {
                array_container_union(src_1, src_2, CAST_array(*dst));
            } else {
                return true;  // otherwise failure won't be caught
            }
            return false;  // not a bitset
        } else {
            memmove(src_1->array + src_2->cardinality, src_1->array,
                    src_1->cardinality * sizeof(uint16_t));
            /*
              Next line is safe:

              We just need to focus on the reading and writing performed on
              array1. In `union_vector16`, both vectorized and scalar code still
              obey the basic rule: read from two inputs, do the union, and then
              write the output.

              Let's say the length(cardinality) of input2 is L2:
              ```
                  |<-  L2  ->|
              array1: [output--- |input 1---|---]
              array2: [input 2---]
              ```
              Let's define 3 __m128i pointers, `pos1` starts from `input1`,
              `pos2` starts from `input2`, these 2 point at the next byte to
              read, `out` starts from `output`, pointing at the next byte to
              overwrite.
              ```
              array1: [output--- |input 1---|---]
                          ^          ^
                      out        pos1
              array2: [input 2---]
                          ^
                          pos2
              ```
              The union output always contains less or equal number of elements
              than all inputs added, so we have:
              ```
              out <= pos1 + pos2
              ```
              therefore:
              ```
              out <= pos1 + L2
              ```
              which means you will not overwrite data beyond pos1, so the data
              haven't read is safe, and we don't care the data already read.
            */
            src_1->cardinality = (int32_t)fast_union_uint16(
                src_1->array + src_2->cardinality, src_1->cardinality,
                src_2->array, src_2->cardinality, src_1->array);
            return false;  // not a bitset
        }
    }
    *dst = bitset_container_create();
    bool returnval = true;  // expect a bitset
    if (*dst != NULL) {
        bitset_container_t *ourbitset = CAST_bitset(*dst);
        bitset_set_list(ourbitset->words, src_1->array, src_1->cardinality);
        bitset_set_list(ourbitset->words, src_2->array, src_2->cardinality);
        ourbitset->cardinality = BITSET_UNKNOWN_CARDINALITY;
    }
    return returnval;
}

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

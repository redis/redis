/*
 * mixed_xor.c
 */

#include <assert.h>
#include <string.h>

#include <roaring/bitset_util.h>
#include <roaring/containers/containers.h>
#include <roaring/containers/convert.h>
#include <roaring/containers/mixed_xor.h>
#include <roaring/containers/perfparameters.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

/* Compute the xor of src_1 and src_2 and write the result to
 * dst (which has no container initially).
 * Result is true iff dst is a bitset  */
bool array_bitset_container_xor(const array_container_t *src_1,
                                const bitset_container_t *src_2,
                                container_t **dst) {
    bitset_container_t *result = bitset_container_create();
    bitset_container_copy(src_2, result);
    result->cardinality = (int32_t)bitset_flip_list_withcard(
        result->words, result->cardinality, src_1->array, src_1->cardinality);

    // do required type conversions.
    if (result->cardinality <= DEFAULT_MAX_SIZE) {
        *dst = array_container_from_bitset(result);
        bitset_container_free(result);
        return false;  // not bitset
    }
    *dst = result;
    return true;  // bitset
}

/* Compute the xor of src_1 and src_2 and write the result to
 * dst. It is allowed for src_2 to be dst.  This version does not
 * update the cardinality of dst (it is set to BITSET_UNKNOWN_CARDINALITY).
 */

void array_bitset_container_lazy_xor(const array_container_t *src_1,
                                     const bitset_container_t *src_2,
                                     bitset_container_t *dst) {
    if (src_2 != dst) bitset_container_copy(src_2, dst);
    bitset_flip_list(dst->words, src_1->array, src_1->cardinality);
    dst->cardinality = BITSET_UNKNOWN_CARDINALITY;
}

/* Compute the xor of src_1 and src_2 and write the result to
 * dst. Result may be either a bitset or an array container
 * (returns "result is bitset"). dst does not initially have
 * any container, but becomes either a bitset container (return
 * result true) or an array container.
 */

bool run_bitset_container_xor(const run_container_t *src_1,
                              const bitset_container_t *src_2,
                              container_t **dst) {
    bitset_container_t *result = bitset_container_create();

    bitset_container_copy(src_2, result);
    for (int32_t rlepos = 0; rlepos < src_1->n_runs; ++rlepos) {
        rle16_t rle = src_1->runs[rlepos];
        bitset_flip_range(result->words, rle.value,
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

/* lazy xor.  Dst is initialized and may be equal to src_2.
 *  Result is left as a bitset container, even if actual
 *  cardinality would dictate an array container.
 */

void run_bitset_container_lazy_xor(const run_container_t *src_1,
                                   const bitset_container_t *src_2,
                                   bitset_container_t *dst) {
    if (src_2 != dst) bitset_container_copy(src_2, dst);
    for (int32_t rlepos = 0; rlepos < src_1->n_runs; ++rlepos) {
        rle16_t rle = src_1->runs[rlepos];
        bitset_flip_range(dst->words, rle.value,
                          rle.value + rle.length + UINT32_C(1));
    }
    dst->cardinality = BITSET_UNKNOWN_CARDINALITY;
}

/* dst does not indicate a valid container initially.  Eventually it
 * can become any kind of container.
 */

int array_run_container_xor(const array_container_t *src_1,
                            const run_container_t *src_2, container_t **dst) {
    // semi following Java XOR implementation as of May 2016
    // the C OR implementation works quite differently and can return a run
    // container
    // TODO could optimize for full run containers.

    // use of lazy following Java impl.
    const int arbitrary_threshold = 32;
    if (src_1->cardinality < arbitrary_threshold) {
        run_container_t *ans = run_container_create();
        array_run_container_lazy_xor(src_1, src_2, ans);  // keeps runs.
        uint8_t typecode_after;
        *dst =
            convert_run_to_efficient_container_and_free(ans, &typecode_after);
        return typecode_after;
    }

    int card = run_container_cardinality(src_2);
    if (card <= DEFAULT_MAX_SIZE) {
        // Java implementation works with the array, xoring the run elements via
        // iterator
        array_container_t *temp = array_container_from_run(src_2);
        bool ret_is_bitset = array_array_container_xor(temp, src_1, dst);
        array_container_free(temp);
        return ret_is_bitset ? BITSET_CONTAINER_TYPE : ARRAY_CONTAINER_TYPE;

    } else {  // guess that it will end up as a bitset
        bitset_container_t *result = bitset_container_from_run(src_2);
        bool is_bitset = bitset_array_container_ixor(result, src_1, dst);
        // any necessary type conversion has been done by the ixor
        int retval = (is_bitset ? BITSET_CONTAINER_TYPE : ARRAY_CONTAINER_TYPE);
        return retval;
    }
}

/* Dst is a valid run container. (Can it be src_2? Let's say not.)
 * Leaves result as run container, even if other options are
 * smaller.
 */

void array_run_container_lazy_xor(const array_container_t *src_1,
                                  const run_container_t *src_2,
                                  run_container_t *dst) {
    run_container_grow(dst, src_1->cardinality + src_2->n_runs, false);
    int32_t rlepos = 0;
    int32_t arraypos = 0;
    dst->n_runs = 0;

    while ((rlepos < src_2->n_runs) && (arraypos < src_1->cardinality)) {
        if (src_2->runs[rlepos].value <= src_1->array[arraypos]) {
            run_container_smart_append_exclusive(dst, src_2->runs[rlepos].value,
                                                 src_2->runs[rlepos].length);
            rlepos++;
        } else {
            run_container_smart_append_exclusive(dst, src_1->array[arraypos],
                                                 0);
            arraypos++;
        }
    }
    while (arraypos < src_1->cardinality) {
        run_container_smart_append_exclusive(dst, src_1->array[arraypos], 0);
        arraypos++;
    }
    while (rlepos < src_2->n_runs) {
        run_container_smart_append_exclusive(dst, src_2->runs[rlepos].value,
                                             src_2->runs[rlepos].length);
        rlepos++;
    }
}

/* dst does not indicate a valid container initially.  Eventually it
 * can become any kind of container.
 */

int run_run_container_xor(const run_container_t *src_1,
                          const run_container_t *src_2, container_t **dst) {
    run_container_t *ans = run_container_create();
    run_container_xor(src_1, src_2, ans);
    uint8_t typecode_after;
    *dst = convert_run_to_efficient_container_and_free(ans, &typecode_after);
    return typecode_after;
}

/*
 * Java implementation (as of May 2016) for array_run, run_run
 * and  bitset_run don't do anything different for inplace.
 * Could adopt the mixed_union.c approach instead (ie, using
 * smart_append_exclusive)
 *
 */

bool array_array_container_xor(const array_container_t *src_1,
                               const array_container_t *src_2,
                               container_t **dst) {
    int totalCardinality =
        src_1->cardinality + src_2->cardinality;  // upper bound
    if (totalCardinality <= DEFAULT_MAX_SIZE) {
        *dst = array_container_create_given_capacity(totalCardinality);
        array_container_xor(src_1, src_2, CAST_array(*dst));
        return false;  // not a bitset
    }
    *dst = bitset_container_from_array(src_1);
    bool returnval = true;  // expect a bitset
    bitset_container_t *ourbitset = CAST_bitset(*dst);
    ourbitset->cardinality = (uint32_t)bitset_flip_list_withcard(
        ourbitset->words, src_1->cardinality, src_2->array, src_2->cardinality);
    if (ourbitset->cardinality <= DEFAULT_MAX_SIZE) {
        // need to convert!
        *dst = array_container_from_bitset(ourbitset);
        bitset_container_free(ourbitset);
        returnval = false;  // not going to be a bitset
    }

    return returnval;
}

bool array_array_container_lazy_xor(const array_container_t *src_1,
                                    const array_container_t *src_2,
                                    container_t **dst) {
    int totalCardinality = src_1->cardinality + src_2->cardinality;
    //
    // We assume that operations involving bitset containers will be faster than
    // operations involving solely array containers, except maybe when array
    // containers are small. Indeed, for example, it is cheap to compute the
    // exclusive union between an array and a bitset container, generally more
    // so than between a large array and another array. So it is advantageous to
    // favour bitset containers during the computation. Of course, if we convert
    // array containers eagerly to bitset containers, we may later need to
    // revert the bitset containers to array containerr to satisfy the Roaring
    // format requirements, but such one-time conversions at the end may not be
    // overly expensive. We arrived to this design based on extensive
    // benchmarking on unions. For XOR/exclusive union, we simply followed the
    // heuristic used by the unions (see  mixed_union.c). Further tuning is
    // possible.
    //
    if (totalCardinality <= ARRAY_LAZY_LOWERBOUND) {
        *dst = array_container_create_given_capacity(totalCardinality);
        if (*dst != NULL) array_container_xor(src_1, src_2, CAST_array(*dst));
        return false;  // not a bitset
    }
    *dst = bitset_container_from_array(src_1);
    bool returnval = true;  // expect a bitset (maybe, for XOR??)
    if (*dst != NULL) {
        bitset_container_t *ourbitset = CAST_bitset(*dst);
        bitset_flip_list(ourbitset->words, src_2->array, src_2->cardinality);
        ourbitset->cardinality = BITSET_UNKNOWN_CARDINALITY;
    }
    return returnval;
}

/* Compute the xor of src_1 and src_2 and write the result to
 * dst (which has no container initially). Return value is
 * "dst is a bitset"
 */

bool bitset_bitset_container_xor(const bitset_container_t *src_1,
                                 const bitset_container_t *src_2,
                                 container_t **dst) {
    bitset_container_t *ans = bitset_container_create();
    int card = bitset_container_xor(src_1, src_2, ans);
    if (card <= DEFAULT_MAX_SIZE) {
        *dst = array_container_from_bitset(ans);
        bitset_container_free(ans);
        return false;  // not bitset
    } else {
        *dst = ans;
        return true;
    }
}

/* Compute the xor of src_1 and src_2 and write the result to
 * dst (which has no container initially).  It will modify src_1
 * to be dst if the result is a bitset.  Otherwise, it will
 * free src_1 and dst will be a new array container.  In both
 * cases, the caller is responsible for deallocating dst.
 * Returns true iff dst is a bitset  */

bool bitset_array_container_ixor(bitset_container_t *src_1,
                                 const array_container_t *src_2,
                                 container_t **dst) {
    *dst = src_1;
    src_1->cardinality = (uint32_t)bitset_flip_list_withcard(
        src_1->words, src_1->cardinality, src_2->array, src_2->cardinality);

    if (src_1->cardinality <= DEFAULT_MAX_SIZE) {
        *dst = array_container_from_bitset(src_1);
        bitset_container_free(src_1);
        return false;  // not bitset
    } else
        return true;
}

/* a bunch of in-place, some of which may not *really* be inplace.
 * TODO: write actual inplace routine if efficiency warrants it
 * Anything inplace with a bitset is a good candidate
 */

bool bitset_bitset_container_ixor(bitset_container_t *src_1,
                                  const bitset_container_t *src_2,
                                  container_t **dst) {
    int card = bitset_container_xor(src_1, src_2, src_1);
    if (card <= DEFAULT_MAX_SIZE) {
        *dst = array_container_from_bitset(src_1);
        bitset_container_free(src_1);
        return false;  // not bitset
    } else {
        *dst = src_1;
        return true;
    }
}

bool array_bitset_container_ixor(array_container_t *src_1,
                                 const bitset_container_t *src_2,
                                 container_t **dst) {
    bool ans = array_bitset_container_xor(src_1, src_2, dst);
    array_container_free(src_1);
    return ans;
}

/* Compute the xor of src_1 and src_2 and write the result to
 * dst. Result may be either a bitset or an array container
 * (returns "result is bitset"). dst does not initially have
 * any container, but becomes either a bitset container (return
 * result true) or an array container.
 */

bool run_bitset_container_ixor(run_container_t *src_1,
                               const bitset_container_t *src_2,
                               container_t **dst) {
    bool ans = run_bitset_container_xor(src_1, src_2, dst);
    run_container_free(src_1);
    return ans;
}

bool bitset_run_container_ixor(bitset_container_t *src_1,
                               const run_container_t *src_2,
                               container_t **dst) {
    bool ans = run_bitset_container_xor(src_2, src_1, dst);
    bitset_container_free(src_1);
    return ans;
}

/* dst does not indicate a valid container initially.  Eventually it
 * can become any kind of container.
 */

int array_run_container_ixor(array_container_t *src_1,
                             const run_container_t *src_2, container_t **dst) {
    int ans = array_run_container_xor(src_1, src_2, dst);
    array_container_free(src_1);
    return ans;
}

int run_array_container_ixor(run_container_t *src_1,
                             const array_container_t *src_2,
                             container_t **dst) {
    int ans = array_run_container_xor(src_2, src_1, dst);
    run_container_free(src_1);
    return ans;
}

bool array_array_container_ixor(array_container_t *src_1,
                                const array_container_t *src_2,
                                container_t **dst) {
    bool ans = array_array_container_xor(src_1, src_2, dst);
    array_container_free(src_1);
    return ans;
}

int run_run_container_ixor(run_container_t *src_1, const run_container_t *src_2,
                           container_t **dst) {
    int ans = run_run_container_xor(src_1, src_2, dst);
    run_container_free(src_1);
    return ans;
}

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

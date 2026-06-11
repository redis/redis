#include <stdio.h>

#include <roaring/bitset_util.h>
#include <roaring/containers/containers.h>
#include <roaring/containers/convert.h>
#include <roaring/containers/perfparameters.h>

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

// file contains grubby stuff that must know impl. details of all container
// types.
bitset_container_t *bitset_container_from_array(const array_container_t *ac) {
    bitset_container_t *ans = bitset_container_create();
    int limit = array_container_cardinality(ac);
    for (int i = 0; i < limit; ++i) bitset_container_set(ans, ac->array[i]);
    return ans;
}

bitset_container_t *bitset_container_from_run(const run_container_t *arr) {
    int card = run_container_cardinality(arr);
    bitset_container_t *answer = bitset_container_create();
    for (int rlepos = 0; rlepos < arr->n_runs; ++rlepos) {
        rle16_t vl = arr->runs[rlepos];
        bitset_set_lenrange(answer->words, vl.value, vl.length);
    }
    answer->cardinality = card;
    return answer;
}

array_container_t *array_container_from_run(const run_container_t *arr) {
    array_container_t *answer =
        array_container_create_given_capacity(run_container_cardinality(arr));
    answer->cardinality = 0;
    for (int rlepos = 0; rlepos < arr->n_runs; ++rlepos) {
        int run_start = arr->runs[rlepos].value;
        int run_end = run_start + arr->runs[rlepos].length;

        for (int run_value = run_start; run_value <= run_end; ++run_value) {
            answer->array[answer->cardinality++] = (uint16_t)run_value;
        }
    }
    return answer;
}

array_container_t *array_container_from_bitset(const bitset_container_t *bits) {
    array_container_t *result =
        array_container_create_given_capacity(bits->cardinality);
    result->cardinality = bits->cardinality;
#if CROARING_IS_X64
#if CROARING_COMPILER_SUPPORTS_AVX512
    if (croaring_hardware_support() & ROARING_SUPPORTS_AVX512) {
        bitset_extract_setbits_avx512_uint16(
            bits->words, BITSET_CONTAINER_SIZE_IN_WORDS, result->array,
            bits->cardinality, 0);
    } else
#endif
    {
        //  sse version ends up being slower here
        // (bitset_extract_setbits_sse_uint16)
        // because of the sparsity of the data
        bitset_extract_setbits_uint16(
            bits->words, BITSET_CONTAINER_SIZE_IN_WORDS, result->array, 0);
    }
#else
    // If the system is not x64, then we have no accelerated function.
    bitset_extract_setbits_uint16(bits->words, BITSET_CONTAINER_SIZE_IN_WORDS,
                                  result->array, 0);
#endif

    return result;
}

/* assumes that container has adequate space.  Run from [s,e] (inclusive) */
static void add_run(run_container_t *rc, int s, int e) {
    rc->runs[rc->n_runs].value = s;
    rc->runs[rc->n_runs].length = e - s;
    rc->n_runs++;
}

run_container_t *run_container_from_array(const array_container_t *c) {
    int32_t n_runs = array_container_number_of_runs(c);
    run_container_t *answer = run_container_create_given_capacity(n_runs);
    int prev = -2;
    int run_start = -1;
    int32_t card = c->cardinality;
    if (card == 0) return answer;
    for (int i = 0; i < card; ++i) {
        const uint16_t cur_val = c->array[i];
        if (cur_val != prev + 1) {
            // new run starts; flush old one, if any
            if (run_start != -1) add_run(answer, run_start, prev);
            run_start = cur_val;
        }
        prev = c->array[i];
    }
    // now prev is the last seen value
    add_run(answer, run_start, prev);
    // assert(run_container_cardinality(answer) == c->cardinality);
    return answer;
}

/**
 * Convert the runcontainer to either a Bitmap or an Array Container, depending
 * on the cardinality.  Frees the container.
 * Allocates and returns new container, which caller is responsible for freeing.
 * It does not free the run container.
 */
container_t *convert_to_bitset_or_array_container(run_container_t *rc,
                                                  int32_t card,
                                                  uint8_t *resulttype) {
    if (card <= DEFAULT_MAX_SIZE) {
        array_container_t *answer = array_container_create_given_capacity(card);
        answer->cardinality = 0;
        for (int rlepos = 0; rlepos < rc->n_runs; ++rlepos) {
            uint16_t run_start = rc->runs[rlepos].value;
            uint16_t run_end = run_start + rc->runs[rlepos].length;
            for (uint16_t run_value = run_start; run_value < run_end;
                 ++run_value) {
                answer->array[answer->cardinality++] = run_value;
            }
            answer->array[answer->cardinality++] = run_end;
        }
        assert(card == answer->cardinality);
        *resulttype = ARRAY_CONTAINER_TYPE;
        // run_container_free(r);
        return answer;
    }
    bitset_container_t *answer = bitset_container_create();
    for (int rlepos = 0; rlepos < rc->n_runs; ++rlepos) {
        uint16_t run_start = rc->runs[rlepos].value;
        bitset_set_lenrange(answer->words, run_start, rc->runs[rlepos].length);
    }
    answer->cardinality = card;
    *resulttype = BITSET_CONTAINER_TYPE;
    // run_container_free(r);
    return answer;
}

/* Converts a run container to either an array or a bitset, IF it saves space.
 */
/* If a conversion occurs, the caller is responsible to free the original
 * container and
 * he becomes responsible to free the new one. */
container_t *convert_run_to_efficient_container(run_container_t *c,
                                                uint8_t *typecode_after) {
    int32_t size_as_run_container =
        run_container_serialized_size_in_bytes(c->n_runs);

    int32_t size_as_bitset_container =
        bitset_container_serialized_size_in_bytes();
    int32_t card = run_container_cardinality(c);
    int32_t size_as_array_container =
        array_container_serialized_size_in_bytes(card);

    int32_t min_size_non_run =
        size_as_bitset_container < size_as_array_container
            ? size_as_bitset_container
            : size_as_array_container;
    if (size_as_run_container <= min_size_non_run) {  // no conversion
        *typecode_after = RUN_CONTAINER_TYPE;
        return c;
    }
    if (card <= DEFAULT_MAX_SIZE) {
        // to array
        array_container_t *answer = array_container_create_given_capacity(card);
        answer->cardinality = 0;
        for (int rlepos = 0; rlepos < c->n_runs; ++rlepos) {
            int run_start = c->runs[rlepos].value;
            int run_end = run_start + c->runs[rlepos].length;

            for (int run_value = run_start; run_value <= run_end; ++run_value) {
                answer->array[answer->cardinality++] = (uint16_t)run_value;
            }
        }
        *typecode_after = ARRAY_CONTAINER_TYPE;
        return answer;
    }

    // else to bitset
    bitset_container_t *answer = bitset_container_create();

    for (int rlepos = 0; rlepos < c->n_runs; ++rlepos) {
        int start = c->runs[rlepos].value;
        int end = start + c->runs[rlepos].length;
        bitset_set_range(answer->words, start, end + 1);
    }
    answer->cardinality = card;
    *typecode_after = BITSET_CONTAINER_TYPE;
    return answer;
}

// like convert_run_to_efficient_container but frees the old result if needed
container_t *convert_run_to_efficient_container_and_free(
    run_container_t *c, uint8_t *typecode_after) {
    container_t *answer = convert_run_to_efficient_container(c, typecode_after);
    if (answer != c) run_container_free(c);
    return answer;
}

/* once converted, the original container is disposed here, rather than
   in roaring_array
*/

// TODO: split into run-  array-  and bitset-  subfunctions for sanity;
// a few function calls won't really matter.

container_t *convert_run_optimize(container_t *c, uint8_t typecode_original,
                                  uint8_t *typecode_after) {
    if (typecode_original == RUN_CONTAINER_TYPE) {
        container_t *newc =
            convert_run_to_efficient_container(CAST_run(c), typecode_after);
        if (newc != c) {
            container_free(c, typecode_original);
        }
        return newc;
    } else if (typecode_original == ARRAY_CONTAINER_TYPE) {
        // it might need to be converted to a run container.
        array_container_t *c_qua_array = CAST_array(c);
        int32_t n_runs = array_container_number_of_runs(c_qua_array);
        int32_t size_as_run_container =
            run_container_serialized_size_in_bytes(n_runs);
        int32_t card = array_container_cardinality(c_qua_array);
        int32_t size_as_array_container =
            array_container_serialized_size_in_bytes(card);

        if (size_as_run_container >= size_as_array_container) {
            *typecode_after = ARRAY_CONTAINER_TYPE;
            return c;
        }
        // else convert array to run container
        run_container_t *answer = run_container_create_given_capacity(n_runs);
        int prev = -2;
        int run_start = -1;

        assert(card > 0);
        for (int i = 0; i < card; ++i) {
            uint16_t cur_val = c_qua_array->array[i];
            if (cur_val != prev + 1) {
                // new run starts; flush old one, if any
                if (run_start != -1) add_run(answer, run_start, prev);
                run_start = cur_val;
            }
            prev = c_qua_array->array[i];
        }
        assert(run_start >= 0);
        // now prev is the last seen value
        add_run(answer, run_start, prev);
        *typecode_after = RUN_CONTAINER_TYPE;
        array_container_free(c_qua_array);
        return answer;
    } else if (typecode_original ==
               BITSET_CONTAINER_TYPE) {  // run conversions on bitset
        // does bitset need conversion to run?
        bitset_container_t *c_qua_bitset = CAST_bitset(c);
        int32_t n_runs = bitset_container_number_of_runs(c_qua_bitset);
        int32_t size_as_run_container =
            run_container_serialized_size_in_bytes(n_runs);
        int32_t size_as_bitset_container =
            bitset_container_serialized_size_in_bytes();

        if (size_as_bitset_container <= size_as_run_container) {
            // no conversion needed.
            *typecode_after = BITSET_CONTAINER_TYPE;
            return c;
        }
        // bitset to runcontainer (ported from Java  RunContainer(
        // BitmapContainer bc, int nbrRuns))
        assert(n_runs > 0);  // no empty bitmaps
        run_container_t *answer = run_container_create_given_capacity(n_runs);

        int long_ctr = 0;
        uint64_t cur_word = c_qua_bitset->words[0];
        while (true) {
            while (cur_word == UINT64_C(0) &&
                   long_ctr < BITSET_CONTAINER_SIZE_IN_WORDS - 1)
                cur_word = c_qua_bitset->words[++long_ctr];

            if (cur_word == UINT64_C(0)) {
                bitset_container_free(c_qua_bitset);
                *typecode_after = RUN_CONTAINER_TYPE;
                return answer;
            }

            int local_run_start = roaring_trailing_zeroes(cur_word);
            int run_start = local_run_start + 64 * long_ctr;
            uint64_t cur_word_with_1s = cur_word | (cur_word - 1);

            int run_end = 0;
            while (cur_word_with_1s == UINT64_C(0xFFFFFFFFFFFFFFFF) &&
                   long_ctr < BITSET_CONTAINER_SIZE_IN_WORDS - 1)
                cur_word_with_1s = c_qua_bitset->words[++long_ctr];

            if (cur_word_with_1s == UINT64_C(0xFFFFFFFFFFFFFFFF)) {
                run_end = 64 + long_ctr * 64;  // exclusive, I guess
                add_run(answer, run_start, run_end - 1);
                bitset_container_free(c_qua_bitset);
                *typecode_after = RUN_CONTAINER_TYPE;
                return answer;
            }
            int local_run_end = roaring_trailing_zeroes(~cur_word_with_1s);
            run_end = local_run_end + long_ctr * 64;
            add_run(answer, run_start, run_end - 1);
            cur_word = cur_word_with_1s & (cur_word_with_1s + 1);
        }
        return answer;
    } else {
        assert(false);
        roaring_unreachable;
        return NULL;
    }
}

container_t *container_from_run_range(const run_container_t *run, uint32_t min,
                                      uint32_t max, uint8_t *typecode_after) {
    // We expect most of the time to end up with a bitset container
    bitset_container_t *bitset = bitset_container_create();
    *typecode_after = BITSET_CONTAINER_TYPE;
    int32_t union_cardinality = 0;
    for (int32_t i = 0; i < run->n_runs; ++i) {
        uint32_t rle_min = run->runs[i].value;
        uint32_t rle_max = rle_min + run->runs[i].length;
        bitset_set_lenrange(bitset->words, rle_min, rle_max - rle_min);
        union_cardinality += run->runs[i].length + 1;
    }
    union_cardinality += max - min + 1;
    union_cardinality -=
        bitset_lenrange_cardinality(bitset->words, min, max - min);
    bitset_set_lenrange(bitset->words, min, max - min);
    bitset->cardinality = union_cardinality;
    if (bitset->cardinality <= DEFAULT_MAX_SIZE) {
        // we need to convert to an array container
        array_container_t *array = array_container_from_bitset(bitset);
        *typecode_after = ARRAY_CONTAINER_TYPE;
        bitset_container_free(bitset);
        return array;
    }
    return bitset;
}

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

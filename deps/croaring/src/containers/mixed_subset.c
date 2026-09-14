#include <roaring/array_util.h>
#include <roaring/containers/mixed_subset.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

bool array_container_is_subset_bitset(const array_container_t* container1,
                                      const bitset_container_t* container2) {
    if (container2->cardinality != BITSET_UNKNOWN_CARDINALITY) {
        if (container2->cardinality < container1->cardinality) {
            return false;
        }
    }
    for (int i = 0; i < container1->cardinality; ++i) {
        if (!bitset_container_contains(container2, container1->array[i])) {
            return false;
        }
    }
    return true;
}

bool run_container_is_subset_array(const run_container_t* container1,
                                   const array_container_t* container2) {
    if (run_container_cardinality(container1) > container2->cardinality)
        return false;
    int32_t start_pos = -1, stop_pos = -1;
    for (int i = 0; i < container1->n_runs; ++i) {
        int32_t start = container1->runs[i].value;
        int32_t stop = start + container1->runs[i].length;
        start_pos = advanceUntil(container2->array, stop_pos,
                                 container2->cardinality, start);
        stop_pos = advanceUntil(container2->array, stop_pos,
                                container2->cardinality, stop);
        if (stop_pos == container2->cardinality) {
            return false;
        } else if (stop_pos - start_pos != stop - start ||
                   container2->array[start_pos] != start ||
                   container2->array[stop_pos] != stop) {
            return false;
        }
    }
    return true;
}

bool array_container_is_subset_run(const array_container_t* container1,
                                   const run_container_t* container2) {
    if (container1->cardinality > run_container_cardinality(container2))
        return false;
    int i_array = 0, i_run = 0;
    while (i_array < container1->cardinality && i_run < container2->n_runs) {
        uint32_t start = container2->runs[i_run].value;
        uint32_t stop = start + container2->runs[i_run].length;
        if (container1->array[i_array] < start) {
            return false;
        } else if (container1->array[i_array] > stop) {
            i_run++;
        } else {  // the value of the array is in the run
            i_array++;
        }
    }
    if (i_array == container1->cardinality) {
        return true;
    } else {
        return false;
    }
}

bool run_container_is_subset_bitset(const run_container_t* container1,
                                    const bitset_container_t* container2) {
    // todo: this code could be much faster
    if (container2->cardinality != BITSET_UNKNOWN_CARDINALITY) {
        if (container2->cardinality < run_container_cardinality(container1)) {
            return false;
        }
    } else {
        int32_t card = bitset_container_compute_cardinality(
            container2);  // modify container2?
        if (card < run_container_cardinality(container1)) {
            return false;
        }
    }
    for (int i = 0; i < container1->n_runs; ++i) {
        uint32_t run_start = container1->runs[i].value;
        uint32_t le = container1->runs[i].length;
        for (uint32_t j = run_start; j <= run_start + le; ++j) {
            if (!bitset_container_contains(container2, j)) {
                return false;
            }
        }
    }
    return true;
}

bool bitset_container_is_subset_run(const bitset_container_t* container1,
                                    const run_container_t* container2) {
    // todo: this code could be much faster
    if (container1->cardinality != BITSET_UNKNOWN_CARDINALITY) {
        if (container1->cardinality > run_container_cardinality(container2)) {
            return false;
        }
    }
    int32_t i_bitset = 0, i_run = 0;
    while (i_bitset < BITSET_CONTAINER_SIZE_IN_WORDS &&
           i_run < container2->n_runs) {
        uint64_t w = container1->words[i_bitset];
        while (w != 0 && i_run < container2->n_runs) {
            uint32_t start = container2->runs[i_run].value;
            uint32_t stop = start + container2->runs[i_run].length;
            uint64_t t = w & (~w + 1);
            uint16_t r = i_bitset * 64 + roaring_trailing_zeroes(w);
            if (r < start) {
                return false;
            } else if (r > stop) {
                i_run++;
                continue;
            } else {
                w ^= t;
            }
        }
        if (w == 0) {
            i_bitset++;
        } else {
            return false;
        }
    }
    if (i_bitset < BITSET_CONTAINER_SIZE_IN_WORDS) {
        // terminated iterating on the run containers, check that rest of bitset
        // is empty
        for (; i_bitset < BITSET_CONTAINER_SIZE_IN_WORDS; i_bitset++) {
            if (container1->words[i_bitset] != 0) {
                return false;
            }
        }
    }
    return true;
}

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

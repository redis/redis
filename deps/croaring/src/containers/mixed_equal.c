#include <roaring/containers/mixed_equal.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

bool array_container_equal_bitset(const array_container_t* container1,
                                  const bitset_container_t* container2) {
    if (container2->cardinality != BITSET_UNKNOWN_CARDINALITY) {
        if (container2->cardinality != container1->cardinality) {
            return false;
        }
    }
    int32_t pos = 0;
    for (int32_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; ++i) {
        uint64_t w = container2->words[i];
        while (w != 0) {
            uint64_t t = w & (~w + 1);
            uint16_t r = i * 64 + roaring_trailing_zeroes(w);
            if (pos >= container1->cardinality) {
                return false;
            }
            if (container1->array[pos] != r) {
                return false;
            }
            ++pos;
            w ^= t;
        }
    }
    return (pos == container1->cardinality);
}

bool run_container_equals_array(const run_container_t* container1,
                                const array_container_t* container2) {
    if (run_container_cardinality(container1) != container2->cardinality)
        return false;
    int32_t pos = 0;
    for (int i = 0; i < container1->n_runs; ++i) {
        const uint32_t run_start = container1->runs[i].value;
        const uint32_t le = container1->runs[i].length;

        if (container2->array[pos] != run_start) {
            return false;
        }

        if (container2->array[pos + le] != run_start + le) {
            return false;
        }

        pos += le + 1;
    }
    return true;
}

bool run_container_equals_bitset(const run_container_t* container1,
                                 const bitset_container_t* container2) {
    int run_card = run_container_cardinality(container1);
    int bitset_card = (container2->cardinality != BITSET_UNKNOWN_CARDINALITY)
                          ? container2->cardinality
                          : bitset_container_compute_cardinality(container2);
    if (bitset_card != run_card) {
        return false;
    }

    for (int32_t i = 0; i < container1->n_runs; i++) {
        uint32_t begin = container1->runs[i].value;
        if (container1->runs[i].length) {
            uint32_t end = begin + container1->runs[i].length + 1;
            if (!bitset_container_contains_range(container2, begin, end)) {
                return false;
            }
        } else {
            if (!bitset_container_contains(container2, begin)) {
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

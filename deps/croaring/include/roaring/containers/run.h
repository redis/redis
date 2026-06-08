/*
 * run.h
 *
 * Run containers store a set of 16-bit integers as a sorted array of
 * non-overlapping runs. Each run is represented by a starting value and a
 * length, encoding one contiguous interval of present integers.
 *
 * This representation is effective when the data contains long consecutive
 * ranges because it compresses many adjacent values into a small number of
 * run records while still supporting search and set operations over the
 * interval list.
 */

#ifndef INCLUDE_CONTAINERS_RUN_H_
#define INCLUDE_CONTAINERS_RUN_H_

#include <roaring/roaring_types.h>  // roaring_iterator

// Include other headers after roaring_types.h
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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

/* struct rle16_s - run length pair
 *
 * @value:  start position of the run
 * @length: length of the run is `length + 1`
 *
 * An RLE pair {v, l} would represent the integers between the interval
 * [v, v+l+1], e.g. {3, 2} = [3, 4, 5].
 */
struct rle16_s {
    uint16_t value;
    uint16_t length;
};

typedef struct rle16_s rle16_t;

#ifdef __cplusplus
#define CROARING_MAKE_RLE16(val, len) \
    { (uint16_t)(val), (uint16_t)(len) }  // no tagged structs until c++20
#else
#define CROARING_MAKE_RLE16(val, len) \
    (rle16_t) { .value = (uint16_t)(val), .length = (uint16_t)(len) }
#endif

/* struct run_container_s - run container bitmap
 *
 * @n_runs:   number of rle_t pairs in `runs`.
 * @capacity: capacity in rle_t pairs `runs` can hold.
 * @runs:     pairs of rle_t.
 */
STRUCT_CONTAINER(run_container_s) {
    int32_t n_runs;
    int32_t capacity;
    rle16_t *runs;
};

typedef struct run_container_s run_container_t;

#define CAST_run(c) CAST(run_container_t *, c)  // safer downcast
#define const_CAST_run(c) CAST(const run_container_t *, c)
#define movable_CAST_run(c) movable_CAST(run_container_t **, c)

/* Create a new run container. Return NULL in case of failure. */
run_container_t *run_container_create(void);

/* Create a new run container with given capacity. Return NULL in case of
 * failure. */
run_container_t *run_container_create_given_capacity(int32_t size);

/*
 * Shrink the capacity to the actual size, return the number of bytes saved.
 */
int run_container_shrink_to_fit(run_container_t *src);

/* Free memory owned by `run'. */
void run_container_free(run_container_t *run);

/* Duplicate container */
run_container_t *run_container_clone(const run_container_t *src);

/*
 * Effectively deletes the value at index index, repacking data.
 */
static inline void recoverRoomAtIndex(run_container_t *run, uint16_t index) {
    memmove(run->runs + index, run->runs + (1 + index),
            (run->n_runs - index - 1) * sizeof(rle16_t));
    run->n_runs--;
}

/**
 * Good old binary search through rle data
 */
inline int32_t interleavedBinarySearch(const rle16_t *array, int32_t lenarray,
                                       uint16_t ikey) {
    int32_t low = 0;
    int32_t high = lenarray - 1;
    while (low <= high) {
        int32_t middleIndex = (low + high) >> 1;
        uint16_t middleValue = array[middleIndex].value;
        if (middleValue < ikey) {
            low = middleIndex + 1;
        } else if (middleValue > ikey) {
            high = middleIndex - 1;
        } else {
            return middleIndex;
        }
    }
    return -(low + 1);
}

/*
 * Returns index of the run which contains $ikey
 */
static inline int32_t rle16_find_run(const rle16_t *array, int32_t lenarray,
                                     uint16_t ikey) {
    int32_t low = 0;
    int32_t high = lenarray - 1;
    while (low <= high) {
        int32_t middleIndex = (low + high) >> 1;
        uint16_t min = array[middleIndex].value;
        uint16_t max = array[middleIndex].value + array[middleIndex].length;
        if (ikey > max) {
            low = middleIndex + 1;
        } else if (ikey < min) {
            high = middleIndex - 1;
        } else {
            return middleIndex;
        }
    }
    return -(low + 1);
}

/**
 * Returns number of runs which can'be be merged with the key because they
 * are less than the key.
 * Note that [5,6,7,8] can be merged with the key 9 and won't be counted.
 */
static inline int32_t rle16_count_less(const rle16_t *array, int32_t lenarray,
                                       uint16_t key) {
    if (lenarray == 0) return 0;
    int32_t low = 0;
    int32_t high = lenarray - 1;
    while (low <= high) {
        int32_t middleIndex = (low + high) >> 1;
        uint16_t min_value = array[middleIndex].value;
        uint16_t max_value =
            array[middleIndex].value + array[middleIndex].length;
        if (max_value + UINT32_C(1) < key) {  // uint32 arithmetic
            low = middleIndex + 1;
        } else if (key < min_value) {
            high = middleIndex - 1;
        } else {
            return middleIndex;
        }
    }
    return low;
}

static inline int32_t rle16_count_greater(const rle16_t *array,
                                          int32_t lenarray, uint16_t key) {
    if (lenarray == 0) return 0;
    int32_t low = 0;
    int32_t high = lenarray - 1;
    while (low <= high) {
        int32_t middleIndex = (low + high) >> 1;
        uint16_t min_value = array[middleIndex].value;
        uint16_t max_value =
            array[middleIndex].value + array[middleIndex].length;
        if (max_value < key) {
            low = middleIndex + 1;
        } else if (key + UINT32_C(1) < min_value) {  // uint32 arithmetic
            high = middleIndex - 1;
        } else {
            return lenarray - (middleIndex + 1);
        }
    }
    return lenarray - low;
}

/**
 * increase capacity to at least min. Whether the
 * existing data needs to be copied over depends on copy. If "copy" is false,
 * then the new content will be uninitialized, otherwise a copy is made.
 */
void run_container_grow(run_container_t *run, int32_t min, bool copy);

/**
 * Moves the data so that we can write data at index
 */
static inline void makeRoomAtIndex(run_container_t *run, uint16_t index) {
    /* This function calls realloc + memmove sequentially to move by one index.
     * Potentially copying twice the array.
     */
    if (run->n_runs + 1 > run->capacity)
        run_container_grow(run, run->n_runs + 1, true);
    memmove(run->runs + 1 + index, run->runs + index,
            (run->n_runs - index) * sizeof(rle16_t));
    run->n_runs++;
}

/* Add `pos' to `run'. Returns true if `pos' was not present. */
bool run_container_add(run_container_t *run, uint16_t pos);

/* Remove `pos' from `run'. Returns true if `pos' was present. */
static inline bool run_container_remove(run_container_t *run, uint16_t pos) {
    int32_t index = interleavedBinarySearch(run->runs, run->n_runs, pos);
    if (index >= 0) {
        int32_t le = run->runs[index].length;
        if (le == 0) {
            recoverRoomAtIndex(run, (uint16_t)index);
        } else {
            run->runs[index].value++;
            run->runs[index].length--;
        }
        return true;
    }
    index = -index - 2;  // points to preceding value, possibly -1
    if (index >= 0) {    // possible match
        int32_t offset = pos - run->runs[index].value;
        int32_t le = run->runs[index].length;
        if (offset < le) {
            // need to break in two
            run->runs[index].length = (uint16_t)(offset - 1);
            // need to insert
            uint16_t newvalue = pos + 1;
            int32_t newlength = le - offset - 1;
            makeRoomAtIndex(run, (uint16_t)(index + 1));
            run->runs[index + 1].value = newvalue;
            run->runs[index + 1].length = (uint16_t)newlength;
            return true;

        } else if (offset == le) {
            run->runs[index].length--;
            return true;
        }
    }
    // no match
    return false;
}

/* Check whether `pos' is present in `run'.  */
inline bool run_container_contains(const run_container_t *run, uint16_t pos) {
    int32_t index = interleavedBinarySearch(run->runs, run->n_runs, pos);
    if (index >= 0) return true;
    index = -index - 2;  // points to preceding value, possibly -1
    if (index != -1) {   // possible match
        int32_t offset = pos - run->runs[index].value;
        int32_t le = run->runs[index].length;
        if (offset <= le) return true;
    }
    return false;
}

/*
 * Check whether all positions in a range of positions from pos_start (included)
 * to pos_end (excluded) is present in `run'.
 */
static inline bool run_container_contains_range(const run_container_t *run,
                                                uint32_t pos_start,
                                                uint32_t pos_end) {
    uint32_t count = 0;
    int32_t index =
        interleavedBinarySearch(run->runs, run->n_runs, (uint16_t)pos_start);
    if (index < 0) {
        index = -index - 2;
        if ((index == -1) ||
            ((pos_start - run->runs[index].value) > run->runs[index].length)) {
            return false;
        }
    }
    for (int32_t i = index; i < run->n_runs; ++i) {
        const uint32_t stop = run->runs[i].value + run->runs[i].length;
        if (run->runs[i].value >= pos_end) break;
        if (stop >= pos_end) {
            count += (((pos_end - run->runs[i].value) > 0)
                          ? (pos_end - run->runs[i].value)
                          : 0);
            break;
        }
        const uint32_t min = (stop - pos_start) > 0 ? (stop - pos_start) : 0;
        count += (min < run->runs[i].length) ? min : run->runs[i].length;
    }
    return count >= (pos_end - pos_start - 1);
}

/* Get the cardinality of `run'. Requires an actual computation. */
int run_container_cardinality(const run_container_t *run);

/* Card > 0?, see run_container_empty for the reverse */
static inline bool run_container_nonzero_cardinality(
    const run_container_t *run) {
    return run->n_runs > 0;  // runs never empty
}

/* Card == 0?, see run_container_nonzero_cardinality for the reverse */
static inline bool run_container_empty(const run_container_t *run) {
    return run->n_runs == 0;  // runs never empty
}

/* Copy one container into another. We assume that they are distinct. */
void run_container_copy(const run_container_t *src, run_container_t *dst);

/**
 * Append run described by vl to the run container, possibly merging.
 * It is assumed that the run would be inserted at the end of the container, no
 * check is made.
 * It is assumed that the run container has the necessary capacity: caller is
 * responsible for checking memory capacity.
 *
 *
 * This is not a safe function, it is meant for performance: use with care.
 */
static inline void run_container_append(run_container_t *run, rle16_t vl,
                                        rle16_t *previousrl) {
    const uint32_t previousend = previousrl->value + previousrl->length;
    if (vl.value > previousend + 1) {  // we add a new one
        run->runs[run->n_runs] = vl;
        run->n_runs++;
        *previousrl = vl;
    } else {
        uint32_t newend = vl.value + vl.length + UINT32_C(1);
        if (newend > previousend) {  // we merge
            previousrl->length = (uint16_t)(newend - 1 - previousrl->value);
            run->runs[run->n_runs - 1] = *previousrl;
        }
    }
}

/**
 * Like run_container_append but it is assumed that the content of run is empty.
 */
static inline rle16_t run_container_append_first(run_container_t *run,
                                                 rle16_t vl) {
    run->runs[run->n_runs] = vl;
    run->n_runs++;
    return vl;
}

/**
 * append a single value  given by val to the run container, possibly merging.
 * It is assumed that the value would be inserted at the end of the container,
 * no check is made.
 * It is assumed that the run container has the necessary capacity: caller is
 * responsible for checking memory capacity.
 *
 * This is not a safe function, it is meant for performance: use with care.
 */
static inline void run_container_append_value(run_container_t *run,
                                              uint16_t val,
                                              rle16_t *previousrl) {
    const uint32_t previousend = previousrl->value + previousrl->length;
    if (val > previousend + 1) {  // we add a new one
        *previousrl = CROARING_MAKE_RLE16(val, 0);
        run->runs[run->n_runs] = *previousrl;
        run->n_runs++;
    } else if (val == previousend + 1) {  // we merge
        previousrl->length++;
        run->runs[run->n_runs - 1] = *previousrl;
    }
}

/**
 * Like run_container_append_value but it is assumed that the content of run is
 * empty.
 */
static inline rle16_t run_container_append_value_first(run_container_t *run,
                                                       uint16_t val) {
    rle16_t newrle = CROARING_MAKE_RLE16(val, 0);
    run->runs[run->n_runs] = newrle;
    run->n_runs++;
    return newrle;
}

/* Check whether the container spans the whole chunk (cardinality = 1<<16).
 * This check can be done in constant time (inexpensive). */
static inline bool run_container_is_full(const run_container_t *run) {
    rle16_t vl = run->runs[0];
    return (run->n_runs == 1) && (vl.value == 0) && (vl.length == 0xFFFF);
}

/* Compute the union of `src_1' and `src_2' and write the result to `dst'
 * It is assumed that `dst' is distinct from both `src_1' and `src_2'. */
void run_container_union(const run_container_t *src_1,
                         const run_container_t *src_2, run_container_t *dst);

/* Compute the union of `src_1' and `src_2' and write the result to `src_1' */
void run_container_union_inplace(run_container_t *src_1,
                                 const run_container_t *src_2);

/* Compute the intersection of src_1 and src_2 and write the result to
 * dst. It is assumed that dst is distinct from both src_1 and src_2. */
void run_container_intersection(const run_container_t *src_1,
                                const run_container_t *src_2,
                                run_container_t *dst);

/* Compute the size of the intersection of src_1 and src_2 . */
int run_container_intersection_cardinality(const run_container_t *src_1,
                                           const run_container_t *src_2);

/* Check whether src_1 and src_2 intersect. */
bool run_container_intersect(const run_container_t *src_1,
                             const run_container_t *src_2);

/* Compute the symmetric difference of `src_1' and `src_2' and write the result
 * to `dst'
 * It is assumed that `dst' is distinct from both `src_1' and `src_2'. */
void run_container_xor(const run_container_t *src_1,
                       const run_container_t *src_2, run_container_t *dst);

/*
 * Write out the 16-bit integers contained in this container as a list of 32-bit
 * integers using base
 * as the starting value (it might be expected that base has zeros in its 16
 * least significant bits).
 * The function returns the number of values written.
 * The caller is responsible for allocating enough memory in out.
 */
int run_container_to_uint32_array(void *vout, const run_container_t *cont,
                                  uint32_t base);

/*
 * Print this container using printf (useful for debugging).
 */
void run_container_printf(const run_container_t *v);

/*
 * Print this container using printf as a comma-separated list of 32-bit
 * integers starting at base.
 */
void run_container_printf_as_uint32_array(const run_container_t *v,
                                          uint32_t base);

bool run_container_validate(const run_container_t *run, const char **reason);

/**
 * Return the serialized size in bytes of a container having "num_runs" runs.
 */
static inline int32_t run_container_serialized_size_in_bytes(int32_t num_runs) {
    return sizeof(uint16_t) +
           sizeof(rle16_t) * num_runs;  // each run requires 2 2-byte entries.
}

bool run_container_iterate(const run_container_t *cont, uint32_t base,
                           roaring_iterator iterator, void *ptr);
bool run_container_iterate64(const run_container_t *cont, uint32_t base,
                             roaring_iterator64 iterator, uint64_t high_bits,
                             void *ptr);

/**
 * Writes the underlying array to buf, outputs how many bytes were written.
 * This is meant to be byte-by-byte compatible with the Java and Go versions of
 * Roaring.
 * The number of bytes written should be run_container_size_in_bytes(container).
 */
int32_t run_container_write(const run_container_t *container, char *buf);

/**
 * Reads the instance from buf, outputs how many bytes were read.
 * This is meant to be byte-by-byte compatible with the Java and Go versions of
 * Roaring.
 * The number of bytes read should be bitset_container_size_in_bytes(container).
 * The cardinality parameter is provided for consistency with other containers,
 * but
 * it might be effectively ignored..
 */
int32_t run_container_read(int32_t cardinality, run_container_t *container,
                           const char *buf);

/**
 * Return the serialized size in bytes of a container (see run_container_write).
 * This is meant to be compatible with the Java and Go versions of Roaring.
 */
CROARING_ALLOW_UNALIGNED
static inline int32_t run_container_size_in_bytes(
    const run_container_t *container) {
    return run_container_serialized_size_in_bytes(container->n_runs);
}

/**
 * Return true if the two containers have the same content.
 */
CROARING_ALLOW_UNALIGNED
static inline bool run_container_equals(const run_container_t *container1,
                                        const run_container_t *container2) {
    if (container1->n_runs != container2->n_runs) {
        return false;
    }
    return memequals(container1->runs, container2->runs,
                     container1->n_runs * sizeof(rle16_t));
}

/**
 * Return true if container1 is a subset of container2.
 */
bool run_container_is_subset(const run_container_t *container1,
                             const run_container_t *container2);

/**
 * Used in a start-finish scan that appends segments, for XOR and NOT
 */

void run_container_smart_append_exclusive(run_container_t *src,
                                          const uint16_t start,
                                          const uint16_t length);

/**
 * The new container consists of a single run [start,stop).
 * It is required that stop>start, the caller is responsability for this check.
 * It is required that stop <= (1<<16), the caller is responsability for this
 * check. The cardinality of the created container is stop - start. Returns NULL
 * on failure
 */
static inline run_container_t *run_container_create_range(uint32_t start,
                                                          uint32_t stop) {
    run_container_t *rc = run_container_create_given_capacity(1);
    if (rc) {
        rle16_t r;
        r.value = (uint16_t)start;
        r.length = (uint16_t)(stop - start - 1);
        run_container_append_first(rc, r);
    }
    return rc;
}

/**
 * If the element of given rank is in this container, supposing that the first
 * element has rank start_rank, then the function returns true and sets element
 * accordingly.
 * Otherwise, it returns false and update start_rank.
 */
bool run_container_select(const run_container_t *container,
                          uint32_t *start_rank, uint32_t rank,
                          uint32_t *element);

/* Compute the difference of src_1 and src_2 and write the result to
 * dst. It is assumed that dst is distinct from both src_1 and src_2. */

void run_container_andnot(const run_container_t *src_1,
                          const run_container_t *src_2, run_container_t *dst);

void run_container_offset(const run_container_t *c, container_t **loc,
                          container_t **hic, uint16_t offset);

/* Returns the smallest value (assumes not empty) */
inline uint16_t run_container_minimum(const run_container_t *run) {
    if (run->n_runs == 0) return 0;
    return run->runs[0].value;
}

/* Returns the largest value (assumes not empty) */
inline uint16_t run_container_maximum(const run_container_t *run) {
    if (run->n_runs == 0) return 0;
    return run->runs[run->n_runs - 1].value + run->runs[run->n_runs - 1].length;
}

/* Returns the number of values equal or smaller than x */
int run_container_rank(const run_container_t *arr, uint16_t x);

/* bulk version of run_container_rank(); return number of consumed elements */
uint32_t run_container_rank_many(const run_container_t *arr,
                                 uint64_t start_rank, const uint32_t *begin,
                                 const uint32_t *end, uint64_t *ans);

/* Returns the index of x, if not exsist return -1 */
int run_container_get_index(const run_container_t *arr, uint16_t x);

/* Returns the index of the first run containing a value at least as large as x,
 * or -1 */
inline int run_container_index_equalorlarger(const run_container_t *arr,
                                             uint16_t x) {
    int32_t index = interleavedBinarySearch(arr->runs, arr->n_runs, x);
    if (index >= 0) return index;
    index = -index - 2;  // points to preceding run, possibly -1
    if (index != -1) {   // possible match
        int32_t offset = x - arr->runs[index].value;
        int32_t le = arr->runs[index].length;
        if (offset <= le) return index;
    }
    index += 1;
    if (index < arr->n_runs) {
        return index;
    }
    return -1;
}

/*
 * Add all values in range [min, max] using hint.
 */
static inline void run_container_add_range_nruns(run_container_t *run,
                                                 uint32_t min, uint32_t max,
                                                 int32_t nruns_less,
                                                 int32_t nruns_greater) {
    int32_t nruns_common = run->n_runs - nruns_less - nruns_greater;
    if (nruns_common == 0) {
        makeRoomAtIndex(run, (uint16_t)nruns_less);
        run->runs[nruns_less].value = (uint16_t)min;
        run->runs[nruns_less].length = (uint16_t)(max - min);
    } else {
        uint32_t common_min = run->runs[nruns_less].value;
        uint32_t common_max = run->runs[nruns_less + nruns_common - 1].value +
                              run->runs[nruns_less + nruns_common - 1].length;
        uint32_t result_min = (common_min < min) ? common_min : min;
        uint32_t result_max = (common_max > max) ? common_max : max;

        run->runs[nruns_less].value = (uint16_t)result_min;
        run->runs[nruns_less].length = (uint16_t)(result_max - result_min);

        memmove(&(run->runs[nruns_less + 1]),
                &(run->runs[run->n_runs - nruns_greater]),
                nruns_greater * sizeof(rle16_t));
        run->n_runs = nruns_less + 1 + nruns_greater;
    }
}

/**
 * Add all values in range [min, max]. This function is currently unused
 * and left as documentation.
 */
/*static inline void run_container_add_range(run_container_t* run,
                                           uint32_t min, uint32_t max) {
    int32_t nruns_greater = rle16_count_greater(run->runs, run->n_runs, max);
    int32_t nruns_less = rle16_count_less(run->runs, run->n_runs -
nruns_greater, min); run_container_add_range_nruns(run, min, max, nruns_less,
nruns_greater);
}*/

/**
 * Shifts last $count elements either left (distance < 0) or right (distance >
 * 0)
 */
static inline void run_container_shift_tail(run_container_t *run, int32_t count,
                                            int32_t distance) {
    if (distance > 0) {
        if (run->capacity < count + distance) {
            run_container_grow(run, count + distance, true);
        }
    }
    int32_t srcpos = run->n_runs - count;
    int32_t dstpos = srcpos + distance;
    memmove(&(run->runs[dstpos]), &(run->runs[srcpos]),
            sizeof(rle16_t) * count);
    run->n_runs += distance;
}

/**
 * Remove all elements in range [min, max]
 */
static inline void run_container_remove_range(run_container_t *run,
                                              uint32_t min, uint32_t max) {
    int32_t first = rle16_find_run(run->runs, run->n_runs, (uint16_t)min);
    int32_t last = rle16_find_run(run->runs, run->n_runs, (uint16_t)max);

    if (first >= 0 && min > run->runs[first].value &&
        max < ((uint32_t)run->runs[first].value +
               (uint32_t)run->runs[first].length)) {
        // split this run into two adjacent runs

        // right subinterval
        makeRoomAtIndex(run, (uint16_t)(first + 1));
        run->runs[first + 1].value = (uint16_t)(max + 1);
        run->runs[first + 1].length =
            (uint16_t)((run->runs[first].value + run->runs[first].length) -
                       (max + 1));

        // left subinterval
        run->runs[first].length =
            (uint16_t)((min - 1) - run->runs[first].value);

        return;
    }

    // update left-most partial run
    if (first >= 0) {
        if (min > run->runs[first].value) {
            run->runs[first].length =
                (uint16_t)((min - 1) - run->runs[first].value);
            first++;
        }
    } else {
        first = -first - 1;
    }

    // update right-most run
    if (last >= 0) {
        uint16_t run_max = run->runs[last].value + run->runs[last].length;
        if (run_max > max) {
            run->runs[last].value = (uint16_t)(max + 1);
            run->runs[last].length = (uint16_t)(run_max - (max + 1));
            last--;
        }
    } else {
        last = (-last - 1) - 1;
    }

    // remove intermediate runs
    if (first <= last) {
        run_container_shift_tail(run, run->n_runs - (last + 1),
                                 -(last - first + 1));
    }
}

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

#endif /* INCLUDE_CONTAINERS_RUN_H_ */

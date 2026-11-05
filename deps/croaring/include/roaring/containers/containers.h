/*
 * containers.h
 *
 * This header is the central internal interface for Roaring container
 * operations. It ties together the concrete container types (array, bitset,
 * run, and shared containers), their type codes, common helper functions, and
 * the mixed-operation headers used to combine different representations.
 *
 * In practice, it acts as the dispatch layer that lets higher-level bitmap
 * code manipulate containers through a uniform interface while still selecting
 * type-specific implementations when needed.
 */
#ifndef CONTAINERS_CONTAINERS_H
#define CONTAINERS_CONTAINERS_H

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include <roaring/bitset_util.h>
#include <roaring/containers/array.h>
#include <roaring/containers/bitset.h>
#include <roaring/containers/convert.h>
#include <roaring/containers/mixed_andnot.h>
#include <roaring/containers/mixed_equal.h>
#include <roaring/containers/mixed_intersection.h>
#include <roaring/containers/mixed_negation.h>
#include <roaring/containers/mixed_subset.h>
#include <roaring/containers/mixed_union.h>
#include <roaring/containers/mixed_xor.h>
#include <roaring/containers/run.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

// would enum be possible or better?

/**
 * The switch case statements follow
 * BITSET_CONTAINER_TYPE -- ARRAY_CONTAINER_TYPE -- RUN_CONTAINER_TYPE
 * so it makes more sense to number them 1, 2, 3 (in the vague hope that the
 * compiler might exploit this ordering).
 */

#define BITSET_CONTAINER_TYPE 1
#define ARRAY_CONTAINER_TYPE 2
#define RUN_CONTAINER_TYPE 3
#define SHARED_CONTAINER_TYPE 4

/**
 * Macros for pairing container type codes, suitable for switch statements.
 * Use PAIR_CONTAINER_TYPES() for the switch, CONTAINER_PAIR() for the cases:
 *
 *     switch (PAIR_CONTAINER_TYPES(type1, type2)) {
 *        case CONTAINER_PAIR(BITSET,ARRAY):
 *        ...
 *     }
 */
#define PAIR_CONTAINER_TYPES(type1, type2) (4 * (type1) + (type2))

#define CONTAINER_PAIR(name1, name2) \
    (4 * (name1##_CONTAINER_TYPE) + (name2##_CONTAINER_TYPE))

/**
 * A shared container is a wrapper around a container
 * with reference counting.
 */
STRUCT_CONTAINER(shared_container_s) {
    container_t *container;
    uint8_t typecode;
    croaring_refcount_t counter;  // to be managed atomically
};

typedef struct shared_container_s shared_container_t;

#define CAST_shared(c) CAST(shared_container_t *, c)  // safer downcast
#define const_CAST_shared(c) CAST(const shared_container_t *, c)
#define movable_CAST_shared(c) movable_CAST(shared_container_t **, c)

/*
 * With copy_on_write = true
 *  Create a new shared container if the typecode is not SHARED_CONTAINER_TYPE,
 * otherwise, increase the count
 * If copy_on_write = false, then clone.
 * Return NULL in case of failure.
 **/
container_t *get_copy_of_container(container_t *container, uint8_t *typecode,
                                   bool copy_on_write);

/* Frees a shared container (actually decrement its counter and only frees when
 * the counter falls to zero). */
void shared_container_free(shared_container_t *container);

/* extract a copy from the shared container, freeing the shared container if
there is just one instance left,
clone instances when the counter is higher than one
*/
container_t *shared_container_extract_copy(shared_container_t *container,
                                           uint8_t *typecode);

/* access to container underneath */
static inline const container_t *container_unwrap_shared(
    const container_t *candidate_shared_container, uint8_t *type) {
    if (*type == SHARED_CONTAINER_TYPE) {
        *type = const_CAST_shared(candidate_shared_container)->typecode;
        assert(*type != SHARED_CONTAINER_TYPE);
        return const_CAST_shared(candidate_shared_container)->container;
    } else {
        return candidate_shared_container;
    }
}

/* access to container underneath */
static inline container_t *container_mutable_unwrap_shared(container_t *c,
                                                           uint8_t *type) {
    if (*type == SHARED_CONTAINER_TYPE) {  // the passed in container is shared
        *type = CAST_shared(c)->typecode;
        assert(*type != SHARED_CONTAINER_TYPE);
        return CAST_shared(c)->container;  // return the enclosed container
    } else {
        return c;  // wasn't shared, so return as-is
    }
}

/* access to container underneath and queries its type */
static inline uint8_t get_container_type(const container_t *c, uint8_t type) {
    if (type == SHARED_CONTAINER_TYPE) {
        return const_CAST_shared(c)->typecode;
    } else {
        return type;
    }
}

/**
 * Copies a container, requires a typecode. This allocates new memory, caller
 * is responsible for deallocation. If the container is not shared, then it is
 * physically cloned. Sharable containers are not cloneable.
 */
container_t *container_clone(const container_t *container, uint8_t typecode);

/* access to container underneath, cloning it if needed */
static inline container_t *get_writable_copy_if_shared(container_t *c,
                                                       uint8_t *type) {
    if (*type == SHARED_CONTAINER_TYPE) {  // shared, return enclosed container
        return shared_container_extract_copy(CAST_shared(c), type);
    } else {
        return c;  // not shared, so return as-is
    }
}

/**
 * End of shared container code
 */

static const char *container_names[] = {"bitset", "array", "run", "shared"};
static const char *shared_container_names[] = {
    "bitset (shared)", "array (shared)", "run (shared)"};

// no matter what the initial container was, convert it to a bitset
// if a new container is produced, caller responsible for freeing the previous
// one
// container should not be a shared container
static inline bitset_container_t *container_to_bitset(container_t *c,
                                                      uint8_t typecode) {
    bitset_container_t *result = NULL;
    switch (typecode) {
        case BITSET_CONTAINER_TYPE:
            return CAST_bitset(c);  // nothing to do
        case ARRAY_CONTAINER_TYPE:
            result = bitset_container_from_array(CAST_array(c));
            return result;
        case RUN_CONTAINER_TYPE:
            result = bitset_container_from_run(CAST_run(c));
            return result;
        case SHARED_CONTAINER_TYPE:
            assert(false);
            roaring_unreachable;
    }
    assert(false);
    roaring_unreachable;
    return 0;  // unreached
}

/**
 * Get the container name from the typecode
 * (unused at time of writing)
 */
/*static inline const char *get_container_name(uint8_t typecode) {
    switch (typecode) {
        case BITSET_CONTAINER_TYPE:
            return container_names[0];
        case ARRAY_CONTAINER_TYPE:
            return container_names[1];
        case RUN_CONTAINER_TYPE:
            return container_names[2];
        case SHARED_CONTAINER_TYPE:
            return container_names[3];
        default:
            assert(false);
            roaring_unreachable;
            return "unknown";
    }
}*/

static inline const char *get_full_container_name(const container_t *c,
                                                  uint8_t typecode) {
    switch (typecode) {
        case BITSET_CONTAINER_TYPE:
            return container_names[0];
        case ARRAY_CONTAINER_TYPE:
            return container_names[1];
        case RUN_CONTAINER_TYPE:
            return container_names[2];
        case SHARED_CONTAINER_TYPE:
            switch (const_CAST_shared(c)->typecode) {
                case BITSET_CONTAINER_TYPE:
                    return shared_container_names[0];
                case ARRAY_CONTAINER_TYPE:
                    return shared_container_names[1];
                case RUN_CONTAINER_TYPE:
                    return shared_container_names[2];
                default:
                    assert(false);
                    roaring_unreachable;
                    return "unknown";
            }
            break;
        default:
            assert(false);
            roaring_unreachable;
            return "unknown";
    }
    roaring_unreachable;
    return NULL;
}

/**
 * Get the container cardinality (number of elements), requires a  typecode
 */
static inline int container_get_cardinality(const container_t *c,
                                            uint8_t typecode) {
    c = container_unwrap_shared(c, &typecode);
    switch (typecode) {
        case BITSET_CONTAINER_TYPE:
            return bitset_container_cardinality(const_CAST_bitset(c));
        case ARRAY_CONTAINER_TYPE:
            return array_container_cardinality(const_CAST_array(c));
        case RUN_CONTAINER_TYPE:
            return run_container_cardinality(const_CAST_run(c));
    }
    assert(false);
    roaring_unreachable;
    return 0;  // unreached
}

// returns true if a container is known to be full. Note that a lazy bitset
// container
// might be full without us knowing
static inline bool container_is_full(const container_t *c, uint8_t typecode) {
    c = container_unwrap_shared(c, &typecode);
    switch (typecode) {
        case BITSET_CONTAINER_TYPE:
            return bitset_container_cardinality(const_CAST_bitset(c)) ==
                   (1 << 16);
        case ARRAY_CONTAINER_TYPE:
            return array_container_cardinality(const_CAST_array(c)) ==
                   (1 << 16);
        case RUN_CONTAINER_TYPE:
            return run_container_is_full(const_CAST_run(c));
    }
    assert(false);
    roaring_unreachable;
    return 0;  // unreached
}

static inline int container_shrink_to_fit(container_t *c, uint8_t type) {
    c = container_mutable_unwrap_shared(c, &type);
    switch (type) {
        case BITSET_CONTAINER_TYPE:
            return 0;  // no shrinking possible
        case ARRAY_CONTAINER_TYPE:
            return array_container_shrink_to_fit(CAST_array(c));
        case RUN_CONTAINER_TYPE:
            return run_container_shrink_to_fit(CAST_run(c));
    }
    assert(false);
    roaring_unreachable;
    return 0;  // unreached
}

/**
 * make a container with a run of ones
 */
/* initially always use a run container, even if an array might be
 * marginally
 * smaller */
static inline container_t *container_range_of_ones(uint32_t range_start,
                                                   uint32_t range_end,
                                                   uint8_t *result_type) {
    assert(range_end >= range_start);
    uint64_t cardinality = range_end - range_start + 1;
    if (cardinality <= 2) {
        *result_type = ARRAY_CONTAINER_TYPE;
        return array_container_create_range(range_start, range_end);
    } else {
        *result_type = RUN_CONTAINER_TYPE;
        return run_container_create_range(range_start, range_end);
    }
}

/*  Create a container with all the values between in [min,max) at a
    distance k*step from min. */
static inline container_t *container_from_range(uint8_t *type, uint32_t min,
                                                uint32_t max, uint16_t step) {
    if (step == 0) return NULL;  // being paranoid
    if (step == 1) {
        return container_range_of_ones(min, max, type);
        // Note: the result is not always a run (need to check the cardinality)
        //*type = RUN_CONTAINER_TYPE;
        // return run_container_create_range(min, max);
    }
    int size = (max - min + step - 1) / step;
    if (size <= DEFAULT_MAX_SIZE) {  // array container
        *type = ARRAY_CONTAINER_TYPE;
        array_container_t *array = array_container_create_given_capacity(size);
        array_container_add_from_range(array, min, max, step);
        assert(array->cardinality == size);
        return array;
    } else {  // bitset container
        *type = BITSET_CONTAINER_TYPE;
        bitset_container_t *bitset = bitset_container_create();
        bitset_container_add_from_range(bitset, min, max, step);
        assert(bitset->cardinality == size);
        return bitset;
    }
}

/**
 * "repair" the container after lazy operations.
 */
static inline container_t *container_repair_after_lazy(container_t *c,
                                                       uint8_t *type) {
    c = get_writable_copy_if_shared(c, type);  // !!! unnecessary cloning
    container_t *result = NULL;
    switch (*type) {
        case BITSET_CONTAINER_TYPE: {
            bitset_container_t *bc = CAST_bitset(c);
            bc->cardinality = bitset_container_compute_cardinality(bc);
            if (bc->cardinality <= DEFAULT_MAX_SIZE) {
                result = array_container_from_bitset(bc);
                bitset_container_free(bc);
                *type = ARRAY_CONTAINER_TYPE;
                return result;
            }
            return c;
        }
        case ARRAY_CONTAINER_TYPE:
            return c;  // nothing to do
        case RUN_CONTAINER_TYPE:
            return convert_run_to_efficient_container_and_free(CAST_run(c),
                                                               type);
        case SHARED_CONTAINER_TYPE:
            assert(false);
    }
    assert(false);
    roaring_unreachable;
    return 0;  // unreached
}

/**
 * Writes the underlying array to buf, outputs how many bytes were written.
 * This is meant to be byte-by-byte compatible with the Java and Go versions of
 * Roaring.
 * The number of bytes written should be
 * container_write(container, buf).
 *
 */
static inline int32_t container_write(const container_t *c, uint8_t typecode,
                                      char *buf) {
    c = container_unwrap_shared(c, &typecode);
    switch (typecode) {
        case BITSET_CONTAINER_TYPE:
            return bitset_container_write(const_CAST_bitset(c), buf);
        case ARRAY_CONTAINER_TYPE:
            return array_container_write(const_CAST_array(c), buf);
        case RUN_CONTAINER_TYPE:
            return run_container_write(const_CAST_run(c), buf);
    }
    assert(false);
    roaring_unreachable;
    return 0;  // unreached
}

/**
 * Get the container size in bytes under portable serialization (see
 * container_write), requires a
 * typecode
 */
static inline int32_t container_size_in_bytes(const container_t *c,
                                              uint8_t typecode) {
    c = container_unwrap_shared(c, &typecode);
    switch (typecode) {
        case BITSET_CONTAINER_TYPE:
            return bitset_container_size_in_bytes(const_CAST_bitset(c));
        case ARRAY_CONTAINER_TYPE:
            return array_container_size_in_bytes(const_CAST_array(c));
        case RUN_CONTAINER_TYPE:
            return run_container_size_in_bytes(const_CAST_run(c));
    }
    assert(false);
    roaring_unreachable;
    return 0;  // unreached
}

/**
 * print the container (useful for debugging), requires a  typecode
 */
void container_printf(const container_t *container, uint8_t typecode);

/**
 * print the content of the container as a comma-separated list of 32-bit values
 * starting at base, requires a  typecode
 */
void container_printf_as_uint32_array(const container_t *container,
                                      uint8_t typecode, uint32_t base);

bool container_internal_validate(const container_t *container, uint8_t typecode,
                                 const char **reason);

/**
 * Checks whether a container is not empty, requires a  typecode
 */
static inline bool container_nonzero_cardinality(const container_t *c,
                                                 uint8_t typecode) {
    c = container_unwrap_shared(c, &typecode);
    switch (typecode) {
        case BITSET_CONTAINER_TYPE:
            return bitset_container_const_nonzero_cardinality(
                const_CAST_bitset(c));
        case ARRAY_CONTAINER_TYPE:
            return array_container_nonzero_cardinality(const_CAST_array(c));
        case RUN_CONTAINER_TYPE:
            return run_container_nonzero_cardinality(const_CAST_run(c));
    }
    assert(false);
    roaring_unreachable;
    return 0;  // unreached
}

/**
 * Recover memory from a container, requires a  typecode
 */
void container_free(container_t *container, uint8_t typecode);

/**
 * Convert a container to an array of values, requires a  typecode as well as a
 * "base" (most significant values)
 * Returns number of ints added.
 */
static inline int container_to_uint32_array(uint32_t *output,
                                            const container_t *c,
                                            uint8_t typecode, uint32_t base) {
    c = container_unwrap_shared(c, &typecode);
    switch (typecode) {
        case BITSET_CONTAINER_TYPE:
            return bitset_container_to_uint32_array(output,
                                                    const_CAST_bitset(c), base);
        case ARRAY_CONTAINER_TYPE:
            return array_container_to_uint32_array(output, const_CAST_array(c),
                                                   base);
        case RUN_CONTAINER_TYPE:
            return run_container_to_uint32_array(output, const_CAST_run(c),
                                                 base);
    }
    assert(false);
    roaring_unreachable;
    return 0;  // unreached
}

/**
 * Add a value to a container, requires a  typecode, fills in new_typecode and
 * return (possibly different) container.
 * This function may allocate a new container, and caller is responsible for
 * memory deallocation
 */
static inline container_t *container_add(
    container_t *c, uint16_t val,
    uint8_t typecode,  // !!! should be second argument?
    uint8_t *new_typecode) {
    c = get_writable_copy_if_shared(c, &typecode);
    switch (typecode) {
        case BITSET_CONTAINER_TYPE:
            bitset_container_set(CAST_bitset(c), val);
            *new_typecode = BITSET_CONTAINER_TYPE;
            return c;
        case ARRAY_CONTAINER_TYPE: {
            array_container_t *ac = CAST_array(c);
            if (array_container_try_add(ac, val, DEFAULT_MAX_SIZE) != -1) {
                *new_typecode = ARRAY_CONTAINER_TYPE;
                return ac;
            } else {
                bitset_container_t *bitset = bitset_container_from_array(ac);
                bitset_container_add(bitset, val);
                *new_typecode = BITSET_CONTAINER_TYPE;
                return bitset;
            }
        } break;
        case RUN_CONTAINER_TYPE:
            // per Java, no container type adjustments are done (revisit?)
            run_container_add(CAST_run(c), val);
            *new_typecode = RUN_CONTAINER_TYPE;
            return c;
        default:
            assert(false);
            roaring_unreachable;
            return NULL;
    }
}

/**
 * Remove a value from a container, requires a  typecode, fills in new_typecode
 * and
 * return (possibly different) container.
 * This function may allocate a new container, and caller is responsible for
 * memory deallocation
 */
static inline container_t *container_remove(
    container_t *c, uint16_t val,
    uint8_t typecode,  // !!! should be second argument?
    uint8_t *new_typecode) {
    c = get_writable_copy_if_shared(c, &typecode);
    switch (typecode) {
        case BITSET_CONTAINER_TYPE:
            if (bitset_container_remove(CAST_bitset(c), val)) {
                int card = bitset_container_cardinality(CAST_bitset(c));
                if (card <= DEFAULT_MAX_SIZE) {
                    *new_typecode = ARRAY_CONTAINER_TYPE;
                    return array_container_from_bitset(CAST_bitset(c));
                }
            }
            *new_typecode = typecode;
            return c;
        case ARRAY_CONTAINER_TYPE:
            *new_typecode = typecode;
            array_container_remove(CAST_array(c), val);
            return c;
        case RUN_CONTAINER_TYPE:
            // per Java, no container type adjustments are done (revisit?)
            run_container_remove(CAST_run(c), val);
            *new_typecode = RUN_CONTAINER_TYPE;
            return c;
        default:
            assert(false);
            roaring_unreachable;
            return NULL;
    }
}

/**
 * Check whether a value is in a container, requires a typecode
 */
inline bool container_contains(
    const container_t *c, uint16_t val,
    uint8_t typecode  // !!! should be second argument?
) {
    if (typecode == SHARED_CONTAINER_TYPE) {
        typecode = const_CAST_shared(c)->typecode;
        assert(typecode != SHARED_CONTAINER_TYPE);
        c = const_CAST_shared(c)->container;
    }

    switch (typecode) {
        case BITSET_CONTAINER_TYPE:
            return bitset_container_get(const_CAST_bitset(c), val);
        case ARRAY_CONTAINER_TYPE:
            return array_container_contains(const_CAST_array(c), val);
        case RUN_CONTAINER_TYPE:
            return run_container_contains(const_CAST_run(c), val);
        default:
            assert(false);
            roaring_unreachable;
            return false;
    }
}

/**
 * Check whether a range of values from range_start (included) to range_end
 * (excluded) is in a container, requires a typecode
 */
static inline bool container_contains_range(
    const container_t *c, uint32_t range_start, uint32_t range_end,
    uint8_t typecode  // !!! should be second argument?
) {
    c = container_unwrap_shared(c, &typecode);
    switch (typecode) {
        case BITSET_CONTAINER_TYPE:
            return bitset_container_get_range(const_CAST_bitset(c), range_start,
                                              range_end);
        case ARRAY_CONTAINER_TYPE:
            return array_container_contains_range(const_CAST_array(c),
                                                  range_start, range_end);
        case RUN_CONTAINER_TYPE:
            return run_container_contains_range(const_CAST_run(c), range_start,
                                                range_end);
        default:
            assert(false);
            roaring_unreachable;
            return false;
    }
}

/**
 * Returns true if the two containers have the same content. Note that
 * two containers having different types can be "equal" in this sense.
 */
static inline bool container_equals(const container_t *c1, uint8_t type1,
                                    const container_t *c2, uint8_t type2) {
    c1 = container_unwrap_shared(c1, &type1);
    c2 = container_unwrap_shared(c2, &type2);
    switch (PAIR_CONTAINER_TYPES(type1, type2)) {
        case CONTAINER_PAIR(BITSET, BITSET):
            return bitset_container_equals(const_CAST_bitset(c1),
                                           const_CAST_bitset(c2));

        case CONTAINER_PAIR(BITSET, RUN):
            return run_container_equals_bitset(const_CAST_run(c2),
                                               const_CAST_bitset(c1));

        case CONTAINER_PAIR(RUN, BITSET):
            return run_container_equals_bitset(const_CAST_run(c1),
                                               const_CAST_bitset(c2));

        case CONTAINER_PAIR(BITSET, ARRAY):
            // java would always return false?
            return array_container_equal_bitset(const_CAST_array(c2),
                                                const_CAST_bitset(c1));

        case CONTAINER_PAIR(ARRAY, BITSET):
            // java would always return false?
            return array_container_equal_bitset(const_CAST_array(c1),
                                                const_CAST_bitset(c2));

        case CONTAINER_PAIR(ARRAY, RUN):
            return run_container_equals_array(const_CAST_run(c2),
                                              const_CAST_array(c1));

        case CONTAINER_PAIR(RUN, ARRAY):
            return run_container_equals_array(const_CAST_run(c1),
                                              const_CAST_array(c2));

        case CONTAINER_PAIR(ARRAY, ARRAY):
            return array_container_equals(const_CAST_array(c1),
                                          const_CAST_array(c2));

        case CONTAINER_PAIR(RUN, RUN):
            return run_container_equals(const_CAST_run(c1), const_CAST_run(c2));

        default:
            assert(false);
            roaring_unreachable;
            return false;
    }
}

/**
 * Returns true if the container c1 is a subset of the container c2. Note that
 * c1 can be a subset of c2 even if they have a different type.
 */
static inline bool container_is_subset(const container_t *c1, uint8_t type1,
                                       const container_t *c2, uint8_t type2) {
    c1 = container_unwrap_shared(c1, &type1);
    c2 = container_unwrap_shared(c2, &type2);
    switch (PAIR_CONTAINER_TYPES(type1, type2)) {
        case CONTAINER_PAIR(BITSET, BITSET):
            return bitset_container_is_subset(const_CAST_bitset(c1),
                                              const_CAST_bitset(c2));

        case CONTAINER_PAIR(BITSET, RUN):
            return bitset_container_is_subset_run(const_CAST_bitset(c1),
                                                  const_CAST_run(c2));

        case CONTAINER_PAIR(RUN, BITSET):
            return run_container_is_subset_bitset(const_CAST_run(c1),
                                                  const_CAST_bitset(c2));

        case CONTAINER_PAIR(BITSET, ARRAY):
            return false;  // by construction, size(c1) > size(c2)

        case CONTAINER_PAIR(ARRAY, BITSET):
            return array_container_is_subset_bitset(const_CAST_array(c1),
                                                    const_CAST_bitset(c2));

        case CONTAINER_PAIR(ARRAY, RUN):
            return array_container_is_subset_run(const_CAST_array(c1),
                                                 const_CAST_run(c2));

        case CONTAINER_PAIR(RUN, ARRAY):
            return run_container_is_subset_array(const_CAST_run(c1),
                                                 const_CAST_array(c2));

        case CONTAINER_PAIR(ARRAY, ARRAY):
            return array_container_is_subset(const_CAST_array(c1),
                                             const_CAST_array(c2));

        case CONTAINER_PAIR(RUN, RUN):
            return run_container_is_subset(const_CAST_run(c1),
                                           const_CAST_run(c2));

        default:
            assert(false);
            roaring_unreachable;
            return false;
    }
}

// macro-izations possibilities for generic non-inplace binary-op dispatch

/**
 * Compute intersection between two containers, generate a new container (having
 * type result_type), requires a typecode. This allocates new memory, caller
 * is responsible for deallocation.
 */
static inline container_t *container_and(const container_t *c1, uint8_t type1,
                                         const container_t *c2, uint8_t type2,
                                         uint8_t *result_type) {
    c1 = container_unwrap_shared(c1, &type1);
    c2 = container_unwrap_shared(c2, &type2);
    container_t *result = NULL;
    switch (PAIR_CONTAINER_TYPES(type1, type2)) {
        case CONTAINER_PAIR(BITSET, BITSET):
            *result_type =
                bitset_bitset_container_intersection(
                    const_CAST_bitset(c1), const_CAST_bitset(c2), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, ARRAY):
            result = array_container_create();
            array_container_intersection(
                const_CAST_array(c1), const_CAST_array(c2), CAST_array(result));
            *result_type = ARRAY_CONTAINER_TYPE;  // never bitset
            return result;

        case CONTAINER_PAIR(RUN, RUN):
            result = run_container_create();
            run_container_intersection(const_CAST_run(c1), const_CAST_run(c2),
                                       CAST_run(result));
            return convert_run_to_efficient_container_and_free(CAST_run(result),
                                                               result_type);

        case CONTAINER_PAIR(BITSET, ARRAY):
            result = array_container_create();
            array_bitset_container_intersection(const_CAST_array(c2),
                                                const_CAST_bitset(c1),
                                                CAST_array(result));
            *result_type = ARRAY_CONTAINER_TYPE;  // never bitset
            return result;

        case CONTAINER_PAIR(ARRAY, BITSET):
            result = array_container_create();
            *result_type = ARRAY_CONTAINER_TYPE;  // never bitset
            array_bitset_container_intersection(const_CAST_array(c1),
                                                const_CAST_bitset(c2),
                                                CAST_array(result));
            return result;

        case CONTAINER_PAIR(BITSET, RUN):
            *result_type =
                run_bitset_container_intersection(
                    const_CAST_run(c2), const_CAST_bitset(c1), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(RUN, BITSET):
            *result_type =
                run_bitset_container_intersection(
                    const_CAST_run(c1), const_CAST_bitset(c2), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, RUN):
            result = array_container_create();
            *result_type = ARRAY_CONTAINER_TYPE;  // never bitset
            array_run_container_intersection(
                const_CAST_array(c1), const_CAST_run(c2), CAST_array(result));
            return result;

        case CONTAINER_PAIR(RUN, ARRAY):
            result = array_container_create();
            *result_type = ARRAY_CONTAINER_TYPE;  // never bitset
            array_run_container_intersection(
                const_CAST_array(c2), const_CAST_run(c1), CAST_array(result));
            return result;

        default:
            assert(false);
            roaring_unreachable;
            return NULL;
    }
}

/**
 * Compute the size of the intersection between two containers.
 */
static inline int container_and_cardinality(const container_t *c1,
                                            uint8_t type1,
                                            const container_t *c2,
                                            uint8_t type2) {
    c1 = container_unwrap_shared(c1, &type1);
    c2 = container_unwrap_shared(c2, &type2);
    switch (PAIR_CONTAINER_TYPES(type1, type2)) {
        case CONTAINER_PAIR(BITSET, BITSET):
            return bitset_container_and_justcard(const_CAST_bitset(c1),
                                                 const_CAST_bitset(c2));

        case CONTAINER_PAIR(ARRAY, ARRAY):
            return array_container_intersection_cardinality(
                const_CAST_array(c1), const_CAST_array(c2));

        case CONTAINER_PAIR(RUN, RUN):
            return run_container_intersection_cardinality(const_CAST_run(c1),
                                                          const_CAST_run(c2));

        case CONTAINER_PAIR(BITSET, ARRAY):
            return array_bitset_container_intersection_cardinality(
                const_CAST_array(c2), const_CAST_bitset(c1));

        case CONTAINER_PAIR(ARRAY, BITSET):
            return array_bitset_container_intersection_cardinality(
                const_CAST_array(c1), const_CAST_bitset(c2));

        case CONTAINER_PAIR(BITSET, RUN):
            return run_bitset_container_intersection_cardinality(
                const_CAST_run(c2), const_CAST_bitset(c1));

        case CONTAINER_PAIR(RUN, BITSET):
            return run_bitset_container_intersection_cardinality(
                const_CAST_run(c1), const_CAST_bitset(c2));

        case CONTAINER_PAIR(ARRAY, RUN):
            return array_run_container_intersection_cardinality(
                const_CAST_array(c1), const_CAST_run(c2));

        case CONTAINER_PAIR(RUN, ARRAY):
            return array_run_container_intersection_cardinality(
                const_CAST_array(c2), const_CAST_run(c1));

        default:
            assert(false);
            roaring_unreachable;
            return 0;
    }
}

/**
 * Check whether two containers intersect.
 */
static inline bool container_intersect(const container_t *c1, uint8_t type1,
                                       const container_t *c2, uint8_t type2) {
    c1 = container_unwrap_shared(c1, &type1);
    c2 = container_unwrap_shared(c2, &type2);
    switch (PAIR_CONTAINER_TYPES(type1, type2)) {
        case CONTAINER_PAIR(BITSET, BITSET):
            return bitset_container_intersect(const_CAST_bitset(c1),
                                              const_CAST_bitset(c2));

        case CONTAINER_PAIR(ARRAY, ARRAY):
            return array_container_intersect(const_CAST_array(c1),
                                             const_CAST_array(c2));

        case CONTAINER_PAIR(RUN, RUN):
            return run_container_intersect(const_CAST_run(c1),
                                           const_CAST_run(c2));

        case CONTAINER_PAIR(BITSET, ARRAY):
            return array_bitset_container_intersect(const_CAST_array(c2),
                                                    const_CAST_bitset(c1));

        case CONTAINER_PAIR(ARRAY, BITSET):
            return array_bitset_container_intersect(const_CAST_array(c1),
                                                    const_CAST_bitset(c2));

        case CONTAINER_PAIR(BITSET, RUN):
            return run_bitset_container_intersect(const_CAST_run(c2),
                                                  const_CAST_bitset(c1));

        case CONTAINER_PAIR(RUN, BITSET):
            return run_bitset_container_intersect(const_CAST_run(c1),
                                                  const_CAST_bitset(c2));

        case CONTAINER_PAIR(ARRAY, RUN):
            return array_run_container_intersect(const_CAST_array(c1),
                                                 const_CAST_run(c2));

        case CONTAINER_PAIR(RUN, ARRAY):
            return array_run_container_intersect(const_CAST_array(c2),
                                                 const_CAST_run(c1));

        default:
            assert(false);
            roaring_unreachable;
            return 0;
    }
}

/**
 * Compute intersection between two containers, with result in the first
 container if possible. If the returned pointer is identical to c1,
 then the container has been modified. If the returned pointer is different
 from c1, then a new container has been created and the caller is responsible
 for freeing it.
 The type of the first container may change. Returns the modified
 (and possibly new) container.
*/
static inline container_t *container_iand(container_t *c1, uint8_t type1,
                                          const container_t *c2, uint8_t type2,
                                          uint8_t *result_type) {
    c1 = get_writable_copy_if_shared(c1, &type1);
    c2 = container_unwrap_shared(c2, &type2);
    container_t *result = NULL;
    switch (PAIR_CONTAINER_TYPES(type1, type2)) {
        case CONTAINER_PAIR(BITSET, BITSET):
            *result_type = bitset_bitset_container_intersection_inplace(
                               CAST_bitset(c1), const_CAST_bitset(c2), &result)
                               ? BITSET_CONTAINER_TYPE
                               : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, ARRAY):
            array_container_intersection_inplace(CAST_array(c1),
                                                 const_CAST_array(c2));
            *result_type = ARRAY_CONTAINER_TYPE;
            return c1;

        case CONTAINER_PAIR(RUN, RUN):
            result = run_container_create();
            run_container_intersection(const_CAST_run(c1), const_CAST_run(c2),
                                       CAST_run(result));
            // as of January 2016, Java code used non-in-place intersection for
            // two runcontainers
            return convert_run_to_efficient_container_and_free(CAST_run(result),
                                                               result_type);

        case CONTAINER_PAIR(BITSET, ARRAY):
            // c1 is a bitmap so no inplace possible
            result = array_container_create();
            array_bitset_container_intersection(const_CAST_array(c2),
                                                const_CAST_bitset(c1),
                                                CAST_array(result));
            *result_type = ARRAY_CONTAINER_TYPE;  // never bitset
            return result;

        case CONTAINER_PAIR(ARRAY, BITSET):
            *result_type = ARRAY_CONTAINER_TYPE;  // never bitset
            array_bitset_container_intersection(
                const_CAST_array(c1), const_CAST_bitset(c2),
                CAST_array(c1));  // result is allowed to be same as c1
            return c1;

        case CONTAINER_PAIR(BITSET, RUN):
            // will attempt in-place computation
            *result_type = run_bitset_container_intersection(
                               const_CAST_run(c2), const_CAST_bitset(c1), &c1)
                               ? BITSET_CONTAINER_TYPE
                               : ARRAY_CONTAINER_TYPE;
            return c1;

        case CONTAINER_PAIR(RUN, BITSET):
            *result_type =
                run_bitset_container_intersection(
                    const_CAST_run(c1), const_CAST_bitset(c2), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, RUN):
            result = array_container_create();
            *result_type = ARRAY_CONTAINER_TYPE;  // never bitset
            array_run_container_intersection(
                const_CAST_array(c1), const_CAST_run(c2), CAST_array(result));
            return result;

        case CONTAINER_PAIR(RUN, ARRAY):
            result = array_container_create();
            *result_type = ARRAY_CONTAINER_TYPE;  // never bitset
            array_run_container_intersection(
                const_CAST_array(c2), const_CAST_run(c1), CAST_array(result));
            return result;

        default:
            assert(false);
            roaring_unreachable;
            return NULL;
    }
}

/**
 * Compute union between two containers, generate a new container (having type
 * result_type), requires a typecode. This allocates new memory, caller
 * is responsible for deallocation.
 */
static inline container_t *container_or(const container_t *c1, uint8_t type1,
                                        const container_t *c2, uint8_t type2,
                                        uint8_t *result_type) {
    c1 = container_unwrap_shared(c1, &type1);
    c2 = container_unwrap_shared(c2, &type2);
    container_t *result = NULL;
    switch (PAIR_CONTAINER_TYPES(type1, type2)) {
        case CONTAINER_PAIR(BITSET, BITSET):
            result = bitset_container_create();
            bitset_container_or(const_CAST_bitset(c1), const_CAST_bitset(c2),
                                CAST_bitset(result));
            *result_type = BITSET_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, ARRAY):
            *result_type =
                array_array_container_union(const_CAST_array(c1),
                                            const_CAST_array(c2), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(RUN, RUN):
            result = run_container_create();
            run_container_union(const_CAST_run(c1), const_CAST_run(c2),
                                CAST_run(result));
            *result_type = RUN_CONTAINER_TYPE;
            // todo: could be optimized since will never convert to array
            result = convert_run_to_efficient_container_and_free(
                CAST_run(result), result_type);
            return result;

        case CONTAINER_PAIR(BITSET, ARRAY):
            result = bitset_container_create();
            array_bitset_container_union(const_CAST_array(c2),
                                         const_CAST_bitset(c1),
                                         CAST_bitset(result));
            *result_type = BITSET_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, BITSET):
            result = bitset_container_create();
            array_bitset_container_union(const_CAST_array(c1),
                                         const_CAST_bitset(c2),
                                         CAST_bitset(result));
            *result_type = BITSET_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(BITSET, RUN):
            if (run_container_is_full(const_CAST_run(c2))) {
                result = run_container_create();
                *result_type = RUN_CONTAINER_TYPE;
                run_container_copy(const_CAST_run(c2), CAST_run(result));
                return result;
            }
            result = bitset_container_create();
            run_bitset_container_union(
                const_CAST_run(c2), const_CAST_bitset(c1), CAST_bitset(result));
            *result_type = BITSET_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(RUN, BITSET):
            if (run_container_is_full(const_CAST_run(c1))) {
                result = run_container_create();
                *result_type = RUN_CONTAINER_TYPE;
                run_container_copy(const_CAST_run(c1), CAST_run(result));
                return result;
            }
            result = bitset_container_create();
            run_bitset_container_union(
                const_CAST_run(c1), const_CAST_bitset(c2), CAST_bitset(result));
            *result_type = BITSET_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, RUN):
            result = run_container_create();
            array_run_container_union(const_CAST_array(c1), const_CAST_run(c2),
                                      CAST_run(result));
            result = convert_run_to_efficient_container_and_free(
                CAST_run(result), result_type);
            return result;

        case CONTAINER_PAIR(RUN, ARRAY):
            result = run_container_create();
            array_run_container_union(const_CAST_array(c2), const_CAST_run(c1),
                                      CAST_run(result));
            result = convert_run_to_efficient_container_and_free(
                CAST_run(result), result_type);
            return result;

        default:
            assert(false);
            roaring_unreachable;
            return NULL;  // unreached
    }
}

/**
 * Compute union between two containers, generate a new container (having type
 * result_type), requires a typecode. This allocates new memory, caller
 * is responsible for deallocation.
 *
 * This lazy version delays some operations such as the maintenance of the
 * cardinality. It requires repair later on the generated containers.
 */
static inline container_t *container_lazy_or(const container_t *c1,
                                             uint8_t type1,
                                             const container_t *c2,
                                             uint8_t type2,
                                             uint8_t *result_type) {
    c1 = container_unwrap_shared(c1, &type1);
    c2 = container_unwrap_shared(c2, &type2);
    container_t *result = NULL;
    switch (PAIR_CONTAINER_TYPES(type1, type2)) {
        case CONTAINER_PAIR(BITSET, BITSET):
            result = bitset_container_create();
            bitset_container_or_nocard(const_CAST_bitset(c1),
                                       const_CAST_bitset(c2),
                                       CAST_bitset(result));  // is lazy
            *result_type = BITSET_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, ARRAY):
            *result_type =
                array_array_container_lazy_union(const_CAST_array(c1),
                                                 const_CAST_array(c2), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(RUN, RUN):
            result = run_container_create();
            run_container_union(const_CAST_run(c1), const_CAST_run(c2),
                                CAST_run(result));
            *result_type = RUN_CONTAINER_TYPE;
            // we are being lazy
            result = convert_run_to_efficient_container_and_free(
                CAST_run(result), result_type);
            return result;

        case CONTAINER_PAIR(BITSET, ARRAY):
            result = bitset_container_create();
            array_bitset_container_lazy_union(const_CAST_array(c2),
                                              const_CAST_bitset(c1),
                                              CAST_bitset(result));  // is lazy
            *result_type = BITSET_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, BITSET):
            result = bitset_container_create();
            array_bitset_container_lazy_union(const_CAST_array(c1),
                                              const_CAST_bitset(c2),
                                              CAST_bitset(result));  // is lazy
            *result_type = BITSET_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(BITSET, RUN):
            if (run_container_is_full(const_CAST_run(c2))) {
                result = run_container_create();
                *result_type = RUN_CONTAINER_TYPE;
                run_container_copy(const_CAST_run(c2), CAST_run(result));
                return result;
            }
            result = bitset_container_create();
            run_bitset_container_lazy_union(const_CAST_run(c2),
                                            const_CAST_bitset(c1),
                                            CAST_bitset(result));  // is lazy
            *result_type = BITSET_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(RUN, BITSET):
            if (run_container_is_full(const_CAST_run(c1))) {
                result = run_container_create();
                *result_type = RUN_CONTAINER_TYPE;
                run_container_copy(const_CAST_run(c1), CAST_run(result));
                return result;
            }
            result = bitset_container_create();
            run_bitset_container_lazy_union(const_CAST_run(c1),
                                            const_CAST_bitset(c2),
                                            CAST_bitset(result));  // is lazy
            *result_type = BITSET_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, RUN):
            result = run_container_create();
            array_run_container_union(const_CAST_array(c1), const_CAST_run(c2),
                                      CAST_run(result));
            *result_type = RUN_CONTAINER_TYPE;
            // next line skipped since we are lazy
            // result = convert_run_to_efficient_container(result, result_type);
            return result;

        case CONTAINER_PAIR(RUN, ARRAY):
            result = run_container_create();
            array_run_container_union(const_CAST_array(c2), const_CAST_run(c1),
                                      CAST_run(result));  // TODO make lazy
            *result_type = RUN_CONTAINER_TYPE;
            // next line skipped since we are lazy
            // result = convert_run_to_efficient_container(result, result_type);
            return result;

        default:
            assert(false);
            roaring_unreachable;
            return NULL;  // unreached
    }
}

/**
 * Compute the union between two containers, with result in the first container.
 * If the returned pointer is identical to c1, then the container has been
 * modified.
 * If the returned pointer is different from c1, then a new container has been
 * created and the caller is responsible for freeing it.
 * The type of the first container may change. Returns the modified
 * (and possibly new) container
 */
static inline container_t *container_ior(container_t *c1, uint8_t type1,
                                         const container_t *c2, uint8_t type2,
                                         uint8_t *result_type) {
    c1 = get_writable_copy_if_shared(c1, &type1);
    c2 = container_unwrap_shared(c2, &type2);
    container_t *result = NULL;
    switch (PAIR_CONTAINER_TYPES(type1, type2)) {
        case CONTAINER_PAIR(BITSET, BITSET):
            bitset_container_or(const_CAST_bitset(c1), const_CAST_bitset(c2),
                                CAST_bitset(c1));
#ifdef OR_BITSET_CONVERSION_TO_FULL
            if (CAST_bitset(c1)->cardinality == (1 << 16)) {  // we convert
                result = run_container_create_range(0, (1 << 16));
                *result_type = RUN_CONTAINER_TYPE;
                return result;
            }
#endif
            *result_type = BITSET_CONTAINER_TYPE;
            return c1;

        case CONTAINER_PAIR(ARRAY, ARRAY):
            *result_type = array_array_container_inplace_union(
                               CAST_array(c1), const_CAST_array(c2), &result)
                               ? BITSET_CONTAINER_TYPE
                               : ARRAY_CONTAINER_TYPE;
            if ((result == NULL) && (*result_type == ARRAY_CONTAINER_TYPE)) {
                return c1;  // the computation was done in-place!
            }
            return result;

        case CONTAINER_PAIR(RUN, RUN):
            run_container_union_inplace(CAST_run(c1), const_CAST_run(c2));
            return convert_run_to_efficient_container(CAST_run(c1),
                                                      result_type);

        case CONTAINER_PAIR(BITSET, ARRAY):
            array_bitset_container_union(
                const_CAST_array(c2), const_CAST_bitset(c1), CAST_bitset(c1));
            *result_type = BITSET_CONTAINER_TYPE;  // never array
            return c1;

        case CONTAINER_PAIR(ARRAY, BITSET):
            // c1 is an array, so no in-place possible
            result = bitset_container_create();
            *result_type = BITSET_CONTAINER_TYPE;
            array_bitset_container_union(const_CAST_array(c1),
                                         const_CAST_bitset(c2),
                                         CAST_bitset(result));
            return result;

        case CONTAINER_PAIR(BITSET, RUN):
            if (run_container_is_full(const_CAST_run(c2))) {
                result = run_container_create();
                *result_type = RUN_CONTAINER_TYPE;
                run_container_copy(const_CAST_run(c2), CAST_run(result));
                return result;
            }
            run_bitset_container_union(const_CAST_run(c2),
                                       const_CAST_bitset(c1),
                                       CAST_bitset(c1));  // allowed
            *result_type = BITSET_CONTAINER_TYPE;
            return c1;

        case CONTAINER_PAIR(RUN, BITSET):
            if (run_container_is_full(const_CAST_run(c1))) {
                *result_type = RUN_CONTAINER_TYPE;
                return c1;
            }
            result = bitset_container_create();
            run_bitset_container_union(
                const_CAST_run(c1), const_CAST_bitset(c2), CAST_bitset(result));
            *result_type = BITSET_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, RUN):
            result = run_container_create();
            array_run_container_union(const_CAST_array(c1), const_CAST_run(c2),
                                      CAST_run(result));
            result = convert_run_to_efficient_container_and_free(
                CAST_run(result), result_type);
            return result;

        case CONTAINER_PAIR(RUN, ARRAY):
            array_run_container_inplace_union(const_CAST_array(c2),
                                              CAST_run(c1));
            c1 = convert_run_to_efficient_container(CAST_run(c1), result_type);
            return c1;

        default:
            assert(false);
            roaring_unreachable;
            return NULL;
    }
}

/**
 * Compute the union between two containers, with result in the first container.
 * If the returned pointer is identical to c1, then the container has been
 * modified.
 * If the returned pointer is different from c1, then a new container has been
 * created and the caller is responsible for freeing it.
 * The type of the first container may change. Returns the modified
 * (and possibly new) container
 *
 * This lazy version delays some operations such as the maintenance of the
 * cardinality. It requires repair later on the generated containers.
 */
static inline container_t *container_lazy_ior(container_t *c1, uint8_t type1,
                                              const container_t *c2,
                                              uint8_t type2,
                                              uint8_t *result_type) {
    assert(type1 != SHARED_CONTAINER_TYPE);
    // c1 = get_writable_copy_if_shared(c1,&type1);
    c2 = container_unwrap_shared(c2, &type2);
    container_t *result = NULL;
    switch (PAIR_CONTAINER_TYPES(type1, type2)) {
        case CONTAINER_PAIR(BITSET, BITSET):
#ifdef LAZY_OR_BITSET_CONVERSION_TO_FULL
            // if we have two bitsets, we might as well compute the cardinality
            bitset_container_or(const_CAST_bitset(c1), const_CAST_bitset(c2),
                                CAST_bitset(c1));
            // it is possible that two bitsets can lead to a full container
            if (CAST_bitset(c1)->cardinality == (1 << 16)) {  // we convert
                result = run_container_create_range(0, (1 << 16));
                *result_type = RUN_CONTAINER_TYPE;
                return result;
            }
#else
            bitset_container_or_nocard(const_CAST_bitset(c1),
                                       const_CAST_bitset(c2), CAST_bitset(c1));

#endif
            *result_type = BITSET_CONTAINER_TYPE;
            return c1;

        case CONTAINER_PAIR(ARRAY, ARRAY):
            *result_type = array_array_container_lazy_inplace_union(
                               CAST_array(c1), const_CAST_array(c2), &result)
                               ? BITSET_CONTAINER_TYPE
                               : ARRAY_CONTAINER_TYPE;
            if ((result == NULL) && (*result_type == ARRAY_CONTAINER_TYPE)) {
                return c1;  // the computation was done in-place!
            }
            return result;

        case CONTAINER_PAIR(RUN, RUN):
            run_container_union_inplace(CAST_run(c1), const_CAST_run(c2));
            *result_type = RUN_CONTAINER_TYPE;
            return convert_run_to_efficient_container(CAST_run(c1),
                                                      result_type);

        case CONTAINER_PAIR(BITSET, ARRAY):
            array_bitset_container_lazy_union(const_CAST_array(c2),
                                              const_CAST_bitset(c1),
                                              CAST_bitset(c1));  // is lazy
            *result_type = BITSET_CONTAINER_TYPE;                // never array
            return c1;

        case CONTAINER_PAIR(ARRAY, BITSET):
            // c1 is an array, so no in-place possible
            result = bitset_container_create();
            *result_type = BITSET_CONTAINER_TYPE;
            array_bitset_container_lazy_union(const_CAST_array(c1),
                                              const_CAST_bitset(c2),
                                              CAST_bitset(result));  // is lazy
            return result;

        case CONTAINER_PAIR(BITSET, RUN):
            if (run_container_is_full(const_CAST_run(c2))) {
                result = run_container_create();
                *result_type = RUN_CONTAINER_TYPE;
                run_container_copy(const_CAST_run(c2), CAST_run(result));
                return result;
            }
            run_bitset_container_lazy_union(
                const_CAST_run(c2), const_CAST_bitset(c1),
                CAST_bitset(c1));  // allowed //  lazy
            *result_type = BITSET_CONTAINER_TYPE;
            return c1;

        case CONTAINER_PAIR(RUN, BITSET):
            if (run_container_is_full(const_CAST_run(c1))) {
                *result_type = RUN_CONTAINER_TYPE;
                return c1;
            }
            result = bitset_container_create();
            run_bitset_container_lazy_union(const_CAST_run(c1),
                                            const_CAST_bitset(c2),
                                            CAST_bitset(result));  //  lazy
            *result_type = BITSET_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, RUN):
            result = run_container_create();
            array_run_container_union(const_CAST_array(c1), const_CAST_run(c2),
                                      CAST_run(result));
            *result_type = RUN_CONTAINER_TYPE;
            // next line skipped since we are lazy
            // result = convert_run_to_efficient_container_and_free(result,
            // result_type);
            return result;

        case CONTAINER_PAIR(RUN, ARRAY):
            array_run_container_inplace_union(const_CAST_array(c2),
                                              CAST_run(c1));
            *result_type = RUN_CONTAINER_TYPE;
            // next line skipped since we are lazy
            // result = convert_run_to_efficient_container_and_free(result,
            // result_type);
            return c1;

        default:
            assert(false);
            roaring_unreachable;
            return NULL;
    }
}

/**
 * Compute symmetric difference (xor) between two containers, generate a new
 * container (having type result_type), requires a typecode. This allocates new
 * memory, caller is responsible for deallocation.
 */
static inline container_t *container_xor(const container_t *c1, uint8_t type1,
                                         const container_t *c2, uint8_t type2,
                                         uint8_t *result_type) {
    c1 = container_unwrap_shared(c1, &type1);
    c2 = container_unwrap_shared(c2, &type2);
    container_t *result = NULL;
    switch (PAIR_CONTAINER_TYPES(type1, type2)) {
        case CONTAINER_PAIR(BITSET, BITSET):
            *result_type =
                bitset_bitset_container_xor(const_CAST_bitset(c1),
                                            const_CAST_bitset(c2), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, ARRAY):
            *result_type =
                array_array_container_xor(const_CAST_array(c1),
                                          const_CAST_array(c2), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(RUN, RUN):
            *result_type = (uint8_t)run_run_container_xor(
                const_CAST_run(c1), const_CAST_run(c2), &result);
            return result;

        case CONTAINER_PAIR(BITSET, ARRAY):
            *result_type =
                array_bitset_container_xor(const_CAST_array(c2),
                                           const_CAST_bitset(c1), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, BITSET):
            *result_type =
                array_bitset_container_xor(const_CAST_array(c1),
                                           const_CAST_bitset(c2), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(BITSET, RUN):
            *result_type =
                run_bitset_container_xor(const_CAST_run(c2),
                                         const_CAST_bitset(c1), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(RUN, BITSET):
            *result_type =
                run_bitset_container_xor(const_CAST_run(c1),
                                         const_CAST_bitset(c2), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, RUN):
            *result_type = (uint8_t)array_run_container_xor(
                const_CAST_array(c1), const_CAST_run(c2), &result);
            return result;

        case CONTAINER_PAIR(RUN, ARRAY):
            *result_type = (uint8_t)array_run_container_xor(
                const_CAST_array(c2), const_CAST_run(c1), &result);
            return result;

        default:
            assert(false);
            roaring_unreachable;
            return NULL;  // unreached
    }
}

/* Applies an offset to the non-empty container 'c'.
 * The results are stored in new containers returned via 'lo' and 'hi', for the
 * low and high halves of the result (where the low half matches the original
 * key and the high one corresponds to values for the following key). Either one
 * of 'lo' and 'hi' are allowed to be 'NULL', but not both. Whenever one of them
 * is not 'NULL', it should point to a 'NULL' container. Whenever one of them is
 * 'NULL' the shifted elements for that part will not be computed. If either of
 * the resulting containers turns out to be empty, the pointed container will
 * remain 'NULL'.
 */
static inline void container_add_offset(const container_t *c, uint8_t type,
                                        container_t **lo, container_t **hi,
                                        uint16_t offset) {
    assert(offset != 0);
    assert(container_nonzero_cardinality(c, type));
    assert(lo != NULL || hi != NULL);
    assert(lo == NULL || *lo == NULL);
    assert(hi == NULL || *hi == NULL);

    switch (type) {
        case BITSET_CONTAINER_TYPE:
            bitset_container_offset(const_CAST_bitset(c), lo, hi, offset);
            break;
        case ARRAY_CONTAINER_TYPE:
            array_container_offset(const_CAST_array(c), lo, hi, offset);
            break;
        case RUN_CONTAINER_TYPE:
            run_container_offset(const_CAST_run(c), lo, hi, offset);
            break;
        default:
            assert(false);
            roaring_unreachable;
            break;
    }
}

/**
 * Compute xor between two containers, generate a new container (having type
 * result_type), requires a typecode. This allocates new memory, caller
 * is responsible for deallocation.
 *
 * This lazy version delays some operations such as the maintenance of the
 * cardinality. It requires repair later on the generated containers.
 */
static inline container_t *container_lazy_xor(const container_t *c1,
                                              uint8_t type1,
                                              const container_t *c2,
                                              uint8_t type2,
                                              uint8_t *result_type) {
    c1 = container_unwrap_shared(c1, &type1);
    c2 = container_unwrap_shared(c2, &type2);
    container_t *result = NULL;
    switch (PAIR_CONTAINER_TYPES(type1, type2)) {
        case CONTAINER_PAIR(BITSET, BITSET):
            result = bitset_container_create();
            bitset_container_xor_nocard(const_CAST_bitset(c1),
                                        const_CAST_bitset(c2),
                                        CAST_bitset(result));  // is lazy
            *result_type = BITSET_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, ARRAY):
            *result_type =
                array_array_container_lazy_xor(const_CAST_array(c1),
                                               const_CAST_array(c2), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(RUN, RUN):
            // nothing special done yet.
            *result_type = (uint8_t)run_run_container_xor(
                const_CAST_run(c1), const_CAST_run(c2), &result);
            return result;

        case CONTAINER_PAIR(BITSET, ARRAY):
            result = bitset_container_create();
            *result_type = BITSET_CONTAINER_TYPE;
            array_bitset_container_lazy_xor(const_CAST_array(c2),
                                            const_CAST_bitset(c1),
                                            CAST_bitset(result));
            return result;

        case CONTAINER_PAIR(ARRAY, BITSET):
            result = bitset_container_create();
            *result_type = BITSET_CONTAINER_TYPE;
            array_bitset_container_lazy_xor(const_CAST_array(c1),
                                            const_CAST_bitset(c2),
                                            CAST_bitset(result));
            return result;

        case CONTAINER_PAIR(BITSET, RUN):
            result = bitset_container_create();
            run_bitset_container_lazy_xor(
                const_CAST_run(c2), const_CAST_bitset(c1), CAST_bitset(result));
            *result_type = BITSET_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(RUN, BITSET):
            result = bitset_container_create();
            run_bitset_container_lazy_xor(
                const_CAST_run(c1), const_CAST_bitset(c2), CAST_bitset(result));
            *result_type = BITSET_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, RUN):
            result = run_container_create();
            array_run_container_lazy_xor(const_CAST_array(c1),
                                         const_CAST_run(c2), CAST_run(result));
            *result_type = RUN_CONTAINER_TYPE;
            // next line skipped since we are lazy
            // result = convert_run_to_efficient_container(result, result_type);
            return result;

        case CONTAINER_PAIR(RUN, ARRAY):
            result = run_container_create();
            array_run_container_lazy_xor(const_CAST_array(c2),
                                         const_CAST_run(c1), CAST_run(result));
            *result_type = RUN_CONTAINER_TYPE;
            // next line skipped since we are lazy
            // result = convert_run_to_efficient_container(result, result_type);
            return result;

        default:
            assert(false);
            roaring_unreachable;
            return NULL;  // unreached
    }
}

/**
 * Compute the xor between two containers, with result in the first container.
 * If the returned pointer is identical to c1, then the container has been
 * modified.
 * If the returned pointer is different from c1, then a new container has been
 * created. The original container is freed by container_ixor.
 * The type of the first container may change. Returns the modified (and
 * possibly new) container.
 */
static inline container_t *container_ixor(container_t *c1, uint8_t type1,
                                          const container_t *c2, uint8_t type2,
                                          uint8_t *result_type) {
    c1 = get_writable_copy_if_shared(c1, &type1);
    c2 = container_unwrap_shared(c2, &type2);
    container_t *result = NULL;
    switch (PAIR_CONTAINER_TYPES(type1, type2)) {
        case CONTAINER_PAIR(BITSET, BITSET):
            *result_type = bitset_bitset_container_ixor(
                               CAST_bitset(c1), const_CAST_bitset(c2), &result)
                               ? BITSET_CONTAINER_TYPE
                               : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, ARRAY):
            *result_type = array_array_container_ixor(
                               CAST_array(c1), const_CAST_array(c2), &result)
                               ? BITSET_CONTAINER_TYPE
                               : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(RUN, RUN):
            *result_type = (uint8_t)run_run_container_ixor(
                CAST_run(c1), const_CAST_run(c2), &result);
            return result;

        case CONTAINER_PAIR(BITSET, ARRAY):
            *result_type = bitset_array_container_ixor(
                               CAST_bitset(c1), const_CAST_array(c2), &result)
                               ? BITSET_CONTAINER_TYPE
                               : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, BITSET):
            *result_type = array_bitset_container_ixor(
                               CAST_array(c1), const_CAST_bitset(c2), &result)
                               ? BITSET_CONTAINER_TYPE
                               : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(BITSET, RUN):
            *result_type = bitset_run_container_ixor(
                               CAST_bitset(c1), const_CAST_run(c2), &result)
                               ? BITSET_CONTAINER_TYPE
                               : ARRAY_CONTAINER_TYPE;

            return result;

        case CONTAINER_PAIR(RUN, BITSET):
            *result_type = run_bitset_container_ixor(
                               CAST_run(c1), const_CAST_bitset(c2), &result)
                               ? BITSET_CONTAINER_TYPE
                               : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, RUN):
            *result_type = (uint8_t)array_run_container_ixor(
                CAST_array(c1), const_CAST_run(c2), &result);
            return result;

        case CONTAINER_PAIR(RUN, ARRAY):
            *result_type = (uint8_t)run_array_container_ixor(
                CAST_run(c1), const_CAST_array(c2), &result);
            return result;

        default:
            assert(false);
            roaring_unreachable;
            return NULL;
    }
}

/**
 * Compute the xor between two containers, with result in the first container.
 * If the returned pointer is identical to c1, then the container has been
 * modified.
 * If the returned pointer is different from c1, then a new container has been
 * created and the caller is responsible for freeing it.
 * The type of the first container may change. Returns the modified
 * (and possibly new) container
 *
 * This lazy version delays some operations such as the maintenance of the
 * cardinality. It requires repair later on the generated containers.
 */
static inline container_t *container_lazy_ixor(container_t *c1, uint8_t type1,
                                               const container_t *c2,
                                               uint8_t type2,
                                               uint8_t *result_type) {
    assert(type1 != SHARED_CONTAINER_TYPE);
    // c1 = get_writable_copy_if_shared(c1,&type1);
    c2 = container_unwrap_shared(c2, &type2);
    switch (PAIR_CONTAINER_TYPES(type1, type2)) {
        case CONTAINER_PAIR(BITSET, BITSET):
            bitset_container_xor_nocard(CAST_bitset(c1), const_CAST_bitset(c2),
                                        CAST_bitset(c1));  // is lazy
            *result_type = BITSET_CONTAINER_TYPE;
            return c1;

        // TODO: other cases being lazy, esp. when we know inplace not likely
        // could see the corresponding code for union
        default:
            // we may have a dirty bitset (without a precomputed cardinality)
            // and calling container_ixor on it might be unsafe.
            if (type1 == BITSET_CONTAINER_TYPE) {
                bitset_container_t *bc = CAST_bitset(c1);
                if (bc->cardinality == BITSET_UNKNOWN_CARDINALITY) {
                    bc->cardinality = bitset_container_compute_cardinality(bc);
                }
            }
            return container_ixor(c1, type1, c2, type2, result_type);
    }
}

/**
 * Compute difference (andnot) between two containers, generate a new
 * container (having type result_type), requires a typecode. This allocates new
 * memory, caller is responsible for deallocation.
 */
static inline container_t *container_andnot(const container_t *c1,
                                            uint8_t type1,
                                            const container_t *c2,
                                            uint8_t type2,
                                            uint8_t *result_type) {
    c1 = container_unwrap_shared(c1, &type1);
    c2 = container_unwrap_shared(c2, &type2);
    container_t *result = NULL;
    switch (PAIR_CONTAINER_TYPES(type1, type2)) {
        case CONTAINER_PAIR(BITSET, BITSET):
            *result_type =
                bitset_bitset_container_andnot(const_CAST_bitset(c1),
                                               const_CAST_bitset(c2), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, ARRAY):
            result = array_container_create();
            array_array_container_andnot(
                const_CAST_array(c1), const_CAST_array(c2), CAST_array(result));
            *result_type = ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(RUN, RUN):
            if (run_container_is_full(const_CAST_run(c2))) {
                result = array_container_create();
                *result_type = ARRAY_CONTAINER_TYPE;
                return result;
            }
            *result_type = (uint8_t)run_run_container_andnot(
                const_CAST_run(c1), const_CAST_run(c2), &result);
            return result;

        case CONTAINER_PAIR(BITSET, ARRAY):
            *result_type =
                bitset_array_container_andnot(const_CAST_bitset(c1),
                                              const_CAST_array(c2), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, BITSET):
            result = array_container_create();
            array_bitset_container_andnot(const_CAST_array(c1),
                                          const_CAST_bitset(c2),
                                          CAST_array(result));
            *result_type = ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(BITSET, RUN):
            if (run_container_is_full(const_CAST_run(c2))) {
                result = array_container_create();
                *result_type = ARRAY_CONTAINER_TYPE;
                return result;
            }
            *result_type =
                bitset_run_container_andnot(const_CAST_bitset(c1),
                                            const_CAST_run(c2), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(RUN, BITSET):
            *result_type =
                run_bitset_container_andnot(const_CAST_run(c1),
                                            const_CAST_bitset(c2), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, RUN):
            if (run_container_is_full(const_CAST_run(c2))) {
                result = array_container_create();
                *result_type = ARRAY_CONTAINER_TYPE;
                return result;
            }
            result = array_container_create();
            array_run_container_andnot(const_CAST_array(c1), const_CAST_run(c2),
                                       CAST_array(result));
            *result_type = ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(RUN, ARRAY):
            *result_type = (uint8_t)run_array_container_andnot(
                const_CAST_run(c1), const_CAST_array(c2), &result);
            return result;

        default:
            assert(false);
            roaring_unreachable;
            return NULL;  // unreached
    }
}

/**
 * Compute the andnot between two containers, with result in the first
 * container.
 * If the returned pointer is identical to c1, then the container has been
 * modified.
 * If the returned pointer is different from c1, then a new container has been
 * created. The original container is freed by container_iandnot.
 * The type of the first container may change. Returns the modified (and
 * possibly new) container.
 */
static inline container_t *container_iandnot(container_t *c1, uint8_t type1,
                                             const container_t *c2,
                                             uint8_t type2,
                                             uint8_t *result_type) {
    c1 = get_writable_copy_if_shared(c1, &type1);
    c2 = container_unwrap_shared(c2, &type2);
    container_t *result = NULL;
    switch (PAIR_CONTAINER_TYPES(type1, type2)) {
        case CONTAINER_PAIR(BITSET, BITSET):
            *result_type = bitset_bitset_container_iandnot(
                               CAST_bitset(c1), const_CAST_bitset(c2), &result)
                               ? BITSET_CONTAINER_TYPE
                               : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, ARRAY):
            array_array_container_iandnot(CAST_array(c1), const_CAST_array(c2));
            *result_type = ARRAY_CONTAINER_TYPE;
            return c1;

        case CONTAINER_PAIR(RUN, RUN):
            *result_type = (uint8_t)run_run_container_iandnot(
                CAST_run(c1), const_CAST_run(c2), &result);
            return result;

        case CONTAINER_PAIR(BITSET, ARRAY):
            *result_type = bitset_array_container_iandnot(
                               CAST_bitset(c1), const_CAST_array(c2), &result)
                               ? BITSET_CONTAINER_TYPE
                               : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, BITSET):
            *result_type = ARRAY_CONTAINER_TYPE;
            array_bitset_container_iandnot(CAST_array(c1),
                                           const_CAST_bitset(c2));
            return c1;

        case CONTAINER_PAIR(BITSET, RUN):
            *result_type = bitset_run_container_iandnot(
                               CAST_bitset(c1), const_CAST_run(c2), &result)
                               ? BITSET_CONTAINER_TYPE
                               : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(RUN, BITSET):
            *result_type = run_bitset_container_iandnot(
                               CAST_run(c1), const_CAST_bitset(c2), &result)
                               ? BITSET_CONTAINER_TYPE
                               : ARRAY_CONTAINER_TYPE;
            return result;

        case CONTAINER_PAIR(ARRAY, RUN):
            *result_type = ARRAY_CONTAINER_TYPE;
            array_run_container_iandnot(CAST_array(c1), const_CAST_run(c2));
            return c1;

        case CONTAINER_PAIR(RUN, ARRAY):
            *result_type = (uint8_t)run_array_container_iandnot(
                CAST_run(c1), const_CAST_array(c2), &result);
            return result;

        default:
            assert(false);
            roaring_unreachable;
            return NULL;
    }
}

/**
 * Visit all values x of the container once, passing (base+x,ptr)
 * to iterator. You need to specify a container and its type.
 * Returns true if the iteration should continue.
 */
static inline bool container_iterate(const container_t *c, uint8_t type,
                                     uint32_t base, roaring_iterator iterator,
                                     void *ptr) {
    c = container_unwrap_shared(c, &type);
    switch (type) {
        case BITSET_CONTAINER_TYPE:
            return bitset_container_iterate(const_CAST_bitset(c), base,
                                            iterator, ptr);
        case ARRAY_CONTAINER_TYPE:
            return array_container_iterate(const_CAST_array(c), base, iterator,
                                           ptr);
        case RUN_CONTAINER_TYPE:
            return run_container_iterate(const_CAST_run(c), base, iterator,
                                         ptr);
        default:
            assert(false);
            roaring_unreachable;
    }
    assert(false);
    roaring_unreachable;
    return false;
}

static inline bool container_iterate64(const container_t *c, uint8_t type,
                                       uint32_t base,
                                       roaring_iterator64 iterator,
                                       uint64_t high_bits, void *ptr) {
    c = container_unwrap_shared(c, &type);
    switch (type) {
        case BITSET_CONTAINER_TYPE:
            return bitset_container_iterate64(const_CAST_bitset(c), base,
                                              iterator, high_bits, ptr);
        case ARRAY_CONTAINER_TYPE:
            return array_container_iterate64(const_CAST_array(c), base,
                                             iterator, high_bits, ptr);
        case RUN_CONTAINER_TYPE:
            return run_container_iterate64(const_CAST_run(c), base, iterator,
                                           high_bits, ptr);
        default:
            assert(false);
            roaring_unreachable;
    }
    assert(false);
    roaring_unreachable;
    return false;
}

static inline container_t *container_not(const container_t *c, uint8_t type,
                                         uint8_t *result_type) {
    c = container_unwrap_shared(c, &type);
    container_t *result = NULL;
    switch (type) {
        case BITSET_CONTAINER_TYPE:
            *result_type =
                bitset_container_negation(const_CAST_bitset(c), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;
        case ARRAY_CONTAINER_TYPE:
            result = bitset_container_create();
            *result_type = BITSET_CONTAINER_TYPE;
            array_container_negation(const_CAST_array(c), CAST_bitset(result));
            return result;
        case RUN_CONTAINER_TYPE:
            *result_type =
                (uint8_t)run_container_negation(const_CAST_run(c), &result);
            return result;

        default:
            assert(false);
            roaring_unreachable;
    }
    assert(false);
    roaring_unreachable;
    return NULL;
}

static inline container_t *container_not_range(const container_t *c,
                                               uint8_t type,
                                               uint32_t range_start,
                                               uint32_t range_end,
                                               uint8_t *result_type) {
    c = container_unwrap_shared(c, &type);
    container_t *result = NULL;
    switch (type) {
        case BITSET_CONTAINER_TYPE:
            *result_type =
                bitset_container_negation_range(const_CAST_bitset(c),
                                                range_start, range_end, &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;
        case ARRAY_CONTAINER_TYPE:
            *result_type =
                array_container_negation_range(const_CAST_array(c), range_start,
                                               range_end, &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;
        case RUN_CONTAINER_TYPE:
            *result_type = (uint8_t)run_container_negation_range(
                const_CAST_run(c), range_start, range_end, &result);
            return result;

        default:
            assert(false);
            roaring_unreachable;
    }
    assert(false);
    roaring_unreachable;
    return NULL;
}

static inline container_t *container_inot(container_t *c, uint8_t type,
                                          uint8_t *result_type) {
    c = get_writable_copy_if_shared(c, &type);
    container_t *result = NULL;
    switch (type) {
        case BITSET_CONTAINER_TYPE:
            *result_type =
                bitset_container_negation_inplace(CAST_bitset(c), &result)
                    ? BITSET_CONTAINER_TYPE
                    : ARRAY_CONTAINER_TYPE;
            return result;
        case ARRAY_CONTAINER_TYPE:
            // will never be inplace
            result = bitset_container_create();
            *result_type = BITSET_CONTAINER_TYPE;
            array_container_negation(CAST_array(c), CAST_bitset(result));
            array_container_free(CAST_array(c));
            return result;
        case RUN_CONTAINER_TYPE:
            *result_type =
                (uint8_t)run_container_negation_inplace(CAST_run(c), &result);
            return result;

        default:
            assert(false);
            roaring_unreachable;
    }
    assert(false);
    roaring_unreachable;
    return NULL;
}

static inline container_t *container_inot_range(container_t *c, uint8_t type,
                                                uint32_t range_start,
                                                uint32_t range_end,
                                                uint8_t *result_type) {
    c = get_writable_copy_if_shared(c, &type);
    container_t *result = NULL;
    switch (type) {
        case BITSET_CONTAINER_TYPE:
            *result_type = bitset_container_negation_range_inplace(
                               CAST_bitset(c), range_start, range_end, &result)
                               ? BITSET_CONTAINER_TYPE
                               : ARRAY_CONTAINER_TYPE;
            return result;
        case ARRAY_CONTAINER_TYPE:
            *result_type = array_container_negation_range_inplace(
                               CAST_array(c), range_start, range_end, &result)
                               ? BITSET_CONTAINER_TYPE
                               : ARRAY_CONTAINER_TYPE;
            return result;
        case RUN_CONTAINER_TYPE:
            *result_type = (uint8_t)run_container_negation_range_inplace(
                CAST_run(c), range_start, range_end, &result);
            return result;

        default:
            assert(false);
            roaring_unreachable;
    }
    assert(false);
    roaring_unreachable;
    return NULL;
}

/**
 * If the element of given rank is in this container, supposing that
 * the first
 * element has rank start_rank, then the function returns true and
 * sets element
 * accordingly.
 * Otherwise, it returns false and update start_rank.
 */
static inline bool container_select(const container_t *c, uint8_t type,
                                    uint32_t *start_rank, uint32_t rank,
                                    uint32_t *element) {
    c = container_unwrap_shared(c, &type);
    switch (type) {
        case BITSET_CONTAINER_TYPE:
            return bitset_container_select(const_CAST_bitset(c), start_rank,
                                           rank, element);
        case ARRAY_CONTAINER_TYPE:
            return array_container_select(const_CAST_array(c), start_rank, rank,
                                          element);
        case RUN_CONTAINER_TYPE:
            return run_container_select(const_CAST_run(c), start_rank, rank,
                                        element);
        default:
            assert(false);
            roaring_unreachable;
    }
    assert(false);
    roaring_unreachable;
    return false;
}

static inline uint16_t container_maximum(const container_t *c, uint8_t type) {
    c = container_unwrap_shared(c, &type);
    switch (type) {
        case BITSET_CONTAINER_TYPE:
            return bitset_container_maximum(const_CAST_bitset(c));
        case ARRAY_CONTAINER_TYPE:
            return array_container_maximum(const_CAST_array(c));
        case RUN_CONTAINER_TYPE:
            return run_container_maximum(const_CAST_run(c));
        default:
            assert(false);
            roaring_unreachable;
    }
    assert(false);
    roaring_unreachable;
    return false;
}

static inline uint16_t container_minimum(const container_t *c, uint8_t type) {
    c = container_unwrap_shared(c, &type);
    switch (type) {
        case BITSET_CONTAINER_TYPE:
            return bitset_container_minimum(const_CAST_bitset(c));
        case ARRAY_CONTAINER_TYPE:
            return array_container_minimum(const_CAST_array(c));
        case RUN_CONTAINER_TYPE:
            return run_container_minimum(const_CAST_run(c));
        default:
            assert(false);
            roaring_unreachable;
    }
    assert(false);
    roaring_unreachable;
    return false;
}

// number of values smaller or equal to x
static inline int container_rank(const container_t *c, uint8_t type,
                                 uint16_t x) {
    c = container_unwrap_shared(c, &type);
    switch (type) {
        case BITSET_CONTAINER_TYPE:
            return bitset_container_rank(const_CAST_bitset(c), x);
        case ARRAY_CONTAINER_TYPE:
            return array_container_rank(const_CAST_array(c), x);
        case RUN_CONTAINER_TYPE:
            return run_container_rank(const_CAST_run(c), x);
        default:
            assert(false);
            roaring_unreachable;
    }
    assert(false);
    roaring_unreachable;
    return false;
}

// bulk version of container_rank(); return number of consumed elements
static inline uint32_t container_rank_many(const container_t *c, uint8_t type,
                                           uint64_t start_rank,
                                           const uint32_t *begin,
                                           const uint32_t *end, uint64_t *ans) {
    c = container_unwrap_shared(c, &type);
    switch (type) {
        case BITSET_CONTAINER_TYPE:
            return bitset_container_rank_many(const_CAST_bitset(c), start_rank,
                                              begin, end, ans);
        case ARRAY_CONTAINER_TYPE:
            return array_container_rank_many(const_CAST_array(c), start_rank,
                                             begin, end, ans);
        case RUN_CONTAINER_TYPE:
            return run_container_rank_many(const_CAST_run(c), start_rank, begin,
                                           end, ans);
        default:
            assert(false);
            roaring_unreachable;
    }
    assert(false);
    roaring_unreachable;
    return 0;
}

// return the index of x, if not exsist return -1
static inline int container_get_index(const container_t *c, uint8_t type,
                                      uint16_t x) {
    c = container_unwrap_shared(c, &type);
    switch (type) {
        case BITSET_CONTAINER_TYPE:
            return bitset_container_get_index(const_CAST_bitset(c), x);
        case ARRAY_CONTAINER_TYPE:
            return array_container_get_index(const_CAST_array(c), x);
        case RUN_CONTAINER_TYPE:
            return run_container_get_index(const_CAST_run(c), x);
        default:
            assert(false);
            roaring_unreachable;
    }
    assert(false);
    roaring_unreachable;
    return false;
}

/**
 * Add all values in range [min, max] to a given container.
 *
 * If the returned pointer is different from $container, then a new container
 * has been created and the caller is responsible for freeing it.
 * The type of the first container may change. Returns the modified
 * (and possibly new) container.
 */
static inline container_t *container_add_range(container_t *c, uint8_t type,
                                               uint32_t min, uint32_t max,
                                               uint8_t *result_type) {
    // NB: when selecting new container type, we perform only inexpensive checks
    switch (type) {
        case BITSET_CONTAINER_TYPE: {
            bitset_container_t *bitset = CAST_bitset(c);

            int32_t union_cardinality = 0;
            union_cardinality += bitset->cardinality;
            union_cardinality += max - min + 1;
            union_cardinality -=
                bitset_lenrange_cardinality(bitset->words, min, max - min);

            if (union_cardinality == INT32_C(0x10000)) {
                *result_type = RUN_CONTAINER_TYPE;
                return run_container_create_range(0, INT32_C(0x10000));
            } else {
                *result_type = BITSET_CONTAINER_TYPE;
                bitset_set_lenrange(bitset->words, min, max - min);
                bitset->cardinality = union_cardinality;
                return bitset;
            }
        }
        case ARRAY_CONTAINER_TYPE: {
            array_container_t *array = CAST_array(c);

            int32_t nvals_greater =
                count_greater(array->array, array->cardinality, (uint16_t)max);
            int32_t nvals_less =
                count_less(array->array, array->cardinality - nvals_greater,
                           (uint16_t)min);
            int32_t union_cardinality =
                nvals_less + (max - min + 1) + nvals_greater;

            if (union_cardinality == INT32_C(0x10000)) {
                *result_type = RUN_CONTAINER_TYPE;
                return run_container_create_range(0, INT32_C(0x10000));
            } else if (union_cardinality <= DEFAULT_MAX_SIZE) {
                *result_type = ARRAY_CONTAINER_TYPE;
                array_container_add_range_nvals(array, min, max, nvals_less,
                                                nvals_greater);
                return array;
            } else {
                *result_type = BITSET_CONTAINER_TYPE;
                bitset_container_t *bitset = bitset_container_from_array(array);
                bitset_set_lenrange(bitset->words, min, max - min);
                bitset->cardinality = union_cardinality;
                return bitset;
            }
        }
        case RUN_CONTAINER_TYPE: {
            run_container_t *run = CAST_run(c);

            int32_t nruns_greater =
                rle16_count_greater(run->runs, run->n_runs, (uint16_t)max);
            int32_t nruns_less = rle16_count_less(
                run->runs, run->n_runs - nruns_greater, (uint16_t)min);

            int32_t run_size_bytes =
                (nruns_less + 1 + nruns_greater) * sizeof(rle16_t);
            int32_t bitset_size_bytes =
                BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t);

            if (run_size_bytes <= bitset_size_bytes) {
                run_container_add_range_nruns(run, min, max, nruns_less,
                                              nruns_greater);
                *result_type = RUN_CONTAINER_TYPE;
                return run;
            } else {
                return container_from_run_range(run, min, max, result_type);
            }
        }
        default:
            roaring_unreachable;
    }
}

/*
 * Removes all elements in range [min, max].
 * Returns one of:
 *   - NULL if no elements left
 *   - pointer to the original container
 *   - pointer to a newly-allocated container (if it is more efficient)
 *
 * If the returned pointer is different from $container, then a new container
 * has been created and the caller is responsible for freeing the original
 * container.
 */
static inline container_t *container_remove_range(container_t *c, uint8_t type,
                                                  uint32_t min, uint32_t max,
                                                  uint8_t *result_type) {
    switch (type) {
        case BITSET_CONTAINER_TYPE: {
            bitset_container_t *bitset = CAST_bitset(c);

            int32_t result_cardinality =
                bitset->cardinality -
                bitset_lenrange_cardinality(bitset->words, min, max - min);

            if (result_cardinality == 0) {
                return NULL;
            } else if (result_cardinality <= DEFAULT_MAX_SIZE) {
                *result_type = ARRAY_CONTAINER_TYPE;
                bitset_reset_range(bitset->words, min, max + 1);
                bitset->cardinality = result_cardinality;
                return array_container_from_bitset(bitset);
            } else {
                *result_type = BITSET_CONTAINER_TYPE;
                bitset_reset_range(bitset->words, min, max + 1);
                bitset->cardinality = result_cardinality;
                return bitset;
            }
        }
        case ARRAY_CONTAINER_TYPE: {
            array_container_t *array = CAST_array(c);

            int32_t nvals_greater =
                count_greater(array->array, array->cardinality, (uint16_t)max);
            int32_t nvals_less =
                count_less(array->array, array->cardinality - nvals_greater,
                           (uint16_t)min);
            int32_t result_cardinality = nvals_less + nvals_greater;

            if (result_cardinality == 0) {
                return NULL;
            } else {
                *result_type = ARRAY_CONTAINER_TYPE;
                array_container_remove_range(
                    array, nvals_less, array->cardinality - result_cardinality);
                return array;
            }
        }
        case RUN_CONTAINER_TYPE: {
            run_container_t *run = CAST_run(c);

            if (run->n_runs == 0) {
                return NULL;
            }
            if (min <= run_container_minimum(run) &&
                max >= run_container_maximum(run)) {
                return NULL;
            }

            run_container_remove_range(run, min, max);
            return convert_run_to_efficient_container(run, result_type);
        }
        default:
            roaring_unreachable;
    }
}

#ifdef __cplusplus
using api::roaring_container_iterator_t;
#endif

/**
 * Initializes the iterator at the first entry in the container.
 */
roaring_container_iterator_t container_init_iterator(const container_t *c,
                                                     uint8_t typecode,
                                                     uint16_t *value);

/**
 * Initializes the iterator at the last entry in the container.
 */
roaring_container_iterator_t container_init_iterator_last(const container_t *c,
                                                          uint8_t typecode,
                                                          uint16_t *value);

/**
 * Moves the iterator to the next entry. Returns true and sets `value` if a
 * value is present.
 */
inline bool container_iterator_next(const container_t *c, uint8_t typecode,
                                    roaring_container_iterator_t *it,
                                    uint16_t *value) {
    switch (typecode) {
        case BITSET_CONTAINER_TYPE: {
            const bitset_container_t *bc = const_CAST_bitset(c);
            it->index++;

            uint32_t wordindex = it->index / 64;
            if (wordindex >= BITSET_CONTAINER_SIZE_IN_WORDS) {
                return false;
            }

            uint64_t word =
                bc->words[wordindex] & (UINT64_MAX << (it->index % 64));
            // next part could be optimized/simplified
            while (word == 0 &&
                   (wordindex + 1 < BITSET_CONTAINER_SIZE_IN_WORDS)) {
                wordindex++;
                word = bc->words[wordindex];
            }
            if (word != 0) {
                it->index = wordindex * 64 + roaring_trailing_zeroes(word);
                *value = it->index;
                return true;
            }
            return false;
        }
        case ARRAY_CONTAINER_TYPE: {
            const array_container_t *ac = const_CAST_array(c);
            it->index++;
            if (it->index < ac->cardinality) {
                *value = ac->array[it->index];
                return true;
            }
            return false;
        }
        case RUN_CONTAINER_TYPE: {
            if (*value == UINT16_MAX) {  // Avoid overflow to zero
                return false;
            }

            const run_container_t *rc = const_CAST_run(c);
            uint32_t limit =
                rc->runs[it->index].value + rc->runs[it->index].length;
            if (*value < limit) {
                (*value)++;
                return true;
            }

            it->index++;
            if (it->index < rc->n_runs) {
                *value = rc->runs[it->index].value;
                return true;
            }
            return false;
        }
        default:
            assert(false);
            roaring_unreachable;
            return false;
    }
}

/**
 * Moves the iterator to the previous entry. Returns true and sets `value` if a
 * value is present.
 */
inline bool container_iterator_prev(const container_t *c, uint8_t typecode,
                                    roaring_container_iterator_t *it,
                                    uint16_t *value) {
    switch (typecode) {
        case BITSET_CONTAINER_TYPE: {
            if (--it->index < 0) {
                return false;
            }

            const bitset_container_t *bc = const_CAST_bitset(c);
            int32_t wordindex = it->index / 64;
            uint64_t word =
                bc->words[wordindex] & (UINT64_MAX >> (63 - (it->index % 64)));

            while (word == 0 && --wordindex >= 0) {
                word = bc->words[wordindex];
            }
            if (word == 0) {
                return false;
            }

            it->index = (wordindex * 64) + (63 - roaring_leading_zeroes(word));
            *value = it->index;
            return true;
        }
        case ARRAY_CONTAINER_TYPE: {
            if (--it->index < 0) {
                return false;
            }
            const array_container_t *ac = const_CAST_array(c);
            *value = ac->array[it->index];
            return true;
        }
        case RUN_CONTAINER_TYPE: {
            if (*value == 0) {
                return false;
            }

            const run_container_t *rc = const_CAST_run(c);
            (*value)--;
            if (*value >= rc->runs[it->index].value) {
                return true;
            }

            if (--it->index < 0) {
                return false;
            }

            *value = rc->runs[it->index].value + rc->runs[it->index].length;
            return true;
        }
        default:
            assert(false);
            roaring_unreachable;
            return false;
    }
}

/**
 * Moves the iterator to the smallest entry that is greater than or equal to
 * `val`. Returns true and sets `value_out` if a value is present. `value_out`
 * should be initialized to a value.
 */
bool container_iterator_lower_bound(const container_t *c, uint8_t typecode,
                                    roaring_container_iterator_t *it,
                                    uint16_t *value_out, uint16_t val);

/**
 * Reads up to `count` entries from the container, and writes them into `buf`
 * as `high16 | entry`. Returns true and sets `value_out` if a value is present
 * after reading the entries. Sets `consumed` to the number of values read.
 * `count` should be greater than zero.
 */
bool container_iterator_read_into_uint32(const container_t *c, uint8_t typecode,
                                         roaring_container_iterator_t *it,
                                         uint32_t high16, uint32_t *buf,
                                         uint32_t count, uint32_t *consumed,
                                         uint16_t *value_out);

/**
 * Reads up to `count` entries from the container, and writes them into `buf`
 * as `high48 | entry`. Returns true and sets `value_out` if a value is present
 * after reading the entries. Sets `consumed` to the number of values read.
 * `count` should be greater than zero.
 */
bool container_iterator_read_into_uint64(const container_t *c, uint8_t typecode,
                                         roaring_container_iterator_t *it,
                                         uint64_t high48, uint64_t *buf,
                                         uint32_t count, uint32_t *consumed,
                                         uint16_t *value_out);

/**
 * Reads up to `count` entries backward from the container, writing them into
 * `buf` as `high16 | entry` in descending order. Returns true and sets
 * `value_out` if a value is present before the entries read. Sets `consumed`
 * to the number of values read. `count` should be greater than zero.
 *
 * `value_out` must be initialized to the current value yielded by the iterator.
 */
bool container_iterator_read_backward_into_uint32(
    const container_t *c, uint8_t typecode, roaring_container_iterator_t *it,
    uint32_t high16, uint32_t *buf, uint32_t count, uint32_t *consumed,
    uint16_t *value_out);

/**
 * Reads up to `count` entries backward from the container, writing them into
 * `buf` as `high48 | entry` in descending order. Returns true and sets
 * `value_out` if a value is present before the entries read. Sets `consumed`
 * to the number of values read. `count` should be greater than zero.
 *
 * `value_out` must be initialized to the current value yielded by the iterator.
 */
bool container_iterator_read_backward_into_uint64(
    const container_t *c, uint8_t typecode, roaring_container_iterator_t *it,
    uint64_t high48, uint64_t *buf, uint32_t count, uint32_t *consumed,
    uint16_t *value_out);

/**
 * Skips the next `skip_count` entries in the container iterator. Returns true
 * and sets `value_out` if a value is present after skipping. Returns false if
 * the end of the container is reached during the skip operation. Sets
 * consumed_count to the number of values actually skipped (which may be less
 * than skip_count if the end of the container is reached).
 *
 * value_out must be initialized to the previous value yielded by the iterator.
 *
 * skip_count must be greater than zero.
 */
bool container_iterator_skip(const container_t *c, uint8_t typecode,
                             roaring_container_iterator_t *it,
                             uint32_t skip_count, uint32_t *consumed_count,
                             uint16_t *value_out);

/**
 * Skips the previous `skip_count` entries in the container iterator (moves
 * backwards). Returns true and sets `value_out` if a value is present after
 * skipping backwards. Returns false if the beginning of the container is
 * reached during the skip operation. Sets consumed_count to the number of
 * values actually skipped backwards (which may be less than skip_count if
 * the beginning of the container is reached).
 *
 * value_out must be initialized to the current value yielded by the iterator.
 *
 * skip_count must be greater than zero.
 */
bool container_iterator_skip_backward(const container_t *c, uint8_t typecode,
                                      roaring_container_iterator_t *it,
                                      uint32_t skip_count,
                                      uint32_t *consumed_count,
                                      uint16_t *value_out);

/**
 * Finds the end of the consecutive run starting at the current iterator
 * position within a container. Returns the low16 of the last consecutive
 * value. If there are more values in the container after the run,
 * *has_more is set to true, the iterator is positioned at the next value,
 * and *value is updated to that value. Otherwise *has_more is set to false.
 *
 * *value must be the low 16 bits of the current value at the iterator's
 * position on entry.
 */
uint16_t container_iterator_find_run_end(const container_t *c, uint8_t typecode,
                                         roaring_container_iterator_t *it,
                                         uint16_t *value, bool *has_more);

/**
 * Finds the start of the consecutive run ending at the current iterator
 * position within a container. Returns the low16 of the first consecutive
 * value. If there are more values in the container before the run,
 * *has_more is set to true, the iterator is positioned at the previous value,
 * and *value is updated to that value. Otherwise *has_more is set to false.
 *
 * *value must be the low 16 bits of the current value at the iterator's
 * position on entry.
 */
uint16_t container_iterator_find_run_start(const container_t *c,
                                           uint8_t typecode,
                                           roaring_container_iterator_t *it,
                                           uint16_t *value, bool *has_more);

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

#endif

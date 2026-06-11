/*
    Shared type definitions used across the CRoaring public API and internal
    components.

    This file centralizes common typedefs and small structs that are referenced
    by multiple headers, including iterator callback signatures, roaring array
    metadata, statistics structures, and compatibility types used to bridge the
    C and C++ builds.
*/

#ifndef ROARING_TYPES_H
#define ROARING_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include <roaring/portability.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace api {
#endif

/**
 * When building .c files as C++, there's added compile-time checking if the
 * container types are derived from a `container_t` base class.  So long as
 * such a base class is empty, the struct will behave compatibly with C structs
 * despite the derivation.  This is due to the Empty Base Class Optimization:
 *
 * https://en.cppreference.com/w/cpp/language/ebo
 *
 * But since C isn't namespaced, taking `container_t` globally might collide
 * with other projects.  So roaring.h uses ROARING_CONTAINER_T, while internal
 * code #undefs that after declaring `typedef ROARING_CONTAINER_T container_t;`
 */
#if defined(__cplusplus)
extern "C++" {
struct container_s {};
}
#define ROARING_CONTAINER_T ::roaring::api::container_s
#else
#define ROARING_CONTAINER_T void  // no compile-time checking
#endif

#define ROARING_FLAG_COW UINT8_C(0x1)
#define ROARING_FLAG_FROZEN UINT8_C(0x2)

/**
 * Roaring arrays are array-based key-value pairs having containers as values
 * and 16-bit integer keys. A roaring bitmap  might be implemented as such.
 */

// parallel arrays.  Element sizes quite different.
// Alternative is array
// of structs.  Which would have better
// cache performance through binary searches?

typedef struct roaring_array_s {
    int32_t size;
    int32_t allocation_size;
    ROARING_CONTAINER_T **containers;  // Use container_t in non-API files!
    uint16_t *keys;
    uint8_t *typecodes;
    uint8_t flags;
} roaring_array_t;

typedef bool (*roaring_iterator)(uint32_t value, void *param);
typedef bool (*roaring_iterator64)(uint64_t value, void *param);

/**
 *  (For advanced users.)
 * The roaring_statistics_t can be used to collect detailed statistics about
 * the composition of a roaring bitmap.
 */
typedef struct roaring_statistics_s {
    uint32_t n_containers; /* number of containers */

    uint32_t n_array_containers;  /* number of array containers */
    uint32_t n_run_containers;    /* number of run containers */
    uint32_t n_bitset_containers; /* number of bitmap containers */

    uint32_t
        n_values_array_containers;    /* number of values in array containers */
    uint32_t n_values_run_containers; /* number of values in run containers */
    uint32_t
        n_values_bitset_containers; /* number of values in  bitmap containers */

    uint32_t n_bytes_array_containers;  /* number of allocated bytes in array
                                           containers */
    uint32_t n_bytes_run_containers;    /* number of allocated bytes in run
                                           containers */
    uint32_t n_bytes_bitset_containers; /* number of allocated bytes in  bitmap
                                           containers */

    uint32_t
        max_value; /* the maximal value, undefined if cardinality is zero */
    uint32_t
        min_value; /* the minimal value, undefined if cardinality is zero */

    CROARING_DEPRECATED
    uint64_t sum_value; /* deprecated always zero */

    uint64_t cardinality; /* total number of values stored in the bitmap */

    // and n_values_arrays, n_values_rle, n_values_bitmap
} roaring_statistics_t;

/**
 *  (For advanced users.)
 * The roaring64_statistics_t can be used to collect detailed statistics about
 * the composition of a roaring64 bitmap.
 */
typedef struct roaring64_statistics_s {
    uint64_t n_containers; /* number of containers */

    uint64_t n_array_containers;  /* number of array containers */
    uint64_t n_run_containers;    /* number of run containers */
    uint64_t n_bitset_containers; /* number of bitmap containers */

    uint64_t
        n_values_array_containers;    /* number of values in array containers */
    uint64_t n_values_run_containers; /* number of values in run containers */
    uint64_t
        n_values_bitset_containers; /* number of values in  bitmap containers */

    uint64_t n_bytes_array_containers;  /* number of allocated bytes in array
                                           containers */
    uint64_t n_bytes_run_containers;    /* number of allocated bytes in run
                                           containers */
    uint64_t n_bytes_bitset_containers; /* number of allocated bytes in  bitmap
                                           containers */

    uint64_t
        max_value; /* the maximal value, undefined if cardinality is zero */
    uint64_t
        min_value; /* the minimal value, undefined if cardinality is zero */

    uint64_t cardinality; /* total number of values stored in the bitmap */

    // and n_values_arrays, n_values_rle, n_values_bitmap
} roaring64_statistics_t;

/**
 * Roaring-internal type used to iterate within a roaring container.
 */
typedef struct roaring_container_iterator_s {
    // For bitset and array containers this is the index of the bit / entry.
    // For run containers this points at the run.
    int32_t index;
} roaring_container_iterator_t;

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace api {
#endif

#endif /* ROARING_TYPES_H */

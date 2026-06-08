/*
 * mixed_equal.h
 *
 */

#ifndef CONTAINERS_MIXED_EQUAL_H_
#define CONTAINERS_MIXED_EQUAL_H_

#include <roaring/containers/array.h>
#include <roaring/containers/bitset.h>
#include <roaring/containers/run.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

/**
 * Return true if the two containers have the same content.
 */
bool array_container_equal_bitset(const array_container_t* container1,
                                  const bitset_container_t* container2);

/**
 * Return true if the two containers have the same content.
 */
bool run_container_equals_array(const run_container_t* container1,
                                const array_container_t* container2);
/**
 * Return true if the two containers have the same content.
 */
bool run_container_equals_bitset(const run_container_t* container1,
                                 const bitset_container_t* container2);

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

#endif /* CONTAINERS_MIXED_EQUAL_H_ */

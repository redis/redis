/*
 * mixed_intersection.h
 *
 * This header declares intersection operations between different Roaring
 * container types, such as array, bitset, and run containers. These mixed
 * routines are used when the input containers have different internal
 * representations and the implementation must choose the appropriate result
 * form based on the data.
 *
 * In addition to materializing intersections, the file also provides helpers
 * for intersection cardinality, intersection predicates, and selected inplace
 * variants.
 */

#ifndef INCLUDE_CONTAINERS_MIXED_INTERSECTION_H_
#define INCLUDE_CONTAINERS_MIXED_INTERSECTION_H_

/* These functions appear to exclude cases where the
 * inputs have the same type and the output is guaranteed
 * to have the same type as the inputs.  Eg, array intersection
 */

#include <roaring/containers/array.h>
#include <roaring/containers/bitset.h>
#include <roaring/containers/run.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

/* Compute the intersection of src_1 and src_2 and write the result to
 * dst. It is allowed for dst to be equal to src_1. We assume that dst is a
 * valid container. */
void array_bitset_container_intersection(const array_container_t *src_1,
                                         const bitset_container_t *src_2,
                                         array_container_t *dst);

/* Compute the size of the intersection of src_1 and src_2. */
int array_bitset_container_intersection_cardinality(
    const array_container_t *src_1, const bitset_container_t *src_2);

/* Checking whether src_1 and src_2 intersect. */
bool array_bitset_container_intersect(const array_container_t *src_1,
                                      const bitset_container_t *src_2);

/*
 * Compute the intersection between src_1 and src_2 and write the result
 * to *dst. If the return function is true, the result is a bitset_container_t
 * otherwise is a array_container_t. We assume that dst is not pre-allocated. In
 * case of failure, *dst will be NULL.
 */
bool bitset_bitset_container_intersection(const bitset_container_t *src_1,
                                          const bitset_container_t *src_2,
                                          container_t **dst);

/* Compute the intersection between src_1 and src_2 and write the result to
 * dst. It is allowed for dst to be equal to src_1. We assume that dst is a
 * valid container. */
void array_run_container_intersection(const array_container_t *src_1,
                                      const run_container_t *src_2,
                                      array_container_t *dst);

/* Compute the intersection between src_1 and src_2 and write the result to
 * *dst. If the result is true then the result is a bitset_container_t
 * otherwise is a array_container_t.
 * If *dst == src_2, then an in-place intersection is attempted
 **/
bool run_bitset_container_intersection(const run_container_t *src_1,
                                       const bitset_container_t *src_2,
                                       container_t **dst);

/* Compute the size of the intersection between src_1 and src_2 . */
int array_run_container_intersection_cardinality(const array_container_t *src_1,
                                                 const run_container_t *src_2);

/* Compute the size of the intersection  between src_1 and src_2
 **/
int run_bitset_container_intersection_cardinality(
    const run_container_t *src_1, const bitset_container_t *src_2);

/* Check that src_1 and src_2 intersect. */
bool array_run_container_intersect(const array_container_t *src_1,
                                   const run_container_t *src_2);

/* Check that src_1 and src_2 intersect.
 **/
bool run_bitset_container_intersect(const run_container_t *src_1,
                                    const bitset_container_t *src_2);

/*
 * Same as bitset_bitset_container_intersection except that if the output is to
 * be a
 * bitset_container_t, then src_1 is modified and no allocation is made.
 * If the output is to be an array_container_t, then caller is responsible
 * to free the container.
 * In all cases, the result is in *dst.
 */
bool bitset_bitset_container_intersection_inplace(
    bitset_container_t *src_1, const bitset_container_t *src_2,
    container_t **dst);

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

#endif /* INCLUDE_CONTAINERS_MIXED_INTERSECTION_H_ */

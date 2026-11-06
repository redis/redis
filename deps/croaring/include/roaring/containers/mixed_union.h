/*
 * mixed_union.h
 *
 * This header declares union operations between different Roaring container
 * types, such as array, bitset, and run containers. These mixed-operation
 * helpers are used when the two input containers do not share the same
 * representation and the result may need to stay in, or be converted to, a
 * representation chosen according to the data.
 *
 * The file includes regular, lazy, and inplace variants so callers can select
 * between fully maintained results and faster deferred-maintenance paths.
 */

#ifndef INCLUDE_CONTAINERS_MIXED_UNION_H_
#define INCLUDE_CONTAINERS_MIXED_UNION_H_

/* These functions appear to exclude cases where the
 * inputs have the same type and the output is guaranteed
 * to have the same type as the inputs.  Eg, bitset unions
 */

#include <roaring/containers/array.h>
#include <roaring/containers/bitset.h>
#include <roaring/containers/run.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

/* Compute the union of src_1 and src_2 and write the result to
 * dst. It is allowed for src_2 to be dst.   */
void array_bitset_container_union(const array_container_t *src_1,
                                  const bitset_container_t *src_2,
                                  bitset_container_t *dst);

/* Compute the union of src_1 and src_2 and write the result to
 * dst. It is allowed for src_2 to be dst.  This version does not
 * update the cardinality of dst (it is set to BITSET_UNKNOWN_CARDINALITY). */
void array_bitset_container_lazy_union(const array_container_t *src_1,
                                       const bitset_container_t *src_2,
                                       bitset_container_t *dst);

/*
 * Compute the union between src_1 and src_2 and write the result
 * to *dst. If the return function is true, the result is a bitset_container_t
 * otherwise is a array_container_t. We assume that dst is not pre-allocated. In
 * case of failure, *dst will be NULL.
 */
bool array_array_container_union(const array_container_t *src_1,
                                 const array_container_t *src_2,
                                 container_t **dst);

/*
 * Compute the union between src_1 and src_2 and write the result
 * to *dst if it cannot be written to src_1. If the return function is true,
 * the result is a bitset_container_t
 * otherwise is a array_container_t. When the result is an array_container_t, it
 * it either written to src_1 (if *dst is null) or to *dst.
 * If the result is a bitset_container_t and *dst is null, then there was a
 * failure.
 */
bool array_array_container_inplace_union(array_container_t *src_1,
                                         const array_container_t *src_2,
                                         container_t **dst);

/*
 * Same as array_array_container_union except that it will more eagerly produce
 * a bitset.
 */
bool array_array_container_lazy_union(const array_container_t *src_1,
                                      const array_container_t *src_2,
                                      container_t **dst);

/*
 * Same as array_array_container_inplace_union except that it will more eagerly
 * produce a bitset.
 */
bool array_array_container_lazy_inplace_union(array_container_t *src_1,
                                              const array_container_t *src_2,
                                              container_t **dst);

/* Compute the union of src_1 and src_2 and write the result to
 * dst. We assume that dst is a
 * valid container. The result might need to be further converted to array or
 * bitset container,
 * the caller is responsible for the eventual conversion. */
void array_run_container_union(const array_container_t *src_1,
                               const run_container_t *src_2,
                               run_container_t *dst);

/* Compute the union of src_1 and src_2 and write the result to
 * src2. The result might need to be further converted to array or
 * bitset container,
 * the caller is responsible for the eventual conversion. */
void array_run_container_inplace_union(const array_container_t *src_1,
                                       run_container_t *src_2);

/* Compute the union of src_1 and src_2 and write the result to
 * dst. It is allowed for dst to be src_2.
 * If run_container_is_full(src_1) is true, you must not be calling this
 *function.
 **/
void run_bitset_container_union(const run_container_t *src_1,
                                const bitset_container_t *src_2,
                                bitset_container_t *dst);

/* Compute the union of src_1 and src_2 and write the result to
 * dst. It is allowed for dst to be src_2.  This version does not
 * update the cardinality of dst (it is set to BITSET_UNKNOWN_CARDINALITY).
 * If run_container_is_full(src_1) is true, you must not be calling this
 * function.
 * */
void run_bitset_container_lazy_union(const run_container_t *src_1,
                                     const bitset_container_t *src_2,
                                     bitset_container_t *dst);

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

#endif /* INCLUDE_CONTAINERS_MIXED_UNION_H_ */

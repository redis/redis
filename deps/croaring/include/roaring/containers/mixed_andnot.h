/*
 * mixed_andnot.h
 *
 * This header declares mixed-container difference operations of the form
 * `A \ B` (also called andnot) between Roaring container types such as array,
 * bitset, and run containers. These helpers are used when the operands have
 * different internal representations and the result may need to change
 * representation depending on density.
 *
 * The file includes both allocating and inplace-oriented variants so callers
 * can either materialize a fresh result or reuse storage when that is
 * efficient and semantically allowed.
 */
#ifndef INCLUDE_CONTAINERS_MIXED_ANDNOT_H_
#define INCLUDE_CONTAINERS_MIXED_ANDNOT_H_

#include <roaring/containers/array.h>
#include <roaring/containers/bitset.h>
#include <roaring/containers/run.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst, a valid array container that could be the same as dst.*/
void array_bitset_container_andnot(const array_container_t *src_1,
                                   const bitset_container_t *src_2,
                                   array_container_t *dst);

/* Compute the andnot of src_1 and src_2 and write the result to
 * src_1 */

void array_bitset_container_iandnot(array_container_t *src_1,
                                    const bitset_container_t *src_2);

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst, which does not initially have a valid container.
 * Return true for a bitset result; false for array
 */

bool bitset_array_container_andnot(const bitset_container_t *src_1,
                                   const array_container_t *src_2,
                                   container_t **dst);

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst (which has no container initially).  It will modify src_1
 * to be dst if the result is a bitset.  Otherwise, it will
 * free src_1 and dst will be a new array container.  In both
 * cases, the caller is responsible for deallocating dst.
 * Returns true iff dst is a bitset  */

bool bitset_array_container_iandnot(bitset_container_t *src_1,
                                    const array_container_t *src_2,
                                    container_t **dst);

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst. Result may be either a bitset or an array container
 * (returns "result is bitset"). dst does not initially have
 * any container, but becomes either a bitset container (return
 * result true) or an array container.
 */

bool run_bitset_container_andnot(const run_container_t *src_1,
                                 const bitset_container_t *src_2,
                                 container_t **dst);

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst. Result may be either a bitset or an array container
 * (returns "result is bitset"). dst does not initially have
 * any container, but becomes either a bitset container (return
 * result true) or an array container.
 */

bool run_bitset_container_iandnot(run_container_t *src_1,
                                  const bitset_container_t *src_2,
                                  container_t **dst);

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst. Result may be either a bitset or an array container
 * (returns "result is bitset").  dst does not initially have
 * any container, but becomes either a bitset container (return
 * result true) or an array container.
 */

bool bitset_run_container_andnot(const bitset_container_t *src_1,
                                 const run_container_t *src_2,
                                 container_t **dst);

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst (which has no container initially).  It will modify src_1
 * to be dst if the result is a bitset.  Otherwise, it will
 * free src_1 and dst will be a new array container.  In both
 * cases, the caller is responsible for deallocating dst.
 * Returns true iff dst is a bitset  */

bool bitset_run_container_iandnot(bitset_container_t *src_1,
                                  const run_container_t *src_2,
                                  container_t **dst);

/* dst does not indicate a valid container initially.  Eventually it
 * can become any type of container.
 */

int run_array_container_andnot(const run_container_t *src_1,
                               const array_container_t *src_2,
                               container_t **dst);

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst (which has no container initially).  It will modify src_1
 * to be dst if the result is a bitset.  Otherwise, it will
 * free src_1 and dst will be a new array container.  In both
 * cases, the caller is responsible for deallocating dst.
 * Returns true iff dst is a bitset  */

int run_array_container_iandnot(run_container_t *src_1,
                                const array_container_t *src_2,
                                container_t **dst);

/* dst must be a valid array container, allowed to be src_1 */

void array_run_container_andnot(const array_container_t *src_1,
                                const run_container_t *src_2,
                                array_container_t *dst);

/* dst does not indicate a valid container initially.  Eventually it
 * can become any kind of container.
 */

void array_run_container_iandnot(array_container_t *src_1,
                                 const run_container_t *src_2);

/* dst does not indicate a valid container initially.  Eventually it
 * can become any kind of container.
 */

int run_run_container_andnot(const run_container_t *src_1,
                             const run_container_t *src_2, container_t **dst);

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst (which has no container initially).  It will modify src_1
 * to be dst if the result is a bitset.  Otherwise, it will
 * free src_1 and dst will be a new array container.  In both
 * cases, the caller is responsible for deallocating dst.
 * Returns true iff dst is a bitset  */

int run_run_container_iandnot(run_container_t *src_1,
                              const run_container_t *src_2, container_t **dst);

/*
 * dst is a valid array container and may be the same as src_1
 */

void array_array_container_andnot(const array_container_t *src_1,
                                  const array_container_t *src_2,
                                  array_container_t *dst);

/* inplace array-array andnot will always be able to reuse the space of
 * src_1 */
void array_array_container_iandnot(array_container_t *src_1,
                                   const array_container_t *src_2);

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst (which has no container initially). Return value is
 * "dst is a bitset"
 */

bool bitset_bitset_container_andnot(const bitset_container_t *src_1,
                                    const bitset_container_t *src_2,
                                    container_t **dst);

/* Compute the andnot of src_1 and src_2 and write the result to
 * dst (which has no container initially).  It will modify src_1
 * to be dst if the result is a bitset.  Otherwise, it will
 * free src_1 and dst will be a new array container.  In both
 * cases, the caller is responsible for deallocating dst.
 * Returns true iff dst is a bitset  */

bool bitset_bitset_container_iandnot(bitset_container_t *src_1,
                                     const bitset_container_t *src_2,
                                     container_t **dst);

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

#endif

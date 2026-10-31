/*
 * mixed_xor.h
 *
 * This header declares XOR operations between different Roaring container
 * types, such as array, bitset, and run containers. These "mixed" routines
 * handle cases where the two inputs do not share the same representation and
 * where the most appropriate output representation may depend on the data.
 *
 * It includes regular, lazy, and inplace variants so higher-level bitmap code
 * can choose between fully normalized results and faster deferred-maintenance
 * paths.
 */

#ifndef INCLUDE_CONTAINERS_MIXED_XOR_H_
#define INCLUDE_CONTAINERS_MIXED_XOR_H_

/* These functions appear to exclude cases where the
 * inputs have the same type and the output is guaranteed
 * to have the same type as the inputs.  Eg, bitset unions
 */

/*
 * Java implementation (as of May 2016) for array_run, run_run
 * and  bitset_run don't do anything different for inplace.
 * (They are not truly in place.)
 */

#include <roaring/containers/array.h>
#include <roaring/containers/bitset.h>
#include <roaring/containers/run.h>

// #include "containers.h"

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

/* Compute the xor of src_1 and src_2 and write the result to
 * dst (which has no container initially).
 * Result is true iff dst is a bitset  */
bool array_bitset_container_xor(const array_container_t *src_1,
                                const bitset_container_t *src_2,
                                container_t **dst);

/* Compute the xor of src_1 and src_2 and write the result to
 * dst. It is allowed for src_2 to be dst.  This version does not
 * update the cardinality of dst (it is set to BITSET_UNKNOWN_CARDINALITY).
 */

void array_bitset_container_lazy_xor(const array_container_t *src_1,
                                     const bitset_container_t *src_2,
                                     bitset_container_t *dst);
/* Compute the xor of src_1 and src_2 and write the result to
 * dst (which has no container initially). Return value is
 * "dst is a bitset"
 */

bool bitset_bitset_container_xor(const bitset_container_t *src_1,
                                 const bitset_container_t *src_2,
                                 container_t **dst);

/* Compute the xor of src_1 and src_2 and write the result to
 * dst. Result may be either a bitset or an array container
 * (returns "result is bitset"). dst does not initially have
 * any container, but becomes either a bitset container (return
 * result true) or an array container.
 */

bool run_bitset_container_xor(const run_container_t *src_1,
                              const bitset_container_t *src_2,
                              container_t **dst);

/* lazy xor.  Dst is initialized and may be equal to src_2.
 *  Result is left as a bitset container, even if actual
 *  cardinality would dictate an array container.
 */

void run_bitset_container_lazy_xor(const run_container_t *src_1,
                                   const bitset_container_t *src_2,
                                   bitset_container_t *dst);

/* dst does not indicate a valid container initially.  Eventually it
 * can become any kind of container.
 */

int array_run_container_xor(const array_container_t *src_1,
                            const run_container_t *src_2, container_t **dst);

/* dst does not initially have a valid container.  Creates either
 * an array or a bitset container, indicated by return code
 */

bool array_array_container_xor(const array_container_t *src_1,
                               const array_container_t *src_2,
                               container_t **dst);

/* dst does not initially have a valid container.  Creates either
 * an array or a bitset container, indicated by return code.
 * A bitset container will not have a valid cardinality and the
 * container type might not be correct for the actual cardinality
 */

bool array_array_container_lazy_xor(const array_container_t *src_1,
                                    const array_container_t *src_2,
                                    container_t **dst);

/* Dst is a valid run container. (Can it be src_2? Let's say not.)
 * Leaves result as run container, even if other options are
 * smaller.
 */

void array_run_container_lazy_xor(const array_container_t *src_1,
                                  const run_container_t *src_2,
                                  run_container_t *dst);

/* dst does not indicate a valid container initially.  Eventually it
 * can become any kind of container.
 */

int run_run_container_xor(const run_container_t *src_1,
                          const run_container_t *src_2, container_t **dst);

/* INPLACE versions (initial implementation may not exploit all inplace
 * opportunities (if any...)
 */

/* Compute the xor of src_1 and src_2 and write the result to
 * dst (which has no container initially).  It will modify src_1
 * to be dst if the result is a bitset.  Otherwise, it will
 * free src_1 and dst will be a new array container.  In both
 * cases, the caller is responsible for deallocating dst.
 * Returns true iff dst is a bitset  */

bool bitset_array_container_ixor(bitset_container_t *src_1,
                                 const array_container_t *src_2,
                                 container_t **dst);

bool bitset_bitset_container_ixor(bitset_container_t *src_1,
                                  const bitset_container_t *src_2,
                                  container_t **dst);

bool array_bitset_container_ixor(array_container_t *src_1,
                                 const bitset_container_t *src_2,
                                 container_t **dst);

/* Compute the xor of src_1 and src_2 and write the result to
 * dst. Result may be either a bitset or an array container
 * (returns "result is bitset"). dst does not initially have
 * any container, but becomes either a bitset container (return
 * result true) or an array container.
 */

bool run_bitset_container_ixor(run_container_t *src_1,
                               const bitset_container_t *src_2,
                               container_t **dst);

bool bitset_run_container_ixor(bitset_container_t *src_1,
                               const run_container_t *src_2, container_t **dst);

/* dst does not indicate a valid container initially.  Eventually it
 * can become any kind of container.
 */

int array_run_container_ixor(array_container_t *src_1,
                             const run_container_t *src_2, container_t **dst);

int run_array_container_ixor(run_container_t *src_1,
                             const array_container_t *src_2, container_t **dst);

bool array_array_container_ixor(array_container_t *src_1,
                                const array_container_t *src_2,
                                container_t **dst);

int run_run_container_ixor(run_container_t *src_1, const run_container_t *src_2,
                           container_t **dst);

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

#endif

/*
 * convert.h
 *
 * This header declares conversion helpers between the different Roaring
 * container representations: array, bitset, and run containers. These
 * routines are used when an operation produces data better represented in a
 * different form, or when the library wants to switch to the most space-
 * efficient container type.
 *
 * In addition to direct conversions, the file also provides helpers that
 * choose between candidate result representations based on cardinality and
 * storage efficiency.
 */

#ifndef INCLUDE_CONTAINERS_CONVERT_H_
#define INCLUDE_CONTAINERS_CONVERT_H_

#include <roaring/containers/array.h>
#include <roaring/containers/bitset.h>
#include <roaring/containers/run.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

/* Convert an array into a bitset. The input container is not freed or modified.
 */
bitset_container_t *bitset_container_from_array(const array_container_t *arr);

/* Convert a run into a bitset. The input container is not freed or modified. */
bitset_container_t *bitset_container_from_run(const run_container_t *arr);

/* Convert a run into an array. The input container is not freed or modified. */
array_container_t *array_container_from_run(const run_container_t *arr);

/* Convert a bitset into an array. The input container is not freed or modified.
 */
array_container_t *array_container_from_bitset(const bitset_container_t *bits);

/* Convert an array into a run. The input container is not freed or modified.
 */
run_container_t *run_container_from_array(const array_container_t *c);

/* convert a run into either an array or a bitset
 * might free the container. This does not free the input run container. */
container_t *convert_to_bitset_or_array_container(run_container_t *rc,
                                                  int32_t card,
                                                  uint8_t *resulttype);

/* convert containers to and from runcontainers, as is most space efficient.
 * The container might be freed. */
container_t *convert_run_optimize(container_t *c, uint8_t typecode_original,
                                  uint8_t *typecode_after);

/* converts a run container to either an array or a bitset, IF it saves space.
 */
/* If a conversion occurs, the caller is responsible to free the original
 * container and
 * he becomes reponsible to free the new one. */
container_t *convert_run_to_efficient_container(run_container_t *c,
                                                uint8_t *typecode_after);

// like convert_run_to_efficient_container but frees the old result if needed
container_t *convert_run_to_efficient_container_and_free(
    run_container_t *c, uint8_t *typecode_after);

/**
 * Create new container which is a union of run container and
 * range [min, max]. Caller is responsible for freeing run container.
 */
container_t *container_from_run_range(const run_container_t *run, uint32_t min,
                                      uint32_t max, uint8_t *typecode_after);

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

#endif /* INCLUDE_CONTAINERS_CONVERT_H_ */

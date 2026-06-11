/*
 * perfparameters.h
 *
 * This header centralizes a small set of performance-tuning constants and
 * heuristic defaults used by CRoaring container code. These parameters control
 * decisions such as initial container sizing and when lazy or eager operations
 * may convert between container representations.
 *
 * In practice, these values encode trade-offs between memory use, allocation
 * overhead, and execution speed for common workloads.
 */
#ifndef PERFPARAMETERS_H_
#define PERFPARAMETERS_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

/**
During lazy computations, we can transform array containers into bitset
containers as
long as we can expect them to have  ARRAY_LAZY_LOWERBOUND values.
*/
enum { ARRAY_LAZY_LOWERBOUND = 1024 };

/* default initial size of a run container
   setting it to zero delays the malloc.*/
enum { RUN_DEFAULT_INIT_SIZE = 0 };

/* default initial size of an array container
   setting it to zero delays the malloc */
enum { ARRAY_DEFAULT_INIT_SIZE = 0 };

/* automatic bitset conversion during lazy or */
#ifndef LAZY_OR_BITSET_CONVERSION
#define LAZY_OR_BITSET_CONVERSION true
#endif

/* automatically attempt to convert a bitset to a full run during lazy
 * evaluation */
#ifndef LAZY_OR_BITSET_CONVERSION_TO_FULL
#define LAZY_OR_BITSET_CONVERSION_TO_FULL true
#endif

/* automatically attempt to convert a bitset to a full run */
#ifndef OR_BITSET_CONVERSION_TO_FULL
#define OR_BITSET_CONVERSION_TO_FULL true
#endif

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif

#endif

#ifndef __MONOTONIC_H
#define __MONOTONIC_H
/* The monotonic clock is an always increasing clock source.  It is unrelated to
 * the actual time of day and should only be used for relative timings.  The
 * monotonic clock is also not guaranteed to be chronologically precise; there
 * may be slight skew/shift from a precise clock.
 *
 * Depending on system architecture, the monotonic time may be able to be
 * retrieved much faster than a normal clock source by using an instruction
 * counter on the CPU.  On x86 architectures (for example), the RDTSC
 * instruction is a very fast clock source for this purpose.
 */

#include "fmacros.h"
#include <stdint.h>
#include <unistd.h>

/* A counter in micro-seconds.  The 'monotime' type is provided for variables
 * holding a monotonic time.  This will help distinguish & document that the
 * variable is associated with the monotonic clock and should not be confused
 * with other types of time.*/
typedef uint64_t monotime;

/* Retrieve counter of micro-seconds relative to an arbitrary point in time.  */
extern monotime (*getMonotonicUs)(void);


/* Call once at startup to initialize the monotonic clock.  Though this only
 * needs to be called once, it may be called additional times without impact.
 * Returns a printable string indicating the type of clock initialized.
 * (The returned string is static and doesn't need to be freed.)  */
const char * monotonicInit();


/* Functions to measure elapsed time.  Example:
 *     monotime myTimer;
 *     elapsedStart(&myTimer);
 *     while (elapsedMs(myTimer) < 10) {} // loops for 10ms
 */
static inline void elapsedStart(monotime *start_time) {
    *start_time = getMonotonicUs();
}

static inline uint64_t elapsedUs(monotime start_time) {
    return getMonotonicUs() - start_time;
}

static inline uint64_t elapsedMs(monotime start_time) {
    return elapsedUs(start_time) / 1000;
}

#endif

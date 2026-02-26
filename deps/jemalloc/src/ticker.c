#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

/*
 * To avoid using floating point math down core paths (still necessary because
 * versions of the glibc dynamic loader that did not preserve xmm registers are
 * still somewhat common, requiring us to be compilable with -mno-sse), and also
 * to avoid generally expensive library calls, we use a precomputed table of
 * values.  We want to sample U uniformly on [0, 1], and then compute
 * ceil(log(u)/log(1-1/nticks)).  We're mostly interested in the case where
 * nticks is reasonably big, so 1/log(1-1/nticks) is well-approximated by
 * -nticks.
 *
 * To compute log(u), we sample an integer in [1, 64] and divide, then just look
 * up results in a table.  As a space-compression mechanism, we store these as
 * uint8_t by dividing the range (255) by the highest-magnitude value the log
 * can take on, and using that as a multiplier.  We then have to divide by that
 * multiplier at the end of the computation.
 *
 * The values here are computed in src/ticker.py
 */

const uint8_t ticker_geom_table[1 << TICKER_GEOM_NBITS] = {
	254, 211, 187, 169, 156, 144, 135, 127,
	120, 113, 107, 102, 97, 93, 89, 85,
	81, 77, 74, 71, 68, 65, 62, 60,
	57, 55, 53, 50, 48, 46, 44, 42,
	40, 39, 37, 35, 33, 32, 30, 29,
	27, 26, 24, 23, 21, 20, 19, 18,
	16, 15, 14, 13, 12, 10, 9, 8,
	7, 6, 5, 4, 3, 2, 1, 0
};

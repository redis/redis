#ifndef __BITMAP_ROARING_H
#define __BITMAP_ROARING_H

#include <stdint.h>

/* Native bitmaps support 64-bit indexing. The logical byte length must stay
 * representable as a non-negative signed 64-bit bit count (range arithmetic
 * in BITCOUNT/BITPOS works on signed bit totals), which caps the length at
 * 2^60-1 bytes and therefore the highest addressable bit offset at 2^63-9.
 * Legacy string bitmaps stay bounded by proto-max-bulk-len. */
#define BITMAP_OBJECT_MAX_BYTES ((uint64_t)(INT64_MAX >> 3))
#define BITMAP_OBJECT_MAX_BITOFFSET (BITMAP_OBJECT_MAX_BYTES * 8 - 1)

void bitmapRoaringInit(void);

#endif /* __BITMAP_ROARING_H */

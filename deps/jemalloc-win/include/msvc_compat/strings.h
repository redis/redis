#ifndef JEMALLOC_STRINGS_H
#define JEMALLOC_STRINGS_H

#ifdef _MSC_VER

/* MSVC doesn't define ffs/ffsl. This dummy strings.h header is provided
 * for both */
#include <intrin.h>
#pragma intrinsic(_BitScanForward)
#ifdef _WIN64
#pragma intrinsic(_BitScanForward64)
#endif

#ifdef __cplusplus
extern "C" {
#endif

int ffsl(long x);
int ffs(int x);

#ifdef __cplusplus
}
#endif

static __forceinline int ffsl(long x)
{
	unsigned long i = 0;

	if (_BitScanForward(&i, (unsigned long)x))
		return (i + 1);
	return (0);
}

static __forceinline int ffs(int x)
{
	return (ffsl(x));
}

#endif  /* _MSC_VER */

#endif  /* JEMALLOC_STRINGS_H */

#ifndef strings_h
#define strings_h

/* MSVC doesn't define ffs/ffsl. This dummy strings.h header is provided
 * for both */
#ifdef _MSC_VER
#  include <intrin.h>
#  pragma intrinsic(_BitScanForward)
static __forceinline int ffsl(long x)
{
	unsigned long i;

	if (_BitScanForward(&i, x))
		return (i + 1);
	return (0);
}

static __forceinline int ffs(int x)
{

	return (ffsl(x));
}

#else
#  define ffsl(x) __builtin_ffsl(x)
#  define ffs(x) __builtin_ffs(x)
#endif

#endif /* strings_h */

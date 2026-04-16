
#ifndef __FAST_FLOAT_STRTOD_H__
#define __FAST_FLOAT_STRTOD_H__

#include <stddef.h>

double fast_float_strtod(const char *nptr, size_t len, char **endptr);

#ifdef REDIS_TEST
int fastFloatTest(int argc, char **argv, int flags);
#endif

#endif /* __FAST_FLOAT_STRTOD_H__ */

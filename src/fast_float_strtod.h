
#ifndef __FAST_FLOAT_STRTOD_H__
#define __FAST_FLOAT_STRTOD_H__

#include <stddef.h>

double fast_float_strtod_n(const char *nptr, size_t len, char **endptr);
double fast_float_strtod(const char *nptr, char **endptr);

#endif /* __FAST_FLOAT_STRTOD_H__ */

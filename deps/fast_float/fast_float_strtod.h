
#ifndef __FAST_FLOAT_STRTOD_H__
#define __FAST_FLOAT_STRTOD_H__

#include <stddef.h>

#if defined(__cplusplus)
extern "C"
{
#endif
    double fast_float_strtod(const char *str, size_t len, char **endptr);

#if defined(__cplusplus)
}
#endif

#endif /* __FAST_FLOAT_STRTOD_H__ */


#ifndef __FAST_FLOAT_STRTOD_H__
#define __FAST_FLOAT_STRTOD_H__

#include <stddef.h>

#if defined(__cplusplus)
extern "C"
{
#endif
    double fast_float_strtod(const char *in, char **out);
    double fast_float_strtod_n(const char *in, size_t len, char **out);

#if defined(__cplusplus)
}
#endif

#endif /* __FAST_FLOAT_STRTOD_H__ */

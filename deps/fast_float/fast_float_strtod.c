#define FFC_IMPL
#include "ffc.h"
#include <string.h>
#include <errno.h>

/* Convert NPTR to a double using the ffc library (C port of fast_float).
 *
 * This function behaves similarly to the standard strtod function, converting
 * the initial portion of the string pointed to by `nptr` to a `double` value.
 * If the conversion fails, errno is set to EINVAL.
 *
 * @param nptr   A pointer to the null-terminated byte string to be interpreted.
 * @param endptr A pointer to a pointer to character. If `endptr` is not NULL,
 *               it will point to the character after the last character used
 *               in the conversion.
 * @return       The converted value as a double. If no valid conversion could
 *               be performed, returns 0.0. */
double fast_float_strtod(const char *nptr, char **endptr) {
    double result = 0.0;
    const char *end = nptr + strlen(nptr);
    ffc_parse_options opts = ffc_parse_options_default();
    opts.format |= FFC_FORMAT_FLAG_ALLOW_LEADING_PLUS;
    ffc_result r = ffc_from_chars_double_options(nptr, end, &result, opts);
    if (r.outcome != FFC_OUTCOME_OK) {
        errno = EINVAL;
    }
    if (endptr != NULL) {
        *endptr = (char *)r.ptr;
    }
    return result;
}

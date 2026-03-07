#define FFC_IMPL
#include "ffc.h"
#include <errno.h>

/* Convert a string to a double using the ffc library (C port of fast_float).
 *
 * @param str    A pointer to the string to be parsed.
 * @param len    The length of the string (no strlen needed).
 * @param endptr If not NULL, set to point past the last parsed character.
 * @return       The parsed double, or 0.0 on failure (with errno = EINVAL). */
double fast_float_strtod(const char *str, size_t len, char **endptr) {
    double result = 0.0;
    ffc_parse_options opts = ffc_parse_options_default();
    opts.format |= FFC_FORMAT_FLAG_ALLOW_LEADING_PLUS;
    ffc_result r = ffc_from_chars_double_options(str, str + len, &result, opts);
    if (r.outcome != FFC_OUTCOME_OK) {
        errno = EINVAL;
    }
    if (endptr != NULL) {
        *endptr = (char *)r.ptr;
    }
    return result;
}

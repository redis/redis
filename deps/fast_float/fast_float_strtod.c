#define FFC_IMPL
#include "ffc.h"
#include <string.h>
#include <errno.h>

// scientific+fixed, allow leading plus, decimal point as '.'
static const ffc_parse_options REDIS_FFC_OPTIONS = {
  .format = FFC_PRESET_GENERAL | FFC_FORMAT_FLAG_ALLOW_LEADING_PLUS,
  .decimal_point = '.'
};

/* Convert NPTR to a double using the fast_float library.
 *
 * This function behaves similarly to the standard strtod function, converting
 * the initial portion of the string pointed to by `nptr` to a `double` value,
 * using the fast_float library for high performance. If the conversion fails,
 * errno is set to EINVAL error code.
 *
 * @param nptr   A pointer to the null-terminated byte string to be interpreted.
 * @param endptr A pointer to a pointer to character. If `endptr` is not NULL,
 *               it will point to the character after the last character used
 *               in the conversion.
 * @return       The converted value as a double. If no valid conversion could
 *               be performed, returns 0.0.
 * If ENDPTR is not NULL, a pointer to the character after the last one used
 * in the number is put in *ENDPTR.  */

double fast_float_strtod(const char *nptr, char **endptr) {
  double result = 0.0;
  // nocommit I do not like that we are calling strlen here, as the algorithm is designed to
  // parse a double from within a larger buffer, and indicate the stop point. Is nptr ever a larger buffer
  // It would be better to accept a `len` and allow the caller to call strlen or provide the buffer len if they have it

  ffc_result answer = ffc_from_chars_double_options(nptr, nptr + strlen(nptr), &result, REDIS_FFC_OPTIONS);
  if (answer.outcome != FFC_OUTCOME_OK) {
    errno = EINVAL;
  }
  if (endptr != NULL) {
    *endptr = (char *)answer.ptr;
  }
  return result;
}

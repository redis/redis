#include "fast_float.h"
#include <iostream>
#include <string>
#include <system_error>

/* Convert NPTR to a double using the fast_float library.
 * If ENDPTR is not NULL, a pointer to the character after the last one used
 * in the number is put in *ENDPTR.  */
extern "C" double fast_float_strtod(const char *nptr, char **endptr) {
  double result;
  auto answer = fast_float::from_chars(nptr, nptr + strlen(nptr), result);
  if (endptr != NULL) {
    *endptr = (char *)answer.ptr;
  }
  return result;
}

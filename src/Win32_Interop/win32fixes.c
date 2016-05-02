/*
* Modified by Henry Rawas (henryr@schakra.com)
*  - make it compatible with Visual Studio builds
*  - added wstrtod to handle INF, NAN
*  - added gettimeofday routine
*  - modified rename to retry after failure
*/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <locale.h>
#if _MSC_VER < 1800
#define isnan _isnan
#define isfinite _finite
#define isinf(x) (!_finite(x))
#else
#include <math.h>
#endif

static _locale_t clocale = NULL;
double wstrtod(const char *nptr, char **eptr) {
    double d;
    char *leptr;
    if (clocale == NULL) {
        clocale = _create_locale(LC_ALL, "C");
    }
    d = _strtod_l(nptr, &leptr, clocale);
    /* if 0, check if input was inf */
    if (d == 0 && nptr == leptr) {
        int neg = 0;
        while (isspace(*nptr)) {
            nptr++;
        }
        if (*nptr == '+') {
            nptr++;
        } else if (*nptr == '-') {
            nptr++;
            neg = 1;
        }

        if (_strnicmp("INF", nptr, 3) == 0) {
            if (eptr != NULL) {
                if ((_strnicmp("INFINITE", nptr, 8) == 0) || (_strnicmp("INFINITY", nptr, 8) == 0)) {
                    *eptr = (char*) (nptr + 8);
                } else {
                    *eptr = (char*) (nptr + 3);
                }
            }
            if (neg == 1) {
                return -HUGE_VAL;
            } else {
                return HUGE_VAL;
            }
        } else if (_strnicmp("NAN", nptr, 3) == 0) {
            if (eptr != NULL) {
                *eptr = (char*) (nptr + 3);
            }
            /* create a NaN : 0 * infinity*/
            d = HUGE_VAL;
            return d * 0;
        }
    }
    if (eptr != NULL) {
        *eptr = leptr;
    }
    return d;
}



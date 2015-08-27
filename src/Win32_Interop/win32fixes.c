/*
* Modified by Henry Rawas (henryr@schakra.com)
*  - make it compatible with Visual Studio builds
*  - added wstrtod to handle INF, NAN
*  - added gettimeofday routine
*  - modified rename to retry after failure
*/

#include <stdlib.h>
#include <errno.h>
#include "win32fixes.h"
#include <locale.h>

/* Redefined here to avoid redis.h so it can be used in other projects */
#define REDIS_NOTUSED(V) ((void) V)

/* Behaves as posix, works without ifdefs, makes compiler happy */
int sigaction(int sig, struct sigaction *in, struct sigaction *out) {
    REDIS_NOTUSED(out);

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction
     * is used. Otherwise, sa_handler is used */
    if (in->sa_flags & SA_SIGINFO) {
        signal(sig, in->sa_sigaction);
    } else {
        signal(sig, in->sa_handler);
    }
    return 0;
}

/* Terminates process, implemented only for SIGKILL */
int kill(pid_t pid, int sig) {
    if (sig == SIGKILL) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, 0, pid);
        if (!TerminateProcess(h, 127)) {
            errno = EINVAL; /* GetLastError() */
            CloseHandle(h);
            return -1;
        };
        CloseHandle(h);
        return 0;
    } else {
        errno = EINVAL;
        return -1;
    }
}

/* Redis CPU GetProcessTimes -> rusage  */
int getrusage(int who, struct rusage * r) {
    FILETIME starttime, exittime, kerneltime, usertime;
    ULARGE_INTEGER li;

    if (r == NULL) {
        errno = EFAULT;
        return -1;
    }

    memset(r, 0, sizeof(struct rusage));

    if (who == RUSAGE_SELF) {
        if (!GetProcessTimes(GetCurrentProcess(),
            &starttime,
            &exittime,
            &kerneltime,
            &usertime))
        {
            errno = EFAULT;
            return -1;
        }
    }

    if (who == RUSAGE_CHILDREN) {
        /* Childless on windows */
        starttime.dwLowDateTime = 0;
        starttime.dwHighDateTime = 0;
        exittime.dwLowDateTime = 0;
        exittime.dwHighDateTime = 0;
        kerneltime.dwLowDateTime = 0;
        kerneltime.dwHighDateTime = 0;
        usertime.dwLowDateTime = 0;
        usertime.dwHighDateTime = 0;
    }
    memcpy(&li, &kerneltime, sizeof(FILETIME));
    li.QuadPart /= 10L;
    r->ru_stime.tv_sec = (long) (li.QuadPart / 1000000L);
    r->ru_stime.tv_usec = (long) (li.QuadPart % 1000000L);

    memcpy(&li, &usertime, sizeof(FILETIME));
    li.QuadPart /= 10L;
    r->ru_utime.tv_sec = (long) (li.QuadPart / 1000000L);
    r->ru_utime.tv_usec = (long) (li.QuadPart % 1000000L);

    return 0;
}

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
                if (_strnicmp("INFINITE", nptr, 8) == 0) {
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

int strerror_r(int err, char* buf, size_t buflen) {
    int size = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
                            FORMAT_MESSAGE_IGNORE_INSERTS,
                            NULL,
                            err,
                            0,
                            buf,
                            (DWORD)buflen,
                            NULL);
    if (size == 0) {
        char* strerr = strerror(err);
        if (strlen(strerr) >= buflen) {
            errno = ERANGE;
            return -1;
        }
        strcpy(buf, strerr);
    }
    if (size > 2 && buf[size - 2] == '\r') {
        /* remove extra CRLF */
        buf[size - 2] = '\0';
    }
    return 0;
}

char wsa_strerror_buf[128];
char *wsa_strerror(int err) {
    int size = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
                            FORMAT_MESSAGE_IGNORE_INSERTS,
                            NULL,
                            err,
                            0,
                            wsa_strerror_buf,
                            128,
                            NULL);
    if (size == 0) {
        return strerror(err);
    }
    if (size > 2 && wsa_strerror_buf[size - 2] == '\r') {
        /* remove extra CRLF */
        wsa_strerror_buf[size - 2] = '\0';
    }
    return wsa_strerror_buf;
}


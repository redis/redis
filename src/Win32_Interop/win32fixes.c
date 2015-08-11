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
#include <signal.h>
#include <time.h>
#include <locale.h>
#include <math.h>
#include <string.h>
#include <assert.h>
#include "Win32_ThreadControl.h"

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

/* Replace MS C rtl rand which is 15bit with 32 bit */
int replace_random() {
    unsigned int x=0;
    if (RtlGenRandom == NULL) {
        // load proc if not loaded
        HMODULE lib = LoadLibraryA("advapi32.dll");
        RtlGenRandom = (RtlGenRandomFunc)GetProcAddress(lib, "SystemFunction036");
        if (RtlGenRandom == NULL) return 1;
    }
    RtlGenRandom(&x, sizeof(unsigned int));
    return (int)(x >> 1);
}

/* Rename which works on Windows when file exists */
int replace_rename(const char *src, const char *dst) {
    /* anti-virus may lock file - error code 5. Retry until it works or get a different error */
    int retries = 50;
    while (1) {
        if (MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
            return 0;
        } else {
            errno = GetLastError();
            if (errno != 5) break;
            retries--;
            if (retries == 0) {
                retries = 50;
                Sleep(10);
            }
        }
    }
    /* On error we will return generic error code without GetLastError() */
    return -1;
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

#define DELTA_EPOCH_IN_MICROSECS  11644473600000000Ui64

struct timezone {
    int  tz_minuteswest; /* minutes W of Greenwich */
    int  tz_dsttime;     /* type of dst correction */
};

/* fnGetSystemTimePreciseAsFileTime is NULL if and only if it hasn't been initialized. */
static VOID (WINAPI *fnGetSystemTimePreciseAsFileTime)(LPFILETIME) = NULL;

/* Interval (in seconds) of the high-resolution clock.
 * Special values:
 *   0 : it hasn't been initialized
 *  -1 : the system doesn't have high-resolution clock support 
 */
static double highResTimeInterval = 0;

void InitHighResRelativeTime() {
    LARGE_INTEGER perfFrequency;

    if (highResTimeInterval != 0)
        return;

    /* Retrieve high-resolution timer frequency
    * and precompute its reciprocal.
    */
    if (QueryPerformanceFrequency(&perfFrequency)) {
        highResTimeInterval = 1.0 / perfFrequency.QuadPart;
    } else {
        highResTimeInterval = -1;
    }

    assert(highResTimeInterval != 0);
}

void InitHighResAbsoluteTime() {
    FARPROC fp;
    HMODULE module;

    if (fnGetSystemTimePreciseAsFileTime != NULL)
        return;

    /* Use GetSystemTimeAsFileTime as fallbcak where GetSystemTimePreciseAsFileTime is not available */
    fnGetSystemTimePreciseAsFileTime = GetSystemTimeAsFileTime;
    module = GetModuleHandleA("kernel32.dll");
    if (module) {
        fp = GetProcAddress(module, "GetSystemTimePreciseAsFileTime");
        if (fp) {
            fnGetSystemTimePreciseAsFileTime = (VOID(WINAPI*)(LPFILETIME)) fp;
        }
    }

    assert(fnGetSystemTimePreciseAsFileTime != NULL);
}

void InitTimeFunctions() {
    InitHighResRelativeTime();
    InitHighResAbsoluteTime();
}

uint64_t GetHighResRelativeTime(double scale) {
  LARGE_INTEGER counter;

  if (highResTimeInterval <= 0) {
      if (highResTimeInterval == 0) {
          InitHighResRelativeTime();
      }

      /* If the performance interval is less than zero, there's no support. */
      if (highResTimeInterval < 0) {
          return 0;
      }
  }

  if (!QueryPerformanceCounter(&counter)) {
      return 0;
  }

  /* Because we have no guarantee about the order of magnitude of the
   * performance counter interval, integer math could cause this computation
   * to overflow. Therefore we resort to floating point math.
   */
  return (uint64_t) ((double)counter.QuadPart * highResTimeInterval * scale);
}

time_t gettimeofdaysecs(unsigned int *usec) {
    FILETIME ft;
    time_t tmpres = 0;

    GetSystemTimeAsFileTime(&ft);

    tmpres |= ft.dwHighDateTime;
    tmpres <<= 32;
    tmpres |= ft.dwLowDateTime;

    /*converting file time to unix epoch*/
    tmpres /= 10;  /*convert into microseconds*/
    tmpres -= DELTA_EPOCH_IN_MICROSECS;
    if (usec != NULL) {
        *usec = (unsigned int) (tmpres % 1000000UL);
    }
    return (tmpres / 1000000UL);
}

int gettimeofday_fast(struct timeval *tv, struct timezone *tz) {
    FILETIME ft;
    unsigned __int64 tmpres = 0;
    static int tzflag;

    if (NULL != tv) {
        GetSystemTimeAsFileTime(&ft);

        tmpres |= ft.dwHighDateTime;
        tmpres <<= 32;
        tmpres |= ft.dwLowDateTime;

        /*converting file time to unix epoch*/
        tmpres /= 10;  /*convert into microseconds*/
        tmpres -= DELTA_EPOCH_IN_MICROSECS; 
        tv->tv_sec = (long)(tmpres / 1000000UL);
        tv->tv_usec = (long)(tmpres % 1000000UL);
    }

    if (NULL != tz) {
        if (!tzflag)
        {
            _tzset();
            tzflag++;
        }
        tz->tz_minuteswest = _timezone / 60;
        tz->tz_dsttime = _daylight;
    }

    return 0;
}

int gettimeofday_highres(struct timeval *tv, struct timezone *tz) {
    FILETIME ft;
    unsigned __int64 tmpres = 0;
    static int tzflag;

    if (NULL == fnGetSystemTimePreciseAsFileTime) {
        InitHighResAbsoluteTime();
    }

    if (NULL != tv) {
        fnGetSystemTimePreciseAsFileTime(&ft);

        tmpres |= ft.dwHighDateTime;
        tmpres <<= 32;
        tmpres |= ft.dwLowDateTime;

        /*converting file time to unix epoch*/
        tmpres /= 10;  /*convert into microseconds*/
        tmpres -= DELTA_EPOCH_IN_MICROSECS;
        tv->tv_sec = (long) (tmpres / 1000000UL);
        tv->tv_usec = (long) (tmpres % 1000000UL);
    }

    if (NULL != tz) {
        if (!tzflag) {
            _tzset();
            tzflag++;
        }
        tz->tz_minuteswest = _timezone / 60;
        tz->tz_dsttime = _daylight;
    }

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

char *ctime_r(const time_t *clock, char *buf)  {
    // Note: ctime_r is documented (http://www.mkssoftware.com/docs/man3/ctime_r.3.asp) to be reentrant. 
    // _ctime64 is not thread safe. Since this is used only in sentinel.c, and Redis is single threaded, 
    // I am bypasing the critical section needed to guard against reentrancy.
    char* t = _ctime64(clock);
    if (t != NULL) {
        strcpy(buf, t);
    } else {
        buf[0] = 0;
    }
    return buf;
}

int truncate(const char *path, PORT_LONGLONG length) {
    LARGE_INTEGER newSize;
    HANDLE toTruncate;
    toTruncate = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (toTruncate != INVALID_HANDLE_VALUE) {
        newSize.QuadPart = length;
        if (FALSE == (SetFilePointerEx(toTruncate, newSize, NULL, FILE_BEGIN) && SetEndOfFile(toTruncate))) {
            errno = ENOENT;
            return -1;
        } else {
            return 0;
        }
    } else {
        errno = ENOENT;
        return -1;
    }
}

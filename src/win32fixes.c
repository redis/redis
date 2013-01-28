/*
* Modified by Henry Rawas (henryr@schakra.com)
*  - make it compatible with Visual Studio builds
*  - added wstrtod to handle INF, NAN
*  - added gettimeofday routine
*  - modified rename to retry after failure
*/

#ifdef _WIN32

#include <process.h>
#include <stdlib.h>
#include <errno.h>
#ifndef FD_SETSIZE
#define FD_SETSIZE 16000
#endif
#include <winsock2.h>
#include <windows.h>
#include <signal.h>
#include <time.h>
#include <locale.h>
#include <math.h>
#include <string.h>
#include "win32fixes.h"


/* Redefined here to avoid redis.h so it can be used in other projects */
#define REDIS_NOTUSED(V) ((void) V)
#define REDIS_THREAD_STACK_SIZE (1024*1024*4)

/* Winsock requires library initialization on startup  */
int w32initWinSock(void) {

    WSADATA t_wsa;
    WORD wVers;
    int iError;

    wVers = MAKEWORD(2, 2);
    iError = WSAStartup(wVers, &t_wsa);

    if(iError != NO_ERROR || LOBYTE(t_wsa.wVersion) != 2 || HIBYTE(t_wsa.wVersion) != 2 ) {
        return 0; /* not done; check WSAGetLastError() for error number */
    };

    return 1;
}

/* Behaves as posix, works without ifdefs, makes compiler happy */
int sigaction(int sig, struct sigaction *in, struct sigaction *out) {
    REDIS_NOTUSED(out);

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction
     * is used. Otherwise, sa_handler is used */
    if (in->sa_flags & SA_SIGINFO)
        signal(sig, in->sa_sigaction);
    else
        signal(sig, in->sa_handler);

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
    };
}

/* Forced write to disk */
int fsync (int fd) {
    HANDLE h = (HANDLE) _get_osfhandle(fd);
    DWORD err;

    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }

    if (!FlushFileBuffers(h)) {
        /* Windows error -> Unix */
        err = GetLastError();
        switch (err) {
            case ERROR_INVALID_HANDLE:
            errno = EINVAL;
            break;

            default:
            errno = EIO;
        }
        return -1;
    }

    return 0;
}

/* Missing wait3() implementation */
pid_t wait3(int *stat_loc, int options, void *rusage) {
    REDIS_NOTUSED(stat_loc);
    REDIS_NOTUSED(options);
    REDIS_NOTUSED(rusage);
    return (pid_t) waitpid((intptr_t) -1, 0, WAIT_FLAGS);
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
    RtlGenRandom(&x, sizeof(UINT_MAX));
    return (int)(x >> 1);
}

/* BSD sockets compatibile replacement */
int replace_setsockopt(int socket, int level, int optname, const void *optval, socklen_t optlen) {
    return (setsockopt)((SOCKET)socket, level, optname, (const char *)optval, optlen);
}

/* set size with 64bit support */
int replace_ftruncate(int fd, off64_t length) {
    HANDLE h = (HANDLE) _get_osfhandle (fd);
    LARGE_INTEGER l, o;

    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }

    l.QuadPart = length;

    if (!SetFilePointerEx(h, l, &o, FILE_BEGIN)) return -1;
    if (!SetEndOfFile(h)) return -1;

    return 0;
}

/* Rename which works on Windows when file exists */
int replace_rename(const char *src, const char *dst) {
    /* anti-virus may lock file - error code 5. Retry until it works or get a different error */
    int retries = 50;
    while (1) {
        if (MoveFileEx(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
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

/* Proxy structure to pass func and arg to thread */
typedef struct thread_params
{
    void *(*func)(void *);
    void * arg;
} thread_params;

/* Proxy function by windows thread requirements */
static unsigned __stdcall win32_proxy_threadproc(void *arg) {

    thread_params *p = (thread_params *) arg;
    p->func(p->arg);

    /* Dealocate params */
    free(p);

    _endthreadex(0);
    return 0;
}

int pthread_create(pthread_t *thread, const void *unused,
           void *(*start_routine)(void*), void *arg) {
    HANDLE h;
    thread_params *params = (thread_params *)malloc(sizeof(thread_params));
    REDIS_NOTUSED(unused);

    params->func = start_routine;
    params->arg  = arg;

    h =(HANDLE) _beginthreadex(NULL,  /* Security not used */
                               REDIS_THREAD_STACK_SIZE, /* Set custom stack size */
                               win32_proxy_threadproc,  /* calls win32 stdcall proxy */
                               params, /* real threadproc is passed as paremeter */
                               STACK_SIZE_PARAM_IS_A_RESERVATION,  /* reserve stack */
                               thread /* returned thread id */
                );

    if (!h)
        return errno;

    CloseHandle(h);
    return 0;
}

/* Noop in windows */
int pthread_detach (pthread_t thread) {
    REDIS_NOTUSED(thread);
    return 0; /* noop */
}

pthread_t pthread_self(void) {
    return GetCurrentThreadId();
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oset) {
    REDIS_NOTUSED(set);
    REDIS_NOTUSED(oset);
    switch (how) {
      case SIG_BLOCK:
      case SIG_UNBLOCK:
      case SIG_SETMASK:
           break;
      default:
            errno = EINVAL;
            return -1;
    }

  errno = ENOSYS;
  return 0;
}

int win32_pthread_join(pthread_t *thread, void **value_ptr)  {
    int result;
    HANDLE h = OpenThread(SYNCHRONIZE, FALSE, *thread);
    REDIS_NOTUSED(value_ptr);

    switch (WaitForSingleObject(h, INFINITE)) {
            case WAIT_OBJECT_0:
                    result = 0;
            case WAIT_ABANDONED:
                    result = EINVAL;
            default:
                    result = GetLastError();
    }

    CloseHandle(h);
    return result;
}

int pthread_cond_init(pthread_cond_t *cond, const void *unused) {
        REDIS_NOTUSED(unused);
        cond->waiters = 0;
        cond->was_broadcast = 0;

        InitializeCriticalSection(&cond->waiters_lock);

        cond->sema = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
        if (!cond->sema) {
            errno = GetLastError();
            return -1;
        }

        cond->continue_broadcast = CreateEvent(NULL,    /* security */
                                FALSE,                  /* auto-reset */
                                FALSE,                  /* not signaled */
                                NULL);                  /* name */
        if (!cond->continue_broadcast) {
            errno = GetLastError();
            return -1;
        }

        return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
        CloseHandle(cond->sema);
        CloseHandle(cond->continue_broadcast);
        DeleteCriticalSection(&cond->waiters_lock);
        return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
        int last_waiter;

        EnterCriticalSection(&cond->waiters_lock);
        cond->waiters++;
        LeaveCriticalSection(&cond->waiters_lock);

        /*
         * Unlock external mutex and wait for signal.
         * NOTE: we've held mutex locked long enough to increment
         * waiters count above, so there's no problem with
         * leaving mutex unlocked before we wait on semaphore.
         */
        LeaveCriticalSection(mutex);

        /* let's wait - ignore return value */
        WaitForSingleObject(cond->sema, INFINITE);

        /*
         * Decrease waiters count. If we are the last waiter, then we must
         * notify the broadcasting thread that it can continue.
         * But if we continued due to cond_signal, we do not have to do that
         * because the signaling thread knows that only one waiter continued.
         */
        EnterCriticalSection(&cond->waiters_lock);
        cond->waiters--;
        last_waiter = cond->was_broadcast && cond->waiters == 0;
        LeaveCriticalSection(&cond->waiters_lock);

        if (last_waiter) {
                /*
                 * cond_broadcast was issued while mutex was held. This means
                 * that all other waiters have continued, but are contending
                 * for the mutex at the end of this function because the
                 * broadcasting thread did not leave cond_broadcast, yet.
                 * (This is so that it can be sure that each waiter has
                 * consumed exactly one slice of the semaphor.)
                 * The last waiter must tell the broadcasting thread that it
                 * can go on.
                 */
                SetEvent(cond->continue_broadcast);
                /*
                 * Now we go on to contend with all other waiters for
                 * the mutex. Auf in den Kampf!
                 */
        }
        /* lock external mutex again */
        EnterCriticalSection(mutex);

        return 0;
}

/*
 * IMPORTANT: This implementation requires that pthread_cond_signal
 * is called while the mutex is held that is used in the corresponding
 * pthread_cond_wait calls!
 */
int pthread_cond_signal(pthread_cond_t *cond) {
        int have_waiters;

        EnterCriticalSection(&cond->waiters_lock);
        have_waiters = cond->waiters > 0;
        LeaveCriticalSection(&cond->waiters_lock);

        /*
         * Signal only when there are waiters
         */
        if (have_waiters)
                return ReleaseSemaphore(cond->sema, 1, NULL) ?
                        0 : GetLastError();
        else
                return 0;
}


/* Redis forks to perform background writing */
/* fork() on unix will split process in two */
/* marking memory pages as Copy-On-Write so */
/* child process will have data snapshot.   */
/* Windows has no support for fork().       */
int fork(void) {
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
        kerneltime.dwLowDateTime  = 0;
        kerneltime.dwHighDateTime  = 0;
        usertime.dwLowDateTime = 0;
        usertime.dwHighDateTime = 0;
   }
    memcpy(&li, &kerneltime, sizeof(FILETIME));
    li.QuadPart /= 10L;
    r->ru_stime.tv_sec = (long)(li.QuadPart / 1000000L);
    r->ru_stime.tv_usec = (long)(li.QuadPart % 1000000L);

    memcpy(&li, &usertime, sizeof(FILETIME));
    li.QuadPart /= 10L;
    r->ru_utime.tv_sec = (long)(li.QuadPart / 1000000L);
    r->ru_utime.tv_usec = (long)(li.QuadPart % 1000000L);

    return 0;
}

#define DELTA_EPOCH_IN_MICROSECS  11644473600000000Ui64

struct timezone
{
  int  tz_minuteswest; /* minutes W of Greenwich */
  int  tz_dsttime;     /* type of dst correction */
};

time_t gettimeofdaysecs(unsigned int *usec)
{
  FILETIME ft;
  time_t tmpres = 0;

    GetSystemTimeAsFileTime(&ft);

    tmpres |= ft.dwHighDateTime;
    tmpres <<= 32;
    tmpres |= ft.dwLowDateTime;

    /*converting file time to unix epoch*/
    tmpres /= 10;  /*convert into microseconds*/
    tmpres -= DELTA_EPOCH_IN_MICROSECS; 
    *usec = (unsigned int)(tmpres % 1000000UL);
    return (tmpres / 1000000UL);
}

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
  FILETIME ft;
  unsigned __int64 tmpres = 0;
  static int tzflag;

  if (NULL != tv)
  {
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

  if (NULL != tz)
  {
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

static _locale_t clocale = NULL;
double wstrtod(const char *nptr, char **eptr) {
    double d;
    char *leptr;
    if (clocale == NULL)
        clocale = _create_locale(LC_ALL, "C");
    d = _strtod_l(nptr, &leptr, clocale);
    /* if 0, check if input was inf */
    if (d == 0 && nptr == leptr) {
        int neg = 0;
        while (isspace(*nptr))
            nptr++;
        if (*nptr == '+')
            nptr++;
        else if (*nptr == '-') {
            nptr++;
            neg = 1;
        }

        if (strnicmp("INF", nptr, 3) == 0) {
            if (eptr != NULL) {
                if (strnicmp("INFINITE", nptr, 8) == 0)
                    *eptr = (char*)(nptr + 8);
                else
                    *eptr = (char*)(nptr + 3);
            }
            if (neg == 1)
                return -HUGE_VAL;
            else
                return HUGE_VAL;
        } else if (strnicmp("NAN", nptr, 3) == 0) {
            if (eptr != NULL)
                *eptr = (char*)(nptr + 3);
            /* create a NaN : 0 * infinity*/
            d = HUGE_VAL;
            return d * 0;
        }
    }
    if (eptr != NULL)
        *eptr = leptr;
    return d;
}

int strerror_r(int err, char* buf, size_t buflen) {
    int size = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
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
    int size = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                            FORMAT_MESSAGE_IGNORE_INSERTS,
                            NULL,
                            err,
                            0,
                            wsa_strerror_buf,
                            128,
                            NULL);
    if (size == 0) return strerror(err);
    if (size > 2 && wsa_strerror_buf[size - 2] == '\r') {
        /* remove extra CRLF */
        wsa_strerror_buf[size - 2] = '\0';
    }
    return wsa_strerror_buf;
}

#endif

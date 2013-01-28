/*
* Modified by Henry Rawas (henryr@schakra.com)
*  - make it compatible with Visual Studio builds
*  - added wstrtod to handle INF, NAN
*  - added support for using IOCP with sockets
*/

#ifndef WIN32FIXES_H
#define WIN32FIXES_H

#ifdef WIN32
#ifndef _WIN32
#define _WIN32
#endif
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define __USE_W32_SOCKETS

#include "fmacros.h"
#include <stdlib.h>
#include <stdio.h>
#include <io.h>
#include <signal.h>
#include <sys/types.h>
#ifndef FD_SETSIZE
#define FD_SETSIZE 16000
#endif
#include <winsock2.h>  /* setsocketopt */
#include <ws2tcpip.h>
#include <windows.h>
#include <float.h>
#include <fcntl.h>    /* _O_BINARY */
#include <limits.h>  /* INT_MAX */
#include <process.h>
#include <sys/types.h>

#define fseeko fseeko64
#define ftello ftello64

#define inline __inline

#undef ftruncate
#define ftruncate replace_ftruncate
#ifndef off64_t
#define off64_t off_t
#endif

int replace_ftruncate(int fd, off64_t length);


#define snprintf _snprintf
#define ftello64 _ftelli64
#define fseeko64 _fseeki64
#define strcasecmp _stricmp
#define strtoll _strtoi64
#define isnan _isnan
#define isfinite _finite
#define isinf(x) (!_finite(x))
#define lseek64 lseek
/* following defined to choose little endian byte order */
#define __i386__ 1
#if !defined(va_copy)
#define va_copy(d,s)  d = (s)
#endif

#define sleep(x) Sleep((x)*1000)

#ifndef __RTL_GENRANDOM
#define __RTL_GENRANDOM 1
typedef BOOLEAN (_stdcall* RtlGenRandomFunc)(void * RandomBuffer, ULONG RandomBufferLength);
#endif
RtlGenRandomFunc RtlGenRandom;

#define random() (long)replace_random()
#define rand() replace_random()
int replace_random();

#if !defined(ssize_t)
typedef int ssize_t;
#endif

#if !defined(mode_t)
#define mode_t long
#endif

#if !defined(u_int32_t)
/* sha1 */
typedef unsigned __int32 u_int32_t;
#endif

/* Redis calls usleep(1) to give thread some time
* Sleep(0) should do the same on windows
* In other cases, usleep is called with milisec resolution,
* which can be directly translated to winapi Sleep() */
#undef usleep
#define usleep(x) (x == 1) ? Sleep(0) : Sleep((int)((x)/1000))

#define pipe(fds) _pipe(fds, 8192, _O_BINARY|_O_NOINHERIT)

/* Processes */
#define waitpid(pid,statusp,options) _cwait (statusp, pid, WAIT_CHILD)

#define WAIT_T int
#define WTERMSIG(x) ((x) & 0xff) /* or: SIGABRT ?? */
#define WCOREDUMP(x) 0
#define WEXITSTATUS(x) (((x) >> 8) & 0xff) /* or: (x) ?? */
#define WIFSIGNALED(x) (WTERMSIG (x) != 0) /* or: ((x) == 3) ?? */
#define WIFEXITED(x) (WTERMSIG (x) == 0) /* or: ((x) != 3) ?? */
#define WIFSTOPPED(x) 0

#define WNOHANG 1

/* file mapping */
#define PROT_READ 1
#define PROT_WRITE 2

#define MAP_FAILED   (void *) -1

#define MAP_SHARED 1
#define MAP_PRIVATE 2

/* rusage */
#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

#ifndef _RUSAGE_T_
#define _RUSAGE_T_
struct rusage {
    struct timeval ru_utime;    /* user time used */
    struct timeval ru_stime;    /* system time used */
};
#endif

int getrusage(int who, struct rusage * rusage);

/* Signals */
#define SIGNULL  0 /* Null	Check access to pid*/
#define SIGHUP	 1 /* Hangup	Terminate; can be trapped*/
#define SIGINT	 2 /* Interrupt	Terminate; can be trapped */
#define SIGQUIT	 3 /* Quit	Terminate with core dump; can be trapped */
#define SIGTRAP  5
#define SIGBUS   7
#define SIGKILL	 9 /* Kill	Forced termination; cannot be trapped */
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM	15 /* Terminate	Terminate; can be trapped  */
#define SIGSTOP 17
#define SIGTSTP 18
#define SIGCONT 19
#define SIGCHLD 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGABRT 22
/* #define SIGSTOP	24 /*Pause the process; cannot be trapped*/
/* #define SIGTSTP	25 /*Terminal stop	Pause the process; can be trapped*/
/* #define SIGCONT	26 */
#define SIGWINCH 28
#define SIGUSR1  30
#define SIGUSR2  31

#define ucontext_t void*

#define SA_NOCLDSTOP    0x00000001u
#define SA_NOCLDWAIT    0x00000002u
#define SA_SIGINFO      0x00000004u
#define SA_ONSTACK      0x08000000u
#define SA_RESTART      0x10000000u
#define SA_NODEFER      0x40000000u
#define SA_RESETHAND    0x80000000u
#define SA_NOMASK       SA_NODEFER
#define SA_ONESHOT      SA_RESETHAND
#define SA_RESTORER     0x04000000

#define sigemptyset(pset)    (*(pset) = 0)
#define sigfillset(pset)     (*(pset) = (unsigned int)-1)
#define sigaddset(pset, num) (*(pset) |= (1L<<(num)))
#define sigdelset(pset, num) (*(pset) &= ~(1L<<(num)))
#define sigismember(pset, num) (*(pset) & (1L<<(num)))

#ifndef SIG_SETMASK
#define SIG_SETMASK (0)
#define SIG_BLOCK   (1)
#define SIG_UNBLOCK (2)
#endif /*SIG_SETMASK*/

typedef	void (*__p_sig_fn_t)(int);
typedef int pid_t;

#ifndef _SIGSET_T_
#define _SIGSET_T_
#ifdef _WIN64
typedef unsigned long long _sigset_t;
#else
typedef unsigned long _sigset_t;
#endif
# define sigset_t _sigset_t
#endif /* _SIGSET_T_ */

struct sigaction {
    int          sa_flags;
    sigset_t     sa_mask;
    __p_sig_fn_t sa_handler;
    __p_sig_fn_t sa_sigaction;
};

int sigaction(int sig, struct sigaction *in, struct sigaction *out);

/* Sockets */

#ifndef ECONNRESET
#define ECONNRESET WSAECONNRESET
#endif

#ifndef EINPROGRESS
#define EINPROGRESS WSAEINPROGRESS
#endif

#ifndef ETIMEDOUT
#define ETIMEDOUT WSAETIMEDOUT
#endif

#define setsockopt(a,b,c,d,e) replace_setsockopt(a,b,c,d,e)

int replace_setsockopt(int socket, int level, int optname,
                     const void *optval, socklen_t optlen);

#define rename(a,b) replace_rename(a,b)
int replace_rename(const char *src, const char *dest);

//threads avoiding pthread.h

#define pthread_mutex_t CRITICAL_SECTION
#define pthread_attr_t ssize_t

#define pthread_mutex_init(a,b) (InitializeCriticalSectionAndSpinCount((a), 0x80000400),0)
#define pthread_mutex_destroy(a) DeleteCriticalSection((a))
#define pthread_mutex_lock EnterCriticalSection
#define pthread_mutex_unlock LeaveCriticalSection

#define pthread_equal(t1, t2) ((t1) == (t2))

#define pthread_attr_init(x) (*(x) = 0)
#define pthread_attr_getstacksize(x, y) (*(y) = *(x))
#define pthread_attr_setstacksize(x, y) (*(x) = y)

#define pthread_t u_int

int pthread_create(pthread_t *thread, const void *unused,
                    void *(*start_routine)(void*), void *arg);

pthread_t pthread_self(void);

typedef struct {
    CRITICAL_SECTION waiters_lock;
    LONG waiters;
    int was_broadcast;
    HANDLE sema;
    HANDLE continue_broadcast;
} pthread_cond_t;

int pthread_cond_init(pthread_cond_t *cond, const void *unused);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);

int pthread_detach (pthread_t thread);
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);

/* Misc Unix -> Win32 */
int kill(pid_t pid, int sig);
int fsync (int fd);
pid_t wait3(int *stat_loc, int options, void *rusage);

int w32initWinSock(void);
/* int inet_aton(const char *cp_arg, struct in_addr *addr) */

/* redis-check-dump  */
void *mmap(void *start, size_t length, int prot, int flags, int fd, off offset);
int munmap(void *start, size_t length);

int fork(void);
int gettimeofday(struct timeval *tv, struct timezone *tz);
time_t gettimeofdaysecs(unsigned int *usec);

/* strtod does not handle Inf and Nan
We need to do the check before calling strtod */
#undef strtod
#define strtod(nptr, eptr) wstrtod((nptr), (eptr))

double wstrtod(const char *nptr, char **eptr);


/* structs and functions for using IOCP with windows sockets */

/* need callback on write complete. aeWinSendReq is used to pass parameters */
typedef struct aeWinSendReq {
    void *client;
    void *data;
    char *buf;
    int len;
} aeWinSendReq;


int aeWinSocketAttach(int fd);
int aeWinCloseSocket(int fd);
int aeWinReceiveDone(int fd);
int aeWinSocketSend(int fd, char *buf, int len, int flags,
                    void *eventLoop, void *client, void *data, void *proc);
int aeWinListen(SOCKET sock, int backlog);
int aeWinAccept(int fd, struct sockaddr *sa, socklen_t *len);
int aeWinSocketConnect(int fd, const struct sockaddr *sa, int len);

int strerror_r(int err, char* buf, size_t buflen);
char *wsa_strerror(int err);

// access check for executable uses X_OK. For Windows use READ access.
#ifndef X_OK
#define X_OK 4
#endif

#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif

#ifndef siginfo_t
typedef struct {
    int si_signo;
    int si_code;
    int si_value;
    int si_errno;
    pid_t si_pid;
    int si_uid;
    void *si_addr;
    int si_status;
    int si_band;
} siginfo_t;
#endif

#endif /* WIN32 */
#endif /* WIN32FIXES_H */

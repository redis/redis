/*
* Modified by Henry Rawas (henryr@schakra.com)
*  - make it compatible with Visual Studio builds
*  - added wstrtod to handle INF, NAN
*  - added support for using IOCP with sockets
*/

#ifndef WIN32FIXES_H
#define WIN32FIXES_H

#pragma warning(error: 4005)

#ifdef WIN32
#ifndef _WIN32
#define _WIN32
#endif
#endif

#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define __USE_W32_SOCKETS

#include "win32_types.h"

#include "..\fmacros.h"
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <windows.h>
#include <fcntl.h>      /* _O_BINARY */
#include <limits.h>     /* INT_MAX */
#include <process.h>
#include "Win32_APIs.h"

#include "Win32_FDAPI.h"    

#if _MSC_VER < 1800
#define isnan _isnan
#define isfinite _finite
#define isinf(x) (!_finite(x))
#else
#include <math.h>
#endif

#if !defined(mode_t)
#define mode_t long
#endif

#if !defined(u_int32_t)
/* sha1 */
typedef unsigned __int32 u_int32_t;
#endif

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
/* #define SIGCONT	26 /* */
#define SIGWINCH 28
#define SIGUSR1  30
#define SIGUSR2  31

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

#define sigemptyset(pset)       (*(pset) = 0)
#define sigfillset(pset)        (*(pset) = (unsigned int)-1)
#define sigaddset(pset, num)    (*(pset) |= (1L<<(num)))
#define sigdelset(pset, num)    (*(pset) &= ~(1L<<(num)))
#define sigismember(pset, num)  (*(pset) & (1L<<(num)))

typedef	void (*__p_sig_fn_t)(int);

#ifndef _SIGSET_T_
#define _SIGSET_T_
typedef size_t _sigset_t;
#define sigset_t _sigset_t
#endif /* _SIGSET_T_ */

struct sigaction {
    int          sa_flags;
    sigset_t     sa_mask;
    __p_sig_fn_t sa_handler;
    __p_sig_fn_t sa_sigaction;
};

int sigaction(int sig, struct sigaction *in, struct sigaction *out);

#if _MSC_VER < 1800
#ifndef ECONNRESET
#define ECONNRESET WSAECONNRESET
#endif

#ifndef EINPROGRESS
#define EINPROGRESS WSAEINPROGRESS
#endif

#ifndef ETIMEDOUT
#define ETIMEDOUT WSAETIMEDOUT
#endif
#endif


/* Misc Unix -> Win32 */
int kill(pid_t pid, int sig);
pid_t wait3(int *stat_loc, int options, void *rusage);

/* strtod does not handle Inf and Nan, we need to do the check before calling strtod */
#undef strtod
#define strtod(nptr, eptr) wstrtod((nptr), (eptr))

double wstrtod(const char *nptr, char **eptr);

int strerror_r(int err, char* buf, size_t buflen);
char *wsa_strerror(int err);

/* structs and functions for using IOCP with windows sockets */

/* need callback on write complete. aeWinSendReq is used to pass parameters */
typedef struct aeWinSendReq {
    void *client;
    void *data;
    char *buf;
    int len;
} aeWinSendReq;

int WSIOCP_ReceiveDone(int rfd);
int WSIOCP_SocketSend(int rfd, char *buf, int len, void *eventLoop, void *client, void *data, void *proc);
int WSIOCP_Listen(int rfd, int backlog);
int WSIOCP_Accept(int rfd, struct sockaddr *sa, socklen_t *len);
int WSIOCP_SocketConnect(int rfd, const SOCKADDR_STORAGE *ss);
int WSIOCP_SocketConnectBind(int rfd, const SOCKADDR_STORAGE *ss, const char* source_addr);

// access check for executable uses X_OK. For Windows use READ access.
#ifndef X_OK
#define X_OK 4
#endif

#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif



#endif /* WIN32FIXES_H */

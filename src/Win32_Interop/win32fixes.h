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
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <windows.h>
#include <fcntl.h>      /* _O_BINARY */
#include <limits.h>     /* INT_MAX */

#include "Win32_APIs.h"
#include "Win32_FDAPI.h"    

#define WNOHANG 1

/* file mapping */
#define PROT_READ 1
#define PROT_WRITE 2

#define MAP_FAILED   (void *) -1

#define MAP_SHARED 1
#define MAP_PRIVATE 2

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

/* strtod does not handle Inf and Nan, we need to do the check before calling strtod */
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

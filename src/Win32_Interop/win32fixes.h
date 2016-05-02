/*
* Modified by Henry Rawas (henryr@schakra.com)
*  - make it compatible with Visual Studio builds
*  - added wstrtod to handle INF, NAN
*  - added support for using IOCP with sockets
*/

#ifndef WIN32FIXES_H
#define WIN32FIXES_H

#pragma warning(error: 4005)
#pragma warning(error: 4013)

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
#include <sys/stat.h>
#include <fcntl.h>      // for _O_BINARY
#include <limits.h>     // for INT_MAX

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

#ifdef __cplusplus
extern "C"
{
#endif

/* strtod does not handle Inf and Nan, we need to do the check before calling strtod */
#undef strtod
#define strtod(nptr, eptr) wstrtod((nptr), (eptr))

double wstrtod(const char *nptr, char **eptr);

#ifdef __cplusplus
}
#endif

// access check for executable uses X_OK. For Windows use READ access.
#ifndef X_OK
#define X_OK 4
#endif

#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif

#endif /* WIN32FIXES_H */

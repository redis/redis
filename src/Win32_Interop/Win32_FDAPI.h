/*
 * Copyright (c), Microsoft Open Technologies, Inc.
 * All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#ifndef _WIN32_FDAPI_H
#define _WIN32_FDAPI_H
#endif

// fcntl flags used in Redis
#define F_GETFL     3
#define F_SETFL     4
#define O_NONBLOCK  0x0004

typedef unsigned long nfds_t;

#define INCL_WINSOCK_API_PROTOTYPES 0 // Important! Do not include Winsock API definitions to avoid conflicts with API entry points defnied below.
#include "win32_types.h"
#include <WinSock2.h>
#include <fcntl.h>
#include <stdio.h>

// the following are required to be defined before WS2tcpip is included.
typedef int (*redis_WSAGetLastError)(void);

#ifdef __cplusplus
extern "C"
{
#endif

extern redis_WSAGetLastError WSAGetLastError;

#ifdef __cplusplus
}
#endif

// including a version of this file modified to eliminate prototype definitions not removed by INCL_WINSOCK_API_PROTOTYPES
#include "WS2tcpip.h"

// reintroducing the inline APIs removed by INCL_WINSOCK_API_PROTOTYPES that Redis is using
#ifdef UNICODE
#define gai_strerror   gai_strerrorW
#else
#define gai_strerror   gai_strerrorA
#endif  /* UNICODE */

#define GAI_STRERROR_BUFFER_SIZE 1024

WS2TCPIP_INLINE
char *
gai_strerrorA(
    _In_ int ecode)
{
    DWORD dwMsgLen;
    static char buff[GAI_STRERROR_BUFFER_SIZE + 1];

    dwMsgLen = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM
                             |FORMAT_MESSAGE_IGNORE_INSERTS
                             |FORMAT_MESSAGE_MAX_WIDTH_MASK,
                              NULL,
                              ecode,
                              MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                              (LPSTR)buff,
                              GAI_STRERROR_BUFFER_SIZE,
                              NULL);

    return buff;
}

WS2TCPIP_INLINE
WCHAR *
gai_strerrorW(
    _In_ int ecode
    )
{
    DWORD dwMsgLen;
    static WCHAR buff[GAI_STRERROR_BUFFER_SIZE + 1];

    dwMsgLen = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM
                             |FORMAT_MESSAGE_IGNORE_INSERTS
                             |FORMAT_MESSAGE_MAX_WIDTH_MASK,
                              NULL,
                              ecode,
                              MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                              (LPWSTR)buff,
                              GAI_STRERROR_BUFFER_SIZE,
                              NULL);

    return buff;
}

#if WINVER <= _WIN32_WINNT_WS03
#define POLLRDNORM  0x0100
#define POLLRDBAND  0x0200
#define POLLIN      (POLLRDNORM | POLLRDBAND)
#define POLLPRI     0x0400

#define POLLWRNORM  0x0010
#define POLLOUT     (POLLWRNORM)
#define POLLWRBAND  0x0020

#define POLLERR     0x0001
#define POLLHUP     0x0002
#define POLLNVAL    0x0004

typedef struct pollfd {

    SOCKET  fd;
    SHORT   events;
    SHORT   revents;

} WSAPOLLFD, *PWSAPOLLFD, FAR *LPWSAPOLLFD;
#endif

// other API forwards
typedef int (*redis_setmode)(int fd,int mode);
typedef size_t (*redis_fwrite)(const void * _Str, size_t _Size, size_t _Count, FILE * _File);
typedef int (*redis_fclose)(FILE* file);
typedef int (*redis_fileno)(FILE* file);

// API prototypes must match the unix implementation
typedef int (*redis_pipe)(int pipefd[2]);
typedef int (*redis_socket)(int af,int type,int protocol);
typedef int (*redis_close)(int fd);
typedef int (*redis_open)(const char * _Filename, int _OpenFlag, int flags);
typedef int (*redis_accept)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
typedef int (*redis_setsockopt)(int sockfd, int level, int optname,const void *optval, socklen_t optlen);
typedef int (*redis_fcntl)(int fd, int cmd, int flags);
typedef int (*redis_poll)(struct pollfd *fds, nfds_t nfds, int timeout); 
typedef int (*redis_getsockopt)(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
typedef int (*redis_connect)(int sockfd, const struct sockaddr *addr, size_t addrlen);
typedef ssize_t (*redis_read)(int fd, void *buf, size_t count);
typedef ssize_t (*redis_write)(int fd, const void *buf, size_t count); 
typedef int (*redis_fsync)(int fd);
typedef int (*_redis_fstat)(int fd, struct __stat64 *buffer);
typedef int (*redis_listen)(int sockfd, int backlog);
typedef int (*redis_ftruncate)(int fd, PORT_LONGLONG length);
typedef int (*redis_bind)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
typedef u_short (*redis_htons)(u_short hostshort);
typedef u_long (*redis_htonl)(u_long hostlong);
typedef u_short (*redis_ntohs)(u_short netshort);
typedef int (*redis_getpeername)(int sockfd, struct sockaddr *addr, socklen_t * addrlen);
typedef int (*redis_getsockname)(int sockfd, struct sockaddr* addrsock, int* addrlen );
typedef void (*redis_freeaddrinfo)(struct addrinfo *ai);
typedef int (*redis_getaddrinfo)(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
typedef const char* (*redis_inet_ntop)(int af, const void *src, char *dst, size_t size);

typedef int (*redis_select)(int nfds, fd_set *readfds, fd_set *writefds,fd_set *exceptfds, struct timeval *timeout);
typedef u_int (*redis_ntohl)(u_int netlong);
typedef int (*redis_isatty)(int fd);
typedef int (*redis_access)(const char *pathname, int mode);
typedef u_int64 (*redis_lseek64)(int fd, u_int64 offset, int whence); 
typedef intptr_t (*redis_get_osfhandle)(int fd);
typedef int (*redis_open_osfhandle)(intptr_t osfhandle, int flags);

typedef BOOL fnWSIOCP_CloseSocketStateRFD(int rfd);

// access() mode definitions 
#define X_OK    0
#define W_OK    2
#define R_OK    4

#ifdef __cplusplus
extern "C"
{
#endif

// API replacements
extern redis_pipe pipe;
extern redis_socket socket;
extern redis_open open;
extern redis_accept accept;
extern redis_setsockopt setsockopt;
extern redis_fcntl fcntl;
extern redis_poll poll;
extern redis_getsockopt getsockopt;
extern redis_connect connect;
extern redis_read read;
extern redis_write write;
extern redis_fsync fsync;
extern redis_listen listen;
extern redis_ftruncate ftruncate;
extern redis_bind bind;
extern redis_htons htons;
extern redis_htonl htonl;
extern redis_getpeername getpeername;
extern redis_getsockname getsockname;
extern redis_ntohs ntohs;
extern redis_select select;
extern redis_ntohl ntohl;
extern redis_isatty isatty;
extern redis_access access;
extern redis_lseek64 lseek64;
extern redis_freeaddrinfo freeaddrinfo;
extern redis_getaddrinfo getaddrinfo;
extern redis_inet_ntop inet_ntop;

extern redis_close fdapi_close;
extern _redis_fstat fdapi_fstat64;
extern redis_setmode fdapi_setmode;
extern redis_fwrite fdapi_fwrite;
extern redis_fclose fdapi_fclose;
extern redis_fileno fdapi_fileno;
extern redis_get_osfhandle fdapi_get_osfhandle;
extern redis_open_osfhandle fdapi_open_osfhandle;

// Other FD based APIs
BOOL   FDAPI_SetFDInformation(int rfd, DWORD mask, DWORD flags);
HANDLE FDAPI_CreateIoCompletionPort(int rfd, HANDLE ExistingCompletionPort, ULONG_PTR CompletionKey, DWORD NumberOfConcurrentThreads);
BOOL   FDAPI_AcceptEx(int listenFD,int acceptFD,PVOID lpOutputBuffer,DWORD dwReceiveDataLength,DWORD dwLocalAddressLength,DWORD dwRemoteAddressLength,LPDWORD lpdwBytesReceived,LPOVERLAPPED lpOverlapped);
BOOL   FDAPI_ConnectEx(int rfd,const struct sockaddr *name,int namelen,PVOID lpSendBuffer,DWORD dwSendDataLength,LPDWORD lpdwBytesSent,LPOVERLAPPED lpOverlapped);
void   FDAPI_GetAcceptExSockaddrs(int rfd, PVOID lpOutputBuffer,DWORD dwReceiveDataLength,DWORD dwLocalAddressLength,DWORD dwRemoteAddressLength,LPSOCKADDR *LocalSockaddr,LPINT LocalSockaddrLength,LPSOCKADDR *RemoteSockaddr,LPINT RemoteSockaddrLength);
int    FDAPI_UpdateAcceptContext(int rfd);
void   FDAPI_ClearSocketInfo(int rfd);
void** FDAPI_GetSocketStatePtr(int rfd);

int    FDAPI_WSAIoctl(int rfd, DWORD dwIoControlCode, LPVOID lpvInBuffer, DWORD cbInBuffer, LPVOID lpvOutBuffer, DWORD cbOutBuffer, LPDWORD lpcbBytesReturned, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
int    FDAPI_WSASend(int rfd, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent, DWORD dwFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
int    FDAPI_WSARecv(int rfd, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
BOOL   FDAPI_WSAGetOverlappedResult(int rfd, LPWSAOVERLAPPED lpOverlapped, LPDWORD lpcbTransfer, BOOL fWait, LPDWORD lpdwFlags);
BOOL   FDAPI_CloseDuplicatedSocket(int rfd);
int    FDAPI_WSADuplicateSocket(int rfd, DWORD dwProcessId, LPWSAPROTOCOL_INFO lpProtocolInfo);
int    FDAPI_WSASocket(int af, int type, int protocol, LPWSAPROTOCOL_INFO lpProtocolInfo, GROUP g, DWORD dwFlags);
int    FDAPI_ioctlsocket(int rfd, long cmd, u_long *argp);

void   FDAPI_SetCloseSocketState(fnWSIOCP_CloseSocketStateRFD* func);

// Other networking functions
BOOL ParseStorageAddress(const char *ip, int port, SOCKADDR_STORAGE* pSotrageAddr);

// Macroize CRT definitions to point to our own
#ifndef FDAPI_NOCRTREDEFS
#define close(fd) fdapi_close(fd)
#define setmode(fd,mode) fdapi_setmode(fd,mode)
#define fwrite(Str, Size, Count, File) fdapi_fwrite(Str,Size,Count,File)
#define fclose(File) fdapi_fclose(File)
#define fileno(File) fdapi_fileno(File)
#define _get_osfhandle(fd) fdapi_get_osfhandle(fd)

#define _INC_STAT_INL
#define fstat(_Desc, _Stat) fdapi_fstat64(_Desc,_Stat)
#endif

#ifdef __cplusplus
}
#endif

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
#define	F_GETFL		3
#define	F_SETFL		4
#define	O_NONBLOCK	0x0004

typedef unsigned long nfds_t;

#if !defined(ssize_t)
typedef int ssize_t;
#endif

#define INCL_WINSOCK_API_PROTOTYPES 0 // Important! Do not include Winsock API definitions to avoid conflicts with API entry points defnied below.
#include <WinSock2.h>
#undef FD_ISSET
#include <fcntl.h>
#include <stdio.h>

// the following are required to be defined before WS2tcpip is included.
typedef void (*redis_WSASetLastError)(int iError);
typedef int (*redis_WSAGetLastError)(void);
typedef int (*redis_WSAIoctl)(int rfd,DWORD dwIoControlCode,LPVOID lpvInBuffer,DWORD cbInBuffer,LPVOID lpvOutBuffer,DWORD cbOutBuffer,LPDWORD lpcbBytesReturned,LPWSAOVERLAPPED lpOverlapped,LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);

#ifdef __cplusplus
extern "C"
{
#endif

extern redis_WSASetLastError WSASetLastError;
extern redis_WSAGetLastError WSAGetLastError;
extern redis_WSAIoctl WSAIoctl;

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

// WinSock APIs used in Win32_wsiocp.cpp
typedef int (*redis_WSASend)(int rfd, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent, DWORD dwFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
typedef int (*redis_WSARecv)(int rfd,LPWSABUF lpBuffers,DWORD dwBufferCount,LPDWORD lpNumberOfBytesRecvd,LPDWORD lpFlags,LPWSAOVERLAPPED lpOverlapped,LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
typedef int (*redis_WSACleanup)(void);
typedef int (*redis_ioctlsocket)(int rfd,long cmd,u_long *argp );
typedef unsigned long (*redis_inet_addr)(const char *cp);
typedef struct hostent* (*redis_gethostbyname)(const char *name);
typedef char* (*redis_inet_ntoa)(struct in_addr in);
typedef BOOL (*redis_WSAGetOverlappedResult)(int rfd,LPWSAOVERLAPPED lpOverlapped, LPDWORD lpcbTransfer, BOOL fWait, LPDWORD lpdwFlags);

// other API forwards
typedef int (*redis_setmode)(int fd,int mode);
typedef size_t (*redis_fwrite)(const void * _Str, size_t _Size, size_t _Count, FILE * _File);

// API prototypes must match the unix implementation
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
typedef int (*redis_ftruncate)(int fd, long long length);
typedef int (*redis_bind)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
typedef int (*redis_shutdown)(int sockfd, int how); 
typedef u_short (*redis_htons)(u_short hostshort);
typedef u_long (*redis_htonl)(u_long hostlong);
typedef int (*redis_getpeername)(int sockfd, struct sockaddr *addr, socklen_t * addrlen);
typedef int (*redis_getsockname)(int sockfd, struct sockaddr* addrsock, int* addrlen );
typedef u_short (*redis_ntohs)(u_short netshort);
typedef void (*redis_freeaddrinfo)(struct addrinfo *ai);
typedef int (*redis_getaddrinfo)(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
typedef const char* (*redis_inet_ntop)(int af, const void *src, char *dst, size_t size);

typedef int (*redis_select)(int nfds, fd_set *readfds, fd_set *writefds,fd_set *exceptfds, struct timeval *timeout);
typedef u_int (*redis_ntohl)(u_int netlong);
typedef int (*redis_isatty)(int fd);
typedef int (*redis_access)(const char *pathname, int mode);
typedef u_int64 (*redis_lseek64)(int fd, u_int64 offset, int whence); 
typedef intptr_t (*redis_get_osfhandle)(int fd);
typedef int(*redis_FD_ISSET)(int fd, fd_set *);

// access() mode definitions 
#define X_OK    0
#define W_OK    2
#define R_OK    4

#ifdef __cplusplus
extern "C"
{
#endif

// API replacements
extern redis_socket socket;
extern redis_WSASend WSASend;
extern redis_WSARecv WSARecv;
extern redis_WSACleanup WSACleanup;
extern redis_ioctlsocket ioctlsocket;
extern redis_inet_addr inet_addr;
extern redis_inet_ntoa inet_ntoa;
extern redis_WSAGetOverlappedResult WSAGetOverlappedResult;

extern redis_close fdapi_close;
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
extern _redis_fstat fdapi_fstat64;
extern redis_listen listen;
extern redis_ftruncate ftruncate;
extern redis_bind bind;
extern redis_shutdown shutdown;
extern redis_gethostbyname gethostbyname;
extern redis_htons htons;
extern redis_htonl htonl;
extern redis_getpeername getpeername;
extern redis_getsockname getsockname;
extern redis_ntohs ntohs;
extern redis_setmode fdapi_setmode;
extern redis_fwrite fdapi_fwrite;

extern redis_select select;
extern redis_ntohl ntohl;
extern redis_isatty isatty;
extern redis_access access;
extern redis_lseek64 lseek64;
extern redis_get_osfhandle fdapi_get_osfhandle;
extern redis_freeaddrinfo freeaddrinfo;
extern redis_getaddrinfo getaddrinfo;
extern redis_inet_ntop inet_ntop;
extern redis_FD_ISSET FD_ISSET;

// other FD based APIs
BOOL SetFDInformation(int FD, DWORD mask, DWORD flags);
HANDLE FDAPI_CreateIoCompletionPortOnFD(int FD, HANDLE ExistingCompletionPort, ULONG_PTR CompletionKey, DWORD NumberOfConcurrentThreads);
BOOL FDAPI_AcceptEx(int listenFD,int acceptFD,PVOID lpOutputBuffer,DWORD dwReceiveDataLength,DWORD dwLocalAddressLength,DWORD dwRemoteAddressLength,LPDWORD lpdwBytesReceived,LPOVERLAPPED lpOverlapped);
BOOL FDAPI_ConnectEx(int fd,const struct sockaddr *name,int namelen,PVOID lpSendBuffer,DWORD dwSendDataLength,LPDWORD lpdwBytesSent,LPOVERLAPPED lpOverlapped);
void FDAPI_GetAcceptExSockaddrs(int fd, PVOID lpOutputBuffer,DWORD dwReceiveDataLength,DWORD dwLocalAddressLength,DWORD dwRemoteAddressLength,LPSOCKADDR *LocalSockaddr,LPINT LocalSockaddrLength,LPSOCKADDR *RemoteSockaddr,LPINT RemoteSockaddrLength);
int FDAPI_UpdateAcceptContext( int fd );

// other networking functions
BOOL ParseStorageAddress(const char *ip, int port, SOCKADDR_STORAGE* pSotrageAddr);
int StorageSize(SOCKADDR_STORAGE *ss);

// macroize CRT definitions to point to our own
#ifndef FDAPI_NOCRTREDEFS
#define close(fd) fdapi_close(fd)
#define setmode(fd,mode) fdapi_setmode(fd,mode)
#define fwrite(Str, Size, Count, File) fdapi_fwrite(Str,Size,Count,File)
#define _get_osfhandle(fd) fdapi_get_osfhandle(fd)

#define _INC_STAT_INL
#define fstat(_Desc, _Stat) fdapi_fstat64(_Desc,_Stat)
#endif

#ifdef __cplusplus

bool IsWindowsVersionAtLeast(WORD wMajorVersion, WORD wMinorVersion, WORD wServicePackMajor);
}
#endif

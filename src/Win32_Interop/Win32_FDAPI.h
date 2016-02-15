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

#ifndef WIN32_INTEROP_FDAPI_H
#define WIN32_INTEROP_FDAPI_H

// fcntl flags used in Redis
#define	F_GETFL		3
#define	F_SETFL		4
#define	O_NONBLOCK	0x0004

typedef unsigned long nfds_t;

// Important! Do not include Winsock API definitions to avoid conflicts
// with API entry points defined below.
#define INCL_WINSOCK_API_PROTOTYPES 0
#include "win32_types.h"
#include <WinSock2.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>

// Including a version of this file modified to eliminate prototype
// definitions not removed by INCL_WINSOCK_API_PROTOTYPES
#include "WS2tcpip.h"

// Reintroducing the inline APIs removed by INCL_WINSOCK_API_PROTOTYPES
// that Redis is using
#ifdef UNICODE
#define gai_strerror   gai_strerrorW
#else
#define gai_strerror   gai_strerrorA
#endif /* UNICODE */

#define GAI_STRERROR_BUFFER_SIZE 1024

WS2TCPIP_INLINE char* gai_strerrorA(_In_ int ecode) {
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

WS2TCPIP_INLINE WCHAR* gai_strerrorW(_In_ int ecode) {
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

// API prototypes must match the unix implementation
typedef int (*fdapi_pipe)(int pipefd[2]);
typedef int (*fdapi_socket)(int af,int type,int protocol);
typedef int (*fdapi_open)(const char * _Filename, int _OpenFlag, int flags);
typedef int (*fdapi_accept)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
typedef int (*fdapi_setsockopt)(int sockfd, int level, int optname,const void *optval, socklen_t optlen);
typedef int (*fdapi_fcntl)(int fd, int cmd, int flags);
typedef int (*fdapi_poll)(struct pollfd *fds, nfds_t nfds, int timeout);
typedef int (*fdapi_getsockopt)(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
typedef int (*fdapi_connect)(int sockfd, const struct sockaddr *addr, size_t addrlen);
typedef ssize_t (*fdapi_read)(int fd, void *buf, size_t count);
typedef ssize_t (*fdapi_write)(int fd, const void *buf, size_t count);
typedef int (*fdapi_fsync)(int fd);
typedef int (*fdapi_listen)(int sockfd, int backlog);
typedef int (*fdapi_ftruncate)(int fd, PORT_LONGLONG length);
typedef int (*fdapi_bind)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
typedef u_short (*fdapi_htons)(u_short hostshort);
typedef u_long (*fdapi_htonl)(u_long hostlong);
typedef u_short (*fdapi_ntohs)(u_short netshort);
typedef int (*fdapi_getpeername)(int sockfd, struct sockaddr *addr, socklen_t * addrlen);
typedef int (*fdapi_getsockname)(int sockfd, struct sockaddr* addrsock, int* addrlen );
typedef void (*fdapi_freeaddrinfo)(struct addrinfo *ai);
typedef int (*fdapi_getaddrinfo)(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
typedef const char* (*fdapi_inet_ntop)(int af, const void *src, char *dst, size_t size);
typedef int (*fdapi_inet_pton)(int af, const char * src, void *dst);
typedef int (*fdapi_select)(int nfds, fd_set *readfds, fd_set *writefds,fd_set *exceptfds, struct timeval *timeout);
typedef u_int (*fdapi_ntohl)(u_int netlong);
typedef int (*fdapi_isatty)(int fd);
typedef int (*fdapi_access)(const char *pathname, int mode);
typedef u_int64 (*fdapi_lseek64)(int fd, u_int64 offset, int whence);
typedef int (*fdapi_fstat)(int fd, struct __stat64 *buffer);

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
extern fdapi_accept         accept;
extern fdapi_access         access;
extern fdapi_bind           bind;
extern fdapi_connect        connect;
extern fdapi_fcntl          fcntl;
extern fdapi_fstat          fdapi_fstat64;
extern fdapi_freeaddrinfo   freeaddrinfo;
extern fdapi_fsync          fsync;
extern fdapi_ftruncate      ftruncate;
extern fdapi_getaddrinfo    getaddrinfo;
extern fdapi_getsockopt     getsockopt;
extern fdapi_getpeername    getpeername;
extern fdapi_getsockname    getsockname;
extern fdapi_htonl          htonl;
extern fdapi_htons          htons;
extern fdapi_isatty         isatty;
extern fdapi_inet_ntop      inet_ntop;
extern fdapi_inet_pton      inet_pton;
extern fdapi_listen         listen;
extern fdapi_lseek64        lseek64;
extern fdapi_ntohl          ntohl;
extern fdapi_ntohs          ntohs;
extern fdapi_open           open;
extern fdapi_pipe           pipe;
extern fdapi_poll           poll;
extern fdapi_read           read;
extern fdapi_select         select;
extern fdapi_setsockopt     setsockopt;
extern fdapi_socket         socket;
extern fdapi_write          write;

// Other FD based APIs
void    FDAPI_SaveSocketAddrStorage(int rfd, SOCKADDR_STORAGE* socketAddrStorage);
BOOL    FDAPI_SocketAttachIOCP(int rfd, HANDLE iocph);
BOOL    FDAPI_AcceptEx(int listenFD,int acceptFD,PVOID lpOutputBuffer,DWORD dwReceiveDataLength,DWORD dwLocalAddressLength,DWORD dwRemoteAddressLength,LPDWORD lpdwBytesReceived,LPOVERLAPPED lpOverlapped);
BOOL    FDAPI_ConnectEx(int fd,const struct sockaddr *name,int namelen,PVOID lpSendBuffer,DWORD dwSendDataLength,LPDWORD lpdwBytesSent,LPOVERLAPPED lpOverlapped);
void    FDAPI_GetAcceptExSockaddrs(int fd, PVOID lpOutputBuffer,DWORD dwReceiveDataLength,DWORD dwLocalAddressLength,DWORD dwRemoteAddressLength,LPSOCKADDR *LocalSockaddr,LPINT LocalSockaddrLength,LPSOCKADDR *RemoteSockaddr,LPINT RemoteSockaddrLength);
int     FDAPI_UpdateAcceptContext( int fd );
int     FDAPI_PipeSetNonBlock(int rfd, int non_blocking);
void**  FDAPI_GetSocketStatePtr(int rfd);
void    FDAPI_ClearSocketInfo(int fd);

int     FDAPI_WSAIoctl(int rfd, DWORD dwIoControlCode, LPVOID lpvInBuffer, DWORD cbInBuffer, LPVOID lpvOutBuffer, DWORD cbOutBuffer, LPDWORD lpcbBytesReturned, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
int     FDAPI_WSASend(int rfd, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent, DWORD dwFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
int     FDAPI_WSARecv(int rfd, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
BOOL    FDAPI_WSAGetOverlappedResult(int rfd, LPWSAOVERLAPPED lpOverlapped, LPDWORD lpcbTransfer, BOOL fWait, LPDWORD lpdwFlags);
BOOL    FDAPI_CloseDuplicatedSocket(int rfd);
int     FDAPI_WSADuplicateSocket(int rfd, DWORD dwProcessId, LPWSAPROTOCOL_INFO lpProtocolInfo);
int     FDAPI_WSASocket(int af, int type, int protocol, LPWSAPROTOCOL_INFO lpProtocolInfo, GROUP g, DWORD dwFlags);
int     FDAPI_WSAGetLastError(void);

intptr_t FDAPI_get_osfhandle(int fd);
int      FDAPI_open_osfhandle(intptr_t osfhandle, int flags);

// FDAPI helper function
void FDAPI_SetCloseSocketState(fnWSIOCP_CloseSocketStateRFD* func);

// Other networking functions
BOOL ParseStorageAddress(const char *ip, int port, SOCKADDR_STORAGE* pSotrageAddr);

extern int FDAPI_close(int rfd);
extern int FDAPI_fclose(FILE *file);
extern int FDAPI_setmode(int fd, int mode);
extern size_t FDAPI_fwrite(const void *buffer, size_t size, size_t count, FILE *file);
extern int FDAPI_fileno(FILE *file);

// Macroize CRT definitions to point to our own
#ifndef FDAPI_NOCRTREDEFS
#define close(fd)                   FDAPI_close(fd)
#define fclose(File)                FDAPI_fclose(File)
#define setmode(fd,mode)            FDAPI_setmode(fd,mode)
#define fwrite(Str,Size,Count,File) FDAPI_fwrite(Str,Size,Count,File)
#define fileno(File)                FDAPI_fileno(File)

#define _INC_STAT_INL
#define fstat(fd,buffer)            fdapi_fstat64(fd,buffer)
#endif

#ifdef __cplusplus
}
#endif
#endif
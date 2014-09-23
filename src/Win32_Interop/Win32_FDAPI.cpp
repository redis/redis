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
#define FDAPI_NOCRTREDEFS
#include "Win32_FDAPI.h"
#include "win32_rfdmap.h"
#include <exception>
#include <mswsock.h>
#include <sys/stat.h>
#include "Win32_fdapi_crt.h"
#include "Win32_variadicFunctor.h"
#include "Win32_ANSI.h"
#include <string>
#include "..\redisLog.h"
using namespace std;

#define CATCH_AND_REPORT()  catch(const std::exception &){::redisLog(REDIS_WARNING, "FDAPI: std exception");}catch(...){::redisLog(REDIS_WARNING, "FDAPI: other exception");}

extern "C" {
// FD lookup Winsock equivalents for Win32_wsiocp.c
redis_WSASetLastError WSASetLastError = NULL;
redis_WSAGetLastError WSAGetLastError = NULL;
redis_WSAIoctl WSAIoctl = NULL;
redis_WSASend WSASend = NULL;
redis_WSARecv WSARecv = NULL;
redis_WSACleanup WSACleanup = NULL;
redis_WSAGetOverlappedResult WSAGetOverlappedResult = NULL;

// other API forwards
redis_fwrite fdapi_fwrite = NULL;
redis_setmode fdapi_setmode = NULL;
redis_select select = NULL;
redis_ntohl ntohl = NULL;
redis_isatty isatty = NULL;
redis_access access = NULL;
redis_lseek64 lseek64 = NULL;
redis_get_osfhandle fdapi_get_osfhandle = NULL;

// Unix compatible FD based routines
redis_socket socket = NULL;
redis_close fdapi_close = NULL;
redis_open open = NULL;
redis_ioctlsocket ioctlsocket = NULL;
redis_inet_addr inet_addr = NULL;
redis_inet_ntoa inet_ntoa = NULL;
redis_accept accept = NULL;
redis_setsockopt setsockopt = NULL;
redis_fcntl fcntl = NULL;
redis_poll poll = NULL;
redis_getsockopt getsockopt = NULL;
redis_connect connect = NULL;
redis_read read = NULL;
redis_write write = NULL;
redis_fsync fsync = NULL;
_redis_fstat fdapi_fstat64 = NULL;
redis_listen listen = NULL;
redis_ftruncate ftruncate = NULL;
redis_bind bind = NULL;
redis_shutdown shutdown = NULL;
redis_gethostbyname gethostbyname = NULL;
redis_htons htons = NULL;
redis_htonl htonl = NULL;
redis_getpeername getpeername = NULL;
redis_getsockname getsockname = NULL;
redis_ntohs ntohs = NULL;
redis_freeaddrinfo freeaddrinfo = NULL;
redis_getaddrinfo getaddrinfo = NULL;
redis_inet_ntop inet_ntop = NULL;
redis_FD_ISSET FD_ISSET = NULL;
}

auto f_WSAStartup = dllfunctor_stdcall<int, WORD, LPWSADATA>("ws2_32.dll", "WSAStartup");
int InitWinsock() {
   WSADATA t_wsa;
    WORD wVers;
    int iError;

    wVers = MAKEWORD(2, 2);
	iError = f_WSAStartup(wVers, &t_wsa);

    if(iError != NO_ERROR || LOBYTE(t_wsa.wVersion) != 2 || HIBYTE(t_wsa.wVersion) != 2 ) {
        exit(1);
    } else {
      return 0;
    }
}

auto f_WSACleanup = dllfunctor_stdcall<int>("ws2_32.dll", "WSACleanup");
int  CleanupWinsock() {
	return f_WSACleanup();
}

BOOL SetFDInformation(int FD, DWORD mask, DWORD flags){
    try
    {
        SOCKET s = RFDMap::getInstance().lookupSocket(FD);
        if( s != INVALID_SOCKET ) {
            return SetHandleInformation((HANDLE)s, mask, flags);
        }
    } CATCH_AND_REPORT()

    return FALSE;
}

HANDLE FDAPI_CreateIoCompletionPortOnFD(int FD, HANDLE ExistingCompletionPort, ULONG_PTR CompletionKey, DWORD NumberOfConcurrentThreads) {
    try
    {
        SOCKET s = RFDMap::getInstance().lookupSocket(FD);
        if( s != INVALID_SOCKET ) {
            return CreateIoCompletionPort((HANDLE)s, ExistingCompletionPort, CompletionKey, NumberOfConcurrentThreads);
        }
    } CATCH_AND_REPORT()

    return INVALID_HANDLE_VALUE;
}

auto f_WSAIoctl = dllfunctor_stdcall<int, SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPVOID, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE>("ws2_32.dll", "WSAIoctl");
BOOL FDAPI_AcceptEx(int listenFD, int acceptFD, PVOID lpOutputBuffer, DWORD dwReceiveDataLength, DWORD dwLocalAddressLength, DWORD dwRemoteAddressLength, LPDWORD lpdwBytesReceived, LPOVERLAPPED lpOverlapped)
{
    try
    {
        SOCKET sListen = RFDMap::getInstance().lookupSocket(listenFD);
        SOCKET sAccept = RFDMap::getInstance().lookupSocket(acceptFD);
		if (sListen != INVALID_SOCKET &&  sAccept != INVALID_SOCKET) {
			LPFN_ACCEPTEX acceptex;
			const GUID wsaid_acceptex = WSAID_ACCEPTEX;
			DWORD bytes;

			if (SOCKET_ERROR ==
				f_WSAIoctl(sListen,
				SIO_GET_EXTENSION_FUNCTION_POINTER,
				(void *)&wsaid_acceptex,
				sizeof(GUID),
				&acceptex,
				sizeof(LPFN_ACCEPTEX),
				&bytes,
				NULL,
				NULL)) {
				return FALSE;
			}

			return acceptex(sListen, sAccept, lpOutputBuffer, dwReceiveDataLength, dwLocalAddressLength, dwRemoteAddressLength, lpdwBytesReceived, lpOverlapped);
		}
    } CATCH_AND_REPORT()

    return FALSE;
}

bool IsWindowsVersionAtLeast(WORD wMajorVersion, WORD wMinorVersion, WORD wServicePackMajor) {
	OSVERSIONINFOEXW osvi = { sizeof(osvi), 0, 0, 0, 0, { 0 }, 0, 0 };
	DWORDLONG        const dwlConditionMask = VerSetConditionMask(
		VerSetConditionMask(
		VerSetConditionMask(
		0, VER_MAJORVERSION, VER_GREATER_EQUAL),
		VER_MINORVERSION, VER_GREATER_EQUAL),
		VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL);

	osvi.dwMajorVersion = wMajorVersion;
	osvi.dwMinorVersion = wMinorVersion;
	osvi.wServicePackMajor = wServicePackMajor;

	return VerifyVersionInfoW(&osvi, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR, dwlConditionMask) != FALSE;
}

void EnableFastLoopback(SOCKET s) {
#ifndef _WIN32_WINNT_WIN8
    #define _WIN32_WINNT_WIN8                   0x0602
#endif
    
    // if Win8+, use fast path option on loopback 
	if (IsWindowsVersionAtLeast(HIBYTE(_WIN32_WINNT_WIN8), LOBYTE(_WIN32_WINNT_WIN8), 0)) {
#ifndef SIO_LOOPBACK_FAST_PATH
		const DWORD SIO_LOOPBACK_FAST_PATH = 0x98000010;	// from Win8 SDK
#endif
		int enabled = 1;
		DWORD result_byte_count = -1;
		int result = f_WSAIoctl(s, SIO_LOOPBACK_FAST_PATH, &enabled, sizeof(enabled), NULL, 0, &result_byte_count, NULL, NULL);
		if (result != 0) {
			throw std::system_error(WSAGetLastError(), system_category(), "WSAIoctl failed");
		}
	}
}

BOOL FDAPI_ConnectEx(int fd,const struct sockaddr *name,int namelen,PVOID lpSendBuffer,DWORD dwSendDataLength,LPDWORD lpdwBytesSent,LPOVERLAPPED lpOverlapped)
{
    try
    {
        SOCKET s = RFDMap::getInstance().lookupSocket(fd);
        if( s != INVALID_SOCKET) {
            LPFN_CONNECTEX connectex;
            const GUID wsaid_connectex = WSAID_CONNECTEX;
            DWORD bytes;

            if( SOCKET_ERROR == 
                    WSAIoctl(fd,
                             SIO_GET_EXTENSION_FUNCTION_POINTER,
                             (void *)&wsaid_connectex,
                             sizeof(GUID),
                             &connectex,
                             sizeof(LPFN_ACCEPTEX),
                             &bytes,
                             NULL,
                             NULL)) {
                return FALSE;
            }

			EnableFastLoopback(s);

            return connectex(s,name,namelen,lpSendBuffer,dwSendDataLength,lpdwBytesSent,lpOverlapped);
        }
    } CATCH_AND_REPORT()

    return FALSE;
}

void FDAPI_GetAcceptExSockaddrs(int fd,PVOID lpOutputBuffer,DWORD dwReceiveDataLength,DWORD dwLocalAddressLength,DWORD dwRemoteAddressLength,LPSOCKADDR *LocalSockaddr,LPINT LocalSockaddrLength,LPSOCKADDR *RemoteSockaddr,LPINT RemoteSockaddrLength)
{
    try
    {
        SOCKET s = RFDMap::getInstance().lookupSocket(fd);
        if( s != INVALID_SOCKET) {
            LPFN_GETACCEPTEXSOCKADDRS getacceptsockaddrs;
            const GUID wsaid_getacceptsockaddrs = WSAID_GETACCEPTEXSOCKADDRS;
            DWORD bytes;

            if( SOCKET_ERROR == 
                    WSAIoctl(fd,
                             SIO_GET_EXTENSION_FUNCTION_POINTER,
                             (void *)&wsaid_getacceptsockaddrs,
                             sizeof(GUID),
                             &getacceptsockaddrs,
                             sizeof(LPFN_ACCEPTEX),
                             &bytes,
                             NULL,
                             NULL)) {
                return;
            }

            return getacceptsockaddrs(lpOutputBuffer,dwReceiveDataLength,dwLocalAddressLength,dwRemoteAddressLength,LocalSockaddr,LocalSockaddrLength,RemoteSockaddr,RemoteSockaddrLength);
        }
    } CATCH_AND_REPORT()
}

int FDAPI_UpdateAcceptContext(int fd)
{
    try
    {
        SOCKET s = RFDMap::getInstance().lookupSocket(fd);
        if( s != INVALID_SOCKET) {
            return setsockopt(fd,
                        SOL_SOCKET,
                        SO_UPDATE_ACCEPT_CONTEXT,
                        (char*)&s,
                        sizeof(SOCKET));
        }
    } CATCH_AND_REPORT()
    errno = EBADF;
    return RFDMap::invalidRFD;
}

auto f_socket = dllfunctor_stdcall<SOCKET, int, int, int>("ws2_32.dll", "socket");
int redis_socket_impl(int af,int type,int protocol) {
    RFD rfd = RFDMap::invalidRFD;
    try {
        SOCKET s = f_socket( af, type, protocol );
        if( s != INVALID_SOCKET ) {
            rfd = RFDMap::getInstance().addSocket( s );
        }
        return rfd; 
    } CATCH_AND_REPORT()

    return rfd;
}

// In unix a fd is a fd. All are closed with close().
auto f_closesocket = dllfunctor_stdcall<int, SOCKET>("ws2_32.dll", "closesocket");
int redis_close_impl(RFD rfd) {
    try {
        SOCKET s = RFDMap::getInstance().lookupSocket(rfd);
        if( s != INVALID_SOCKET ) {
            RFDMap::getInstance().removeSocket(s);
            return f_closesocket(s);
        } else {
            int posixFD = RFDMap::getInstance().lookupPosixFD(rfd);
            if(posixFD != -1) {
                RFDMap::getInstance().removePosixFD(posixFD);
                int retval = crt_close(posixFD);
                if( retval == -1 ) {
                    errno = GetLastError();
                }
                return retval;
            }
            else {
                errno = EBADF;
                return -1;
            }
        }
    } CATCH_AND_REPORT()

    return -1;
}

int __cdecl redis_open_impl(const char * _Filename, int _OpenFlag, int flags = 0) {
    RFD rfd = RFDMap::invalidRFD;
    try {
        int posixFD = crt_open(_Filename,_OpenFlag,flags);
        if(posixFD != -1) {
            rfd = RFDMap::getInstance().addPosixFD(posixFD);
            return rfd;
        } else {
            errno = GetLastError();
            return -1;
        }
    } CATCH_AND_REPORT()

    return rfd;
}

auto f_accept = dllfunctor_stdcall<SOCKET, SOCKET, struct sockaddr*, int*>("ws2_32.dll", "accept");
int redis_accept_impl(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    try {
        SOCKET s = RFDMap::getInstance().lookupSocket(sockfd);
        if( s != INVALID_SOCKET ) {
            SOCKET sAccept = f_accept(s, addr, addrlen);
            if( sAccept != INVALID_SOCKET ) {
                return RFDMap::getInstance().addSocket(sAccept);
            } else {
                errno = WSAGetLastError();
                if((errno==ENOENT)||(errno==WSAEWOULDBLOCK)) {
                    errno = EAGAIN;
                    return RFDMap::invalidRFD;
                }
            }
        }
    } CATCH_AND_REPORT()

    errno = EBADF;
    return RFDMap::invalidRFD;
}

auto f_setsockopt = dllfunctor_stdcall<int, SOCKET, int, int, const char*, int>("ws2_32.dll", "setsockopt");
int redis_setsockopt_impl(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    try {
        SOCKET s = RFDMap::getInstance().lookupSocket(sockfd);
        if( s != INVALID_SOCKET ) {
            return f_setsockopt(s, level, optname,(const char*)optval, optlen);
        }
    } CATCH_AND_REPORT()

    errno = EBADF;
    return -1;
}

auto f_ioctlsocket = dllfunctor_stdcall<int, SOCKET, long, u_long*>("ws2_32.dll", "ioctlsocket");
int redis_fcntl_impl(int fd, int cmd, int flags = 0 ) {
    try {
        SOCKET s = RFDMap::getInstance().lookupSocket(fd);
        if( s != INVALID_SOCKET ) {
            switch(cmd) {
                case F_GETFL:
                {
                    // Since there is no way to determine if a socket is blocking in winsock, we keep track of this separately.
                    RedisSocketState state;
                    RFDMap::getInstance().GetSocketState(s, state);
                    return state.IsBlockingSocket ? O_NONBLOCK : 0;
                }
                case F_SETFL:
                {
                    RedisSocketState state;
                    state.IsBlockingSocket = ((flags & O_NONBLOCK) != 0);
                    u_long fionbio_flags = state.IsBlockingSocket;
                    if( f_ioctlsocket(s, FIONBIO, &fionbio_flags) == SOCKET_ERROR ) {
                        errno = WSAGetLastError();
                        return -1;
                    } else {
                        RFDMap::getInstance().SetSocketState( s, state );
                        return 0;
                    }
                    break;
                }
                default:
                {
                    DebugBreak();
                    return -1;
                }
            }
        } 
    } CATCH_AND_REPORT()

    errno = EBADF;
    return -1;
}

static auto f_WSAFDIsSet = dllfunctor_stdcall<int, SOCKET, fd_set*>("ws2_32.dll", "__WSAFDIsSet");
#define FD_ISSET(fd, set) f_WSAFDIsSet((SOCKET)(fd), (fd_set *)(set))
int redis_FD_ISSET_impl(int fd, fd_set* pSet) {
    fd_set copy;
    FD_ZERO(&copy);
    for (u_int n = 0; n < pSet->fd_count; n++)  {
        SOCKET s = RFDMap::getInstance().lookupSocket((RFD)(pSet->fd_array[n]));
        if (s == INVALID_SOCKET) {
            errno = EBADF;
            return -1;
        }
        FD_SET(s, &copy);
    }
    SOCKET s = RFDMap::getInstance().lookupSocket(fd);
    if (s == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
    return f_WSAFDIsSet(s, &copy);
}

int redis_poll_impl(struct pollfd *fds, nfds_t nfds, int timeout) {
    try {
        struct pollfd* pollCopy = new struct pollfd[nfds];
        if (pollCopy == NULL) {
            errno = ENOMEM;
            return -1;
        }

        // NOTE: Treating the fds.fd as a Redis file descriptor and converting to a SOCKET for WSAPoll. 
        for (nfds_t n = 0; n < nfds; n ++) {
            pollCopy[n].fd = RFDMap::getInstance().lookupSocket((RFD)(fds[n].fd));
            pollCopy[n].events = fds[n].events;
            pollCopy[n].revents = fds[n].revents;
        }

        if (IsWindowsVersionAtLeast(HIBYTE(_WIN32_WINNT_WIN6), LOBYTE(_WIN32_WINNT_WIN6), 0)) {
            static auto f_WSAPoll = dllfunctor_stdcall<int, WSAPOLLFD*, ULONG, INT>("ws2_32.dll", "WSAPoll");

            // See the community addition comments at http://msdn.microsoft.com/en-us/library/windows/desktop/ms741669%28v=vs.85%29.aspx for this API. 
            // BugCheck seems to indicate that problems with this API in Win8 have been addressed, but this needs to be verified.
            int ret = f_WSAPoll(pollCopy, nfds, timeout);

            for (nfds_t n = 0; n < nfds; n++) {
                fds[n].events = pollCopy[n].events;
                fds[n].revents = pollCopy[n].revents;
            }

            delete pollCopy;
            pollCopy = NULL;

            return ret;
        } else {
            int ret;
            fd_set readSet;
            fd_set writeSet;
            fd_set excepSet;

            FD_ZERO(&readSet);
            FD_ZERO(&writeSet);
            FD_ZERO(&excepSet);

            if (nfds >= FD_SETSIZE) {
                errno = EINVAL;
                return -1;
            }

            nfds_t i;
            for (i = 0; i < nfds; i++) {
                if (fds[i].fd < 0) {
                    continue;
                }
                if (pollCopy[i].fd >= FD_SETSIZE) {
                    errno = EINVAL;
                    return -1;
                }

                if (pollCopy[i].events & POLLIN) FD_SET(pollCopy[i].fd, &readSet);
                if (pollCopy[i].events & POLLOUT) FD_SET(pollCopy[i].fd, &writeSet);
                if (pollCopy[i].events & POLLERR) FD_SET(pollCopy[i].fd, &excepSet);
            }

            if (timeout < 0) {
                ret = select(0, &readSet, &writeSet, &excepSet, NULL);
            } else {
                struct timeval tv;
                tv.tv_sec = timeout / 1000;
                tv.tv_usec = 1000 * (timeout % 1000);
                ret = select(0, &readSet, &writeSet, &excepSet, &tv);
            }

            if (ret < 0) {
                return ret;
            }

            for (i = 0; i < nfds; i++) {
                fds[i].revents = 0;

                if (FD_ISSET(pollCopy[i].fd, &readSet)) fds[i].revents |= POLLIN;
                if (FD_ISSET(pollCopy[i].fd, &writeSet)) fds[i].revents |= POLLOUT;
                if (FD_ISSET(pollCopy[i].fd, &excepSet)) fds[i].revents |= POLLERR;
            }

            delete pollCopy;
            pollCopy = NULL;

            return ret;
        }
    } CATCH_AND_REPORT()

    errno = EBADF;
    return -1;
}

auto f_getsockopt = dllfunctor_stdcall<int, SOCKET, int, int, char*, int*>("ws2_32.dll", "getsockopt");
int redis_getsockopt_impl(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    try {
        SOCKET s = RFDMap::getInstance().lookupSocket(sockfd);
        if( s == INVALID_SOCKET ) {
            errno = EBADF;
            return -1;
        }

        return f_getsockopt(s,level,optname,(char*)optval,optlen);
    } CATCH_AND_REPORT()

    errno = EBADF;
    return -1;
}


auto f_connect = dllfunctor_stdcall<int, SOCKET, const struct sockaddr*, int>("ws2_32.dll", "connect");
int redis_connect_impl(int sockfd, const struct sockaddr *addr, size_t addrlen) {
    try {
        SOCKET s = RFDMap::getInstance().lookupSocket(sockfd);
        if( s == INVALID_SOCKET ) {
            errno = EBADF;
            return -1;
        }
		EnableFastLoopback(s);
        int r = f_connect(s, addr, (int)addrlen);
        errno = WSAGetLastError();
        if ((errno == WSAEINVAL) || (errno == WSAEWOULDBLOCK) || (errno == WSA_IO_PENDING)) {
            errno = EINPROGRESS;
        }
        return r;
    } CATCH_AND_REPORT()

    errno = EBADF;
    return -1;
}


auto f_recv = dllfunctor_stdcall<int, SOCKET, char*, int, int>("ws2_32.dll", "recv");
ssize_t redis_read_impl(int fd, void *buf, size_t count) {
    try {
        SOCKET s = RFDMap::getInstance().lookupSocket( fd );
        if( s != INVALID_SOCKET ) {
            int retval = f_recv( s, (char*)buf, (unsigned int)count, 0);
            if (retval == -1) {
                errno = GetLastError();
                if (errno == WSAEWOULDBLOCK) {
                    errno = EAGAIN;
                }
            }
            return retval;
        } else {
            int posixFD = RFDMap::getInstance().lookupPosixFD( fd );
            if( posixFD != -1 ) {
                int retval = crt_read(posixFD, buf,(unsigned int)count);
                if(retval == -1) {
                    errno = GetLastError();
                }
                return retval;
            }
            else {
                errno = EBADF;
                return 0;
            }
        }
    } CATCH_AND_REPORT()

    errno = EBADF;
    return -1;
}

auto f_send = dllfunctor_stdcall<int, SOCKET, const char*, int, int>("ws2_32.dll", "send");
ssize_t redis_write_impl(int fd, const void *buf, size_t count) {
    try {
        SOCKET s = RFDMap::getInstance().lookupSocket( fd );
        if( s != INVALID_SOCKET ) {
            return (int)f_send( s, (char*)buf, (unsigned int)count, 0);
        } else {
            int posixFD = RFDMap::getInstance().lookupPosixFD( fd );
            if( posixFD != -1 ) {
                if (posixFD == _fileno(stdout)) {
                    DWORD bytesWritten = 0;
                    if (FALSE != ParseAndPrintANSIString(GetStdHandle(STD_OUTPUT_HANDLE), buf, (DWORD)count, &bytesWritten)) {
                        return (int)bytesWritten;
                    } else {
                        errno = GetLastError();
                        return 0;
                    }
                } else if (posixFD == _fileno(stderr)) {
                    DWORD bytesWritten = 0;
                    if (FALSE != ParseAndPrintANSIString(GetStdHandle(STD_ERROR_HANDLE), buf, (DWORD)count, &bytesWritten)) {
                        return (int)bytesWritten;
                    } else {
                        errno = GetLastError();
                        return 0;
                    }
                } else {
                    int retval = crt_write(posixFD, buf, (unsigned int)count);
                    if (retval == -1) {
                        errno = GetLastError();
                    }
                    return retval;
                }
            }
            else {
                errno = EBADF;
                return 0;
            }
        }
    } CATCH_AND_REPORT()

    errno = EBADF;
    return -1;
}

int redis_fsync_impl(int fd) {
    try {
        int posixFD = RFDMap::getInstance().lookupPosixFD( fd );
        if( posixFD == -1 ) {
            // There is one place in Redis where we are not tracking posix FDs because it involves
            // direct ocnversion of a FILE* to an FD.
            posixFD = fd;
        }

        HANDLE h = (HANDLE) crtget_osfhandle(posixFD);
        DWORD err;

        if (h == INVALID_HANDLE_VALUE) {
            errno = EBADF;
            return -1;
        }

        if (!FlushFileBuffers(h)) {
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
    } CATCH_AND_REPORT()

    errno = EBADF;
    return -1;
}

int redis_fstat_impl(int fd, struct __stat64 *buffer) {
    try {
        int posixFD = RFDMap::getInstance().lookupPosixFD( fd );
        if( posixFD == -1 ) {
            posixFD = fd;
        }

        return _fstat64(posixFD, buffer);
    } CATCH_AND_REPORT()

    errno = EBADF;
    return -1;
}


auto f_listen = dllfunctor_stdcall<int, SOCKET, int>("ws2_32.dll", "listen");
int redis_listen_impl(int sockfd, int backlog) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( sockfd );
        if( s != INVALID_SOCKET ) {
			EnableFastLoopback(s);
			return f_listen( s, backlog );
        } else {
            errno = EBADF;
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

int redis_ftruncate_impl(int fd, long long length) {
    try
    {
        LARGE_INTEGER l, o;
        HANDLE h = INVALID_HANDLE_VALUE;

        int posixFD = RFDMap::getInstance().lookupPosixFD( fd );
        if( posixFD == -1 ) {
            h = (HANDLE) crtget_osfhandle (fd);
        } else {
            h = (HANDLE) crtget_osfhandle (posixFD);
        }

        if( h == INVALID_HANDLE_VALUE) {
            errno = EBADF;
            return -1;
        }

        l.QuadPart = length;

        if (!SetFilePointerEx(h, l, &o, FILE_BEGIN)) return -1;
        if (!SetEndOfFile(h)) return -1;

        return 0;
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

auto f_bind = dllfunctor_stdcall<int, SOCKET, const struct sockaddr*, int>("ws2_32.dll", "bind");
int redis_bind_impl(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( sockfd );
        if( s != INVALID_SOCKET ) {
            return f_bind(s, addr, addrlen);
        } else {
            errno = EBADF;
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

auto f_shutdown = dllfunctor_stdcall<int, SOCKET, int>("ws2_32.dll", "shutdown");
int redis_shutdown_impl(int sockfd, int how) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( sockfd );
        if( s != INVALID_SOCKET ) {
            return f_shutdown(s, how);
        } else {
            errno = EBADF;
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

auto f_WSASetLastError = dllfunctor_stdcall<void, int>("ws2_32.dll", "WSASetLastError");
void redis_WSASetLastError_impl(int iError) {
    f_WSASetLastError(iError);
}

auto f_WSAGetLastError = dllfunctor_stdcall<int>("ws2_32.dll", "WSAGetLastError");
int redis_WSAGetLastError_impl(void) {
    return f_WSAGetLastError();
}

auto f_WSAGetOverlappedResult = dllfunctor_stdcall<BOOL, SOCKET, LPWSAOVERLAPPED, LPDWORD, BOOL, LPDWORD>("ws2_32.dll", "WSAGetOverlappedResult");
BOOL redis_WSAGetOverlappedResult_impl(int rfd, LPWSAOVERLAPPED lpOverlapped, LPDWORD lpcbTransfer, BOOL fWait, LPDWORD lpdwFlags) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( rfd );
        if( s != INVALID_SOCKET ) {
            return f_WSAGetOverlappedResult(s,lpOverlapped, lpcbTransfer, fWait, lpdwFlags);
        } else {
            errno = EBADF;
            return SOCKET_ERROR;
        }
    } CATCH_AND_REPORT();

    return SOCKET_ERROR;
}

int redis_WSAIoctl_impl(RFD rfd,DWORD dwIoControlCode,LPVOID lpvInBuffer,DWORD cbInBuffer,LPVOID lpvOutBuffer,DWORD cbOutBuffer,LPDWORD lpcbBytesReturned,LPWSAOVERLAPPED lpOverlapped,LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( rfd );
        if( s != INVALID_SOCKET ) {
            return f_WSAIoctl(s,dwIoControlCode,lpvInBuffer,cbInBuffer,lpvOutBuffer,cbOutBuffer,lpcbBytesReturned,lpOverlapped,lpCompletionRoutine);
        } else {
            errno = EBADF;
            return SOCKET_ERROR;
        }
    }  CATCH_AND_REPORT();

    return SOCKET_ERROR;
}


auto f_WSASend = dllfunctor_stdcall<int, SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE>("ws2_32.dll", "WSASend");
int redis_WSASend_impl(int rfd, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent, DWORD dwFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( rfd );
        if( s != INVALID_SOCKET ) {
            return f_WSASend( s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags, lpOverlapped, lpCompletionRoutine );
        } else {
            errno = EBADF;
            return SOCKET_ERROR;
        }
    }  CATCH_AND_REPORT();

    return SOCKET_ERROR;
}

auto f_WSARecv = dllfunctor_stdcall<int, SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE>("ws2_32.dll", "WSARecv");
int redis_WSARecv_impl(int rfd, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( rfd );
        if( s != INVALID_SOCKET ) {
            return f_WSARecv( s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags, lpOverlapped, lpCompletionRoutine );
        } else {
            errno = EBADF;
            return SOCKET_ERROR;
        }
    } CATCH_AND_REPORT();

    return SOCKET_ERROR;
}

int redis_WSACleanup_impl(void) {
	return f_WSACleanup();
}

int redis_ioctlsocket_impl(int rfd, long cmd, u_long *argp) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( rfd );
        if( s != INVALID_SOCKET ) {
            return f_ioctlsocket(s,cmd,argp);
        } else {
            errno = EBADF;
            return SOCKET_ERROR;
        }
    } CATCH_AND_REPORT();

    return SOCKET_ERROR;
}

auto f_inet_addr = dllfunctor_stdcall<unsigned long, const char*>("ws2_32.dll", "inet_addr");
unsigned long redis_inet_addr_impl(const char *cp) {
    return f_inet_addr(cp);
}


auto f_gethostbyname = dllfunctor_stdcall<struct hostent*, const char*>("ws2_32.dll", "gethostbyname");
struct hostent* redis_gethostbyname_impl(const char *name) {
    return f_gethostbyname(name);
}


auto f_inet_ntoa = dllfunctor_stdcall<char *, struct in_addr>("ws2_32.dll", "inet_ntoa");
char* redis_inet_ntoa_impl(struct in_addr in) {
    return f_inet_ntoa(in);
}

auto f_htons = dllfunctor_stdcall<u_short, u_short>("ws2_32.dll", "htons");
u_short redis_htons_impl(u_short hostshort) {
    return f_htons(hostshort);
}

auto f_htonl = dllfunctor_stdcall<u_long, u_long>("ws2_32.dll", "htonl");
u_long redis_htonl_impl(u_long hostlong) {
    return f_htonl(hostlong);
}

auto f_getpeername = dllfunctor_stdcall<int, SOCKET, struct sockaddr*, int*>("ws2_32.dll", "getpeername");
int redis_getpeername_impl(int sockfd, struct sockaddr *addr, socklen_t * addrlen) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( sockfd );
        if( s != INVALID_SOCKET ) {
            return f_getpeername(s,addr, addrlen);
        } else {
            errno = EBADF;
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

auto f_getsockname = dllfunctor_stdcall<int, SOCKET, struct sockaddr*, int*>("ws2_32.dll", "getsockname");
int redis_getsockname_impl(int sockfd, struct sockaddr* addrsock, int* addrlen) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( sockfd );
        if( s != INVALID_SOCKET ) {
            return f_getsockname(s,addrsock, addrlen);
        } else {
            errno = EBADF;
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

auto f_ntohs = dllfunctor_stdcall<u_short,u_short>("ws2_32.dll", "ntohs");
u_short redis_ntohs_impl(u_short netshort) {
    return f_ntohs( netshort );
}

int redis_setmode_impl(int fd,int mode) {
    return crtsetmode(fd,mode);
}

size_t redis_fwrite_impl(const void * _Str, size_t _Size, size_t _Count, FILE * _File) {
    return crtfwrite(_Str, _Size, _Count, _File);
}

auto f_select = dllfunctor_stdcall<int, int, fd_set*, fd_set*, fd_set*, const struct timeval*>("ws2_32.dll", "select");
int redis_select_impl(int nfds, fd_set *readfds, fd_set *writefds,fd_set *exceptfds, struct timeval *timeout) {
   try { 
       if (readfds != NULL) {
           for(u_int r = 0; r < readfds->fd_count; r++) {
               readfds->fd_array[r] = RFDMap::getInstance().lookupSocket( (RFD)readfds->fd_array[r] );
           }
       }
       if (writefds != NULL) {
            for(u_int r = 0; r < writefds->fd_count; r++) {
                writefds->fd_array[r] = RFDMap::getInstance().lookupSocket( (RFD)writefds->fd_array[r] );
            }
       }
       if (exceptfds != NULL ) {
           for(u_int r = 0; r < exceptfds->fd_count; r++) {
               exceptfds->fd_array[r] = RFDMap::getInstance().lookupSocket( (RFD)exceptfds->fd_array[r] );
           }
       }

       return f_select(nfds,readfds,writefds,exceptfds,timeout);
   } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

auto f_ntohl = dllfunctor_stdcall<u_long, u_long>("ws2_32.dll", "ntohl");
u_int redis_ntohl_impl(u_int netlong){
    return f_ntohl(netlong);
}

int redis_isatty_impl(int fd) {
   try {
        int posixFD = RFDMap::getInstance().lookupPosixFD(fd);
        if( posixFD != -1) {
            return crt_isatty(posixFD);
		} else if (fd >= 0 && fd <= 2) {
			return crt_isatty(fd);
		} else {
			errno = EBADF;
			return 0;
		}
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

int redis_access_impl(const char *pathname, int mode) {
    return crt_access(pathname, mode);
}

u_int64 redis_lseek64_impl(int fd, u_int64 offset, int whence) {
   try {
        int posixFD = RFDMap::getInstance().lookupPosixFD(fd);
        if( posixFD != -1) {
            return crt_lseek64(posixFD, offset, whence);
        } else {
            errno = EBADF;
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

intptr_t redis_get_osfhandle_impl(int fd) {
   try {
        int posixFD = RFDMap::getInstance().lookupPosixFD(fd);
        if( posixFD != -1) {
            return crtget_osfhandle(posixFD);
        } else {
            errno = EBADF;
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

auto f_freeaddrinfo = dllfunctor_stdcall<void, addrinfo*>("ws2_32.dll", "freeaddrinfo");
void redis_freeaddrinfo_impl(struct addrinfo *ai) {
    f_freeaddrinfo(ai);   
}

auto f_getaddrinfo = dllfunctor_stdcall<int, PCSTR, PCSTR, const ADDRINFOA*, ADDRINFOA**>("ws2_32.dll", "getaddrinfo");
int redis_getaddrinfo_impl(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    return f_getaddrinfo(node, service,hints, res);
}

const char* redis_inet_ntop_impl(int af, const void *src, char *dst, size_t size) {
    if (IsWindowsVersionAtLeast(HIBYTE(_WIN32_WINNT_WIN6), LOBYTE(_WIN32_WINNT_WIN6), 0)) {
        static auto f_inet_ntop = dllfunctor_stdcall<const char*, int, const void*, char*, size_t>("ws2_32.dll", "inet_ntop");
        return f_inet_ntop(af, src, dst, size);
    } else {
        static auto f_WSAAddressToStringA = dllfunctor_stdcall<int, LPSOCKADDR, DWORD, LPWSAPROTOCOL_INFO, LPSTR, LPDWORD>("ws2_32.dll", "WSAAddressToStringA");
        struct sockaddr_in srcaddr;

        memset(&srcaddr, 0, sizeof(struct sockaddr_in));
        memcpy(&(srcaddr.sin_addr), src, sizeof(srcaddr.sin_addr));

        srcaddr.sin_family = af;
        if (f_WSAAddressToStringA((struct sockaddr*) &srcaddr, sizeof(struct sockaddr_in), 0, dst, (LPDWORD)&size) != 0) {
            return NULL;
        }
        return dst;
    }
}

BOOL ParseStorageAddress(const char *ip, int port, SOCKADDR_STORAGE* pSotrageAddr) {
    struct addrinfo hints, *res;
    int status;
    char port_buffer[6];

    sprintf(port_buffer, "%hu", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    /* Setting AI_PASSIVE will give you a wildcard address if addr is NULL */
    hints.ai_flags = AI_NUMERICSERV | AI_PASSIVE;

    if ((status = getaddrinfo(ip, port_buffer, &hints, &res) != 0)) {
        fprintf(stderr, "getaddrinfo: %S\n", gai_strerror(status));
        return FALSE;
    }

    /* Note, we're taking the first valid address, there may be more than one */
    memcpy(pSotrageAddr, res->ai_addr, res->ai_addrlen);

    freeaddrinfo(res);
    return TRUE;
}

int StorageSize(SOCKADDR_STORAGE *ss) {
    switch (ss->ss_family) {
        case AF_INET:
            return sizeof(SOCKADDR_IN);
        case AF_INET6:
            return sizeof(SOCKADDR_IN6);
        default:
            return -1;
    }
}


class Win32_FDSockMap {
public:
    static Win32_FDSockMap& getInstance() {
        static Win32_FDSockMap    instance; // Instantiated on first use. Guaranteed to be destroyed.
        return instance;
    }

private:
    Win32_FDSockMap() {
        InitWinsock();

        socket = redis_socket_impl;
        fdapi_close = redis_close_impl;
        open = redis_open_impl;
        setsockopt = redis_setsockopt_impl;
        fcntl = redis_fcntl_impl;
        poll = redis_poll_impl; 
        getsockopt = redis_getsockopt_impl;
        connect = redis_connect_impl;
        read = redis_read_impl;
        write = redis_write_impl; 
        fsync = redis_fsync_impl;
        fdapi_fstat64 = (_redis_fstat)redis_fstat_impl;
        listen = redis_listen_impl;
        ftruncate = redis_ftruncate_impl;
        bind = redis_bind_impl;
        shutdown = redis_shutdown_impl;
        htons = redis_htons_impl;
        htonl = redis_htonl_impl;
        getpeername = redis_getpeername_impl;
        getsockname = redis_getsockname_impl;
        ntohs = redis_ntohs_impl;
        ioctlsocket = redis_ioctlsocket_impl;
        inet_addr = redis_inet_addr_impl;
        gethostbyname = redis_gethostbyname_impl;
        inet_ntoa = redis_inet_ntoa_impl; 
        fdapi_fwrite = redis_fwrite_impl;
        fdapi_setmode = redis_setmode_impl;
        WSASetLastError = redis_WSASetLastError_impl;
        WSAGetLastError = redis_WSAGetLastError_impl;
        WSAIoctl = redis_WSAIoctl_impl;
        WSASend = redis_WSASend_impl;
        WSARecv = redis_WSARecv_impl;
        WSACleanup = redis_WSACleanup_impl;
        WSAGetOverlappedResult = redis_WSAGetOverlappedResult_impl;
        select = redis_select_impl;
        ntohl = redis_ntohl_impl;
        isatty = redis_isatty_impl;
        access = redis_access_impl;
        lseek64 = redis_lseek64_impl;
        fdapi_get_osfhandle = redis_get_osfhandle_impl;
        freeaddrinfo = redis_freeaddrinfo_impl;
        getaddrinfo = redis_getaddrinfo_impl;
        inet_ntop = redis_inet_ntop_impl;
        FD_ISSET = redis_FD_ISSET_impl;
        accept = redis_accept_impl;
    }

    ~Win32_FDSockMap() {
        CleanupWinsock();
    }

    Win32_FDSockMap(Win32_FDSockMap const&);	  // Don't implement to guarantee singleton semantics
    void operator=(Win32_FDSockMap const&); // Don't implement to guarantee singleton semantics
};

// guarantee global initialization
static class Win32_FDSockMap& init = Win32_FDSockMap::getInstance();


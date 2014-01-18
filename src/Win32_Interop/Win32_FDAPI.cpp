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

#include "Win32_FDAPI.h"
#include "win32_rfdmap.h"
#include "..\APIBridge\APIBridge.h"
#include <exception>
#include <mswsock.h>
#include <sys/stat.h>

#define CATCH_AND_REPORT()  catch(const std::exception &){printf("std exception");}catch(...){printf("other exception");}

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
redis_setmode _setmode = NULL;
redis_select select = NULL;
redis_ntohl ntohl = NULL;
redis_isatty isatty = NULL;
redis_access access = NULL;
redis_lseek64 lseek64 = NULL;
redis_get_osfhandle _get_osfhandle = NULL;

// Unix compatible FD based routines
redis_socket socket = NULL;
redis_close close = NULL;
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
}

int InitWinsock() {
   WSADATA t_wsa;
    WORD wVers;
    int iError;

    wVers = MAKEWORD(2, 2);
    iError = APIBridge::WSAStartup(wVers, &t_wsa);

    if(iError != NO_ERROR || LOBYTE(t_wsa.wVersion) != 2 || HIBYTE(t_wsa.wVersion) != 2 ) {
        exit(1);
    } else {
      return 0;
    }
}

int  CleanupWinsock() {
    return APIBridge::WSACleanup();
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

BOOL FDAPI_AcceptEx(int listenFD,int acceptFD,PVOID lpOutputBuffer,DWORD dwReceiveDataLength,DWORD dwLocalAddressLength,DWORD dwRemoteAddressLength,LPDWORD lpdwBytesReceived,LPOVERLAPPED lpOverlapped)
{
    try
    {
        SOCKET sListen = RFDMap::getInstance().lookupSocket(listenFD);
        SOCKET sAccept = RFDMap::getInstance().lookupSocket(acceptFD);
        if( sListen != INVALID_SOCKET &&  sAccept != INVALID_SOCKET) {
            LPFN_ACCEPTEX acceptex;
            const GUID wsaid_acceptex = WSAID_ACCEPTEX;
            DWORD bytes;

            if( SOCKET_ERROR == 
                    WSAIoctl(listenFD,
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

            return acceptex(sListen, sAccept, lpOutputBuffer,dwReceiveDataLength, dwLocalAddressLength, dwRemoteAddressLength, lpdwBytesReceived, lpOverlapped);
        }
    } CATCH_AND_REPORT()

    return FALSE;
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


int redis_socket_impl(int af,int type,int protocol) {
    RFD rfd = RFDMap::invalidRFD;
    try {
        SOCKET s = APIBridge::socket( af, type, protocol );
        if( s != INVALID_SOCKET ) {
            rfd = RFDMap::getInstance().addSocket( s );
        }
        return rfd; 
    } CATCH_AND_REPORT()

    return rfd;
}

// In unix a fd is a fd. All are closed with close().
int redis_close_impl(RFD rfd) {
    try {
        SOCKET s = RFDMap::getInstance().lookupSocket(rfd);
        if( s != INVALID_SOCKET ) {
            RFDMap::getInstance().removeSocket(s);
            return APIBridge::closesocket(s);
        } else {
            int posixFD = RFDMap::getInstance().lookupPosixFD(rfd);
            if(posixFD != -1) {
                RFDMap::getInstance().removePosixFD(posixFD);
                int retval = APIBridge::close(posixFD);
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
        int posixFD = APIBridge::open(_Filename,_OpenFlag,flags);
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


int redis_accept_impl(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    try {
        SOCKET s = RFDMap::getInstance().lookupSocket(sockfd);
        if( s != INVALID_SOCKET ) {
            SOCKET sAccept = APIBridge::accept(s, addr, addrlen);
            if( sAccept != INVALID_SOCKET ) {
                return RFDMap::getInstance().addSocket(sAccept);
            } else {
                errno = WSAGetLastError();
                if((errno==ENOENT)||(errno==WSAEWOULDBLOCK)) {
                    errno=EAGAIN;
                }
            }
        }
    } CATCH_AND_REPORT()

    errno = EBADF;
    return RFDMap::invalidRFD;
}

int redis_setsockopt_impl(int sockfd, int level, int optname,const void *optval, socklen_t optlen) {
    try {
        SOCKET s = RFDMap::getInstance().lookupSocket(sockfd);
        if( s != INVALID_SOCKET ) {
            return APIBridge::setsockopt(s, level, optname,(const char*)optval, optlen);
        }
    } CATCH_AND_REPORT()

    errno = EBADF;
    return -1;
}


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
                    if( APIBridge::ioctlsocket(s, FIONBIO, &fionbio_flags) == SOCKET_ERROR ) {
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

        // See the community addition comments at http://msdn.microsoft.com/en-us/library/windows/desktop/ms741669%28v=vs.85%29.aspx for this API. 
        // BugCheck seems to indicate that problems with this API in Win8 have been addressed, but this needs to be verified.
        int ret = APIBridge::WSAPoll(pollCopy, nfds, timeout);

        for (nfds_t n = 0; n < nfds; n ++) {
            fds[n].events = pollCopy[n].events;
            fds[n].revents = pollCopy[n].revents;
        }

        delete pollCopy;
        pollCopy = NULL;

        return ret;
    } CATCH_AND_REPORT()

    errno = EBADF;
    return -1;
}

int redis_getsockopt_impl(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    try {
        SOCKET s = RFDMap::getInstance().lookupSocket(sockfd);
        if( s == INVALID_SOCKET ) {
            errno = EBADF;
            return -1;
        }

        return APIBridge::getsockopt(s,level,optname,(char*)optval,optlen);
    } CATCH_AND_REPORT()

    errno = EBADF;
    return -1;
}


int redis_connect_impl(int sockfd, const struct sockaddr *addr, size_t addrlen) {
    try {
        SOCKET s = RFDMap::getInstance().lookupSocket(sockfd);
        if( s == INVALID_SOCKET ) {
            errno = EBADF;
            return -1;
        }
        int r = APIBridge::connect(s, addr, (int)addrlen);
        errno = WSAGetLastError();
        if ((errno == WSAEINVAL) || (errno == WSAEWOULDBLOCK) || (errno == WSA_IO_PENDING)) {
            errno = EINPROGRESS;
        }
        return r;
    } CATCH_AND_REPORT()

    errno = EBADF;
    return -1;
}

ssize_t redis_read_impl(int fd, void *buf, size_t count) {
    try {
        SOCKET s = RFDMap::getInstance().lookupSocket( fd );
        if( s != INVALID_SOCKET ) {
            int retval = APIBridge::recv( s, (char*)buf, (unsigned int)count, 0);
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
                int retval = APIBridge::read(posixFD, buf,(unsigned int)count);
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

ssize_t redis_write_impl(int fd, const void *buf, size_t count) {
    try {
        SOCKET s = RFDMap::getInstance().lookupSocket( fd );
        if( s != INVALID_SOCKET ) {
            return (int)APIBridge::send( s, (char*)buf, (unsigned int)count, 0);
        } else {
            int posixFD = RFDMap::getInstance().lookupPosixFD( fd );
            if( posixFD != -1 ) {
                int retval = APIBridge::write(posixFD, buf,(unsigned int)count);
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

int redis_fsync_impl(int fd) {
    try {
        int posixFD = RFDMap::getInstance().lookupPosixFD( fd );
        if( posixFD == -1 ) {
            // There is one place in Redis where we are not tracking posix FDs because it involves
            // direct ocnversion of a FILE* to an FD.
            posixFD = fd;
        }

        HANDLE h = (HANDLE) APIBridge::_get_osfhandle(posixFD);
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

int redis_listen_impl(int sockfd, int backlog) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( sockfd );
        if( s != INVALID_SOCKET ) {
            return APIBridge::listen( s, backlog );
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
            h = (HANDLE) APIBridge::_get_osfhandle (fd);
        } else {
            h = (HANDLE) APIBridge::_get_osfhandle (posixFD);
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

int redis_bind_impl(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( sockfd );
        if( s != INVALID_SOCKET ) {
            return APIBridge::bind(s, addr, addrlen);
        } else {
            errno = EBADF;
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

int redis_shutdown_impl(int sockfd, int how) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( sockfd );
        if( s != INVALID_SOCKET ) {
            return APIBridge::shutdown(s, how);
        } else {
            errno = EBADF;
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

void redis_WSASetLastError_impl(int iError) {
    APIBridge::WSASetLastError(iError);
}

int redis_WSAGetLastError_impl(void) {
    return APIBridge::WSAGetLastError();
}

BOOL redis_WSAGetOverlappedResult_impl(int rfd,LPWSAOVERLAPPED lpOverlapped, LPDWORD lpcbTransfer, BOOL fWait, LPDWORD lpdwFlags) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( rfd );
        if( s != INVALID_SOCKET ) {
            return APIBridge::WSAGetOverlappedResult(s,lpOverlapped, lpcbTransfer, fWait, lpdwFlags);
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
            return APIBridge::WSAIoctl(s,dwIoControlCode,lpvInBuffer,cbInBuffer,lpvOutBuffer,cbOutBuffer,lpcbBytesReturned,lpOverlapped,lpCompletionRoutine);
        } else {
            errno = EBADF;
            return SOCKET_ERROR;
        }
    }  CATCH_AND_REPORT();

    return SOCKET_ERROR;
}


int redis_WSASend_impl(int rfd, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent, DWORD dwFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( rfd );
        if( s != INVALID_SOCKET ) {
            return APIBridge::WSASend( s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags, lpOverlapped, lpCompletionRoutine );
        } else {
            errno = EBADF;
            return SOCKET_ERROR;
        }
    }  CATCH_AND_REPORT();

    return SOCKET_ERROR;
}

int redis_WSARecv_impl(int rfd,LPWSABUF lpBuffers,DWORD dwBufferCount,LPDWORD lpNumberOfBytesRecvd,LPDWORD lpFlags,LPWSAOVERLAPPED lpOverlapped,LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( rfd );
        if( s != INVALID_SOCKET ) {
            return APIBridge::WSARecv( s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags, lpOverlapped, lpCompletionRoutine );
        } else {
            errno = EBADF;
            return SOCKET_ERROR;
        }
    } CATCH_AND_REPORT();

    return SOCKET_ERROR;
}

int redis_WSACleanup_impl(void) {
    return APIBridge::WSACleanup();
}

int redis_ioctlsocket_impl(int rfd,long cmd,u_long *argp) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( rfd );
        if( s != INVALID_SOCKET ) {
            return APIBridge::ioctlsocket(s,cmd,argp);
        } else {
            errno = EBADF;
            return SOCKET_ERROR;
        }
    } CATCH_AND_REPORT();

    return SOCKET_ERROR;
}

unsigned long redis_inet_addr_impl(const char *cp) {
    return APIBridge::inet_addr(cp);
}


struct hostent* redis_gethostbyname_impl(const char *name) {
    return APIBridge::gethostbyname(name);
}


char* redis_inet_ntoa_impl(struct in_addr in) {
    return APIBridge::inet_ntoa(in);
}

u_short redis_htons_impl(u_short hostshort) {
    return APIBridge::htons(hostshort);
}

u_long redis_htonl_impl(u_long hostlong) {
    return APIBridge::htonl(hostlong);
}

int redis_getpeername_impl(int sockfd, struct sockaddr *addr, socklen_t * addrlen) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( sockfd );
        if( s != INVALID_SOCKET ) {
            return APIBridge::getpeername(s,addr, addrlen);
        } else {
            errno = EBADF;
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

int redis_getsockname_impl(int sockfd, struct sockaddr* addrsock, int* addrlen ) {
   try {
        SOCKET s = RFDMap::getInstance().lookupSocket( sockfd );
        if( s != INVALID_SOCKET ) {
            return APIBridge::getsockname(s,addrsock, addrlen);
        } else {
            errno = EBADF;
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

u_short redis_ntohs_impl(u_short netshort) {
    return APIBridge::ntohs( netshort );
}

int redis_setmode_impl(int fd,int mode) {
    return APIBridge::_setmode(fd,mode);
}

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

       return APIBridge::select(nfds,readfds,writefds,exceptfds,timeout);
   } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

u_int redis_ntohl_impl(u_int netlong){
    return APIBridge::ntohl(netlong);
}

int redis_isatty_impl(int fd) {
   try {
        int posixFD = RFDMap::getInstance().lookupPosixFD(fd);
        if( posixFD != -1) {
            return APIBridge::isatty(posixFD);
        } else {
            errno = EBADF;
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

int redis_access_impl(const char *pathname, int mode) {
    return APIBridge::access(pathname, mode);
}

u_int64 redis_lseek64_impl(int fd, u_int64 offset, int whence) {
   try {
        int posixFD = RFDMap::getInstance().lookupPosixFD(fd);
        if( posixFD != -1) {
            return APIBridge::lseek64(posixFD, offset, whence);
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
            return APIBridge::get_osfhandle(posixFD);
        } else {
            errno = EBADF;
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
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
        close = redis_close_impl;
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
        _setmode = redis_setmode_impl;
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
        _get_osfhandle = redis_get_osfhandle_impl;
    }

    ~Win32_FDSockMap() {
        CleanupWinsock();
    }

    Win32_FDSockMap(Win32_FDSockMap const&);	  // Don't implement to guarantee singleton semantics
    void operator=(Win32_FDSockMap const&); // Don't implement to guarantee singleton semantics
};

// guarantee global initialization
static class Win32_FDSockMap& init = Win32_FDSockMap::getInstance();


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
#include "win32_types.h"

#define FDAPI_NOCRTREDEFS
#include "Win32_FDAPI.h"
#include "win32_rfdmap.h"
#include <exception>
#include <mswsock.h>
#include <sys/stat.h>
#include "Win32_fdapi_crt.h"
#include "Win32_variadicFunctor.h"
#include "Win32_ANSI.h"
#include "win32_util.h"
#include <string>
#include "Win32_RedisLog.h"
#include "Win32_Common.h"

using namespace std;

#define CATCH_AND_REPORT()  catch(const std::exception &){::redisLog(REDIS_WARNING, "FDAPI: std exception");}catch(...){::redisLog(REDIS_WARNING, "FDAPI: other exception");}

extern "C" {
// FD lookup Winsock equivalents for Win32_wsiocp.c
redis_WSASend WSASend = NULL;
redis_WSARecv WSARecv = NULL;
redis_WSAGetOverlappedResult WSAGetOverlappedResult = NULL;
redis_WSADuplicateSocket WSADuplicateSocket = NULL;
redis_WSASocket WSASocket = NULL;

// other API forwards
redis_fwrite fdapi_fwrite = NULL;
redis_fclose fdapi_fclose = NULL;
redis_fileno fdapi_fileno = NULL;
redis_setmode fdapi_setmode = NULL;
redis_select select = NULL;
redis_ntohl ntohl = NULL;
redis_isatty isatty = NULL;
redis_access access = NULL;
redis_lseek64 lseek64 = NULL;
redis_get_osfhandle fdapi_get_osfhandle = NULL;
redis_open_osfhandle fdapi_open_osfhandle = NULL;

// Unix compatible FD based routines
redis_pipe pipe = NULL;
redis_socket socket = NULL;
redis_close fdapi_close = NULL;
redis_open open = NULL;
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
redis_inet_pton inet_pton = NULL;
}

auto f_WSAGetLastError = dllfunctor_stdcall<int>("ws2_32.dll", "WSAGetLastError");
int FDAPI_WSAGetLastError(void) {
    return f_WSAGetLastError();
}

void FDAPI_SaveSocketAddrStorage(int rfd, SOCKADDR_STORAGE* socketAddrStorage) {
    SocketInfo* socket_info = RFDMap::getInstance().lookupSocketInfo(rfd);
    if (socket_info != NULL) {
        memcpy(&(socket_info->socketAddrStorage), socketAddrStorage, sizeof(SOCKADDR_STORAGE));
    }
}

auto f_ioctlsocket = dllfunctor_stdcall<int, SOCKET, long, u_long*>("ws2_32.dll", "ioctlsocket");

BOOL FDAPI_SocketAttachIOCP(int rfd, HANDLE iocph) {
    SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
    if (socket != INVALID_SOCKET) {
        // Set the socket to nonblocking mode
        DWORD yes = 1;
        if (f_ioctlsocket(socket, FIONBIO, &yes) != SOCKET_ERROR) {
            // Make the socket non-inheritable
            if (SetHandleInformation((HANDLE) socket, HANDLE_FLAG_INHERIT, 0)) {
                // Associate it with the I/O completion port.
                // Use the rfd as the completion key.
                if (CreateIoCompletionPort((HANDLE) socket,
                                           iocph,
                                           (ULONG_PTR) rfd,
                                           0) != NULL) {
                    return TRUE;
                }
            }
        }
        errno = f_WSAGetLastError();
    } else {
        errno = EBADF;
    }
    return FALSE;
}

auto f_WSAIoctl = dllfunctor_stdcall<int, SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPVOID, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE>("ws2_32.dll", "WSAIoctl");
int FDAPI_WSAIoctl(int rfd, DWORD dwIoControlCode, LPVOID lpvInBuffer, DWORD cbInBuffer, LPVOID lpvOutBuffer, DWORD cbOutBuffer, LPDWORD lpcbBytesReturned, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            if (f_WSAIoctl(socket,
                           dwIoControlCode,
                           lpvInBuffer,
                           cbInBuffer,
                           lpvOutBuffer,
                           cbOutBuffer,
                           lpcbBytesReturned,
                           lpOverlapped,
                           lpCompletionRoutine) == 0) {
                return 0;
            } else {
                errno = f_WSAGetLastError();
                return SOCKET_ERROR;
            }
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return SOCKET_ERROR;
}

BOOL FDAPI_AcceptEx(int listenFD, int acceptFD, PVOID lpOutputBuffer, DWORD dwReceiveDataLength, DWORD dwLocalAddressLength, DWORD dwRemoteAddressLength, LPDWORD lpdwBytesReceived, LPOVERLAPPED lpOverlapped) {
    try {
        SOCKET sListen = RFDMap::getInstance().lookupSocket(listenFD);
        SOCKET sAccept = RFDMap::getInstance().lookupSocket(acceptFD);
        if (sListen != INVALID_SOCKET &&  sAccept != INVALID_SOCKET) {
            LPFN_ACCEPTEX acceptex;
            const GUID wsaid_acceptex = WSAID_ACCEPTEX;
            DWORD bytes;

            if (SOCKET_ERROR == f_WSAIoctl(sListen,
                                           SIO_GET_EXTENSION_FUNCTION_POINTER,
                                           (void *) &wsaid_acceptex,
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
    } CATCH_AND_REPORT();

    return FALSE;
}

#ifndef SIO_LOOPBACK_FAST_PATH
const DWORD SIO_LOOPBACK_FAST_PATH = 0x98000010;	// from Win8 SDK
#endif

void EnableFastLoopback(SOCKET socket) {
    // If Win8+ (6.2), use fast path option on loopback
    if (IsWindowsVersionAtLeast(6, 2, 0)) {
        int enabled = 1;
        DWORD result_byte_count = -1;
        int result = f_WSAIoctl(socket, SIO_LOOPBACK_FAST_PATH, &enabled, sizeof(enabled), NULL, 0, &result_byte_count, NULL, NULL);
        if (result != 0) {
            throw std::system_error(f_WSAGetLastError(), system_category(), "WSAIoctl failed");
        }
    }
}

BOOL FDAPI_ConnectEx(int rfd, const struct sockaddr *name, int namelen, PVOID lpSendBuffer, DWORD dwSendDataLength, LPDWORD lpdwBytesSent, LPOVERLAPPED lpOverlapped) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            LPFN_CONNECTEX connectex;
            const GUID wsaid_connectex = WSAID_CONNECTEX;
            DWORD bytes;

            if (SOCKET_ERROR == f_WSAIoctl(socket,
                                           SIO_GET_EXTENSION_FUNCTION_POINTER,
                                           (void *) &wsaid_connectex,
                                           sizeof(GUID),
                                           &connectex,
                                           sizeof(LPFN_ACCEPTEX),
                                           &bytes,
                                           NULL,
                                           NULL)) {
                return FALSE;
            }

            EnableFastLoopback(socket);

            return connectex(socket, 
                             name,namelen,
                             lpSendBuffer,
                             dwSendDataLength,
                             lpdwBytesSent,
                             lpOverlapped);
        }
    } CATCH_AND_REPORT();

    return FALSE;
}

void FDAPI_GetAcceptExSockaddrs(int rfd, PVOID lpOutputBuffer, DWORD dwReceiveDataLength, DWORD dwLocalAddressLength, DWORD dwRemoteAddressLength, LPSOCKADDR *LocalSockaddr, LPINT LocalSockaddrLength, LPSOCKADDR *RemoteSockaddr, LPINT RemoteSockaddrLength) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            LPFN_GETACCEPTEXSOCKADDRS getacceptsockaddrs;
            const GUID wsaid_getacceptsockaddrs = WSAID_GETACCEPTEXSOCKADDRS;
            DWORD bytes;

            if (SOCKET_ERROR == f_WSAIoctl(socket,
                                           SIO_GET_EXTENSION_FUNCTION_POINTER,
                                           (void *) &wsaid_getacceptsockaddrs,
                                           sizeof(GUID),
                                           &getacceptsockaddrs,
                                           sizeof(LPFN_ACCEPTEX),
                                           &bytes,
                                           NULL,
                                           NULL)) {
                return;
            }

            getacceptsockaddrs(lpOutputBuffer, 
                               dwReceiveDataLength,
                               dwLocalAddressLength,
                               dwRemoteAddressLength,
                               LocalSockaddr,
                               LocalSockaddrLength,
                               RemoteSockaddr,
                               RemoteSockaddrLength);
        }
    } CATCH_AND_REPORT();
}

auto f_setsockopt = dllfunctor_stdcall<int, SOCKET, int, int, const char*, int>("ws2_32.dll", "setsockopt");

int FDAPI_UpdateAcceptContext(int rfd) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            return f_setsockopt(socket,
                                SOL_SOCKET,
                                SO_UPDATE_ACCEPT_CONTEXT,
                                (char*) &socket,
                                sizeof(SOCKET));
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return SOCKET_ERROR;
}

void** FDAPI_GetSocketStatePtr(int rfd) {
    SocketInfo* socket_info = RFDMap::getInstance().lookupSocketInfo(rfd);
    if (socket_info == NULL) {
        return NULL;
    } else {
        return &(socket_info->state);
    }
}

void FDAPI_ClearSocketState(int rfd) {
    SocketInfo * socket_info = RFDMap::getInstance().lookupSocketInfo(rfd);
    if (socket_info && socket_info->state) {
        if (socket_info->socket == INVALID_SOCKET) {
            RFDMap::getInstance().removeRFDToSocket(rfd);
        } else {
            socket_info->state = NULL;
        }
    } else {
        redisLog(REDIS_WARNING, "ClearSocketState called on non attached socket.");
    }
}

int FDAPI_PipeSetNonBlock(int rfd, int non_blocking) {
    try {
        int posixFD = RFDMap::getInstance().lookupPosixFD(rfd);
        if (posixFD != -1) {
            HANDLE h = (HANDLE) crt_get_osfhandle(posixFD);
            if (h == INVALID_HANDLE_VALUE) {
                errno = EBADF;
                return -1;
            }

            if (GetFileType(h) == FILE_TYPE_PIPE) {
                /* h is a pipe or socket.  */
                DWORD state;
                if (GetNamedPipeHandleState(h, &state, NULL, NULL, NULL, NULL, 0)) {
                    /* h is a pipe.  */
                    if ((state & PIPE_NOWAIT) != 0) {
                        if (non_blocking)
                            return 0;
                        state &= ~PIPE_NOWAIT;
                    } else {
                        if (!non_blocking)
                            return 0;
                        state |= PIPE_NOWAIT;
                    }
                    if (SetNamedPipeHandleState(h, &state, NULL, NULL)) {
                        return 0;
                    }
                    errno = EINVAL;
                    return -1;
                } else {
                    /* h is a socket.  */
                    errno = EINVAL;
                    return -1;
                }
            } else {
                /* Win32 does not support non-blocking on regular files.  */
                if (!non_blocking) {
                    return 0;
                }
                errno = ENOTSUP;
                return -1;
            }
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

int redis_pipe_impl(int *pfds) {
    int result = -1;
    try {
        // Not passing _O_NOINHERIT, the underlying handles are inheritable by default
        result = crt_pipe(pfds, 8192, _O_BINARY);
        if (result == 0) {
            pfds[0] = RFDMap::getInstance().addPosixFD(pfds[0]);
            pfds[1] = RFDMap::getInstance().addPosixFD(pfds[1]);
        }
    } CATCH_AND_REPORT();

    return result;
}

auto f_socket = dllfunctor_stdcall<SOCKET, int, int, int>("ws2_32.dll", "socket");
int redis_socket_impl(int af, int type, int protocol) {
    try {
        SOCKET socket = f_socket(af, type, protocol);
        if (socket != INVALID_SOCKET) {
            return RFDMap::getInstance().addSocket(socket);
        } else {
            errno = f_WSAGetLastError();
            return -1;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

// In unix a fd is a fd. All are closed with close().
auto f_closesocket = dllfunctor_stdcall<int, SOCKET>("ws2_32.dll", "closesocket");
int redis_close_impl(RFD rfd) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            RFDMap::getInstance().removeSocket(socket);
            return f_closesocket(socket);
        } else {
            int posixFD = RFDMap::getInstance().lookupPosixFD(rfd);
            if (posixFD != -1) {
                RFDMap::getInstance().removePosixFD(posixFD);
                int retval = crt_close(posixFD);
                if (retval == -1) {
                    errno = GetLastError();
                }
                return retval;
            } else {
                errno = EBADF;
                return -1;
            }
        }
    } CATCH_AND_REPORT();

    return -1;
}

int __cdecl redis_open_impl(const char * _Filename, int _OpenFlag, int flags = 0) {
    try {
        int posixFD = crt_open(_Filename, _OpenFlag, flags);
        if (posixFD != -1) {
            return RFDMap::getInstance().addPosixFD(posixFD);
        }
    } CATCH_AND_REPORT();

    errno = GetLastError();
    return -1;
}

auto f_accept = dllfunctor_stdcall<SOCKET, SOCKET, struct sockaddr*, int*>("ws2_32.dll", "accept");
int redis_accept_impl(int rfd, struct sockaddr *addr, socklen_t *addrlen) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            SOCKET sAccept = f_accept(socket, addr, addrlen);
            if (sAccept != INVALID_SOCKET) {
                return RFDMap::getInstance().addSocket(sAccept);
            } else {
                errno = f_WSAGetLastError();
                if ((errno == ENOENT) || (errno == WSAEWOULDBLOCK)) {
                    errno = EAGAIN;
                    return -1;
                }
            }
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

int redis_setsockopt_impl(int rfd, int level, int optname, const void *optval, socklen_t optlen) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            if (f_setsockopt(socket, level, optname, (const char*) optval, optlen) == 0) {
                return 0;
            } else {
                errno = f_WSAGetLastError();
                return -1;
            }
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

int redis_fcntl_impl(int rfd, int cmd, int flags = 0 ) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            switch (cmd) {
                case F_GETFL:
                {
                    // Since there is no way to determine if a socket is blocking in winsock, we keep track of this separately.
                    return RFDMap::getInstance().GetSocketFlags(socket);
                }
                case F_SETFL:
                {
                    u_long fionbio_flags = (flags & O_NONBLOCK);
                    if (f_ioctlsocket(socket, FIONBIO, &fionbio_flags) == SOCKET_ERROR) {
                        errno = f_WSAGetLastError();
                        return -1;
                    } else {
                        RFDMap::getInstance().SetSocketFlags(socket, flags);
                        return 0;
                    }
                    break;
                }
                default:
                {
                    return -1;
                }
            }
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

static auto f_WSAFDIsSet = dllfunctor_stdcall<int, SOCKET, fd_set*>("ws2_32.dll", "__WSAFDIsSet");

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

        if (IsWindowsVersionAtLeast(6, 0, 0)) {
            static auto f_WSAPoll = dllfunctor_stdcall<int, WSAPOLLFD*, ULONG, INT>("ws2_32.dll", "WSAPoll");

            // WSAPoll implementation has a bug that cause the client to wait forever on a non-existant endpoint 
            // See https://github.com/MSOpenTech/redis/issues/214
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
                if (fds[i].fd == INVALID_SOCKET) {
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

                if (f_WSAFDIsSet(pollCopy[i].fd, &readSet)) fds[i].revents |= POLLIN;
                if (f_WSAFDIsSet(pollCopy[i].fd, &writeSet)) fds[i].revents |= POLLOUT;
                if (f_WSAFDIsSet(pollCopy[i].fd, &excepSet)) fds[i].revents |= POLLERR;
            }

            delete pollCopy;
            pollCopy = NULL;

            return ret;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

auto f_getsockopt = dllfunctor_stdcall<int, SOCKET, int, int, char*, int*>("ws2_32.dll", "getsockopt");
int redis_getsockopt_impl(int rfd, int level, int optname, void *optval, socklen_t *optlen) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            return f_getsockopt(socket, level, optname, (char*) optval, optlen);
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}


auto f_connect = dllfunctor_stdcall<int, SOCKET, const struct sockaddr*, int>("ws2_32.dll", "connect");
int redis_connect_impl(int rfd, const struct sockaddr *addr, size_t addrlen) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            EnableFastLoopback(socket);
            int result = f_connect(socket, addr, (int) addrlen);
            errno = f_WSAGetLastError();
            if ((errno == WSAEINVAL) || (errno == WSAEWOULDBLOCK) || (errno == WSA_IO_PENDING)) {
                errno = EINPROGRESS;
            }
            return result;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

auto f_recv = dllfunctor_stdcall<int, SOCKET, char*, int, int>("ws2_32.dll", "recv");
ssize_t redis_read_impl(int rfd, void *buf, size_t count) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            int retval = f_recv(socket, (char*) buf, (unsigned int) count, 0);
            if (retval == -1) {
                errno = GetLastError();
                if (errno == WSAEWOULDBLOCK) {
                    errno = EAGAIN;
                }
            }
            return retval;
        } else {
            int posixFD = RFDMap::getInstance().lookupPosixFD(rfd);
            if (posixFD != -1) {
                int retval = crt_read(posixFD, buf, (unsigned int) count);
                if (retval == -1) {
                    errno = GetLastError();
                }
                return retval;
            } else {
                errno = EBADF;
                return 0;
            }
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

auto f_send = dllfunctor_stdcall<int, SOCKET, const char*, int, int>("ws2_32.dll", "send");
ssize_t redis_write_impl(int rfd, const void *buf, size_t count) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            int ret = f_send(socket, (char*) buf, (unsigned int) count, 0);
            if (ret == SOCKET_ERROR) {
                set_errno_from_last_error();
            }
            return ret;
        } else {
            int posixFD = RFDMap::getInstance().lookupPosixFD(rfd);
            if (posixFD != -1) {
                if (posixFD == _fileno(stdout)) {
                    DWORD bytesWritten = 0;
                    if (FALSE != ParseAndPrintANSIString(GetStdHandle(STD_OUTPUT_HANDLE), buf, (DWORD) count, &bytesWritten)) {
                        return (int) bytesWritten;
                    } else {
                        errno = GetLastError();
                        return 0;
                    }
                } else if (posixFD == _fileno(stderr)) {
                    DWORD bytesWritten = 0;
                    if (FALSE != ParseAndPrintANSIString(GetStdHandle(STD_ERROR_HANDLE), buf, (DWORD) count, &bytesWritten)) {
                        return (int) bytesWritten;
                    } else {
                        errno = GetLastError();
                        return 0;
                    }
                } else {
                    int retval = crt_write(posixFD, buf, (unsigned int) count);
                    if (retval == -1) {
                        errno = GetLastError();
                    }
                    return retval;
                }
            } else {
                errno = EBADF;
                return 0;
            }
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

int redis_fsync_impl(int rfd) {
    try {
        int posixFD = RFDMap::getInstance().lookupPosixFD(rfd);
        if (posixFD != -1) {
            HANDLE h = (HANDLE) crt_get_osfhandle(posixFD);
            if (h == INVALID_HANDLE_VALUE) {
                errno = EBADF;
                return -1;
            }

            if (!FlushFileBuffers(h)) {
                DWORD err = GetLastError();
                switch (err) {
                    case ERROR_INVALID_HANDLE:
                        errno = EINVAL;
                        break;

                    default:
                        errno = EIO;
                }
                return -1;
            }
        }
        return 0;
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

int redis_fstat_impl(int rfd, struct __stat64 *buffer) {
    try {
        int posixFD = RFDMap::getInstance().lookupPosixFD(rfd);
        if (posixFD != -1) {
            return _fstat64(posixFD, buffer);
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

auto f_listen = dllfunctor_stdcall<int, SOCKET, int>("ws2_32.dll", "listen");
int redis_listen_impl(int rfd, int backlog) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            EnableFastLoopback(socket);
            return f_listen(socket, backlog);
        } else {
            errno = EBADF;
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

int redis_ftruncate_impl(int rfd, PORT_LONGLONG length) {
    try {
        int posixFD = RFDMap::getInstance().lookupPosixFD(rfd);
        if (posixFD != -1) {
            HANDLE h = (HANDLE) crt_get_osfhandle(posixFD);

            if (h == INVALID_HANDLE_VALUE) {
                errno = EBADF;
                return -1;
            }

            LARGE_INTEGER l, o;
            l.QuadPart = length;

            if (!SetFilePointerEx(h, l, &o, FILE_BEGIN)) return -1;
            if (!SetEndOfFile(h)) return -1;

            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

auto f_bind = dllfunctor_stdcall<int, SOCKET, const struct sockaddr*, int>("ws2_32.dll", "bind");
int redis_bind_impl(int rfd, const struct sockaddr *addr, socklen_t addrlen) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            return f_bind(socket, addr, addrlen);
        } else {
            errno = EBADF;
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

auto f_shutdown = dllfunctor_stdcall<int, SOCKET, int>("ws2_32.dll", "shutdown");
int redis_shutdown_impl(int rfd, int how) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            return f_shutdown(socket, how);
        } else {
            errno = EBADF;
            return 0;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

auto f_WSAGetOverlappedResult = dllfunctor_stdcall<BOOL, SOCKET, LPWSAOVERLAPPED, LPDWORD, BOOL, LPDWORD>("ws2_32.dll", "WSAGetOverlappedResult");
BOOL redis_WSAGetOverlappedResult_impl(int rfd, LPWSAOVERLAPPED lpOverlapped, LPDWORD lpcbTransfer, BOOL fWait, LPDWORD lpdwFlags) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            return f_WSAGetOverlappedResult(socket, lpOverlapped, lpcbTransfer, fWait, lpdwFlags);
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return SOCKET_ERROR;
}

auto f_WSADuplicateSocket = dllfunctor_stdcall<int, SOCKET, DWORD, LPWSAPROTOCOL_INFO>("ws2_32.dll", "WSADuplicateSocketW");
int redis_WSADuplicateSocket_impl(int rfd, DWORD dwProcessId, LPWSAPROTOCOL_INFO lpProtocolInfo) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            return f_WSADuplicateSocket(socket, dwProcessId, lpProtocolInfo);
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return SOCKET_ERROR;
}

auto f_WSASocket = dllfunctor_stdcall<SOCKET, int, int, int, LPWSAPROTOCOL_INFO, GROUP, DWORD>("ws2_32.dll", "WSASocketW");
int redis_WSASocket_impl(int af, int type, int protocol, LPWSAPROTOCOL_INFO lpProtocolInfo, GROUP g, DWORD dwFlags) {
    try {
        SOCKET socket = f_WSASocket(af,
                                    type,
                                    protocol,
                                    lpProtocolInfo,
                                    g,
                                    dwFlags);

        if (socket != INVALID_SOCKET) {
            return RFDMap::getInstance().addSocket(socket);
        }
    } CATCH_AND_REPORT();

    return -1;
}

auto f_WSASend = dllfunctor_stdcall<int, SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE>("ws2_32.dll", "WSASend");
int redis_WSASend_impl(int rfd, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent, DWORD dwFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            return f_WSASend(socket, 
                             lpBuffers,
                             dwBufferCount,
                             lpNumberOfBytesSent,
                             dwFlags,
                             lpOverlapped,
                             lpCompletionRoutine);
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return SOCKET_ERROR;
}

auto f_WSARecv = dllfunctor_stdcall<int, SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE>("ws2_32.dll", "WSARecv");
int redis_WSARecv_impl(int rfd, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            return f_WSARecv(socket, 
                             lpBuffers,
                             dwBufferCount,
                             lpNumberOfBytesRecvd,
                             lpFlags,
                             lpOverlapped,
                             lpCompletionRoutine);
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
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
int redis_getpeername_impl(int rfd, struct sockaddr *addr, socklen_t * addrlen) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            int result = f_getpeername(socket, addr, addrlen);
            // Workaround for getpeername failing to retrieve the endpoint address
            if (result != 0) {
                SocketInfo* socket_info = RFDMap::getInstance().lookupSocketInfo(rfd);
                if (socket_info != NULL) {
                    memcpy(addr, &(socket_info->socketAddrStorage), sizeof(SOCKADDR_STORAGE));
                    *addrlen = sizeof(SOCKADDR_STORAGE);
                    return 0;
                }
            }
            return result;
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return SOCKET_ERROR;
}

auto f_getsockname = dllfunctor_stdcall<int, SOCKET, struct sockaddr*, int*>("ws2_32.dll", "getsockname");
int redis_getsockname_impl(int rfd, struct sockaddr* addrsock, int* addrlen) {
    try {
        SOCKET socket = RFDMap::getInstance().lookupSocket(rfd);
        if (socket != INVALID_SOCKET) {
            return f_getsockname(socket, addrsock, addrlen);
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return SOCKET_ERROR;
}

auto f_ntohs = dllfunctor_stdcall<u_short,u_short>("ws2_32.dll", "ntohs");
u_short redis_ntohs_impl(u_short netshort) {
    return f_ntohs( netshort );
}

int redis_setmode_impl(int fd,int mode) {
    return crt_setmode(fd, mode);
}

size_t redis_fwrite_impl(const void * _Str, size_t _Size, size_t _Count, FILE * _File) {
    return crt_fwrite(_Str, _Size, _Count, _File);
}

int redis_fclose_impl(FILE * file) {
    int posixFD = crt_fileno(file);
    if (posixFD != -1) {
        RFDMap::getInstance().removePosixFD(posixFD);
    }
    return crt_fclose(file);
}

int redis_fileno_impl(FILE* file) {
    int posixFD = crt_fileno(file);
    if (posixFD != -1) {
        // If posixFD is already mapped, addPosixFD() will return the existing rfd.
        return RFDMap::getInstance().addPosixFD(posixFD);
    }
    return -1;
}

auto f_select = dllfunctor_stdcall<int, int, fd_set*, fd_set*, fd_set*, const struct timeval*>("ws2_32.dll", "select");
int redis_select_impl(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    try {
        if (readfds != NULL) {
            for (u_int r = 0; r < readfds->fd_count; r++) {
                readfds->fd_array[r] = RFDMap::getInstance().lookupSocket((RFD) readfds->fd_array[r]);
            }
        }
        if (writefds != NULL) {
            for (u_int r = 0; r < writefds->fd_count; r++) {
                writefds->fd_array[r] = RFDMap::getInstance().lookupSocket((RFD) writefds->fd_array[r]);
            }
        }
        if (exceptfds != NULL) {
            for (u_int r = 0; r < exceptfds->fd_count; r++) {
                exceptfds->fd_array[r] = RFDMap::getInstance().lookupSocket((RFD) exceptfds->fd_array[r]);
            }
        }

        return f_select(nfds, readfds, writefds, exceptfds, timeout);
    } CATCH_AND_REPORT();

    errno = EBADF;
    return SOCKET_ERROR;
}

auto f_ntohl = dllfunctor_stdcall<u_long, u_long>("ws2_32.dll", "ntohl");
u_int redis_ntohl_impl(u_int netlong){
    return f_ntohl(netlong);
}

int redis_isatty_impl(int rfd) {
    try {
        int posixFD = RFDMap::getInstance().lookupPosixFD(rfd);
        if (posixFD != -1) {
            return crt_isatty(posixFD);
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

int redis_access_impl(const char *pathname, int mode) {
    return crt_access(pathname, mode);
}

u_int64 redis_lseek64_impl(int rfd, u_int64 offset, int whence) {
    try {
        int posixFD = RFDMap::getInstance().lookupPosixFD(rfd);
        if (posixFD != -1) {
            return crt_lseek64(posixFD, offset, whence);
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

intptr_t redis_get_osfhandle_impl(RFD rfd) {
    try {
        int posixFD = RFDMap::getInstance().lookupPosixFD(rfd);
        if (posixFD != -1) {
            return crt_get_osfhandle(posixFD);
        }
    } CATCH_AND_REPORT();

    errno = EBADF;
    return -1;
}

int redis_open_osfhandle_impl(intptr_t osfhandle, int flags) {
    try {
        int posixFD = crt_open_osfhandle(osfhandle, flags);
        if (posixFD != -1) {
            return RFDMap::getInstance().addPosixFD(posixFD);
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
    if (IsWindowsVersionAtLeast(6, 0, 0)) {
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

int redis_inet_pton_impl(int family, const char* src, void* dst) {
    if (IsWindowsVersionAtLeast(6, 0, 0)) {
        static auto f_inet_pton = dllfunctor_stdcall<int, int, const char*, const void*>("ws2_32.dll", "inet_pton");
        return f_inet_pton(family, src, dst);
    } else {
        static auto f_WSAStringToAddressA = dllfunctor_stdcall<int, LPSTR, INT, LPWSAPROTOCOL_INFO, LPSOCKADDR, LPINT>("ws2_32.dll", "WSAStringToAddressA");
        struct sockaddr ss;
        int size = sizeof(ss);
        ZeroMemory(&ss, sizeof(ss));

        char src_copy[INET6_ADDRSTRLEN + 1];
        strncpy(src_copy, src, INET6_ADDRSTRLEN + 1);
        src_copy[INET6_ADDRSTRLEN] = 0;
        /* Non-Const API*/
        if (f_WSAStringToAddressA(src_copy, family, NULL, (struct sockaddr *)&ss, &size) == 0) {
            switch (family) {
                case AF_INET:
                    *(struct in_addr *)dst = ((struct sockaddr_in *)&ss)->sin_addr;
                    return 1;
                case AF_INET6:
                    *(struct in6_addr *)dst = ((struct sockaddr_in6 *)&ss)->sin6_addr;
                    return 1;
            }
        }
        return 0;
    }
}

BOOL ParseStorageAddress(const char *ip, int port, SOCKADDR_STORAGE* pStorageAddr) {
    struct addrinfo hints, *res;
    int status;
    char port_buffer[6];

    sprintf(port_buffer, "%hu", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    // Setting AI_PASSIVE will give you a wildcard address if addr is NULL
    hints.ai_flags = AI_NUMERICSERV | AI_PASSIVE;

    if ((status = getaddrinfo(ip, port_buffer, &hints, &res)) != 0) {
        return FALSE;
    }

    // Note, we're taking the first valid address, there may be more than one
    memcpy(pStorageAddr, res->ai_addr, res->ai_addrlen);

    freeaddrinfo(res);
    return TRUE;
}

auto f_WSAStartup = dllfunctor_stdcall<int, WORD, LPWSADATA>("ws2_32.dll", "WSAStartup");
int InitWinsock() {
    WSADATA t_wsa;
    WORD wVers;
    int iError;

    wVers = MAKEWORD(2, 2);
    iError = f_WSAStartup(wVers, &t_wsa);

    if (iError != NO_ERROR || LOBYTE(t_wsa.wVersion) != 2 || HIBYTE(t_wsa.wVersion) != 2) {
        exit(1);
    } else {
        return 0;
    }
}

auto f_WSACleanup = dllfunctor_stdcall<int>("ws2_32.dll", "WSACleanup");
int CleanupWinsock() {
    return f_WSACleanup();
}

class Win32_FDSockMap {
public:
    static Win32_FDSockMap& getInstance() {
        static Win32_FDSockMap instance; // Instantiated on first use. Guaranteed to be destroyed.
        return instance;
    }

private:
    Win32_FDSockMap() {
        InitWinsock();

        pipe = redis_pipe_impl;
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
        inet_addr = redis_inet_addr_impl;
        gethostbyname = redis_gethostbyname_impl;
        inet_ntoa = redis_inet_ntoa_impl;
        inet_pton = redis_inet_pton_impl;
        fdapi_fwrite = redis_fwrite_impl;
        fdapi_fclose = redis_fclose_impl;
        fdapi_fileno = redis_fileno_impl;
        fdapi_setmode = redis_setmode_impl;
        WSASend = redis_WSASend_impl;
        WSARecv = redis_WSARecv_impl;
        WSAGetOverlappedResult = redis_WSAGetOverlappedResult_impl;
        WSADuplicateSocket = redis_WSADuplicateSocket_impl;
        WSASocket = redis_WSASocket_impl;
        select = redis_select_impl;
        ntohl = redis_ntohl_impl;
        isatty = redis_isatty_impl;
        access = redis_access_impl;
        lseek64 = redis_lseek64_impl;
        fdapi_get_osfhandle = redis_get_osfhandle_impl;
        fdapi_open_osfhandle = redis_open_osfhandle_impl;
        freeaddrinfo = redis_freeaddrinfo_impl;
        getaddrinfo = redis_getaddrinfo_impl;
        inet_ntop = redis_inet_ntop_impl;
        accept = redis_accept_impl;
    }

    ~Win32_FDSockMap() {
        CleanupWinsock();
    }

    Win32_FDSockMap(Win32_FDSockMap const&);    // Don't implement to guarantee singleton semantics
    void operator=(Win32_FDSockMap const&);     // Don't implement to guarantee singleton semantics
};

// guarantee global initialization
static class Win32_FDSockMap& init = Win32_FDSockMap::getInstance();


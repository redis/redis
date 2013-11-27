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

#pragma comment (lib, "ws2_32.lib")
#define FD_SETSIZE 64000
#include <WinSock2.h>
#include "APIBridge.h"
#include <io.h>
#include <stdlib.h>

int APIBridge::WSAStartup(WORD wVersionRequired, LPWSADATA lpWSAData) {
    return ::WSAStartup( wVersionRequired, lpWSAData );
}

int APIBridge::WSACleanup(void) {
    return ::WSACleanup();
}

int APIBridge::WSAGetLastError(void) {
   return ::WSAGetLastError();
}

void APIBridge::WSASetLastError(int iError) {
    ::WSASetLastError(iError);
}

BOOL APIBridge::WSAGetOverlappedResult(SOCKET s, LPWSAOVERLAPPED lpOverlapped, LPDWORD lpcbTransfer,BOOL fWait,LPDWORD lpdwFlags) {
    return ::WSAGetOverlappedResult(s,lpOverlapped,lpcbTransfer,fWait,lpdwFlags);
}

int APIBridge::WSAIoctl(SOCKET s,DWORD dwIoControlCode,LPVOID lpvInBuffer,DWORD cbInBuffer,LPVOID lpvOutBuffer,DWORD cbOutBuffer,LPDWORD lpcbBytesReturned,LPWSAOVERLAPPED lpOverlapped,LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
    return ::WSAIoctl(s,dwIoControlCode,lpvInBuffer,cbInBuffer,lpvOutBuffer,cbOutBuffer,lpcbBytesReturned,lpOverlapped,lpCompletionRoutine);
}

int APIBridge::WSASend(SOCKET s,LPWSABUF lpBuffers,DWORD dwBufferCount,LPDWORD lpNumberOfBytesSent,DWORD dwFlags,LPWSAOVERLAPPED lpOverlapped,LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
    return ::WSASend(s,lpBuffers,dwBufferCount,lpNumberOfBytesSent,dwFlags,lpOverlapped,lpCompletionRoutine);
}

int APIBridge::WSARecv(SOCKET s,LPWSABUF lpBuffers,DWORD dwBufferCount,LPDWORD lpNumberOfBytesRecvd,LPDWORD lpFlags,LPWSAOVERLAPPED lpOverlapped,LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
    return ::WSARecv(s,lpBuffers,dwBufferCount,lpNumberOfBytesRecvd,lpFlags,lpOverlapped,lpCompletionRoutine);
}

int APIBridge::WSAPoll(WSAPOLLFD fdarray[],ULONG nfds,INT timeout){
    return ::WSAPoll(fdarray,nfds,timeout);
}

SOCKET APIBridge::socket(int af,int type,int protocol) {
    return ::socket(af,type,protocol);
}

int APIBridge::closesocket(SOCKET s) {
    return ::closesocket(s);
}

int APIBridge::close(int fd) {
    int retval = _close(fd);
    if( retval == -1 )
        SetLastError(errno);
    return retval;
}

#pragma warning(disable:4996)
int APIBridge::open(const char *filename, int oflag, int pmode) {
    int retval = ::open(filename, oflag, pmode);
    if( retval == -1 )
        SetLastError(errno);
    return retval;
}

SOCKET APIBridge::accept(SOCKET s,struct sockaddr *addr,int *addrlen){
    return ::accept(s,addr,addrlen);
}

int APIBridge::setsockopt(SOCKET s,int level,int optname,const char *optval,int optlen) {
    return ::setsockopt(s,level,optname,optval,optlen);
}

int APIBridge::getsockopt(SOCKET s,int level,int optname,char *optval,int *optlen) {
    return ::getsockopt(s,level,optname,optval,optlen);
}

int APIBridge::connect(SOCKET s,const struct sockaddr *name,int namelen) {
    return ::connect(s,name,namelen);
}

int APIBridge::recv(SOCKET s,char *buf,int len,int flags) {
    return ::recv(s,buf,len,flags);
}

int APIBridge::read(int fd,void *buffer,unsigned int count) {
    return ::read(fd,buffer,count);
}

int APIBridge::send(SOCKET s,const char *buf,int len,int flags) {
    return ::send(s,buf,len,flags);
}

int APIBridge::write(int fd,const void *buffer,unsigned int count){
    int retval = ::write(fd,buffer,count);
    if( retval == -1 )
        SetLastError(errno);
    return retval;
}

intptr_t APIBridge::_get_osfhandle(int fd){
    intptr_t retval = ::_get_osfhandle(fd);
    if( retval == -1 )
        SetLastError(errno);
    return retval;
}

int APIBridge::listen(SOCKET s,int backlog) {
    return ::listen(s,backlog);
}

int APIBridge::bind(SOCKET s,const struct sockaddr *name,int namelen) {
    return ::bind(s,name,namelen);
}

int APIBridge::shutdown(SOCKET s,int how) {
    return ::shutdown(s,how);
}

int APIBridge::ioctlsocket(SOCKET s,long cmd,u_long *argp) {
    return ::ioctlsocket(s,cmd,argp);
}

unsigned long APIBridge::inet_addr(const char *cp) {
    return ::inet_addr(cp);
}

struct hostent* APIBridge::gethostbyname(const char *name) {
    return ::gethostbyname(name);
}

char* APIBridge::inet_ntoa(struct in_addr in) {
    return ::inet_ntoa(in);
}

u_short APIBridge::htons(u_short hostshort) {
    return ::htons(hostshort);
}

u_long APIBridge::htonl(u_long hostlong) {
    return ::htonl(hostlong);
}

int APIBridge::getpeername(SOCKET s,struct sockaddr *name,int *namelen) {
    return ::getpeername(s,name,namelen);
}

int APIBridge::getsockname(SOCKET s,struct sockaddr *name,int *namelen) {
    return ::getsockname(s,name,namelen);
}

u_short APIBridge::ntohs(u_short netshort) {
    return ::ntohs(netshort);
}

int APIBridge::_setmode(int fd,int mode) {
    return ::_setmode(fd,mode);
}

int APIBridge::select(int nfds, fd_set *readfds, fd_set *writefds,fd_set *exceptfds, struct timeval *timeout) {
    return ::select(nfds, readfds, writefds,exceptfds, timeout);
}

u_int APIBridge::ntohl(u_int netlong) {
    return ::ntohl(netlong);
}

int APIBridge::isatty(int fd) {
    int retval = ::isatty(fd);
    if( retval == -1 )
        SetLastError(errno);
    return retval;
}

int APIBridge::access(const char *pathname, int mode) {
    return ::access(pathname, mode);
}

u_int64 APIBridge::lseek64(int fd, u_int64 offset, int whence) {
    int retval = ::lseek(fd, (long)offset, whence);
    if( retval == -1 )
        SetLastError(errno);
    return retval;
}
 
intptr_t APIBridge::get_osfhandle(int fd) {
    intptr_t retval = ::_get_osfhandle(fd);
    if( retval == -1 )
        SetLastError(errno);
    return retval;
}









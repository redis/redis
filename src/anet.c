/* anet.c -- Basic TCP socket stuff made a bit less boring
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#include "anet.h"

char* global_ssl_cert_password = NULL;

int password_callback(char* buffer, int num, int rwflag, void* userdata) {
    if ( NULL == global_ssl_cert_password || num < (int)(strlen(global_ssl_cert_password) + 1)) {
      return(0);
    }
    strcpy(buffer, global_ssl_cert_password);
    return strlen(global_ssl_cert_password);
}

int verify_callback(int ok, X509_STORE_CTX* store) {
  char data[255];

  if (!ok) {
    X509* cert = X509_STORE_CTX_get_current_cert(store);
    int depth = X509_STORE_CTX_get_error_depth(store);
    int err = X509_STORE_CTX_get_error(store);

    printf("Error with certificate at depth: %d!\n", depth);
    X509_NAME_oneline(X509_get_issuer_name(cert), data, 255);
    printf("\tIssuer: %s\n", data);
    X509_NAME_oneline(X509_get_subject_name(cert), data, 255);
    printf("\tSubject: %s\n", data);
    printf("\tError %d: %s\n", err, X509_verify_cert_error_string(err));
  }

  return ok;
} 

static void anetSetError(char *err, const char *fmt, ...)
{
    va_list ap;

    if (!err) return;
    va_start(ap, fmt);
    vsnprintf(err, ANET_ERR_LEN, fmt, ap);
    va_end(ap);
}

int anetSetBlock(char *err, int fd, int non_block) {
    int flags;

    /* Set the socket blocking (if non_block is zero) or non-blocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */

    if ((flags = fcntl(fd, F_GETFL)) == -1) {
        anetSetError(err, "fcntl(F_GETFL): %s", strerror(errno));
        return ANET_ERR;
    }

    if (non_block)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;

    if (fcntl(fd, F_SETFL, flags) == -1) {
        anetSetError(err, "fcntl(F_SETFL,O_NONBLOCK): %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

int anetNonBlock(char *err, int fd) {
    return anetSetBlock(err,fd,1);
}

int anetBlock(char *err, int fd) {
    return anetSetBlock(err,fd,0);
}

/* Set TCP keep alive option to detect dead peers. The interval option
 * is only used for Linux as we are using Linux-specific APIs to set
 * the probe send time, interval, and count. */
int anetKeepAlive(char *err, int fd, int interval)
{
    int val = 1;

    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1)
    {
        anetSetError(err, "setsockopt SO_KEEPALIVE: %s", strerror(errno));
        return ANET_ERR;
    }

#ifdef __linux__
    /* Default settings are more or less garbage, with the keepalive time
     * set to 7200 by default on Linux. Modify settings to make the feature
     * actually useful. */

    /* Send first probe after interval. */
    val = interval;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) {
        anetSetError(err, "setsockopt TCP_KEEPIDLE: %s\n", strerror(errno));
        return ANET_ERR;
    }

    /* Send next probes after the specified interval. Note that we set the
     * delay as interval / 3, as we send three probes before detecting
     * an error (see the next setsockopt call). */
    val = interval/3;
    if (val == 0) val = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) {
        anetSetError(err, "setsockopt TCP_KEEPINTVL: %s\n", strerror(errno));
        return ANET_ERR;
    }

    /* Consider the socket in error state after three we send three ACK
     * probes without getting a reply. */
    val = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) {
        anetSetError(err, "setsockopt TCP_KEEPCNT: %s\n", strerror(errno));
        return ANET_ERR;
    }
#else
    ((void) interval); /* Avoid unused var warning for non Linux systems. */
#endif

    return ANET_OK;
}

static int anetSetTcpNoDelay(char *err, int fd, int val)
{
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) == -1)
    {
        anetSetError(err, "setsockopt TCP_NODELAY: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

int anetEnableTcpNoDelay(char *err, int fd)
{
    return anetSetTcpNoDelay(err, fd, 1);
}

int anetDisableTcpNoDelay(char *err, int fd)
{
    return anetSetTcpNoDelay(err, fd, 0);
}


int anetSetSendBuffer(char *err, int fd, int buffsize)
{
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffsize, sizeof(buffsize)) == -1)
    {
        anetSetError(err, "setsockopt SO_SNDBUF: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

int anetTcpKeepAlive(char *err, int fd)
{
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt SO_KEEPALIVE: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

/* Set the socket send timeout (SO_SNDTIMEO socket option) to the specified
 * number of milliseconds, or disable it if the 'ms' argument is zero. */
int anetSendTimeout(char *err, int fd, long long ms) {
    struct timeval tv;

    tv.tv_sec = ms/1000;
    tv.tv_usec = (ms%1000)*1000;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1) {
        anetSetError(err, "setsockopt SO_SNDTIMEO: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

/* anetGenericResolve() is called by anetResolve() and anetResolveIP() to
 * do the actual work. It resolves the hostname "host" and set the string
 * representation of the IP address into the buffer pointed by "ipbuf".
 *
 * If flags is set to ANET_IP_ONLY the function only resolves hostnames
 * that are actually already IPv4 or IPv6 addresses. This turns the function
 * into a validating / normalizing function. */
int anetGenericResolve(char *err, char *host, char *ipbuf, size_t ipbuf_len,
                       int flags)
{
    struct addrinfo hints, *info;
    int rv;

    memset(&hints,0,sizeof(hints));
    if (flags & ANET_IP_ONLY) hints.ai_flags = AI_NUMERICHOST;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;  /* specify socktype to avoid dups */

    if ((rv = getaddrinfo(host, NULL, &hints, &info)) != 0) {
        anetSetError(err, "%s", gai_strerror(rv));
        return ANET_ERR;
    }
    if (info->ai_family == AF_INET) {
        struct sockaddr_in *sa = (struct sockaddr_in *)info->ai_addr;
        inet_ntop(AF_INET, &(sa->sin_addr), ipbuf, ipbuf_len);
    } else {
        struct sockaddr_in6 *sa = (struct sockaddr_in6 *)info->ai_addr;
        inet_ntop(AF_INET6, &(sa->sin6_addr), ipbuf, ipbuf_len);
    }

    freeaddrinfo(info);
    return ANET_OK;
}

int anetResolve(char *err, char *host, char *ipbuf, size_t ipbuf_len) {
    return anetGenericResolve(err,host,ipbuf,ipbuf_len,ANET_NONE);
}

int anetResolveIP(char *err, char *host, char *ipbuf, size_t ipbuf_len) {
    return anetGenericResolve(err,host,ipbuf,ipbuf_len,ANET_IP_ONLY);
}

static int anetSetReuseAddr(char *err, int fd) {
    int yes = 1;
    /* Make sure connection-intensive things like the redis benckmark
     * will be able to close/open sockets a zillion of times */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt SO_REUSEADDR: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

static int anetCreateSocket(char *err, int domain) {
    int s;
    if ((s = socket(domain, SOCK_STREAM, 0)) == -1) {
        anetSetError(err, "creating socket: %s", strerror(errno));
        return ANET_ERR;
    }

    /* Make sure connection-intensive things like the redis benchmark
     * will be able to close/open sockets a zillion of times */
    if (anetSetReuseAddr(err,s) == ANET_ERR) {
        close(s);
        return ANET_ERR;
    }
    return s;
}

static int anetTcpGenericConnect(char *err, char *addr, int port,
                                 char *source_addr, int flags)
{
    int s = ANET_ERR, rv;
    char portstr[6];  /* strlen("65535") + 1; */
    struct addrinfo hints, *servinfo, *bservinfo, *p, *b;

    snprintf(portstr,sizeof(portstr),"%d",port);
    memset(&hints,0,sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(addr,portstr,&hints,&servinfo)) != 0) {
        anetSetError(err, "%s", gai_strerror(rv));
        return ANET_ERR;
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
        /* Try to create the socket and to connect it.
         * If we fail in the socket() call, or on connect(), we retry with
         * the next entry in servinfo. */
        if ((s = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) == -1)
            continue;
        if (anetSetReuseAddr(err,s) == ANET_ERR) goto error;
        if (flags & ANET_CONNECT_NONBLOCK && anetNonBlock(err,s) != ANET_OK)
            goto error;
        if (source_addr) {
            int bound = 0;
            /* Using getaddrinfo saves us from self-determining IPv4 vs IPv6 */
            if ((rv = getaddrinfo(source_addr, NULL, &hints, &bservinfo)) != 0)
            {
                anetSetError(err, "%s", gai_strerror(rv));
                goto end;
            }
            for (b = bservinfo; b != NULL; b = b->ai_next) {
                if (bind(s,b->ai_addr,b->ai_addrlen) != -1) {
                    bound = 1;
                    break;
                }
            }
            freeaddrinfo(bservinfo);
            if (!bound) {
                anetSetError(err, "bind: %s", strerror(errno));
                goto end;
            }
        }
        if (connect(s,p->ai_addr,p->ai_addrlen) == -1) {
            /* If the socket is non-blocking, it is ok for connect() to
             * return an EINPROGRESS error here. */
            if (errno == EINPROGRESS && flags & ANET_CONNECT_NONBLOCK)
                goto end;
            close(s);
            s = ANET_ERR;
            continue;
        }

        /* If we ended an iteration of the for loop without errors, we
         * have a connected socket. Let's return to the caller. */
        goto end;
    }
    if (p == NULL)
        anetSetError(err, "creating socket: %s", strerror(errno));

error:
    if (s != ANET_ERR) {
        close(s);
        s = ANET_ERR;
    }
end:
    freeaddrinfo(servinfo);
    return s;
}

int anetSSLGenericConnect( char* err, char* addr, int port, int flags, anetSSLConnection* sslctn, char* certFilePath, char* certDirPath, char* checkCommonName ) {
  sslctn->sd = -1;
  sslctn->ctx = NULL;
  sslctn->ssl = NULL;
  sslctn->bio = NULL;
  sslctn->conn_str = NULL;

  // Set up a SSL_CTX object, which will tell our BIO object how to do its work
  SSL_CTX* ctx = SSL_CTX_new(TLSv1_1_client_method());
  sslctn->ctx = ctx;

  // Create a SSL object pointer, which our BIO object will provide.
  SSL* ssl;

  // Create our BIO object for SSL connections.
  BIO* bio = BIO_new_ssl_connect(ctx);
  sslctn->bio = bio;

  // Failure?
  if (bio == NULL) {
     char errorbuf[1024];

     ERR_error_string(1024,errorbuf);
     anetSetError(err, "SSL Error: Error creating BIO: %s\n", errorbuf);

     // We need to free up the SSL_CTX before we leave.
     anetCleanupSSL( sslctn );
     return ANET_ERR;
  }

  // Makes ssl point to bio's SSL object.
  BIO_get_ssl(bio, &ssl);
  sslctn->ssl = ssl;

  // Set the SSL to automatically retry on failure.
  SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);

  char* connect_str = (char *)calloc( 1, strlen( addr ) + 10 );
  sprintf( connect_str, "%s:%d", addr, port );
  sslctn->conn_str = connect_str;

  // We're connection to to the redis server at IP:port.
  BIO_set_conn_hostname(bio, connect_str);

  SSL_CTX_load_verify_locations(ctx, certFilePath, certDirPath);

  // Same as before, try to connect.
  if (BIO_do_connect(bio) <= 0) {
    char errorbuf[1024];
    ERR_error_string(1024,errorbuf);
    anetSetError(err, "SSL Error: Failed to connect: %s\n", errorbuf);
    anetCleanupSSL( sslctn );
    return ANET_ERR;
  }

  // Now we need to do the SSL handshake, so we can communicate.
  if (BIO_do_handshake(bio) <= 0) {
    char errorbuf[1024];
    ERR_error_string(1024,errorbuf);
    anetSetError(err, "SSL Error: handshake failure: %s\n", errorbuf);
    anetCleanupSSL( sslctn );
    return ANET_ERR;
  }

  long verify_result = SSL_get_verify_result(ssl);
  if( verify_result == X509_V_OK) {
    X509* peerCertificate = SSL_get_peer_certificate(ssl);

    char commonName [512];
    X509_NAME * name = X509_get_subject_name(peerCertificate);
    X509_NAME_get_text_by_NID(name, NID_commonName, commonName, 512);
    if( checkCommonName != NULL && strlen( checkCommonName ) > 0 ) {
      if(wildcmp(commonName, checkCommonName, strlen(checkCommonName)) == 0) {
        anetSetError(err, "SSL Error: Error validating peer common name: %s\n", commonName);
        anetCleanupSSL( sslctn );
        return ANET_ERR;
      }
    }
  } else {
     char errorbuf[1024];
     ERR_error_string(1024,errorbuf);
     anetSetError(err, "SSL Error: Error retrieving peer certificate: %s\n", errorbuf);
     anetCleanupSSL( sslctn );
     return ANET_ERR;
  }

  return BIO_get_fd( bio, NULL );
}

int anetTcpConnect(char *err, char *addr, int port)
{
    return anetTcpGenericConnect(err,addr,port,NULL,ANET_CONNECT_NONE);
}

int anetTcpNonBlockConnect(char *err, char *addr, int port)
{
    return anetTcpGenericConnect(err,addr,port,NULL,ANET_CONNECT_NONBLOCK);
}

int anetTcpNonBlockBindConnect(char *err, char *addr, int port, char *source_addr)
{
    return anetTcpGenericConnect(err,addr,port,source_addr,ANET_CONNECT_NONBLOCK);
}

int anetUnixGenericConnect(char *err, char *path, int flags)
{
    int s;
    struct sockaddr_un sa;

    if ((s = anetCreateSocket(err,AF_LOCAL)) == ANET_ERR)
        return ANET_ERR;

    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path,path,sizeof(sa.sun_path)-1);
    if (flags & ANET_CONNECT_NONBLOCK) {
       if (anetNonBlock(err,s) != ANET_OK)
         return ANET_ERR;
  }
    if (connect(s,(struct sockaddr*)&sa,sizeof(sa)) == -1) {
        if (errno == EINPROGRESS &&
            flags & ANET_CONNECT_NONBLOCK)
            return s;

        anetSetError(err, "connect: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return s;
}

int anetUnixConnect(char *err, char *path)
{
    return anetUnixGenericConnect(err,path,ANET_CONNECT_NONE);
}

int anetUnixNonBlockConnect(char *err, char *path)
{
    return anetUnixGenericConnect(err,path,ANET_CONNECT_NONBLOCK);
}

/* Like read(2) but make sure 'count' is read before to return
 * (unless error or EOF condition is encountered) */
int anetRead(int fd, char *buf, int count)
{
    int nread, totlen = 0;
    while(totlen != count) {
        nread = read(fd,buf,count-totlen);
        if (nread == 0) return totlen;
        if (nread == -1) return -1;
        totlen += nread;
        buf += nread;
    }
    return totlen;
}

/* Like write(2) but make sure 'count' is read before to return
 * (unless error is encountered) */
int anetWrite(int fd, char *buf, int count)
{
    int nwritten, totlen = 0;
    while(totlen != count) {
        nwritten = write(fd,buf,count-totlen);
        if (nwritten == 0) return totlen;
        if (nwritten == -1) return -1;
        totlen += nwritten;
        buf += nwritten;
    }
    return totlen;
}

static int anetListen(char *err, int s, struct sockaddr *sa, socklen_t len, int backlog) {
    if (bind(s,sa,len) == -1) {
        anetSetError(err, "bind: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }

    if (listen(s, backlog) == -1) {
        anetSetError(err, "listen: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return ANET_OK;
}

static int anetV6Only(char *err, int s) {
    int yes = 1;
    if (setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,&yes,sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return ANET_OK;
}

static int _anetTcpServer(char *err, int port, char *bindaddr, int af, int backlog)
{
    int s, rv;
    char _port[6];  /* strlen("65535") */
    struct addrinfo hints, *servinfo, *p;

    snprintf(_port,6,"%d",port);
    memset(&hints,0,sizeof(hints));
    hints.ai_family = af;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;    /* No effect if bindaddr != NULL */

    if ((rv = getaddrinfo(bindaddr,_port,&hints,&servinfo)) != 0) {
        anetSetError(err, "%s", gai_strerror(rv));
        return ANET_ERR;
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((s = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) == -1)
            continue;

        if (af == AF_INET6 && anetV6Only(err,s) == ANET_ERR) goto error;
        if (anetSetReuseAddr(err,s) == ANET_ERR) goto error;
        if (anetListen(err,s,p->ai_addr,p->ai_addrlen,backlog) == ANET_ERR) goto error;
        goto end;
    }
    if (p == NULL) {
        anetSetError(err, "unable to bind socket");
        goto error;
    }

error:
    s = ANET_ERR;
end:
    freeaddrinfo(servinfo);
    return s;
}

int anetTcpServer(char *err, int port, char *bindaddr, int backlog)
{
    return _anetTcpServer(err, port, bindaddr, AF_INET, backlog);
}

int anetTcp6Server(char *err, int port, char *bindaddr, int backlog)
{
    return _anetTcpServer(err, port, bindaddr, AF_INET6, backlog);
}

int anetUnixServer(char *err, char *path, mode_t perm, int backlog)
{
    int s;
    struct sockaddr_un sa;

    if ((s = anetCreateSocket(err,AF_LOCAL)) == ANET_ERR)
        return ANET_ERR;

    memset(&sa,0,sizeof(sa));
    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path,path,sizeof(sa.sun_path)-1);
    if (anetListen(err,s,(struct sockaddr*)&sa,sizeof(sa),backlog) == ANET_ERR)
        return ANET_ERR;
    if (perm)
        chmod(sa.sun_path, perm);
    return s;
}

static int anetGenericAccept(char *err, int s, struct sockaddr *sa, socklen_t *len) {
    int fd;
    while(1) {
        fd = accept(s,sa,len);
        if (fd == -1) {
            if (errno == EINTR)
                continue;
            else {
                anetSetError(err, "accept: %s", strerror(errno));
                return ANET_ERR;
            }
        }
        break;
    }
    return fd;
}

int anetTcpAccept(char *err, int s, char *ip, size_t ip_len, int *port) {
    int fd;
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);
    if ((fd = anetGenericAccept(err,s,(struct sockaddr*)&sa,&salen)) == -1)
        return ANET_ERR;

    if (sa.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&sa;
        if (ip) inet_ntop(AF_INET,(void*)&(s->sin_addr),ip,ip_len);
        if (port) *port = ntohs(s->sin_port);
    } else {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&sa;
        if (ip) inet_ntop(AF_INET6,(void*)&(s->sin6_addr),ip,ip_len);
        if (port) *port = ntohs(s->sin6_port);
    }
    return fd;
}

int anetUnixAccept(char *err, int s) {
    int fd;
    struct sockaddr_un sa;
    socklen_t salen = sizeof(sa);
    if ((fd = anetGenericAccept(err,s,(struct sockaddr*)&sa,&salen)) == -1)
        return ANET_ERR;

    return fd;
}

int anetPeerToString(int fd, char *ip, size_t ip_len, int *port) {
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);

    if (getpeername(fd,(struct sockaddr*)&sa,&salen) == -1) goto error;
    if (ip_len == 0) goto error;

    if (sa.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&sa;
        if (ip) inet_ntop(AF_INET,(void*)&(s->sin_addr),ip,ip_len);
        if (port) *port = ntohs(s->sin_port);
    } else if (sa.ss_family == AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&sa;
        if (ip) inet_ntop(AF_INET6,(void*)&(s->sin6_addr),ip,ip_len);
        if (port) *port = ntohs(s->sin6_port);
    } else if (sa.ss_family == AF_UNIX) {
        if (ip) strncpy(ip,"/unixsocket",ip_len);
        if (port) *port = 0;
    } else {
        goto error;
    }
    return 0;

error:
    if (ip) {
        if (ip_len >= 2) {
            ip[0] = '?';
            ip[1] = '\0';
        } else if (ip_len == 1) {
            ip[0] = '\0';
        }
    }
    if (port) *port = 0;
    return -1;
}

/* Format an IP,port pair into something easy to parse. If IP is IPv6
 * (matches for ":"), the ip is surrounded by []. IP and port are just
 * separated by colons. This the standard to display addresses within Redis. */
int anetFormatAddr(char *buf, size_t buf_len, char *ip, int port) {
    return snprintf(buf,buf_len, strchr(ip,':') ?
           "[%s]:%d" : "%s:%d", ip, port);
}

/* Like anetFormatAddr() but extract ip and port from the socket's peer. */
int anetFormatPeer(int fd, char *buf, size_t buf_len) {
    char ip[INET6_ADDRSTRLEN];
    int port;

    anetPeerToString(fd,ip,sizeof(ip),&port);
    return anetFormatAddr(buf, buf_len, ip, port);
}

int anetSockName(int fd, char *ip, size_t ip_len, int *port) {
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);

    if (getsockname(fd,(struct sockaddr*)&sa,&salen) == -1) {
        if (port) *port = 0;
        ip[0] = '?';
        ip[1] = '\0';
        return -1;
    }
    if (sa.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&sa;
        if (ip) inet_ntop(AF_INET,(void*)&(s->sin_addr),ip,ip_len);
        if (port) *port = ntohs(s->sin_port);
    } else {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&sa;
        if (ip) inet_ntop(AF_INET6,(void*)&(s->sin6_addr),ip,ip_len);
        if (port) *port = ntohs(s->sin6_port);
    }
    return 0;
}

int anetFormatSock(int fd, char *fmt, size_t fmt_len) {
    char ip[INET6_ADDRSTRLEN];
    int port;

    anetSockName(fd,ip,sizeof(ip),&port);
    return anetFormatAddr(fmt, fmt_len, ip, port);
}

int anetSSLAccept( char *err, int fd, struct redisServer server, anetSSLConnection *ctn) {

  ctn->sd = -1;
  ctn->ctx = NULL;
  ctn->ssl = NULL;
  ctn->bio = NULL;
  ctn->conn_str = NULL;

  if( fd == -1 ) {
    return ANET_ERR;
  }
  ctn->sd = fd;

  // Create the SSL Context ( server method )
  SSL_CTX* ctx = SSL_CTX_new(TLSv1_1_server_method());
  ctn->ctx = ctx;

   /*
     You will need to generate certificates, the root certificate authority file, the private key file, and the random file yourself.
     Google will help. The openssl executable created when you compiled OpenSSL can do all this.
   */

    // Load trusted root authorities
    SSL_CTX_load_verify_locations(ctx, server.ssl_root_file, server.ssl_root_dir);

    // Sets the default certificate password callback function. Read more under the Certificate Verification section.
    if( NULL != server.ssl_srvr_cert_passwd ) {
      global_ssl_cert_password = server.ssl_srvr_cert_passwd;
      SSL_CTX_set_default_passwd_cb(ctx, password_callback);
    }

    // Sets the certificate file to be used.
    SSL_CTX_use_certificate_file(ctx, server.ssl_cert_file, SSL_FILETYPE_PEM);

    // Sets the private key file to be used.
    SSL_CTX_use_PrivateKey_file(ctx, server.ssl_pk_file, SSL_FILETYPE_PEM);

    // Set the maximum depth to be used verifying certificates
    // Due to a bug, this is not enforced. The verify callback must enforce it.
    SSL_CTX_set_verify_depth(ctx, 1);

    // Set the certificate verification callback.
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER /* | SSL_VERIFY_FAIL_IF_NO_PEER_CERT */, verify_callback);

    /*
      End certificate verification setup.
    */

    // We need to load the Diffie-Hellman key exchange parameters.
    // First load dh1024.pem (you DID create it, didn't you?)
    BIO* bio = BIO_new_file(server.ssl_dhk_file, "r");

    // Did we get a handle to the file?
    if (bio == NULL) {
      anetSetError(err, "SSL Accept: Couldn't open DH param file");
      anetCleanupSSL( ctn);
      return ANET_ERR;
    }

    // Read in the DH params.
    DH* ret = PEM_read_bio_DHparams(bio, NULL, NULL, NULL);

    // Free up the BIO object.
    BIO_free(bio);

    // Set up our SSL_CTX to use the DH parameters.
    if (SSL_CTX_set_tmp_dh(ctx, ret) < 0) {
      anetSetError(err, "SSL Accept: Couldn't set DH parameters");
      anetCleanupSSL( ctn );
      return ANET_ERR;
    }

    // Now we need to generate a RSA key for use.
    // 1024-bit key. If you want to use something stronger, go ahead but it must be a power of 2. Upper limit should be 4096.
    RSA* rsa = RSA_generate_key(1024, RSA_F4, NULL, NULL);

    // Set up our SSL_CTX to use the generated RSA key.
    if (!SSL_CTX_set_tmp_rsa(ctx, rsa)) {
      anetSetError(err, "SSL Accept: Couldn't set RSA Key");
      anetCleanupSSL( ctn );
      return ANET_ERR;
    }

    // Free up the RSA structure.
    RSA_free(rsa);

    /*
      For some reason many tutorials don't include this...

      Servers must specify the ciphers they can use, to verify that both the client and the server can use the same cipher.
      If you don't do this, you'll get errors relating to "no shared ciphers" or "no common ciphers".

      Use all ciphers with the exception of: ones with anonymous Diffie-Hellman, low-strength ciphers, export ciphers, md5 hashing. Ordered from strongest to weakest.
      Note that as ciphers become broken, it will be necessary to change the available cipher list to remain secure.
    */

    SSL_CTX_set_cipher_list(ctx, "ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");

    // Set up our SSL object as before
    SSL* ssl = SSL_new(ctx);
    ctn->ssl = ssl;
    
    // Set up our BIO object to use the client socket
    BIO* sslclient = BIO_new_socket(fd, BIO_NOCLOSE);
    ctn->bio = sslclient;

    // Set up our SSL object to use the BIO.
    SSL_set_bio(ssl, sslclient, sslclient);

    // Do SSL handshaking.
    int r = SSL_accept(ssl);

    // Something failed. Print out all the error information, since all of it may be relevant to the problem.
    if (r != 1) {
      char error[65535];

      ERR_error_string_n(ERR_get_error(), error, 65535);

      anetSetError(err, "SSL Accept: Error %d - %s ", SSL_get_error(ssl, r), error );

      // We failed to accept this client connection.
      // Ideally here you'll drop the connection and continue on.
      anetCleanupSSL( ctn );
      return ANET_ERR;
    }

    /* Verify certificate */
    if (SSL_get_verify_result(ssl) != X509_V_OK) {
      anetSetError(err, "SSL Accept: Certificate failed verification!");
      // Ideally here you'll close this connection and continue on.
      anetCleanupSSL( ctn );
      return ANET_ERR;
    }

    return ANET_OK;
}

void anetSSLPrepare( ) {
  CRYPTO_malloc_init(); // Initialize malloc, free, etc for OpenSSL's use
  SSL_library_init(); // Initialize OpenSSL's SSL libraries
  SSL_load_error_strings(); // Load SSL error strings
  ERR_load_BIO_strings(); // Load BIO error strings
  OpenSSL_add_all_algorithms(); // Load all available encryption algorithms
}

void anetCleanupSSL( anetSSLConnection *ctn ) {
  if( NULL != ctn ) {
/*    if( NULL != ctn->bio ) {
      // Free up that BIO object we created.
      BIO_free_all(ctn->bio);
      ctn->bio = NULL;
    }
*/
    if( NULL != ctn->ssl ) {
      // Remember, we also need to free up that SSL_CTX object!
      SSL_free(ctn->ssl);
      ctn->ssl = NULL;
    }

    if( NULL != ctn->ctx ) {
      // Remember, we also need to free up that SSL_CTX object!
      SSL_CTX_free(ctn->ctx);
      ctn->ctx = NULL;
    }
    if( NULL != ctn->conn_str ) {
      free(ctn->conn_str);
      ctn->conn_str = NULL;
    }
  }
}

int wildcmp(const char *wild, const char *string, int len) {
  int cp = 0;
  int mp = 0;
  int strn_cntr = len;
  int wild_cntr = strlen(wild);

  while( (*(wild+wild_cntr) != '*') && strn_cntr >= 0 && wild_cntr >= 0 ) {
    if ((*(wild+wild_cntr) != *(string+strn_cntr)) && (*(wild+wild_cntr) != '?')) {
      return 0;
    }
    wild_cntr--;
    strn_cntr--;
  }

  while( ( wild_cntr >= 0 ) && ( strn_cntr >= 0 ) ) {
    if( *(wild+wild_cntr) == '*') {
      if( --wild_cntr < 0 ) {
        return 1;
      }
      mp = wild_cntr;
      cp = strn_cntr-1;
    } else if ((*(wild+wild_cntr) == *(string+strn_cntr)) || (*(wild+wild_cntr) == '?')) {
      wild_cntr--;
      strn_cntr--;
    } else {
      wild_cntr = mp;
      strn_cntr = cp--;
    }
  }

  while (*(wild+wild_cntr) == '*') {
    wild_cntr--;
  }

  return ( (wild_cntr == -1) && (strn_cntr == -1) );
}


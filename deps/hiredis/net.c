/* Extracted from anet.c to work properly with Hiredis error reporting.
 *
 * Copyright (c) 2006-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2011, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
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
#include <sys/select.h>
#include <sys/un.h>
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
#include <poll.h>
#include <limits.h>
#include <openssl/bio.h> // BIO objects for I/O
#include <openssl/ssl.h> // SSL and SSL_CTX for SSL connections
#include <openssl/err.h> // Error reporting

#include "net.h"
#include "sds.h"

/* Defined in hiredis.c */
void __redisSetError(redisContext *c, int type, const char *str);
int compareTimeval( struct timeval a, struct timeval b );
struct timeval subtractTimeval( struct timeval a, struct timeval b);

static void redisContextCloseFd(redisContext *c) {
    if (c && c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

static void __redisSetErrorFromErrno(redisContext *c, int type, const char *prefix) {
    char buf[128] = { 0 };
    size_t len = 0;

    if (prefix != NULL)
        len = snprintf(buf,sizeof(buf),"%s: ",prefix);
    strerror_r(errno,buf+len,sizeof(buf)-len);
    __redisSetError(c,type,buf);
}

static int redisSetReuseAddr(redisContext *c) {
    int on = 1;
    if (setsockopt(c->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
        redisContextCloseFd(c);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

static int redisCreateSocket(redisContext *c, int type) {
    int s;
    if ((s = socket(type, SOCK_STREAM, 0)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
        return REDIS_ERR;
    }
    c->fd = s;
    if (type == AF_INET) {
        if (redisSetReuseAddr(c) == REDIS_ERR) {
            return REDIS_ERR;
        }
    }
    return REDIS_OK;
}

static int redisSetBlocking(redisContext *c, int blocking) {
    int flags;

    /* Set the socket nonblocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
    if ((flags = fcntl(c->fd, F_GETFL)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"fcntl(F_GETFL)");
        redisContextCloseFd(c);
        return REDIS_ERR;
    }

    if (blocking)
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;

    if (fcntl(c->fd, F_SETFL, flags) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"fcntl(F_SETFL)");
        redisContextCloseFd(c);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

int redisKeepAlive(redisContext *c, int interval) {
    int val = 1;
    int fd = c->fd;

    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1){
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }

    val = interval;

#ifdef _OSX
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }
#else
#ifndef __sun
    val = interval;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }

    val = interval/3;
    if (val == 0) val = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }

    val = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }
#endif
#endif

    return REDIS_OK;
}

static int redisSetTcpNoDelay(redisContext *c) {
    int yes = 1;
    if (setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(TCP_NODELAY)");
        redisContextCloseFd(c);
       return REDIS_ERR;
   }
    return REDIS_OK;
}

#define __MAX_MSEC (((LONG_MAX) - 999) / 1000)

static int redisContextWaitReady(redisContext *c, const struct timeval *timeout) {
    struct pollfd   wfd[1];
    long msec;

    msec          = -1;
    wfd[0].fd     = c->fd;
    wfd[0].events = POLLOUT;

    /* Only use timeout when not NULL. */
    if (timeout != NULL) {
        if (timeout->tv_usec > 1000000 || timeout->tv_sec > __MAX_MSEC) {
            __redisSetErrorFromErrno(c, REDIS_ERR_IO, NULL);
            redisContextCloseFd(c);
            return REDIS_ERR;
        }

        msec = (timeout->tv_sec * 1000) + ((timeout->tv_usec + 999) / 1000);

        if (msec < 0 || msec > INT_MAX) {
            msec = INT_MAX;
        }
    }

    if (errno == EINPROGRESS) {
        int res;

        if ((res = poll(wfd, 1, msec)) == -1) {
            __redisSetErrorFromErrno(c, REDIS_ERR_IO, "poll(2)");
            redisContextCloseFd(c);
            return REDIS_ERR;
        } else if (res == 0) {
            errno = ETIMEDOUT;
            __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
            redisContextCloseFd(c);
            return REDIS_ERR;
        }

        if (redisCheckSocketError(c) != REDIS_OK)
            return REDIS_ERR;

        return REDIS_OK;
    }

    __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
    redisContextCloseFd(c);
    return REDIS_ERR;
}

int redisCheckSocketError(redisContext *c) {
    int err = 0;
    socklen_t errlen = sizeof(err);

    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"getsockopt(SO_ERROR)");
        return REDIS_ERR;
    }

    if (err) {
        errno = err;
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
        return REDIS_ERR;
    }

    return REDIS_OK;
}

int redisContextSetTimeout(redisContext *c, const struct timeval tv) {
    if (setsockopt(c->fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(SO_RCVTIMEO)");
        return REDIS_ERR;
    }
    if (setsockopt(c->fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(SO_SNDTIMEO)");
        return REDIS_ERR;
    }
    return REDIS_OK;
}

static int _redisContextConnectTcp(redisContext *c, const char *addr, int port,
                                   const struct timeval *timeout,
                                   const char *source_addr) {
    int s, rv;
    char _port[6];  /* strlen("65535"); */
    struct addrinfo hints, *servinfo, *bservinfo, *p, *b;
    int blocking = (c->flags & REDIS_BLOCK);

    snprintf(_port, 6, "%d", port);
    memset(&hints,0,sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    /* Try with IPv6 if no IPv4 address was found. We do it in this order since
     * in a Redis client you can't afford to test if you have IPv6 connectivity
     * as this would add latency to every connect. Otherwise a more sensible
     * route could be: Use IPv6 if both addresses are available and there is IPv6
     * connectivity. */
    if ((rv = getaddrinfo(addr,_port,&hints,&servinfo)) != 0) {
         hints.ai_family = AF_INET6;
         if ((rv = getaddrinfo(addr,_port,&hints,&servinfo)) != 0) {
            __redisSetError(c,REDIS_ERR_OTHER,gai_strerror(rv));
            return REDIS_ERR;
        }
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((s = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) == -1)
            continue;

        c->fd = s;
        if (redisSetBlocking(c,0) != REDIS_OK)
            goto error;
        if (source_addr) {
            int bound = 0;
            /* Using getaddrinfo saves us from self-determining IPv4 vs IPv6 */
            if ((rv = getaddrinfo(source_addr, NULL, &hints, &bservinfo)) != 0) {
                char buf[128];
                snprintf(buf,sizeof(buf),"Can't get addr: %s",gai_strerror(rv));
                __redisSetError(c,REDIS_ERR_OTHER,buf);
                goto error;
            }
            for (b = bservinfo; b != NULL; b = b->ai_next) {
                if (bind(s,b->ai_addr,b->ai_addrlen) != -1) {
                    bound = 1;
                    break;
                }
            }
            freeaddrinfo(bservinfo);
            if (!bound) {
                char buf[128];
                snprintf(buf,sizeof(buf),"Can't bind socket: %s",strerror(errno));
                __redisSetError(c,REDIS_ERR_OTHER,buf);
                goto error;
            }
        }
        if (connect(s,p->ai_addr,p->ai_addrlen) == -1) {
            if (errno == EHOSTUNREACH) {
                redisContextCloseFd(c);
                continue;
            } else if (errno == EINPROGRESS && !blocking) {
                /* This is ok. */
            } else {
                if (redisContextWaitReady(c,timeout) != REDIS_OK)
                    goto error;
            }
        }
        if (blocking && redisSetBlocking(c,1) != REDIS_OK)
            goto error;
        if (redisSetTcpNoDelay(c) != REDIS_OK)
            goto error;

        c->flags |= REDIS_CONNECTED;
        rv = REDIS_OK;
        goto end;
    }
    if (p == NULL) {
        char buf[128];
        snprintf(buf,sizeof(buf),"Can't create socket: %s",strerror(errno));
        __redisSetError(c,REDIS_ERR_OTHER,buf);
        goto error;
    }

error:
    rv = REDIS_ERR;
end:
    freeaddrinfo(servinfo);
    return rv;  // Need to return REDIS_OK if alright
}

int redisContextConnectTcp(redisContext *c, const char *addr, int port,
                           const struct timeval *timeout) {
    return _redisContextConnectTcp(c, addr, port, timeout, NULL);
}

int redisContextConnectBindTcp(redisContext *c, const char *addr, int port,
                               const struct timeval *timeout,
                               const char *source_addr) {
    return _redisContextConnectTcp(c, addr, port, timeout, source_addr);
}

int redisContextConnectSSL(redisContext *c, const char *addr, int port, char* certfile, char* certdir, struct timeval *timeout) {
  struct timeval  start_time;
  int             has_timeout = 0;
  int             is_nonblocking = 0;

  c->ssl.sd = -1;
  c->ssl.ctx = NULL;
  c->ssl.ssl = NULL;
  c->ssl.bio = NULL;

  // Set up a SSL_CTX object, which will tell our BIO object how to do its work
  SSL_CTX* ctx = SSL_CTX_new(SSLv3_client_method());
  c->ssl.ctx = ctx;

  // Create a SSL object pointer, which our BIO object will provide.
  SSL* ssl;

  // Create our BIO object for SSL connections.
  BIO* bio = BIO_new_ssl_connect(ctx);
  c->ssl.bio = bio;

  // Failure?
  if (bio == NULL) {
     char errorbuf[1024];
     __redisSetError(c,REDIS_ERR_OTHER,"SSL Error: Error creating BIO!\n");

     ERR_error_string(1024,errorbuf);
     __redisSetError(c,REDIS_ERR_OTHER,errorbuf);

     // We need to free up the SSL_CTX before we leave.
     cleanupSSL( &c->ssl );
     return REDIS_ERR;
  }

  // Makes ssl point to bio's SSL object.
  BIO_get_ssl(bio, &ssl);
  c->ssl.ssl = ssl;

  // Set the SSL to automatically retry on failure.
  SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);

  char* connect_str = (char *)calloc( 1, strlen( addr ) + 10 );
  sprintf( connect_str, "%s:%d", addr, port );
  c->ssl.conn_str = connect_str;

  // We're connection to the REDIS server at host:port
  BIO_set_conn_hostname(bio, connect_str);

  SSL_CTX_load_verify_locations(ctx, certfile, certdir);

  // Are we supposed to be blocking or non-blocking?
  if( (c->flags & REDIS_BLOCK) == 0) {
	  is_nonblocking = 1;
	  BIO_set_nbio(bio,1);
  }

  // What about a timeout?
  // If we're blocking and have a timeout, we need to handle that specially
  if( NULL != timeout ) {
	BIO_set_nbio(bio,1);
	has_timeout = 1;
	gettimeofday(&start_time, NULL);
  }

  while(1) {
    struct timeval	cur_time,
		            elapsed_time;

	if (has_timeout) {
	  gettimeofday(&cur_time, NULL);
      elapsed_time = subtractTimeval( cur_time, start_time );

  	  if (compareTimeval( elapsed_time, *timeout) > 0) {
  	    char errorbuf[1024];

  	    __redisSetError(c,REDIS_ERR_OTHER,"SSL Error: Connection timed out.");
  	    ERR_error_string(1024,errorbuf);
  	    __redisSetError(c,REDIS_ERR_OTHER,errorbuf);
  	    cleanupSSL( &(c->ssl) );
  	    return REDIS_ERR;
  	  }
	}

    // Same as before, try to connect.
    if (BIO_do_connect(bio) <= 0 ) {
      if( BIO_should_retry( bio ) ) {
    	  // We need to retry.
      } else {
        char errorbuf[1024];
        __redisSetError(c,REDIS_ERR_OTHER,"SSL Error: Failed to connect");
        ERR_error_string(1024,errorbuf);
        __redisSetError(c,REDIS_ERR_OTHER,errorbuf);
        cleanupSSL( &(c->ssl) );
        return REDIS_ERR;
      }
    } else {
      // connect is done...
      break;
    }

    if( has_timeout ) {
    	// Do select and seelct on it
    	int result;
    	int fd = BIO_get_fd( bio, NULL );
    	struct timeval time_left;

    	if (has_timeout) {
    	  time_left = subtractTimeval( *timeout, elapsed_time );
    	}

    	fd_set readset, writeset;
        FD_ZERO(&readset);
    	FD_SET(fd, &readset);
        FD_ZERO(&writeset);
    	FD_SET(fd, &writeset);

    	do {
    	  result = select( fd+1, &readset, &writeset, NULL, &time_left);
    	} while( result == -1 && errno == EINTR );
    }

  }

  while(1) {
    struct timeval	cur_time,
		            elapsed_time;

	if (has_timeout) {
	  gettimeofday(&cur_time, NULL);
      elapsed_time = subtractTimeval( cur_time, start_time );

  	  if (compareTimeval( elapsed_time, *timeout) > 0) {
  	    char errorbuf[1024];

  	    __redisSetError(c,REDIS_ERR_OTHER,"SSL Error: Connection timed out.");
  	    ERR_error_string(1024,errorbuf);
  	    __redisSetError(c,REDIS_ERR_OTHER,errorbuf);
  	    cleanupSSL( &(c->ssl) );
  	    return REDIS_ERR;
  	  }
	}

    // Now we need to do the SSL handshake, so we can communicate.
    if (BIO_do_handshake(bio) <= 0) {
        if( BIO_should_retry( bio ) ) {
      	  // We need to retry.
        } else {
          char errorbuf[1024];
          __redisSetError(c,REDIS_ERR_OTHER,"SSL Error: handshake failure");
          ERR_error_string(1024,errorbuf);
          __redisSetError(c,REDIS_ERR_OTHER,errorbuf);
          cleanupSSL( &(c->ssl) );
          return REDIS_ERR;
        }
    } else {
    	// handshake is done...
    	break;
    }

    if( has_timeout ) {
    	// Do select and seelct on it
    	int result;
    	int fd = BIO_get_fd( bio, NULL );
    	struct timeval time_left;

    	if (has_timeout) {
    	  time_left = subtractTimeval( *timeout, elapsed_time );
    	}

    	fd_set readset, writeset;
        FD_ZERO(&readset);
    	FD_SET(fd, &readset);
        FD_ZERO(&writeset);
    	FD_SET(fd, &writeset);

    	do {
    	  result = select( fd+1, &readset, &writeset, NULL, &time_left);
    	} while( result == -1 && errno == EINTR );
    }

  }

  long verify_result = SSL_get_verify_result(ssl);
  if( verify_result == X509_V_OK) {
    X509* peerCertificate = SSL_get_peer_certificate(ssl);

    char commonName [512];
    X509_NAME * name = X509_get_subject_name(peerCertificate);
    X509_NAME_get_text_by_NID(name, NID_commonName, commonName, 512);

    // TODO: Add a redis config parameter for the common name to check for.
    //       Since Redis connections are generally done with an IP address directly,
    //       we can't just assume that the name we get on the addr is an actual name.
    //
    //    if(strcasecmp(commonName, "BradBroerman") != 0) {
    //      __redisSetError(c,REDIS_ERR_OTHER,"SSL Error: Error validating cert common name.\n\n" );
    //      cleanupSSL( &(c->ssl) );
    //      return REDIS_ERR;

  } else {
     char errorbuf[1024];
     __redisSetError(c,REDIS_ERR_OTHER,"SSL Error: Error retrieving peer certificate.\n" );
     ERR_error_string(1024,errorbuf);
     __redisSetError(c,REDIS_ERR_OTHER,errorbuf);
     cleanupSSL( &(c->ssl) );
     return REDIS_ERR;
  }

  BIO_set_nbio(bio,is_nonblocking);

  return REDIS_OK;
}

void cleanupSSL( SSLConnection* ctn ) {
  if( NULL != ctn ) {
    if( NULL != ctn->ctx ) {
      // Remember, we also need to free up that SSL_CTX object!
      SSL_CTX_free(ctn->ctx);
      ctn->ctx = NULL;
    }
    if( NULL != ctn->bio ) {
      // Free up that BIO object we created.
      BIO_free_all(ctn->bio);
      ctn->bio = NULL;
    }
    if( NULL != ctn->conn_str ) {
      free( ctn->conn_str );
      ctn->conn_str = NULL;
    }
  }

  return;
}

void setupSSL() {
  CRYPTO_malloc_init(); // Initialize malloc, free, etc for OpenSSL's use
  SSL_library_init(); // Initialize OpenSSL's SSL libraries
  SSL_load_error_strings(); // Load SSL error strings
  ERR_load_BIO_strings(); // Load BIO error strings
  OpenSSL_add_all_algorithms(); // Load all available encryption algorithms
}

int redisContextConnectUnix(redisContext *c, const char *path, const struct timeval *timeout) {

    int blocking = (c->flags & REDIS_BLOCK);
    struct sockaddr_un sa;

    if (redisCreateSocket(c,AF_LOCAL) < 0)
        return REDIS_ERR;
    if (redisSetBlocking(c,0) != REDIS_OK)
        return REDIS_ERR;

    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path,path,sizeof(sa.sun_path)-1);
    if (connect(c->fd, (struct sockaddr*)&sa, sizeof(sa)) == -1) {
        if (errno == EINPROGRESS && !blocking) {
            /* This is ok. */
        } else {
            if (redisContextWaitReady(c,timeout) != REDIS_OK)
                return REDIS_ERR;
        }
    }

    /* Reset socket to be blocking after connect(2). */
    if (blocking && redisSetBlocking(c,1) != REDIS_OK)
        return REDIS_ERR;

    c->flags |= REDIS_CONNECTED;
    return REDIS_OK;
}
struct timeval subtractTimeval( struct timeval a, struct timeval b )
{
  struct timeval difference;

  difference.tv_sec = a.tv_sec - b.tv_sec;
  difference.tv_usec = a.tv_usec - b.tv_usec;

  if (a.tv_usec < b.tv_usec) {
    b.tv_sec -= 1L;
    b.tv_usec += 1000000L;
  }

  return difference;
}

int compareTimeval( struct timeval a, struct timeval b )
{
  if (a.tv_sec > b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_usec > b.tv_usec) ) {
    return 1;
  } else if( a.tv_sec == b.tv_sec && a.tv_usec == b.tv_usec ) {
    return 0;
  } 
  
  return -1;
}


/*
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2011, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2019, Redis Labs
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

#include "hiredis.h"
#include "async.h"

#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "async_private.h"

void __redisSetError(redisContext *c, int type, const char *str);

/* The SSL context is attached to SSL/TLS connections as a privdata. */
typedef struct redisSSLContext {
    /**
     * OpenSSL SSL_CTX; It is optional and will not be set when using
     * user-supplied SSL.
     */
    SSL_CTX *ssl_ctx;

    /**
     * OpenSSL SSL object.
     */
    SSL *ssl;

    /**
     * SSL_write() requires to be called again with the same arguments it was
     * previously called with in the event of an SSL_read/SSL_write situation
     */
    size_t lastLen;

    /** Whether the SSL layer requires read (possibly before a write) */
    int wantRead;

    /**
     * Whether a write was requested prior to a read. If set, the write()
     * should resume whenever a read takes place, if possible
     */
    int pendingWrite;
} redisSSLContext;

/* Forward declaration */
redisContextFuncs redisContextSSLFuncs;

#ifdef HIREDIS_SSL_TRACE
/**
 * Callback used for debugging
 */
static void sslLogCallback(const SSL *ssl, int where, int ret) {
    const char *retstr = "";
    int should_log = 1;
    /* Ignore low-level SSL stuff */

    if (where & SSL_CB_ALERT) {
        should_log = 1;
    }
    if (where == SSL_CB_HANDSHAKE_START || where == SSL_CB_HANDSHAKE_DONE) {
        should_log = 1;
    }
    if ((where & SSL_CB_EXIT) && ret == 0) {
        should_log = 1;
    }

    if (!should_log) {
        return;
    }

    retstr = SSL_alert_type_string(ret);
    printf("ST(0x%x). %s. R(0x%x)%s\n", where, SSL_state_string_long(ssl), ret, retstr);

    if (where == SSL_CB_HANDSHAKE_DONE) {
        printf("Using SSL version %s. Cipher=%s\n", SSL_get_version(ssl), SSL_get_cipher_name(ssl));
    }
}
#endif

/**
 * OpenSSL global initialization and locking handling callbacks.
 * Note that this is only required for OpenSSL < 1.1.0.
 */

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define HIREDIS_USE_CRYPTO_LOCKS
#endif

#ifdef HIREDIS_USE_CRYPTO_LOCKS
typedef pthread_mutex_t sslLockType;
static void sslLockInit(sslLockType *l) {
    pthread_mutex_init(l, NULL);
}
static void sslLockAcquire(sslLockType *l) {
    pthread_mutex_lock(l);
}
static void sslLockRelease(sslLockType *l) {
    pthread_mutex_unlock(l);
}
static pthread_mutex_t *ossl_locks;

static void opensslDoLock(int mode, int lkid, const char *f, int line) {
    sslLockType *l = ossl_locks + lkid;

    if (mode & CRYPTO_LOCK) {
        sslLockAcquire(l);
    } else {
        sslLockRelease(l);
    }

    (void)f;
    (void)line;
}

static void initOpensslLocks(void) {
    unsigned ii, nlocks;
    if (CRYPTO_get_locking_callback() != NULL) {
        /* Someone already set the callback before us. Don't destroy it! */
        return;
    }
    nlocks = CRYPTO_num_locks();
    ossl_locks = malloc(sizeof(*ossl_locks) * nlocks);
    for (ii = 0; ii < nlocks; ii++) {
        sslLockInit(ossl_locks + ii);
    }
    CRYPTO_set_locking_callback(opensslDoLock);
}
#endif /* HIREDIS_USE_CRYPTO_LOCKS */

/**
 * SSL Connection initialization.
 */

static int redisSSLConnect(redisContext *c, SSL_CTX *ssl_ctx, SSL *ssl) {
    if (c->privdata) {
        __redisSetError(c, REDIS_ERR_OTHER, "redisContext was already associated");
        return REDIS_ERR;
    }
    c->privdata = calloc(1, sizeof(redisSSLContext));

    c->funcs = &redisContextSSLFuncs;
    redisSSLContext *rssl = c->privdata;

    rssl->ssl_ctx = ssl_ctx;
    rssl->ssl = ssl;

    SSL_set_mode(rssl->ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_set_fd(rssl->ssl, c->fd);
    SSL_set_connect_state(rssl->ssl);

    ERR_clear_error();
    int rv = SSL_connect(rssl->ssl);
    if (rv == 1) {
        return REDIS_OK;
    }

    rv = SSL_get_error(rssl->ssl, rv);
    if (((c->flags & REDIS_BLOCK) == 0) &&
        (rv == SSL_ERROR_WANT_READ || rv == SSL_ERROR_WANT_WRITE)) {
        return REDIS_OK;
    }

    if (c->err == 0) {
        char err[512];
        if (rv == SSL_ERROR_SYSCALL)
            snprintf(err,sizeof(err)-1,"SSL_connect failed: %s",strerror(errno));
        else {
            unsigned long e = ERR_peek_last_error();
            snprintf(err,sizeof(err)-1,"SSL_connect failed: %s",
                    ERR_reason_error_string(e));
        }
        __redisSetError(c, REDIS_ERR_IO, err);
    }
    return REDIS_ERR;
}

int redisInitiateSSL(redisContext *c, SSL *ssl) {
    return redisSSLConnect(c, NULL, ssl);
}

int redisSecureConnection(redisContext *c, const char *capath,
                          const char *certpath, const char *keypath, const char *servername) {

    SSL_CTX *ssl_ctx = NULL;
    SSL *ssl = NULL;

    /* Initialize global OpenSSL stuff */
    static int isInit = 0;
    if (!isInit) {
        isInit = 1;
        SSL_library_init();
#ifdef HIREDIS_USE_CRYPTO_LOCKS
        initOpensslLocks();
#endif
    }

    ssl_ctx = SSL_CTX_new(SSLv23_client_method());
    if (!ssl_ctx) {
        __redisSetError(c, REDIS_ERR_OTHER, "Failed to create SSL_CTX");
        goto error;
    }

#ifdef HIREDIS_SSL_TRACE
    SSL_CTX_set_info_callback(ssl_ctx, sslLogCallback);
#endif
    SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, NULL);
    if ((certpath != NULL && keypath == NULL) || (keypath != NULL && certpath == NULL)) {
        __redisSetError(c, REDIS_ERR_OTHER, "certpath and keypath must be specified together");
        goto error;
    }

    if (capath) {
        if (!SSL_CTX_load_verify_locations(ssl_ctx, capath, NULL)) {
            __redisSetError(c, REDIS_ERR_OTHER, "Invalid CA certificate");
            goto error;
        }
    }
    if (certpath) {
        if (!SSL_CTX_use_certificate_chain_file(ssl_ctx, certpath)) {
            __redisSetError(c, REDIS_ERR_OTHER, "Invalid client certificate");
            goto error;
        }
        if (!SSL_CTX_use_PrivateKey_file(ssl_ctx, keypath, SSL_FILETYPE_PEM)) {
            __redisSetError(c, REDIS_ERR_OTHER, "Invalid client key");
            goto error;
        }
    }

    ssl = SSL_new(ssl_ctx);
    if (!ssl) {
        __redisSetError(c, REDIS_ERR_OTHER, "Couldn't create new SSL instance");
        goto error;
    }
    if (servername) {
        if (!SSL_set_tlsext_host_name(ssl, servername)) {
            __redisSetError(c, REDIS_ERR_OTHER, "Couldn't set server name indication");
            goto error;
        }
    }

    return redisSSLConnect(c, ssl_ctx, ssl);

error:
    if (ssl) SSL_free(ssl);
    if (ssl_ctx) SSL_CTX_free(ssl_ctx);
    return REDIS_ERR;
}

static int maybeCheckWant(redisSSLContext *rssl, int rv) {
    /**
     * If the error is WANT_READ or WANT_WRITE, the appropriate flags are set
     * and true is returned. False is returned otherwise
     */
    if (rv == SSL_ERROR_WANT_READ) {
        rssl->wantRead = 1;
        return 1;
    } else if (rv == SSL_ERROR_WANT_WRITE) {
        rssl->pendingWrite = 1;
        return 1;
    } else {
        return 0;
    }
}

/**
 * Implementation of redisContextFuncs for SSL connections.
 */

static void redisSSLFreeContext(void *privdata){
    redisSSLContext *rsc = privdata;

    if (!rsc) return;
    if (rsc->ssl) {
        SSL_free(rsc->ssl);
        rsc->ssl = NULL;
    }
    if (rsc->ssl_ctx) {
        SSL_CTX_free(rsc->ssl_ctx);
        rsc->ssl_ctx = NULL;
    }
    free(rsc);
}

static int redisSSLRead(redisContext *c, char *buf, size_t bufcap) {
    redisSSLContext *rssl = c->privdata;

    int nread = SSL_read(rssl->ssl, buf, bufcap);
    if (nread > 0) {
        return nread;
    } else if (nread == 0) {
        __redisSetError(c, REDIS_ERR_EOF, "Server closed the connection");
        return -1;
    } else {
        int err = SSL_get_error(rssl->ssl, nread);
        if (c->flags & REDIS_BLOCK) {
            /**
             * In blocking mode, we should never end up in a situation where
             * we get an error without it being an actual error, except
             * in the case of EINTR, which can be spuriously received from
             * debuggers or whatever.
             */
            if (errno == EINTR) {
                return 0;
            } else {
                const char *msg = NULL;
                if (errno == EAGAIN) {
                    msg = "Resource temporarily unavailable";
                }
                __redisSetError(c, REDIS_ERR_IO, msg);
                return -1;
            }
        }

        /**
         * We can very well get an EWOULDBLOCK/EAGAIN, however
         */
        if (maybeCheckWant(rssl, err)) {
            return 0;
        } else {
            __redisSetError(c, REDIS_ERR_IO, NULL);
            return -1;
        }
    }
}

static int redisSSLWrite(redisContext *c) {
    redisSSLContext *rssl = c->privdata;

    size_t len = rssl->lastLen ? rssl->lastLen : sdslen(c->obuf);
    int rv = SSL_write(rssl->ssl, c->obuf, len);

    if (rv > 0) {
        rssl->lastLen = 0;
    } else if (rv < 0) {
        rssl->lastLen = len;

        int err = SSL_get_error(rssl->ssl, rv);
        if ((c->flags & REDIS_BLOCK) == 0 && maybeCheckWant(rssl, err)) {
            return 0;
        } else {
            __redisSetError(c, REDIS_ERR_IO, NULL);
            return -1;
        }
    }
    return rv;
}

static void redisSSLAsyncRead(redisAsyncContext *ac) {
    int rv;
    redisSSLContext *rssl = ac->c.privdata;
    redisContext *c = &ac->c;

    rssl->wantRead = 0;

    if (rssl->pendingWrite) {
        int done;

        /* This is probably just a write event */
        rssl->pendingWrite = 0;
        rv = redisBufferWrite(c, &done);
        if (rv == REDIS_ERR) {
            __redisAsyncDisconnect(ac);
            return;
        } else if (!done) {
            _EL_ADD_WRITE(ac);
        }
    }

    rv = redisBufferRead(c);
    if (rv == REDIS_ERR) {
        __redisAsyncDisconnect(ac);
    } else {
        _EL_ADD_READ(ac);
        redisProcessCallbacks(ac);
    }
}

static void redisSSLAsyncWrite(redisAsyncContext *ac) {
    int rv, done = 0;
    redisSSLContext *rssl = ac->c.privdata;
    redisContext *c = &ac->c;

    rssl->pendingWrite = 0;
    rv = redisBufferWrite(c, &done);
    if (rv == REDIS_ERR) {
        __redisAsyncDisconnect(ac);
        return;
    }

    if (!done) {
        if (rssl->wantRead) {
            /* Need to read-before-write */
            rssl->pendingWrite = 1;
            _EL_DEL_WRITE(ac);
        } else {
            /* No extra reads needed, just need to write more */
            _EL_ADD_WRITE(ac);
        }
    } else {
        /* Already done! */
        _EL_DEL_WRITE(ac);
    }

    /* Always reschedule a read */
    _EL_ADD_READ(ac);
}

redisContextFuncs redisContextSSLFuncs = {
    .free_privdata = redisSSLFreeContext,
    .async_read = redisSSLAsyncRead,
    .async_write = redisSSLAsyncWrite,
    .read = redisSSLRead,
    .write = redisSSLWrite
};


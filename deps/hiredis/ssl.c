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
#include "net.h"

#include <assert.h>
#include <errno.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#ifdef OPENSSL_IS_BORINGSSL
#undef X509_NAME
#undef X509_EXTENSIONS
#undef PKCS7_ISSUER_AND_SERIAL
#undef PKCS7_SIGNER_INFO
#undef OCSP_REQUEST
#undef OCSP_RESPONSE
#endif
#else
#include <pthread.h>
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "win32.h"
#include "async_private.h"
#include "hiredis_ssl.h"

void __redisSetError(redisContext *c, int type, const char *str);

struct redisSSLContext {
    /* Associated OpenSSL SSL_CTX as created by redisCreateSSLContext() */
    SSL_CTX *ssl_ctx;

    /* Requested SNI, or NULL */
    char *server_name;
};

/* The SSL connection context is attached to SSL/TLS connections as a privdata. */
typedef struct redisSSL {
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
} redisSSL;

/* Forward declaration */
redisContextFuncs redisContextSSLFuncs;

/**
 * OpenSSL global initialization and locking handling callbacks.
 * Note that this is only required for OpenSSL < 1.1.0.
 */

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define HIREDIS_USE_CRYPTO_LOCKS
#endif

#ifdef HIREDIS_USE_CRYPTO_LOCKS
#ifdef _WIN32
typedef CRITICAL_SECTION sslLockType;
static void sslLockInit(sslLockType* l) {
    InitializeCriticalSection(l);
}
static void sslLockAcquire(sslLockType* l) {
    EnterCriticalSection(l);
}
static void sslLockRelease(sslLockType* l) {
    LeaveCriticalSection(l);
}
#else
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
#endif

static sslLockType* ossl_locks;

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

static int initOpensslLocks(void) {
    unsigned ii, nlocks;
    if (CRYPTO_get_locking_callback() != NULL) {
        /* Someone already set the callback before us. Don't destroy it! */
        return REDIS_OK;
    }
    nlocks = CRYPTO_num_locks();
    ossl_locks = hi_malloc(sizeof(*ossl_locks) * nlocks);
    if (ossl_locks == NULL)
        return REDIS_ERR;

    for (ii = 0; ii < nlocks; ii++) {
        sslLockInit(ossl_locks + ii);
    }
    CRYPTO_set_locking_callback(opensslDoLock);
    return REDIS_OK;
}
#endif /* HIREDIS_USE_CRYPTO_LOCKS */

int redisInitOpenSSL(void)
{
    SSL_library_init();
#ifdef HIREDIS_USE_CRYPTO_LOCKS
    initOpensslLocks();
#endif

    return REDIS_OK;
}

/**
 * redisSSLContext helper context destruction.
 */

const char *redisSSLContextGetError(redisSSLContextError error)
{
    switch (error) {
        case REDIS_SSL_CTX_NONE:
            return "No Error";
        case REDIS_SSL_CTX_CREATE_FAILED:
            return "Failed to create OpenSSL SSL_CTX";
        case REDIS_SSL_CTX_CERT_KEY_REQUIRED:
            return "Client cert and key must both be specified or skipped";
        case REDIS_SSL_CTX_CA_CERT_LOAD_FAILED:
            return "Failed to load CA Certificate or CA Path";
        case REDIS_SSL_CTX_CLIENT_CERT_LOAD_FAILED:
            return "Failed to load client certificate";
        case REDIS_SSL_CTX_PRIVATE_KEY_LOAD_FAILED:
            return "Failed to load private key";
        case REDIS_SSL_CTX_OS_CERTSTORE_OPEN_FAILED:
            return "Failed to open system certificate store";
        case REDIS_SSL_CTX_OS_CERT_ADD_FAILED:
            return "Failed to add CA certificates obtained from system to the SSL context";
        default:
            return "Unknown error code";
    }
}

void redisFreeSSLContext(redisSSLContext *ctx)
{
    if (!ctx)
        return;

    if (ctx->server_name) {
        hi_free(ctx->server_name);
        ctx->server_name = NULL;
    }

    if (ctx->ssl_ctx) {
        SSL_CTX_free(ctx->ssl_ctx);
        ctx->ssl_ctx = NULL;
    }

    hi_free(ctx);
}


/**
 * redisSSLContext helper context initialization.
 */

redisSSLContext *redisCreateSSLContext(const char *cacert_filename, const char *capath,
        const char *cert_filename, const char *private_key_filename,
        const char *server_name, redisSSLContextError *error)
{
    redisSSLOptions options = {
        .cacert_filename = cacert_filename,
        .capath = capath,
        .cert_filename = cert_filename,
        .private_key_filename = private_key_filename,
        .server_name = server_name,
        .verify_mode = REDIS_SSL_VERIFY_PEER,
    };

    return redisCreateSSLContextWithOptions(&options, error);
}

redisSSLContext *redisCreateSSLContextWithOptions(redisSSLOptions *options, redisSSLContextError *error) {
    const char *cacert_filename = options->cacert_filename;
    const char *capath = options->capath;
    const char *cert_filename = options->cert_filename;
    const char *private_key_filename = options->private_key_filename;
    const char *server_name = options->server_name;

#ifdef _WIN32
    HCERTSTORE win_store = NULL;
    PCCERT_CONTEXT win_ctx = NULL;
#endif

    redisSSLContext *ctx = hi_calloc(1, sizeof(redisSSLContext));
    if (ctx == NULL)
        goto error;

    ctx->ssl_ctx = SSL_CTX_new(SSLv23_client_method());
    if (!ctx->ssl_ctx) {
        if (error) *error = REDIS_SSL_CTX_CREATE_FAILED;
        goto error;
    }

    SSL_CTX_set_options(ctx->ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
    SSL_CTX_set_verify(ctx->ssl_ctx, options->verify_mode, NULL);

    if ((cert_filename != NULL && private_key_filename == NULL) ||
            (private_key_filename != NULL && cert_filename == NULL)) {
        if (error) *error = REDIS_SSL_CTX_CERT_KEY_REQUIRED;
        goto error;
    }

    if (capath || cacert_filename) {
#ifdef _WIN32
        if (0 == strcmp(cacert_filename, "wincert")) {
            win_store = CertOpenSystemStore(NULL, "Root");
            if (!win_store) {
                if (error) *error = REDIS_SSL_CTX_OS_CERTSTORE_OPEN_FAILED;
                goto error;
            }
            X509_STORE* store = SSL_CTX_get_cert_store(ctx->ssl_ctx);
            while (win_ctx = CertEnumCertificatesInStore(win_store, win_ctx)) {
                X509* x509 = NULL;
                x509 = d2i_X509(NULL, (const unsigned char**)&win_ctx->pbCertEncoded, win_ctx->cbCertEncoded);
                if (x509) {
                    if ((1 != X509_STORE_add_cert(store, x509)) ||
                        (1 != SSL_CTX_add_client_CA(ctx->ssl_ctx, x509)))
                    {
                        if (error) *error = REDIS_SSL_CTX_OS_CERT_ADD_FAILED;
                        goto error;
                    }
                    X509_free(x509);
                }
            }
            CertFreeCertificateContext(win_ctx);
            CertCloseStore(win_store, 0);
        } else
#endif
        if (!SSL_CTX_load_verify_locations(ctx->ssl_ctx, cacert_filename, capath)) {
            if (error) *error = REDIS_SSL_CTX_CA_CERT_LOAD_FAILED;
            goto error;
        }
    } else {
        if (!SSL_CTX_set_default_verify_paths(ctx->ssl_ctx)) {
            if (error) *error = REDIS_SSL_CTX_CLIENT_DEFAULT_CERT_FAILED;
            goto error;
        }
    }

    if (cert_filename) {
        if (!SSL_CTX_use_certificate_chain_file(ctx->ssl_ctx, cert_filename)) {
            if (error) *error = REDIS_SSL_CTX_CLIENT_CERT_LOAD_FAILED;
            goto error;
        }
        if (!SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, private_key_filename, SSL_FILETYPE_PEM)) {
            if (error) *error = REDIS_SSL_CTX_PRIVATE_KEY_LOAD_FAILED;
            goto error;
        }
    }

    if (server_name)
        ctx->server_name = hi_strdup(server_name);

    return ctx;

error:
#ifdef _WIN32
    CertFreeCertificateContext(win_ctx);
    CertCloseStore(win_store, 0);
#endif
    redisFreeSSLContext(ctx);
    return NULL;
}

/**
 * SSL Connection initialization.
 */


static int redisSSLConnect(redisContext *c, SSL *ssl) {
    if (c->privctx) {
        __redisSetError(c, REDIS_ERR_OTHER, "redisContext was already associated");
        return REDIS_ERR;
    }

    redisSSL *rssl = hi_calloc(1, sizeof(redisSSL));
    if (rssl == NULL) {
        __redisSetError(c, REDIS_ERR_OOM, "Out of memory");
        return REDIS_ERR;
    }

    c->funcs = &redisContextSSLFuncs;
    rssl->ssl = ssl;

    SSL_set_mode(rssl->ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_set_fd(rssl->ssl, c->fd);
    SSL_set_connect_state(rssl->ssl);

    ERR_clear_error();
    int rv = SSL_connect(rssl->ssl);
    if (rv == 1) {
        c->privctx = rssl;
        return REDIS_OK;
    }

    rv = SSL_get_error(rssl->ssl, rv);
    if (((c->flags & REDIS_BLOCK) == 0) &&
        (rv == SSL_ERROR_WANT_READ || rv == SSL_ERROR_WANT_WRITE)) {
        c->privctx = rssl;
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

    hi_free(rssl);
    return REDIS_ERR;
}

/**
 * A wrapper around redisSSLConnect() for users who manage their own context and
 * create their own SSL object.
 */

int redisInitiateSSL(redisContext *c, SSL *ssl) {
    return redisSSLConnect(c, ssl);
}

/**
 * A wrapper around redisSSLConnect() for users who use redisSSLContext and don't
 * manage their own SSL objects.
 */

int redisInitiateSSLWithContext(redisContext *c, redisSSLContext *redis_ssl_ctx)
{
    if (!c || !redis_ssl_ctx)
        return REDIS_ERR;

    /* We want to verify that redisSSLConnect() won't fail on this, as it will
     * not own the SSL object in that case and we'll end up leaking.
     */
    if (c->privctx)
        return REDIS_ERR;

    SSL *ssl = SSL_new(redis_ssl_ctx->ssl_ctx);
    if (!ssl) {
        __redisSetError(c, REDIS_ERR_OTHER, "Couldn't create new SSL instance");
        goto error;
    }

    if (redis_ssl_ctx->server_name) {
        if (!SSL_set_tlsext_host_name(ssl, redis_ssl_ctx->server_name)) {
            __redisSetError(c, REDIS_ERR_OTHER, "Failed to set server_name/SNI");
            goto error;
        }
    }

    if (redisSSLConnect(c, ssl) != REDIS_OK) {
        goto error;
    }

    return REDIS_OK;

error:
    if (ssl)
        SSL_free(ssl);
    return REDIS_ERR;
}

static int maybeCheckWant(redisSSL *rssl, int rv) {
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

static void redisSSLFree(void *privctx){
    redisSSL *rsc = privctx;

    if (!rsc) return;
    if (rsc->ssl) {
        SSL_free(rsc->ssl);
        rsc->ssl = NULL;
    }
    hi_free(rsc);
}

static ssize_t redisSSLRead(redisContext *c, char *buf, size_t bufcap) {
    redisSSL *rssl = c->privctx;

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

static ssize_t redisSSLWrite(redisContext *c) {
    redisSSL *rssl = c->privctx;

    size_t len = rssl->lastLen ? rssl->lastLen : hi_sdslen(c->obuf);
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
    redisSSL *rssl = ac->c.privctx;
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
    redisSSL *rssl = ac->c.privctx;
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
    .close = redisNetClose,
    .free_privctx = redisSSLFree,
    .async_read = redisSSLAsyncRead,
    .async_write = redisSSLAsyncWrite,
    .read = redisSSLRead,
    .write = redisSSLWrite
};


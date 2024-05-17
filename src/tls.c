/*
 * Copyright (c) 2019-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#define REDISMODULE_CORE_MODULE /* A module that's part of the redis core, uses server.h too. */

#include "server.h"
#include "connhelpers.h"
#include "adlist.h"

#if (USE_OPENSSL == 1 /* BUILD_YES */ ) || ((USE_OPENSSL == 2 /* BUILD_MODULE */) && (BUILD_TLS_MODULE == 2))

#include <openssl/conf.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/pem.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/decoder.h>
#endif
#include <sys/uio.h>
#include <arpa/inet.h>

#define REDIS_TLS_PROTO_TLSv1       (1<<0)
#define REDIS_TLS_PROTO_TLSv1_1     (1<<1)
#define REDIS_TLS_PROTO_TLSv1_2     (1<<2)
#define REDIS_TLS_PROTO_TLSv1_3     (1<<3)

/* Use safe defaults */
#ifdef TLS1_3_VERSION
#define REDIS_TLS_PROTO_DEFAULT     (REDIS_TLS_PROTO_TLSv1_2|REDIS_TLS_PROTO_TLSv1_3)
#else
#define REDIS_TLS_PROTO_DEFAULT     (REDIS_TLS_PROTO_TLSv1_2)
#endif

SSL_CTX *redis_tls_ctx = NULL;
SSL_CTX *redis_tls_client_ctx = NULL;

static int parseProtocolsConfig(const char *str) {
    int i, count = 0;
    int protocols = 0;

    if (!str) return REDIS_TLS_PROTO_DEFAULT;
    sds *tokens = sdssplitlen(str, strlen(str), " ", 1, &count);

    if (!tokens) { 
        serverLog(LL_WARNING, "Invalid tls-protocols configuration string");
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (!strcasecmp(tokens[i], "tlsv1")) protocols |= REDIS_TLS_PROTO_TLSv1;
        else if (!strcasecmp(tokens[i], "tlsv1.1")) protocols |= REDIS_TLS_PROTO_TLSv1_1;
        else if (!strcasecmp(tokens[i], "tlsv1.2")) protocols |= REDIS_TLS_PROTO_TLSv1_2;
        else if (!strcasecmp(tokens[i], "tlsv1.3")) {
#ifdef TLS1_3_VERSION
            protocols |= REDIS_TLS_PROTO_TLSv1_3;
#else
            serverLog(LL_WARNING, "TLSv1.3 is specified in tls-protocols but not supported by OpenSSL.");
            protocols = -1;
            break;
#endif
        } else {
            serverLog(LL_WARNING, "Invalid tls-protocols specified. "
                    "Use a combination of 'TLSv1', 'TLSv1.1', 'TLSv1.2' and 'TLSv1.3'.");
            protocols = -1;
            break;
        }
    }
    sdsfreesplitres(tokens, count);

    return protocols;
}

/* list of connections with pending data already read from the socket, but not
 * served to the reader yet. */
static list *pending_list = NULL;

/**
 * OpenSSL global initialization and locking handling callbacks.
 * Note that this is only required for OpenSSL < 1.1.0.
 */

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define USE_CRYPTO_LOCKS
#endif

#ifdef USE_CRYPTO_LOCKS

static pthread_mutex_t *openssl_locks;

static void sslLockingCallback(int mode, int lock_id, const char *f, int line) {
    pthread_mutex_t *mt = openssl_locks + lock_id;

    if (mode & CRYPTO_LOCK) {
        pthread_mutex_lock(mt);
    } else {
        pthread_mutex_unlock(mt);
    }

    (void)f;
    (void)line;
}

static void initCryptoLocks(void) {
    unsigned i, nlocks;
    if (CRYPTO_get_locking_callback() != NULL) {
        /* Someone already set the callback before us. Don't destroy it! */
        return;
    }
    nlocks = CRYPTO_num_locks();
    openssl_locks = zmalloc(sizeof(*openssl_locks) * nlocks);
    for (i = 0; i < nlocks; i++) {
        pthread_mutex_init(openssl_locks + i, NULL);
    }
    CRYPTO_set_locking_callback(sslLockingCallback);
}
#endif /* USE_CRYPTO_LOCKS */

static void tlsInit(void) {
    /* Enable configuring OpenSSL using the standard openssl.cnf
     * OPENSSL_config()/OPENSSL_init_crypto() should be the first 
     * call to the OpenSSL* library.
     *  - OPENSSL_config() should be used for OpenSSL versions < 1.1.0
     *  - OPENSSL_init_crypto() should be used for OpenSSL versions >= 1.1.0
     */
    #if OPENSSL_VERSION_NUMBER < 0x10100000L
    OPENSSL_config(NULL);
    SSL_load_error_strings();
    SSL_library_init();
    #elif OPENSSL_VERSION_NUMBER < 0x10101000L
    OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CONFIG, NULL);
    #else
    OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CONFIG|OPENSSL_INIT_ATFORK, NULL);
    #endif

#ifdef USE_CRYPTO_LOCKS
    initCryptoLocks();
#endif

    if (!RAND_poll()) {
        serverLog(LL_WARNING, "OpenSSL: Failed to seed random number generator.");
    }

    pending_list = listCreate();
}

static void tlsCleanup(void) {
    if (redis_tls_ctx) {
        SSL_CTX_free(redis_tls_ctx);
        redis_tls_ctx = NULL;
    }
    if (redis_tls_client_ctx) {
        SSL_CTX_free(redis_tls_client_ctx);
        redis_tls_client_ctx = NULL;
    }

    #if OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(LIBRESSL_VERSION_NUMBER)
    // unavailable on LibreSSL
    OPENSSL_cleanup();
    #endif
}

/* Callback for passing a keyfile password stored as an sds to OpenSSL */
static int tlsPasswordCallback(char *buf, int size, int rwflag, void *u) {
    UNUSED(rwflag);

    const char *pass = u;
    size_t pass_len;

    if (!pass) return -1;
    pass_len = strlen(pass);
    if (pass_len > (size_t) size) return -1;
    memcpy(buf, pass, pass_len);

    return (int) pass_len;
}

/* Create a *base* SSL_CTX using the SSL configuration provided. The base context
 * includes everything that's common for both client-side and server-side connections.
 */
static SSL_CTX *createSSLContext(redisTLSContextConfig *ctx_config, int protocols, int client) {
    const char *cert_file = client ? ctx_config->client_cert_file : ctx_config->cert_file;
    const char *key_file = client ? ctx_config->client_key_file : ctx_config->key_file;
    const char *key_file_pass = client ? ctx_config->client_key_file_pass : ctx_config->key_file_pass;
    char errbuf[256];
    SSL_CTX *ctx = NULL;

    ctx = SSL_CTX_new(SSLv23_method());
    if (!ctx) goto error;

    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3);

#ifdef SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS
    SSL_CTX_set_options(ctx, SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);
#endif

    if (!(protocols & REDIS_TLS_PROTO_TLSv1))
        SSL_CTX_set_options(ctx, SSL_OP_NO_TLSv1);
    if (!(protocols & REDIS_TLS_PROTO_TLSv1_1))
        SSL_CTX_set_options(ctx, SSL_OP_NO_TLSv1_1);
#ifdef SSL_OP_NO_TLSv1_2
    if (!(protocols & REDIS_TLS_PROTO_TLSv1_2))
        SSL_CTX_set_options(ctx, SSL_OP_NO_TLSv1_2);
#endif
#ifdef SSL_OP_NO_TLSv1_3
    if (!(protocols & REDIS_TLS_PROTO_TLSv1_3))
        SSL_CTX_set_options(ctx, SSL_OP_NO_TLSv1_3);
#endif

#ifdef SSL_OP_NO_COMPRESSION
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
#endif

    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE|SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);

    SSL_CTX_set_default_passwd_cb(ctx, tlsPasswordCallback);
    SSL_CTX_set_default_passwd_cb_userdata(ctx, (void *) key_file_pass);

    if (SSL_CTX_use_certificate_chain_file(ctx, cert_file) <= 0) {
        ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
        serverLog(LL_WARNING, "Failed to load certificate: %s: %s", cert_file, errbuf);
        goto error;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) <= 0) {
        ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
        serverLog(LL_WARNING, "Failed to load private key: %s: %s", key_file, errbuf);
        goto error;
    }

    if ((ctx_config->ca_cert_file || ctx_config->ca_cert_dir) &&
        SSL_CTX_load_verify_locations(ctx, ctx_config->ca_cert_file, ctx_config->ca_cert_dir) <= 0) {
        ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
        serverLog(LL_WARNING, "Failed to configure CA certificate(s) file/directory: %s", errbuf);
        goto error;
    }

    if (ctx_config->ciphers && !SSL_CTX_set_cipher_list(ctx, ctx_config->ciphers)) {
        serverLog(LL_WARNING, "Failed to configure ciphers: %s", ctx_config->ciphers);
        goto error;
    }

#ifdef TLS1_3_VERSION
    if (ctx_config->ciphersuites && !SSL_CTX_set_ciphersuites(ctx, ctx_config->ciphersuites)) {
        serverLog(LL_WARNING, "Failed to configure ciphersuites: %s", ctx_config->ciphersuites);
        goto error;
    }
#endif

    return ctx;

error:
    if (ctx) SSL_CTX_free(ctx);
    return NULL;
}

/* Attempt to configure/reconfigure TLS. This operation is atomic and will
 * leave the SSL_CTX unchanged if fails.
 * @priv: config of redisTLSContextConfig.
 * @reconfigure: if true, ignore the previous configure; if false, only
 *               configure from @ctx_config if redis_tls_ctx is NULL.
 */
static int tlsConfigure(void *priv, int reconfigure) {
    redisTLSContextConfig *ctx_config = (redisTLSContextConfig *)priv;
    char errbuf[256];
    SSL_CTX *ctx = NULL;
    SSL_CTX *client_ctx = NULL;

    if (!reconfigure && redis_tls_ctx) {
        return C_OK;
    }

    if (!ctx_config->cert_file) {
        serverLog(LL_WARNING, "No tls-cert-file configured!");
        goto error;
    }

    if (!ctx_config->key_file) {
        serverLog(LL_WARNING, "No tls-key-file configured!");
        goto error;
    }

    if (((server.tls_auth_clients != TLS_CLIENT_AUTH_NO) || server.tls_cluster || server.tls_replication) &&
            !ctx_config->ca_cert_file && !ctx_config->ca_cert_dir) {
        serverLog(LL_WARNING, "Either tls-ca-cert-file or tls-ca-cert-dir must be specified when tls-cluster, tls-replication or tls-auth-clients are enabled!");
        goto error;
    }

    int protocols = parseProtocolsConfig(ctx_config->protocols);
    if (protocols == -1) goto error;

    /* Create server side/general context */
    ctx = createSSLContext(ctx_config, protocols, 0);
    if (!ctx) goto error;

    if (ctx_config->session_caching) {
        SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
        SSL_CTX_sess_set_cache_size(ctx, ctx_config->session_cache_size);
        SSL_CTX_set_timeout(ctx, ctx_config->session_cache_timeout);
        SSL_CTX_set_session_id_context(ctx, (void *) "redis", 5);
    } else {
        SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    }

#ifdef SSL_OP_NO_CLIENT_RENEGOTIATION
    SSL_CTX_set_options(ctx, SSL_OP_NO_CLIENT_RENEGOTIATION);
#endif

    if (ctx_config->prefer_server_ciphers)
        SSL_CTX_set_options(ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);

#if ((OPENSSL_VERSION_NUMBER < 0x30000000L) && defined(SSL_CTX_set_ecdh_auto))
    SSL_CTX_set_ecdh_auto(ctx, 1);
#endif
    SSL_CTX_set_options(ctx, SSL_OP_SINGLE_DH_USE);

    if (ctx_config->dh_params_file) {
        FILE *dhfile = fopen(ctx_config->dh_params_file, "r");
        if (!dhfile) {
            serverLog(LL_WARNING, "Failed to load %s: %s", ctx_config->dh_params_file, strerror(errno));
            goto error;
        }

#if (OPENSSL_VERSION_NUMBER >= 0x30000000L)
        EVP_PKEY *pkey = NULL;
        OSSL_DECODER_CTX *dctx = OSSL_DECODER_CTX_new_for_pkey(
                &pkey, "PEM", NULL, "DH", OSSL_KEYMGMT_SELECT_DOMAIN_PARAMETERS, NULL, NULL);
        if (!dctx) {
            serverLog(LL_WARNING, "No decoder for DH params.");
            fclose(dhfile);
            goto error;
        }
        if (!OSSL_DECODER_from_fp(dctx, dhfile)) {
            serverLog(LL_WARNING, "%s: failed to read DH params.", ctx_config->dh_params_file);
            OSSL_DECODER_CTX_free(dctx);
            fclose(dhfile);
            goto error;
        }

        OSSL_DECODER_CTX_free(dctx);
        fclose(dhfile);

        if (SSL_CTX_set0_tmp_dh_pkey(ctx, pkey) <= 0) {
            ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
            serverLog(LL_WARNING, "Failed to load DH params file: %s: %s", ctx_config->dh_params_file, errbuf);
            EVP_PKEY_free(pkey);
            goto error;
        }
        /* Not freeing pkey, it is owned by OpenSSL now */
#else
        DH *dh = PEM_read_DHparams(dhfile, NULL, NULL, NULL);
        fclose(dhfile);
        if (!dh) {
            serverLog(LL_WARNING, "%s: failed to read DH params.", ctx_config->dh_params_file);
            goto error;
        }

        if (SSL_CTX_set_tmp_dh(ctx, dh) <= 0) {
            ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
            serverLog(LL_WARNING, "Failed to load DH params file: %s: %s", ctx_config->dh_params_file, errbuf);
            DH_free(dh);
            goto error;
        }

        DH_free(dh);
#endif
    } else {
#if (OPENSSL_VERSION_NUMBER >= 0x30000000L)
        SSL_CTX_set_dh_auto(ctx, 1);
#endif
    }

    /* If a client-side certificate is configured, create an explicit client context */
    if (ctx_config->client_cert_file && ctx_config->client_key_file) {
        client_ctx = createSSLContext(ctx_config, protocols, 1);
        if (!client_ctx) goto error;
    }

    SSL_CTX_free(redis_tls_ctx);
    SSL_CTX_free(redis_tls_client_ctx);
    redis_tls_ctx = ctx;
    redis_tls_client_ctx = client_ctx;

    return C_OK;

error:
    if (ctx) SSL_CTX_free(ctx);
    if (client_ctx) SSL_CTX_free(client_ctx);
    return C_ERR;
}

#ifdef TLS_DEBUGGING
#define TLSCONN_DEBUG(fmt, ...) \
    serverLog(LL_DEBUG, "TLSCONN: " fmt, __VA_ARGS__)
#else
#define TLSCONN_DEBUG(fmt, ...)
#endif

static ConnectionType CT_TLS;

/* Normal socket connections have a simple events/handler correlation.
 *
 * With TLS connections we need to handle cases where during a logical read
 * or write operation, the SSL library asks to block for the opposite
 * socket operation.
 *
 * When this happens, we need to do two things:
 * 1. Make sure we register for the event.
 * 2. Make sure we know which handler needs to execute when the
 *    event fires.  That is, if we notify the caller of a write operation
 *    that it blocks, and SSL asks for a read, we need to trigger the
 *    write handler again on the next read event.
 *
 */

typedef enum {
    WANT_READ = 1,
    WANT_WRITE
} WantIOType;

#define TLS_CONN_FLAG_READ_WANT_WRITE   (1<<0)
#define TLS_CONN_FLAG_WRITE_WANT_READ   (1<<1)
#define TLS_CONN_FLAG_FD_SET            (1<<2)

typedef struct tls_connection {
    connection c;
    int flags;
    SSL *ssl;
    char *ssl_error;
    listNode *pending_list_node;
} tls_connection;

static connection *createTLSConnection(int client_side) {
    SSL_CTX *ctx = redis_tls_ctx;
    if (client_side && redis_tls_client_ctx)
        ctx = redis_tls_client_ctx;
    tls_connection *conn = zcalloc(sizeof(tls_connection));
    conn->c.type = &CT_TLS;
    conn->c.fd = -1;
    conn->c.iovcnt = IOV_MAX;
    conn->ssl = SSL_new(ctx);
    return (connection *) conn;
}

static connection *connCreateTLS(void) {
    return createTLSConnection(1);
}

/* Fetch the latest OpenSSL error and store it in the connection */
static void updateTLSError(tls_connection *conn) {
    conn->c.last_errno = 0;
    if (conn->ssl_error) zfree(conn->ssl_error);
    conn->ssl_error = zmalloc(512);
    ERR_error_string_n(ERR_get_error(), conn->ssl_error, 512);
}

/* Create a new TLS connection that is already associated with
 * an accepted underlying file descriptor.
 *
 * The socket is not ready for I/O until connAccept() was called and
 * invoked the connection-level accept handler.
 *
 * Callers should use connGetState() and verify the created connection
 * is not in an error state.
 */
static connection *connCreateAcceptedTLS(int fd, void *priv) {
    int require_auth = *(int *)priv;
    tls_connection *conn = (tls_connection *) createTLSConnection(0);
    conn->c.fd = fd;
    conn->c.state = CONN_STATE_ACCEPTING;

    if (!conn->ssl) {
        updateTLSError(conn);
        conn->c.state = CONN_STATE_ERROR;
        return (connection *) conn;
    }

    switch (require_auth) {
        case TLS_CLIENT_AUTH_NO:
            SSL_set_verify(conn->ssl, SSL_VERIFY_NONE, NULL);
            break;
        case TLS_CLIENT_AUTH_OPTIONAL:
            SSL_set_verify(conn->ssl, SSL_VERIFY_PEER, NULL);
            break;
        default: /* TLS_CLIENT_AUTH_YES, also fall-secure */
            SSL_set_verify(conn->ssl, SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
            break;
    }

    SSL_set_fd(conn->ssl, conn->c.fd);
    SSL_set_accept_state(conn->ssl);

    return (connection *) conn;
}

static void tlsEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask);
static void updateSSLEvent(tls_connection *conn);

/* Process the return code received from OpenSSL>
 * Update the want parameter with expected I/O.
 * Update the connection's error state if a real error has occurred.
 * Returns an SSL error code, or 0 if no further handling is required.
 */
static int handleSSLReturnCode(tls_connection *conn, int ret_value, WantIOType *want) {
    if (ret_value <= 0) {
        int ssl_err = SSL_get_error(conn->ssl, ret_value);
        switch (ssl_err) {
            case SSL_ERROR_WANT_WRITE:
                *want = WANT_WRITE;
                return 0;
            case SSL_ERROR_WANT_READ:
                *want = WANT_READ;
                return 0;
            case SSL_ERROR_SYSCALL:
                conn->c.last_errno = errno;
                if (conn->ssl_error) zfree(conn->ssl_error);
                conn->ssl_error = errno ? zstrdup(strerror(errno)) : NULL;
                break;
            default:
                /* Error! */
                updateTLSError(conn);
                break;
        }

        return ssl_err;
    }

    return 0;
}

/* Handle OpenSSL return code following SSL_write() or SSL_read():
 *
 * - Updates conn state and last_errno.
 * - If update_event is nonzero, calls updateSSLEvent() when necessary.
 *
 * Returns ret_value, or -1 on error or dropped connection.
 */
static int updateStateAfterSSLIO(tls_connection *conn, int ret_value, int update_event) {
    /* If system call was interrupted, there's no need to go through the full
     * OpenSSL error handling and just report this for the caller to retry the
     * operation.
     */
    if (errno == EINTR) {
        conn->c.last_errno = EINTR;
        return -1;
    }

    if (ret_value <= 0) {
        WantIOType want = 0;
        int ssl_err;
        if (!(ssl_err = handleSSLReturnCode(conn, ret_value, &want))) {
            if (want == WANT_READ) conn->flags |= TLS_CONN_FLAG_WRITE_WANT_READ;
            if (want == WANT_WRITE) conn->flags |= TLS_CONN_FLAG_READ_WANT_WRITE;
            if (update_event) updateSSLEvent(conn);
            errno = EAGAIN;
            return -1;
        } else {
            if (ssl_err == SSL_ERROR_ZERO_RETURN ||
                ((ssl_err == SSL_ERROR_SYSCALL && !errno))) {
                conn->c.state = CONN_STATE_CLOSED;
                return -1;
            } else {
                conn->c.state = CONN_STATE_ERROR;
                return -1;
            }
        }
    }

    return ret_value;
}

static void registerSSLEvent(tls_connection *conn, WantIOType want) {
    int mask = aeGetFileEvents(server.el, conn->c.fd);

    switch (want) {
        case WANT_READ:
            if (mask & AE_WRITABLE) aeDeleteFileEvent(server.el, conn->c.fd, AE_WRITABLE);
            if (!(mask & AE_READABLE)) aeCreateFileEvent(server.el, conn->c.fd, AE_READABLE,
                        tlsEventHandler, conn);
            break;
        case WANT_WRITE:
            if (mask & AE_READABLE) aeDeleteFileEvent(server.el, conn->c.fd, AE_READABLE);
            if (!(mask & AE_WRITABLE)) aeCreateFileEvent(server.el, conn->c.fd, AE_WRITABLE,
                        tlsEventHandler, conn);
            break;
        default:
            serverAssert(0);
            break;
    }
}

static void updateSSLEvent(tls_connection *conn) {
    int mask = aeGetFileEvents(server.el, conn->c.fd);
    int need_read = conn->c.read_handler || (conn->flags & TLS_CONN_FLAG_WRITE_WANT_READ);
    int need_write = conn->c.write_handler || (conn->flags & TLS_CONN_FLAG_READ_WANT_WRITE);

    if (need_read && !(mask & AE_READABLE))
        aeCreateFileEvent(server.el, conn->c.fd, AE_READABLE, tlsEventHandler, conn);
    if (!need_read && (mask & AE_READABLE))
        aeDeleteFileEvent(server.el, conn->c.fd, AE_READABLE);

    if (need_write && !(mask & AE_WRITABLE))
        aeCreateFileEvent(server.el, conn->c.fd, AE_WRITABLE, tlsEventHandler, conn);
    if (!need_write && (mask & AE_WRITABLE))
        aeDeleteFileEvent(server.el, conn->c.fd, AE_WRITABLE);
}

static void tlsHandleEvent(tls_connection *conn, int mask) {
    int ret, conn_error;

    TLSCONN_DEBUG("tlsEventHandler(): fd=%d, state=%d, mask=%d, r=%d, w=%d, flags=%d",
            fd, conn->c.state, mask, conn->c.read_handler != NULL, conn->c.write_handler != NULL,
            conn->flags);

    ERR_clear_error();

    switch (conn->c.state) {
        case CONN_STATE_CONNECTING:
            conn_error = anetGetError(conn->c.fd);
            if (conn_error) {
                conn->c.last_errno = conn_error;
                conn->c.state = CONN_STATE_ERROR;
            } else {
                if (!(conn->flags & TLS_CONN_FLAG_FD_SET)) {
                    SSL_set_fd(conn->ssl, conn->c.fd);
                    conn->flags |= TLS_CONN_FLAG_FD_SET;
                }
                ret = SSL_connect(conn->ssl);
                if (ret <= 0) {
                    WantIOType want = 0;
                    if (!handleSSLReturnCode(conn, ret, &want)) {
                        registerSSLEvent(conn, want);

                        /* Avoid hitting UpdateSSLEvent, which knows nothing
                         * of what SSL_connect() wants and instead looks at our
                         * R/W handlers.
                         */
                        return;
                    }

                    /* If not handled, it's an error */
                    conn->c.state = CONN_STATE_ERROR;
                } else {
                    conn->c.state = CONN_STATE_CONNECTED;
                }
            }

            if (!callHandler((connection *) conn, conn->c.conn_handler)) return;
            conn->c.conn_handler = NULL;
            break;
        case CONN_STATE_ACCEPTING:
            ret = SSL_accept(conn->ssl);
            if (ret <= 0) {
                WantIOType want = 0;
                if (!handleSSLReturnCode(conn, ret, &want)) {
                    /* Avoid hitting UpdateSSLEvent, which knows nothing
                     * of what SSL_connect() wants and instead looks at our
                     * R/W handlers.
                     */
                    registerSSLEvent(conn, want);
                    return;
                }

                /* If not handled, it's an error */
                conn->c.state = CONN_STATE_ERROR;
            } else {
                conn->c.state = CONN_STATE_CONNECTED;
            }

            if (!callHandler((connection *) conn, conn->c.conn_handler)) return;
            conn->c.conn_handler = NULL;
            break;
        case CONN_STATE_CONNECTED:
        {
            int call_read = ((mask & AE_READABLE) && conn->c.read_handler) ||
                ((mask & AE_WRITABLE) && (conn->flags & TLS_CONN_FLAG_READ_WANT_WRITE));
            int call_write = ((mask & AE_WRITABLE) && conn->c.write_handler) ||
                ((mask & AE_READABLE) && (conn->flags & TLS_CONN_FLAG_WRITE_WANT_READ));

            /* Normally we execute the readable event first, and the writable
             * event laster. This is useful as sometimes we may be able
             * to serve the reply of a query immediately after processing the
             * query.
             *
             * However if WRITE_BARRIER is set in the mask, our application is
             * asking us to do the reverse: never fire the writable event
             * after the readable. In such a case, we invert the calls.
             * This is useful when, for instance, we want to do things
             * in the beforeSleep() hook, like fsynching a file to disk,
             * before replying to a client. */
            int invert = conn->c.flags & CONN_FLAG_WRITE_BARRIER;

            if (!invert && call_read) {
                conn->flags &= ~TLS_CONN_FLAG_READ_WANT_WRITE;
                if (!callHandler((connection *) conn, conn->c.read_handler)) return;
            }

            /* Fire the writable event. */
            if (call_write) {
                conn->flags &= ~TLS_CONN_FLAG_WRITE_WANT_READ;
                if (!callHandler((connection *) conn, conn->c.write_handler)) return;
            }

            /* If we have to invert the call, fire the readable event now
             * after the writable one. */
            if (invert && call_read) {
                conn->flags &= ~TLS_CONN_FLAG_READ_WANT_WRITE;
                if (!callHandler((connection *) conn, conn->c.read_handler)) return;
            }

            /* If SSL has pending that, already read from the socket, we're at
             * risk of not calling the read handler again, make sure to add it
             * to a list of pending connection that should be handled anyway. */
            if ((mask & AE_READABLE)) {
                if (SSL_pending(conn->ssl) > 0) {
                    if (!conn->pending_list_node) {
                        listAddNodeTail(pending_list, conn);
                        conn->pending_list_node = listLast(pending_list);
                    }
                } else if (conn->pending_list_node) {
                    listDelNode(pending_list, conn->pending_list_node);
                    conn->pending_list_node = NULL;
                }
            }

            break;
        }
        default:
            break;
    }

    updateSSLEvent(conn);
}

static void tlsEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask) {
    UNUSED(el);
    UNUSED(fd);
    tls_connection *conn = clientData;
    tlsHandleEvent(conn, mask);
}

static void tlsAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    int max = server.max_new_tls_conns_per_cycle;
    char cip[NET_IP_STR_LEN];
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE,"Accepted %s:%d", cip, cport);
        acceptCommonHandler(connCreateAcceptedTLS(cfd, &server.tls_auth_clients),0,cip);
    }
}

static int connTLSAddr(connection *conn, char *ip, size_t ip_len, int *port, int remote) {
    return anetFdToString(conn->fd, ip, ip_len, port, remote);
}

static int connTLSIsLocal(connection *conn) {
    return connectionTypeTcp()->is_local(conn);
}

static int connTLSListen(connListener *listener) {
    return listenToPort(listener);
}

static void connTLSShutdown(connection *conn_) {
    tls_connection *conn = (tls_connection *) conn_;

    if (conn->ssl) {
        if (conn->c.state == CONN_STATE_CONNECTED)
            SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }

    connectionTypeTcp()->shutdown(conn_);
}

static void connTLSClose(connection *conn_) {
    tls_connection *conn = (tls_connection *) conn_;

    if (conn->ssl) {
        if (conn->c.state == CONN_STATE_CONNECTED)
            SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }

    if (conn->ssl_error) {
        zfree(conn->ssl_error);
        conn->ssl_error = NULL;
    }

    if (conn->pending_list_node) {
        listDelNode(pending_list, conn->pending_list_node);
        conn->pending_list_node = NULL;
    }

    connectionTypeTcp()->close(conn_);
}

static int connTLSAccept(connection *_conn, ConnectionCallbackFunc accept_handler) {
    tls_connection *conn = (tls_connection *) _conn;
    int ret;

    if (conn->c.state != CONN_STATE_ACCEPTING) return C_ERR;
    ERR_clear_error();

    /* Try to accept */
    conn->c.conn_handler = accept_handler;
    ret = SSL_accept(conn->ssl);

    if (ret <= 0) {
        WantIOType want = 0;
        if (!handleSSLReturnCode(conn, ret, &want)) {
            registerSSLEvent(conn, want);   /* We'll fire back */
            return C_OK;
        } else {
            conn->c.state = CONN_STATE_ERROR;
            return C_ERR;
        }
    }

    conn->c.state = CONN_STATE_CONNECTED;
    if (!callHandler((connection *) conn, conn->c.conn_handler)) return C_OK;
    conn->c.conn_handler = NULL;

    return C_OK;
}

static int connTLSConnect(connection *conn_, const char *addr, int port, const char *src_addr, ConnectionCallbackFunc connect_handler) {
    tls_connection *conn = (tls_connection *) conn_;
    unsigned char addr_buf[sizeof(struct in6_addr)];

    if (conn->c.state != CONN_STATE_NONE) return C_ERR;
    ERR_clear_error();

    /* Check whether addr is an IP address, if not, use the value for Server Name Indication */
    if (inet_pton(AF_INET, addr, addr_buf) != 1 && inet_pton(AF_INET6, addr, addr_buf) != 1) {
        SSL_set_tlsext_host_name(conn->ssl, addr);
    }

    /* Initiate Socket connection first */
    if (connectionTypeTcp()->connect(conn_, addr, port, src_addr, connect_handler) == C_ERR) return C_ERR;

    /* Return now, once the socket is connected we'll initiate
     * TLS connection from the event handler.
     */
    return C_OK;
}

static int connTLSWrite(connection *conn_, const void *data, size_t data_len) {
    tls_connection *conn = (tls_connection *) conn_;
    int ret;

    if (conn->c.state != CONN_STATE_CONNECTED) return -1;
    ERR_clear_error();
    ret = SSL_write(conn->ssl, data, data_len);
    return updateStateAfterSSLIO(conn, ret, 1);
}

static int connTLSWritev(connection *conn_, const struct iovec *iov, int iovcnt) {
    if (iovcnt == 1) return connTLSWrite(conn_, iov[0].iov_base, iov[0].iov_len);

    /* Accumulate the amount of bytes of each buffer and check if it exceeds NET_MAX_WRITES_PER_EVENT. */
    size_t iov_bytes_len = 0;
    for (int i = 0; i < iovcnt; i++) {
        iov_bytes_len += iov[i].iov_len;
        if (iov_bytes_len > NET_MAX_WRITES_PER_EVENT) break;
    }

    /* The amount of all buffers is greater than NET_MAX_WRITES_PER_EVENT, 
     * which is not worth doing so much memory copying to reduce system calls,
     * therefore, invoke connTLSWrite() multiple times to avoid memory copies. */
    if (iov_bytes_len > NET_MAX_WRITES_PER_EVENT) {
        ssize_t tot_sent = 0;
        for (int i = 0; i < iovcnt; i++) {
            ssize_t sent = connTLSWrite(conn_, iov[i].iov_base, iov[i].iov_len);
            if (sent <= 0) return tot_sent > 0 ? tot_sent : sent;
            tot_sent += sent;
            if ((size_t) sent != iov[i].iov_len) break;
        }
        return tot_sent;
    }

    /* The amount of all buffers is less than NET_MAX_WRITES_PER_EVENT, 
     * which is worth doing more memory copies in exchange for fewer system calls, 
     * so concatenate these scattered buffers into a contiguous piece of memory 
     * and send it away by one call to connTLSWrite(). */
    char buf[iov_bytes_len];
    size_t offset = 0;
    for (int i = 0; i < iovcnt; i++) {
        memcpy(buf + offset, iov[i].iov_base, iov[i].iov_len);
        offset += iov[i].iov_len;
    }
    return connTLSWrite(conn_, buf, iov_bytes_len);
}

static int connTLSRead(connection *conn_, void *buf, size_t buf_len) {
    tls_connection *conn = (tls_connection *) conn_;
    int ret;

    if (conn->c.state != CONN_STATE_CONNECTED) return -1;
    ERR_clear_error();
    ret = SSL_read(conn->ssl, buf, buf_len);
    return updateStateAfterSSLIO(conn, ret, 1);
}

static const char *connTLSGetLastError(connection *conn_) {
    tls_connection *conn = (tls_connection *) conn_;

    if (conn->ssl_error) return conn->ssl_error;
    return NULL;
}

static int connTLSSetWriteHandler(connection *conn, ConnectionCallbackFunc func, int barrier) {
    conn->write_handler = func;
    if (barrier)
        conn->flags |= CONN_FLAG_WRITE_BARRIER;
    else
        conn->flags &= ~CONN_FLAG_WRITE_BARRIER;
    updateSSLEvent((tls_connection *) conn);
    return C_OK;
}

static int connTLSSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    conn->read_handler = func;
    updateSSLEvent((tls_connection *) conn);
    return C_OK;
}

static void setBlockingTimeout(tls_connection *conn, long long timeout) {
    anetBlock(NULL, conn->c.fd);
    anetSendTimeout(NULL, conn->c.fd, timeout);
    anetRecvTimeout(NULL, conn->c.fd, timeout);
}

static void unsetBlockingTimeout(tls_connection *conn) {
    anetNonBlock(NULL, conn->c.fd);
    anetSendTimeout(NULL, conn->c.fd, 0);
    anetRecvTimeout(NULL, conn->c.fd, 0);
}

static int connTLSBlockingConnect(connection *conn_, const char *addr, int port, long long timeout) {
    tls_connection *conn = (tls_connection *) conn_;
    int ret;

    if (conn->c.state != CONN_STATE_NONE) return C_ERR;

    /* Initiate socket blocking connect first */
    if (connectionTypeTcp()->blocking_connect(conn_, addr, port, timeout) == C_ERR) return C_ERR;

    /* Initiate TLS connection now.  We set up a send/recv timeout on the socket,
     * which means the specified timeout will not be enforced accurately. */
    SSL_set_fd(conn->ssl, conn->c.fd);
    setBlockingTimeout(conn, timeout);

    if ((ret = SSL_connect(conn->ssl)) <= 0) {
        conn->c.state = CONN_STATE_ERROR;
        return C_ERR;
    }
    unsetBlockingTimeout(conn);

    conn->c.state = CONN_STATE_CONNECTED;
    return C_OK;
}

static ssize_t connTLSSyncWrite(connection *conn_, char *ptr, ssize_t size, long long timeout) {
    tls_connection *conn = (tls_connection *) conn_;

    setBlockingTimeout(conn, timeout);
    SSL_clear_mode(conn->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);
    ERR_clear_error();
    int ret = SSL_write(conn->ssl, ptr, size);
    ret = updateStateAfterSSLIO(conn, ret, 0);
    SSL_set_mode(conn->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);
    unsetBlockingTimeout(conn);

    return ret;
}

static ssize_t connTLSSyncRead(connection *conn_, char *ptr, ssize_t size, long long timeout) {
    tls_connection *conn = (tls_connection *) conn_;

    setBlockingTimeout(conn, timeout);
    ERR_clear_error();
    int ret = SSL_read(conn->ssl, ptr, size);
    ret = updateStateAfterSSLIO(conn, ret, 0);
    unsetBlockingTimeout(conn);

    return ret;
}

static ssize_t connTLSSyncReadLine(connection *conn_, char *ptr, ssize_t size, long long timeout) {
    tls_connection *conn = (tls_connection *) conn_;
    ssize_t nread = 0;

    setBlockingTimeout(conn, timeout);

    size--;
    while(size) {
        char c;

        ERR_clear_error();
        int ret = SSL_read(conn->ssl, &c, 1);
        ret = updateStateAfterSSLIO(conn, ret, 0);
        if (ret <= 0) {
            nread = -1;
            goto exit;
        }
        if (c == '\n') {
            *ptr = '\0';
            if (nread && *(ptr-1) == '\r') *(ptr-1) = '\0';
            goto exit;
        } else {
            *ptr++ = c;
            *ptr = '\0';
            nread++;
        }
        size--;
    }
exit:
    unsetBlockingTimeout(conn);
    return nread;
}

static const char *connTLSGetType(connection *conn_) {
    (void) conn_;

    return CONN_TYPE_TLS;
}

static int tlsHasPendingData(void) {
    if (!pending_list)
        return 0;
    return listLength(pending_list) > 0;
}

static int tlsProcessPendingData(void) {
    listIter li;
    listNode *ln;

    int processed = listLength(pending_list);
    listRewind(pending_list,&li);
    while((ln = listNext(&li))) {
        tls_connection *conn = listNodeValue(ln);
        tlsHandleEvent(conn, AE_READABLE);
    }
    return processed;
}

/* Fetch the peer certificate used for authentication on the specified
 * connection and return it as a PEM-encoded sds.
 */
static sds connTLSGetPeerCert(connection *conn_) {
    tls_connection *conn = (tls_connection *) conn_;
    if ((conn_->type != connectionTypeTls()) || !conn->ssl) return NULL;

    X509 *cert = SSL_get_peer_certificate(conn->ssl);
    if (!cert) return NULL;

    BIO *bio = BIO_new(BIO_s_mem());
    if (bio == NULL || !PEM_write_bio_X509(bio, cert)) {
        if (bio != NULL) BIO_free(bio);
        return NULL;
    }

    const char *bio_ptr;
    long long bio_len = BIO_get_mem_data(bio, &bio_ptr);
    sds cert_pem = sdsnewlen(bio_ptr, bio_len);
    BIO_free(bio);

    return cert_pem;
}

static ConnectionType CT_TLS = {
    /* connection type */
    .get_type = connTLSGetType,

    /* connection type initialize & finalize & configure */
    .init = tlsInit,
    .cleanup = tlsCleanup,
    .configure = tlsConfigure,

    /* ae & accept & listen & error & address handler */
    .ae_handler = tlsEventHandler,
    .accept_handler = tlsAcceptHandler,
    .addr = connTLSAddr,
    .is_local = connTLSIsLocal,
    .listen = connTLSListen,

    /* create/shutdown/close connection */
    .conn_create = connCreateTLS,
    .conn_create_accepted = connCreateAcceptedTLS,
    .shutdown = connTLSShutdown,
    .close = connTLSClose,

    /* connect & accept */
    .connect = connTLSConnect,
    .blocking_connect = connTLSBlockingConnect,
    .accept = connTLSAccept,

    /* IO */
    .read = connTLSRead,
    .write = connTLSWrite,
    .writev = connTLSWritev,
    .set_write_handler = connTLSSetWriteHandler,
    .set_read_handler = connTLSSetReadHandler,
    .get_last_error = connTLSGetLastError,
    .sync_write = connTLSSyncWrite,
    .sync_read = connTLSSyncRead,
    .sync_readline = connTLSSyncReadLine,

    /* pending data */
    .has_pending_data = tlsHasPendingData,
    .process_pending_data = tlsProcessPendingData,

    /* TLS specified methods */
    .get_peer_cert = connTLSGetPeerCert,
};

int RedisRegisterConnectionTypeTLS(void) {
    return connTypeRegister(&CT_TLS);
}

#else   /* USE_OPENSSL */

int RedisRegisterConnectionTypeTLS(void) {
    serverLog(LL_VERBOSE, "Connection type %s not builtin", CONN_TYPE_TLS);
    return C_ERR;
}

#endif

#if BUILD_TLS_MODULE == 2 /* BUILD_MODULE */

#include "release.h"

int RedisModule_OnLoad(void *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    /* Connection modules must be part of the same build as redis. */
    if (strcmp(REDIS_BUILD_ID_RAW, redisBuildIdRaw())) {
        serverLog(LL_NOTICE, "Connection type %s was not built together with the redis-server used.", CONN_TYPE_TLS);
        return REDISMODULE_ERR;
    }

    if (RedisModule_Init(ctx,"tls",1,REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* Connection modules is available only bootup. */
    if ((RedisModule_GetContextFlags(ctx) & REDISMODULE_CTX_FLAGS_SERVER_STARTUP) == 0) {
        serverLog(LL_NOTICE, "Connection type %s can be loaded only during bootup", CONN_TYPE_TLS);
        return REDISMODULE_ERR;
    }

    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD);

    if(connTypeRegister(&CT_TLS) != C_OK)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(void *arg) {
    UNUSED(arg);
    serverLog(LL_NOTICE, "Connection type %s can not be unloaded", CONN_TYPE_TLS);
    return REDISMODULE_ERR;
}
#endif

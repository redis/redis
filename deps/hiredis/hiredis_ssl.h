
/*
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

#ifndef __HIREDIS_SSL_H
#define __HIREDIS_SSL_H

#ifdef __cplusplus
extern "C" {
#endif

/* This is the underlying struct for SSL in ssl.h, which is not included to
 * keep build dependencies short here.
 */
struct ssl_st;

/* A wrapper around OpenSSL SSL_CTX to allow easy SSL use without directly
 * calling OpenSSL.
 */
typedef struct redisSSLContext redisSSLContext;

/**
 * Initialization errors that redisCreateSSLContext() may return.
 */

typedef enum {
    REDIS_SSL_CTX_NONE = 0,                     /* No Error */
    REDIS_SSL_CTX_CREATE_FAILED,                /* Failed to create OpenSSL SSL_CTX */
    REDIS_SSL_CTX_CERT_KEY_REQUIRED,            /* Client cert and key must both be specified or skipped */
    REDIS_SSL_CTX_CA_CERT_LOAD_FAILED,          /* Failed to load CA Certificate or CA Path */
    REDIS_SSL_CTX_CLIENT_CERT_LOAD_FAILED,      /* Failed to load client certificate */
    REDIS_SSL_CTX_PRIVATE_KEY_LOAD_FAILED       /* Failed to load private key */
} redisSSLContextError;

/**
 * Return the error message corresponding with the specified error code.
 */

const char *redisSSLContextGetError(redisSSLContextError error);

/**
 * Helper function to initialize the OpenSSL library.
 *
 * OpenSSL requires one-time initialization before it can be used. Callers should
 * call this function only once, and only if OpenSSL is not directly initialized
 * elsewhere.
 */
int redisInitOpenSSL(void);

/**
 * Helper function to initialize an OpenSSL context that can be used
 * to initiate SSL connections.
 *
 * cacert_filename is an optional name of a CA certificate/bundle file to load
 * and use for validation.
 *
 * capath is an optional directory path where trusted CA certificate files are
 * stored in an OpenSSL-compatible structure.
 *
 * cert_filename and private_key_filename are optional names of a client side
 * certificate and private key files to use for authentication. They need to
 * be both specified or omitted.
 *
 * server_name is an optional and will be used as a server name indication
 * (SNI) TLS extension.
 *
 * If error is non-null, it will be populated in case the context creation fails
 * (returning a NULL).
 */

redisSSLContext *redisCreateSSLContext(const char *cacert_filename, const char *capath,
        const char *cert_filename, const char *private_key_filename,
        const char *server_name, redisSSLContextError *error);

/**
 * Free a previously created OpenSSL context.
 */
void redisFreeSSLContext(redisSSLContext *redis_ssl_ctx);

/**
 * Initiate SSL on an existing redisContext.
 *
 * This is similar to redisInitiateSSL() but does not require the caller
 * to directly interact with OpenSSL, and instead uses a redisSSLContext
 * previously created using redisCreateSSLContext().
 */

int redisInitiateSSLWithContext(redisContext *c, redisSSLContext *redis_ssl_ctx);

/**
 * Initiate SSL/TLS negotiation on a provided OpenSSL SSL object.
 */

int redisInitiateSSL(redisContext *c, struct ssl_st *ssl);

#ifdef __cplusplus
}
#endif

#endif  /* __HIREDIS_SSL_H */

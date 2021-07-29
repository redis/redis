/* CLI (command line interface) common methods
 * 
 * Copyright (c) 2020, Redis Labs
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

#include "cli_common.h"
#include <errno.h>
#include <hiredis.h>
#include <sdscompat.h> /* Use hiredis' sds compat header that maps sds calls to their hi_ variants */
#include <sds.h> /* use sds.h from hiredis, so that only one set of sds functions will be present in the binary */
#ifdef USE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <hiredis_ssl.h>
#endif


/* Wrapper around redisSecureConnection to avoid hiredis_ssl dependencies if
 * not building with TLS support.
 */
int cliSecureConnection(redisContext *c, cliSSLconfig config, const char **err) {
#ifdef USE_OPENSSL
    static SSL_CTX *ssl_ctx = NULL;

    if (!ssl_ctx) {
        ssl_ctx = SSL_CTX_new(SSLv23_client_method());
        if (!ssl_ctx) {
            *err = "Failed to create SSL_CTX";
            goto error;
        }
        SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
        SSL_CTX_set_verify(ssl_ctx, config.skip_cert_verify ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, NULL);

        if (config.cacert || config.cacertdir) {
            if (!SSL_CTX_load_verify_locations(ssl_ctx, config.cacert, config.cacertdir)) {
                *err = "Invalid CA Certificate File/Directory";
                goto error;
            }
        } else {
            if (!SSL_CTX_set_default_verify_paths(ssl_ctx)) {
                *err = "Failed to use default CA paths";
                goto error;
            }
        }

        if (config.cert && !SSL_CTX_use_certificate_chain_file(ssl_ctx, config.cert)) {
            *err = "Invalid client certificate";
            goto error;
        }

        if (config.key && !SSL_CTX_use_PrivateKey_file(ssl_ctx, config.key, SSL_FILETYPE_PEM)) {
            *err = "Invalid private key";
            goto error;
        }
        if (config.ciphers && !SSL_CTX_set_cipher_list(ssl_ctx, config.ciphers)) {
            *err = "Error while configuring ciphers";
            goto error;
        }
#ifdef TLS1_3_VERSION
        if (config.ciphersuites && !SSL_CTX_set_ciphersuites(ssl_ctx, config.ciphersuites)) {
            *err = "Error while setting cypher suites";
            goto error;
        }
#endif
    }

    SSL *ssl = SSL_new(ssl_ctx);
    if (!ssl) {
        *err = "Failed to create SSL object";
        return REDIS_ERR;
    }

    if (config.sni && !SSL_set_tlsext_host_name(ssl, config.sni)) {
        *err = "Failed to configure SNI";
        SSL_free(ssl);
        return REDIS_ERR;
    }

    return redisInitiateSSL(c, ssl);

error:
    SSL_CTX_free(ssl_ctx);
    ssl_ctx = NULL;
    return REDIS_ERR;
#else
    (void) config;
    (void) c;
    (void) err;
    return REDIS_OK;
#endif
}

/* Wrapper around hiredis to allow arbitrary reads and writes.
 *
 * We piggybacks on top of hiredis to achieve transparent TLS support,
 * and use its internal buffers so it can co-exist with commands
 * previously/later issued on the connection.
 *
 * Interface is close to enough to read()/write() so things should mostly
 * work transparently.
 */

/* Write a raw buffer through a redisContext. If we already have something
 * in the buffer (leftovers from hiredis operations) it will be written
 * as well.
 */
ssize_t cliWriteConn(redisContext *c, const char *buf, size_t buf_len)
{
    int done = 0;

    /* Append data to buffer which is *usually* expected to be empty
     * but we don't assume that, and write.
     */
    c->obuf = sdscatlen(c->obuf, buf, buf_len);
    if (redisBufferWrite(c, &done) == REDIS_ERR) {
        if (!(c->flags & REDIS_BLOCK))
            errno = EAGAIN;

        /* On error, we assume nothing was written and we roll back the
         * buffer to its original state.
         */
        if (sdslen(c->obuf) > buf_len)
            sdsrange(c->obuf, 0, -(buf_len+1));
        else
            sdsclear(c->obuf);

        return -1;
    }

    /* If we're done, free up everything. We may have written more than
     * buf_len (if c->obuf was not initially empty) but we don't have to
     * tell.
     */
    if (done) {
        sdsclear(c->obuf);
        return buf_len;
    }

    /* Write was successful but we have some leftovers which we should
     * remove from the buffer.
     *
     * Do we still have data that was there prior to our buf? If so,
     * restore buffer to it's original state and report no new data was
     * written.
     */
    if (sdslen(c->obuf) > buf_len) {
        sdsrange(c->obuf, 0, -(buf_len+1));
        return 0;
    }

    /* At this point we're sure no prior data is left. We flush the buffer
     * and report how much we've written.
     */
    size_t left = sdslen(c->obuf);
    sdsclear(c->obuf);
    return buf_len - left;
}

/* Wrapper around OpenSSL (libssl and libcrypto) initialisation
 */
int cliSecureInit()
{
#ifdef USE_OPENSSL
    ERR_load_crypto_strings();
    SSL_load_error_strings();
    SSL_library_init();
#endif
    return REDIS_OK;
}

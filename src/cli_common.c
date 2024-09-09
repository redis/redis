/* CLI (command line interface) common methods
 * 
 * Copyright (c) 2020-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#include "fmacros.h"
#include "cli_common.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <hiredis.h>
#include <sdscompat.h> /* Use hiredis' sds compat header that maps sds calls to their hi_ variants */
#include <sds.h> /* use sds.h from hiredis, so that only one set of sds functions will be present in the binary */
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#ifdef USE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <hiredis_ssl.h>
#endif

#define UNUSED(V) ((void) V)

char *redisGitSHA1(void);
char *redisGitDirty(void);

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
int cliSecureInit(void)
{
#ifdef USE_OPENSSL
    ERR_load_crypto_strings();
    SSL_load_error_strings();
    SSL_library_init();
#endif
    return REDIS_OK;
}

/* Create an sds from stdin */
sds readArgFromStdin(void) {
    char buf[1024];
    sds arg = sdsempty();

    while(1) {
        int nread = read(fileno(stdin),buf,1024);

        if (nread == 0) break;
        else if (nread == -1) {
            perror("Reading from standard input");
            exit(1);
        }
        arg = sdscatlen(arg,buf,nread);
    }
    return arg;
}

/* Create an sds array from argv, either as-is or by dequoting every
 * element. When quoted is non-zero, may return a NULL to indicate an
 * invalid quoted string.
 *
 * The caller should free the resulting array of sds strings with
 * sdsfreesplitres().
 */
sds *getSdsArrayFromArgv(int argc,char **argv, int quoted) {
    sds *res = sds_malloc(sizeof(sds) * argc);

    for (int j = 0; j < argc; j++) {
        if (quoted) {
            sds unquoted = unquoteCString(argv[j]);
            if (!unquoted) {
                while (--j >= 0) sdsfree(res[j]);
                sds_free(res);
                return NULL;
            }
            res[j] = unquoted;
        } else {
            res[j] = sdsnew(argv[j]);
        }
    }

    return res;
}

/* Unquote a null-terminated string and return it as a binary-safe sds. */
sds unquoteCString(char *str) {
    int count;
    sds *unquoted = sdssplitargs(str, &count);
    sds res = NULL;

    if (unquoted && count == 1) {
        res = unquoted[0];
        unquoted[0] = NULL;
    }

    if (unquoted)
        sdsfreesplitres(unquoted, count);

    return res;
}


/* URL-style percent decoding. */
#define isHexChar(c) (isdigit(c) || ((c) >= 'a' && (c) <= 'f'))
#define decodeHexChar(c) (isdigit(c) ? (c) - '0' : (c) - 'a' + 10)
#define decodeHex(h, l) ((decodeHexChar(h) << 4) + decodeHexChar(l))

static sds percentDecode(const char *pe, size_t len) {
    const char *end = pe + len;
    sds ret = sdsempty();
    const char *curr = pe;

    while (curr < end) {
        if (*curr == '%') {
            if ((end - curr) < 2) {
                fprintf(stderr, "Incomplete URI encoding\n");
                exit(1);
            }

            char h = tolower(*(++curr));
            char l = tolower(*(++curr));
            if (!isHexChar(h) || !isHexChar(l)) {
                fprintf(stderr, "Illegal character in URI encoding\n");
                exit(1);
            }
            char c = decodeHex(h, l);
            ret = sdscatlen(ret, &c, 1);
            curr++;
        } else {
            ret = sdscatlen(ret, curr++, 1);
        }
    }

    return ret;
}

/* Parse a URI and extract the server connection information.
 * URI scheme is based on the provisional specification[1] excluding support
 * for query parameters. Valid URIs are:
 *   scheme:    "redis://"
 *   authority: [[<username> ":"] <password> "@"] [<hostname> [":" <port>]]
 *   path:      ["/" [<db>]]
 *
 *  [1]: https://www.iana.org/assignments/uri-schemes/prov/redis */
void parseRedisUri(const char *uri, const char* tool_name, cliConnInfo *connInfo, int *tls_flag) {
#ifdef USE_OPENSSL
    UNUSED(tool_name);
#else
    UNUSED(tls_flag);
#endif

    const char *scheme = "redis://";
    const char *tlsscheme = "rediss://";
    const char *curr = uri;
    const char *end = uri + strlen(uri);
    const char *userinfo, *username, *port, *host, *path;

    /* URI must start with a valid scheme. */
    if (!strncasecmp(tlsscheme, curr, strlen(tlsscheme))) {
#ifdef USE_OPENSSL
        *tls_flag = 1;
        curr += strlen(tlsscheme);
#else
        fprintf(stderr,"rediss:// is only supported when %s is compiled with OpenSSL\n", tool_name);
        exit(1);
#endif
    } else if (!strncasecmp(scheme, curr, strlen(scheme))) {
        curr += strlen(scheme);
    } else {
        fprintf(stderr,"Invalid URI scheme\n");
        exit(1);
    }
    if (curr == end) return;

    /* Extract user info. */
    if ((userinfo = strchr(curr,'@'))) {
        if ((username = strchr(curr, ':')) && username < userinfo) {
            connInfo->user = percentDecode(curr, username - curr);
            curr = username + 1;
        }

        connInfo->auth = percentDecode(curr, userinfo - curr);
        curr = userinfo + 1;
    }
    if (curr == end) return;

    /* Extract host and port. */
    path = strchr(curr, '/');
    if (*curr != '/') {
        host = path ? path - 1 : end;
        if (*curr == '[') {
            curr += 1;
            if ((port = strchr(curr, ']'))) {
                if (*(port+1) == ':') {
                    connInfo->hostport = atoi(port + 2);
                }
                host = port - 1;
            }
        } else {
            if ((port = strchr(curr, ':'))) {
                connInfo->hostport = atoi(port + 1);
                host = port - 1;
            }
        }
        sdsfree(connInfo->hostip);
        connInfo->hostip = sdsnewlen(curr, host - curr + 1);
    }
    curr = path ? path + 1 : end;
    if (curr == end) return;

    /* Extract database number. */
    connInfo->input_dbnum = atoi(curr);
}

void freeCliConnInfo(cliConnInfo connInfo){
    if (connInfo.hostip) sdsfree(connInfo.hostip);
    if (connInfo.auth) sdsfree(connInfo.auth);
    if (connInfo.user) sdsfree(connInfo.user);
}

/*
 * Escape a Unicode string for JSON output (--json), following RFC 7159:
 * https://datatracker.ietf.org/doc/html/rfc7159#section-7
*/
sds escapeJsonString(sds s, const char *p, size_t len) {
    s = sdscatlen(s,"\"",1);
    while(len--) {
        switch(*p) {
        case '\\':
        case '"':
            s = sdscatprintf(s,"\\%c",*p);
            break;
        case '\n': s = sdscatlen(s,"\\n",2); break;
        case '\f': s = sdscatlen(s,"\\f",2); break;
        case '\r': s = sdscatlen(s,"\\r",2); break;
        case '\t': s = sdscatlen(s,"\\t",2); break;
        case '\b': s = sdscatlen(s,"\\b",2); break;
        default:
            s = sdscatprintf(s,*(unsigned char *)p <= 0x1f ? "\\u%04x" : "%c",*p);
        }
        p++;
    }
    return sdscatlen(s,"\"",1);
}

sds cliVersion(void) {
    sds version = sdscatprintf(sdsempty(), "%s", REDIS_VERSION);

    /* Add git commit and working tree status when available. */
    if (strtoll(redisGitSHA1(),NULL,16)) {
        version = sdscatprintf(version, " (git:%s", redisGitSHA1());
        if (strtoll(redisGitDirty(),NULL,10))
            version = sdscatprintf(version, "-dirty");
        version = sdscat(version, ")");
    }
    return version;
}

/* This is a wrapper to call redisConnect or redisConnectWithTimeout. */
redisContext *redisConnectWrapper(const char *ip, int port, const struct timeval tv) {
    if (tv.tv_sec == 0 && tv.tv_usec == 0) {
        return redisConnect(ip, port);
    } else {
        return redisConnectWithTimeout(ip, port, tv);
    }
}

/* This is a wrapper to call redisConnectUnix or redisConnectUnixWithTimeout. */
redisContext *redisConnectUnixWrapper(const char *path, const struct timeval tv) {
    if (tv.tv_sec == 0 && tv.tv_usec == 0) {
        return redisConnectUnix(path);
    } else {
        return redisConnectUnixWithTimeout(path, tv);
    }
}

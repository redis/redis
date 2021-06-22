#ifndef __CLICOMMON_H
#define __CLICOMMON_H

#include <hiredis.h>
#include <sdscompat.h> /* Use hiredis' sds compat header that maps sds calls to their hi_ variants */

typedef struct cliSSLconfig {
    /* Requested SNI, or NULL */
    char *sni;
    /* CA Certificate file, or NULL */
    char *cacert;
    /* Directory where trusted CA certificates are stored, or NULL */
    char *cacertdir;
    /* Skip server certificate verification. */
    int skip_cert_verify;
    /* Client certificate to authenticate with, or NULL */
    char *cert;
    /* Private key file to authenticate with, or NULL */
    char *key;
    /* Preferred cipher list, or NULL (applies only to <= TLSv1.2) */
    char* ciphers;
    /* Preferred ciphersuites list, or NULL (applies only to TLSv1.3) */
    char* ciphersuites;
} cliSSLconfig;

/* Wrapper around redisSecureConnection to avoid hiredis_ssl dependencies if
 * not building with TLS support.
 */
int cliSecureConnection(redisContext *c, cliSSLconfig config, const char **err);

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
ssize_t cliWriteConn(redisContext *c, const char *buf, size_t buf_len);

/* Wrapper around OpenSSL (libssl and libcrypto) initialisation.
 */
int cliSecureInit();

/* Create an sds from stdin */
sds readArgFromStdin(void);

/* Create an sds array from argv, either as-is or by dequoting every
 * element. When quoted is non-zero, may return a NULL to indicate an
 * invalid quoted string.
 */
sds *getSdsArrayFromArgv(int argc,char **argv, int quoted);

/* Unquote a null-terminated string and return it as a binary-safe sds. */
sds unquoteCString(char *str);

#endif /* __CLICOMMON_H */

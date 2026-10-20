#ifndef __CLICOMMON_H
#define __CLICOMMON_H

#include <hiredis.h>
#include <sdscompat.h> /* Use hiredis' sds compat header that maps sds calls to their hi_ variants */

#ifdef USE_OPENSSL
#include <openssl/ssl.h>
#endif

#if defined(TLS_NO_GROUPS)
#define CLI_TLS_SUPPORTS_GROUPS 0
#elif defined(SSL_CTX_set1_groups_list)
#define CLI_TLS_SUPPORTS_GROUPS 1
#define cliSslCtxSetGroupsList(ctx, list) SSL_CTX_set1_groups_list((ctx), (list))
#elif defined(SSL_CTX_set1_curves_list)
#define CLI_TLS_SUPPORTS_GROUPS 1
#define cliSslCtxSetGroupsList(ctx, list) SSL_CTX_set1_curves_list((ctx), (list))
#else
#define CLI_TLS_SUPPORTS_GROUPS 0
#endif

/* Peer identity helpers shared with tls.c and sentinel.c: the address-literal
 * test (always available) and, with OpenSSL, the build gate and the function
 * that binds an identity to the verification parameters. */
#include "tlshelpers.h"

/* Whether server certificate identity verification is available in this build
 * (OpenSSL >= 1.0.2, not compiled out with TLS_NO_PEER_NAME_VERIFICATION). */
#ifdef USE_OPENSSL
#define CLI_TLS_SUPPORTS_VERIFY_NAME CONN_TLS_SUPPORTS_VERIFY_NAME
#else
#define CLI_TLS_SUPPORTS_VERIFY_NAME 0
#endif

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
    /* Preferred TLS named groups list, or NULL */
    char* groups;
    /* Verify only that the server certificate chains to the trusted CA, not
     * that it carries the expected identity (--no-verify-hostname). */
    int skip_hostname_verify;
    /* Space-separated expected peer certificate name(s) to verify instead of
     * the name derived from --sni or the dialed host, or NULL
     * (--tls-expected-peer-name). */
    char *expected_peer_name;
} cliSSLconfig;


/* server connection information object, used to describe an ip:port pair, db num user input, and user:pass. */
typedef struct cliConnInfo {
    char *hostip;
    int hostport;
    int input_dbnum;
    char *auth;
    char *user;
} cliConnInfo;

int cliSecureConnection(redisContext *c, cliSSLconfig config, const char **err);

/* Validate a space-separated list of expected TLS peer names as given to
 * --tls-expected-peer-name. Returns NULL if valid, or an error message. When
 * valid and parent_domain_entries is non-NULL, it receives the number of
 * entries beginning with a dot (OpenSSL parent-domain patterns), so the caller
 * can surface their wider scope. */
const char *cliValidateExpectedPeerNames(const char *names, int *parent_domain_entries);

ssize_t cliWriteConn(redisContext *c, const char *buf, size_t buf_len);

int cliSecureInit(void);

sds readArgFromStdin(void);

sds *getSdsArrayFromArgv(int argc,char **argv, int quoted);

sds unquoteCString(char *str);

void parseRedisUri(const char *uri, const char* tool_name, cliConnInfo *connInfo, int *tls_flag);

void freeCliConnInfo(cliConnInfo connInfo);

sds escapeJsonString(sds s, const char *p, size_t len);

sds cliVersion(void);

redisContext *redisConnectWrapper(const char *ip, int port, const struct timeval tv);
redisContext *redisConnectUnixWrapper(const char *path, const struct timeval tv);

#endif /* __CLICOMMON_H */

/* SSL implementation for redis
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright 2019 Amazon.com, Inc. or its affiliates.
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

#ifndef SSL_H
#define SSL_H

#include <stdint.h>
#include "ae.h"
#include "adlist.h"

#ifdef BUILD_SSL
#include <s2n.h>
#include <openssl/ssl.h>
#endif

#define SSL_ENABLE_DEFAULT 0
#define SSL_CIPHER_PREFS_DEFAULT "default"

struct client;
struct s2n_config;
struct s2n_connection;
struct client_cert_verify_context;

typedef enum {
    NEGOTIATE_NOT_STARTED = 0, NEGOTIATE_RETRY, NEGOTIATE_DONE, NEGOTIATE_FAILED
} SslNegotiationStatus;

typedef enum{
    SSL_SERVER = 0, SSL_CLIENT
} sslMode;

#define SSL_PERFORMANCE_MODE_LOW_LATENCY 0
#define SSL_PERFORMANCE_MODE_HIGH_THROUGHPUT 1
#define SSL_PERFORMANCE_MODE_DEFAULT SSL_PERFORMANCE_MODE_LOW_LATENCY
#define DER_CERT_LEN_BYTES 3
#define CERT_CNAME_MAX_LENGTH 256
#define CERT_DATE_MAX_LENGTH 256
/* default value of root-ca-certs-path parameter. */
#define ROOT_CA_CERTS_PATH "/etc/ssl/certs/ca-bundle.crt"

#define NEWLINE_PING_IN_PROGRESS_FLAG (1<<0)
#define LOAD_NOTIFICATION_SENT_FLAG (1<<1)
#define CLIENT_CONNECTION_FLAG (1<<2)
#define OLD_CERTIFICATE_FLAG (1<<3)

/* Our internal representation of an SSL connection */
typedef struct ssl_connection {

#ifdef BUILD_SSL
    /* S2N connection reference */
    struct s2n_connection* s2nconn;
#endif
    /* File descriptor for the SSL connection */
    int fd;

    /* An int containing references to flags */
    int connection_flags;
    /* NEWLINE_PING_IN_PROGRESS_FLAG: Is a newline ping in progress (from a call to sslPing())?  1 if true, 0 otherwise. */
    /* LOAD_NOTIFICATION_SENT_FLAG: Has the load notification character been sent to the master */
    /* CLIENT_CONNECTION_FLAG: Is this connection associated with a redis client */
    /* OLD_CERTIFICATE_FLAG: Is this connection associated with an older certificate */

    /* Does the S2N connection contain cached data?  Set to a list node (present in
     * sslconn_with_cached_data list) if true, NULL otherwise. */
    listNode *cached_data_node;

} ssl_connection;

/* Structure to store SSL related information */
typedef struct ssl_t {
    int enable_ssl; /* Controls whether SSL is enabled or not*/

#ifdef BUILD_SSL
    struct s2n_config *server_ssl_config; /* Structure to store S2N configuration like certificate, diffie-hellman parameters, cipher suite preferences */
    struct s2n_config *server_ssl_config_old; /* SSL configuration corresponding to expired/expiring certificate */
    struct s2n_config *client_ssl_config; /* Structure to store s2n configuration for replication */
    struct s2n_cert_chain_and_key *cert_chain_and_key; /* Structure representing a certificate chain and private key pair for the current server config */
    struct s2n_cert_chain_and_key *cert_chain_and_key_old; /* Structure representing a certificate chain and private key pair for the old server config */
    struct client_cert_verify_context *client_cert_verify_context; /* Context for certification verification in clients */
#endif

    char *ssl_certificate; /* SSL certificate for SSL */
    char *ssl_certificate_file; /* File containing SSL certificate for SSL */
    char *ssl_certificate_private_key; /* private key corresponding to SSL certificate*/
    char *ssl_certificate_private_key_file; /* File containing private key corresponding to SSL certificate*/
    char *ssl_dh_params; /* DH parameters for SSL*/
    char *ssl_dh_params_file; /* File containing DH parameters for SSL*/
    char *ssl_cipher_prefs; /* Cipher preferences for SSL */
    int ssl_performance_mode; /* SSL performance mode - low latency or high throughput */
    char *root_ca_certs_path; /* Path to root CA certificates */

    ssl_connection **fd_to_sslconn; /* socket fd to SSL connection mapping */
    size_t fd_to_sslconn_size; /* current size of fd_to_sslconn mapping */

    list *sslconn_with_cached_data; /* A list of SSL connections which contain cached data, which will be drained by repeated read task. */
    long long repeated_reads_task_id; /* The ae task ID of the timer event to process repeated reads, or -1 if not set. */
    uint64_t total_repeated_reads; /* Total number of repeated reads performed since the process began. */
    unsigned long max_repeated_read_list_length; /* The maximum number of repeated reads processed simultaneously. */
    
    char * expected_hostname; /* Expected hostname used for verifying ssl connections */
    char * certificate_not_before_date; /* The not before date on the latest ssl certificate */
    char * certificate_not_after_date; /* The not after date on the latest ssl certificate */
    long certificate_serial; /* The serial of the certificate, this should be converted to hex before displaying */
    int connections_to_previous_certificate; /* The number of connections that connected to the old certificate */ 
    int connections_to_current_certificate; /* The number of connections that connected to the new certificate */
} ssl_t;

/* SSL helper functions that can be called without SSL compiled */
int getSslPerformanceModeByName(char *name);
char *getSslPerformanceModeStr(int mode);
void initSslConfigDefaults(ssl_t *ssl);
void noopHandler(aeEventLoop *el, int fd, void *privdata, int mask);

#ifdef BUILD_SSL
typedef struct
{
    X509_STORE * trust_store;
    char *expected_hostname;
} client_cert_verify_context;

/* Macros to help simplify SSL code path */
#define isSSLEnabled() server.ssl_config.enable_ssl
#define isSSLCompiled() 1

/* SSL configuration functions */
void initSsl(ssl_t *ssl);
void cleanupSsl(ssl_t *ssl);
int isResizeAllowed(int new_size);
int resizeFdToSslConnSize(unsigned int setsize);
int renewCertificate(char *new_certificate, char *new_private_key, char *new_certificate_filename,
    char *new_private_key_filename);

/* SSL connection functions */
ssl_connection *
initSslConnection(sslMode mode, int fd, int ssl_performance_mode, char *masterhost);
int setupSslOnClient(struct client *c, int fd, int ssl_performance_mode);
void cleanupSslConnectionForFd(int fd);
void cleanupSslConnectionForFdWithoutShutdown(int fd);

/* SSL Negotiation functions */
void sslNegotiateWithClient(aeEventLoop *el, int fd, void *privdata, int mask);
void sslNegotiateWithMaster(aeEventLoop *el, int fd, void *privdata, int mask);
void sslNegotiateWithClusterNodeAsServer(aeEventLoop *el, int fd, void *privdata, int mask);
void sslNegotiateWithClusterNodeAsClient(aeEventLoop *el, int fd, void *privdata, int mask);

void startSslNegotiateWithMasterAfterRdbLoad(int fd);
void startSslNegotiateWithSlaveAfterRdbTransfer(struct client *slave);
void startWaitForSlaveToLoadRdbAfterRdbTransfer(struct client *slave);
int syncSslNegotiateForFd(int fd, long timeout);
void deleteReadEventHandlerForSlavesWaitingBgsave();

/* SSL Wrapping functions. These overwrite IO system calls to transparently
 * add SSL compatibility when applicable */
#define read(fd, buffer, len) __redis_wrap_read((fd), (buffer), (len))
#define write(fd, buffer, len) __redis_wrap_write((fd), (buffer), (len))
#define close(fd) __redis_wrap_close((fd))
#define ping(fd) __redis_wrap_ping((fd))
#define strerror(err) __redis_wrap_strerror((err))

ssize_t __redis_wrap_read(int fd, void *buffer, size_t nbytes);
ssize_t __redis_wrap_write(int fd, const void *buffer, size_t nbytes);
int __redis_wrap_close(int fd);
void __redis_wrap_ping(int fd);
const char *__redis_wrap_strerror(int err);

#define isSSLFd(fd) (((size_t)(fd) < server.ssl_config.fd_to_sslconn_size) && \
    (server.ssl_config.fd_to_sslconn[(fd)] != NULL))

#else

/* Macros to help simplify SSL code path */
#define ping(fd) write((fd), "\n", 1)
#define isSSLEnabled() 0
#define isSSLCompiled() 0

/* In order to avoid #ifdefing within redis, all SSL functions have dummy implementations
 * for functions that have SSL dependencies. These dummy functions are either noops
 * or failures, and should never be called outside of SSL compilation. */
#define noop() (void)(1)

#define initSsl(ssl) noop()
#define cleanupSsl(ssl) noop()
#define isResizeAllowed(new_size) C_ERR
#define resizeFdToSslConnSize(setsize) C_ERR
#define renewCertificate(new_certificate, new_private_key, new_certificate_filename, new_private_key_filename) C_ERR

#define initSslConnection(mode, fd, ssl_performance_mode, masterhost) NULL
#define setupSslOnClient(c, fd, ssl_performance_mode) C_ERR
#define cleanupSslConnectionForFd(fd) noop()
#define cleanupSslConnectionForFdWithoutShutdown(fd) noop()

#define sslNegotiateWithClient noopHandler
#define sslNegotiateWithMaster noopHandler
#define sslNegotiateWithClusterNodeAsServer noopHandler
#define sslNegotiateWithClusterNodeAsClient noopHandler

#define startSslNegotiateWithMasterAfterRdbLoad(fd) noop()
#define startSslNegotiateWithSlaveAfterRdbTransfer(slave) noop()
#define startWaitForSlaveToLoadRdbAfterRdbTransfer(slave) noop()
#define syncSslNegotiateForFd(fd, timeout) 0
#define deleteReadEventHandlerForSlavesWaitingBgsave() noop()
#endif

#endif /* SSL_H */

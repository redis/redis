#include "server.h" // Only for logging statements

#include "ssl.h"
#include <stdlib.h>

#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/pem.h>

/* -------------------------- private prototypes ---------------------------- */
// Helper functions for the connection
int shutdownSslConnection(ssl_connection *conn);

static inline ssize_t sslRecv(ssl_connection *ssl_conn, void *buffer, size_t nbytes, s2n_blocked_status * blocked);

// Functions for SSL configuration
static int initClientSslConfig(ssl_t *ssl);

static int initServerSslConfig(ssl_t *ssl);

static struct s2n_config *
initSslConfigForServer(const char *certificate, const char *privateKey, const char *dhParams,
                       const char *cipherPrefs);

static struct s2n_config *
initSslConfigForClient(const char *cipher_prefs,
                       const char *certificate, const char *rootCACertificatesPath);

static struct s2n_config *
initSslConfig(int is_server, const char *certificate, const char *private_key, const char *dh_params,
              const char *cipher_prefs, const char *rootCACertificatesPath);

uint8_t s2nVerifyHost(const char *hostName, size_t length, void *data);

/* Functions used for reading and parsing X509 certificates */
static int getCnameFromCertificate(const char *certificate, char *subject_name);    

int updateServerCertificateInformation(const char *certificate, char *not_before_date, char *not_after_date, long *serial);    

int convertASN1TimeToString(ASN1_TIME *timePointer, char* outputBuffer, size_t length);

X509 *getX509FromCertificate(const char *certificate);

static void updateClientsUsingOldCertificate(ssl_t *ssl_config);

// Functions for SSL negotiation
int updateEventHandlerForSslHandshake(s2n_blocked_status blocked, aeEventLoop *el, int fd, void *privdata);

SslNegotiationStatus sslNegotiate(aeEventLoop *el, ssl_connection *ssl_conn, void *privdata);

void sslNegotiationHandler(aeEventLoop *el, int fd, void *privdata, int mask);

// Functions for processoring repeated reads                       
int processRepeatedReads(struct aeEventLoop *eventLoop, long long id, void *clientData);

void addRepeatedRead(ssl_connection *conn);

void removeRepeatedRead(ssl_connection *conn);

/* ------------------------- private functions ------------------------------ */

/**
 * shuts down the SSL connection. It effectively sends
 * a SHUTDOWN tls alert to the peer (as a SSL best practice before
 * we close socket)
 */
int shutdownSslConnection(ssl_connection *conn) {
    serverLog(LL_DEBUG, "Shutting down SSL conn");
    if (conn != NULL && conn->s2nconn != NULL) {
        s2n_blocked_status blocked;
        s2n_shutdown(conn->s2nconn, &blocked);
    }
    return C_OK;
}

/*
 * SSL compatible wrapper around recv that is used as an abstraction for sslRead.
 */
static inline ssize_t sslRecv(ssl_connection *ssl_conn, void *buffer, size_t nbytes, s2n_blocked_status * blocked) {
    s2n_errno = S2N_ERR_T_OK;
    errno = 0;

    ssize_t bytesRead = s2n_recv(ssl_conn->s2nconn, buffer, nbytes, blocked);

    if (bytesRead < 0 && s2n_error_get_type(s2n_errno) == S2N_ERR_T_BLOCKED) {
        /* No data was returned because the socket did not have a full frame.  We can only continue when the socket is readable again. */

        //set errno as well in case IO blocked. This is so that calling code treats
        //it like regular blocking IO and does not has to do any special logic for SSL based IO
        errno = EAGAIN;
    }

    return bytesRead;
}

/**
 * Perform the same verification as open source s2n uses except don't use the connection name
 * since it doesn't have the right endpoint in some cases for cluster bus.
 */
uint8_t s2nVerifyHost(const char *hostName, size_t length, void *data) {
    ssl_t *config = (ssl_t *) data;
    /* if present, match server_name of the connection using rules
     * outlined in RFC6125 6.4. */

    if (config->expected_hostname == NULL) {
        return 0;
    }

    /* complete match */
    if (strlen(config->expected_hostname) == length &&
            strncasecmp(config->expected_hostname, hostName, length) == 0) {
        return 1;
    }

    /* match 1 level of wildcard */
    if (length > 2 && hostName[0] == '*' && hostName[1] == '.') {
        const char *suffix = strchr(config->expected_hostname, '.');

        if (suffix == NULL) {
            return 0;
        }

        if (strlen(suffix) == length - 1 &&
                strncasecmp(suffix, hostName + 1, length - 1) == 0) {
            return 1;
        }
    }

    return 0;
}

/**
 * Return an x509 object from a certificate string.
 */
X509 *getX509FromCertificate(const char *certificate){
    BIO *bio = NULL;
    // Create a read-only BIO backed by the supplied memory buffer
    bio = BIO_new_mem_buf((void *) certificate, -1);
    
    if (!bio) {
        serverLog(LL_WARNING, "Error allocating BIO buffer");
        return NULL;
    }
    
    X509 *x509_cert = NULL;
    //Read a certificate in PEM format from a BIO
    if (!(x509_cert = PEM_read_bio_X509(bio, NULL, NULL, NULL))) {
        BIO_free(bio);
        serverLog(LL_DEBUG, "Error converting certificate from PEM to X509 format");
        return NULL;
    }
    
    // Cleanup. bio is no longer needed
    BIO_free(bio);
    return x509_cert;
}

/**
 * Extract the Cname from a certificate to be used later in hostname validation. We need this
 * because we want to verify the hostname we are connecting to even when we are using the IP address.
 */
static int getCnameFromCertificate(const char *certificate, char *subject_name) {    
    X509 *x509_cert = getX509FromCertificate(certificate);
    if (x509_cert == NULL) {
        return C_ERR;
    }
    
    if (X509_NAME_get_text_by_NID(X509_get_subject_name(x509_cert), NID_commonName, subject_name,
                                  CERT_CNAME_MAX_LENGTH) == -1) {
        X509_free(x509_cert);
        serverLog(LL_DEBUG, "Could not find a CN entry in certificate");
        return C_ERR;
    }
    X509_free(x509_cert);
    serverLog(LL_DEBUG, "Successfully extracted subject name from certificate. Subject Name: %s", subject_name);
    return C_OK;
}

/**
 * Convert an ANSI string to a C String and write it to the output buffer.
 */
int convertASN1TimeToString(ASN1_TIME *timePointer, char* outputBuffer, size_t length) {
    BIO *buffer = BIO_new(BIO_s_mem());
    if (ASN1_TIME_print(buffer, timePointer) <= 0) {
        BIO_free(buffer);
        return C_ERR;
    }
    if (BIO_gets(buffer, outputBuffer, length) <= 0) {
        BIO_free(buffer);
        return C_ERR;
    }
    BIO_free(buffer);
    return C_OK;
}

/**
 * Read the provided certificate file and populate the not_after and not_before dates. The values returned
 * are not guaranteed to be right unless C_OK is returned.
 */
int updateServerCertificateInformation(const char *certificate, char *not_before_date, char *not_after_date, long *serial) {
    X509 *x509_cert = getX509FromCertificate(certificate);
    if (x509_cert == NULL) {
        return C_ERR;
    }
    
    if(convertASN1TimeToString(X509_get_notBefore(x509_cert), not_before_date, CERT_DATE_MAX_LENGTH) == -1){
        serverLog(LL_DEBUG, "Failed to extract not before date from certificate.");
        X509_free(x509_cert);
        return C_ERR;
    }
    serverLog(LL_DEBUG, "Successfully extracted not before date: %s from certificate.", not_before_date);
    
    if(convertASN1TimeToString(X509_get_notAfter(x509_cert), not_after_date, CERT_DATE_MAX_LENGTH) == -1){
        serverLog(LL_DEBUG, "Failed to extract not after date from provided certificate.");
        X509_free(x509_cert);
        return C_ERR;
    }
    
    serverLog(LL_DEBUG, "Successfully extracted not after date: %s from certificate.", not_after_date);    
    long newSerial = ASN1_INTEGER_get(X509_get_serialNumber(x509_cert));
    if(newSerial == 0){
        serverLog(LL_DEBUG, "Failed to extract not before date from provided certificate.");
        X509_free(x509_cert);
        return C_ERR;
    }
    *serial = newSerial;
    serverLog(LL_DEBUG, "Successfully extracted serial: %lx from certificate.", newSerial);
    X509_free(x509_cert);
    return C_OK;
}

/**
 * Helper method to see where SSL negotiation is blocked on read or write and register
 * to listen on file descriptor accordingly
 */
int updateEventHandlerForSslHandshake(s2n_blocked_status blocked, aeEventLoop *el, int fd, void *privdata) {
    int deleteEvent;
    int listenEvent;
    switch (blocked) {
        case S2N_BLOCKED_ON_READ:
            deleteEvent = AE_WRITABLE;
            listenEvent = AE_READABLE;
            break;
        case S2N_BLOCKED_ON_WRITE:
            deleteEvent = AE_READABLE;
            listenEvent = AE_WRITABLE;
            break;
        default:
            return C_OK;

    }
    aeDeleteFileEvent(el, fd, deleteEvent);
    if (aeGetFileEvents(el, fd) == AE_NONE) {
        if (aeCreateFileEvent(el, fd, listenEvent, sslNegotiationHandler, privdata) == AE_ERR) {
            return C_ERR;
        }
    }
    return C_OK;
}
/**
 * SSL ae event handler for ssl negotiation. This function should not be called directly, instead
 * call startSslNegotiation(). This handler will continue to fire until the ssl negotiation is 
 * completed where it will then either called the success or failure callback with the provided
 * private data in the negotiation context 
 */
void sslNegotiationHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    sslNegotiationContext *context = (sslNegotiationContext *) privdata;
    SslNegotiationStatus status = sslNegotiate(el, context->connection, privdata);
    switch (status) {
        case NEGOTIATE_FAILED:
            context->failure_callback(el, fd, context->connection, mask);
            break;
        case NEGOTIATE_RETRY:
            // Probably should have something here
            break;
        case NEGOTIATE_DONE:
            context->success_callback(el, fd, context->connection, mask);
            return;
        default:
            serverAssert(0);
    }
}

/**
 * Helper method for SSL negotiation. This is a generic method which abstracts the logic
 * of SSL negotiation so that it can be reusable by all places where SSL negotiation needs to
 * happen. Invoker just needs to handle error conditions - NEGOTIATE_FAILED and in case of success,
 * NEGOTIATE_DONE perform any post negotiation handling
 */
SslNegotiationStatus
sslNegotiate(aeEventLoop *el, ssl_connection *ssl_conn, void *privdata) {

    serverLog(LL_DEBUG, "resuming SSL negotiation");
    s2n_blocked_status blocked;
    if (s2n_negotiate(ssl_conn->s2nconn, &blocked) < 0) {
        if (s2n_error_get_type(s2n_errno) == S2N_ERR_T_BLOCKED) {
            /* Blocked, come back later */
            serverLog(LL_DEBUG, "SSL Negotiation is blocked on IO: %s : %s : %d. Will resume soon",
                    s2n_strerror(s2n_errno, "EN"), s2n_strerror_debug(s2n_errno, "EN"), blocked);
            return updateEventHandlerForSslHandshake(blocked, el, ssl_conn->fd, privdata) == C_OK
                    ? NEGOTIATE_RETRY
                    : NEGOTIATE_FAILED;
        } else {
             /* Everything else */
            serverLog(LL_WARNING, "SSL Negotiation unsuccessful due to Error: %s: %s : %s",
                        s2n_strerror(s2n_errno, "EN"), s2n_strerror_debug(s2n_errno, "EN"), strerror(errno));
            serverLog(LL_DEBUG, "Deleting SSL negotiation event handler to stop further invocations");
                
            //stop further invocations of this method
            aeDeleteFileEvent(el, ssl_conn->fd, AE_READABLE | AE_WRITABLE);
            return NEGOTIATE_FAILED;
        }
    }
    //if we are here, it means SSL negotiation is complete and successful
    serverLog(LL_DEBUG, "negotiation done successfully with cipher: %s", s2n_connection_get_cipher(ssl_conn->s2nconn));
    aeDeleteFileEvent(el, ssl_conn->fd, AE_READABLE | AE_WRITABLE);
    return NEGOTIATE_DONE;
}

/*
 * Initialize SSL configuration to act as client
 * (e.g. replication client, cluster bus client)
 */
int initClientSslConfig(ssl_t *ssl) {
    if (ssl->enable_ssl && ssl->client_ssl_config == NULL) {

        ssl->client_ssl_config = initSslConfigForClient(ssl->ssl_cipher_prefs,
                                                           ssl->ssl_certificate,
                                                           ssl->root_ca_certs_path);

        if (!ssl->client_ssl_config) {
            serverLog(LL_WARNING, "Error initializing client SSL configuration");
            return C_ERR;
        }
    }
    return C_OK;
}

/**
 * Initializes SSL configuration to act as server
 * (e.g. replication master, cluster bus master, query processor server)
 */
int initServerSslConfig(ssl_t *ssl) {
    if (ssl->enable_ssl && ssl->server_ssl_config == NULL) {
        ssl->server_ssl_config = initSslConfigForServer(ssl->ssl_certificate, ssl->ssl_certificate_private_key,
                                                           ssl->ssl_dh_params, ssl->ssl_cipher_prefs);
        if (!ssl->server_ssl_config) {
            serverLog(LL_WARNING, "Error initializing server SSL configuration");
            return C_ERR;
        }
        ssl->server_ssl_config_creation_time = time(NULL);
    }
    return C_OK;
}


static struct s2n_config *
initSslConfigForServer(const char *certificate, const char *privateKey, const char *dhParams,
                       const char *cipherPrefs) {
    return initSslConfig(1, certificate, privateKey, dhParams, cipherPrefs, NULL);
}

static struct s2n_config *
initSslConfigForClient(const char *cipher_prefs,
                       const char *certificate, const char *rootCACertificatesPath) {
    return initSslConfig(0, certificate, NULL, NULL, cipher_prefs, rootCACertificatesPath);
}

static struct s2n_config *
initSslConfig(int is_server, const char *certificate, const char *private_key, const char *dh_params,
              const char *cipher_prefs, const char *rootCACertificatesPath) {
    serverLog(LL_DEBUG, "Initializing %s SSL configuration", is_server ? "Server" : "Client");
    struct s2n_config *ssl_config = s2n_config_new();
    if (!ssl_config) {
        serverLog(LL_WARNING, "Error getting new s2n config: '%s'.", s2n_strerror(s2n_errno, "EN"));
        return NULL;
    }

    if (is_server && s2n_config_add_cert_chain_and_key(ssl_config, certificate,
                                                               private_key) < 0) {
        serverLog(LL_WARNING, "Error adding certificate/key to s2n config: '%s'.",
                  s2n_strerror(s2n_errno, "EN"));
        goto config_error;
    }

    if (is_server && s2n_config_add_dhparams(ssl_config, dh_params) < 0) {
        serverLog(LL_WARNING, "Error adding DH parameters to s2n config: '%s'.",
                  s2n_strerror(s2n_errno, "EN"));
        goto config_error;
    }

    /* Load the root ca certificate */
    if(!is_server && 
            s2n_config_set_verification_ca_location(ssl_config, NULL, rootCACertificatesPath) < 0) {
        serverLog(LL_WARNING, "Error while loading CA certificates into s2n: '%s'.", s2n_strerror(s2n_errno, "EN"));
        goto config_error;
    }

    /** 
     * Load the intermediate nodes from the provided certificate file, this will also load the leaf nodes
     * but they will be unused.
    */
    if(!is_server && s2n_config_add_pem_to_trust_store(ssl_config, certificate) < 0) {
        serverLog(LL_WARNING, "Error while loading SSL certificate into s2n: '%s'.", s2n_strerror(s2n_errno, "EN"));
        goto config_error;        
    }
    
    if(!is_server && s2n_config_set_verify_host_callback(ssl_config, s2nVerifyHost, ssl_config) < 0){
        serverLog(LL_WARNING, "Error while setting host verify callback: '%s'.", s2n_strerror(s2n_errno, "EN"));               
        goto config_error;            
    }

    if (s2n_config_set_cipher_preferences(ssl_config, cipher_prefs) < 0) {
        serverLog(LL_WARNING, "Error setting cipher prefs on s2n config: '%s'.",
                  s2n_strerror(s2n_errno, "EN"));
        goto config_error;
    }

    return ssl_config;

config_error:
    if (s2n_config_free(ssl_config) < 0)
        serverLog(LL_WARNING, "Error freeing server SSL configuration");
    return NULL;
}

/**
 * Disconnect any clients that are still using old certificate and mark all
 * of the connections as using the older connection so that the count of 
 * connections is accurate.
 */
static void updateClientsUsingOldCertificate(ssl_t *ssl_config) {
    UNUSED(ssl_config);
    // TOOD: Just a place holder not needed for POC
}

/* A task that will execute in every event loop iteration and invoke the read handler of any SSL connection
 * for which S2N has cached application data. */
int processRepeatedReads(struct aeEventLoop *eventLoop, long long id, void *privdata) {
    UNUSED(id);
    ssl_t *ssl_config = (ssl_t *) privdata;

    if (!ssl_config->enable_ssl || listLength(ssl_config->sslconn_with_cached_data) == 0) {
        ssl_config->repeated_reads_task_id = AE_ERR;
        return AE_NOMORE;
    }

    // Create a copy of our list so it can be modified arbitrarily during read handler execution
    list *copy = listDup(ssl_config->sslconn_with_cached_data);

    // Record maximum list length
    if (listLength(copy) > ssl_config->max_repeated_read_list_length) {
        ssl_config->max_repeated_read_list_length = listLength(copy);
    }

    listNode *ln;
    listIter li;

    listRewind(copy, &li);
    while((ln = listNext(&li))) {
        ssl_connection *conn = ln->value;
        // If the descriptor is not processing read events, skip it this time and check next time.  It will remain on our list until
        // drained.
        if (aeGetFileEvents(eventLoop, conn->fd) & AE_READABLE) {
            // The read handler is expected to remove itself from the repeat read list when there is no longer cached data
            aeGetFileProc(eventLoop, conn->fd, AE_READABLE)(eventLoop, conn->fd, aeGetClientData(eventLoop, conn->fd), AE_READABLE);
            ssl_config->total_repeated_reads++;
        }
    }

    listRelease(copy);

    if (listLength(ssl_config->sslconn_with_cached_data) == 0) {
        /* No more cached data left */
        ssl_config->repeated_reads_task_id = AE_ERR;
        return AE_NOMORE;
    } else {
        return 0; /* Run as fast as possible without sleeping next time around */
    }
}

/* Queue an SSL connection to have its read handler invoked outside of the normal
 * socket notification events in case we do not receive one because there is cached
 * application data inside S2N.  If already queued, will do nothing.  The handler
 * will be repeatedly invoked until removeRepeatedRead is called. */
void addRepeatedRead(ssl_connection *conn) {
    if (conn->cached_data_node != NULL) {
        return;
    }

    listAddNodeTail(conn->ssl_config->sslconn_with_cached_data, conn);
    conn->cached_data_node = listLast(conn->ssl_config->sslconn_with_cached_data);

    if (conn->ssl_config->repeated_reads_task_id == AE_ERR) {
        // Schedule the task to process the list
        conn->ssl_config->repeated_reads_task_id = aeCreateTimeEvent(conn->ssl_config->event_loop, 0, processRepeatedReads, NULL, NULL);
        if (conn->ssl_config->repeated_reads_task_id == AE_ERR) {
            serverLog(LL_WARNING, "Can't create the processRepeatedReads time event.");
        }
    }
}

/* Remove the SSL connection from the queue of repeated read handlers if it exists.
 * One must call this to stop subsequent repeated reads. */
void removeRepeatedRead(ssl_connection *conn) {
    if (conn->cached_data_node == NULL) {
        return;
    }

    listDelNode(conn->ssl_config->sslconn_with_cached_data, conn->cached_data_node);
    conn->cached_data_node = NULL;

    // processRepeatedReads task is responsible for self-terminating when no more reads
}


/* -------------------------- public function definitions ---------------------------- */

/**
 * Initialize default values for SSL related global variables. It should be
 * invoked at Redis startup to provide sane default values to SSL related
 * variables
 */
void initSslConfigDefaults(ssl_t *ssl_config) {
    ssl_config->enable_ssl = SSL_ENABLE_DEFAULT;
    ssl_config->ssl_certificate = NULL;
    ssl_config->ssl_certificate_file = NULL;
    ssl_config->ssl_certificate_private_key = NULL;
    ssl_config->ssl_certificate_private_key_file = NULL;
    ssl_config->ssl_dh_params = NULL;
    ssl_config->ssl_dh_params_file = NULL;
    ssl_config->ssl_cipher_prefs = SSL_CIPHER_PREFS_DEFAULT;
    ssl_config->server_ssl_config = NULL;
    ssl_config->server_ssl_config_creation_time = 0;
    ssl_config->ssl_performance_mode = SSL_PERFORMANCE_MODE_DEFAULT;
    ssl_config->client_ssl_config = NULL;
    ssl_config->server_ssl_config_old = NULL;
    ssl_config->root_ca_certs_path = NULL;
    ssl_config->sslconn_with_cached_data = NULL;
    ssl_config->repeated_reads_task_id = AE_ERR;
    ssl_config->total_repeated_reads = 0;
    ssl_config->max_repeated_read_list_length = 0;
    ssl_config->expected_hostname = NULL;
    ssl_config->certificate_not_after_date = NULL;
    ssl_config->certificate_not_before_date = NULL;
    ssl_config->connections_to_current_certificate = 0;
    ssl_config->connections_to_previous_certificate = 0;
    ssl_config->certificate_serial = 0;
}


/**
 * Initializes any global level resource required for SSL. This method
 * should be invoked at startup time
 */
void initSsl(ssl_t *ssl) {
    if (ssl->enable_ssl == true) {
        serverLog(LL_NOTICE, "Initializing SSL configuration");
        setenv("S2N_ENABLE_CLIENT_MODE", "1", 1);
        // MLOCK is used to keep memory from being moved to SWAP. However, S2N can run into 
        // kernel limits for the number distinct mapped ranges assocciated to a process when
        // a large number of clients are connected. Failed mlock calls will not free memory, 
        // so pages will not get unmapped until the engine is rebooted. In order to avoid this, 
        // we are unconditionally disabling MLOCK.
        setenv("S2N_DONT_MLOCK", "1", 1);
        if (s2n_init() < 0) {
            serverLog(LL_WARNING, "Error running s2n_init(): '%s'. Exiting", s2n_strerror(s2n_errno, "EN"));
            serverAssert(0);
        }
        //initialize openssl error strings
        SSL_load_error_strings();
        //required for cert validation
        OpenSSL_add_all_algorithms();
        
        //initialize configuration for Redis to act as Server (regular mode and cluster bus server)
        if (initServerSslConfig(ssl) == C_ERR) {
            serverLog(LL_WARNING, "Error initializing server SSL configuration. Exiting.");
            serverAssert(0);
        }

        //initialize configuration for Redis to act as client (Replica and cluster bus client)
        if (initClientSslConfig(ssl) == C_ERR) {
            serverLog(LL_WARNING, "Error initializing client SSL configuration. Exiting.");
            serverAssert(0);
        }
        // The expected hostname from the certificate to use as part of hostname validation
        ssl->expected_hostname = zmalloc(CERT_CNAME_MAX_LENGTH);
        if (getCnameFromCertificate(ssl->ssl_certificate, ssl->expected_hostname) == C_ERR) {
            serverLog(LL_WARNING, "Error while discovering expected hostname from certificate file");        
            serverAssert(0);
        }
        
        // Allocate space for not before and not after dates
        ssl->certificate_not_after_date = zmalloc(CERT_DATE_MAX_LENGTH);
        ssl->certificate_not_before_date = zmalloc(CERT_DATE_MAX_LENGTH);

        if (updateServerCertificateInformation(ssl->ssl_certificate, 
                ssl->certificate_not_before_date, ssl->certificate_not_after_date,
                &(ssl->certificate_serial)) == C_ERR) {
            serverLog(LL_WARNING, "Error while discovering not_after and not_before from certificate file");        
            serverAssert(0);         
        }
        
        ssl->sslconn_with_cached_data = listCreate();
    }
}

/**
 * Cleans any global level resources used by SSL. This method
 * should be invoked at shutdown time
 */
void cleanupSsl(ssl_t *ssl) {
    if (ssl->enable_ssl) {
        if (s2n_cleanup() < 0)
            serverLog(LL_WARNING, "Error cleaning up SSL resources: %s", s2n_strerror(s2n_errno, "EN"));
        if (s2n_config_free(ssl->server_ssl_config) < 0)
            serverLog(LL_WARNING, "Error freeing server SSL config: %s", s2n_strerror(s2n_errno, "EN"));
        if (s2n_config_free(ssl->client_ssl_config) < 0)
            serverLog(LL_WARNING, "Error freeing client SSL config: %s", s2n_strerror(s2n_errno, "EN"));
        ERR_free_strings();
        //removes all ciphers and digests from internal table of digest algorithms and ciphers
        EVP_cleanup();

        listRelease(ssl->sslconn_with_cached_data);
        zfree(ssl->expected_hostname);
        zfree(ssl->certificate_not_after_date);
        zfree(ssl->certificate_not_before_date);
    }
}

/**
 * Returns true if fd_to_ssl_conn can be resized from cur_size to new_size
 */
int isResizeAllowed(ssl_connection **fd_to_ssl_conn, int cur_size, int new_size) {
    int max_fd = -1;
    for (int i = cur_size - 1; i >= 0; i--) {
        if (fd_to_ssl_conn[i] != NULL) {
            max_fd = i;
            break;
        }
    }
    return max_fd < new_size;
}

/**
 * Update the certificate/private key pair used by SSL. This method can be used to
 * renew the expiring certificate without bouncing Redis
 */
int renewCertificate(ssl_t *ssl_config, char *new_certificate, char *new_private_key, char *new_certificate_filename,
                     char *new_private_key_filename) {
    serverLog(LL_NOTICE, "Initializing SSL configuration for new certificate");
    struct s2n_config *new_config = initSslConfigForServer(new_certificate, new_private_key,
                                                           ssl_config->ssl_dh_params,
                                                           ssl_config->ssl_cipher_prefs);
    if (new_config == NULL) {
        serverLog(LL_DEBUG, "Error creating SSL configuration using new certificate");
        return C_ERR;
    }

    char *newNotBeforeDate = zmalloc(CERT_DATE_MAX_LENGTH);    
    char *newNotAfterDate = zmalloc(CERT_DATE_MAX_LENGTH);
    long newSerial = 0;

    // Update the not before and not after date provided in info
    if (updateServerCertificateInformation(new_certificate, newNotBeforeDate, newNotAfterDate, &newSerial) != C_OK) {
        serverLog(LL_DEBUG, "Failed to read not_before and not_after date from new certificate");
        zfree(newNotBeforeDate);
        zfree(newNotAfterDate);
        return C_ERR;
    }

    //After we have validated that new cert is valid, disconnect any
    //clients using the oldest certificate. We don't want to have more that
    // 2 certificates in use at a time. We proactively disconnect any
    //clients using oldest certificate to stay within 2 certificate limit
    updateClientsUsingOldCertificate(ssl_config);

    //save the SSL configuration for the expiring certificate
    //We gotta keep it as there are existing connections using this configuration
    ssl_config->server_ssl_config_old = ssl_config->server_ssl_config;

    //start using new configuration. Any new connections
    //will start using new certificate from this point onwards
    ssl_config->server_ssl_config = new_config;
    ssl_config->server_ssl_config_creation_time = time(NULL);

    //free the memory used by old stuff
    zfree((void *) ssl_config->ssl_certificate);
    zfree((void *) ssl_config->ssl_certificate_file);
    zfree((void *) ssl_config->ssl_certificate_private_key);
    zfree((void *) ssl_config->ssl_certificate_private_key_file);
    zfree((void *) ssl_config->certificate_not_before_date);    
    zfree((void *) ssl_config->certificate_not_after_date);

    //save the references to the new stuff
    ssl_config->ssl_certificate = new_certificate;
    ssl_config->ssl_certificate_file = new_certificate_filename;

    ssl_config->ssl_certificate_private_key = new_private_key;
    ssl_config->ssl_certificate_private_key_file = new_private_key_filename;

    ssl_config->certificate_not_before_date = newNotBeforeDate;
    ssl_config->certificate_not_after_date = newNotAfterDate;
    ssl_config->certificate_serial = newSerial;
    
    // Update the connection count for redis info
    ssl_config->connections_to_previous_certificate = ssl_config->connections_to_current_certificate;
    ssl_config->connections_to_current_certificate = 0;

    serverLog(LL_NOTICE, "Successfully renewed SSL certificate");

    return C_OK;
}


/**
 * Creates and initializes an SSL connection. It performs following critical functions on a connection
 * so that it is usable by Redis
 *  - create a new connection in Server or Client mode
 *  - Associates appropriate configuration with the connection
 *  - Associates appropriate socket file descriptor with the connection
 *  - Set a performance mode on the connection
 *  - Create an entry for Socket FD to SSL connection mapping
 */
ssl_connection *
createSslConnection(connectionType connection_mode, ssl_t *config, int fd, int ssl_performance_mode, char *masterhost) {

    ssl_connection *sslconn = zmalloc(sizeof(ssl_connection));
    if (!sslconn) {
        serverLog(LL_WARNING, "Error creating new ssl connection.");
        return NULL;
    }
    sslconn->connection_flags = 0;
    sslconn->cached_data_node = NULL;
    sslconn->ssl_config = config;
    sslconn->fd = fd;

    // create a new connection in Server or Client mode
    sslconn->s2nconn = s2n_connection_new(connection_mode);
    if (!sslconn->s2nconn) {
        serverLog(LL_WARNING, "Error creating new s2n connection. Error: '%s'", s2n_strerror(s2n_errno, "EN"));
        goto error;
    }

    // Associates appropriate configuration with the connection
    if (s2n_connection_set_config(sslconn->s2nconn, config->server_ssl_config) < 0) {
        serverLog(LL_WARNING, "Error setting configuration on s2n connection. Error: '%s'",
                  s2n_strerror(s2n_errno, "EN"));
        goto error;
    }
    // Associates appropriate socket file descriptor with the connection
    if (s2n_connection_set_fd(sslconn->s2nconn, fd) < 0) {
        serverLog(LL_WARNING, "Error setting socket file descriptor: %d on s2n connection. Error:'%s'", fd,
                  s2n_strerror(s2n_errno, "EN"));
        goto error;
    }
    //disable blinding. Blinding could lead to Redis sleeping upto to 10s which is not desirable in a
    //single threaded application
    if(s2n_connection_set_blinding(sslconn->s2nconn, S2N_SELF_SERVICE_BLINDING) < 0) {
        serverLog(LL_WARNING, "Error setting blinding mode: S2N_SELF_SERVICE_BLINDING on s2n connection. Error:'%s'",
                  s2n_strerror(s2n_errno, "EN"));
        goto error;
    }
    // Set a performance mode on the connection
    switch (ssl_performance_mode) {
        case SSL_PERFORMANCE_MODE_HIGH_THROUGHPUT:
            if (s2n_connection_prefer_throughput(sslconn->s2nconn) < 0) {
                serverLog(LL_WARNING, "Error setting performance mode of high throughput on SSL connection");
                goto error;
            }
            break;
        case SSL_PERFORMANCE_MODE_LOW_LATENCY:
            if (s2n_connection_prefer_low_latency(sslconn->s2nconn) < 0) {
                serverLog(LL_WARNING, "Error setting performance mode of low latency on SSL connection");
                goto error;
            }
            break;
        default:
            serverLog(LL_DEBUG, "Invalid SSL performance mode: %d", ssl_performance_mode);
            goto error;

    }

    //Set master host on the ssl connection
    if (connection_mode == S2N_CLIENT && masterhost != NULL && s2n_set_server_name(sslconn->s2nconn, masterhost) < 0) {
        serverLog(LL_WARNING, "Error setting server name on s2n connection: '%s'", s2n_strerror(s2n_errno, "EN"));
        goto error;
    }

    // Create an entry for Socket FD to SSL connection mapping
    serverLog(LL_DEBUG, "SSL Connection setup successfully for fd %d", fd);
    return sslconn;

error:
    releaseSslConnection(sslconn);
    return NULL;
}

/**
 * This method should be used to cleanup a connection. It will shutdown the
 * SSL connection (sends a SHUTDOWN TLS alert) for secure shutdown, free
 * the memory consumed by connection and delete the mapping from Socket FD
 * to this connection
 */
int releaseSslConnection(struct ssl_connection *conn) {
    int ret = C_OK;
    if (conn->connection_flags & OLD_CERTIFICATE_FLAG) {
        conn->ssl_config->connections_to_previous_certificate--;
    } else {
        conn->ssl_config->connections_to_current_certificate--;
    }

    
    // Don't shutdown if we haven't even initialized anything
    if(s2n_connection_get_client_hello(conn->s2nconn) != NULL){
        shutdownSslConnection(conn);                        
    }

    if (conn != NULL) {
        if (conn->s2nconn != NULL) {
            /*
             * Just doing s2n_connection_free is not sufficient in production.
             * s2n_connection_wipe calls s2n_connection_wipe_io which frees
             * some memory allocated. Just doing s2n_connection_free
             * was causing a memory leak reported by valgrind and after a while, redis
             * would stop accepting new connections
             */            
            if(s2n_connection_wipe(conn->s2nconn) < 0) {
                serverLog(LL_WARNING, "Error wiping connection: '%s'", s2n_strerror(s2n_errno, "EN"));
            }

            if (s2n_connection_free(conn->s2nconn) < 0) {
                serverLog(LL_WARNING, "Error freeing connection: '%s'", s2n_strerror(s2n_errno, "EN"));
                ret = C_ERR;
            }
        }
        if (conn->cached_data_node != NULL) {
            removeRepeatedRead(conn);
        }
        zfree(conn);
    } 
    return C_OK;
}

// This will install all needed handlers to continue
void sslStartNegotiation(ssl_connection *ssl_conn, aeEventLoop *el, aeFileProc *successcallback, aeFileProc *failurecallback) {
    sslNegotiationContext *context = zmalloc(sizeof(sslNegotiationContext));
    context->success_callback = successcallback;
    context->failure_callback = failurecallback;
    context->connection = ssl_conn;
    aeCreateFileEvent(el, ssl_conn->fd, AE_WRITABLE, sslNegotiationHandler, context);
}

/**
 * SSL compatible read IO method. It will automatically detect if
 * SSL is enabled or not and accordingly use standard
 * read/write or SSL based IO methods. Special note - It is made
 * inline for performance reasons
 */
inline ssize_t sslRead(ssl_connection *ssl_conn, void *buffer, size_t nbytes) {
    s2n_blocked_status blocked;
    ssize_t bytesRead = sslRecv(ssl_conn, buffer, nbytes, &blocked);
    if (bytesRead > 0 && blocked == S2N_BLOCKED_ON_READ) {
        /* Data was returned, but we didn't consume an entire frame, so signal that we need to repeat the event handler. */
        addRepeatedRead(ssl_conn);
    } else {
        /* Either the entire frame was consumed, or nothing was returned because we were blocked on a socket read. */
        removeRepeatedRead(ssl_conn);
    }

    return bytesRead;
}

/**
 * SSL compatible write IO method. It will automatically detect if
 * SSL is enabled or not and accordingly used standard
 * read/write or SSL based IO methods. Special note - It is made
 * inline for performance reasons
 */
inline ssize_t sslWrite(ssl_connection *ssl_conn, const void *buffer, size_t nbytes) {
    s2n_errno = S2N_ERR_T_OK;
    errno = 0;

    s2n_blocked_status blocked;
    // if (ssl_conn->connection_flags & NEWLINE_PING_IN_PROGRESS_FLAG) {
    //     /* We previously called sslPing and it didn't fully complete the request!
    //         * We need to flush out that request before continuing since s2n is stateful. */
    //     ssize_t r = s2n_send(ssl_conn->s2nconn, "\n", 1, &blocked);
    //     if (r < 0) {
    //         // Still didn't succeed

    //         if (s2n_error_get_type(s2n_errno) == S2N_ERR_T_BLOCKED) {
    //             errno = EAGAIN;
    //         }

    //         return r;
    //     }

    //     // Success! Continue to our actual request.
    //     ssl_conn->connection_flags &= ~NEWLINE_PING_IN_PROGRESS_FLAG;
    // }

    ssize_t r = s2n_send(ssl_conn->s2nconn, buffer, nbytes, &blocked);

    //set errno as well in case IO blocked. This is so that calling code treats
    //it like regular blocking IO and does not has to do any special logic for SSL based IO
    if (r < 0 && s2n_error_get_type(s2n_errno) == S2N_ERR_T_BLOCKED) errno = EAGAIN;
    return r;
}

/**
 * Converts SSL performance mode string to corresponding integer constant.
 */
int getSslPerformanceModeByName(char *name) {
    if (!strcasecmp(name, "low-latency")) return SSL_PERFORMANCE_MODE_LOW_LATENCY;
    else if (!strcasecmp(name, "high-throughput")) return SSL_PERFORMANCE_MODE_HIGH_THROUGHPUT;
    else return -1;
}

/**
 * Converts SSL performance mode integer to corresponding str
 */
char *getSslPerformanceModeStr(int mode) {
    if (mode == SSL_PERFORMANCE_MODE_LOW_LATENCY) return "low-latency";
    else if (mode == SSL_PERFORMANCE_MODE_HIGH_THROUGHPUT) return "high-throughput";
    else return "invalid input";
}

/**
 * SSL compatible IO error string method. It will automatically detect if
 * SSL is enabled or not and accordingly use standard strerror method
 * or s2n_strerror for SSL related errors
 */
inline const char *sslstrerror(void) {
    if (s2n_error_get_type(s2n_errno) == S2N_ERR_T_IO) {
        //S2N_ERR_T_IO => underlying I/O operation failed, check system errno
        //therefore in this case, returning System IO error string
        return strerror(errno);
    } else {
        return s2n_strerror(s2n_errno, "EN");
    }
}
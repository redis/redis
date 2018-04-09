#ifndef __SSL_PROXY_H
#define __SSL_PROXY_H

#include "ae.h"
#include "anet.h"
#include "ssl.h"

#include <pthread.h>

/* Anti-warning macro... */
#define UNUSED(V) ((void) V)

#define BUFFER_SIZE 16556 

typedef struct sslProxy {
    char *application_socket; /* The socket the proxy thread will open new inbound connections too */

    char neterr[ANET_ERR_LEN];   /* Error buffer for anet.c */
    aeEventLoop *el; /* The event loop that will process the proxy process */
    pthread_t proxy_thread; /* The pthread used by the proxy for which the main thread can join on */
    pthread_mutex_t proxy_mutex; /* Single mutex for updated the sslProxy object. Used to update proxy from main thread */
    int running; /* A flag used to indicate if the proxy should continue running */

    int *fds; /* Initialization values for fds to listen too. Must already be listening */
    int fdcount; /* count of fds */

    ssl_t *config; /* ssl configuration for which to establish new connections on */
} sslProxy;

typedef struct sslProxyConnection {
    sds application_buffer; /* Buffer used between the proxy and redis main thread, data into and out of this buffer is never encrypted. */
    sds client_buffer; /* Buffer used between the proxy and incoming clients, data into and out of this buffer is always encrypted. */

    sslProxy *proxy;
    int client_fd; /* Only used for adding file events when needing to write to client. All underlying IO is handled by ssl_connection */
    int application_fd; /* Reference to the application fd. This is needed for cleanup when not a socket error. */
    ssl_connection *ssl_connection; /* Pointer to the underlying SSL connection backing the client fd. The proxy connection has ownership
                                       of the fd only until the ssl communication is completed, then it is handed off to the underlying
                                       ssl connection. */
} sslProxyConnection;

sslProxy *createSslProxy(char *socket, int *listeningfds, int fdcount, ssl_t *ssl_config, int maxclients);
void releaseSslProxy(sslProxy *proxy);

int sslProxyStart(sslProxy *proxy);
int sslProxyStop(sslProxy *proxy);

#endif
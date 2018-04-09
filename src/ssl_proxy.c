#include "server.h" // Only for logging statements

#include "ae.h"
#include "sds.h"
#include "ssl_proxy.h"
#include "zmalloc.h"
#include "ssl.h"

#include <stdio.h> 
#include <stdlib.h> 
#include <pthread.h> 
#include <unistd.h>

// Private functions
void sslProxyApplicationReadHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void sslProxyClientReadHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void sslProxyApplicationWriteHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void sslProxyClientWriteHandler(aeEventLoop *el, int fd, void *privdata, int mask);

void handleNegotiationFailure(aeEventLoop *el, int fd, void *privdata, int mask);
void handleNegotiationSuccess(aeEventLoop *el, int fd, void *privdata, int mask);

// It seems like ae should support some private data 
static __thread sslProxy *threadProxy;

// --------------------- Internal interface --------------------------

//
// <--> | proxy | <--> | redis
// 1. Write to internet
// 2. Read from internet
// 3. Write to redis
// 4. Read from redis

void sslProxyReleaseConnection(sslProxyConnection *connection) {
    sdsfree(connection->application_buffer);
    sdsfree(connection->client_buffer);

    aeDeleteFileEvent(connection->proxy->el, connection->client_fd, AE_READABLE | AE_WRITABLE);
    aeDeleteFileEvent(connection->proxy->el, connection->application_fd, AE_READABLE | AE_WRITABLE);
    close(connection->application_fd);
    releaseSslConnection(connection->ssl_connection);
}

void sslProxyCreateConnection(sslProxy *proxy, int clientfd) {
    ssl_connection *ssl_conn = createSslConnection(SSL_SERVER, proxy->config, clientfd, SSL_PERFORMANCE_MODE_HIGH_THROUGHPUT, NULL);
    sslStartNegotiation(ssl_conn, proxy->el, handleNegotiationSuccess, handleNegotiationFailure);

    // Return nothing, as the callback will be executed on the event loop once it's done
}

void handleNegotiationFailure(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(privdata);
    UNUSED(mask);
    close(fd);
}

void handleNegotiationSuccess(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(mask);

    int appfd = anetUnixNonBlockConnect(threadProxy->neterr, threadProxy->application_socket);
    if (appfd == -1){
        close(fd);
        return;
    }
    serverLog(LL_DEBUG, "Accepted connection");

    sslProxyConnection *proxy_connection = zmalloc(sizeof(sslProxyConnection));
    proxy_connection->application_buffer = sdsempty();
    proxy_connection->client_buffer = sdsempty();
    proxy_connection->proxy = threadProxy;
    proxy_connection->client_fd = fd;
    proxy_connection->application_fd = appfd;
    proxy_connection->ssl_connection = (ssl_connection *) privdata;

    aeCreateFileEvent(el, fd, AE_READABLE, sslProxyClientReadHandler, proxy_connection);
    aeCreateFileEvent(el, appfd, AE_READABLE, sslProxyApplicationReadHandler, proxy_connection);
    sslProxyClientReadHandler(el, fd, proxy_connection, AE_READABLE);
    serverLog(LL_DEBUG, "Done with setup");
}

// Read handler for redis -> proxy
void sslProxyApplicationReadHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(mask);
    sslProxyConnection *connection = (sslProxyConnection *) privdata;

    char buffer[BUFFER_SIZE];
    char *bufferp = buffer;

    size_t nread = read(fd, buffer, BUFFER_SIZE);

    if (nread <= 0) {
        sslProxyReleaseConnection(connection);
        return;
    }
    
    // Best case, we have no pending data so transfer it all to the other buffer
    if (sdslen(connection->client_buffer) == 0) {
        size_t nwrite = sslWrite(connection->ssl_connection, buffer, nread);
        if (nwrite == nread) return;
        bufferp += nwrite;
        nread -= nwrite;
    }
    connection->client_buffer = sdscatlen(connection->client_buffer, bufferp, nread);

    // Reads in from the application get forwarded to the client
    aeCreateFileEvent(el, connection->client_fd, AE_WRITABLE, sslProxyClientWriteHandler, connection);
}

void sslProxyClientReadHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(mask);
    UNUSED(fd);
    sslProxyConnection *connection = (sslProxyConnection *) privdata;
    char buffer[BUFFER_SIZE];
    char *bufferp = buffer;

    size_t nread = sslRead(connection->ssl_connection, buffer, BUFFER_SIZE);
    if (nread <= 0) {
        sslProxyReleaseConnection(connection);
        return;
    }

    // Best case, we have no pending data so transfer it all to the other buffer
    if (sdslen(connection->application_buffer) == 0) {
        size_t nwrite = write(connection->application_fd, buffer, nread);
        if (nwrite == nread) return;
        bufferp += nwrite;
        nread -= nwrite;
    }
    connection->application_buffer = sdscatlen(connection->application_buffer, bufferp, nread);

    // Reads in from the client get forwarded to the application
    aeCreateFileEvent(el, connection->application_fd, AE_WRITABLE, sslProxyApplicationWriteHandler, connection);
}

// Write handler for proxy -> redis
void sslProxyApplicationWriteHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(mask);
    sslProxyConnection *connection = (sslProxyConnection *) privdata;
    size_t nwritten = write(fd, connection->application_buffer, sdslen(connection->application_buffer));

    if (nwritten <= 0) {
        sslProxyReleaseConnection(connection);
        return;
    }

    sdsrange(connection->application_buffer, nwritten, -1);
    if (sdslen(connection->application_buffer) == 0)
        aeDeleteFileEvent(el, fd, AE_WRITABLE);
}


void sslProxyClientWriteHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(mask);
    sslProxyConnection *connection = (sslProxyConnection *) privdata;
    size_t nwritten = sslWrite(connection->ssl_connection, connection->client_buffer, sdslen(connection->client_buffer));
    
    if (nwritten <= 0) {
        sslProxyReleaseConnection(connection);
        return;
    }

    sdsrange(connection->client_buffer, nwritten, -1);
    if (sdslen(connection->client_buffer) == 0)
        aeDeleteFileEvent(el, fd, AE_WRITABLE);
}

void sslProxyAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    sslProxy *proxy = (sslProxy *) privdata;
    int cport, cfd, max = 10;
    char cip[1000];
    UNUSED(el);
    UNUSED(mask);

    while(max--) {
        cfd = anetTcpAccept(proxy->neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            return;
        }
        anetNonBlock(NULL, cfd);
        anetEnableTcpNoDelay(NULL,fd);
        sslProxyCreateConnection(proxy, cfd);
    }
}

int proxyCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);
    if (!threadProxy->running) {
        // TODO cleanup
        aeStop(eventLoop);
    }
    return 100;
}

void proxyBeforeSleep(struct aeEventLoop *eventLoop) {
    pthread_mutex_unlock(&(threadProxy->proxy_mutex));
    UNUSED(eventLoop);
}

void proxyAfterSleep(struct aeEventLoop *eventLoop) {
    pthread_mutex_lock(&(threadProxy->proxy_mutex));
    UNUSED(eventLoop);
}

// A normal C function that is executed as a thread  
// when its name is specified in pthread_create() 
void *sslProxyStartThread(void *arg) { 
    sslProxy *proxy = (sslProxy*) arg;
    threadProxy = proxy;
    for(int i = 0; i < proxy->fdcount; i++){
        aeCreateFileEvent(proxy->el, proxy->fds[i], AE_READABLE, sslProxyAcceptHandler, proxy);
    }

    aeCreateTimeEvent(proxy->el, 1, proxyCron, NULL, NULL);

    aeMain(proxy->el);
    return NULL;
} 

//// ----- Functions called from redis -----
sslProxy *createSslProxy(char *socket, int *listeningfds, int fdcount, ssl_t *ssl_config, int maxclients) {
    sslProxy *proxy = zmalloc(sizeof(sslProxy));
    proxy->el = aeCreateEventLoop(maxclients);
    proxy->application_socket = socket;
    proxy->running = 0;
    proxy->fds = listeningfds;
    proxy->fdcount = fdcount;
    proxy->config = ssl_config;

    pthread_mutex_init(&(proxy->proxy_mutex), NULL);
    aeSetBeforeSleepProc(proxy->el, proxyBeforeSleep);
    aeSetAfterSleepProc(proxy->el, proxyAfterSleep);
    return proxy;
}

void releaseSslProxy(sslProxy *proxy) {
    aeDeleteEventLoop(proxy->el);
    zfree(proxy);
}

// Thread starts the proxy in a separate thread
int sslProxyStart(sslProxy *proxy) {
    pthread_mutex_lock(&(proxy->proxy_mutex));
    proxy->running = 1;
    pthread_create(&(proxy->proxy_thread), NULL, &sslProxyStartThread, proxy); 
    pthread_mutex_unlock(&(proxy->proxy_mutex));
    return 1;
}

// This signals the proxy in the separate thread to stop
int sslProxyStop(sslProxy *proxy) {
    pthread_mutex_lock(&(proxy->proxy_mutex));
    proxy->running = 0;
    pthread_mutex_unlock(&(proxy->proxy_mutex));

    pthread_join(proxy->proxy_thread, NULL);

    return 1;
}
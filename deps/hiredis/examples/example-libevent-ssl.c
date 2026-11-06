#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <hiredis.h>
#include <hiredis_ssl.h>
#include <async.h>
#include <adapters/libevent.h>

void getCallback(redisAsyncContext *c, void *r, void *privdata) {
    redisReply *reply = r;
    if (reply == NULL) return;
    printf("argv[%s]: %s\n", (char*)privdata, reply->str);

    /* Disconnect after receiving the reply to GET */
    redisAsyncDisconnect(c);
}

void connectCallback(const redisAsyncContext *c, int status) {
    if (status != REDIS_OK) {
        printf("Error: %s\n", c->errstr);
        return;
    }
    printf("Connected...\n");
}

void disconnectCallback(const redisAsyncContext *c, int status) {
    if (status != REDIS_OK) {
        printf("Error: %s\n", c->errstr);
        return;
    }
    printf("Disconnected...\n");
}

int main (int argc, char **argv) {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    struct event_base *base = event_base_new();
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s <key> <host> <port> <cert> <certKey> [ca]\n", argv[0]);
        exit(1);
    }

    const char *value = argv[1];
    size_t nvalue = strlen(value);

    const char *hostname = argv[2];
    int port = atoi(argv[3]);

    const char *cert = argv[4];
    const char *certKey = argv[5];
    const char *caCert = argc > 5 ? argv[6] : NULL;

    redisSSLContext *ssl;
    redisSSLContextError ssl_error = REDIS_SSL_CTX_NONE;

    redisInitOpenSSL();

    ssl = redisCreateSSLContext(caCert, NULL,
            cert, certKey, NULL, &ssl_error);
    if (!ssl) {
        printf("Error: %s\n", redisSSLContextGetError(ssl_error));
        return 1;
    }

    redisAsyncContext *c = redisAsyncConnect(hostname, port);
    if (c->err) {
        /* Let *c leak for now... */
        printf("Error: %s\n", c->errstr);
        return 1;
    }
    if (redisInitiateSSLWithContext(&c->c, ssl) != REDIS_OK) {
        printf("SSL Error!\n");
        exit(1);
    }

    redisLibeventAttach(c,base);
    redisAsyncSetConnectCallback(c,connectCallback);
    redisAsyncSetDisconnectCallback(c,disconnectCallback);
    redisAsyncCommand(c, NULL, NULL, "SET key %b", value, nvalue);
    redisAsyncCommand(c, getCallback, (char*)"end-1", "GET key");
    event_base_dispatch(base);

    redisFreeSSLContext(ssl);
    return 0;
}

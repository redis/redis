#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <hiredis.h>
#include <async.h>
#include <adapters/libevent.h>

void getCallback(redisAsyncContext *c, void *r, void *privdata) {
    redisReply *reply = r;
    if (reply == NULL) {
        if (c->errstr) {
            printf("errstr: %s\n", c->errstr);
        }
        return;
    }
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
    signal(SIGPIPE, SIG_IGN);
    struct event_base *base = event_base_new();
    redisOptions options = {0};
    REDIS_OPTIONS_SET_TCP(&options, "127.0.0.1", 6379);
    struct timeval tv = {0};
    tv.tv_sec = 1;
    options.timeout = &tv;


    redisAsyncContext *c = redisAsyncConnectWithOptions(&options);
    if (c->err) {
        /* Let *c leak for now... */
        printf("Error: %s\n", c->errstr);
        return 1;
    }

    redisLibeventAttach(c,base);
    redisAsyncSetConnectCallback(c,connectCallback);
    redisAsyncSetDisconnectCallback(c,disconnectCallback);
    redisAsyncCommand(c, NULL, NULL, "SET key %b", argv[argc-1], strlen(argv[argc-1]));
    redisAsyncCommand(c, getCallback, (char*)"end-1", "GET key");
    event_base_dispatch(base);
    return 0;
}

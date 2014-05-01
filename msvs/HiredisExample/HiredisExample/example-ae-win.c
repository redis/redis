#ifdef _WIN32
#include "..\..\src\Win32_Interop\win32fixes.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "hiredis.h"
#include "async.h"
#include "adapters\ae.h"


/* Put event loop in the global scope, so it can be explicitly stopped */
static aeEventLoop *loop;

static int getCallbackCalls = 0;

void getCallbackContinue(redisAsyncContext *c, void *r, void *privdata) {
    redisReply *reply = r;
    if (reply == NULL) return;
    
    getCallbackCalls++;
    printf("getCallback called %d times.\n", getCallbackCalls);
    printf("argv[%s]: %s\n", (char*)privdata, reply->str);
}

void getCallbackEnd(redisAsyncContext *c, void *r, void *privdata) {
    redisReply *reply = r;
    if (reply == NULL) return;

    getCallbackCalls++;
    printf("getCallback called %d times.\n", getCallbackCalls);
    printf("argv[%s]: %s\n", (char*)privdata, reply->str);

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

    aeStop(loop);
}

int main (int argc, char **argv) {
#ifndef _WIN32
	signal(SIGPIPE, SIG_IGN);
#endif

#ifdef _WIN32
	/* For Win32_IOCP the event loop must be created before the async connect */
	loop = aeCreateEventLoop(1024 * 10);
#endif

    redisAsyncContext *c = redisAsyncConnect("127.0.0.1", 6379);
    if (c->err) {
        /* Let *c leak for now... */
        printf("Error: %s\n", c->errstr);
        return 1;
    }

#ifndef _WIN32
    loop = aeCreateEventLoop(1024*10);
#endif

	redisAeAttach(loop, c);
    redisAsyncSetConnectCallback(c,connectCallback);
    redisAsyncSetDisconnectCallback(c,disconnectCallback);
    redisAsyncCommand(c, NULL, NULL, "SET key %s", argv[argc-1], strlen(argv[argc-1]));
    for (int i = 0; i < 20000; i++) {
        printf("calling get %i times\n", i);
        redisAsyncCommand(c, getCallbackContinue, "0", "GET key");
    }
    redisAsyncCommand(c, getCallbackEnd, "0", "GET key");
    aeMain(loop);
    return 0;
}


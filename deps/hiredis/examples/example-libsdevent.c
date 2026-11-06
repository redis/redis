#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <hiredis.h>
#include <async.h>
#include <adapters/libsdevent.h>

void debugCallback(redisAsyncContext *c, void *r, void *privdata) {
    (void)privdata;
    redisReply *reply = r;
    if (reply == NULL) {
        /* The DEBUG SLEEP command will almost always fail, because we have set a 1 second timeout */
        printf("`DEBUG SLEEP` error: %s\n", c->errstr ? c->errstr : "unknown error");
        return;
    }
    /* Disconnect after receiving the reply of DEBUG SLEEP (which will not)*/
    redisAsyncDisconnect(c);
}

void getCallback(redisAsyncContext *c, void *r, void *privdata) {
    redisReply *reply = r;
    if (reply == NULL) {
        printf("`GET key` error: %s\n", c->errstr ? c->errstr : "unknown error");
        return;
    }
    printf("`GET key` result: argv[%s]: %s\n", (char*)privdata, reply->str);

    /* start another request that demonstrate timeout */
    redisAsyncCommand(c, debugCallback, NULL, "DEBUG SLEEP %f", 1.5);
}

void connectCallback(const redisAsyncContext *c, int status) {
    if (status != REDIS_OK) {
        printf("connect error: %s\n", c->errstr);
        return;
    }
    printf("Connected...\n");
}

void disconnectCallback(const redisAsyncContext *c, int status) {
    if (status != REDIS_OK) {
        printf("disconnect because of error: %s\n", c->errstr);
        return;
    }
    printf("Disconnected...\n");
}

int main (int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    struct sd_event *event;
    sd_event_default(&event);

    redisAsyncContext *c = redisAsyncConnect("127.0.0.1", 6379);
    if (c->err) {
        printf("Error: %s\n", c->errstr);
        redisAsyncFree(c);
        return 1;
    }

    redisLibsdeventAttach(c,event);
    redisAsyncSetConnectCallback(c,connectCallback);
    redisAsyncSetDisconnectCallback(c,disconnectCallback);
    redisAsyncSetTimeout(c, (struct timeval){ .tv_sec = 1, .tv_usec = 0});

    /*
    In this demo, we first `set key`, then `get key` to demonstrate the basic usage of libsdevent adapter.
    Then in `getCallback`, we start a `debug sleep` command to create 1.5 second long request.
    Because we have set a 1 second timeout to the connection, the command will always fail with a
    timeout error, which is shown in the `debugCallback`.
    */

    redisAsyncCommand(c, NULL, NULL, "SET key %b", argv[argc-1], strlen(argv[argc-1]));
    redisAsyncCommand(c, getCallback, (char*)"end-1", "GET key");

    /* sd-event does not quit when there are no handlers registered. Manually exit after 1.5 seconds */
    sd_event_source *s;
    sd_event_add_time_relative(event, &s, CLOCK_MONOTONIC, 1500000, 1, NULL, NULL);

    sd_event_loop(event);
    sd_event_source_disable_unref(s);
    sd_event_unref(event);
    return 0;
}

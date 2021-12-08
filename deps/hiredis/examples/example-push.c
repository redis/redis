/*
 * Copyright (c) 2020, Michael Grunder <michael dot grunder at gmail dot com>
 *
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hiredis.h>
#include <win32.h>

#define KEY_COUNT 5

#define panicAbort(fmt, ...) \
    do { \
        fprintf(stderr, "%s:%d:%s(): " fmt, __FILE__, __LINE__, __func__, __VA_ARGS__); \
        exit(-1); \
    } while (0)

static void assertReplyAndFree(redisContext *context, redisReply *reply, int type) {
    if (reply == NULL)
        panicAbort("NULL reply from server (error: %s)", context->errstr);

    if (reply->type != type) {
        if (reply->type == REDIS_REPLY_ERROR)
            fprintf(stderr, "Redis Error: %s\n", reply->str);

        panicAbort("Expected reply type %d but got type %d", type, reply->type);
    }

    freeReplyObject(reply);
}

/* Switch to the RESP3 protocol and enable client tracking */
static void enableClientTracking(redisContext *c) {
    redisReply *reply = redisCommand(c, "HELLO 3");
    if (reply == NULL || c->err) {
        panicAbort("NULL reply or server error (error: %s)", c->errstr);
    }

    if (reply->type != REDIS_REPLY_MAP) {
        fprintf(stderr, "Error: Can't send HELLO 3 command.  Are you sure you're ");
        fprintf(stderr, "connected to redis-server >= 6.0.0?\nRedis error: %s\n",
                        reply->type == REDIS_REPLY_ERROR ? reply->str : "(unknown)");
        exit(-1);
    }

    freeReplyObject(reply);

    /* Enable client tracking */
    reply = redisCommand(c, "CLIENT TRACKING ON");
    assertReplyAndFree(c, reply, REDIS_REPLY_STATUS);
}

void pushReplyHandler(void *privdata, void *r) {
    redisReply *reply = r;
    int *invalidations = privdata;

    /* Sanity check on the invalidation reply */
    if (reply->type != REDIS_REPLY_PUSH || reply->elements != 2 ||
        reply->element[1]->type != REDIS_REPLY_ARRAY ||
        reply->element[1]->element[0]->type != REDIS_REPLY_STRING)
    {
        panicAbort("%s", "Can't parse PUSH message!");
    }

    /* Increment our invalidation count */
    *invalidations += 1;

    printf("pushReplyHandler(): INVALIDATE '%s' (invalidation count: %d)\n",
           reply->element[1]->element[0]->str, *invalidations);

    freeReplyObject(reply);
}

/* We aren't actually freeing anything here, but it is included to show that we can
 * have hiredis call our data destructor when freeing the context */
void privdata_dtor(void *privdata) {
    unsigned int *icount = privdata;
    printf("privdata_dtor():  In context privdata dtor (invalidations: %u)\n", *icount);
}

int main(int argc, char **argv) {
    unsigned int j, invalidations = 0;
    redisContext *c;
    redisReply *reply;

    const char *hostname = (argc > 1) ? argv[1] : "127.0.0.1";
    int port = (argc > 2) ? atoi(argv[2]) : 6379;

    redisOptions o = {0};
    REDIS_OPTIONS_SET_TCP(&o, hostname, port);

    /* Set our context privdata to the address of our invalidation counter.  Each
     * time our PUSH handler is called, hiredis will pass the privdata for context.
     *
     * This could also be done after we create the context like so:
     *
     *    c->privdata = &invalidations;
     *    c->free_privdata = privdata_dtor;
     */
    REDIS_OPTIONS_SET_PRIVDATA(&o, &invalidations, privdata_dtor);

    /* Set our custom PUSH message handler */
    o.push_cb = pushReplyHandler;

    c = redisConnectWithOptions(&o);
    if (c == NULL || c->err)
        panicAbort("Connection error:  %s", c ? c->errstr : "OOM");

    /* Enable RESP3 and turn on client tracking */
    enableClientTracking(c);

    /* Set some keys and then read them back.  Once we do that, Redis will deliver
     * invalidation push messages whenever the key is modified */
    for (j = 0; j < KEY_COUNT; j++) {
        reply = redisCommand(c, "SET key:%d initial:%d", j, j);
        assertReplyAndFree(c, reply, REDIS_REPLY_STATUS);

        reply = redisCommand(c, "GET key:%d", j);
        assertReplyAndFree(c, reply, REDIS_REPLY_STRING);
    }

    /* Trigger invalidation messages by updating keys we just read */
    for (j = 0; j < KEY_COUNT; j++) {
        printf("            main(): SET key:%d update:%d\n", j, j);
        reply = redisCommand(c, "SET key:%d update:%d", j, j);
        assertReplyAndFree(c, reply, REDIS_REPLY_STATUS);
        printf("            main(): SET REPLY OK\n");
    }

    printf("\nTotal detected invalidations: %d, expected: %d\n", invalidations, KEY_COUNT);

    /* PING server */
    redisFree(c);
}

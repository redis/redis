#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>

#include "hiredis.h"

/* The following lines make up our testing "framework" :) */
static int tests = 0, fails = 0;
#define test(_s) { printf("#%02d ", ++tests); printf(_s); }
#define test_cond(_c) if(_c) printf("PASSED\n"); else {printf("FAILED\n"); fails++;}

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

static int use_unix = 0;
static redisContext *blocking_context = NULL;
static void __connect(redisContext **target) {
    *target = blocking_context = (use_unix ?
        redisConnectUnix("/tmp/redis.sock") : redisConnect((char*)"127.0.0.1", 6379));
    if (blocking_context->err) {
        printf("Connection error: %s\n", blocking_context->errstr);
        exit(1);
    }
}

static void test_format_commands() {
    char *cmd;
    int len;

    test("Format command without interpolation: ");
    len = redisFormatCommand(&cmd,"SET foo bar");
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(3+2)+4+(3+2));
    free(cmd);

    test("Format command with %%s string interpolation: ");
    len = redisFormatCommand(&cmd,"SET %s %s","foo","bar");
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(3+2)+4+(3+2));
    free(cmd);

    test("Format command with %%s and an empty string: ");
    len = redisFormatCommand(&cmd,"SET %s %s","foo","");
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$0\r\n\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(3+2)+4+(0+2));
    free(cmd);

    test("Format command with %%b string interpolation: ");
    len = redisFormatCommand(&cmd,"SET %b %b","foo",3,"b\0r",3);
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nb\0r\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(3+2)+4+(3+2));
    free(cmd);

    test("Format command with %%b and an empty string: ");
    len = redisFormatCommand(&cmd,"SET %b %b","foo",3,"",0);
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$0\r\n\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(3+2)+4+(0+2));
    free(cmd);

    test("Format command with literal %%: ");
    len = redisFormatCommand(&cmd,"SET %% %%");
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$1\r\n%\r\n$1\r\n%\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(1+2)+4+(1+2));
    free(cmd);

    test("Format command with printf-delegation (long long): ");
    len = redisFormatCommand(&cmd,"key:%08lld",1234ll);
    test_cond(strncmp(cmd,"*1\r\n$12\r\nkey:00001234\r\n",len) == 0 &&
        len == 4+5+(12+2));
    free(cmd);

    test("Format command with printf-delegation (float): ");
    len = redisFormatCommand(&cmd,"v:%06.1f",12.34f);
    test_cond(strncmp(cmd,"*1\r\n$8\r\nv:0012.3\r\n",len) == 0 &&
        len == 4+4+(8+2));
    free(cmd);

    test("Format command with printf-delegation and extra interpolation: ");
    len = redisFormatCommand(&cmd,"key:%d %b",1234,"foo",3);
    test_cond(strncmp(cmd,"*2\r\n$8\r\nkey:1234\r\n$3\r\nfoo\r\n",len) == 0 &&
        len == 4+4+(8+2)+4+(3+2));
    free(cmd);

    test("Format command with wrong printf format and extra interpolation: ");
    len = redisFormatCommand(&cmd,"key:%08p %b",1234,"foo",3);
    test_cond(strncmp(cmd,"*2\r\n$6\r\nkey:8p\r\n$3\r\nfoo\r\n",len) == 0 &&
        len == 4+4+(6+2)+4+(3+2));
    free(cmd);

    const char *argv[3];
    argv[0] = "SET";
    argv[1] = "foo\0xxx";
    argv[2] = "bar";
    size_t lens[3] = { 3, 7, 3 };
    int argc = 3;

    test("Format command by passing argc/argv without lengths: ");
    len = redisFormatCommandArgv(&cmd,argc,argv,NULL);
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(3+2)+4+(3+2));
    free(cmd);

    test("Format command by passing argc/argv with lengths: ");
    len = redisFormatCommandArgv(&cmd,argc,argv,lens);
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$7\r\nfoo\0xxx\r\n$3\r\nbar\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(7+2)+4+(3+2));
    free(cmd);
}

static void test_blocking_connection() {
    redisContext *c;
    redisReply *reply;
    int major, minor;

    test("Returns error when host cannot be resolved: ");
    c = redisConnect((char*)"idontexist.local", 6379);
    test_cond(c->err == REDIS_ERR_OTHER &&
        strcmp(c->errstr,"Can't resolve: idontexist.local") == 0);
    redisFree(c);

    test("Returns error when the port is not open: ");
    c = redisConnect((char*)"localhost", 56380);
    test_cond(c->err == REDIS_ERR_IO &&
        strcmp(c->errstr,"Connection refused") == 0);
    redisFree(c);

    __connect(&c);
    test("Is able to deliver commands: ");
    reply = redisCommand(c,"PING");
    test_cond(reply->type == REDIS_REPLY_STATUS &&
        strcasecmp(reply->str,"pong") == 0)
    freeReplyObject(reply);

    /* Switch to DB 9 for testing, now that we know we can chat. */
    reply = redisCommand(c,"SELECT 9");
    freeReplyObject(reply);

    /* Make sure the DB is emtpy */
    reply = redisCommand(c,"DBSIZE");
    if (reply->type != REDIS_REPLY_INTEGER || reply->integer != 0) {
        printf("Database #9 is not empty, test can not continue\n");
        exit(1);
    }
    freeReplyObject(reply);

    test("Is a able to send commands verbatim: ");
    reply = redisCommand(c,"SET foo bar");
    test_cond (reply->type == REDIS_REPLY_STATUS &&
        strcasecmp(reply->str,"ok") == 0)
    freeReplyObject(reply);

    test("%%s String interpolation works: ");
    reply = redisCommand(c,"SET %s %s","foo","hello world");
    freeReplyObject(reply);
    reply = redisCommand(c,"GET foo");
    test_cond(reply->type == REDIS_REPLY_STRING &&
        strcmp(reply->str,"hello world") == 0);
    freeReplyObject(reply);

    test("%%b String interpolation works: ");
    reply = redisCommand(c,"SET %b %b","foo",3,"hello\x00world",11);
    freeReplyObject(reply);
    reply = redisCommand(c,"GET foo");
    test_cond(reply->type == REDIS_REPLY_STRING &&
        memcmp(reply->str,"hello\x00world",11) == 0)

    test("Binary reply length is correct: ");
    test_cond(reply->len == 11)
    freeReplyObject(reply);

    test("Can parse nil replies: ");
    reply = redisCommand(c,"GET nokey");
    test_cond(reply->type == REDIS_REPLY_NIL)
    freeReplyObject(reply);

    /* test 7 */
    test("Can parse integer replies: ");
    reply = redisCommand(c,"INCR mycounter");
    test_cond(reply->type == REDIS_REPLY_INTEGER && reply->integer == 1)
    freeReplyObject(reply);

    test("Can parse multi bulk replies: ");
    freeReplyObject(redisCommand(c,"LPUSH mylist foo"));
    freeReplyObject(redisCommand(c,"LPUSH mylist bar"));
    reply = redisCommand(c,"LRANGE mylist 0 -1");
    test_cond(reply->type == REDIS_REPLY_ARRAY &&
              reply->elements == 2 &&
              !memcmp(reply->element[0]->str,"bar",3) &&
              !memcmp(reply->element[1]->str,"foo",3))
    freeReplyObject(reply);

    /* m/e with multi bulk reply *before* other reply.
     * specifically test ordering of reply items to parse. */
    test("Can handle nested multi bulk replies: ");
    freeReplyObject(redisCommand(c,"MULTI"));
    freeReplyObject(redisCommand(c,"LRANGE mylist 0 -1"));
    freeReplyObject(redisCommand(c,"PING"));
    reply = (redisCommand(c,"EXEC"));
    test_cond(reply->type == REDIS_REPLY_ARRAY &&
              reply->elements == 2 &&
              reply->element[0]->type == REDIS_REPLY_ARRAY &&
              reply->element[0]->elements == 2 &&
              !memcmp(reply->element[0]->element[0]->str,"bar",3) &&
              !memcmp(reply->element[0]->element[1]->str,"foo",3) &&
              reply->element[1]->type == REDIS_REPLY_STATUS &&
              strcasecmp(reply->element[1]->str,"pong") == 0);
    freeReplyObject(reply);

    {
        /* Find out Redis version to determine the path for the next test */
        const char *field = "redis_version:";
        char *p, *eptr;

        reply = redisCommand(c,"INFO");
        p = strstr(reply->str,field);
        major = strtol(p+strlen(field),&eptr,10);
        p = eptr+1; /* char next to the first "." */
        minor = strtol(p,&eptr,10);
        freeReplyObject(reply);
    }

    test("Returns I/O error when the connection is lost: ");
    reply = redisCommand(c,"QUIT");
    if (major >= 2 && minor > 0) {
        /* > 2.0 returns OK on QUIT and read() should be issued once more
         * to know the descriptor is at EOF. */
        test_cond(strcasecmp(reply->str,"OK") == 0 &&
            redisGetReply(c,(void**)&reply) == REDIS_ERR);
        freeReplyObject(reply);
    } else {
        test_cond(reply == NULL);
    }

    /* On 2.0, QUIT will cause the connection to be closed immediately and
     * the read(2) for the reply on QUIT will set the error to EOF.
     * On >2.0, QUIT will return with OK and another read(2) needed to be
     * issued to find out the socket was closed by the server. In both
     * conditions, the error will be set to EOF. */
    assert(c->err == REDIS_ERR_EOF &&
        strcmp(c->errstr,"Server closed the connection") == 0);

    /* Clean up context and reconnect again */
    redisFree(c);
    __connect(&c);
}

static void test_reply_reader() {
    void *reader;
    void *reply;
    char *err;
    int ret;

    test("Error handling in reply parser: ");
    reader = redisReplyReaderCreate();
    redisReplyReaderFeed(reader,(char*)"@foo\r\n",6);
    ret = redisReplyReaderGetReply(reader,NULL);
    err = redisReplyReaderGetError(reader);
    test_cond(ret == REDIS_ERR &&
              strcasecmp(err,"Protocol error, got \"@\" as reply type byte") == 0);
    redisReplyReaderFree(reader);

    /* when the reply already contains multiple items, they must be free'd
     * on an error. valgrind will bark when this doesn't happen. */
    test("Memory cleanup in reply parser: ");
    reader = redisReplyReaderCreate();
    redisReplyReaderFeed(reader,(char*)"*2\r\n",4);
    redisReplyReaderFeed(reader,(char*)"$5\r\nhello\r\n",11);
    redisReplyReaderFeed(reader,(char*)"@foo\r\n",6);
    ret = redisReplyReaderGetReply(reader,NULL);
    err = redisReplyReaderGetError(reader);
    test_cond(ret == REDIS_ERR &&
              strcasecmp(err,"Protocol error, got \"@\" as reply type byte") == 0);
    redisReplyReaderFree(reader);

    test("Set error on nested multi bulks with depth > 1: ");
    reader = redisReplyReaderCreate();
    redisReplyReaderFeed(reader,(char*)"*1\r\n",4);
    redisReplyReaderFeed(reader,(char*)"*1\r\n",4);
    redisReplyReaderFeed(reader,(char*)"*1\r\n",4);
    ret = redisReplyReaderGetReply(reader,NULL);
    err = redisReplyReaderGetError(reader);
    test_cond(ret == REDIS_ERR &&
              strncasecmp(err,"No support for",14) == 0);
    redisReplyReaderFree(reader);

    test("Works with NULL functions for reply: ");
    reader = redisReplyReaderCreate();
    redisReplyReaderSetReplyObjectFunctions(reader,NULL);
    redisReplyReaderFeed(reader,(char*)"+OK\r\n",5);
    ret = redisReplyReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_OK && reply == (void*)REDIS_REPLY_STATUS);
    redisReplyReaderFree(reader);

    test("Works when a single newline (\\r\\n) covers two calls to feed: ");
    reader = redisReplyReaderCreate();
    redisReplyReaderSetReplyObjectFunctions(reader,NULL);
    redisReplyReaderFeed(reader,(char*)"+OK\r",4);
    ret = redisReplyReaderGetReply(reader,&reply);
    assert(ret == REDIS_OK && reply == NULL);
    redisReplyReaderFeed(reader,(char*)"\n",1);
    ret = redisReplyReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_OK && reply == (void*)REDIS_REPLY_STATUS);
    redisReplyReaderFree(reader);
}

static void test_throughput() {
    int i;
    long long t1, t2;
    redisContext *c = blocking_context;
    redisReply **replies;

    test("Throughput:\n");
    for (i = 0; i < 500; i++)
        freeReplyObject(redisCommand(c,"LPUSH mylist foo"));

    replies = malloc(sizeof(redisReply*)*1000);
    t1 = usec();
    for (i = 0; i < 1000; i++) {
        replies[i] = redisCommand(c,"PING");
        assert(replies[i] != NULL && replies[i]->type == REDIS_REPLY_STATUS);
    }
    t2 = usec();
    for (i = 0; i < 1000; i++) freeReplyObject(replies[i]);
    free(replies);
    printf("\t(1000x PING: %.2fs)\n", (t2-t1)/1000000.0);

    replies = malloc(sizeof(redisReply*)*1000);
    t1 = usec();
    for (i = 0; i < 1000; i++) {
        replies[i] = redisCommand(c,"LRANGE mylist 0 499");
        assert(replies[i] != NULL && replies[i]->type == REDIS_REPLY_ARRAY);
        assert(replies[i] != NULL && replies[i]->elements == 500);
    }
    t2 = usec();
    for (i = 0; i < 1000; i++) freeReplyObject(replies[i]);
    free(replies);
    printf("\t(1000x LRANGE with 500 elements: %.2fs)\n", (t2-t1)/1000000.0);
}

static void cleanup() {
    redisContext *c = blocking_context;
    redisReply *reply;

    /* Make sure we're on DB 9 */
    reply = redisCommand(c,"SELECT 9");
    assert(reply != NULL); freeReplyObject(reply);
    reply = redisCommand(c,"FLUSHDB");
    assert(reply != NULL); freeReplyObject(reply);
    redisFree(c);
}

// static long __test_callback_flags = 0;
// static void __test_callback(redisContext *c, void *privdata) {
//     ((void)c);
//     /* Shift to detect execution order */
//     __test_callback_flags <<= 8;
//     __test_callback_flags |= (long)privdata;
// }
//
// static void __test_reply_callback(redisContext *c, redisReply *reply, void *privdata) {
//     ((void)c);
//     /* Shift to detect execution order */
//     __test_callback_flags <<= 8;
//     __test_callback_flags |= (long)privdata;
//     if (reply) freeReplyObject(reply);
// }
//
// static redisContext *__connect_nonblock() {
//     /* Reset callback flags */
//     __test_callback_flags = 0;
//     return redisConnectNonBlock("127.0.0.1", 6379, NULL);
// }
//
// static void test_nonblocking_connection() {
//     redisContext *c;
//     int wdone = 0;
//
//     test("Calls command callback when command is issued: ");
//     c = __connect_nonblock();
//     redisSetCommandCallback(c,__test_callback,(void*)1);
//     redisCommand(c,"PING");
//     test_cond(__test_callback_flags == 1);
//     redisFree(c);
//
//     test("Calls disconnect callback on redisDisconnect: ");
//     c = __connect_nonblock();
//     redisSetDisconnectCallback(c,__test_callback,(void*)2);
//     redisDisconnect(c);
//     test_cond(__test_callback_flags == 2);
//     redisFree(c);
//
//     test("Calls disconnect callback and free callback on redisFree: ");
//     c = __connect_nonblock();
//     redisSetDisconnectCallback(c,__test_callback,(void*)2);
//     redisSetFreeCallback(c,__test_callback,(void*)4);
//     redisFree(c);
//     test_cond(__test_callback_flags == ((2 << 8) | 4));
//
//     test("redisBufferWrite against empty write buffer: ");
//     c = __connect_nonblock();
//     test_cond(redisBufferWrite(c,&wdone) == REDIS_OK && wdone == 1);
//     redisFree(c);
//
//     test("redisBufferWrite against not yet connected fd: ");
//     c = __connect_nonblock();
//     redisCommand(c,"PING");
//     test_cond(redisBufferWrite(c,NULL) == REDIS_ERR &&
//               strncmp(c->error,"write:",6) == 0);
//     redisFree(c);
//
//     test("redisBufferWrite against closed fd: ");
//     c = __connect_nonblock();
//     redisCommand(c,"PING");
//     redisDisconnect(c);
//     test_cond(redisBufferWrite(c,NULL) == REDIS_ERR &&
//               strncmp(c->error,"write:",6) == 0);
//     redisFree(c);
//
//     test("Process callbacks in the right sequence: ");
//     c = __connect_nonblock();
//     redisCommandWithCallback(c,__test_reply_callback,(void*)1,"PING");
//     redisCommandWithCallback(c,__test_reply_callback,(void*)2,"PING");
//     redisCommandWithCallback(c,__test_reply_callback,(void*)3,"PING");
//
//     /* Write output buffer */
//     wdone = 0;
//     while(!wdone) {
//         usleep(500);
//         redisBufferWrite(c,&wdone);
//     }
//
//     /* Read until at least one callback is executed (the 3 replies will
//      * arrive in a single packet, causing all callbacks to be executed in
//      * a single pass). */
//     while(__test_callback_flags == 0) {
//         assert(redisBufferRead(c) == REDIS_OK);
//         redisProcessCallbacks(c);
//     }
//     test_cond(__test_callback_flags == 0x010203);
//     redisFree(c);
//
//     test("redisDisconnect executes pending callbacks with NULL reply: ");
//     c = __connect_nonblock();
//     redisSetDisconnectCallback(c,__test_callback,(void*)1);
//     redisCommandWithCallback(c,__test_reply_callback,(void*)2,"PING");
//     redisDisconnect(c);
//     test_cond(__test_callback_flags == 0x0201);
//     redisFree(c);
// }

int main(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1],"-s") == 0)
            use_unix = 1;
    }

    signal(SIGPIPE, SIG_IGN);
    test_format_commands();
    test_blocking_connection();
    test_reply_reader();
    // test_nonblocking_connection();
    test_throughput();
    cleanup();

    if (fails == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("*** %d TESTS FAILED ***\n", fails);
    }
    return 0;
}

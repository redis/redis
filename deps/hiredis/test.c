#include "fmacros.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>

#include "hiredis.h"
#ifdef HIREDIS_TEST_SSL
#include "hiredis_ssl.h"
#endif
#include "net.h"

enum connection_type {
    CONN_TCP,
    CONN_UNIX,
    CONN_FD,
    CONN_SSL
};

struct config {
    enum connection_type type;

    struct {
        const char *host;
        int port;
        struct timeval timeout;
    } tcp;

    struct {
        const char *path;
    } unix_sock;

    struct {
        const char *host;
        int port;
        const char *ca_cert;
        const char *cert;
        const char *key;
    } ssl;
};

/* The following lines make up our testing "framework" :) */
static int tests = 0, fails = 0;
#define test(_s) { printf("#%02d ", ++tests); printf(_s); }
#define test_cond(_c) if(_c) printf("\033[0;32mPASSED\033[0;0m\n"); else {printf("\033[0;31mFAILED\033[0;0m\n"); fails++;}

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

/* The assert() calls below have side effects, so we need assert()
 * even if we are compiling without asserts (-DNDEBUG). */
#ifdef NDEBUG
#undef assert
#define assert(e) (void)(e)
#endif

static redisContext *select_database(redisContext *c) {
    redisReply *reply;

    /* Switch to DB 9 for testing, now that we know we can chat. */
    reply = redisCommand(c,"SELECT 9");
    assert(reply != NULL);
    freeReplyObject(reply);

    /* Make sure the DB is emtpy */
    reply = redisCommand(c,"DBSIZE");
    assert(reply != NULL);
    if (reply->type == REDIS_REPLY_INTEGER && reply->integer == 0) {
        /* Awesome, DB 9 is empty and we can continue. */
        freeReplyObject(reply);
    } else {
        printf("Database #9 is not empty, test can not continue\n");
        exit(1);
    }

    return c;
}

static int disconnect(redisContext *c, int keep_fd) {
    redisReply *reply;

    /* Make sure we're on DB 9. */
    reply = redisCommand(c,"SELECT 9");
    assert(reply != NULL);
    freeReplyObject(reply);
    reply = redisCommand(c,"FLUSHDB");
    assert(reply != NULL);
    freeReplyObject(reply);

    /* Free the context as well, but keep the fd if requested. */
    if (keep_fd)
        return redisFreeKeepFd(c);
    redisFree(c);
    return -1;
}

static void do_ssl_handshake(redisContext *c, struct config config) {
#ifdef HIREDIS_TEST_SSL
    redisSecureConnection(c, config.ssl.ca_cert, config.ssl.cert, config.ssl.key, NULL);
    if (c->err) {
        printf("SSL error: %s\n", c->errstr);
        redisFree(c);
        exit(1);
    }
#else
    (void) c;
    (void) config;
#endif
}

static redisContext *do_connect(struct config config) {
    redisContext *c = NULL;

    if (config.type == CONN_TCP) {
        c = redisConnect(config.tcp.host, config.tcp.port);
    } else if (config.type == CONN_SSL) {
        c = redisConnect(config.ssl.host, config.ssl.port);
    } else if (config.type == CONN_UNIX) {
        c = redisConnectUnix(config.unix_sock.path);
    } else if (config.type == CONN_FD) {
        /* Create a dummy connection just to get an fd to inherit */
        redisContext *dummy_ctx = redisConnectUnix(config.unix_sock.path);
        if (dummy_ctx) {
            int fd = disconnect(dummy_ctx, 1);
            printf("Connecting to inherited fd %d\n", fd);
            c = redisConnectFd(fd);
        }
    } else {
        assert(NULL);
    }

    if (c == NULL) {
        printf("Connection error: can't allocate redis context\n");
        exit(1);
    } else if (c->err) {
        printf("Connection error: %s\n", c->errstr);
        redisFree(c);
        exit(1);
    }

    if (config.type == CONN_SSL) {
        do_ssl_handshake(c, config);
    }

    return select_database(c);
}

static void do_reconnect(redisContext *c, struct config config) {
    redisReconnect(c);

    if (config.type == CONN_SSL) {
        do_ssl_handshake(c, config);
    }
}

static void test_format_commands(void) {
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

    test("Format command with an empty string in between proper interpolations: ");
    len = redisFormatCommand(&cmd,"SET %s %s","","foo");
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$0\r\n\r\n$3\r\nfoo\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(0+2)+4+(3+2));
    free(cmd);

    test("Format command with %%b string interpolation: ");
    len = redisFormatCommand(&cmd,"SET %b %b","foo",(size_t)3,"b\0r",(size_t)3);
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nb\0r\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(3+2)+4+(3+2));
    free(cmd);

    test("Format command with %%b and an empty string: ");
    len = redisFormatCommand(&cmd,"SET %b %b","foo",(size_t)3,"",(size_t)0);
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$0\r\n\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(3+2)+4+(0+2));
    free(cmd);

    test("Format command with literal %%: ");
    len = redisFormatCommand(&cmd,"SET %% %%");
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$1\r\n%\r\n$1\r\n%\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(1+2)+4+(1+2));
    free(cmd);

    /* Vararg width depends on the type. These tests make sure that the
     * width is correctly determined using the format and subsequent varargs
     * can correctly be interpolated. */
#define INTEGER_WIDTH_TEST(fmt, type) do {                                                \
    type value = 123;                                                                     \
    test("Format command with printf-delegation (" #type "): ");                          \
    len = redisFormatCommand(&cmd,"key:%08" fmt " str:%s", value, "hello");               \
    test_cond(strncmp(cmd,"*2\r\n$12\r\nkey:00000123\r\n$9\r\nstr:hello\r\n",len) == 0 && \
        len == 4+5+(12+2)+4+(9+2));                                                       \
    free(cmd);                                                                            \
} while(0)

#define FLOAT_WIDTH_TEST(type) do {                                                       \
    type value = 123.0;                                                                   \
    test("Format command with printf-delegation (" #type "): ");                          \
    len = redisFormatCommand(&cmd,"key:%08.3f str:%s", value, "hello");                   \
    test_cond(strncmp(cmd,"*2\r\n$12\r\nkey:0123.000\r\n$9\r\nstr:hello\r\n",len) == 0 && \
        len == 4+5+(12+2)+4+(9+2));                                                       \
    free(cmd);                                                                            \
} while(0)

    INTEGER_WIDTH_TEST("d", int);
    INTEGER_WIDTH_TEST("hhd", char);
    INTEGER_WIDTH_TEST("hd", short);
    INTEGER_WIDTH_TEST("ld", long);
    INTEGER_WIDTH_TEST("lld", long long);
    INTEGER_WIDTH_TEST("u", unsigned int);
    INTEGER_WIDTH_TEST("hhu", unsigned char);
    INTEGER_WIDTH_TEST("hu", unsigned short);
    INTEGER_WIDTH_TEST("lu", unsigned long);
    INTEGER_WIDTH_TEST("llu", unsigned long long);
    FLOAT_WIDTH_TEST(float);
    FLOAT_WIDTH_TEST(double);

    test("Format command with invalid printf format: ");
    len = redisFormatCommand(&cmd,"key:%08p %b",(void*)1234,"foo",(size_t)3);
    test_cond(len == -1);

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

    sds sds_cmd;

    sds_cmd = sdsempty();
    test("Format command into sds by passing argc/argv without lengths: ");
    len = redisFormatSdsCommandArgv(&sds_cmd,argc,argv,NULL);
    test_cond(strncmp(sds_cmd,"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(3+2)+4+(3+2));
    sdsfree(sds_cmd);

    sds_cmd = sdsempty();
    test("Format command into sds by passing argc/argv with lengths: ");
    len = redisFormatSdsCommandArgv(&sds_cmd,argc,argv,lens);
    test_cond(strncmp(sds_cmd,"*3\r\n$3\r\nSET\r\n$7\r\nfoo\0xxx\r\n$3\r\nbar\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(7+2)+4+(3+2));
    sdsfree(sds_cmd);
}

static void test_append_formatted_commands(struct config config) {
    redisContext *c;
    redisReply *reply;
    char *cmd;
    int len;

    c = do_connect(config);

    test("Append format command: ");

    len = redisFormatCommand(&cmd, "SET foo bar");

    test_cond(redisAppendFormattedCommand(c, cmd, len) == REDIS_OK);

    assert(redisGetReply(c, (void*)&reply) == REDIS_OK);

    free(cmd);
    freeReplyObject(reply);

    disconnect(c, 0);
}

static void test_reply_reader(void) {
    redisReader *reader;
    void *reply;
    int ret;
    int i;

    test("Error handling in reply parser: ");
    reader = redisReaderCreate();
    redisReaderFeed(reader,(char*)"@foo\r\n",6);
    ret = redisReaderGetReply(reader,NULL);
    test_cond(ret == REDIS_ERR &&
              strcasecmp(reader->errstr,"Protocol error, got \"@\" as reply type byte") == 0);
    redisReaderFree(reader);

    /* when the reply already contains multiple items, they must be free'd
     * on an error. valgrind will bark when this doesn't happen. */
    test("Memory cleanup in reply parser: ");
    reader = redisReaderCreate();
    redisReaderFeed(reader,(char*)"*2\r\n",4);
    redisReaderFeed(reader,(char*)"$5\r\nhello\r\n",11);
    redisReaderFeed(reader,(char*)"@foo\r\n",6);
    ret = redisReaderGetReply(reader,NULL);
    test_cond(ret == REDIS_ERR &&
              strcasecmp(reader->errstr,"Protocol error, got \"@\" as reply type byte") == 0);
    redisReaderFree(reader);

    test("Set error on nested multi bulks with depth > 7: ");
    reader = redisReaderCreate();

    for (i = 0; i < 9; i++) {
        redisReaderFeed(reader,(char*)"*1\r\n",4);
    }

    ret = redisReaderGetReply(reader,NULL);
    test_cond(ret == REDIS_ERR &&
              strncasecmp(reader->errstr,"No support for",14) == 0);
    redisReaderFree(reader);

    test("Correctly parses LLONG_MAX: ");
    reader = redisReaderCreate();
    redisReaderFeed(reader, ":9223372036854775807\r\n",22);
    ret = redisReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_OK &&
            ((redisReply*)reply)->type == REDIS_REPLY_INTEGER &&
            ((redisReply*)reply)->integer == LLONG_MAX);
    freeReplyObject(reply);
    redisReaderFree(reader);

    test("Set error when > LLONG_MAX: ");
    reader = redisReaderCreate();
    redisReaderFeed(reader, ":9223372036854775808\r\n",22);
    ret = redisReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_ERR &&
              strcasecmp(reader->errstr,"Bad integer value") == 0);
    freeReplyObject(reply);
    redisReaderFree(reader);

    test("Correctly parses LLONG_MIN: ");
    reader = redisReaderCreate();
    redisReaderFeed(reader, ":-9223372036854775808\r\n",23);
    ret = redisReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_OK &&
            ((redisReply*)reply)->type == REDIS_REPLY_INTEGER &&
            ((redisReply*)reply)->integer == LLONG_MIN);
    freeReplyObject(reply);
    redisReaderFree(reader);

    test("Set error when < LLONG_MIN: ");
    reader = redisReaderCreate();
    redisReaderFeed(reader, ":-9223372036854775809\r\n",23);
    ret = redisReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_ERR &&
              strcasecmp(reader->errstr,"Bad integer value") == 0);
    freeReplyObject(reply);
    redisReaderFree(reader);

    test("Set error when array < -1: ");
    reader = redisReaderCreate();
    redisReaderFeed(reader, "*-2\r\n+asdf\r\n",12);
    ret = redisReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_ERR &&
              strcasecmp(reader->errstr,"Multi-bulk length out of range") == 0);
    freeReplyObject(reply);
    redisReaderFree(reader);

    test("Set error when bulk < -1: ");
    reader = redisReaderCreate();
    redisReaderFeed(reader, "$-2\r\nasdf\r\n",11);
    ret = redisReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_ERR &&
              strcasecmp(reader->errstr,"Bulk string length out of range") == 0);
    freeReplyObject(reply);
    redisReaderFree(reader);

#if LLONG_MAX > SIZE_MAX
    test("Set error when array > SIZE_MAX: ");
    reader = redisReaderCreate();
    redisReaderFeed(reader, "*9223372036854775807\r\n+asdf\r\n",29);
    ret = redisReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_ERR &&
            strcasecmp(reader->errstr,"Multi-bulk length out of range") == 0);
    freeReplyObject(reply);
    redisReaderFree(reader);

    test("Set error when bulk > SIZE_MAX: ");
    reader = redisReaderCreate();
    redisReaderFeed(reader, "$9223372036854775807\r\nasdf\r\n",28);
    ret = redisReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_ERR &&
            strcasecmp(reader->errstr,"Bulk string length out of range") == 0);
    freeReplyObject(reply);
    redisReaderFree(reader);
#endif

    test("Works with NULL functions for reply: ");
    reader = redisReaderCreate();
    reader->fn = NULL;
    redisReaderFeed(reader,(char*)"+OK\r\n",5);
    ret = redisReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_OK && reply == (void*)REDIS_REPLY_STATUS);
    redisReaderFree(reader);

    test("Works when a single newline (\\r\\n) covers two calls to feed: ");
    reader = redisReaderCreate();
    reader->fn = NULL;
    redisReaderFeed(reader,(char*)"+OK\r",4);
    ret = redisReaderGetReply(reader,&reply);
    assert(ret == REDIS_OK && reply == NULL);
    redisReaderFeed(reader,(char*)"\n",1);
    ret = redisReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_OK && reply == (void*)REDIS_REPLY_STATUS);
    redisReaderFree(reader);

    test("Don't reset state after protocol error: ");
    reader = redisReaderCreate();
    reader->fn = NULL;
    redisReaderFeed(reader,(char*)"x",1);
    ret = redisReaderGetReply(reader,&reply);
    assert(ret == REDIS_ERR);
    ret = redisReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_ERR && reply == NULL);
    redisReaderFree(reader);

    /* Regression test for issue #45 on GitHub. */
    test("Don't do empty allocation for empty multi bulk: ");
    reader = redisReaderCreate();
    redisReaderFeed(reader,(char*)"*0\r\n",4);
    ret = redisReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_OK &&
        ((redisReply*)reply)->type == REDIS_REPLY_ARRAY &&
        ((redisReply*)reply)->elements == 0);
    freeReplyObject(reply);
    redisReaderFree(reader);
}

static void test_free_null(void) {
    void *redisCtx = NULL;
    void *reply = NULL;

    test("Don't fail when redisFree is passed a NULL value: ");
    redisFree(redisCtx);
    test_cond(redisCtx == NULL);

    test("Don't fail when freeReplyObject is passed a NULL value: ");
    freeReplyObject(reply);
    test_cond(reply == NULL);
}

#define HIREDIS_BAD_DOMAIN "idontexist-noreally.com"
static void test_blocking_connection_errors(void) {
    redisContext *c;
    struct addrinfo hints = {.ai_family = AF_INET};
    struct addrinfo *ai_tmp = NULL;

    int rv = getaddrinfo(HIREDIS_BAD_DOMAIN, "6379", &hints, &ai_tmp);
    if (rv != 0) {
        // Address does *not* exist
        test("Returns error when host cannot be resolved: ");
        // First see if this domain name *actually* resolves to NXDOMAIN
        c = redisConnect(HIREDIS_BAD_DOMAIN, 6379);
        test_cond(
            c->err == REDIS_ERR_OTHER &&
            (strcmp(c->errstr, "Name or service not known") == 0 ||
             strcmp(c->errstr, "Can't resolve: " HIREDIS_BAD_DOMAIN) == 0 ||
             strcmp(c->errstr, "Name does not resolve") == 0 ||
             strcmp(c->errstr,
                    "nodename nor servname provided, or not known") == 0 ||
             strcmp(c->errstr, "No address associated with hostname") == 0 ||
             strcmp(c->errstr, "Temporary failure in name resolution") == 0 ||
             strcmp(c->errstr,
                    "hostname nor servname provided, or not known") == 0 ||
             strcmp(c->errstr, "no address associated with name") == 0));
        redisFree(c);
    } else {
        printf("Skipping NXDOMAIN test. Found evil ISP!\n");
        freeaddrinfo(ai_tmp);
    }

    test("Returns error when the port is not open: ");
    c = redisConnect((char*)"localhost", 1);
    test_cond(c->err == REDIS_ERR_IO &&
        strcmp(c->errstr,"Connection refused") == 0);
    redisFree(c);

    test("Returns error when the unix_sock socket path doesn't accept connections: ");
    c = redisConnectUnix((char*)"/tmp/idontexist.sock");
    test_cond(c->err == REDIS_ERR_IO); /* Don't care about the message... */
    redisFree(c);
}

static void test_blocking_connection(struct config config) {
    redisContext *c;
    redisReply *reply;

    c = do_connect(config);

    test("Is able to deliver commands: ");
    reply = redisCommand(c,"PING");
    test_cond(reply->type == REDIS_REPLY_STATUS &&
        strcasecmp(reply->str,"pong") == 0)
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
    reply = redisCommand(c,"SET %b %b","foo",(size_t)3,"hello\x00world",(size_t)11);
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

    disconnect(c, 0);
}

static void test_blocking_connection_timeouts(struct config config) {
    redisContext *c;
    redisReply *reply;
    ssize_t s;
    const char *cmd = "DEBUG SLEEP 3\r\n";
    struct timeval tv;

    c = do_connect(config);
    test("Successfully completes a command when the timeout is not exceeded: ");
    reply = redisCommand(c,"SET foo fast");
    freeReplyObject(reply);
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    redisSetTimeout(c, tv);
    reply = redisCommand(c, "GET foo");
    test_cond(reply != NULL && reply->type == REDIS_REPLY_STRING && memcmp(reply->str, "fast", 4) == 0);
    freeReplyObject(reply);
    disconnect(c, 0);

    c = do_connect(config);
    test("Does not return a reply when the command times out: ");
    redisAppendFormattedCommand(c, cmd, strlen(cmd));
    s = c->funcs->write(c);
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    redisSetTimeout(c, tv);
    reply = redisCommand(c, "GET foo");
    test_cond(s > 0 && reply == NULL && c->err == REDIS_ERR_IO && strcmp(c->errstr, "Resource temporarily unavailable") == 0);
    freeReplyObject(reply);

    test("Reconnect properly reconnects after a timeout: ");
    do_reconnect(c, config);
    reply = redisCommand(c, "PING");
    test_cond(reply != NULL && reply->type == REDIS_REPLY_STATUS && strcmp(reply->str, "PONG") == 0);
    freeReplyObject(reply);

    test("Reconnect properly uses owned parameters: ");
    config.tcp.host = "foo";
    config.unix_sock.path = "foo";
    do_reconnect(c, config);
    reply = redisCommand(c, "PING");
    test_cond(reply != NULL && reply->type == REDIS_REPLY_STATUS && strcmp(reply->str, "PONG") == 0);
    freeReplyObject(reply);

    disconnect(c, 0);
}

static void test_blocking_io_errors(struct config config) {
    redisContext *c;
    redisReply *reply;
    void *_reply;
    int major, minor;

    /* Connect to target given by config. */
    c = do_connect(config);
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
    if (major > 2 || (major == 2 && minor > 0)) {
        /* > 2.0 returns OK on QUIT and read() should be issued once more
         * to know the descriptor is at EOF. */
        test_cond(strcasecmp(reply->str,"OK") == 0 &&
            redisGetReply(c,&_reply) == REDIS_ERR);
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
    redisFree(c);

    c = do_connect(config);
    test("Returns I/O error on socket timeout: ");
    struct timeval tv = { 0, 1000 };
    assert(redisSetTimeout(c,tv) == REDIS_OK);
    test_cond(redisGetReply(c,&_reply) == REDIS_ERR &&
        c->err == REDIS_ERR_IO && errno == EAGAIN);
    redisFree(c);
}

static void test_invalid_timeout_errors(struct config config) {
    redisContext *c;

    test("Set error when an invalid timeout usec value is given to redisConnectWithTimeout: ");

    config.tcp.timeout.tv_sec = 0;
    config.tcp.timeout.tv_usec = 10000001;

    c = redisConnectWithTimeout(config.tcp.host, config.tcp.port, config.tcp.timeout);

    test_cond(c->err == REDIS_ERR_IO && strcmp(c->errstr, "Invalid timeout specified") == 0);
    redisFree(c);

    test("Set error when an invalid timeout sec value is given to redisConnectWithTimeout: ");

    config.tcp.timeout.tv_sec = (((LONG_MAX) - 999) / 1000) + 1;
    config.tcp.timeout.tv_usec = 0;

    c = redisConnectWithTimeout(config.tcp.host, config.tcp.port, config.tcp.timeout);

    test_cond(c->err == REDIS_ERR_IO && strcmp(c->errstr, "Invalid timeout specified") == 0);
    redisFree(c);
}

static void test_throughput(struct config config) {
    redisContext *c = do_connect(config);
    redisReply **replies;
    int i, num;
    long long t1, t2;

    test("Throughput:\n");
    for (i = 0; i < 500; i++)
        freeReplyObject(redisCommand(c,"LPUSH mylist foo"));

    num = 1000;
    replies = malloc(sizeof(redisReply*)*num);
    t1 = usec();
    for (i = 0; i < num; i++) {
        replies[i] = redisCommand(c,"PING");
        assert(replies[i] != NULL && replies[i]->type == REDIS_REPLY_STATUS);
    }
    t2 = usec();
    for (i = 0; i < num; i++) freeReplyObject(replies[i]);
    free(replies);
    printf("\t(%dx PING: %.3fs)\n", num, (t2-t1)/1000000.0);

    replies = malloc(sizeof(redisReply*)*num);
    t1 = usec();
    for (i = 0; i < num; i++) {
        replies[i] = redisCommand(c,"LRANGE mylist 0 499");
        assert(replies[i] != NULL && replies[i]->type == REDIS_REPLY_ARRAY);
        assert(replies[i] != NULL && replies[i]->elements == 500);
    }
    t2 = usec();
    for (i = 0; i < num; i++) freeReplyObject(replies[i]);
    free(replies);
    printf("\t(%dx LRANGE with 500 elements: %.3fs)\n", num, (t2-t1)/1000000.0);

    replies = malloc(sizeof(redisReply*)*num);
    t1 = usec();
    for (i = 0; i < num; i++) {
        replies[i] = redisCommand(c, "INCRBY incrkey %d", 1000000);
        assert(replies[i] != NULL && replies[i]->type == REDIS_REPLY_INTEGER);
    }
    t2 = usec();
    for (i = 0; i < num; i++) freeReplyObject(replies[i]);
    free(replies);
    printf("\t(%dx INCRBY: %.3fs)\n", num, (t2-t1)/1000000.0);

    num = 10000;
    replies = malloc(sizeof(redisReply*)*num);
    for (i = 0; i < num; i++)
        redisAppendCommand(c,"PING");
    t1 = usec();
    for (i = 0; i < num; i++) {
        assert(redisGetReply(c, (void*)&replies[i]) == REDIS_OK);
        assert(replies[i] != NULL && replies[i]->type == REDIS_REPLY_STATUS);
    }
    t2 = usec();
    for (i = 0; i < num; i++) freeReplyObject(replies[i]);
    free(replies);
    printf("\t(%dx PING (pipelined): %.3fs)\n", num, (t2-t1)/1000000.0);

    replies = malloc(sizeof(redisReply*)*num);
    for (i = 0; i < num; i++)
        redisAppendCommand(c,"LRANGE mylist 0 499");
    t1 = usec();
    for (i = 0; i < num; i++) {
        assert(redisGetReply(c, (void*)&replies[i]) == REDIS_OK);
        assert(replies[i] != NULL && replies[i]->type == REDIS_REPLY_ARRAY);
        assert(replies[i] != NULL && replies[i]->elements == 500);
    }
    t2 = usec();
    for (i = 0; i < num; i++) freeReplyObject(replies[i]);
    free(replies);
    printf("\t(%dx LRANGE with 500 elements (pipelined): %.3fs)\n", num, (t2-t1)/1000000.0);

    replies = malloc(sizeof(redisReply*)*num);
    for (i = 0; i < num; i++)
        redisAppendCommand(c,"INCRBY incrkey %d", 1000000);
    t1 = usec();
    for (i = 0; i < num; i++) {
        assert(redisGetReply(c, (void*)&replies[i]) == REDIS_OK);
        assert(replies[i] != NULL && replies[i]->type == REDIS_REPLY_INTEGER);
    }
    t2 = usec();
    for (i = 0; i < num; i++) freeReplyObject(replies[i]);
    free(replies);
    printf("\t(%dx INCRBY (pipelined): %.3fs)\n", num, (t2-t1)/1000000.0);

    disconnect(c, 0);
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
//     return redisConnectNonBlock("127.0.0.1", port, NULL);
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
    struct config cfg = {
        .tcp = {
            .host = "127.0.0.1",
            .port = 6379
        },
        .unix_sock = {
            .path = "/tmp/redis.sock"
        }
    };
    int throughput = 1;
    int test_inherit_fd = 1;

    /* Ignore broken pipe signal (for I/O error tests). */
    signal(SIGPIPE, SIG_IGN);

    /* Parse command line options. */
    argv++; argc--;
    while (argc) {
        if (argc >= 2 && !strcmp(argv[0],"-h")) {
            argv++; argc--;
            cfg.tcp.host = argv[0];
        } else if (argc >= 2 && !strcmp(argv[0],"-p")) {
            argv++; argc--;
            cfg.tcp.port = atoi(argv[0]);
        } else if (argc >= 2 && !strcmp(argv[0],"-s")) {
            argv++; argc--;
            cfg.unix_sock.path = argv[0];
        } else if (argc >= 1 && !strcmp(argv[0],"--skip-throughput")) {
            throughput = 0;
        } else if (argc >= 1 && !strcmp(argv[0],"--skip-inherit-fd")) {
            test_inherit_fd = 0;
#ifdef HIREDIS_TEST_SSL
        } else if (argc >= 2 && !strcmp(argv[0],"--ssl-port")) {
            argv++; argc--;
            cfg.ssl.port = atoi(argv[0]);
        } else if (argc >= 2 && !strcmp(argv[0],"--ssl-host")) {
            argv++; argc--;
            cfg.ssl.host = argv[0];
        } else if (argc >= 2 && !strcmp(argv[0],"--ssl-ca-cert")) {
            argv++; argc--;
            cfg.ssl.ca_cert  = argv[0];
        } else if (argc >= 2 && !strcmp(argv[0],"--ssl-cert")) {
            argv++; argc--;
            cfg.ssl.cert = argv[0];
        } else if (argc >= 2 && !strcmp(argv[0],"--ssl-key")) {
            argv++; argc--;
            cfg.ssl.key = argv[0];
#endif
        } else {
            fprintf(stderr, "Invalid argument: %s\n", argv[0]);
            exit(1);
        }
        argv++; argc--;
    }

    test_format_commands();
    test_reply_reader();
    test_blocking_connection_errors();
    test_free_null();

    printf("\nTesting against TCP connection (%s:%d):\n", cfg.tcp.host, cfg.tcp.port);
    cfg.type = CONN_TCP;
    test_blocking_connection(cfg);
    test_blocking_connection_timeouts(cfg);
    test_blocking_io_errors(cfg);
    test_invalid_timeout_errors(cfg);
    test_append_formatted_commands(cfg);
    if (throughput) test_throughput(cfg);

    printf("\nTesting against Unix socket connection (%s):\n", cfg.unix_sock.path);
    cfg.type = CONN_UNIX;
    test_blocking_connection(cfg);
    test_blocking_connection_timeouts(cfg);
    test_blocking_io_errors(cfg);
    if (throughput) test_throughput(cfg);

#ifdef HIREDIS_TEST_SSL
    if (cfg.ssl.port && cfg.ssl.host) {
        printf("\nTesting against SSL connection (%s:%d):\n", cfg.ssl.host, cfg.ssl.port);
        cfg.type = CONN_SSL;

        test_blocking_connection(cfg);
        test_blocking_connection_timeouts(cfg);
        test_blocking_io_errors(cfg);
        test_invalid_timeout_errors(cfg);
        test_append_formatted_commands(cfg);
        if (throughput) test_throughput(cfg);
    }
#endif

    if (test_inherit_fd) {
        printf("\nTesting against inherited fd (%s):\n", cfg.unix_sock.path);
        cfg.type = CONN_FD;
        test_blocking_connection(cfg);
    }


    if (fails) {
        printf("*** %d TESTS FAILED ***\n", fails);
        return 1;
    }

    printf("ALL TESTS PASSED\n");
    return 0;
}

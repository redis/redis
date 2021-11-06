#include "fmacros.h"
#include "sockcompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#include <sys/time.h>
#endif
#include <assert.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>

#include "hiredis.h"
#include "async.h"
#ifdef HIREDIS_TEST_SSL
#include "hiredis_ssl.h"
#endif
#include "net.h"
#include "win32.h"

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

struct privdata {
    int dtor_counter;
};

struct pushCounters {
    int nil;
    int str;
};

#ifdef HIREDIS_TEST_SSL
redisSSLContext *_ssl_ctx = NULL;
#endif

/* The following lines make up our testing "framework" :) */
static int tests = 0, fails = 0, skips = 0;
#define test(_s) { printf("#%02d ", ++tests); printf(_s); }
#define test_cond(_c) if(_c) printf("\033[0;32mPASSED\033[0;0m\n"); else {printf("\033[0;31mFAILED\033[0;0m\n"); fails++;}
#define test_skipped() { printf("\033[01;33mSKIPPED\033[0;0m\n"); skips++; }

static long long usec(void) {
#ifndef _MSC_VER
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
#else
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10;
#endif
}

/* The assert() calls below have side effects, so we need assert()
 * even if we are compiling without asserts (-DNDEBUG). */
#ifdef NDEBUG
#undef assert
#define assert(e) (void)(e)
#endif

/* Helper to extract Redis version information.  Aborts on any failure. */
#define REDIS_VERSION_FIELD "redis_version:"
void get_redis_version(redisContext *c, int *majorptr, int *minorptr) {
    redisReply *reply;
    char *eptr, *s, *e;
    int major, minor;

    reply = redisCommand(c, "INFO");
    if (reply == NULL || c->err || reply->type != REDIS_REPLY_STRING)
        goto abort;
    if ((s = strstr(reply->str, REDIS_VERSION_FIELD)) == NULL)
        goto abort;

    s += strlen(REDIS_VERSION_FIELD);

    /* We need a field terminator and at least 'x.y.z' (5) bytes of data */
    if ((e = strstr(s, "\r\n")) == NULL || (e - s) < 5)
        goto abort;

    /* Extract version info */
    major = strtol(s, &eptr, 10);
    if (*eptr != '.') goto abort;
    minor = strtol(eptr+1, NULL, 10);

    /* Push info the caller wants */
    if (majorptr) *majorptr = major;
    if (minorptr) *minorptr = minor;

    freeReplyObject(reply);
    return;

abort:
    freeReplyObject(reply);
    fprintf(stderr, "Error:  Cannot determine Redis version, aborting\n");
    exit(1);
}

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

/* Switch protocol */
static void send_hello(redisContext *c, int version) {
    redisReply *reply;
    int expected;

    reply = redisCommand(c, "HELLO %d", version);
    expected = version == 3 ? REDIS_REPLY_MAP : REDIS_REPLY_ARRAY;
    assert(reply != NULL && reply->type == expected);
    freeReplyObject(reply);
}

/* Togggle client tracking */
static void send_client_tracking(redisContext *c, const char *str) {
    redisReply *reply;

    reply = redisCommand(c, "CLIENT TRACKING %s", str);
    assert(reply != NULL && reply->type == REDIS_REPLY_STATUS);
    freeReplyObject(reply);
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

static void do_ssl_handshake(redisContext *c) {
#ifdef HIREDIS_TEST_SSL
    redisInitiateSSLWithContext(c, _ssl_ctx);
    if (c->err) {
        printf("SSL error: %s\n", c->errstr);
        redisFree(c);
        exit(1);
    }
#else
    (void) c;
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
        do_ssl_handshake(c);
    }

    return select_database(c);
}

static void do_reconnect(redisContext *c, struct config config) {
    redisReconnect(c);

    if (config.type == CONN_SSL) {
        do_ssl_handshake(c);
    }
}

static void test_format_commands(void) {
    char *cmd;
    int len;

    test("Format command without interpolation: ");
    len = redisFormatCommand(&cmd,"SET foo bar");
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(3+2)+4+(3+2));
    hi_free(cmd);

    test("Format command with %%s string interpolation: ");
    len = redisFormatCommand(&cmd,"SET %s %s","foo","bar");
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(3+2)+4+(3+2));
    hi_free(cmd);

    test("Format command with %%s and an empty string: ");
    len = redisFormatCommand(&cmd,"SET %s %s","foo","");
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$0\r\n\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(3+2)+4+(0+2));
    hi_free(cmd);

    test("Format command with an empty string in between proper interpolations: ");
    len = redisFormatCommand(&cmd,"SET %s %s","","foo");
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$0\r\n\r\n$3\r\nfoo\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(0+2)+4+(3+2));
    hi_free(cmd);

    test("Format command with %%b string interpolation: ");
    len = redisFormatCommand(&cmd,"SET %b %b","foo",(size_t)3,"b\0r",(size_t)3);
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nb\0r\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(3+2)+4+(3+2));
    hi_free(cmd);

    test("Format command with %%b and an empty string: ");
    len = redisFormatCommand(&cmd,"SET %b %b","foo",(size_t)3,"",(size_t)0);
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$0\r\n\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(3+2)+4+(0+2));
    hi_free(cmd);

    test("Format command with literal %%: ");
    len = redisFormatCommand(&cmd,"SET %% %%");
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$1\r\n%\r\n$1\r\n%\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(1+2)+4+(1+2));
    hi_free(cmd);

    /* Vararg width depends on the type. These tests make sure that the
     * width is correctly determined using the format and subsequent varargs
     * can correctly be interpolated. */
#define INTEGER_WIDTH_TEST(fmt, type) do {                                                \
    type value = 123;                                                                     \
    test("Format command with printf-delegation (" #type "): ");                          \
    len = redisFormatCommand(&cmd,"key:%08" fmt " str:%s", value, "hello");               \
    test_cond(strncmp(cmd,"*2\r\n$12\r\nkey:00000123\r\n$9\r\nstr:hello\r\n",len) == 0 && \
        len == 4+5+(12+2)+4+(9+2));                                                       \
    hi_free(cmd);                                                                         \
} while(0)

#define FLOAT_WIDTH_TEST(type) do {                                                       \
    type value = 123.0;                                                                   \
    test("Format command with printf-delegation (" #type "): ");                          \
    len = redisFormatCommand(&cmd,"key:%08.3f str:%s", value, "hello");                   \
    test_cond(strncmp(cmd,"*2\r\n$12\r\nkey:0123.000\r\n$9\r\nstr:hello\r\n",len) == 0 && \
        len == 4+5+(12+2)+4+(9+2));                                                       \
    hi_free(cmd);                                                                         \
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
    hi_free(cmd);

    test("Format command by passing argc/argv with lengths: ");
    len = redisFormatCommandArgv(&cmd,argc,argv,lens);
    test_cond(strncmp(cmd,"*3\r\n$3\r\nSET\r\n$7\r\nfoo\0xxx\r\n$3\r\nbar\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(7+2)+4+(3+2));
    hi_free(cmd);

    hisds sds_cmd;

    sds_cmd = NULL;
    test("Format command into hisds by passing argc/argv without lengths: ");
    len = redisFormatSdsCommandArgv(&sds_cmd,argc,argv,NULL);
    test_cond(strncmp(sds_cmd,"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(3+2)+4+(3+2));
    hi_sdsfree(sds_cmd);

    sds_cmd = NULL;
    test("Format command into hisds by passing argc/argv with lengths: ");
    len = redisFormatSdsCommandArgv(&sds_cmd,argc,argv,lens);
    test_cond(strncmp(sds_cmd,"*3\r\n$3\r\nSET\r\n$7\r\nfoo\0xxx\r\n$3\r\nbar\r\n",len) == 0 &&
        len == 4+4+(3+2)+4+(7+2)+4+(3+2));
    hi_sdsfree(sds_cmd);
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

    hi_free(cmd);
    freeReplyObject(reply);

    disconnect(c, 0);
}

static void test_reply_reader(void) {
    redisReader *reader;
    void *reply, *root;
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

    reader = redisReaderCreate();
    test("Can handle arbitrarily nested multi-bulks: ");
    for (i = 0; i < 128; i++) {
        redisReaderFeed(reader,(char*)"*1\r\n", 4);
    }
    redisReaderFeed(reader,(char*)"$6\r\nLOLWUT\r\n",12);
    ret = redisReaderGetReply(reader,&reply);
    root = reply; /* Keep track of the root reply */
    test_cond(ret == REDIS_OK &&
        ((redisReply*)reply)->type == REDIS_REPLY_ARRAY &&
        ((redisReply*)reply)->elements == 1);

    test("Can parse arbitrarily nested multi-bulks correctly: ");
    while(i--) {
        assert(reply != NULL && ((redisReply*)reply)->type == REDIS_REPLY_ARRAY);
        reply = ((redisReply*)reply)->element[0];
    }
    test_cond(((redisReply*)reply)->type == REDIS_REPLY_STRING &&
        !memcmp(((redisReply*)reply)->str, "LOLWUT", 6));
    freeReplyObject(root);
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

    test("Can configure maximum multi-bulk elements: ");
    reader = redisReaderCreate();
    reader->maxelements = 1024;
    redisReaderFeed(reader, "*1025\r\n", 7);
    ret = redisReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_ERR &&
              strcasecmp(reader->errstr, "Multi-bulk length out of range") == 0);
    freeReplyObject(reply);
    redisReaderFree(reader);

    test("Multi-bulk never overflows regardless of maxelements: ");
    size_t bad_mbulk_len = (SIZE_MAX / sizeof(void *)) + 3;
    char bad_mbulk_reply[100];
    snprintf(bad_mbulk_reply, sizeof(bad_mbulk_reply), "*%llu\r\n+asdf\r\n",
        (unsigned long long) bad_mbulk_len);

    reader = redisReaderCreate();
    reader->maxelements = 0;    /* Don't rely on default limit */
    redisReaderFeed(reader, bad_mbulk_reply, strlen(bad_mbulk_reply));
    ret = redisReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_ERR && strcasecmp(reader->errstr, "Out of memory") == 0);
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

    /* RESP3 verbatim strings (GitHub issue #802) */
    test("Can parse RESP3 verbatim strings: ");
    reader = redisReaderCreate();
    redisReaderFeed(reader,(char*)"=10\r\ntxt:LOLWUT\r\n",17);
    ret = redisReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_OK &&
        ((redisReply*)reply)->type == REDIS_REPLY_VERB &&
         !memcmp(((redisReply*)reply)->str,"LOLWUT", 6));
    freeReplyObject(reply);
    redisReaderFree(reader);

    /* RESP3 push messages (Github issue #815) */
    test("Can parse RESP3 push messages: ");
    reader = redisReaderCreate();
    redisReaderFeed(reader,(char*)">2\r\n$6\r\nLOLWUT\r\n:42\r\n",21);
    ret = redisReaderGetReply(reader,&reply);
    test_cond(ret == REDIS_OK &&
        ((redisReply*)reply)->type == REDIS_REPLY_PUSH &&
        ((redisReply*)reply)->elements == 2 &&
        ((redisReply*)reply)->element[0]->type == REDIS_REPLY_STRING &&
        !memcmp(((redisReply*)reply)->element[0]->str,"LOLWUT",6) &&
        ((redisReply*)reply)->element[1]->type == REDIS_REPLY_INTEGER &&
        ((redisReply*)reply)->element[1]->integer == 42);
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

static void *hi_malloc_fail(size_t size) {
    (void)size;
    return NULL;
}

static void *hi_calloc_fail(size_t nmemb, size_t size) {
    (void)nmemb;
    (void)size;
    return NULL;
}

static void *hi_realloc_fail(void *ptr, size_t size) {
    (void)ptr;
    (void)size;
    return NULL;
}

static void test_allocator_injection(void) {
    hiredisAllocFuncs ha = {
        .mallocFn = hi_malloc_fail,
        .callocFn = hi_calloc_fail,
        .reallocFn = hi_realloc_fail,
        .strdupFn = strdup,
        .freeFn = free,
    };

    // Override hiredis allocators
    hiredisSetAllocators(&ha);

    test("redisContext uses injected allocators: ");
    redisContext *c = redisConnect("localhost", 6379);
    test_cond(c == NULL);

    test("redisReader uses injected allocators: ");
    redisReader *reader = redisReaderCreate();
    test_cond(reader == NULL);

    // Return allocators to default
    hiredisResetAllocators();
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
             strcmp(c->errstr, "nodename nor servname provided, or not known") == 0 ||
             strcmp(c->errstr, "No address associated with hostname") == 0 ||
             strcmp(c->errstr, "Temporary failure in name resolution") == 0 ||
             strcmp(c->errstr, "hostname nor servname provided, or not known") == 0 ||
             strcmp(c->errstr, "no address associated with name") == 0 ||
             strcmp(c->errstr, "No such host is known. ") == 0));
        redisFree(c);
    } else {
        printf("Skipping NXDOMAIN test. Found evil ISP!\n");
        freeaddrinfo(ai_tmp);
    }

#ifndef _WIN32
    test("Returns error when the port is not open: ");
    c = redisConnect((char*)"localhost", 1);
    test_cond(c->err == REDIS_ERR_IO &&
        strcmp(c->errstr,"Connection refused") == 0);
    redisFree(c);

    test("Returns error when the unix_sock socket path doesn't accept connections: ");
    c = redisConnectUnix((char*)"/tmp/idontexist.sock");
    test_cond(c->err == REDIS_ERR_IO); /* Don't care about the message... */
    redisFree(c);
#endif
}

/* Test push handler */
void push_handler(void *privdata, void *r) {
    struct pushCounters *pcounts = privdata;
    redisReply *reply = r, *payload;

    assert(reply && reply->type == REDIS_REPLY_PUSH && reply->elements == 2);

    payload = reply->element[1];
    if (payload->type == REDIS_REPLY_ARRAY) {
        payload = payload->element[0];
    }

    if (payload->type == REDIS_REPLY_STRING) {
        pcounts->str++;
    } else if (payload->type == REDIS_REPLY_NIL) {
        pcounts->nil++;
    }

    freeReplyObject(reply);
}

/* Dummy function just to test setting a callback with redisOptions */
void push_handler_async(redisAsyncContext *ac, void *reply) {
    (void)ac;
    (void)reply;
}

static void test_resp3_push_handler(redisContext *c) {
    struct pushCounters pc = {0};
    redisPushFn *old = NULL;
    redisReply *reply;
    void *privdata;

    /* Switch to RESP3 and turn on client tracking */
    send_hello(c, 3);
    send_client_tracking(c, "ON");
    privdata = c->privdata;
    c->privdata = &pc;

    reply = redisCommand(c, "GET key:0");
    assert(reply != NULL);
    freeReplyObject(reply);

    test("RESP3 PUSH messages are handled out of band by default: ");
    reply = redisCommand(c, "SET key:0 val:0");
    test_cond(reply != NULL && reply->type == REDIS_REPLY_STATUS);
    freeReplyObject(reply);

    assert((reply = redisCommand(c, "GET key:0")) != NULL);
    freeReplyObject(reply);

    old = redisSetPushCallback(c, push_handler);
    test("We can set a custom RESP3 PUSH handler: ");
    reply = redisCommand(c, "SET key:0 val:0");
    test_cond(reply != NULL && reply->type == REDIS_REPLY_STATUS && pc.str == 1);
    freeReplyObject(reply);

    test("We properly handle a NIL invalidation payload: ");
    reply = redisCommand(c, "FLUSHDB");
    test_cond(reply != NULL && reply->type == REDIS_REPLY_STATUS && pc.nil == 1);
    freeReplyObject(reply);

    /* Unset the push callback and generate an invalidate message making
     * sure it is not handled out of band. */
    test("With no handler, PUSH replies come in-band: ");
    redisSetPushCallback(c, NULL);
    assert((reply = redisCommand(c, "GET key:0")) != NULL);
    freeReplyObject(reply);
    assert((reply = redisCommand(c, "SET key:0 invalid")) != NULL);
    test_cond(reply->type == REDIS_REPLY_PUSH);
    freeReplyObject(reply);

    test("With no PUSH handler, no replies are lost: ");
    assert(redisGetReply(c, (void**)&reply) == REDIS_OK);
    test_cond(reply != NULL && reply->type == REDIS_REPLY_STATUS);
    freeReplyObject(reply);

    /* Return to the originally set PUSH handler */
    assert(old != NULL);
    redisSetPushCallback(c, old);

    /* Switch back to RESP2 and disable tracking */
    c->privdata = privdata;
    send_client_tracking(c, "OFF");
    send_hello(c, 2);
}

redisOptions get_redis_tcp_options(struct config config) {
    redisOptions options = {0};
    REDIS_OPTIONS_SET_TCP(&options, config.tcp.host, config.tcp.port);
    return options;
}

static void test_resp3_push_options(struct config config) {
    redisAsyncContext *ac;
    redisContext *c;
    redisOptions options;

    test("We set a default RESP3 handler for redisContext: ");
    options = get_redis_tcp_options(config);
    assert((c = redisConnectWithOptions(&options)) != NULL);
    test_cond(c->push_cb != NULL);
    redisFree(c);

    test("We don't set a default RESP3 push handler for redisAsyncContext: ");
    options = get_redis_tcp_options(config);
    assert((ac = redisAsyncConnectWithOptions(&options)) != NULL);
    test_cond(ac->c.push_cb == NULL);
    redisAsyncFree(ac);

    test("Our REDIS_OPT_NO_PUSH_AUTOFREE flag works: ");
    options = get_redis_tcp_options(config);
    options.options |= REDIS_OPT_NO_PUSH_AUTOFREE;
    assert((c = redisConnectWithOptions(&options)) != NULL);
    test_cond(c->push_cb == NULL);
    redisFree(c);

    test("We can use redisOptions to set a custom PUSH handler for redisContext: ");
    options = get_redis_tcp_options(config);
    options.push_cb = push_handler;
    assert((c = redisConnectWithOptions(&options)) != NULL);
    test_cond(c->push_cb == push_handler);
    redisFree(c);

    test("We can use redisOptions to set a custom PUSH handler for redisAsyncContext: ");
    options = get_redis_tcp_options(config);
    options.async_push_cb = push_handler_async;
    assert((ac = redisAsyncConnectWithOptions(&options)) != NULL);
    test_cond(ac->push_cb == push_handler_async);
    redisAsyncFree(ac);
}

void free_privdata(void *privdata) {
    struct privdata *data = privdata;
    data->dtor_counter++;
}

static void test_privdata_hooks(struct config config) {
    struct privdata data = {0};
    redisOptions options;
    redisContext *c;

    test("We can use redisOptions to set privdata: ");
    options = get_redis_tcp_options(config);
    REDIS_OPTIONS_SET_PRIVDATA(&options, &data, free_privdata);
    assert((c = redisConnectWithOptions(&options)) != NULL);
    test_cond(c->privdata == &data);

    test("Our privdata destructor fires when we free the context: ");
    redisFree(c);
    test_cond(data.dtor_counter == 1);
}

static void test_blocking_connection(struct config config) {
    redisContext *c;
    redisReply *reply;
    int major;

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

    /* Make sure passing NULL to redisGetReply is safe */
    test("Can pass NULL to redisGetReply: ");
    assert(redisAppendCommand(c, "PING") == REDIS_OK);
    test_cond(redisGetReply(c, NULL) == REDIS_OK);

    get_redis_version(c, &major, NULL);
    if (major >= 6) test_resp3_push_handler(c);
    test_resp3_push_options(config);

    test_privdata_hooks(config);

    disconnect(c, 0);
}

/* Send DEBUG SLEEP 0 to detect if we have this command */
static int detect_debug_sleep(redisContext *c) {
    int detected;
    redisReply *reply = redisCommand(c, "DEBUG SLEEP 0\r\n");

    if (reply == NULL || c->err) {
        const char *cause = c->err ? c->errstr : "(none)";
        fprintf(stderr, "Error testing for DEBUG SLEEP (Redis error: %s), exiting\n", cause);
        exit(-1);
    }

    detected = reply->type == REDIS_REPLY_STATUS;
    freeReplyObject(reply);

    return detected;
}

static void test_blocking_connection_timeouts(struct config config) {
    redisContext *c;
    redisReply *reply;
    ssize_t s;
    const char *sleep_cmd = "DEBUG SLEEP 3\r\n";
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
    if (detect_debug_sleep(c)) {
        redisAppendFormattedCommand(c, sleep_cmd, strlen(sleep_cmd));
        s = c->funcs->write(c);
        tv.tv_sec = 0;
        tv.tv_usec = 10000;
        redisSetTimeout(c, tv);
        reply = redisCommand(c, "GET foo");
#ifndef _WIN32
        test_cond(s > 0 && reply == NULL && c->err == REDIS_ERR_IO &&
                  strcmp(c->errstr, "Resource temporarily unavailable") == 0);
#else
        test_cond(s > 0 && reply == NULL && c->err == REDIS_ERR_TIMEOUT &&
                  strcmp(c->errstr, "recv timeout") == 0);
#endif
        freeReplyObject(reply);
    } else {
        test_skipped();
    }

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
    get_redis_version(c, &major, &minor);

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

#ifndef _WIN32
    /* On 2.0, QUIT will cause the connection to be closed immediately and
     * the read(2) for the reply on QUIT will set the error to EOF.
     * On >2.0, QUIT will return with OK and another read(2) needed to be
     * issued to find out the socket was closed by the server. In both
     * conditions, the error will be set to EOF. */
    assert(c->err == REDIS_ERR_EOF &&
        strcmp(c->errstr,"Server closed the connection") == 0);
#endif
    redisFree(c);

    c = do_connect(config);
    test("Returns I/O error on socket timeout: ");
    struct timeval tv = { 0, 1000 };
    assert(redisSetTimeout(c,tv) == REDIS_OK);
    int respcode = redisGetReply(c,&_reply);
#ifndef _WIN32
    test_cond(respcode == REDIS_ERR && c->err == REDIS_ERR_IO && errno == EAGAIN);
#else
    test_cond(respcode == REDIS_ERR && c->err == REDIS_ERR_TIMEOUT);
#endif
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

/* Wrap malloc to abort on failure so OOM checks don't make the test logic
 * harder to follow. */
void *hi_malloc_safe(size_t size) {
    void *ptr = hi_malloc(size);
    if (ptr == NULL) {
        fprintf(stderr, "Error:  Out of memory\n");
        exit(-1);
    }

    return ptr;
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
    replies = hi_malloc_safe(sizeof(redisReply*)*num);
    t1 = usec();
    for (i = 0; i < num; i++) {
        replies[i] = redisCommand(c,"PING");
        assert(replies[i] != NULL && replies[i]->type == REDIS_REPLY_STATUS);
    }
    t2 = usec();
    for (i = 0; i < num; i++) freeReplyObject(replies[i]);
    hi_free(replies);
    printf("\t(%dx PING: %.3fs)\n", num, (t2-t1)/1000000.0);

    replies = hi_malloc_safe(sizeof(redisReply*)*num);
    t1 = usec();
    for (i = 0; i < num; i++) {
        replies[i] = redisCommand(c,"LRANGE mylist 0 499");
        assert(replies[i] != NULL && replies[i]->type == REDIS_REPLY_ARRAY);
        assert(replies[i] != NULL && replies[i]->elements == 500);
    }
    t2 = usec();
    for (i = 0; i < num; i++) freeReplyObject(replies[i]);
    hi_free(replies);
    printf("\t(%dx LRANGE with 500 elements: %.3fs)\n", num, (t2-t1)/1000000.0);

    replies = hi_malloc_safe(sizeof(redisReply*)*num);
    t1 = usec();
    for (i = 0; i < num; i++) {
        replies[i] = redisCommand(c, "INCRBY incrkey %d", 1000000);
        assert(replies[i] != NULL && replies[i]->type == REDIS_REPLY_INTEGER);
    }
    t2 = usec();
    for (i = 0; i < num; i++) freeReplyObject(replies[i]);
    hi_free(replies);
    printf("\t(%dx INCRBY: %.3fs)\n", num, (t2-t1)/1000000.0);

    num = 10000;
    replies = hi_malloc_safe(sizeof(redisReply*)*num);
    for (i = 0; i < num; i++)
        redisAppendCommand(c,"PING");
    t1 = usec();
    for (i = 0; i < num; i++) {
        assert(redisGetReply(c, (void*)&replies[i]) == REDIS_OK);
        assert(replies[i] != NULL && replies[i]->type == REDIS_REPLY_STATUS);
    }
    t2 = usec();
    for (i = 0; i < num; i++) freeReplyObject(replies[i]);
    hi_free(replies);
    printf("\t(%dx PING (pipelined): %.3fs)\n", num, (t2-t1)/1000000.0);

    replies = hi_malloc_safe(sizeof(redisReply*)*num);
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
    hi_free(replies);
    printf("\t(%dx LRANGE with 500 elements (pipelined): %.3fs)\n", num, (t2-t1)/1000000.0);

    replies = hi_malloc_safe(sizeof(redisReply*)*num);
    for (i = 0; i < num; i++)
        redisAppendCommand(c,"INCRBY incrkey %d", 1000000);
    t1 = usec();
    for (i = 0; i < num; i++) {
        assert(redisGetReply(c, (void*)&replies[i]) == REDIS_OK);
        assert(replies[i] != NULL && replies[i]->type == REDIS_REPLY_INTEGER);
    }
    t2 = usec();
    for (i = 0; i < num; i++) freeReplyObject(replies[i]);
    hi_free(replies);
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
    int skips_as_fails = 0;
    int test_unix_socket;

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
        } else if (argc >= 1 && !strcmp(argv[0],"--skips-as-fails")) {
            skips_as_fails = 1;
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

#ifndef _WIN32
    /* Ignore broken pipe signal (for I/O error tests). */
    signal(SIGPIPE, SIG_IGN);

    test_unix_socket = access(cfg.unix_sock.path, F_OK) == 0;

#else
    /* Unix sockets don't exist in Windows */
    test_unix_socket = 0;
#endif

    test_allocator_injection();

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

    printf("\nTesting against Unix socket connection (%s): ", cfg.unix_sock.path);
    if (test_unix_socket) {
        printf("\n");
        cfg.type = CONN_UNIX;
        test_blocking_connection(cfg);
        test_blocking_connection_timeouts(cfg);
        test_blocking_io_errors(cfg);
        if (throughput) test_throughput(cfg);
    } else {
        test_skipped();
    }

#ifdef HIREDIS_TEST_SSL
    if (cfg.ssl.port && cfg.ssl.host) {

        redisInitOpenSSL();
        _ssl_ctx = redisCreateSSLContext(cfg.ssl.ca_cert, NULL, cfg.ssl.cert, cfg.ssl.key, NULL, NULL);
        assert(_ssl_ctx != NULL);

        printf("\nTesting against SSL connection (%s:%d):\n", cfg.ssl.host, cfg.ssl.port);
        cfg.type = CONN_SSL;

        test_blocking_connection(cfg);
        test_blocking_connection_timeouts(cfg);
        test_blocking_io_errors(cfg);
        test_invalid_timeout_errors(cfg);
        test_append_formatted_commands(cfg);
        if (throughput) test_throughput(cfg);

        redisFreeSSLContext(_ssl_ctx);
        _ssl_ctx = NULL;
    }
#endif

    if (test_inherit_fd) {
        printf("\nTesting against inherited fd (%s): ", cfg.unix_sock.path);
        if (test_unix_socket) {
            printf("\n");
            cfg.type = CONN_FD;
            test_blocking_connection(cfg);
        } else {
            test_skipped();
        }
    }

    if (fails || (skips_as_fails && skips)) {
        printf("*** %d TESTS FAILED ***\n", fails);
        if (skips) {
            printf("*** %d TESTS SKIPPED ***\n", skips);
        }
        return 1;
    }

    printf("ALL TESTS PASSED (%d skipped)\n", skips);
    return 0;
}

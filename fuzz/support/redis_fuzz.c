#include "redis_fuzz.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "crc64.h"
#include "dict.h"
#include "mt19937-64.h"
#include "zmalloc.h"

extern void redisOutOfMemoryHandler(size_t allocation_size);
extern void initServerConfig(void);
extern void initServer(void);

static int redis_fuzz_initialized = 0;

static connection *redisFuzzCreateConnection(int *peer_fd) {
    int fds[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -1) return NULL;

    connection *conn = connCreateAccepted(server.el, connectionTypeUnix(), fds[0], NULL);
    if (connAccept(conn, NULL) == C_ERR) {
        connClose(conn);
        close(fds[1]);
        return NULL;
    }

    *peer_fd = fds[1];
    return conn;
}

void redisFuzzInit(void) {
    if (redis_fuzz_initialized) return;

    static const int redis_fuzz_signals[] = {
        SIGTERM, SIGINT, SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT
    };
    struct sigaction saved_actions[
        sizeof(redis_fuzz_signals) / sizeof(redis_fuzz_signals[0])];
    struct timeval tv;

    /* initServer installs Redis's production crash and shutdown handlers.
     * Preserve the libFuzzer/sanitizer handlers so failures produce artifacts
     * instead of entering Redis's interactive crash-memory test. */
    for (size_t i = 0;
         i < sizeof(redis_fuzz_signals) / sizeof(redis_fuzz_signals[0]);
         i++)
    {
        sigaction(redis_fuzz_signals[i], NULL, &saved_actions[i]);
    }

    tzset();
    zmalloc_set_oom_handler(redisOutOfMemoryHandler);
    gettimeofday(&tv, NULL);
    srand((unsigned int)(time(NULL) ^ getpid() ^ tv.tv_usec));
    srandom((unsigned int)(time(NULL) ^ getpid() ^ tv.tv_usec));
    init_genrand64(((long long)tv.tv_sec * 1000000 + tv.tv_usec) ^ getpid());
    crc64_init();

    uint8_t hashseed[16];
    getRandomBytes(hashseed, sizeof(hashseed));
    dictSetHashFunctionSeed(hashseed);

    initServerConfig();
    resetServerSaveParams();
    server.port = 0;
    server.tls_port = 0;
    server.aof_enabled = 0;
    server.protected_mode = 0;
    /* Keep sanitizer/libFuzzer signal handlers in control of crash artifacts. */
    server.crashlog_enabled = 0;
    ACLInit();
    moduleInitModulesSystem();
    connTypeInitialize();
    keyMetaInit();
    initServer();

    for (size_t i = 0;
         i < sizeof(redis_fuzz_signals) / sizeof(redis_fuzz_signals[0]);
         i++)
    {
        sigaction(redis_fuzz_signals[i], &saved_actions[i], NULL);
    }

    redis_fuzz_initialized = 1;
}

void redisFuzzReset(void) {
    updateCachedTime(0);
    server.dirty += emptyData(-1, EMPTYDB_NO_FLAGS | EMPTYDB_NOFUNCTIONS, NULL);
}

static void redisFuzzRunRespWithHooks(sds resp, RedisFuzzInspectFunc inspect,
                                     RedisFuzzPostHook hook, void *ctx) {
    redisFuzzInit();
    updateCachedTime(0);

    int peer_fd = -1;
    connection *conn = redisFuzzCreateConnection(&peer_fd);
    if (!conn) {
        serverPanic("Failed to create fuzz client connection: %s", strerror(errno));
    }

    client *c = createClient(conn);
    c->querybuf = resp;
    c->querybuf_peak = sdslen(resp);
    if (processInputBuffer(c) == C_OK) {
        if (inspect) inspect(c, ctx);
        freeClient(c);
    }
    close(peer_fd);

    if (hook) hook(ctx);
    redisFuzzReset();
}

void redisFuzzRunRespWithInspect(sds resp, RedisFuzzInspectFunc inspect, void *ctx) {
    redisFuzzRunRespWithHooks(resp, inspect, NULL, ctx);
}

void redisFuzzRunRespWithPostHook(sds resp, RedisFuzzPostHook hook, void *ctx) {
    redisFuzzRunRespWithHooks(resp, NULL, hook, ctx);
}

void redisFuzzRunResp(sds resp) {
    redisFuzzRunRespWithHooks(resp, NULL, NULL, NULL);
}

uint8_t redisFuzzByte(RedisFuzzInput *in) {
    if (in->pos >= in->size) return 0;
    return in->data[in->pos++];
}

long long redisFuzzChoice(RedisFuzzInput *in, long long count) {
    if (count <= 0) return 0;
    return redisFuzzByte(in) % count;
}

sds redisFuzzSlice(RedisFuzzInput *in, size_t maxlen) {
    size_t len = redisFuzzByte(in);
    size_t remaining = in->size - in->pos;
    if (len > maxlen) len = maxlen;
    if (len > remaining) len = remaining;
    sds out = sdsnewlen(in->data + in->pos, len);
    in->pos += len;
    return out;
}

void redisFuzzAppendArray(sds *resp, int argc) {
    *resp = sdscatfmt(*resp, "*%i\r\n", argc);
}

void redisFuzzAppendBulk(sds *resp, const char *data, size_t len) {
    *resp = sdscatfmt(*resp, "$%U\r\n", (unsigned long long)len);
    *resp = sdscatlen(*resp, data, len);
    *resp = sdscatlen(*resp, "\r\n", 2);
}

void redisFuzzAppendBulkCString(sds *resp, const char *str) {
    redisFuzzAppendBulk(resp, str, strlen(str));
}

void redisFuzzAppendBulkSds(sds *resp, sds value) {
    redisFuzzAppendBulk(resp, value, sdslen(value));
}

void redisFuzzAppendSmallNumber(sds *resp, RedisFuzzInput *in) {
    static const char *special[] = {
        "-9223372036854775808",
        "-2147483649",
        "-1",
        "0",
        "1",
        "7",
        "8",
        "31",
        "32",
        "63",
        "64",
        "127",
        "128",
        "255",
        "256",
        "511",
        "512",
        "1024",
        "4096",
        "16384",
        "9223372036854775807",
        "not-an-int"
    };

    if (redisFuzzChoice(in, 4) == 0) {
        sds raw = redisFuzzSlice(in, 16);
        redisFuzzAppendBulkSds(resp, raw);
        sdsfree(raw);
        return;
    }

    redisFuzzAppendBulkCString(resp, special[redisFuzzChoice(in, sizeof(special) / sizeof(special[0]))]);
}

void redisFuzzAppendKey(sds *resp, RedisFuzzInput *in) {
    static const char *keys[] = {
        "k0", "k1", "k2", "k3", "{slot}a", "{slot}b", "dst", "src"
    };
    redisFuzzAppendBulkCString(resp, keys[redisFuzzChoice(in, sizeof(keys) / sizeof(keys[0]))]);
}

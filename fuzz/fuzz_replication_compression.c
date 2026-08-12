#include "redis_fuzz.h"

#ifndef USE_COMPRESSION
#error "fuzz_replication_compression requires BUILD_COMPRESSION=yes"
#endif

#include "anet.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CONTROL_BYTES 8
#define MAX_PLAINTEXT 8192
#define MAX_DECOMPRESSED 65536
#define MAX_IO_STEPS 131072

typedef enum {
    DECOMPRESS_OK,
    DECOMPRESS_ERROR,
    DECOMPRESS_LIMIT,
    DECOMPRESS_STALLED,
} DecompressResult;

static void fuzzFail(void) {
    __builtin_trap();
}

static uint8_t scheduleByte(const uint8_t *data, size_t size, size_t index) {
    if (size == 0) return 0;
    return data[index % size];
}

static client *createCompressionClient(compressionDirection direction, int level,
                                       int *peer_fd)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -1)
        fuzzFail();
    if (anetNonBlock(NULL, fds[0]) != ANET_OK ||
        anetNonBlock(NULL, fds[1]) != ANET_OK)
    {
        close(fds[0]);
        close(fds[1]);
        fuzzFail();
    }

    connection *conn =
        connCreateAccepted(server.el, connectionTypeUnix(), fds[0], NULL);
    if (conn == NULL || connAccept(conn, NULL) == C_ERR) {
        if (conn) connClose(conn);
        else close(fds[0]);
        close(fds[1]);
        fuzzFail();
    }

    client *c = createClient(conn);
    if (direction == COMPRESS) {
        c->flags |= CLIENT_SLAVE;
        c->compression_level = level;
    } else {
        c->flags |= CLIENT_MASTER;
        server.repl_master_compression_level = level;
    }

    if (!clientEnableCompression(c, direction)) {
        c->flags &= ~(CLIENT_MASTER | CLIENT_SLAVE);
        freeClient(c);
        close(fds[1]);
        fuzzFail();
    }

    /* The flags are only required while initializing the state. Keeping them
     * set would make freeClient run replication-specific teardown unrelated to
     * this target. */
    c->flags &= ~(CLIENT_MASTER | CLIENT_SLAVE);
    *peer_fd = fds[1];
    return c;
}

static void destroyCompressionClient(client *c, int peer_fd) {
    freeClient(c);
    close(peer_fd);
}

static int drainPeer(int fd, sds *output) {
    unsigned char buf[4096];

    while (1) {
        ssize_t nread = read(fd, buf, sizeof(buf));
        if (nread > 0) {
            *output = sdscatlen(*output, buf, (size_t)nread);
            continue;
        }
        if (nread == 0) return 1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
        return 0;
    }
}

static int flushCompressor(client *c, int peer_fd, sds *compressed) {
    for (int attempts = 0; attempts < 16; attempts++) {
        int written = clientFlushCompressedData(c);
        if (written < 0 || !drainPeer(peer_fd, compressed)) return 0;
        if (written == 0) return 1;
    }
    return 0;
}

static sds compressPlaintext(const unsigned char *plaintext, size_t plaintext_len,
                             const uint8_t *schedule, size_t schedule_len,
                             size_t schedule_salt, int level)
{
    int peer_fd = -1;
    client *c = createCompressionClient(COMPRESS, level, &peer_fd);
    sds compressed = sdsempty();

    /* A fresh state considers its latency deadline expired. Intentionally
     * finish that initial empty frame so all following frame boundaries are
     * controlled only by the fuzzed flush schedule. This also covers empty and
     * concatenated Zstd frames. */
    int written = 0;
    if (clientCompressAndWrite(c, &written, 1) != 0 ||
        !drainPeer(peer_fd, &compressed))
    {
        sdsfree(compressed);
        destroyCompressionClient(c, peer_fd);
        return NULL;
    }

    size_t pos = 0;
    size_t step = 0;
    while (pos < plaintext_len && step < MAX_IO_STEPS) {
        size_t chunk = 1 +
            (scheduleByte(schedule, schedule_len,
                          schedule_salt + step * 5) % 257);
        if (chunk > plaintext_len - pos) chunk = plaintext_len - pos;
        size_t chunk_end = pos + chunk;

        while (pos < chunk_end && step < MAX_IO_STEPS) {
            int socket_written = 0;
            int consumed = clientConnWrite(c, plaintext + pos,
                                           chunk_end - pos, &socket_written);
            if (consumed < 0 || !drainPeer(peer_fd, &compressed)) {
                sdsfree(compressed);
                destroyCompressionClient(c, peer_fd);
                return NULL;
            }
            step++;
            if (consumed == 0) {
                if (!flushCompressor(c, peer_fd, &compressed)) {
                    sdsfree(compressed);
                    destroyCompressionClient(c, peer_fd);
                    return NULL;
                }
                continue;
            }
            pos += (size_t)consumed;
        }

        if (scheduleByte(schedule, schedule_len,
                         schedule_salt + step * 7 + 1) & 1)
        {
            if (!flushCompressor(c, peer_fd, &compressed)) {
                sdsfree(compressed);
                destroyCompressionClient(c, peer_fd);
                return NULL;
            }
        }
    }

    if (pos != plaintext_len ||
        !flushCompressor(c, peer_fd, &compressed) ||
        !drainPeer(peer_fd, &compressed))
    {
        sdsfree(compressed);
        compressed = NULL;
    }

    destroyCompressionClient(c, peer_fd);
    return compressed;
}

static int appendDecompressed(sds *output, const unsigned char *buf, size_t len) {
    if (len > MAX_DECOMPRESSED - sdslen(*output)) return 0;
    *output = sdscatlen(*output, buf, len);
    return 1;
}

static DecompressResult decompressStream(const unsigned char *compressed,
                                         size_t compressed_len,
                                         const uint8_t *schedule,
                                         size_t schedule_len,
                                         sds *decompressed)
{
    int peer_fd = -1;
    client *c = createCompressionClient(DECOMPRESS, 1, &peer_fd);
    unsigned char output_buf[513];
    DecompressResult result = DECOMPRESS_OK;
    size_t pos = 0;
    size_t step = 0;

    while (pos < compressed_len && step < MAX_IO_STEPS) {
        size_t input_chunk = 1 +
            (scheduleByte(schedule, schedule_len, 3 + step * 11) % 129);
        if (input_chunk > compressed_len - pos)
            input_chunk = compressed_len - pos;
        size_t chunk_end = pos + input_chunk;

        while (pos < chunk_end && step < MAX_IO_STEPS) {
            size_t output_len = 1 +
                (scheduleByte(schedule, schedule_len, 5 + step * 13) %
                 sizeof(output_buf));
            size_t consumed = 0;
            int produced = clientReadBufAndDecompress(
                c, (char *)compressed + pos, chunk_end - pos,
                (char *)output_buf, output_len, &consumed);

            if (produced < 0) {
                result = DECOMPRESS_ERROR;
                goto done;
            }
            if (consumed > chunk_end - pos ||
                (size_t)produced > output_len)
            {
                fuzzFail();
            }
            if (produced > 0 &&
                !appendDecompressed(decompressed, output_buf, (size_t)produced))
            {
                result = DECOMPRESS_LIMIT;
                goto done;
            }

            pos += consumed;
            step++;
            if (consumed == 0 && produced == 0) {
                result = DECOMPRESS_STALLED;
                goto done;
            }
        }
    }

    if (pos != compressed_len) {
        result = DECOMPRESS_LIMIT;
        goto done;
    }

    /* A frame can leave decompressed bytes buffered after all caller-owned
     * compressed chunks have been consumed. Exercise the zero-input drain used
     * by replication until no more progress is possible. */
    while (clientHasPendingCompressedData(c) && step < MAX_IO_STEPS) {
        size_t output_len = 1 +
            (scheduleByte(schedule, schedule_len, 7 + step * 17) %
             sizeof(output_buf));
        size_t consumed = 0;
        int produced = clientReadBufAndDecompress(
            c, NULL, 0, (char *)output_buf, output_len, &consumed);

        if (produced < 0) {
            result = DECOMPRESS_ERROR;
            goto done;
        }
        if (consumed != 0 || (size_t)produced > output_len)
            fuzzFail();
        if (produced > 0 &&
            !appendDecompressed(decompressed, output_buf, (size_t)produced))
        {
            result = DECOMPRESS_LIMIT;
            goto done;
        }
        step++;
        if (produced == 0) break;
    }

done:
    destroyCompressionClient(c, peer_fd);
    return result;
}

static int compressionLevel(uint8_t selector) {
    static const int levels[] = {1, 2, 3, 6, 9, 15, 19, 22};
    return levels[selector % (sizeof(levels) / sizeof(levels[0]))];
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    redisFuzzInit();
    server.verbosity = LL_WARNING;
    server.compression_max_latency = INT_MAX;

    uint8_t controls[CONTROL_BYTES] = {0};
    size_t controls_len = size < CONTROL_BYTES ? size : CONTROL_BYTES;
    if (controls_len) memcpy(controls, data, controls_len);

    size_t payload_offset = controls_len;
    size_t payload_len = size - payload_offset;
    if (payload_len > MAX_PLAINTEXT) payload_len = MAX_PLAINTEXT;
    const unsigned char *payload = data + payload_offset;

    int mode = controls[0] & 7;
    int level = compressionLevel(controls[1]);
    sds stream = NULL;
    sds expected = NULL;

    if (mode == 3) {
        /* Raw bytes cover arbitrary, malformed, and accidentally valid Zstd
         * input independently of Redis's compressor. */
        stream = sdsnewlen(payload, payload_len);
    } else if (mode == 4) {
        /* Independently compressed halves create concatenated frames whose
         * plaintext has a precise oracle. */
        size_t split = payload_len ?
            controls[3] % (payload_len + 1) : 0;
        sds first = compressPlaintext(payload, split, data, size, 19, level);
        sds second = compressPlaintext(payload + split, payload_len - split,
                                       data, size, 37, level);
        if (!first || !second) fuzzFail();
        stream = sdsempty();
        stream = sdscatsds(stream, first);
        stream = sdscatsds(stream, second);
        expected = sdsnewlen(payload, payload_len);
        sdsfree(first);
        sdsfree(second);
    } else {
        sds valid =
            compressPlaintext(payload, payload_len, data, size, 11, level);
        if (!valid) fuzzFail();

        switch (mode) {
        case 0:
            stream = sdsdup(valid);
            expected = sdsnewlen(payload, payload_len);
            break;
        case 1: {
            size_t keep = sdslen(valid) ?
                controls[2] % sdslen(valid) : 0;
            stream = sdsnewlen(valid, keep);
            break;
        }
        case 2: {
            stream = sdsdup(valid);
            if (sdslen(stream)) {
                size_t offset = controls[2] % sdslen(stream);
                stream[offset] ^= (unsigned char)(1u << (controls[3] & 7));
            }
            break;
        }
        case 5:
            stream = sdsdup(valid);
            stream = sdscatlen(stream, payload, payload_len);
            break;
        case 6:
            stream = sdsnewlen(payload, payload_len);
            stream = sdscatsds(stream, valid);
            break;
        case 7:
            stream = sdsempty();
            stream = sdscatsds(stream, valid);
            stream = sdscatsds(stream, valid);
            expected = sdsnewlen(payload, payload_len);
            expected = sdscatlen(expected, payload, payload_len);
            break;
        default:
            fuzzFail();
        }
        sdsfree(valid);
    }

    sds actual = sdsempty();
    DecompressResult result =
        decompressStream((const unsigned char *)stream, sdslen(stream),
                         data, size, &actual);

    if (expected &&
        (result != DECOMPRESS_OK ||
         sdslen(actual) != sdslen(expected) ||
         memcmp(actual, expected, sdslen(expected)) != 0))
    {
        fuzzFail();
    }

    sdsfree(actual);
    sdsfree(expected);
    sdsfree(stream);
    return 0;
}

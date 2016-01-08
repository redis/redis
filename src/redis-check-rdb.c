/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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


#include "server.h"
#include "rdb.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "lzf.h"
#include "crc64.h"

#define ERROR(...) { \
    serverLog(LL_WARNING, __VA_ARGS__); \
    exit(1); \
}

/* data type to hold offset in file and size */
typedef struct {
    void *data;
    size_t size;
    size_t offset;
} pos;

static unsigned char level = 0;
static pos positions[16];

#define CURR_OFFSET (positions[level].offset)

/* Hold a stack of errors */
typedef struct {
    char error[16][1024];
    size_t offset[16];
    size_t level;
} errors_t;
static errors_t errors;

#define SHIFT_ERROR(provided_offset, ...) { \
    sprintf(errors.error[errors.level], __VA_ARGS__); \
    errors.offset[errors.level] = provided_offset; \
    errors.level++; \
}

/* Data type to hold opcode with optional key name an success status */
typedef struct {
    char* key;
    int type;
    char success;
} entry;

#define MAX_TYPES_NUM 256
#define MAX_TYPE_NAME_LEN 16
/* store string types for output */
static char types[MAX_TYPES_NUM][MAX_TYPE_NAME_LEN];

/* Return true if 't' is a valid object type. */
static int rdbCheckType(unsigned char t) {
    /* In case a new object type is added, update the following
     * condition as necessary. */
    return
        (t >= RDB_TYPE_HASH_ZIPMAP && t <= RDB_TYPE_HASH_ZIPLIST) ||
        t <= RDB_TYPE_HASH ||
        t >= RDB_OPCODE_EXPIRETIME_MS;
}

/* when number of bytes to read is negative, do a peek */
static int readBytes(void *target, long num) {
    char peek = (num < 0) ? 1 : 0;
    num = (num < 0) ? -num : num;

    pos p = positions[level];
    if (p.offset + num > p.size) {
        return 0;
    } else {
        memcpy(target, (void*)((size_t)p.data + p.offset), num);
        if (!peek) positions[level].offset += num;
    }
    return 1;
}

int processHeader(void) {
    char buf[10] = "_________";
    int dump_version;

    if (!readBytes(buf, 9)) {
        ERROR("Cannot read header");
    }

    /* expect the first 5 bytes to equal REDIS */
    if (memcmp(buf,"REDIS",5) != 0) {
        ERROR("Wrong signature in header");
    }

    dump_version = (int)strtol(buf + 5, NULL, 10);
    if (dump_version < 1 || dump_version > 6) {
        ERROR("Unknown RDB format version: %d", dump_version);
    }
    return dump_version;
}

static int loadType(entry *e) {
    uint32_t offset = CURR_OFFSET;

    /* this byte needs to qualify as type */
    unsigned char t;
    if (readBytes(&t, 1)) {
        if (rdbCheckType(t)) {
            e->type = t;
            return 1;
        } else {
            SHIFT_ERROR(offset, "Unknown type (0x%02x)", t);
        }
    } else {
        SHIFT_ERROR(offset, "Could not read type");
    }

    /* failure */
    return 0;
}

static int peekType() {
    unsigned char t;
    if (readBytes(&t, -1) && (rdbCheckType(t)))
        return t;
    return -1;
}

/* discard time, just consume the bytes */
static int processTime(int type) {
    uint32_t offset = CURR_OFFSET;
    unsigned char t[8];
    int timelen = (type == RDB_OPCODE_EXPIRETIME_MS) ? 8 : 4;

    if (readBytes(t,timelen)) {
        return 1;
    } else {
        SHIFT_ERROR(offset, "Could not read time");
    }

    /* failure */
    return 0;
}

static uint32_t loadLength(int *isencoded) {
    unsigned char buf[2];
    uint32_t len;
    int type;

    if (isencoded) *isencoded = 0;
    if (!readBytes(buf, 1)) return RDB_LENERR;
    type = (buf[0] & 0xC0) >> 6;
    if (type == RDB_6BITLEN) {
        /* Read a 6 bit len */
        return buf[0] & 0x3F;
    } else if (type == RDB_ENCVAL) {
        /* Read a 6 bit len encoding type */
        if (isencoded) *isencoded = 1;
        return buf[0] & 0x3F;
    } else if (type == RDB_14BITLEN) {
        /* Read a 14 bit len */
        if (!readBytes(buf+1,1)) return RDB_LENERR;
        return ((buf[0] & 0x3F) << 8) | buf[1];
    } else {
        /* Read a 32 bit len */
        if (!readBytes(&len, 4)) return RDB_LENERR;
        return (unsigned int)ntohl(len);
    }
}

static char *loadIntegerObject(int enctype) {
    uint32_t offset = CURR_OFFSET;
    unsigned char enc[4];
    long long val;

    if (enctype == RDB_ENC_INT8) {
        uint8_t v;
        if (!readBytes(enc, 1)) return NULL;
        v = enc[0];
        val = (int8_t)v;
    } else if (enctype == RDB_ENC_INT16) {
        uint16_t v;
        if (!readBytes(enc, 2)) return NULL;
        v = enc[0]|(enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == RDB_ENC_INT32) {
        uint32_t v;
        if (!readBytes(enc, 4)) return NULL;
        v = enc[0]|(enc[1]<<8)|(enc[2]<<16)|(enc[3]<<24);
        val = (int32_t)v;
    } else {
        SHIFT_ERROR(offset, "Unknown integer encoding (0x%02x)", enctype);
        return NULL;
    }

    /* convert val into string */
    char *buf;
    buf = zmalloc(sizeof(char) * 128);
    sprintf(buf, "%lld", val);
    return buf;
}

static char* loadLzfStringObject() {
    unsigned int slen, clen;
    char *c, *s;

    if ((clen = loadLength(NULL)) == RDB_LENERR) return NULL;
    if ((slen = loadLength(NULL)) == RDB_LENERR) return NULL;

    c = zmalloc(clen);
    if (!readBytes(c, clen)) {
        zfree(c);
        return NULL;
    }

    s = zmalloc(slen+1);
    if (lzf_decompress(c,clen,s,slen) == 0) {
        zfree(c); zfree(s);
        return NULL;
    }

    zfree(c);
    return s;
}

/* returns NULL when not processable, char* when valid */
static char* loadStringObject() {
    uint32_t offset = CURR_OFFSET;
    int isencoded;
    uint32_t len;

    len = loadLength(&isencoded);
    if (isencoded) {
        switch(len) {
        case RDB_ENC_INT8:
        case RDB_ENC_INT16:
        case RDB_ENC_INT32:
            return loadIntegerObject(len);
        case RDB_ENC_LZF:
            return loadLzfStringObject();
        default:
            /* unknown encoding */
            SHIFT_ERROR(offset, "Unknown string encoding (0x%02x)", len);
            return NULL;
        }
    }

    if (len == RDB_LENERR) return NULL;

    char *buf = zmalloc(sizeof(char) * (len+1));
    if (buf == NULL) return NULL;
    buf[len] = '\0';
    if (!readBytes(buf, len)) {
        zfree(buf);
        return NULL;
    }
    return buf;
}

static int processStringObject(char** store) {
    unsigned long offset = CURR_OFFSET;
    char *key = loadStringObject();
    if (key == NULL) {
        SHIFT_ERROR(offset, "Error reading string object");
        zfree(key);
        return 0;
    }

    if (store != NULL) {
        *store = key;
    } else {
        zfree(key);
    }
    return 1;
}

static double* loadDoubleValue() {
    char buf[256];
    unsigned char len;
    double* val;

    if (!readBytes(&len,1)) return NULL;

    val = zmalloc(sizeof(double));
    switch(len) {
    case 255: *val = R_NegInf;  return val;
    case 254: *val = R_PosInf;  return val;
    case 253: *val = R_Nan;     return val;
    default:
        if (!readBytes(buf, len)) {
            zfree(val);
            return NULL;
        }
        buf[len] = '\0';
        sscanf(buf, "%lg", val);
        return val;
    }
}

static int processDoubleValue(double** store) {
    unsigned long offset = CURR_OFFSET;
    double *val = loadDoubleValue();
    if (val == NULL) {
        SHIFT_ERROR(offset, "Error reading double value");
        zfree(val);
        return 0;
    }

    if (store != NULL) {
        *store = val;
    } else {
        zfree(val);
    }
    return 1;
}

static int loadPair(entry *e) {
    uint32_t offset = CURR_OFFSET;
    uint32_t i;

    /* read key first */
    char *key;
    if (processStringObject(&key)) {
        e->key = key;
    } else {
        SHIFT_ERROR(offset, "Error reading entry key");
        return 0;
    }

    uint32_t length = 0;
    if (e->type == RDB_TYPE_LIST ||
        e->type == RDB_TYPE_SET  ||
        e->type == RDB_TYPE_ZSET ||
        e->type == RDB_TYPE_HASH) {
        if ((length = loadLength(NULL)) == RDB_LENERR) {
            SHIFT_ERROR(offset, "Error reading %s length", types[e->type]);
            return 0;
        }
    }

    switch(e->type) {
    case RDB_TYPE_STRING:
    case RDB_TYPE_HASH_ZIPMAP:
    case RDB_TYPE_LIST_ZIPLIST:
    case RDB_TYPE_SET_INTSET:
    case RDB_TYPE_ZSET_ZIPLIST:
    case RDB_TYPE_HASH_ZIPLIST:
        if (!processStringObject(NULL)) {
            SHIFT_ERROR(offset, "Error reading entry value");
            return 0;
        }
    break;
    case RDB_TYPE_LIST:
    case RDB_TYPE_SET:
        for (i = 0; i < length; i++) {
            offset = CURR_OFFSET;
            if (!processStringObject(NULL)) {
                SHIFT_ERROR(offset, "Error reading element at index %d (length: %d)", i, length);
                return 0;
            }
        }
    break;
    case RDB_TYPE_ZSET:
        for (i = 0; i < length; i++) {
            offset = CURR_OFFSET;
            if (!processStringObject(NULL)) {
                SHIFT_ERROR(offset, "Error reading element key at index %d (length: %d)", i, length);
                return 0;
            }
            offset = CURR_OFFSET;
            if (!processDoubleValue(NULL)) {
                SHIFT_ERROR(offset, "Error reading element value at index %d (length: %d)", i, length);
                return 0;
            }
        }
    break;
    case RDB_TYPE_HASH:
        for (i = 0; i < length; i++) {
            offset = CURR_OFFSET;
            if (!processStringObject(NULL)) {
                SHIFT_ERROR(offset, "Error reading element key at index %d (length: %d)", i, length);
                return 0;
            }
            offset = CURR_OFFSET;
            if (!processStringObject(NULL)) {
                SHIFT_ERROR(offset, "Error reading element value at index %d (length: %d)", i, length);
                return 0;
            }
        }
    break;
    default:
        SHIFT_ERROR(offset, "Type not implemented");
        return 0;
    }
    /* because we're done, we assume success */
    e->success = 1;
    return 1;
}

static entry loadEntry() {
    entry e = { NULL, -1, 0 };
    uint32_t length, offset[4];

    /* reset error container */
    errors.level = 0;

    offset[0] = CURR_OFFSET;
    if (!loadType(&e)) {
        return e;
    }

    offset[1] = CURR_OFFSET;
    if (e.type == RDB_OPCODE_SELECTDB) {
        if ((length = loadLength(NULL)) == RDB_LENERR) {
            SHIFT_ERROR(offset[1], "Error reading database number");
            return e;
        }
        if (length > 63) {
            SHIFT_ERROR(offset[1], "Database number out of range (%d)", length);
            return e;
        }
    } else if (e.type == RDB_OPCODE_EOF) {
        if (positions[level].offset < positions[level].size) {
            SHIFT_ERROR(offset[0], "Unexpected EOF");
        } else {
            e.success = 1;
        }
        return e;
    } else {
        /* optionally consume expire */
        if (e.type == RDB_OPCODE_EXPIRETIME ||
            e.type == RDB_OPCODE_EXPIRETIME_MS) {
            if (!processTime(e.type)) return e;
            if (!loadType(&e)) return e;
        }

        offset[1] = CURR_OFFSET;
        if (!loadPair(&e)) {
            SHIFT_ERROR(offset[1], "Error for type %s", types[e.type]);
            return e;
        }
    }

    /* all entries are followed by a valid type:
     * e.g. a new entry, SELECTDB, EXPIRE, EOF */
    offset[2] = CURR_OFFSET;
    if (peekType() == -1) {
        SHIFT_ERROR(offset[2], "Followed by invalid type");
        SHIFT_ERROR(offset[0], "Error for type %s", types[e.type]);
        e.success = 0;
    } else {
        e.success = 1;
    }

    return e;
}

static void printCentered(int indent, int width, char* body) {
    char head[256], tail[256];
    memset(head, '\0', 256);
    memset(tail, '\0', 256);

    memset(head, '=', indent);
    memset(tail, '=', width - 2 - indent - strlen(body));
    serverLog(LL_WARNING, "%s %s %s", head, body, tail);
}

static void printValid(uint64_t ops, uint64_t bytes) {
    char body[80];
    sprintf(body, "Processed %llu valid opcodes (in %llu bytes)",
        (unsigned long long) ops, (unsigned long long) bytes);
    printCentered(4, 80, body);
}

static void printSkipped(uint64_t bytes, uint64_t offset) {
    char body[80];
    sprintf(body, "Skipped %llu bytes (resuming at 0x%08llx)",
        (unsigned long long) bytes, (unsigned long long) offset);
    printCentered(4, 80, body);
}

static void printErrorStack(entry *e) {
    unsigned int i;
    char body[64];

    if (e->type == -1) {
        sprintf(body, "Error trace");
    } else if (e->type >= 253) {
        sprintf(body, "Error trace (%s)", types[e->type]);
    } else if (!e->key) {
        sprintf(body, "Error trace (%s: (unknown))", types[e->type]);
    } else {
        char tmp[41];
        strncpy(tmp, e->key, 40);

        /* display truncation at the last 3 chars */
        if (strlen(e->key) > 40) {
            memset(&tmp[37], '.', 3);
        }

        /* display unprintable characters as ? */
        for (i = 0; i < strlen(tmp); i++) {
            if (tmp[i] <= 32) tmp[i] = '?';
        }
        sprintf(body, "Error trace (%s: %s)", types[e->type], tmp);
    }

    printCentered(4, 80, body);

    /* display error stack */
    for (i = 0; i < errors.level; i++) {
        serverLog(LL_WARNING, "0x%08lx - %s",
            (unsigned long) errors.offset[i], errors.error[i]);
    }
}

void process(void) {
    uint64_t num_errors = 0, num_valid_ops = 0, num_valid_bytes = 0;
    entry entry = { NULL, -1, 0 };
    int dump_version = processHeader();

    /* Exclude the final checksum for RDB >= 5. Will be checked at the end. */
    if (dump_version >= 5) {
        if (positions[0].size < 8) {
            serverLog(LL_WARNING, "RDB version >= 5 but no room for checksum.");
            exit(1);
        }
        positions[0].size -= 8;
    }

    level = 1;
    while(positions[0].offset < positions[0].size) {
        positions[1] = positions[0];

        entry = loadEntry();
        if (!entry.success) {
            printValid(num_valid_ops, num_valid_bytes);
            printErrorStack(&entry);
            num_errors++;
            num_valid_ops = 0;
            num_valid_bytes = 0;

            /* search for next valid entry */
            uint64_t offset = positions[0].offset + 1;
            int i = 0;

            while (!entry.success && offset < positions[0].size) {
                positions[1].offset = offset;

                /* find 3 consecutive valid entries */
                for (i = 0; i < 3; i++) {
                    entry = loadEntry();
                    if (!entry.success) break;
                }
                /* check if we found 3 consecutive valid entries */
                if (i < 3) {
                    offset++;
                }
            }

            /* print how many bytes we have skipped to find a new valid opcode */
            if (offset < positions[0].size) {
                printSkipped(offset - positions[0].offset, offset);
            }

            positions[0].offset = offset;
        } else {
            num_valid_ops++;
            num_valid_bytes += positions[1].offset - positions[0].offset;

            /* advance position */
            positions[0] = positions[1];
        }
        zfree(entry.key);
    }

    /* because there is another potential error,
     * print how many valid ops we have processed */
    printValid(num_valid_ops, num_valid_bytes);

    /* expect an eof */
    if (entry.type != RDB_OPCODE_EOF) {
        /* last byte should be EOF, add error */
        errors.level = 0;
        SHIFT_ERROR(positions[0].offset, "Expected EOF, got %s", types[entry.type]);

        /* this is an EOF error so reset type */
        entry.type = -1;
        printErrorStack(&entry);

        num_errors++;
    }

    /* Verify checksum */
    if (dump_version >= 5) {
        uint64_t crc = crc64(0,positions[0].data,positions[0].size);
        uint64_t crc2;
        unsigned char *p = (unsigned char*)positions[0].data+positions[0].size;
        crc2 = ((uint64_t)p[0] << 0) |
               ((uint64_t)p[1] << 8) |
               ((uint64_t)p[2] << 16) |
               ((uint64_t)p[3] << 24) |
               ((uint64_t)p[4] << 32) |
               ((uint64_t)p[5] << 40) |
               ((uint64_t)p[6] << 48) |
               ((uint64_t)p[7] << 56);
        if (crc != crc2) {
            SHIFT_ERROR(positions[0].offset, "RDB CRC64 does not match.");
        } else {
            serverLog(LL_WARNING, "CRC64 checksum is OK");
        }
    }

    /* print summary on errors */
    if (num_errors) {
        serverLog(LL_WARNING, "Total unprocessable opcodes: %llu",
            (unsigned long long) num_errors);
    }
}

int redis_check_rdb(char *rdbfilename) {
    int fd;
    off_t size;
    struct stat stat;
    void *data;

    fd = open(rdbfilename, O_RDONLY);
    if (fd < 1) {
        ERROR("Cannot open file: %s", rdbfilename);
    }
    if (fstat(fd, &stat) == -1) {
        ERROR("Cannot stat: %s", rdbfilename);
    } else {
        size = stat.st_size;
    }

    if (sizeof(size_t) == sizeof(int32_t) && size >= INT_MAX) {
        ERROR("Cannot check dump files >2GB on a 32-bit platform");
    }

    data = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        ERROR("Cannot mmap: %s", rdbfilename);
    }

    /* Initialize static vars */
    positions[0].data = data;
    positions[0].size = size;
    positions[0].offset = 0;
    errors.level = 0;

    /* Object types */
    sprintf(types[RDB_TYPE_STRING], "STRING");
    sprintf(types[RDB_TYPE_LIST], "LIST");
    sprintf(types[RDB_TYPE_SET], "SET");
    sprintf(types[RDB_TYPE_ZSET], "ZSET");
    sprintf(types[RDB_TYPE_HASH], "HASH");

    /* Object types only used for dumping to disk */
    sprintf(types[RDB_OPCODE_EXPIRETIME], "EXPIRETIME");
    sprintf(types[RDB_OPCODE_SELECTDB], "SELECTDB");
    sprintf(types[RDB_OPCODE_EOF], "EOF");

    process();

    munmap(data, size);
    close(fd);
    return 0;
}

/* RDB check main: called form redis.c when Redis is executed with the
 * redis-check-rdb alias. */
int redis_check_rdb_main(char **argv, int argc) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <rdb-file-name>\n", argv[0]);
        exit(1);
    }
    serverLog(LL_WARNING, "Checking RDB file %s", argv[1]);
    exit(redis_check_rdb(argv[1]));
    return 0;
}

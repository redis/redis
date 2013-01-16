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


#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <limits.h>
#include "lzf.h"
#include "crc64.h"

/* Object types */
#define REDIS_STRING 0
#define REDIS_LIST 1
#define REDIS_SET 2
#define REDIS_ZSET 3
#define REDIS_HASH 4
#define REDIS_HASH_ZIPMAP 9
#define REDIS_LIST_ZIPLIST 10
#define REDIS_SET_INTSET 11
#define REDIS_ZSET_ZIPLIST 12
#define REDIS_HASH_ZIPLIST 13

/* Objects encoding. Some kind of objects like Strings and Hashes can be
 * internally represented in multiple ways. The 'encoding' field of the object
 * is set to one of this fields for this object. */
#define REDIS_ENCODING_RAW 0    /* Raw representation */
#define REDIS_ENCODING_INT 1    /* Encoded as integer */
#define REDIS_ENCODING_ZIPMAP 2 /* Encoded as zipmap */
#define REDIS_ENCODING_HT 3     /* Encoded as an hash table */

/* Object types only used for dumping to disk */
#define REDIS_EXPIRETIME_MS 252
#define REDIS_EXPIRETIME 253
#define REDIS_SELECTDB 254
#define REDIS_EOF 255

/* Defines related to the dump file format. To store 32 bits lengths for short
 * keys requires a lot of space, so we check the most significant 2 bits of
 * the first byte to interpreter the length:
 *
 * 00|000000 => if the two MSB are 00 the len is the 6 bits of this byte
 * 01|000000 00000000 =>  01, the len is 14 byes, 6 bits + 8 bits of next byte
 * 10|000000 [32 bit integer] => if it's 01, a full 32 bit len will follow
 * 11|000000 this means: specially encoded object will follow. The six bits
 *           number specify the kind of object that follows.
 *           See the REDIS_RDB_ENC_* defines.
 *
 * Lengths up to 63 are stored using a single byte, most DB keys, and may
 * values, will fit inside. */
#define REDIS_RDB_6BITLEN 0
#define REDIS_RDB_14BITLEN 1
#define REDIS_RDB_32BITLEN 2
#define REDIS_RDB_ENCVAL 3
#define REDIS_RDB_LENERR UINT_MAX

/* When a length of a string object stored on disk has the first two bits
 * set, the remaining two bits specify a special encoding for the object
 * accordingly to the following defines: */
#define REDIS_RDB_ENC_INT8 0        /* 8 bit signed integer */
#define REDIS_RDB_ENC_INT16 1       /* 16 bit signed integer */
#define REDIS_RDB_ENC_INT32 2       /* 32 bit signed integer */
#define REDIS_RDB_ENC_LZF 3         /* string compressed with FASTLZ */

#define ERROR(...) { \
    printf(__VA_ARGS__); \
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

/* Global vars that are actually used as constants. The following double
 * values are used for double on-disk serialization, and are initialized
 * at runtime to avoid strange compiler optimizations. */
static double R_Zero, R_PosInf, R_NegInf, R_Nan;

/* store string types for output */
static char types[256][16];

/* Return true if 't' is a valid object type. */
int checkType(unsigned char t) {
    /* In case a new object type is added, update the following 
     * condition as necessary. */
    return
        (t >= REDIS_HASH_ZIPMAP && t <= REDIS_HASH_ZIPLIST) ||
        t <= REDIS_HASH ||
        t >= REDIS_EXPIRETIME_MS;
}

/* when number of bytes to read is negative, do a peek */
int readBytes(void *target, long num) {
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

int processHeader() {
    char buf[10] = "_________";
    int dump_version;

    if (!readBytes(buf, 9)) {
        ERROR("Cannot read header\n");
    }

    /* expect the first 5 bytes to equal REDIS */
    if (memcmp(buf,"REDIS",5) != 0) {
        ERROR("Wrong signature in header\n");
    }

    dump_version = (int)strtol(buf + 5, NULL, 10);
    if (dump_version < 1 || dump_version > 6) {
        ERROR("Unknown RDB format version: %d\n", dump_version);
    }
    return dump_version;
}

int loadType(entry *e) {
    uint32_t offset = CURR_OFFSET;

    /* this byte needs to qualify as type */
    unsigned char t;
    if (readBytes(&t, 1)) {
        if (checkType(t)) {
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

int peekType() {
    unsigned char t;
    if (readBytes(&t, -1) && (checkType(t)))
        return t;
    return -1;
}

/* discard time, just consume the bytes */
int processTime(int type) {
    uint32_t offset = CURR_OFFSET;
    unsigned char t[8];
    int timelen = (type == REDIS_EXPIRETIME_MS) ? 8 : 4;

    if (readBytes(t,timelen)) {
        return 1;
    } else {
        SHIFT_ERROR(offset, "Could not read time");
    }

    /* failure */
    return 0;
}

uint32_t loadLength(int *isencoded) {
    unsigned char buf[2];
    uint32_t len;
    int type;

    if (isencoded) *isencoded = 0;
    if (!readBytes(buf, 1)) return REDIS_RDB_LENERR;
    type = (buf[0] & 0xC0) >> 6;
    if (type == REDIS_RDB_6BITLEN) {
        /* Read a 6 bit len */
        return buf[0] & 0x3F;
    } else if (type == REDIS_RDB_ENCVAL) {
        /* Read a 6 bit len encoding type */
        if (isencoded) *isencoded = 1;
        return buf[0] & 0x3F;
    } else if (type == REDIS_RDB_14BITLEN) {
        /* Read a 14 bit len */
        if (!readBytes(buf+1,1)) return REDIS_RDB_LENERR;
        return ((buf[0] & 0x3F) << 8) | buf[1];
    } else {
        /* Read a 32 bit len */
        if (!readBytes(&len, 4)) return REDIS_RDB_LENERR;
        return (unsigned int)ntohl(len);
    }
}

char *loadIntegerObject(int enctype) {
    uint32_t offset = CURR_OFFSET;
    unsigned char enc[4];
    long long val;

    if (enctype == REDIS_RDB_ENC_INT8) {
        uint8_t v;
        if (!readBytes(enc, 1)) return NULL;
        v = enc[0];
        val = (int8_t)v;
    } else if (enctype == REDIS_RDB_ENC_INT16) {
        uint16_t v;
        if (!readBytes(enc, 2)) return NULL;
        v = enc[0]|(enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == REDIS_RDB_ENC_INT32) {
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
    buf = malloc(sizeof(char) * 128);
    sprintf(buf, "%lld", val);
    return buf;
}

char* loadLzfStringObject() {
    unsigned int slen, clen;
    char *c, *s;

    if ((clen = loadLength(NULL)) == REDIS_RDB_LENERR) return NULL;
    if ((slen = loadLength(NULL)) == REDIS_RDB_LENERR) return NULL;

    c = malloc(clen);
    if (!readBytes(c, clen)) {
        free(c);
        return NULL;
    }

    s = malloc(slen+1);
    if (lzf_decompress(c,clen,s,slen) == 0) {
        free(c); free(s);
        return NULL;
    }

    free(c);
    return s;
}

/* returns NULL when not processable, char* when valid */
char* loadStringObject() {
    uint32_t offset = CURR_OFFSET;
    int isencoded;
    uint32_t len;

    len = loadLength(&isencoded);
    if (isencoded) {
        switch(len) {
        case REDIS_RDB_ENC_INT8:
        case REDIS_RDB_ENC_INT16:
        case REDIS_RDB_ENC_INT32:
            return loadIntegerObject(len);
        case REDIS_RDB_ENC_LZF:
            return loadLzfStringObject();
        default:
            /* unknown encoding */
            SHIFT_ERROR(offset, "Unknown string encoding (0x%02x)", len);
            return NULL;
        }
    }

    if (len == REDIS_RDB_LENERR) return NULL;

    char *buf = malloc(sizeof(char) * (len+1));
    buf[len] = '\0';
    if (!readBytes(buf, len)) {
        free(buf);
        return NULL;
    }
    return buf;
}

int processStringObject(char** store) {
    unsigned long offset = CURR_OFFSET;
    char *key = loadStringObject();
    if (key == NULL) {
        SHIFT_ERROR(offset, "Error reading string object");
        free(key);
        return 0;
    }

    if (store != NULL) {
        *store = key;
    } else {
        free(key);
    }
    return 1;
}

double* loadDoubleValue() {
    char buf[256];
    unsigned char len;
    double* val;

    if (!readBytes(&len,1)) return NULL;

    val = malloc(sizeof(double));
    switch(len) {
    case 255: *val = R_NegInf;  return val;
    case 254: *val = R_PosInf;  return val;
    case 253: *val = R_Nan;     return val;
    default:
        if (!readBytes(buf, len)) {
            free(val);
            return NULL;
        }
        buf[len] = '\0';
        sscanf(buf, "%lg", val);
        return val;
    }
}

int processDoubleValue(double** store) {
    unsigned long offset = CURR_OFFSET;
    double *val = loadDoubleValue();
    if (val == NULL) {
        SHIFT_ERROR(offset, "Error reading double value");
        free(val);
        return 0;
    }

    if (store != NULL) {
        *store = val;
    } else {
        free(val);
    }
    return 1;
}

int loadPair(entry *e) {
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
    if (e->type == REDIS_LIST ||
        e->type == REDIS_SET  ||
        e->type == REDIS_ZSET ||
        e->type == REDIS_HASH) {
        if ((length = loadLength(NULL)) == REDIS_RDB_LENERR) {
            SHIFT_ERROR(offset, "Error reading %s length", types[e->type]);
            return 0;
        }
    }

    switch(e->type) {
    case REDIS_STRING:
    case REDIS_HASH_ZIPMAP:
    case REDIS_LIST_ZIPLIST:
    case REDIS_SET_INTSET:
    case REDIS_ZSET_ZIPLIST:
    case REDIS_HASH_ZIPLIST:
        if (!processStringObject(NULL)) {
            SHIFT_ERROR(offset, "Error reading entry value");
            return 0;
        }
    break;
    case REDIS_LIST:
    case REDIS_SET:
        for (i = 0; i < length; i++) {
            offset = CURR_OFFSET;
            if (!processStringObject(NULL)) {
                SHIFT_ERROR(offset, "Error reading element at index %d (length: %d)", i, length);
                return 0;
            }
        }
    break;
    case REDIS_ZSET:
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
    case REDIS_HASH:
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

entry loadEntry() {
    entry e = { NULL, -1, 0 };
    uint32_t length, offset[4];

    /* reset error container */
    errors.level = 0;

    offset[0] = CURR_OFFSET;
    if (!loadType(&e)) {
        return e;
    }

    offset[1] = CURR_OFFSET;
    if (e.type == REDIS_SELECTDB) {
        if ((length = loadLength(NULL)) == REDIS_RDB_LENERR) {
            SHIFT_ERROR(offset[1], "Error reading database number");
            return e;
        }
        if (length > 63) {
            SHIFT_ERROR(offset[1], "Database number out of range (%d)", length);
            return e;
        }
    } else if (e.type == REDIS_EOF) {
        if (positions[level].offset < positions[level].size) {
            SHIFT_ERROR(offset[0], "Unexpected EOF");
        } else {
            e.success = 1;
        }
        return e;
    } else {
        /* optionally consume expire */
        if (e.type == REDIS_EXPIRETIME || 
            e.type == REDIS_EXPIRETIME_MS) {
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

void printCentered(int indent, int width, char* body) {
    char head[256], tail[256];
    memset(head, '\0', 256);
    memset(tail, '\0', 256);

    memset(head, '=', indent);
    memset(tail, '=', width - 2 - indent - strlen(body));
    printf("%s %s %s\n", head, body, tail);
}

void printValid(uint64_t ops, uint64_t bytes) {
    char body[80];
    sprintf(body, "Processed %llu valid opcodes (in %llu bytes)",
        (unsigned long long) ops, (unsigned long long) bytes);
    printCentered(4, 80, body);
}

void printSkipped(uint64_t bytes, uint64_t offset) {
    char body[80];
    sprintf(body, "Skipped %llu bytes (resuming at 0x%08llx)",
        (unsigned long long) bytes, (unsigned long long) offset);
    printCentered(4, 80, body);
}

void printErrorStack(entry *e) {
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
        printf("0x%08lx - %s\n",
            (unsigned long) errors.offset[i], errors.error[i]);
    }
}

void process() {
    uint64_t num_errors = 0, num_valid_ops = 0, num_valid_bytes = 0;
    entry entry;
    int dump_version = processHeader();

    /* Exclude the final checksum for RDB >= 5. Will be checked at the end. */
    if (dump_version >= 5) {
        if (positions[0].size < 8) {
            printf("RDB version >= 5 but no room for checksum.\n");
            exit(1);
        }
        positions[0].size -= 8;;
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
        free(entry.key);
    }

    /* because there is another potential error,
     * print how many valid ops we have processed */
    printValid(num_valid_ops, num_valid_bytes);

    /* expect an eof */
    if (entry.type != REDIS_EOF) {
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
            printf("CRC64 checksum is OK\n");
        }
    }

    /* print summary on errors */
    if (num_errors) {
        printf("\n");
        printf("Total unprocessable opcodes: %llu\n",
            (unsigned long long) num_errors);
    }
}

int main(int argc, char **argv) {
    /* expect the first argument to be the dump file */
    if (argc <= 1) {
        printf("Usage: %s <dump.rdb>\n", argv[0]);
        exit(0);
    }

    int fd;
    off_t size;
    struct stat stat;
    void *data;

    fd = open(argv[1], O_RDONLY);
    if (fd < 1) {
        ERROR("Cannot open file: %s\n", argv[1]);
    }
    if (fstat(fd, &stat) == -1) {
        ERROR("Cannot stat: %s\n", argv[1]);
    } else {
        size = stat.st_size;
    }

    if (sizeof(size_t) == sizeof(int32_t) && size >= INT_MAX) {
        ERROR("Cannot check dump files >2GB on a 32-bit platform\n");
    }

    data = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        ERROR("Cannot mmap: %s\n", argv[1]);
    }

    /* Initialize static vars */
    positions[0].data = data;
    positions[0].size = size;
    positions[0].offset = 0;
    errors.level = 0;

    /* Object types */
    sprintf(types[REDIS_STRING], "STRING");
    sprintf(types[REDIS_LIST], "LIST");
    sprintf(types[REDIS_SET], "SET");
    sprintf(types[REDIS_ZSET], "ZSET");
    sprintf(types[REDIS_HASH], "HASH");

    /* Object types only used for dumping to disk */
    sprintf(types[REDIS_EXPIRETIME], "EXPIRETIME");
    sprintf(types[REDIS_SELECTDB], "SELECTDB");
    sprintf(types[REDIS_EOF], "EOF");

    /* Double constants initialization */
    R_Zero = 0.0;
    R_PosInf = 1.0/R_Zero;
    R_NegInf = -1.0/R_Zero;
    R_Nan = R_Zero/R_Zero;

    process();

    munmap(data, size);
    close(fd);
    return 0;
}

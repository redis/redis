/*
 * Copyright (c) 2016, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "mt19937-64.h"
#include "server.h"
#include "rdb.h"

#include <stdarg.h>
#include <sys/time.h>
#include <unistd.h>

void createSharedObjects(void);
void rdbLoadProgressCallback(rio *r, const void *buf, size_t len);
int rdbCheckMode = 0;

struct {
    rio *rio;
    robj *key;                      /* Current key we are reading. */
    int key_type;                   /* Current key type if != -1. */
    unsigned long keys;             /* Number of keys processed. */
    unsigned long expires;          /* Number of keys with an expire. */
    unsigned long already_expired;  /* Number of keys already expired. */
    int doing;                      /* The state while reading the RDB. */
    int error_set;                  /* True if error is populated. */
    char error[1024];
} rdbstate;

/* At every loading step try to remember what we were about to do, so that
 * we can log this information when an error is encountered. */
#define RDB_CHECK_DOING_START 0
#define RDB_CHECK_DOING_READ_TYPE 1
#define RDB_CHECK_DOING_READ_EXPIRE 2
#define RDB_CHECK_DOING_READ_KEY 3
#define RDB_CHECK_DOING_READ_OBJECT_VALUE 4
#define RDB_CHECK_DOING_CHECK_SUM 5
#define RDB_CHECK_DOING_READ_LEN 6
#define RDB_CHECK_DOING_READ_AUX 7
#define RDB_CHECK_DOING_READ_MODULE_AUX 8

char *rdb_check_doing_string[] = {
    "start",
    "read-type",
    "read-expire",
    "read-key",
    "read-object-value",
    "check-sum",
    "read-len",
    "read-aux",
    "read-module-aux"
};

char *rdb_type_string[] = {
    "string",
    "list-linked",
    "set-hashtable",
    "zset-v1",
    "hash-hashtable",
    "zset-v2",
    "module-value",
    "","",
    "hash-zipmap",
    "list-ziplist",
    "set-intset",
    "zset-ziplist",
    "hash-ziplist",
    "quicklist",
    "stream"
};

/* Show a few stats collected into 'rdbstate' */
void rdbShowGenericInfo(void) {
    printf("[info] %lu keys read\n", rdbstate.keys);
    printf("[info] %lu expires\n", rdbstate.expires);
    printf("[info] %lu already expired\n", rdbstate.already_expired);
}

/* Called on RDB errors. Provides details about the RDB and the offset
 * we were when the error was detected. */
void rdbCheckError(const char *fmt, ...) {
    char msg[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    printf("--- RDB ERROR DETECTED ---\n");
    printf("[offset %llu] %s\n",
        (unsigned long long) (rdbstate.rio ?
            rdbstate.rio->processed_bytes : 0), msg);
    printf("[additional info] While doing: %s\n",
        rdb_check_doing_string[rdbstate.doing]);
    if (rdbstate.key)
        printf("[additional info] Reading key '%s'\n",
            (char*)rdbstate.key->ptr);
    if (rdbstate.key_type != -1)
        printf("[additional info] Reading type %d (%s)\n",
            rdbstate.key_type,
            ((unsigned)rdbstate.key_type <
             sizeof(rdb_type_string)/sizeof(char*)) ?
                rdb_type_string[rdbstate.key_type] : "unknown");
    rdbShowGenericInfo();
}

/* Print informations during RDB checking. */
void rdbCheckInfo(const char *fmt, ...) {
    char msg[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    printf("[offset %llu] %s\n",
        (unsigned long long) (rdbstate.rio ?
            rdbstate.rio->processed_bytes : 0), msg);
}

/* Used inside rdb.c in order to log specific errors happening inside
 * the RDB loading internals. */
void rdbCheckSetError(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(rdbstate.error, sizeof(rdbstate.error), fmt, ap);
    va_end(ap);
    rdbstate.error_set = 1;
}

/* During RDB check we setup a special signal handler for memory violations
 * and similar conditions, so that we can log the offending part of the RDB
 * if the crash is due to broken content. */
void rdbCheckHandleCrash(int sig, siginfo_t *info, void *secret) {
    UNUSED(sig);
    UNUSED(info);
    UNUSED(secret);

    rdbCheckError("Server crash checking the specified RDB file!");
    exit(1);
}

void rdbCheckSetupSignals(void) {
    struct sigaction act;

    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESETHAND | SA_SIGINFO;
    act.sa_sigaction = rdbCheckHandleCrash;
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGFPE, &act, NULL);
    sigaction(SIGILL, &act, NULL);
    sigaction(SIGABRT, &act, NULL);
}

/* Check the specified RDB file. Return 0 if the RDB looks sane, otherwise
 * 1 is returned.
 * The file is specified as a filename in 'rdbfilename' if 'fp' is not NULL,
 * otherwise the already open file 'fp' is checked. */
int redis_check_rdb(char *rdbfilename, FILE *fp) {
    uint64_t dbid;
    int type, rdbver;
    char buf[1024];
    long long expiretime, now = mstime();
    static rio rdb; /* Pointed by global struct riostate. */

    int closefile = (fp == NULL);
    if (fp == NULL && (fp = fopen(rdbfilename,"r")) == NULL) return 1;

    startLoadingFile(fp, rdbfilename, RDBFLAGS_NONE);
    rioInitWithFile(&rdb,fp);
    rdbstate.rio = &rdb;
    rdb.update_cksum = rdbLoadProgressCallback;
    if (rioRead(&rdb,buf,9) == 0) goto eoferr;
    buf[9] = '\0';
    if (memcmp(buf,"REDIS",5) != 0) {
        rdbCheckError("Wrong signature trying to load DB from file");
        goto err;
    }
    rdbver = atoi(buf+5);
    if (rdbver < 1 || rdbver > RDB_VERSION) {
        rdbCheckError("Can't handle RDB format version %d",rdbver);
        goto err;
    }

    expiretime = -1;
    while(1) {
        robj *key, *val;

        /* Read type. */
        rdbstate.doing = RDB_CHECK_DOING_READ_TYPE;
        if ((type = rdbLoadType(&rdb)) == -1) goto eoferr;

        /* Handle special types. */
        if (type == RDB_OPCODE_EXPIRETIME) {
            rdbstate.doing = RDB_CHECK_DOING_READ_EXPIRE;
            /* EXPIRETIME: load an expire associated with the next key
             * to load. Note that after loading an expire we need to
             * load the actual type, and continue. */
            expiretime = rdbLoadTime(&rdb);
            expiretime *= 1000;
            if (rioGetReadError(&rdb)) goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EXPIRETIME_MS) {
            /* EXPIRETIME_MS: milliseconds precision expire times introduced
             * with RDB v3. Like EXPIRETIME but no with more precision. */
            rdbstate.doing = RDB_CHECK_DOING_READ_EXPIRE;
            expiretime = rdbLoadMillisecondTime(&rdb, rdbver);
            if (rioGetReadError(&rdb)) goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_FREQ) {
            /* FREQ: LFU frequency. */
            uint8_t byte;
            if (rioRead(&rdb,&byte,1) == 0) goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_IDLE) {
            /* IDLE: LRU idle time. */
            if (rdbLoadLen(&rdb,NULL) == RDB_LENERR) goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EOF) {
            /* EOF: End of file, exit the main loop. */
            break;
        } else if (type == RDB_OPCODE_SELECTDB) {
            /* SELECTDB: Select the specified database. */
            rdbstate.doing = RDB_CHECK_DOING_READ_LEN;
            if ((dbid = rdbLoadLen(&rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            rdbCheckInfo("Selecting DB ID %llu", (unsigned long long)dbid);
            continue; /* Read type again. */
        } else if (type == RDB_OPCODE_RESIZEDB) {
            /* RESIZEDB: Hint about the size of the keys in the currently
             * selected data base, in order to avoid useless rehashing. */
            uint64_t db_size, expires_size;
            rdbstate.doing = RDB_CHECK_DOING_READ_LEN;
            if ((db_size = rdbLoadLen(&rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            if ((expires_size = rdbLoadLen(&rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            continue; /* Read type again. */
        } else if (type == RDB_OPCODE_AUX) {
            /* AUX: generic string-string fields. Use to add state to RDB
             * which is backward compatible. Implementations of RDB loading
             * are requierd to skip AUX fields they don't understand.
             *
             * An AUX field is composed of two strings: key and value. */
            robj *auxkey, *auxval;
            rdbstate.doing = RDB_CHECK_DOING_READ_AUX;
            if ((auxkey = rdbLoadStringObject(&rdb)) == NULL) goto eoferr;
            if ((auxval = rdbLoadStringObject(&rdb)) == NULL) goto eoferr;

            rdbCheckInfo("AUX FIELD %s = '%s'",
                (char*)auxkey->ptr, (char*)auxval->ptr);
            decrRefCount(auxkey);
            decrRefCount(auxval);
            continue; /* Read type again. */
        } else if (type == RDB_OPCODE_MODULE_AUX) {
            /* AUX: Auxiliary data for modules. */
            uint64_t moduleid, when_opcode, when;
            rdbstate.doing = RDB_CHECK_DOING_READ_MODULE_AUX;
            if ((moduleid = rdbLoadLen(&rdb,NULL)) == RDB_LENERR) goto eoferr;
            if ((when_opcode = rdbLoadLen(&rdb,NULL)) == RDB_LENERR) goto eoferr;
            if ((when = rdbLoadLen(&rdb,NULL)) == RDB_LENERR) goto eoferr;

            char name[10];
            moduleTypeNameByID(name,moduleid);
            rdbCheckInfo("MODULE AUX for: %s", name);

            robj *o = rdbLoadCheckModuleValue(&rdb,name);
            decrRefCount(o);
            continue; /* Read type again. */
        } else {
            if (!rdbIsObjectType(type)) {
                rdbCheckError("Invalid object type: %d", type);
                goto err;
            }
            rdbstate.key_type = type;
        }

        /* Read key */
        rdbstate.doing = RDB_CHECK_DOING_READ_KEY;
        if ((key = rdbLoadStringObject(&rdb)) == NULL) goto eoferr;
        rdbstate.key = key;
        rdbstate.keys++;
        /* Read value */
        rdbstate.doing = RDB_CHECK_DOING_READ_OBJECT_VALUE;
        if ((val = rdbLoadObject(type,&rdb,key->ptr,NULL)) == NULL) goto eoferr;
        /* Check if the key already expired. */
        if (expiretime != -1 && expiretime < now)
            rdbstate.already_expired++;
        if (expiretime != -1) rdbstate.expires++;
        rdbstate.key = NULL;
        decrRefCount(key);
        decrRefCount(val);
        rdbstate.key_type = -1;
        expiretime = -1;
    }
    /* Verify the checksum if RDB version is >= 5 */
    if (rdbver >= 5 && server.rdb_checksum) {
        uint64_t cksum, expected = rdb.cksum;

        rdbstate.doing = RDB_CHECK_DOING_CHECK_SUM;
        if (rioRead(&rdb,&cksum,8) == 0) goto eoferr;
        memrev64ifbe(&cksum);
        if (cksum == 0) {
            rdbCheckInfo("RDB file was saved with checksum disabled: no check performed.");
        } else if (cksum != expected) {
            rdbCheckError("RDB CRC error");
            goto err;
        } else {
            rdbCheckInfo("Checksum OK");
        }
    }

    if (closefile) fclose(fp);
    stopLoading(1);
    return 0;

eoferr: /* unexpected end of file is handled here with a fatal exit */
    if (rdbstate.error_set) {
        rdbCheckError(rdbstate.error);
    } else {
        rdbCheckError("Unexpected EOF reading RDB file");
    }
err:
    if (closefile) fclose(fp);
    stopLoading(0);
    return 1;
}

/* RDB check main: called form server.c when Redis is executed with the
 * redis-check-rdb alias, on during RDB loading errors.
 *
 * The function works in two ways: can be called with argc/argv as a
 * standalone executable, or called with a non NULL 'fp' argument if we
 * already have an open file to check. This happens when the function
 * is used to check an RDB preamble inside an AOF file.
 *
 * When called with fp = NULL, the function never returns, but exits with the
 * status code according to success (RDB is sane) or error (RDB is corrupted).
 * Otherwise if called with a non NULL fp, the function returns C_OK or
 * C_ERR depending on the success or failure. */
int redis_check_rdb_main(int argc, char **argv, FILE *fp) {
    struct timeval tv;

    if (argc != 2 && fp == NULL) {
        fprintf(stderr, "Usage: %s <rdb-file-name>\n", argv[0]);
        exit(1);
    }

    gettimeofday(&tv, NULL);
    init_genrand64(((long long) tv.tv_sec * 1000000 + tv.tv_usec) ^ getpid());

    /* In order to call the loading functions we need to create the shared
     * integer objects, however since this function may be called from
     * an already initialized Redis instance, check if we really need to. */
    if (shared.integers[0] == NULL)
        createSharedObjects();
    server.loading_process_events_interval_bytes = 0;
    server.sanitize_dump_payload = SANITIZE_DUMP_YES;
    rdbCheckMode = 1;
    rdbCheckInfo("Checking RDB file %s", argv[1]);
    rdbCheckSetupSignals();
    int retval = redis_check_rdb(argv[1],fp);
    if (retval == 0) {
        rdbCheckInfo("\\o/ RDB looks OK! \\o/");
        rdbShowGenericInfo();
    }
    if (fp) return (retval == 0) ? C_OK : C_ERR;
    exit(retval);
}

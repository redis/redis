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
#include <sys/stat.h>
#include <sys/types.h>
#include <regex.h>
#include <libgen.h>

#define AOF_CHECK_OK 0
#define AOF_CHECK_EMPTY 1
#define AOF_CHECK_TRUNCATED 2
#define AOF_CHECK_TIMESTAMP_TRUNCATED 3

typedef enum {
    AOF_RESP,
    AOF_RDB_PREAMBLE,
    AOF_MULTI_PART,
} input_file_type;

aofManifest *aofManifestCreate(void);
void aofManifestFree(aofManifest *am);
aofManifest *aofLoadManifestFromFile(sds am_filepath);

#define ERROR(...) { \
    char __buf[1024]; \
    snprintf(__buf, sizeof(__buf), __VA_ARGS__); \
    snprintf(error, sizeof(error), "0x%16llx: %s", (long long)epos, __buf); \
}

static char error[1044];
static off_t epos;
static long long line = 1;
static time_t to_timestamp = 0;

int consumeNewline(char *buf) {
    if (strncmp(buf,"\r\n",2) != 0) {
        ERROR("Expected \\r\\n, got: %02x%02x",buf[0],buf[1]);
        return 0;
    }
    line += 1;
    return 1;
}

int readLong(FILE *fp, char prefix, long *target) {
    char buf[128], *eptr;
    epos = ftello(fp);
    if (fgets(buf,sizeof(buf),fp) == NULL) {
        return 0;
    }
    if (buf[0] != prefix) {
        ERROR("Expected prefix '%c', got: '%c'",prefix,buf[0]);
        return 0;
    }
    *target = strtol(buf+1,&eptr,10);
    return consumeNewline(eptr);
}

int readBytes(FILE *fp, char *target, long length) {
    long real;
    epos = ftello(fp);
    real = fread(target,1,length,fp);
    if (real != length) {
        ERROR("Expected to read %ld bytes, got %ld bytes",length,real);
        return 0;
    }
    return 1;
}

int readString(FILE *fp, char** target) {
    long len;
    *target = NULL;
    if (!readLong(fp,'$',&len)) {
        return 0;
    }

    if (len < 0 || len > LONG_MAX - 2) {
        ERROR("Expected to read string of %ld bytes, which is not in the suitable range",len);
        return 0;
    }

    /* Increase length to also consume \r\n */
    len += 2;
    *target = (char*)zmalloc(len);
    if (!readBytes(fp,*target,len)) {
        zfree(*target);
        *target = NULL;
        return 0;
    }
    if (!consumeNewline(*target+len-2)) {
        zfree(*target);
        *target = NULL;
        return 0;
    }
    (*target)[len-2] = '\0';
    return 1;
}

int readArgc(FILE *fp, long *target) {
    return readLong(fp,'*',target);
}

/* Used to decode a RESP record in the AOF file to obtain the original 
 * redis command, and also check whether the command is MULTI/EXEC. If the 
 * command is MULTI, the parameter out_multi will be incremented by one, and 
 * if the command is EXEC, the parameter out_multi will be decremented 
 * by one. The parameter out_multi will be used by the upper caller to determine 
 * whether the AOF file contains unclosed transactions.
 **/
int processRESP(FILE *fp, char *filename, int *out_multi) {
    long argc;
    char *str;

    if (!readArgc(fp, &argc)) return 0;

    for (int i = 0; i < argc; i++) {
        if (!readString(fp, &str)) return 0;
        if (i == 0) {
            if (strcasecmp(str, "multi") == 0) {
                if ((*out_multi)++) {
                    ERROR("Unexpected MULTI in AOF %s", filename);
                    zfree(str);
                    return 0;
                }
            } else if (strcasecmp(str, "exec") == 0) {
                if (--(*out_multi)) {
                    ERROR("Unexpected EXEC in AOF %s", filename);
                    zfree(str);
                    return 0;
                }
            }
        }
        zfree(str);
    }

    return 1;
}

/* Used to parse an annotation in the AOF file, the annotation starts with '#' 
 * in AOF. Currently AOF only contains timestamp annotations, but this function 
 * can easily be extended to handle other annotations. 
 * 
 * The processing rule of time annotation is that once the timestamp is found to
 * be greater than 'to_timestamp', the AOF after the annotation is truncated. 
 * Note that in Multi Part AOF, this truncation is only allowed when the last_file 
 * parameter is 1.
 **/
int processAnnotations(FILE *fp, char *filename, int last_file) {
    char buf[AOF_ANNOTATION_LINE_MAX_LEN];

    epos = ftello(fp);
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        printf("Failed to read annotations from AOF %s, aborting...\n", filename);
        exit(1);
    }

    if (to_timestamp && strncmp(buf, "#TS:", 4) == 0) {
        char *endptr;
        errno = 0;
        time_t ts = strtol(buf+4, &endptr, 10);
        if (errno != 0 || *endptr != '\r') {
            printf("Invalid timestamp annotation\n");
            exit(1);
        }
        if (ts <= to_timestamp) return 1;
        if (epos == 0) {
            printf("AOF %s has nothing before timestamp %ld, "
                    "aborting...\n", filename, to_timestamp);
            exit(1);
        }
        if (!last_file) {
            printf("Failed to truncate AOF %s to timestamp %ld to offset %ld because it is not the last file.\n",
                filename, to_timestamp, (long int)epos);
            printf("If you insist, please delete all files after this file according to the manifest "
                "file and delete the corresponding records in manifest file manually. Then re-run redis-check-aof.\n");
            exit(1);
        }
        /* Truncate remaining AOF if exceeding 'to_timestamp' */
        if (ftruncate(fileno(fp), epos) == -1) {
            printf("Failed to truncate AOF %s to timestamp %ld\n",
                    filename, to_timestamp);
            exit(1);
        } else {
            return 0;
        }
    }
    return 1;
}

/* Used to check the validity of a single AOF file. The AOF file can be:
 * 1. Old-style AOF
 * 2. Old-style RDB-preamble AOF
 * 3. BASE or INCR in Multi Part AOF 
 * */
int checkSingleAof(char *aof_filename, char *aof_filepath, int last_file, int fix, int preamble) {
    off_t pos = 0, diff;
    int multi = 0;
    char buf[2];

    FILE *fp = fopen(aof_filepath, "r+");
    if (fp == NULL) {
        printf("Cannot open file %s: %s, aborting...\n", aof_filepath, strerror(errno));
        exit(1);
    }

    struct redis_stat sb;
    if (redis_fstat(fileno(fp),&sb) == -1) {
        printf("Cannot stat file: %s, aborting...\n", aof_filename);
        exit(1);
    }

    off_t size = sb.st_size;
    if (size == 0) {
        return AOF_CHECK_EMPTY;
    }

    if (preamble) {
        char *argv[2] = {NULL, aof_filepath};
        if (redis_check_rdb_main(2, argv, fp) == C_ERR) {
            printf("RDB preamble of AOF file is not sane, aborting.\n");
            exit(1);
        } else {
            printf("RDB preamble is OK, proceeding with AOF tail...\n");
        }
    }

    while(1) {
        if (!multi) pos = ftello(fp);
        if (fgets(buf, sizeof(buf), fp) == NULL) {
            if (feof(fp)) {
                break;
            }
            printf("Failed to read from AOF %s, aborting...\n", aof_filename);
            exit(1);
        }

        if (fseek(fp, -1, SEEK_CUR) == -1) {
            printf("Failed to fseek in AOF %s: %s", aof_filename, strerror(errno));
            exit(1);
        }
    
        if (buf[0] == '#') {
            if (!processAnnotations(fp, aof_filepath, last_file)) {
                fclose(fp);
                return AOF_CHECK_TIMESTAMP_TRUNCATED;
            }
        } else if (buf[0] == '*'){
            if (!processRESP(fp, aof_filepath, &multi)) break;
        } else {
            printf("AOF %s format error\n", aof_filename);
            break;
        }
    }

    if (feof(fp) && multi && strlen(error) == 0) {
        ERROR("Reached EOF before reading EXEC for MULTI");
    }

    if (strlen(error) > 0) {
        printf("%s\n", error);
    }

    diff = size-pos;

    /* In truncate-to-timestamp mode, just exit if there is nothing to truncate. */
    if (diff == 0 && to_timestamp) {
        printf("Truncate nothing in AOF %s to timestamp %ld\n", aof_filename, to_timestamp);
        fclose(fp);
        return AOF_CHECK_OK;
    }

    printf("AOF analyzed: filename=%s, size=%lld, ok_up_to=%lld, ok_up_to_line=%lld, diff=%lld\n",
        aof_filename, (long long) size, (long long) pos, line, (long long) diff);
    if (diff > 0) {
        if (fix) {
            if (!last_file) {
                printf("Failed to truncate AOF %s because it is not the last file\n", aof_filename);
                exit(1);
            }

            char buf[2];
            printf("This will shrink the AOF %s from %lld bytes, with %lld bytes, to %lld bytes\n",
                aof_filename, (long long)size, (long long)diff, (long long)pos);
            printf("Continue? [y/N]: ");
            if (fgets(buf, sizeof(buf), stdin) == NULL || strncasecmp(buf, "y", 1) != 0) {
                printf("Aborting...\n");
                exit(1);
            }
            if (ftruncate(fileno(fp), pos) == -1) {
                printf("Failed to truncate AOF %s\n", aof_filename);
                exit(1);
            } else {
                fclose(fp);
                return AOF_CHECK_TRUNCATED;
            }
        } else {
            printf("AOF %s is not valid. Use the --fix option to try fixing it.\n", aof_filename);
            exit(1);
        }
    }
    fclose(fp);
    return AOF_CHECK_OK;
}

/* Used to determine whether the file is a RDB file. These two possibilities:
 * 1. The file is an old style RDB-preamble AOF
 * 2. The file is a BASE AOF in Multi Part AOF
 * */
int fileIsRDB(char *filepath) {
    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) {
        printf("Cannot open file %s: %s\n", filepath, strerror(errno));
        exit(1);
    }

    struct redis_stat sb;
    if (redis_fstat(fileno(fp), &sb) == -1) {
        printf("Cannot stat file: %s\n", filepath);
        exit(1);
    }

    off_t size = sb.st_size;
    if (size == 0) {
        fclose(fp);
        return 0;
    }

    if (size >= 8) {    /* There must be at least room for the RDB header. */
        char sig[5];
        int rdb_file = fread(sig, sizeof(sig), 1, fp) == 1 &&
                            memcmp(sig, "REDIS", sizeof(sig)) == 0;
        if (rdb_file) {
            fclose(fp);
            return 1;
        } 
    }

    fclose(fp);
    return 0;
}

/* Used to determine whether the file is a manifest file. */
#define MANIFEST_MAX_LINE 1024
int fileIsManifest(char *filepath) {
    int is_manifest = 0;
    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) {
        printf("Cannot open file %s: %s\n", filepath, strerror(errno));
        exit(1);
    }

    struct redis_stat sb;
    if (redis_fstat(fileno(fp), &sb) == -1) {
        printf("Cannot stat file: %s\n", filepath);
        exit(1);
    }

    off_t size = sb.st_size;
    if (size == 0) {
        fclose(fp);
        return 0;
    }

    char buf[MANIFEST_MAX_LINE+1];
    while (1) {
        if (fgets(buf, MANIFEST_MAX_LINE+1, fp) == NULL) {
            if (feof(fp)) {
                break;
            } else {
                printf("Cannot read file: %s\n", filepath);
                exit(1);
            }
        }

        /* Skip comments lines */
        if (buf[0] == '#') {
            continue;
        } else if (!memcmp(buf, "file", strlen("file"))) {
            is_manifest = 1;
        }
    }

    fclose(fp);
    return is_manifest;
}

/* Get the format of the file to be checked. It can be:
 * AOF_RESP: Old-style AOF
 * AOF_RDB_PREAMBLE: Old-style RDB-preamble AOF
 * AOF_MULTI_PART: manifest in Multi Part AOF 
 * 
 * redis-check-aof tool will automatically perform different 
 * verification logic according to different file formats.
 * */
input_file_type getInputFileType(char *filepath) {
    if (fileIsManifest(filepath)) {
        return AOF_MULTI_PART;
    } else if (fileIsRDB(filepath)) {
        return AOF_RDB_PREAMBLE;
    } else {
        return AOF_RESP;
    }
}

void printAofStyle(int ret, char *aofFileName, char *aofType) {
    if (ret == AOF_CHECK_OK) {
        printf("%s %s is valid\n", aofType, aofFileName);
    } else if (ret == AOF_CHECK_EMPTY) {
        printf("%s %s is empty\n", aofType, aofFileName);
    } else if (ret == AOF_CHECK_TIMESTAMP_TRUNCATED) {
        printf("Successfully truncated AOF %s to timestamp %ld\n",
            aofFileName, to_timestamp);
    } else if (ret == AOF_CHECK_TRUNCATED) {
        printf("Successfully truncated AOF %s\n", aofFileName);
    }
}

/* Check if Multi Part AOF is valid. It will check the BASE file and INCR files 
 * at once according to the manifest instructions (this is somewhat similar to 
 * redis' AOF loading).
 * 
 * When the verification is successful, we can guarantee:
 * 1. The manifest file format is valid
 * 2. Both BASE AOF and INCR AOFs format are valid
 * 3. No BASE or INCR AOFs files are missing
 * 
 * Note that in Multi Part AOF, we only allow truncation for the last AOF file.
 * */
void checkMultiPartAof(char *dirpath, char *manifest_filepath, int fix) {
    int total_num = 0, aof_num = 0, last_file;
    int ret;

    printf("Start checking Multi Part AOF\n");
    aofManifest *am = aofLoadManifestFromFile(manifest_filepath);

    if (am->base_aof_info) total_num++;
    if (am->incr_aof_list) total_num += listLength(am->incr_aof_list);

    if (am->base_aof_info) {
        sds aof_filename = am->base_aof_info->file_name;
        sds aof_filepath = makePath(dirpath, aof_filename);
        last_file = ++aof_num == total_num;
        int aof_preable = fileIsRDB(aof_filepath);

        printf("Start to check BASE AOF (%s format).\n", aof_preable ? "RDB":"RESP");
        ret = checkSingleAof(aof_filename, aof_filepath, last_file, fix, aof_preable);
        printAofStyle(ret, aof_filename, (char *)"BASE AOF");
        sdsfree(aof_filepath);
    }

    if (listLength(am->incr_aof_list)) {
        listNode *ln;
        listIter li;

        printf("Start to check INCR files.\n");
        listRewind(am->incr_aof_list, &li);
        while ((ln = listNext(&li)) != NULL) {
            aofInfo *ai = (aofInfo*)ln->value;
            sds aof_filename = (char*)ai->file_name;
            sds aof_filepath = makePath(dirpath, aof_filename);
            last_file = ++aof_num == total_num;
            ret = checkSingleAof(aof_filename, aof_filepath, last_file, fix, 0);
            printAofStyle(ret, aof_filename, (char *)"INCR AOF");
            sdsfree(aof_filepath);
        }
    }

    aofManifestFree(am);
    printf("All AOF files and manifest are valid\n");
}

/* Check if old style AOF is valid. Internally, it will identify whether 
 * the AOF is in RDB-preamble format, and will eventually call `checkSingleAof`
 * to do the check. */
void checkOldStyleAof(char *filepath, int fix, int preamble) {
    printf("Start checking Old-Style AOF\n");
    int ret = checkSingleAof(filepath, filepath, 1, fix, preamble);
    printAofStyle(ret, filepath, (char *)"AOF");
}

int redis_check_aof_main(int argc, char **argv) {
    char *filepath;
    char temp_filepath[PATH_MAX + 1];
    char *dirpath;
    int fix = 0;

    if (argc < 2) {
        goto invalid_args;
    } else if (argc == 2) {
        filepath = argv[1];
    } else if (argc == 3) {
        if (!strcmp(argv[1], "--fix")) {
            filepath = argv[2];
            fix = 1;
        } else {
            goto invalid_args;
        }
    } else if (argc == 4) {
        if (!strcmp(argv[1], "--truncate-to-timestamp")) {
            char *endptr;
            errno = 0;
            to_timestamp = strtol(argv[2], &endptr, 10);
            if (errno != 0 || *endptr != '\0') {
                printf("Invalid timestamp, aborting...\n");
                exit(1);
            }
            filepath = argv[3];
        } else {
            goto invalid_args;
        }
    } else {
        goto invalid_args;
    }

    /* In the glibc implementation dirname may modify their argument. */
    memcpy(temp_filepath, filepath, strlen(filepath) + 1);
    dirpath = dirname(temp_filepath);

    /* Select the corresponding verification method according to the input file type. */
    input_file_type type = getInputFileType(filepath);
    switch (type) {
    case AOF_MULTI_PART:
        checkMultiPartAof(dirpath, filepath, fix);
        break;
    case AOF_RESP:
        checkOldStyleAof(filepath, fix, 0);
        break;
    case AOF_RDB_PREAMBLE:
        checkOldStyleAof(filepath, fix, 1);
        break;
    }

    exit(0);

invalid_args:
    printf("Usage: %s [--fix|--truncate-to-timestamp $timestamp] <file.manifest|file.aof>\n",
        argv[0]);
    exit(1);
}

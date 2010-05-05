#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "config.h"

#define ERROR(...) { \
    char __buf[1024]; \
    sprintf(__buf, __VA_ARGS__); \
    sprintf(error, "0x%08lx: %s", epos, __buf); \
}

static char error[1024];
static long epos;

int consumeNewline(char *buf) {
    if (strncmp(buf,"\r\n",2) != 0) {
        ERROR("Expected \\r\\n, got: %02x%02x",buf[0],buf[1]);
        return 0;
    }
    return 1;
}

int readLong(FILE *fp, char prefix, long *target) {
    char buf[128], *eptr;
    epos = ftell(fp);
    if (fgets(buf,sizeof(buf),fp) == NULL) {
        return 0;
    }
    if (buf[0] != prefix) {
        ERROR("Expected prefix '%c', got: '%c'",buf[0],prefix);
        return 0;
    }
    *target = strtol(buf+1,&eptr,10);
    return consumeNewline(eptr);
}

int readBytes(FILE *fp, char *target, long length) {
    long real;
    epos = ftell(fp);
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

    /* Increase length to also consume \r\n */
    len += 2;
    *target = (char*)malloc(len);
    if (!readBytes(fp,*target,len)) {
        free(*target);
        *target = NULL;
        return 0;
    }
    if (!consumeNewline(*target+len-2)) {
        free(*target);
        *target = NULL;
        return 0;
    }
    (*target)[len-2] = '\0';
    return 1;
}

int readArgc(FILE *fp, long *target) {
    return readLong(fp,'*',target);
}

long process(FILE *fp) {
    long argc, pos = 0;
    int i, multi = 0;
    char *str;

    while(1) {
        if (!multi) pos = ftell(fp);
        if (!readArgc(fp, &argc)) {
            break;
        }

        for (i = 0; i < argc; i++) {
            if (!readString(fp,&str)) {
                break;
            }
            if (i == 0) {
                if (strcasecmp(str, "multi") == 0) {
                    if (multi++) {
                        ERROR("Unexpected MULTI");
                        break;
                    }
                } else if (strcasecmp(str, "exec") == 0) {
                    if (--multi) {
                        ERROR("Unexpected EXEC");
                        break;
                    }
                }
            }
            free(str);
        }

        /* Check if loop was finished */
        if (i < argc) {
            if (str) free(str);
            break;
        }
    }

    if (feof(fp) && multi && strlen(error) == 0) {
        ERROR("Reached EOF before reading EXEC for MULTI");
    }

    if (strlen(error) > 0) {
        printf("%s\n", error);
    }

    return pos;
}

int main(int argc, char **argv) {
    /* expect the first argument to be the dump file */
    if (argc <= 1) {
        printf("Usage: %s <file.aof>\n", argv[0]);
        exit(0);
    }

    FILE *fp = fopen(argv[1],"r");
    if (fp == NULL) {
        printf("Cannot open file: %s\n", argv[1]);
        exit(1);
    }

    struct redis_stat sb;
    if (redis_fstat(fileno(fp),&sb) == -1) {
        printf("Cannot stat file: %s\n", argv[1]);
        exit(1);
    }

    long size = sb.st_size;
    if (size == 0) {
        printf("Empty file: %s\n", argv[1]);
        exit(1);
    }

    long pos = process(fp);
    if (pos < size) {
        printf("First invalid operation at offset %ld.\n", pos);
        exit(1);
    } else {
        printf("AOF is valid.\n");
    }

    fclose(fp);
    return 0;
}

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
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
        return 0;
    }
    if (!consumeNewline(*target+len-2)) {
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
        if (!readArgc(fp, &argc)) break;

        for (i = 0; i < argc; i++) {
            if (!readString(fp,&str)) break;
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

        /* Stop if the loop did not finish */
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
    char *filename;
    int fix = 0;

    if (argc < 2) {
        printf("Usage: %s [--fix] <file.aof>\n", argv[0]);
        exit(1);
    } else if (argc == 2) {
        filename = argv[1];
    } else if (argc == 3) {
        if (strcmp(argv[1],"--fix") != 0) {
            printf("Invalid argument: %s\n", argv[1]);
            exit(1);
        }
        filename = argv[2];
        fix = 1;
    } else {
        printf("Invalid arguments\n");
        exit(1);
    }

    FILE *fp = fopen(filename,"r+");
    if (fp == NULL) {
        printf("Cannot open file: %s\n", filename);
        exit(1);
    }

    struct redis_stat sb;
    if (redis_fstat(fileno(fp),&sb) == -1) {
        printf("Cannot stat file: %s\n", filename);
        exit(1);
    }

    long size = sb.st_size;
    if (size == 0) {
        printf("Empty file: %s\n", filename);
        exit(1);
    }

    long pos = process(fp);
    long diff = size-pos;
    if (diff > 0) {
        if (fix) {
            char buf[2];
            printf("This will shrink the AOF from %ld bytes, with %ld bytes, to %ld bytes\n",size,diff,pos);
            printf("Continue? [y/N]: ");
            if (fgets(buf,sizeof(buf),stdin) == NULL ||
                strncasecmp(buf,"y",1) != 0) {
                    printf("Aborting...\n");
                    exit(1);
            }
            if (ftruncate(fileno(fp), pos) == -1) {
                printf("Failed to truncate AOF\n");
                exit(1);
            } else {
                printf("Successfully truncated AOF\n");
            }
        } else {
            printf("AOF is not valid\n");
            exit(1);
        }
    } else {
        printf("AOF is valid\n");
    }

    fclose(fp);
    return 0;
}

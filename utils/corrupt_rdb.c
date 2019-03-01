/* Trivia program to corrupt an RDB file in order to check the RDB check
 * program behavior and effectiveness.
 *
 * Copyright (C) 2016 Salvatore Sanfilippo.
 * This software is released in the 3-clause BSD license. */

#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

int main(int argc, char **argv) {
    struct stat stat;
    int fd, cycles;

    if (argc != 3) {
        fprintf(stderr,"Usage: <filename> <cycles>\n");
        exit(1);
    }

    srand(time(NULL));
    char *filename = argv[1];
    cycles = atoi(argv[2]);
    fd = open(filename,O_RDWR);
    if (fd == -1) {
        perror("open");
        exit(1);
    }
    fstat(fd,&stat);

    while(cycles--) {
        unsigned char buf[32];
        unsigned long offset = rand()%stat.st_size;
        int writelen = 1+rand()%31;
        int j;

        for (j = 0; j < writelen; j++) buf[j] = (char)rand();
        lseek(fd,offset,SEEK_SET);
        printf("Writing %d bytes at offset %lu\n", writelen, offset);
        write(fd,buf,writelen);
    }
    return 0;
}

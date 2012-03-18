#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>

#if (ULONG_MAX == 4294967295UL)
#define MEMTEST_32BIT
#elif (ULONG_MAX == 18446744073709551615ULL)
#define MEMTEST_64BIT
#else
#error "ULONG_MAX value not supported."
#endif

static struct winsize ws;
size_t progress_printed; /* Printed chars in screen-wide progress bar. */
size_t progress_full; /* How many chars to write to fill the progress bar. */

void memtest_progress_start(char *title, int pass) {
    int j;

    printf("\x1b[H\x1b[2J");    /* Cursor home, clear screen. */
    /* Fill with dots. */
    for (j = 0; j < ws.ws_col*(ws.ws_row-2); j++) printf(".");
    printf("Please keep the test running several minutes per GB of memory.\n");
    printf("Also check http://www.memtest86.com/ and http://pyropus.ca/software/memtester/");
    printf("\x1b[H\x1b[2K");          /* Cursor home, clear current line.  */
    printf("%s [%d]\n", title, pass); /* Print title. */
    progress_printed = 0;
    progress_full = ws.ws_col*(ws.ws_row-3);
    fflush(stdout);
}

void memtest_progress_end(void) {
    printf("\x1b[H\x1b[2J");    /* Cursor home, clear screen. */
}

void memtest_progress_step(size_t curr, size_t size, char c) {
    size_t chars = (curr*progress_full)/size, j;

    for (j = 0; j < chars-progress_printed; j++) {
        printf("%c",c);
        progress_printed++;
    }
    fflush(stdout);
}

/* Fill words stepping a single page at every write, so we continue to
 * touch all the pages in the smallest amount of time reducing the
 * effectiveness of caches, and making it hard for the OS to transfer
 * pages on the swap. */
void memtest_fill(unsigned long *l, size_t bytes) {
    unsigned long step = 4096/sizeof(unsigned long);
    unsigned long words = bytes/sizeof(unsigned long)/2;
    unsigned long iwords = words/step;  /* words per iteration */
    unsigned long off, w, *l1, *l2;

    assert((bytes & 4095) == 0);
    for (off = 0; off < step; off++) {
        l1 = l+off;
        l2 = l1+words;
        for (w = 0; w < iwords; w++) {
#ifdef MEMTEST_32BIT
            *l1 = *l2 = ((unsigned long)     (rand()&0xffff)) |
                        (((unsigned long)    (rand()&0xffff)) << 16);
#else
            *l1 = *l2 = ((unsigned long)     (rand()&0xffff)) |
                        (((unsigned long)    (rand()&0xffff)) << 16) |
                        (((unsigned long)    (rand()&0xffff)) << 32) |
                        (((unsigned long)    (rand()&0xffff)) << 48);
#endif
            l1 += step;
            l2 += step;
            if ((w & 0xffff) == 0)
                memtest_progress_step(w+iwords*off,words,'+');
        }
    }
}

void memtest_compare(unsigned long *l, size_t bytes) {
    unsigned long words = bytes/sizeof(unsigned long)/2;
    unsigned long w, *l1, *l2;

    assert((bytes & 4095) == 0);
    l1 = l;
    l2 = l1+words;
    for (w = 0; w < words; w++) {
        if (*l1 != *l2) {
            printf("\n*** MEMORY ERROR DETECTED: %p != %p (%lu vs %lu)\n",
                (void*)l1, (void*)l2, *l1, *l2);
            exit(1);
        }
        l1 ++;
        l2 ++;
        if ((w & 0xffff) == 0) memtest_progress_step(w,words,'=');
    }
}

void memtest_test(size_t megabytes, int passes) {
    size_t bytes = megabytes*1024*1024;
    unsigned long *m = malloc(bytes);
    int pass = 0, j;

    if (m == NULL) {
        fprintf(stderr,"Unable to allocate %zu megabytes: %s",
            megabytes, strerror(errno));
        exit(1);
    }
    while (pass != passes) {
        pass++;
        memtest_progress_start("Random fill",pass);
        memtest_fill(m,bytes);
        memtest_progress_end();
        for (j = 0; j < 4; j++) {
            memtest_progress_start("Compare",pass);
            memtest_compare(m,bytes);
            memtest_progress_end();
        }
    }
}

void memtest(size_t megabytes, int passes) {
    if (ioctl(1, TIOCGWINSZ, &ws) == -1) {
        ws.ws_col = 80;
        ws.ws_row = 20;
    }
    memtest_test(megabytes,passes);
    printf("\nYour memory passed this test.\n");
    printf("Please if you are still in doubt use the following two tools:\n");
    printf("1) memtest86: http://www.memtest86.com/\n");
    printf("2) memtester: http://pyropus.ca/software/memtester/\n");
    exit(0);
}

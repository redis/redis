#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>

#if (ULONG_MAX == 4294967295UL)
#define MEMTEST_32BIT
#elif (ULONG_MAX == 18446744073709551615ULL)
#define MEMTEST_64BIT
#else
#error "ULONG_MAX value not supported."
#endif

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
    }
}

void memtest_test(size_t megabytes, int passes) {
    size_t bytes = megabytes*1024*1024;
    unsigned long *m = malloc(bytes);
    int pass = 0;

    if (m == NULL) {
        fprintf(stderr,"Unable to allocate %zu megabytes: %s",
            megabytes, strerror(errno));
        exit(1);
    }
    while (pass != passes) {
        pass++;
        printf("PASS %d... ", pass);
        fflush(stdout);
        memtest_fill(m,bytes);
        memtest_compare(m,bytes);
        printf("ok\n");
    }
}

void memtest(size_t megabytes, int passes) {
    memtest_test(megabytes,passes);
    printf("\nYour memory passed this test.\n");
    printf("Please if you are stil in doubt use the following two tools:\n");
    printf("1) memtest86: http://www.memtest86.com/\n");
    printf("2) memtester: http://pyropus.ca/software/memtester/\n");
    exit(0);
}

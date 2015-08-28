/*
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
#ifdef _WIN32
#include "Win32_Interop/Win32_Portability.h"
#include "Win32_Interop/win32_types.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#ifndef _WIN32
#include <termios.h>
#include <sys/ioctl.h>
#if defined(__sun)
#include <stropts.h>
#endif
#else
#include "Win32_Interop/win32fixes.h"
#include "Win32_Interop/win32_ANSI.h"
#endif
#include "config.h"


#if (PORT_ULONG_MAX == 4294967295UL)
#define MEMTEST_32BIT
#elif (PORT_ULONG_MAX == 18446744073709551615ULL)
#define MEMTEST_64BIT
#else
#error "PORT_ULONG_MAX value not supported."
#endif

#ifdef MEMTEST_32BIT
#define ULONG_ONEZERO 0xaaaaaaaaUL
#define ULONG_ZEROONE 0x55555555UL
#else
#define ULONG_ONEZERO 0xaaaaaaaaaaaaaaaaUL
#define ULONG_ZEROONE 0x5555555555555555UL
#endif

#ifdef _WIN32
typedef struct winsize
{
    unsigned short ws_row;
    unsigned short ws_col;
};
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
    size_t chars = ((PORT_ULONGLONG)curr*progress_full)/size, j;

    for (j = 0; j < chars-progress_printed; j++) printf("%c",c);
    progress_printed = chars;
    fflush(stdout);
}

/* Test that addressing is fine. Every location is populated with its own
 * address, and finally verified. This test is very fast but may detect
 * ASAP big issues with the memory subsystem. */
void memtest_addressing(PORT_ULONG *l, size_t bytes) {
    PORT_ULONG words = (PORT_ULONG)(bytes/sizeof(PORT_ULONG));
    PORT_ULONG j,*p;

    /* Fill */
    p = l;
    for (j = 0; j < words; j++) {
        *p = (PORT_ULONG)p;
        p++;
        if ((j & 0xffff) == 0) memtest_progress_step(j,words*2,'A');
    }
    /* Test */
    p = l;
    for (j = 0; j < words; j++) {
        if (*p != (PORT_ULONG)p) {
            printf("\n*** MEMORY ADDRESSING ERROR: %p contains %Iu\n",          WIN_PORT_FIX /* %lu -> %Iu */
                (void*) p, *p);
            exit(1);
        }
        p++;
        if ((j & 0xffff) == 0) memtest_progress_step(j+words,words*2,'A');
    }
}

/* Fill words stepping a single page at every write, so we continue to
 * touch all the pages in the smallest amount of time reducing the
 * effectiveness of caches, and making it hard for the OS to transfer
 * pages on the swap. */
void memtest_fill_random(PORT_ULONG *l, size_t bytes) {
    PORT_ULONG step = (PORT_ULONG)(4096/sizeof(PORT_ULONG));
    PORT_ULONG words = (PORT_ULONG)(bytes/sizeof(PORT_ULONG)/2);
    PORT_ULONG iwords = words/step;  /* words per iteration */
    PORT_ULONG off, w, *l1, *l2;

    assert((bytes & 4095) == 0);
    for (off = 0; off < step; off++) {
        l1 = l+off;
        l2 = l1+words;
        for (w = 0; w < iwords; w++) {
#ifdef MEMTEST_32BIT
            *l1 = *l2 = ((PORT_ULONG)     (rand() & 0xffff)) |
                        (((PORT_ULONG)    (rand()&0xffff)) << 16);
#else
            *l1 = *l2 = ((PORT_ULONG)     (rand()&0xffff)) |
                        (((PORT_ULONG)    (rand()&0xffff)) << 16) |
                        (((PORT_ULONG)    (rand()&0xffff)) << 32) |
                        (((PORT_ULONG)    (rand()&0xffff)) << 48);
#endif
            l1 += step;
            l2 += step;
            if ((w & 0xffff) == 0)
                memtest_progress_step(w+iwords*off,words,'R');
        }
    }
}

/* Like memtest_fill_random() but uses the two specified values to fill
 * memory, in an alternated way (v1|v2|v1|v2|...) */
void memtest_fill_value(PORT_ULONG *l, size_t bytes, PORT_ULONG v1,
                        PORT_ULONG v2, char sym)
{
    PORT_ULONG step = (PORT_ULONG)(4096/sizeof(PORT_ULONG));
    PORT_ULONG words = (PORT_ULONG)(bytes/sizeof(PORT_ULONG)/2);
    PORT_ULONG iwords = words/step;  /* words per iteration */
    PORT_ULONG off, w, *l1, *l2, v;

    assert((bytes & 4095) == 0);
    for (off = 0; off < step; off++) {
        l1 = l+off;
        l2 = l1+words;
        v = (off & 1) ? v2 : v1;
        for (w = 0; w < iwords; w++) {
#ifdef MEMTEST_32BIT
            *l1 = *l2 = ((PORT_ULONG)     v) |
                        (((PORT_ULONG)    v) << 16);
#else
            *l1 = *l2 = ((PORT_ULONG)     v) |
                        (((PORT_ULONG)    v) << 16) |
                        (((PORT_ULONG)    v) << 32) |
                        (((PORT_ULONG)    v) << 48);
#endif
            l1 += step;
            l2 += step;
            if ((w & 0xffff) == 0)
                memtest_progress_step(w+iwords*off,words,sym);
        }
    }
}

void memtest_compare(PORT_ULONG *l, size_t bytes) {
    PORT_ULONG words = (PORT_ULONG)(bytes/sizeof(PORT_ULONG)/2);
    PORT_ULONG w, *l1, *l2;

    assert((bytes & 4095) == 0);
    l1 = l;
    l2 = l1+words;
    for (w = 0; w < words; w++) {
        if (*l1 != *l2) {
            printf("\n*** MEMORY ERROR DETECTED: %p != %p (%Iu vs %Iu)\n",      WIN_PORT_FIX /* %lu -> %Iu */
                (void*)l1, (void*)l2, *l1, *l2);
            exit(1);
        }
        l1 ++;
        l2 ++;
        if ((w & 0xffff) == 0) memtest_progress_step(w,words,'=');
    }
}

void memtest_compare_times(PORT_ULONG *m, size_t bytes, int pass, int times) {
    int j;

    for (j = 0; j < times; j++) {
        memtest_progress_start("Compare",pass);
        memtest_compare(m,bytes);
        memtest_progress_end();
    }
}

void memtest_test(size_t megabytes, int passes) {
    size_t bytes = megabytes*1024*1024;
#ifdef _WIN32
    PORT_ULONG *m = VirtualAllocEx(
        GetCurrentProcess(),
        NULL,
        bytes,
        MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN,
        PAGE_READWRITE);
#else
    PORT_ULONG *m = malloc(bytes);
#endif
    int pass = 0;

    if (m == NULL) {
        fprintf(stderr,"Unable to allocate %Iu megabytes: %s",                  WIN_PORT_FIX /* %zu -> %Iu */
            megabytes, strerror(errno));
        exit(1);
    }
    while (pass != passes) {
        pass++;

        memtest_progress_start("Addressing test",pass);
        memtest_addressing(m,bytes);
        memtest_progress_end();

        memtest_progress_start("Random fill",pass);
        memtest_fill_random(m,bytes);
        memtest_progress_end();
        memtest_compare_times(m,bytes,pass,4);

        memtest_progress_start("Solid fill",pass);
        memtest_fill_value(m,bytes,0,(PORT_ULONG)-1,'S');
        memtest_progress_end();
        memtest_compare_times(m,bytes,pass,4);

        memtest_progress_start("Checkerboard fill",pass);
        memtest_fill_value(m,bytes,ULONG_ONEZERO,ULONG_ZEROONE,'C');
        memtest_progress_end();
        memtest_compare_times(m,bytes,pass,4);
    }
#ifdef _WIN32
    VirtualFreeEx(GetCurrentProcess(), m, 0, MEM_RELEASE);
#else
    free(m);
#endif
}

void memtest_non_destructive_invert(void *addr, size_t size) {
    volatile PORT_ULONG *p = addr;
    size_t words = size / sizeof(PORT_ULONG);
    size_t j;

    /* Invert */
    for (j = 0; j < words; j++)
        p[j] = ~p[j];
}

void memtest_non_destructive_swap(void *addr, size_t size) {
    volatile PORT_ULONG *p = addr;
    size_t words = size / sizeof(PORT_ULONG);
    size_t j;

    /* Swap */
    for (j = 0; j < words; j += 2) {
        PORT_ULONG a, b;

        a = p[j];
        b = p[j+1];
        p[j] = b;
        p[j+1] = a;
    }
}

void memtest(size_t megabytes, int passes) {
#ifdef _WIN32
    HANDLE hOut;
    CONSOLE_SCREEN_BUFFER_INFO b;

    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleScreenBufferInfo(hOut, &b)) {
        ws.ws_col = b.dwSize.X;
        ws.ws_row = b.dwSize.Y;
    } else {
        ws.ws_col = 80;
        ws.ws_row = 20;
    }
#else
    if (ioctl(1, TIOCGWINSZ, &ws) == -1) {
        ws.ws_col = 80;
        ws.ws_row = 20;
    }
#endif
    memtest_test(megabytes,passes);
    printf("\nYour memory passed this test.\n");
    printf("Please if you are still in doubt use the following two tools:\n");
    printf("1) memtest86: http://www.memtest86.com/\n");
    printf("2) memtester: http://pyropus.ca/software/memtester/\n");
    exit(0);
}

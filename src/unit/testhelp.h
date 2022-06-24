/* This is a really minimal testing framework for C.
 *
 * Example:
 *
 * TEST("SDS Test") {
 *     ASSERT("Check if 1 == 1", 1==1)
 *     ASSERT("Check if 5 > 10", 5 > 10)
 * }
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __TESTHELP_H
#define __TESTHELP_H

#include <stdlib.h>
#include <stdio.h>

#define REDIS_TEST_ACCURATE     (1<<0)
#define REDIS_TEST_LARGE_MEMORY (1<<1)
#define REDIS_TEST_VALGRIND     (1<<2)

extern int __failed_tests;
extern int __test_num;

#define KRED  "\33[31m"
#define KGRN  "\33[32m"
#define KBLUE  "\33[33m"
#define KRESET "\33[0m"

#define ASSERT(descr,_c) do { \
    if(_c) printf("[" KGRN "PASSED" KRESET "] "); else {printf("[" KRED "FAILED" KRESET "] "); __failed_tests++;} \
    __test_num++; printf("%d - %s\n", __test_num, descr); \
} while(0)

#define ASSERT1(_c) do { \
    if(_c) printf("[" KGRN "PASSED" KRESET "] "); else {printf("[" KRED "FAILED" KRESET "] "); __failed_tests++;} \
    __test_num++; printf("%d - %s\n", __test_num, #_c); \
} while(0)

/* Explicitly fail the given test */
#define FAIL(x, ...)                                                            \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __func__, __LINE__);                   \
        printf("ERROR! " x "\n", __VA_ARGS__);                                 \
        err++;                                                                 \
    } while (0)

#define TEST(name) printf("test — %s\n", name);
#define TEST_DESC(name, ...) printf("test — " name "\n", __VA_ARGS__);

#define init_test_report() \
    __failed_tests = 0;\
    __test_num = 0

#define UNUSED(x) (void)(x)

#endif

/* This is a really minimal testing framework for C.
 *
 * Example:
 *
 * test_cond("Check if 1 == 1", 1==1)
 * test_cond("Check if 5 > 10", 5 > 10)
 * test_report()
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef __TESTHELP_H
#define __TESTHELP_H

#define REDIS_TEST_ACCURATE     (1<<0)
#define REDIS_TEST_LARGE_MEMORY (1<<1)
#define REDIS_TEST_VALGRIND     (1<<2)
#define REDIS_TEST_VERBOSE      (1<<3)


extern int __failed_tests;
extern int __test_num;

#define test_cond(descr,_c) do { \
    __test_num++; printf("%d - %s: ", __test_num, descr); \
    if(_c) printf("\033[32mPASSED\033[0m\n"); else {printf("\033[31mFAILED\033[0m\n"); __failed_tests++;} \
} while(0)
#define test_report() do { \
    if (__failed_tests) { \
        printf("  Tests:       %d passed, \033[31m%d failed\033[0m, %d total\n", \
                        __test_num-__failed_tests, __failed_tests, __test_num); \
        printf("\033[31m=== WARNING === We have failed tests here...\033[0m\n"); \
        exit(1); \
    } else { \
        printf("  Tests:       \033[32m%d passed\033[0m, %d failed, %d total\n", \
                        __test_num-__failed_tests, __failed_tests, __test_num); \
    } \
} while(0)

#endif

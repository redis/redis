#include <strings.h>
#include <stdio.h>
#include "test_files.h"
#include "testhelp.h"

int __failed_tests = 0;
int __test_num = 0;

/* The flags are the following:
* --accurate:     Runs tests with more iterations.
* --large-memory: Enables tests that consume more than 100mb.
* --quiet:        Only prints out text during failed conditions. */
struct redisTest *getTestByName(const char *name) {
    int numtests = sizeof(redisTests)/sizeof(struct redisTest);
    for (int j = 0; j < numtests; j++) {
        if (!strcasecmp(name,redisTests[j].name)) {
            return &redisTests[j];
        }
    }
    return NULL;
}

int runTest(struct redisTest *test, int argc, char **argv, int flags) {
    printf("[" KBLUE "START" KRESET "] Test - %s\n", test->name);
    init_test_report();
    test->failed = (test->proc(argc, argv, flags) != 0);

    if (__failed_tests) {
        printf("[" KRED "fail" KRESET "]");
    } else {
        printf("[" KGRN "ok" KRESET "]");
    }
    printf(" Test - %s: ", test->name);
    printf("%d tests, %d passed, %d failed\n", __test_num,
        __test_num - __failed_tests, __failed_tests);
    return !__failed_tests;
}

int main(int argc, char **argv) {
    int flags = 0;
    for (int j = 2; j < argc; j++) {
        char *arg = argv[j];
        if (!strcasecmp(arg, "--accurate")) flags |= REDIS_TEST_ACCURATE;
        else if (!strcasecmp(arg, "--large-memory")) flags |= REDIS_TEST_LARGE_MEMORY;
    }

    if (!strcasecmp(argv[1], "all")) {
        int numtests = sizeof(redisTests)/sizeof(struct redisTest);
        int failed_num = 0;
        for (int j = 0; j < numtests; j++) {
            if (!runTest(&redisTests[j], argc, argv, flags)) {
                failed_num++;
            }
        }

        printf("%d test suites executed, %d passed, %d failed\n", numtests,
                numtests-failed_num, failed_num);

        return failed_num == 0 ? 0 : 1;
    } else {
        struct redisTest *test = getTestByName(argv[1]);
        if (!test) return -1; /* test not found */
        return runTest(test, argc, argv, flags);
    }

    return 0;
}

void _serverAssert(const char *estr, const char *file, int line) {
    printf("Hi");
    UNUSED(estr);
    UNUSED(file);
    UNUSED(line);
}

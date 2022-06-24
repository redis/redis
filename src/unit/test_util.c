#include <assert.h>
#include <string.h>
#include <limits.h>
#include "testhelp.h"
#include "../util.h"

static void test_string2ll(void) {
    char buf[32];
    long long v;

    /* May not start with +. */
    strcpy(buf,"+1");
    ASSERT1(string2ll(buf,strlen(buf),&v) == 0);

    /* Leading space. */
    strcpy(buf," 1");
    ASSERT1(string2ll(buf,strlen(buf),&v) == 0);

    /* Trailing space. */
    strcpy(buf,"1 ");
    ASSERT1(string2ll(buf,strlen(buf),&v) == 0);

    /* May not start with 0. */
    strcpy(buf,"01");
    ASSERT1(string2ll(buf,strlen(buf),&v) == 0);

    strcpy(buf,"-1");
    ASSERT1(string2ll(buf,strlen(buf),&v) == 1);
    ASSERT1(v == -1);

    strcpy(buf,"0");
    ASSERT1(string2ll(buf,strlen(buf),&v) == 1);
    ASSERT1(v == 0);

    strcpy(buf,"1");
    ASSERT1(string2ll(buf,strlen(buf),&v) == 1);
    ASSERT1(v == 1);

    strcpy(buf,"99");
    ASSERT1(string2ll(buf,strlen(buf),&v) == 1);
    ASSERT1(v == 99);

    strcpy(buf,"-99");
    ASSERT1(string2ll(buf,strlen(buf),&v) == 1);
    ASSERT1(v == -99);

    strcpy(buf,"-9223372036854775808");
    ASSERT1(string2ll(buf,strlen(buf),&v) == 1);
    ASSERT1(v == LLONG_MIN);

    strcpy(buf,"-9223372036854775809"); /* overflow */
    ASSERT1(string2ll(buf,strlen(buf),&v) == 0);

    strcpy(buf,"9223372036854775807");
    ASSERT1(string2ll(buf,strlen(buf),&v) == 1);
    ASSERT1(v == LLONG_MAX);

    strcpy(buf,"9223372036854775808"); /* overflow */
    ASSERT1(string2ll(buf,strlen(buf),&v) == 0);
}

static void test_string2l(void) {
    char buf[32];
    long v;

    /* May not start with +. */
    strcpy(buf,"+1");
    ASSERT1(string2l(buf,strlen(buf),&v) == 0);

    /* May not start with 0. */
    strcpy(buf,"01");
    ASSERT1(string2l(buf,strlen(buf),&v) == 0);

    strcpy(buf,"-1");
    ASSERT1(string2l(buf,strlen(buf),&v) == 1);
    ASSERT1(v == -1);

    strcpy(buf,"0");
    ASSERT1(string2l(buf,strlen(buf),&v) == 1);
    ASSERT1(v == 0);

    strcpy(buf,"1");
    ASSERT1(string2l(buf,strlen(buf),&v) == 1);
    ASSERT1(v == 1);

    strcpy(buf,"99");
    ASSERT1(string2l(buf,strlen(buf),&v) == 1);
    ASSERT1(v == 99);

    strcpy(buf,"-99");
    ASSERT1(string2l(buf,strlen(buf),&v) == 1);
    ASSERT1(v == -99);

#if LONG_MAX != LLONG_MAX
    strcpy(buf,"-2147483648");
    ASSERT1(string2l(buf,strlen(buf),&v) == 1);
    ASSERT1(v == LONG_MIN);

    strcpy(buf,"-2147483649"); /* overflow */
    ASSERT1(string2l(buf,strlen(buf),&v) == 0);

    strcpy(buf,"2147483647");
    ASSERT1(string2l(buf,strlen(buf),&v) == 1);
    ASSERT1(v == LONG_MAX);

    strcpy(buf,"2147483648"); /* overflow */
    ASSERT1(string2l(buf,strlen(buf),&v) == 0);
#endif
}

static void test_ll2string(void) {
    char buf[32];
    long long v;
    int sz;

    v = 0;
    sz = ll2string(buf, sizeof buf, v);
    ASSERT1(sz == 1);
    ASSERT1(!strcmp(buf, "0"));

    v = -1;
    sz = ll2string(buf, sizeof buf, v);
    ASSERT1(sz == 2);
    ASSERT1(!strcmp(buf, "-1"));

    v = 99;
    sz = ll2string(buf, sizeof buf, v);
    ASSERT1(sz == 2);
    ASSERT1(!strcmp(buf, "99"));

    v = -99;
    sz = ll2string(buf, sizeof buf, v);
    ASSERT1(sz == 3);
    ASSERT1(!strcmp(buf, "-99"));

    v = -2147483648;
    sz = ll2string(buf, sizeof buf, v);
    ASSERT1(sz == 11);
    ASSERT1(!strcmp(buf, "-2147483648"));

    v = LLONG_MIN;
    sz = ll2string(buf, sizeof buf, v);
    ASSERT1(sz == 20);
    ASSERT1(!strcmp(buf, "-9223372036854775808"));

    v = LLONG_MAX;
    sz = ll2string(buf, sizeof buf, v);
    ASSERT1(sz == 19);
    ASSERT1(!strcmp(buf, "9223372036854775807"));
}

int utilTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    test_string2ll();
    test_string2l();
    test_ll2string();
    return 0;
}

// #ifdef REDIS_TEST
// #include <assert.h>
// #include <sys/mman.h>
// #include "testhelp.h"

// static void test_string2ll(void) {
//     char buf[32];
//     long long v;

//     /* May not start with +. */
//     redis_strlcpy(buf,"+1",sizeof(buf));
//     ASSERT1(string2ll(buf,strlen(buf),&v) == 0);

//     /* Leading space. */
//     redis_strlcpy(buf," 1",sizeof(buf));
//     ASSERT1(string2ll(buf,strlen(buf),&v) == 0);

//     /* Trailing space. */
//     redis_strlcpy(buf,"1 ",sizeof(buf));
//     ASSERT1(string2ll(buf,strlen(buf),&v) == 0);

//     /* May not start with 0. */
//     redis_strlcpy(buf,"01",sizeof(buf));
//     ASSERT1(string2ll(buf,strlen(buf),&v) == 0);

//     redis_strlcpy(buf,"-1",sizeof(buf));
//     ASSERT1(string2ll(buf,strlen(buf),&v) == 1);
//     ASSERT1(v == -1);

//     redis_strlcpy(buf,"0",sizeof(buf));
//     ASSERT1(string2ll(buf,strlen(buf),&v) == 1);
//     ASSERT1(v == 0);

//     redis_strlcpy(buf,"1",sizeof(buf));
//     ASSERT1(string2ll(buf,strlen(buf),&v) == 1);
//     ASSERT1(v == 1);

//     redis_strlcpy(buf,"99",sizeof(buf));
//     ASSERT1(string2ll(buf,strlen(buf),&v) == 1);
//     ASSERT1(v == 99);

//     redis_strlcpy(buf,"-99",sizeof(buf));
//     ASSERT1(string2ll(buf,strlen(buf),&v) == 1);
//     ASSERT1(v == -99);

//     redis_strlcpy(buf,"-9223372036854775808",sizeof(buf));
//     ASSERT1(string2ll(buf,strlen(buf),&v) == 1);
//     ASSERT1(v == LLONG_MIN);

//     redis_strlcpy(buf,"-9223372036854775809",sizeof(buf)); /* overflow */
//     ASSERT1(string2ll(buf,strlen(buf),&v) == 0);

//     redis_strlcpy(buf,"9223372036854775807",sizeof(buf));
//     ASSERT1(string2ll(buf,strlen(buf),&v) == 1);
//     ASSERT1(v == LLONG_MAX);

//     redis_strlcpy(buf,"9223372036854775808",sizeof(buf)); /* overflow */
//     ASSERT1(string2ll(buf,strlen(buf),&v) == 0);
// }

// static void test_string2l(void) {
//     char buf[32];
//     long v;

//     /* May not start with +. */
//     redis_strlcpy(buf,"+1",sizeof(buf));
//     ASSERT1(string2l(buf,strlen(buf),&v) == 0);

//     /* May not start with 0. */
//     redis_strlcpy(buf,"01",sizeof(buf));
//     ASSERT1(string2l(buf,strlen(buf),&v) == 0);

//     redis_strlcpy(buf,"-1",sizeof(buf));
//     ASSERT1(string2l(buf,strlen(buf),&v) == 1);
//     ASSERT1(v == -1);

//     redis_strlcpy(buf,"0",sizeof(buf));
//     ASSERT1(string2l(buf,strlen(buf),&v) == 1);
//     ASSERT1(v == 0);

//     redis_strlcpy(buf,"1",sizeof(buf));
//     ASSERT1(string2l(buf,strlen(buf),&v) == 1);
//     ASSERT1(v == 1);

//     redis_strlcpy(buf,"99",sizeof(buf));
//     ASSERT1(string2l(buf,strlen(buf),&v) == 1);
//     ASSERT1(v == 99);

//     redis_strlcpy(buf,"-99",sizeof(buf));
//     ASSERT1(string2l(buf,strlen(buf),&v) == 1);
//     ASSERT1(v == -99);

// #if LONG_MAX != LLONG_MAX
//     redis_strlcpy(buf,"-2147483648",sizeof(buf));
//     ASSERT1(string2l(buf,strlen(buf),&v) == 1);
//     ASSERT1(v == LONG_MIN);

//     redis_strlcpy(buf,"-2147483649",sizeof(buf)); /* overflow */
//     ASSERT1(string2l(buf,strlen(buf),&v) == 0);

//     redis_strlcpy(buf,"2147483647",sizeof(buf));
//     ASSERT1(string2l(buf,strlen(buf),&v) == 1);
//     ASSERT1(v == LONG_MAX);

//     redis_strlcpy(buf,"2147483648",sizeof(buf)); /* overflow */
//     ASSERT1(string2l(buf,strlen(buf),&v) == 0);
// #endif
// }

// static void test_ll2string(void) {
//     char buf[32];
//     long long v;
//     int sz;

//     v = 0;
//     sz = ll2string(buf, sizeof buf, v);
//     ASSERT1(sz == 1);
//     ASSERT1(!strcmp(buf, "0"));

//     v = -1;
//     sz = ll2string(buf, sizeof buf, v);
//     ASSERT1(sz == 2);
//     ASSERT1(!strcmp(buf, "-1"));

//     v = 99;
//     sz = ll2string(buf, sizeof buf, v);
//     ASSERT1(sz == 2);
//     ASSERT1(!strcmp(buf, "99"));

//     v = -99;
//     sz = ll2string(buf, sizeof buf, v);
//     ASSERT1(sz == 3);
//     ASSERT1(!strcmp(buf, "-99"));

//     v = -2147483648;
//     sz = ll2string(buf, sizeof buf, v);
//     ASSERT1(sz == 11);
//     ASSERT1(!strcmp(buf, "-2147483648"));

//     v = LLONG_MIN;
//     sz = ll2string(buf, sizeof buf, v);
//     ASSERT1(sz == 20);
//     ASSERT1(!strcmp(buf, "-9223372036854775808"));

//     v = LLONG_MAX;
//     sz = ll2string(buf, sizeof buf, v);
//     ASSERT1(sz == 19);
//     ASSERT1(!strcmp(buf, "9223372036854775807"));
// }

// static void test_ld2string(void) {
//     char buf[32];
//     long double v;
//     int sz;

//     v = 0.0 / 0.0;
//     sz = ld2string(buf, sizeof(buf), v, LD_STR_AUTO);
//     ASSERT1(sz == 3);
//     ASSERT1(!strcmp(buf, "nan"));
// }

// static void test_fixedpoint_d2string(void) {
//     char buf[32];
//     double v;
//     int sz;
//     v = 0.0;
//     sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
//     ASSERT1(sz == 6);
//     ASSERT1(!strcmp(buf, "0.0000"));
//     sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
//     ASSERT1(sz == 3);
//     ASSERT1(!strcmp(buf, "0.0"));
//     /* set junk in buffer */
//     memset(buf,'A',32);
//     v = 0.0001;
//     sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
//     ASSERT1(sz == 6);
//     ASSERT1(buf[sz] == '\0');
//     ASSERT1(!strcmp(buf, "0.0001"));
//     /* set junk in buffer */
//     memset(buf,'A',32);
//     v = 6.0642951598391699e-05;
//     sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
//     ASSERT1(sz == 6);
//     ASSERT1(buf[sz] == '\0');
//     ASSERT1(!strcmp(buf, "0.0001"));
//     v = 0.01;
//     sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
//     ASSERT1(sz == 6);
//     ASSERT1(!strcmp(buf, "0.0100"));
//     sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
//     ASSERT1(sz == 3);
//     ASSERT1(!strcmp(buf, "0.0"));
//     v = -0.01;
//     sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
//     ASSERT1(sz == 7);
//     ASSERT1(!strcmp(buf, "-0.0100"));
//      v = -0.1;
//     sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
//     ASSERT1(sz == 4);
//     ASSERT1(!strcmp(buf, "-0.1"));
//     v = 0.1;
//     sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
//     ASSERT1(sz == 3);
//     ASSERT1(!strcmp(buf, "0.1"));
//     v = 0.01;
//     sz = fixedpoint_d2string(buf, sizeof buf, v, 17);
//     ASSERT1(sz == 19);
//     ASSERT1(!strcmp(buf, "0.01000000000000000"));
//     v = 10.01;
//     sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
//     ASSERT1(sz == 7);
//     ASSERT1(!strcmp(buf, "10.0100"));
//     /* negative tests */
//     sz = fixedpoint_d2string(buf, sizeof buf, v, 18);
//     ASSERT1(sz == 0);
//     sz = fixedpoint_d2string(buf, sizeof buf, v, 0);
//     ASSERT1(sz == 0);
//     sz = fixedpoint_d2string(buf, 1, v, 1);
//     ASSERT1(sz == 0);
// }

// #if defined(__linux__)
// /* Since fadvise and mincore is only supported in specific platforms like
//  * Linux, we only verify the fadvise mechanism works in Linux */
// static int cache_exist(int fd) {
//     unsigned char flag;
//     void *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
//     ASSERT1(m);
//     ASSERT1(mincore(m, 4096, &flag) == 0);
//     munmap(m, 4096);
//     /* the least significant bit of the byte will be set if the corresponding
//      * page is currently resident in memory */
//     return flag&1;
// }

// static void test_reclaimFilePageCache(void) {
//     char *tmpfile = "/tmp/redis-reclaim-cache-test";
//     int fd = open(tmpfile, O_RDWR|O_CREAT, 0644);
//     ASSERT1(fd >= 0);

//     /* test write file */
//     char buf[4] = "foo";
//     ASSERT1(write(fd, buf, sizeof(buf)) > 0);
//     ASSERT1(cache_exist(fd));
//     ASSERT1(redis_fsync(fd) == 0);
//     ASSERT1(reclaimFilePageCache(fd, 0, 0) == 0);
//     ASSERT1(!cache_exist(fd));

//     /* test read file */
//     ASSERT1(pread(fd, buf, sizeof(buf), 0) > 0);
//     ASSERT1(cache_exist(fd));
//     ASSERT1(reclaimFilePageCache(fd, 0, 0) == 0);
//     ASSERT1(!cache_exist(fd));

//     unlink(tmpfile);
//     printf("reclaimFilePageCach test is ok\n");
// }
// #endif

// int utilTest(int argc, char **argv, int flags) {
//     UNUSED(argc);
//     UNUSED(argv);
//     UNUSED(flags);

//     test_string2ll();
//     test_string2l();
//     test_ll2string();
//     test_ld2string();
//     test_fixedpoint_d2string();
// #if defined(__linux__)
//     if (!(flags & REDIS_TEST_VALGRIND)) {
//         test_reclaimFilePageCache();
//     }
// #endif
//     printf("Done testing util\n");
//     return 0;
// }
// #endif
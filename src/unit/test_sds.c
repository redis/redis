#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "testhelp.h"
#include "../sds.h"

static sds sdsTestTemplateCallback(sds varname, void *arg) {
    UNUSED(arg);
    static const char *_var1 = "variable1";
    static const char *_var2 = "variable2";

    if (!strcmp(varname, _var1)) return sdsnew("value1");
    else if (!strcmp(varname, _var2)) return sdsnew("value2");
    else return NULL;
}

int sdsTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    TEST("sdsnew() and sdsfree()") {
        sds x = sdsnew("foo"), y;

        ASSERT("Create a string and obtain the length",
            sdslen(x) == 3 && memcmp(x,"foo\0",4) == 0);

        sdsfree(x);
        x = sdsnewlen("foo",2);
        ASSERT("Create a string with specified length",
            sdslen(x) == 2 && memcmp(x,"fo\0",3) == 0);

        x = sdscat(x,"bar");
        ASSERT("Strings concatenation",
            sdslen(x) == 5 && memcmp(x,"fobar\0",6) == 0);

        x = sdscpy(x,"a");
        ASSERT("sdscpy() against an originally longer string",
            sdslen(x) == 1 && memcmp(x,"a\0",2) == 0);

        x = sdscpy(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk");
        ASSERT("sdscpy() against an originally shorter string",
            sdslen(x) == 33 &&
            memcmp(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk\0",33) == 0);

        sdsfree(x);
        x = sdscatprintf(sdsempty(),"%d",123);
        ASSERT("sdscatprintf() seems working in the base case",
            sdslen(x) == 3 && memcmp(x,"123\0",4) == 0);

        sdsfree(x);
        x = sdscatprintf(sdsempty(),"a%cb",0);
        ASSERT("sdscatprintf() seems working with \\0 inside of result",
            sdslen(x) == 3 && memcmp(x,"a\0""b\0",4) == 0);

        {
            sdsfree(x);
            char etalon[1024*1024];
            for (size_t i = 0; i < sizeof(etalon); i++) {
                etalon[i] = '0';
            }
            x = sdscatprintf(sdsempty(),"%0*d",(int)sizeof(etalon),0);
            ASSERT("sdscatprintf() can print 1MB",
                sdslen(x) == sizeof(etalon) && memcmp(x,etalon,sizeof(etalon)) == 0);
        }

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "Hello %s World %I,%I--", "Hi!", LLONG_MIN,LLONG_MAX);
        ASSERT("sdscatfmt() seems working in the base case",
            sdslen(x) == 60 &&
            memcmp(x,"--Hello Hi! World -9223372036854775808,"
                     "9223372036854775807--",60) == 0);
        printf("[%s]\n",x);

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "%u,%U--", UINT_MAX, ULLONG_MAX);
        ASSERT("sdscatfmt() seems working with unsigned numbers",
            sdslen(x) == 35 &&
            memcmp(x,"--4294967295,18446744073709551615--",35) == 0);

        sdsfree(x);
        x = sdsnew(" x ");
        sdstrim(x," x");
        ASSERT("sdstrim() works when all chars match",
            sdslen(x) == 0);

        sdsfree(x);
        x = sdsnew(" x ");
        sdstrim(x," ");
        ASSERT("sdstrim() works when a single char remains",
            sdslen(x) == 1 && x[0] == 'x');

        sdsfree(x);
        x = sdsnew("xxciaoyyy");
        sdstrim(x,"xy");
        ASSERT("sdstrim() correctly trims characters",
            sdslen(x) == 4 && memcmp(x,"ciao\0",5) == 0);

        y = sdsdup(x);
        sdsrange(y,1,1);
        ASSERT("sdsrange(...,1,1)",
            sdslen(y) == 1 && memcmp(y,"i\0",2) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,1,-1);
        ASSERT("sdsrange(...,1,-1)",
            sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,-2,-1);
        ASSERT("sdsrange(...,-2,-1)",
            sdslen(y) == 2 && memcmp(y,"ao\0",3) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,2,1);
        ASSERT("sdsrange(...,2,1)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,1,100);
        ASSERT("sdsrange(...,1,100)",
            sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,100,100);
        ASSERT("sdsrange(...,100,100)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,4,6);
        ASSERT("sdsrange(...,4,6)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,3,6);
        ASSERT("sdsrange(...,3,6)",
            sdslen(y) == 1 && memcmp(y,"o\0",2) == 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("foo");
        y = sdsnew("foa");
        ASSERT("sdscmp(foo,foa)", sdscmp(x,y) > 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("bar");
        y = sdsnew("bar");
        ASSERT("sdscmp(bar,bar)", sdscmp(x,y) == 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("aar");
        y = sdsnew("bar");
        ASSERT("sdscmp(bar,bar)", sdscmp(x,y) < 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnewlen("\a\n\0foo\r",7);
        y = sdscatrepr(sdsempty(),x,sdslen(x));
        ASSERT("sdscatrepr(...data...)",
            memcmp(y,"\"\\a\\n\\x00foo\\r\"",15) == 0);

        {
            unsigned int oldfree;
            char *p;
            int i;
            size_t step = 10, j;

            sdsfree(x);
            sdsfree(y);
            x = sdsnew("0");
            ASSERT("sdsnew() free/len buffers", sdslen(x) == 1 && sdsavail(x) == 0);

            /* Run the test a few times in order to hit the first two
             * SDS header types. */
            for (i = 0; i < 10; i++) {
                size_t oldlen = sdslen(x);
                x = sdsMakeRoomFor(x,step);
                int type = x[-1]&SDS_TYPE_MASK;

                ASSERT("sdsMakeRoomFor() len", sdslen(x) == oldlen);
                if (type != SDS_TYPE_5) {
                    ASSERT("sdsMakeRoomFor() free", sdsavail(x) >= step);
                    oldfree = sdsavail(x);
                    UNUSED(oldfree);
                }
                p = x+oldlen;
                for (j = 0; j < step; j++) {
                    p[j] = 'A'+j;
                }
                sdsIncrLen(x,step);
            }
            ASSERT("sdsMakeRoomFor() content",
                memcmp("0ABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJ",x,101) == 0);
            ASSERT("sdsMakeRoomFor() final length",sdslen(x)==101);

            sdsfree(x);
        }

        TEST("Test sds templates") {
            /* Simple template */
            x = sdstemplate("v1={variable1} v2={variable2}", sdsTestTemplateCallback, NULL);
            ASSERT("sdstemplate() normal flow",
                    memcmp(x,"v1=value1 v2=value2",19) == 0);
            sdsfree(x);

            /* Template with callback error */
            x = sdstemplate("v1={variable1} v3={doesnotexist}", sdsTestTemplateCallback, NULL);
            ASSERT("sdstemplate() with callback error", x == NULL);

            /* Template with empty var name */
            x = sdstemplate("v1={", sdsTestTemplateCallback, NULL);
            ASSERT("sdstemplate() with empty var name", x == NULL);

            /* Template with truncated var name */
            x = sdstemplate("v1={start", sdsTestTemplateCallback, NULL);
            ASSERT("sdstemplate() with truncated var name", x == NULL);

            /* Template with quoting */
            x = sdstemplate("v1={{{variable1}} {{} v2={variable2}", sdsTestTemplateCallback, NULL);
            ASSERT("sdstemplate() with quoting",
                    memcmp(x,"v1={value1} {} v2=value2",24) == 0);
            sdsfree(x);
        }

        TEST("sdsresize") {
            /* Test sdsresize - extend */
            x = sdsnew("1234567890123456789012345678901234567890");
            x = sdsResize(x, 200, 1);
            ASSERT("sdsrezie() expand len", sdslen(x) == 40);
            ASSERT("sdsrezie() expand strlen", strlen(x) == 40);
            ASSERT("sdsrezie() expand alloc", sdsalloc(x) == 200);
            /* Test sdsresize - trim free space */
            x = sdsResize(x, 80, 1);
            ASSERT("sdsrezie() shrink len", sdslen(x) == 40);
            ASSERT("sdsrezie() shrink strlen", strlen(x) == 40);
            ASSERT("sdsrezie() shrink alloc", sdsalloc(x) == 80);
            /* Test sdsresize - crop used space */
            x = sdsResize(x, 30, 1);
            ASSERT("sdsrezie() crop len", sdslen(x) == 30);
            ASSERT("sdsrezie() crop strlen", strlen(x) == 30);
            ASSERT("sdsrezie() crop alloc", sdsalloc(x) == 30);
            /* Test sdsresize - extend to different class */
            x = sdsResize(x, 400, 1);
            ASSERT("sdsrezie() expand len", sdslen(x) == 30);
            ASSERT("sdsrezie() expand strlen", strlen(x) == 30);
            ASSERT("sdsrezie() expand alloc", sdsalloc(x) == 400);
            /* Test sdsresize - shrink to different class */
            x = sdsResize(x, 4, 1);
            ASSERT("sdsrezie() crop len", sdslen(x) == 4);
            ASSERT("sdsrezie() crop strlen", strlen(x) == 4);
            ASSERT("sdsrezie() crop alloc", sdsalloc(x) == 4);
            sdsfree(x);
        }

    }
    return 0;
}

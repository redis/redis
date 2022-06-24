#include <sys/time.h>
#include <time.h>

#include "../intset.c"
#include "testhelp.h"

#if 0
static void intsetRepr(intset *is) {
    for (uint32_t i = 0; i < intrev32ifbe(is->length); i++) {
        printf("%lld\n", (uint64_t)_intsetGet(is,i));
    }
    printf("\n");
}

static void error(char *err) {
    printf("%s\n", err);
    exit(1);
}
#endif

static void ok(void) {
    printf("OK\n");
}

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

static intset *createSet(int bits, int size) {
    uint64_t mask = (1<<bits)-1;
    uint64_t value;
    intset *is = intsetNew();

    for (int i = 0; i < size; i++) {
        if (bits > 32) {
            value = (rand()*rand()) & mask;
        } else {
            value = rand() & mask;
        }
        is = intsetAdd(is,value,NULL);
    }
    return is;
}

static void checkConsistency(intset *is) {
    for (uint32_t i = 0; i < (intrev32ifbe(is->length)-1); i++) {
        uint32_t encoding = intrev32ifbe(is->encoding);

        if (encoding == INTSET_ENC_INT16) {
            int16_t *i16 = (int16_t*)is->contents;
            ASSERT1(i16[i] < i16[i+1]);
        } else if (encoding == INTSET_ENC_INT32) {
            int32_t *i32 = (int32_t*)is->contents;
            ASSERT1(i32[i] < i32[i+1]);
        } else {
            int64_t *i64 = (int64_t*)is->contents;
            ASSERT1(i64[i] < i64[i+1]);
        }
    }
}

#define UNUSED(x) (void)(x)
int intsetTest(int argc, char **argv, int flags) {
    uint8_t success;
    int i;
    intset *is;
    srand(time(NULL));

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    printf("Value encodings: "); {
        ASSERT1(_intsetValueEncoding(-32768) == INTSET_ENC_INT16);
        ASSERT1(_intsetValueEncoding(+32767) == INTSET_ENC_INT16);
        ASSERT1(_intsetValueEncoding(-32769) == INTSET_ENC_INT32);
        ASSERT1(_intsetValueEncoding(+32768) == INTSET_ENC_INT32);
        ASSERT1(_intsetValueEncoding(-2147483648) == INTSET_ENC_INT32);
        ASSERT1(_intsetValueEncoding(+2147483647) == INTSET_ENC_INT32);
        ASSERT1(_intsetValueEncoding(-2147483649) == INTSET_ENC_INT64);
        ASSERT1(_intsetValueEncoding(+2147483648) == INTSET_ENC_INT64);
        ASSERT1(_intsetValueEncoding(-9223372036854775808ull) ==
                    INTSET_ENC_INT64);
        ASSERT1(_intsetValueEncoding(+9223372036854775807ull) ==
                    INTSET_ENC_INT64);
        ok();
    }

    printf("Basic adding: "); {
        is = intsetNew();
        is = intsetAdd(is,5,&success); ASSERT1(success);
        is = intsetAdd(is,6,&success); ASSERT1(success);
        is = intsetAdd(is,4,&success); ASSERT1(success);
        is = intsetAdd(is,4,&success); ASSERT1(!success);
        ASSERT1(6 == intsetMax(is));
        ASSERT1(4 == intsetMin(is));
        ok();
        zfree(is);
    }

    printf("Large number of random adds: "); {
        uint32_t inserts = 0;
        is = intsetNew();
        for (i = 0; i < 1024; i++) {
            is = intsetAdd(is,rand()%0x800,&success);
            if (success) inserts++;
        }
        ASSERT1(intrev32ifbe(is->length) == inserts);
        checkConsistency(is);
        ok();
        zfree(is);
    }

    printf("Upgrade from int16 to int32: "); {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        ASSERT1(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,65535,NULL);
        ASSERT1(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        ASSERT1(intsetFind(is,32));
        ASSERT1(intsetFind(is,65535));
        checkConsistency(is);
        zfree(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        ASSERT1(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,-65535,NULL);
        ASSERT1(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        ASSERT1(intsetFind(is,32));
        ASSERT1(intsetFind(is,-65535));
        checkConsistency(is);
        ok();
        zfree(is);
    }

    printf("Upgrade from int16 to int64: "); {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        ASSERT1(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,4294967295,NULL);
        ASSERT1(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        ASSERT1(intsetFind(is,32));
        ASSERT1(intsetFind(is,4294967295));
        checkConsistency(is);
        zfree(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        ASSERT1(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,-4294967295,NULL);
        ASSERT1(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        ASSERT1(intsetFind(is,32));
        ASSERT1(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
        zfree(is);
    }

    printf("Upgrade from int32 to int64: "); {
        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        ASSERT1(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        is = intsetAdd(is,4294967295,NULL);
        ASSERT1(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        ASSERT1(intsetFind(is,65535));
        ASSERT1(intsetFind(is,4294967295));
        checkConsistency(is);
        zfree(is);

        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        ASSERT1(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        is = intsetAdd(is,-4294967295,NULL);
        ASSERT1(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        ASSERT1(intsetFind(is,65535));
        ASSERT1(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
        zfree(is);
    }

    printf("Stress lookups: "); {
        long num = 100000, size = 10000;
        int i, bits = 20;
        long long start;
        is = createSet(bits,size);
        checkConsistency(is);

        start = usec();
        for (i = 0; i < num; i++) intsetSearch(is,rand() % ((1<<bits)-1),NULL);
        printf("%ld lookups, %ld element set, %lldusec\n",
               num,size,usec()-start);
        zfree(is);
    }

    printf("Stress add+delete: "); {
        int i, v1, v2;
        is = intsetNew();
        for (i = 0; i < 0xffff; i++) {
            v1 = rand() % 0xfff;
            is = intsetAdd(is,v1,NULL);
            ASSERT1(intsetFind(is,v1));

            v2 = rand() % 0xfff;
            is = intsetRemove(is,v2,NULL);
            ASSERT1(!intsetFind(is,v2));
        }
        checkConsistency(is);
        ok();
        zfree(is);
    }

    return 0;
}

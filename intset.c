#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "intset.h"
#include "zmalloc.h"

/* Note that these encodings are ordered, so:
 * INTSET_ENC_INT16 < INTSET_ENC_INT32 < INTSET_ENC_INT64. */
#define INTSET_ENC_INT16 (sizeof(int16_t))
#define INTSET_ENC_INT32 (sizeof(int32_t))
#define INTSET_ENC_INT64 (sizeof(int64_t))

/* Accessors for each type of encoding */
#define INTSET_VALUE_ENCODING(__val) (((__val) < INT32_MIN || (__val) > INT32_MAX) ? \
    INTSET_ENC_INT64 : (((__val) < INT16_MIN || (__val) > INT16_MAX) ? \
    INTSET_ENC_INT32 : INTSET_ENC_INT16))
#define INTSET_GET_ENCODED(__is,__pos,__enc) ((__enc == INTSET_ENC_INT64) ? \
    ((int64_t*)(__is)->contents)[__pos] : ((__enc == INTSET_ENC_INT32) ? \
    ((int32_t*)(__is)->contents)[__pos] : ((int16_t*)(__is)->contents)[__pos]))
#define INTSET_GET(__is,__pos) (INTSET_GET_ENCODED(__is,__pos,(__is)->encoding))
#define INTSET_SET(__is,__pos,__val) { \
    if ((__is)->encoding == INTSET_ENC_INT64) \
        ((int64_t*)(__is)->contents)[__pos] = (__val); \
    else if ((__is)->encoding == INTSET_ENC_INT32) \
        ((int32_t*)(__is)->contents)[__pos] = (__val); \
    else \
        ((int16_t*)(__is)->contents)[__pos] = (__val); }

/* Create an empty intset. */
intset *intsetNew(void) {
    intset *is = zmalloc(sizeof(intset));
    is->encoding = INTSET_ENC_INT16;
    is->length = 0;
    return is;
}

/* Resize the intset */
static intset *intsetResize(intset *is, uint32_t len) {
    uint32_t size = len*is->encoding;
    is = zrealloc(is,sizeof(intset)+size);
    return is;
}

static intset *intsetUpgrade(intset *is, uint8_t newenc, uint8_t extra, uint8_t offset) {
    uint8_t curenc = is->encoding;
    int length = is->length;

    /* First set new encoding and resize */
    is->encoding = newenc;
    is = intsetResize(is,is->length+extra);

    /* Upgrade back-to-front so we don't overwrite values */
    while(length--)
        INTSET_SET(is,length+offset,INTSET_GET_ENCODED(is,length,curenc));
    return is;
}

/* Search for the position of "value". Return 1 when the value was found and
 * sets "pos" to the position of the value within the intset. Return 0 when
 * the value is not present in the intset and sets "pos" to the position
 * where "value" can be inserted. */
static uint8_t intsetSearch(intset *is, int64_t value, uint32_t *pos) {
    int min = 0, max = is->length-1, mid = -1;
    int64_t cur = -1;

    /* The value can never be found when the set is empty */
    if (is->length == 0) {
        if (pos) *pos = 0;
        return 0;
    } else {
        /* Check for the case where we know we cannot find the value,
         * but do know the insert position. */
        if (value > INTSET_GET(is,is->length-1)) {
            if (pos) *pos = is->length;
            return 0;
        } else if (value < INTSET_GET(is,0)) {
            if (pos) *pos = 0;
            return 0;
        }
    }

    while(max >= min) {
        mid = (min+max)/2;
        cur = INTSET_GET(is,mid);
        if (value > cur) {
            min = mid+1;
        } else if (value < cur) {
            max = mid-1;
        } else {
            break;
        }
    }

    if (value == cur) {
        if (pos) *pos = mid;
        return 1;
    } else {
        if (pos) *pos = min;
        return 0;
    }
}

static void intsetMoveTail(intset *is, uint32_t from, uint32_t to) {
    void *src, *dst;
    uint32_t bytes = is->length-from;
    if (is->encoding == INTSET_ENC_INT64) {
        src = (int64_t*)is->contents+from;
        dst = (int64_t*)is->contents+to;
        bytes *= sizeof(int64_t);
    } else if (is->encoding == INTSET_ENC_INT32) {
        src = (int32_t*)is->contents+from;
        dst = (int32_t*)is->contents+to;
        bytes *= sizeof(int32_t);
    } else {
        src = (int16_t*)is->contents+from;
        dst = (int16_t*)is->contents+to;
        bytes *= sizeof(int16_t);
    }
    memmove(dst,src,bytes);
}

/* Insert an integer in the intset */
intset *intsetAdd(intset *is, int64_t value, uint8_t *success) {
    uint8_t valenc = INTSET_VALUE_ENCODING(value);
    uint32_t pos, offset;
    if (success) *success = 1;

    /* Upgrade encoding if necessary. If we need to upgrade, we know that
     * this value should be either appended (if > 0) or prepended (if < 0),
     * because it lies outside the range of existing values. */
    if (valenc > is->encoding) {
        offset = value < 0 ? 1 : 0;
        is = intsetUpgrade(is,valenc,1,offset);
        pos = (value < 0) ? 0 : is->length;
    } else {
        /* Abort if the value is already present in the set.
         * This call will populate "pos" with the right position to insert
         * the value when it cannot be found. */
        if (intsetSearch(is,value,&pos)) {
            if (success) *success = 0;
            return is;
        }

        is = intsetResize(is,is->length+1);
        if (pos < is->length) intsetMoveTail(is,pos,pos+1);
    }

    INTSET_SET(is,pos,value);
    is->length++;
    return is;
}

/* Delete integer from intset */
intset *intsetRemove(intset *is, int64_t value, uint8_t *success) {
    uint8_t valenc = INTSET_VALUE_ENCODING(value);
    uint32_t pos;
    if (success) *success = 0;

    if (valenc <= is->encoding && intsetSearch(is,value,&pos)) {
        /* We know we can delete */
        if (success) *success = 1;

        /* Overwrite value with tail and update length */
        if (pos < (is->length-1)) intsetMoveTail(is,pos+1,pos);
        is = intsetResize(is,is->length-1);
        is->length--;
    }
    return is;
}

/* Determine whether a value belongs to this set */
uint8_t intsetFind(intset *is, int64_t value) {
    uint8_t valenc = INTSET_VALUE_ENCODING(value);
    return valenc <= is->encoding && intsetSearch(is,value,NULL);
}

/* Return random member */
int64_t intsetRandom(intset *is) {
    return INTSET_GET(is,rand()%is->length);
}

/* Sets the value to the value at the given position. When this position is
 * out of range the function returns 0, when in range it returns 1. */
uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value) {
    if (pos < is->length) {
        *value = INTSET_GET(is,pos);
        return 1;
    }
    return 0;
}

/* Return intset length */
uint32_t intsetLen(intset *is) {
    return is->length;
}

#ifdef INTSET_TEST_MAIN
#include <sys/time.h>

void intsetRepr(intset *is) {
    int i;
    for (i = 0; i < is->length; i++) {
        printf("%lld\n", (uint64_t)INTSET_GET(is,i));
    }
    printf("\n");
}

void error(char *err) {
    printf("%s\n", err);
    exit(1);
}

void ok(void) {
    printf("OK\n");
}

long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

#define assert(_e) ((_e)?(void)0:(_assert(#_e,__FILE__,__LINE__),exit(1)))
void _assert(char *estr, char *file, int line) {
    printf("\n\n=== ASSERTION FAILED ===\n");
    printf("==> %s:%d '%s' is not true\n",file,line,estr);
}

intset *createSet(int bits, int size) {
    uint64_t mask = (1<<bits)-1;
    uint64_t i, value;
    intset *is = intsetNew();

    for (i = 0; i < size; i++) {
        if (bits > 32) {
            value = (rand()*rand()) & mask;
        } else {
            value = rand() & mask;
        }
        is = intsetAdd(is,value,NULL);
    }
    return is;
}

void checkConsistency(intset *is) {
    int i;

    for (i = 0; i < (is->length-1); i++) {
        if (is->encoding == INTSET_ENC_INT16) {
            int16_t *i16 = (int16_t*)is->contents;
            assert(i16[i] < i16[i+1]);
        } else if (is->encoding == INTSET_ENC_INT32) {
            int32_t *i32 = (int32_t*)is->contents;
            assert(i32[i] < i32[i+1]);
        } else {
            int64_t *i64 = (int64_t*)is->contents;
            assert(i64[i] < i64[i+1]);
        }
    }
}

int main(int argc, char **argv) {
    uint8_t success;
    int i;
    intset *is;
    sranddev();

    printf("Value encodings: "); {
        assert(INTSET_VALUE_ENCODING(-32768) == INTSET_ENC_INT16);
        assert(INTSET_VALUE_ENCODING(+32767) == INTSET_ENC_INT16);
        assert(INTSET_VALUE_ENCODING(-32769) == INTSET_ENC_INT32);
        assert(INTSET_VALUE_ENCODING(+32768) == INTSET_ENC_INT32);
        assert(INTSET_VALUE_ENCODING(-2147483648) == INTSET_ENC_INT32);
        assert(INTSET_VALUE_ENCODING(+2147483647) == INTSET_ENC_INT32);
        assert(INTSET_VALUE_ENCODING(-2147483649) == INTSET_ENC_INT64);
        assert(INTSET_VALUE_ENCODING(+2147483648) == INTSET_ENC_INT64);
        assert(INTSET_VALUE_ENCODING(-9223372036854775808ull) == INTSET_ENC_INT64);
        assert(INTSET_VALUE_ENCODING(+9223372036854775807ull) == INTSET_ENC_INT64);
        ok();
    }

    printf("Basic adding: "); {
        is = intsetNew();
        is = intsetAdd(is,5,&success); assert(success);
        is = intsetAdd(is,6,&success); assert(success);
        is = intsetAdd(is,4,&success); assert(success);
        is = intsetAdd(is,4,&success); assert(!success);
        ok();
    }

    printf("Large number of random adds: "); {
        int inserts = 0;
        is = intsetNew();
        for (i = 0; i < 1024; i++) {
            is = intsetAdd(is,rand()%0x800,&success);
            if (success) inserts++;
        }
        assert(is->length == inserts);
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int16 to int32: "); {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(is->encoding == INTSET_ENC_INT16);
        is = intsetAdd(is,65535,NULL);
        assert(is->encoding == INTSET_ENC_INT32);
        assert(intsetFind(is,32));
        assert(intsetFind(is,65535));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(is->encoding == INTSET_ENC_INT16);
        is = intsetAdd(is,-65535,NULL);
        assert(is->encoding == INTSET_ENC_INT32);
        assert(intsetFind(is,32));
        assert(intsetFind(is,-65535));
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int16 to int64: "); {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(is->encoding == INTSET_ENC_INT16);
        is = intsetAdd(is,4294967295,NULL);
        assert(is->encoding == INTSET_ENC_INT64);
        assert(intsetFind(is,32));
        assert(intsetFind(is,4294967295));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(is->encoding == INTSET_ENC_INT16);
        is = intsetAdd(is,-4294967295,NULL);
        assert(is->encoding == INTSET_ENC_INT64);
        assert(intsetFind(is,32));
        assert(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int32 to int64: "); {
        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        assert(is->encoding == INTSET_ENC_INT32);
        is = intsetAdd(is,4294967295,NULL);
        assert(is->encoding == INTSET_ENC_INT64);
        assert(intsetFind(is,65535));
        assert(intsetFind(is,4294967295));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        assert(is->encoding == INTSET_ENC_INT32);
        is = intsetAdd(is,-4294967295,NULL);
        assert(is->encoding == INTSET_ENC_INT64);
        assert(intsetFind(is,65535));
        assert(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
    }

    printf("Stress lookups: "); {
        long num = 100000, size = 10000;
        int i, bits = 20;
        long long start;
        is = createSet(bits,size);
        checkConsistency(is);

        start = usec();
        for (i = 0; i < num; i++) intsetSearch(is,rand() % ((1<<bits)-1),NULL);
        printf("%ld lookups, %ld element set, %lldusec\n",num,size,usec()-start);
    }

    printf("Stress add+delete: "); {
        int i, v1, v2;
        is = intsetNew();
        for (i = 0; i < 0xffff; i++) {
            v1 = rand() % 0xfff;
            is = intsetAdd(is,v1,NULL);
            assert(intsetFind(is,v1));

            v2 = rand() % 0xfff;
            is = intsetRemove(is,v2,NULL);
            assert(!intsetFind(is,v2));
        }
        checkConsistency(is);
        ok();
    }
}
#endif

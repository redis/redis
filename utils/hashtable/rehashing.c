#include "redis.h"
#include "dict.h"

void _redisAssert(char *x, char *y, int l) {
    printf("ASSERT: %s %s %d\n",x,y,l);
    exit(1);
}

unsigned int dictKeyHash(const void *keyp) {
    unsigned long key = (unsigned long)keyp;
    key = dictGenHashFunction(&key,sizeof(key));
    key += ~(key << 15);
    key ^=  (key >> 10);
    key +=  (key << 3);
    key ^=  (key >> 6);
    key += ~(key << 11);
    key ^=  (key >> 16);
    return key;
}

int dictKeyCompare(void *privdata, const void *key1, const void *key2) {
    unsigned long k1 = (unsigned long)key1;
    unsigned long k2 = (unsigned long)key2;
    return k1 == k2;
}

dictType dictTypeTest = {
    dictKeyHash,                   /* hash function */
    NULL,                          /* key dup */
    NULL,                          /* val dup */
    dictKeyCompare,                /* key compare */
    NULL,                          /* key destructor */
    NULL,                          /* val destructor */
    NULL                           /* allow to expand */
};

void showBuckets(dictht ht) {
    if (ht.table == NULL) {
        printf("NULL\n");
    } else {
        int j;
        for (j = 0; j < ht.size; j++) {
            printf("%c", ht.table[j] ? '1' : '0');
        }
        printf("\n");
    }
}

void show(dict *d) {
    int j;
    if (d->rehashidx != -1) {
        printf("rhidx: ");
        for (j = 0; j < d->rehashidx; j++)
            printf(".");
        printf("|\n");
    }
    printf("ht[0]: ");
    showBuckets(d->ht[0]);
    printf("ht[1]: ");
    showBuckets(d->ht[1]);
    printf("\n");
}

int sortPointers(const void *a, const void *b) {
    unsigned long la, lb;

    la = (long) (*((dictEntry**)a));
    lb = (long) (*((dictEntry**)b));
    return la-lb;
}

void stressGetKeys(dict *d, int times, int *perfect_run, int *approx_run) {
    int j;

    dictEntry **des = zmalloc(sizeof(dictEntry*)*dictSize(d));
    for (j = 0; j < times; j++) {
        int requested = rand() % (dictSize(d)+1);
        int returned = dictGetSomeKeys(d, des, requested);
        int dup = 0;

        qsort(des,returned,sizeof(dictEntry*),sortPointers);
        if (returned > 1) {
            int i;
            for (i = 0; i < returned-1; i++) {
                if (des[i] == des[i+1]) dup++;
            }
        }

        if (requested == returned && dup == 0) {
            (*perfect_run)++;
        } else {
            (*approx_run)++;
            printf("Requested, returned, duplicated: %d %d %d\n",
                requested, returned, dup);
        }
    }
    zfree(des);
}

#define MAX1 120
#define MAX2 1000
int main(void) {
    dict *d = dictCreate(&dictTypeTest,NULL);
    unsigned long i;
    srand(time(NULL));

    for (i = 0; i < MAX1; i++) {
        dictAdd(d,(void*)i,NULL);
        show(d);
    }
    printf("Size: %d\n", (int)dictSize(d));

    for (i = 0; i < MAX1; i++) {
        dictDelete(d,(void*)i);
        dictResize(d);
        show(d);
    }
    dictRelease(d);

    d = dictCreate(&dictTypeTest,NULL);

    printf("Stress testing dictGetSomeKeys\n");
    int perfect_run = 0, approx_run = 0;

    for (i = 0; i < MAX2; i++) {
        dictAdd(d,(void*)i,NULL);
        stressGetKeys(d,100,&perfect_run,&approx_run);
    }

    for (i = 0; i < MAX2; i++) {
        dictDelete(d,(void*)i);
        dictResize(d);
        stressGetKeys(d,100,&perfect_run,&approx_run);
    }

    printf("dictGetSomeKey, %d perfect runs, %d approximated runs\n",
        perfect_run, approx_run);

    dictRelease(d);

    printf("TEST PASSED!\n");
    return 0;
}

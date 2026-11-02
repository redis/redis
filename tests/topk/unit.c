/* Top-K algorithm regression workloads, derived from RedisBloom's unit demo.
 * Copyright (c) 2026-Present, Redis Ltd.; RSALv2/SSPLv1/AGPLv3. */
#include "topk.h"
#include <assert.h>
#include <stdio.h>
void _serverAssert(const char *e, const char *f, int line) {
    fprintf(stderr,"%s:%d: %s\n",f,line,e); abort();
}
void heapifyDown(HeapBucket *array, size_t len, size_t start);
static void add(TopK *t, const char *s, unsigned n) {
    zfree(TopK_Add(t,s,strlen(s),n));
}
int main(void) {
    HeapBucket heap[7]={{0}};
    const unsigned counts[]={5,51,12,2,21,18,8,14,35,19};
    for (unsigned i=0; i<sizeof(counts)/sizeof(*counts); i++) {
        heap[0].count=counts[i]; heapifyDown(heap,7,0);
        for (unsigned j=1; j<7; j++) assert(heap[(j-1)/2].count<=heap[j].count);
    }
    TopK *t=TopK_Create(3,100,3,.925);
    const char *items[]={"1","2","3","4","1","1","4","3","4","1","3","4","1","1","2","2","2"};
    for (unsigned i=0; i<sizeof(items)/sizeof(*items); i++) add(t,items[i],1);
    assert(TopK_Count(t,"1",1)>=6); TopK_Destroy(t);
    t=TopK_Create(3,5,3,.9);
    add(t,"1",1); add(t,"2",1); add(t,"3",1);
    add(t,"4",3); add(t,"5",1); add(t,"5",10); add(t,"1",5);
    add(t,"2",20); add(t,"3",30); add(t,"1",5); add(t,"5",10); add(t,"4",5); add(t,"4",15);
    HeapBucket *list=TopK_List(t);
    assert(list[0].count>=list[1].count && list[1].count>=list[2].count);
    zfree(list); TopK_Destroy(t);
    /* Distribution and decay workloads, deterministic input with a wide sketch. */
    srand(10); t=TopK_Create(31,1000,5,.925);
    char value[32];
    for (int pass=0; pass<30; pass++) for (int i=0; i<300; i+=20) {
        snprintf(value,sizeof(value),"%d",i); add(t,value,1);
    }
    for (int pass=0; pass<10; pass++) for (int i=0; i<300; i+=10) {
        snprintf(value,sizeof(value),"%d",i); add(t,value,1);
    }
    for (int i=0; i<300; i+=20) {
        snprintf(value,sizeof(value),"%d",i); assert(TopK_Query(t,value,strlen(value)));
    }
    for (int pass=0; pass<100; pass++) for (int i=0; i<300; i++) {
        snprintf(value,sizeof(value),"%d",i); add(t,value,1);
    }
    list=TopK_List(t);
    for (unsigned i=1; i<t->k; i++) assert(list[i-1].count>=list[i].count);
    zfree(list); TopK_Destroy(t);
    puts("Top-K heap, controlled, increment, distribution and decay workloads passed");
    return 0;
}

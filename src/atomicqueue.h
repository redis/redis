#ifndef atomicqueue_h
#define atomicqueue_h

#include "atomicvar.h"

typedef struct atomicqueue atomicqueue;

atomicqueue *atomicqueueCreate(unsigned capacity, unsigned elemsize);
int atomicqueueTryPush(atomicqueue *queue, void *value, int *was_empty);
int atomicqueueTryPop(atomicqueue *queue, void *value, int *was_full);

#ifdef REDIS_TEST
int atomicqueueTest(int argc, char *argv[], int flags);
#endif

#endif

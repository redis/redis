#ifndef atomicqueue_h
#define atomicqueue_h

#include "atomicvar.h"

typedef struct atomicqueue atomicqueue;

atomicqueue *atomicqueueCreate(unsigned capacity, unsigned elemsize);
int atomicqueueTryPush(atomicqueue *queue, void *value);
int atomicqueueTryPop(atomicqueue *queue, void *value);

#ifdef REDIS_TEST
int atomicqueueTest(int argc, char *argv[], int flags);
#endif

#endif

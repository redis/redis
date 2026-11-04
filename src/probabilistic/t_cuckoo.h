#ifndef REDIS_T_CUCKOO_H
#define REDIS_T_CUCKOO_H

#include "object.h"
#include "cuckoo.h"

/* Takes ownership of an initialized filter. */
robj *createCuckooObject(CuckooFilter *filter);
void freeCuckooObject(robj *o);
robj *cuckooDup(const robj *o);
size_t cuckooAllocSize(const robj *o);
void cuckooDismiss(robj *o);
unsigned long cuckooDefrag(robj *o, unsigned long cursor, size_t steps,
                          void *(*defrag)(void *));
void cuckooMixDigest(unsigned char *digest, const robj *o);

#ifdef REDIS_TEST
int cuckooObjectTest(int argc, char **argv, int flags);
#endif
#endif

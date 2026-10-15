#ifndef REDIS_T_BLOOM_H
#define REDIS_T_BLOOM_H

#include "object.h"
#include "sb.h"
#include "rio.h"

/* Takes ownership of an initialized chain. */
robj *createBloomObject(SBChain *chain);
robj *bloomDup(const robj *o);
size_t bloomAllocSize(const robj *o);
void bloomDismiss(robj *o);
unsigned long bloomDefrag(robj *o, unsigned long cursor, size_t steps,
                         void *(*defrag)(void *));
void bloomMixDigest(unsigned char *digest, const robj *o);
int bloomRewriteAof(rio *r, robj *key, robj *o, int dbid);

#ifdef REDIS_TEST
int bloomObjectTest(int argc, char **argv, int flags);
#endif
#endif

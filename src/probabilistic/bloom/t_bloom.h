#ifndef REDIS_T_BLOOM_H
#define REDIS_T_BLOOM_H

#include "object.h"
#include "sb.h"
#include "rio.h"

/* No module ID is stored in the native Bloom format. */
#define BLOOM_RDB_NATIVE_ID UINT64_C(0)

/* Takes ownership of an initialized chain. */
robj *createBloomObject(SBChain *chain);
robj *bloomDup(const robj *o);
size_t bloomAllocSize(const robj *o);
void bloomDismiss(robj *o);
unsigned long bloomDefrag(robj *o, unsigned long cursor, size_t steps,
                         void *(*defrag)(void *));
void bloomMixDigest(unsigned char *digest, const robj *o);
int bloomRewriteAof(rio *r, robj *key, robj *o, int dbid);
int bloomIsLegacyRdbType(uint64_t moduleid);
/* Distinguishes corrupt payloads from read/allocation failures for the RDB caller. */
robj *rdbLoadBloomObject(rio *rdb, uint64_t moduleid, int *is_corrupt);

#ifdef REDIS_TEST
int bloomObjectTest(int argc, char **argv, int flags);
#endif
#endif

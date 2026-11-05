#ifndef REDIS_T_CUCKOO_H
#define REDIS_T_CUCKOO_H

#include "object.h"
#include "cuckoo.h"
#include "rio.h"

#define CUCKOO_RDB_NATIVE_ID UINT64_C(0)

/* Takes ownership of an initialized filter. */
robj *createCuckooObject(CuckooFilter *filter);
void freeCuckooObject(robj *o);
robj *cuckooDup(const robj *o);
size_t cuckooAllocSize(const robj *o);
size_t cuckooLength(const robj *o);
size_t cuckooFreeEffort(const robj *o);
int cuckooDefragShouldDefer(const robj *o);
void cuckooLogDebugInfo(const robj *o);
void cuckooDismiss(robj *o);
unsigned long cuckooDefrag(robj *o, unsigned long cursor, size_t steps,
                          void *(*defrag)(void *));
void cuckooMixDigest(unsigned char *digest, const robj *o);

int cuckooValidateBucketSize(long long value, const char **err);
int cuckooValidateInitialSize(long long value, const char **err);

int cuckooIsLegacyRdbType(uint64_t moduleid);
ssize_t rdbSaveCuckooObject(rio *rdb, const robj *o);
robj *rdbLoadCuckooObject(rio *rdb, uint64_t moduleid, int *is_corrupt);
int cuckooRewriteAof(rio *r, robj *key, robj *o, int dbid);

#ifdef REDIS_TEST
int cuckooObjectTest(int argc, char **argv, int flags);
#endif
#endif

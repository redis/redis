/* Native Top-K engine dispatch. The tag belongs to the key, not the server.
 * Copyright (c) 2026-Present, Redis Ltd.; RSALv2/SSPLv1/AGPLv3. */
#ifndef REDIS_NATIVE_TOPK_H
#define REDIS_NATIVE_TOPK_H
#include "server.h"
#include "topk.h"
#include "chk.h"
#include "probabilistic.h"

#define TOPK_HEAVYKEEPER 0
#define TOPK_CUCKOO_HEAVYKEEPER 1
#define TOPK_CHK_DECAY 1.08

typedef struct nativeTopK {
    int algorithm;
    union {
        TopK *legacy;
        chkTopK *chk;
    } value;
} nativeTopK;

static inline int topkIsChk(const robj *o) {
    return ((nativeTopK *)o->ptr)->algorithm == TOPK_CUCKOO_HEAVYKEEPER;
}
static inline TopK *topkLegacy(const robj *o) {
    return ((nativeTopK *)o->ptr)->value.legacy;
}
static inline chkTopK *topkChk(const robj *o) {
    return ((nativeTopK *)o->ptr)->value.chk;
}

robj *createTopkChkObject(chkTopK *t);
void topkChkDefrag(chkTopK *t, void *(*defrag)(void *));
void topkChkDigest(unsigned char *digest, chkTopK *t);
chkTopK *topkChkDup(chkTopK *t);
ssize_t topkChkRdbSave(rio *rdb, chkTopK *t);
robj *topkChkRdbLoad(rio *rdb);
chkHeapBucket *topkChkFind(chkTopK *t, const sds item);
#endif

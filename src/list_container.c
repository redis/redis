#include "server.h"
#include "ziplist.h"
#include "listpack.h"

unsigned int _lpGet(unsigned char *p, unsigned char **sstr, unsigned int *slen, long long *lval) {
    int64_t vlen;
    *sstr = lpGet(p, &vlen, NULL);
    if (*sstr) {
        *slen = vlen;
    } else {
        *lval = vlen;
    }
    return 1;
}

unsigned char *_lpDelete(unsigned char *l, unsigned char **p) {
    return lpDelete(l, *p, p);
}

listContainerType listContainerZiplist = {
    ziplistLen,
    ziplistBlobLen,
    ziplistGet,
    ziplistIndex,
    ziplistNext,
    ziplistPrev,
    ziplistPushHead,
    ziplistPushTail,
    ziplistReplace,
    ziplistDelete,
    ziplistFind,
    ziplistRandomPair,
};

listContainerType listContainerListpack = {
    lpLength,
    lpBytes,
    _lpGet,
    lpSeek,
    lpNext,
    lpPrev,
    lpPushHead,
    lpPushTail,
    lpReplace,
    _lpDelete,
    lpFind,
    lpRandomPair,
};
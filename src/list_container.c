#include "server.h"
#include "ziplist.h"
#include "listpack.h"

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
//     lpLength,
//     lpBytes,
//     lpGet,
//     lpSeek,
//     lpNext,
//     lpPrev,
//     lpPushHead,
//     lpPushTail,
//     lpInsertBefore,
//     lpInsertAfter,
//     lpReplace,
//     lpDelete,
//     lpDeleteRange,
//     lpMerge,
//     lpCompare,
};
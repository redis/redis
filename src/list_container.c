#include "server.h"
#include "ziplist.h"
#include "listpack.h"

listContainerType listContainerZiplist = {
    LIST_CONTAINER_ZIPLIST,
    ziplistNew,
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
    LIST_CONTAINER_LISTPACK,
//     lpEmpty,
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
/* LRU */
#include "redis.h"

dictList* dlCreate () {
    dictList   *dl = zmalloc (sizeof (dictList));

    dl->first = dl->last = NULL;
    return dl;
}

/* This function is called from emptyDb, which removes all data from db,
 * so I don't need to worry about it here. */
void dlEmpty (dictList *dl) {
    dl->first = dl->last = NULL;
}

void dlFlushDb (dictList *dl, int dbid) {
    dictEntry   *de = dl->last,
                *deNext;

    while (de != NULL) {
        deNext = de->lruNext;
        if (de->dbid == dbid)
            dlDelete (dl, de);
        de = deNext;
    }
}

void dlAdd (dictList *dl, dictEntry *de) {
    de->lruNext = NULL;
    de->lruPrev = dl->first;
    if (dl->first) {
        dl->first->lruNext = de;
        dl->first = de;
    } else {
        dl->first = dl->last = de;
    }
}

void dlDelete (dictList *dl, dictEntry *de) {
    if (de->lruNext)
        de->lruNext->lruPrev = de->lruPrev;
    else
        dl->first = de->lruPrev;
    if (de->lruPrev)
        de->lruPrev->lruNext = de->lruNext;
    else
        dl->last = de->lruNext;
}

void dlTouch (dictList *dl, dictEntry *de) {
    dlDelete (dl, de);
    dlAdd (dl, de);
}
/* !LRU */

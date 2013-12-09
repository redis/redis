/* LRU */
#ifndef __DICTLIST_H
#define __DICTLIST_H

struct dictEntry;

typedef struct dictList {
    struct dictEntry *first, *last;
} dictList;

extern dictList *lruList;

dictList* dlCreate ();
void dlEmpty (dictList *dl);
void dlFlushDb (dictList *dl, int dbid);

#define dlGetLast(dl) ((dl)->last)

void dlAdd (dictList *dl, struct dictEntry *de);
void dlDelete (dictList *dl, struct dictEntry *de);
void dlTouch (dictList *dl, struct dictEntry *de);

#endif /* __DICTLIST_H */
/* !LRU */
#ifndef __INTSET_H
#define __INTSET_H

typedef struct intset {
    uint32_t encoding;
    uint32_t length;
    int8_t contents[];
} intset;

intset *intsetNew(void);
intset *intsetAdd(intset *is, int64_t value, uint8_t *success);
intset *intsetDelete(intset *is, int64_t value, uint8_t *success);
uint8_t intsetFind(intset *is, int64_t value);
int64_t intsetRandom(intset *is);

#endif // __INTSET_H

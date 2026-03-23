#ifndef HYPERLOGLOG_H
#define HYPERLOGLOG_H

#include <stdint.h>
#include <stddef.h>
#include "object.h"

typedef struct hllhdr hllhdr;

robj *createHLLObject(void);
int hllAdd(robj *o, unsigned char *ele, size_t elesize);
uint64_t hllCount(struct hllhdr *hdr, int *invalid);

#endif /* HYPERLOGLOG_H */

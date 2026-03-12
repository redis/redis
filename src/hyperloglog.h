#ifndef HYPERLOGLOG_H
#define HYPERLOGLOG_H

#include <stdint.h>
#include <stddef.h>

struct hllhdr {
    char magic[4];      /* "HYLL" */
    uint8_t encoding;   /* HLL_DENSE or HLL_SPARSE. */
    uint8_t notused[3]; /* Reserved for future use, must be zero. */
    uint8_t card[8];    /* Cached cardinality, little endian. */
    uint8_t registers[]; /* Data bytes. */
};

robj *createHLLObject(void);
int hllAdd(robj *o, unsigned char *ele, size_t elesize);
uint64_t hllCount(struct hllhdr *hdr, int *invalid);

#endif /* HYPERLOGLOG_H */

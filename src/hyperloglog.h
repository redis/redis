/* hyperloglog.h -- Redis HyperLogLog probabilistic cardinality approximation.
 *
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

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

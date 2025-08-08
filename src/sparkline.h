/* sparkline.h -- ASCII Sparklines header file
 *
 * ---------------------------------------------------------------------------
 *
 * Copyright (c) 2011-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef __SPARKLINE_H
#define __SPARKLINE_H

/* A sequence is represented of many "samples" */
struct sample {
    double value;
    char *label;
};

struct sequence {
    int length;
    int labels;
    struct sample *samples;
    double min, max;
};

#define SPARKLINE_NO_FLAGS 0
#define SPARKLINE_FILL 1      /* Fill the area under the curve. */
#define SPARKLINE_LOG_SCALE 2 /* Use logarithmic scale. */

struct sequence *createSparklineSequence(void);
void sparklineSequenceAddSample(struct sequence *seq, double value, char *label);
void freeSparklineSequence(struct sequence *seq);
sds sparklineRenderRange(sds output, struct sequence *seq, int rows, int offset, int len, int flags);
sds sparklineRender(sds output, struct sequence *seq, int columns, int rows, int flags);

#endif /* __SPARKLINE_H */

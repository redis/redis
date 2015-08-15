/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"
#include "percentile.h"

#ifndef MIN
#define MIN(i, j) (((i) < (j)) ? (i) : (j))
#endif

percentileSampleReservoir* percentileReservoirAllocate() {
    return zcalloc(sizeof(percentileSampleReservoir));
}

void percentileReservoirDeallocate(percentileSampleReservoir* reservoir) {
    zfree(reservoir);
}

/* Sample an item into a reservoir. All items will be included in the reservoir
 * until the maximum number of samples is reached, at which point new samples
 * will be randomly selected for inclusion (and eviction). This algorithm
 * gurantees that any given sample has a 1/N chance of being in the reservoir,
 * for N total items seen */
void percentileSampleItem(percentileSampleReservoir* reservoir, sample_t item) {
    if (reservoir->totalItems < PERCENTILE_SAMPLE_COUNT) {
        reservoir->samples[reservoir->totalItems] = item;
    }
    else {
        long r = random() % (reservoir->totalItems);
        if (r < PERCENTILE_SAMPLE_COUNT) {
            reservoir->samples[r] = item;
        }
    }

    reservoir->totalItems++;
}

static int compare_sample_t(const void *a, const void *b) {
    const sample_t *lla = (const sample_t*) a;
    const sample_t *llb = (const sample_t*) b;
    return (*lla > *llb) - (*lla < *llb);
}

/* Given a reservoir of samples and a list of percent values, calculate the
 * corresponding percentiles. */
void percentileCalculate(
        percentileSampleReservoir* reservoir,
        int numPercentiles,
        const double* percentiles,
        sample_t* results) {
    // No samples in the reservoir.
    if (reservoir == NULL) {
        memset(results, 0, sizeof(*results) * numPercentiles);
        return;
    }

    // sort samples
    int numSamples = MIN(reservoir->totalItems, PERCENTILE_SAMPLE_COUNT);
    qsort(reservoir->samples, numSamples, sizeof(sample_t), compare_sample_t);

    // extract the percentiles
    for (int i = 0; i < numPercentiles; i++) {
        results[i] = reservoir->samples[(int)(numSamples * percentiles[i])];
    }

}

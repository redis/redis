/*
 * Gate data type: internals shared by gate_core.c, gate_config.c and
 * gate_codec.c. Not part of the interface offered to the Redis module.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * The decoder rebuilds a gate piece by piece, so it needs the same
 * constructors the state machine uses. Keeping them here (instead of
 * duplicating them in the codec) guarantees that a decoded gate has exactly
 * the shape of a live one.
 */

#ifndef GATE_INTERNAL_H
#define GATE_INTERNAL_H

#include "gate_core.h"

/* Bytes attributed to one pending observation storing 'nnz' features. */
#define GATE_OBS_BYTES(nnz) (sizeof(gateObs) + GATE_SPARSE_BYTES(nnz))

/* Buckets of the seq -> observation index for a MAXPENDING setting: the
 * smallest power of two >= max_pending, at least 16. The index has this
 * fixed size for the life of the gate (no rehashing) and its bytes are part
 * of gateFixedBytes(). Implemented in gate_config.c. */
uint32_t gateTableSize(uint32_t max_pending);

/* Bucket of a sequence number in a table of 'size' buckets (a power of
 * two). Fibonacci hashing, so that pending ids sharing low bits (e.g. every
 * 4096th request left unanswered) do not pile up in one chain. */
static inline uint32_t gateTableSlot(uint64_t seq, uint32_t size) {
    return (uint32_t)((seq * 0x9e3779b97f4a7c15ull) >> 32) & (size - 1);
}

/* Number of region rows: K when class-conditional, else 1. */
static inline uint32_t gateConfigRows(const gateConfig *cfg) {
    return cfg->class_mode ? cfg->nclasses : 1;
}

/* 1 if 'row' is tested (not an ALWAYSDEFER class). */
static inline int gateRowTested(const gateConfig *cfg, uint32_t row) {
    return !(cfg->class_mode && cfg->class_defer[row]);
}

/* Regions a generation of this config tests: tested rows * T. */
uint32_t gateTestedRegions(const gateConfig *cfg);

/* Allocate a gate holding a deep copy of the VALIDATED 'cfg', with the
 * threshold grid, the per-row bets, the (CLASSIFIER) zeroed trainer and the
 * observation index built, fixed_bytes cached, next_seq = 1 and everything
 * else zero: no generations, no observations, no counters, lineage / epoch
 * / clocks 0. NULL on allocation failure. gateCreate() and the decoder both
 * start from this. */
gate *gateAllocate(const gateConfig *cfg);

/* Allocate an empty generation shaped for 'g': nrows * T regions
 * (gateRegionInit), policy all -1, drift windows initialized, model zeroed
 * (not allocated: the caller copies or decodes weights into it), every
 * scalar 0. NULL on allocation failure. */
gateGeneration *gateGenerationAllocate(const gate *g);

/* Release a generation including its model. NULL is a no-op. */
void gateGenerationRelease(gateGeneration *gen);

/* Allocate a zeroed observation with result = cand_label = serv_label = -1.
 * NULL on allocation failure. */
gateObs *gateObsAllocate(void);

/* Release an observation and its stored features (must be unlinked). */
void gateObsRelease(gateObs *o);

/* Append an observation whose seq is greater than every pending seq: adds
 * it to the index and the pending list and, when (flags & GATE_OBS_ELIGIBLE),
 * to the eligible FIFO; updates npending, neligible and pending_bytes by
 * GATE_OBS_BYTES(o->x.n). Does not touch next_seq or the counters. */
void gateObsLink(gate *g, gateObs *o);

#endif /* GATE_INTERNAL_H */

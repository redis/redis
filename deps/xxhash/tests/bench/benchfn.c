/*
 * Copyright (C) 2016-2021 Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */



/* *************************************
*  Includes
***************************************/
#include <stdlib.h>      /* malloc, free */
#include <string.h>      /* memset */
#undef NDEBUG            /* assert must not be disabled */
#include <assert.h>      /* assert */

#include "timefn.h"        /* UTIL_time_t, UTIL_getTime */
#include "benchfn.h"


/* *************************************
*  Constants
***************************************/
#define TIMELOOP_MICROSEC     SEC_TO_MICRO      /* 1 second */
#define TIMELOOP_NANOSEC      (1*1000000000ULL) /* 1 second */

#define KB *(1 <<10)
#define MB *(1 <<20)
#define GB *(1U<<30)


/* *************************************
*  Debug errors
***************************************/
#if defined(DEBUG) && (DEBUG >= 1)
#  include <stdio.h>       /* fprintf */
#  define DISPLAY(...)       fprintf(stderr, __VA_ARGS__)
#  define DEBUGOUTPUT(...) { if (DEBUG) DISPLAY(__VA_ARGS__); }
#else
#  define DEBUGOUTPUT(...)
#endif


/* error without displaying */
#define RETURN_QUIET_ERROR(retValue, ...) {           \
    DEBUGOUTPUT("%s: %i: \n", __FILE__, __LINE__);    \
    DEBUGOUTPUT("Error : ");                          \
    DEBUGOUTPUT(__VA_ARGS__);                         \
    DEBUGOUTPUT(" \n");                               \
    return retValue;                                  \
}


/* *************************************
*  Benchmarking an arbitrary function
***************************************/

int BMK_isSuccessful_runOutcome(BMK_runOutcome_t outcome)
{
    return outcome.error_tag_never_ever_use_directly == 0;
}

/* warning : this function will stop program execution if outcome is invalid !
 *           check outcome validity first, using BMK_isValid_runResult() */
BMK_runTime_t BMK_extract_runTime(BMK_runOutcome_t outcome)
{
    assert(outcome.error_tag_never_ever_use_directly == 0);
    return outcome.internal_never_ever_use_directly;
}

size_t BMK_extract_errorResult(BMK_runOutcome_t outcome)
{
    assert(outcome.error_tag_never_ever_use_directly != 0);
    return outcome.error_result_never_ever_use_directly;
}

static BMK_runOutcome_t BMK_runOutcome_error(size_t errorResult)
{
    BMK_runOutcome_t b;
    memset(&b, 0, sizeof(b));
    b.error_tag_never_ever_use_directly = 1;
    b.error_result_never_ever_use_directly = errorResult;
    return b;
}

static BMK_runOutcome_t BMK_setValid_runTime(BMK_runTime_t runTime)
{
    BMK_runOutcome_t outcome;
    memset(&outcome, 0, sizeof(outcome));
    outcome.error_tag_never_ever_use_directly = 0;
    outcome.internal_never_ever_use_directly = runTime;
    return outcome;
}


/* initFn will be measured once, benchFn will be measured `nbLoops` times */
/* initFn is optional, provide NULL if none */
/* benchFn must return a size_t value that errorFn can interpret */
/* takes # of blocks and list of size & stuff for each. */
/* can report result of benchFn for each block into blockResult. */
/* blockResult is optional, provide NULL if this information is not required */
/* note : time per loop can be reported as zero if run time < timer resolution */
BMK_runOutcome_t BMK_benchFunction(BMK_benchParams_t p,
                                   unsigned nbLoops)
{
    /* init */
    {   size_t i;
        for (i = 0; i < p.blockCount; i++) {
            memset(p.dstBuffers[i], 0xE5, p.dstCapacities[i]);  /* warm up and erase result buffer */
    }   }

    /* benchmark */
    {   UTIL_time_t const clockStart = UTIL_getTime();
        size_t dstSize = 0;
        unsigned loopNb, blockNb;
        nbLoops += !nbLoops;   /* minimum nbLoops is 1 */
        if (p.initFn != NULL) p.initFn(p.initPayload);
        for (loopNb = 0; loopNb < nbLoops; loopNb++) {
            for (blockNb = 0; blockNb < p.blockCount; blockNb++) {
                size_t const res = p.benchFn(p.srcBuffers[blockNb], p.srcSizes[blockNb],
                                   p.dstBuffers[blockNb], p.dstCapacities[blockNb],
                                   p.benchPayload);
                if (loopNb == 0) {
                    if (p.blockResults != NULL) p.blockResults[blockNb] = res;
                    if ((p.errorFn != NULL) && (p.errorFn(res))) {
                        RETURN_QUIET_ERROR(BMK_runOutcome_error(res),
                            "Function benchmark failed on block %u (of size %u) with error %i",
                            blockNb, (unsigned)p.srcSizes[blockNb], (int)res);
                    }
                    dstSize += res;
            }   }
        }  /* for (loopNb = 0; loopNb < nbLoops; loopNb++) */

        {   PTime const totalTime = UTIL_clockSpanNano(clockStart);
            BMK_runTime_t rt;
            rt.nanoSecPerRun = (double)totalTime / nbLoops;
            rt.sumOfReturn = dstSize;
            return BMK_setValid_runTime(rt);
    }   }
}


/* ====  Benchmarking any function, providing intermediate results  ==== */

struct BMK_timedFnState_s {
    PTime timeSpent_ns;
    PTime timeBudget_ns;
    PTime runBudget_ns;
    BMK_runTime_t fastestRun;
    unsigned nbLoops;
    UTIL_time_t coolTime;
};  /* typedef'd to BMK_timedFnState_t within bench.h */

BMK_timedFnState_t* BMK_createTimedFnState(unsigned total_ms, unsigned run_ms)
{
    BMK_timedFnState_t* const r = (BMK_timedFnState_t*)malloc(sizeof(*r));
    if (r == NULL) return NULL;   /* malloc() error */
    BMK_resetTimedFnState(r, total_ms, run_ms);
    return r;
}

void BMK_freeTimedFnState(BMK_timedFnState_t* state) { free(state); }

BMK_timedFnState_t*
BMK_initStatic_timedFnState(void* buffer, size_t size, unsigned total_ms, unsigned run_ms)
{
    typedef char check_size[ 2 * (sizeof(BMK_timedFnState_shell) >= sizeof(struct BMK_timedFnState_s)) - 1];  /* static assert : a compilation failure indicates that BMK_timedFnState_shell is not large enough */
    typedef struct { check_size c; BMK_timedFnState_t tfs; } tfs_align;  /* force tfs to be aligned at its next best position */
    size_t const tfs_alignment = offsetof(tfs_align, tfs); /* provides the minimal alignment restriction for BMK_timedFnState_t */
    BMK_timedFnState_t* const r = (BMK_timedFnState_t*)buffer;
    if (buffer == NULL) return NULL;
    if (size < sizeof(struct BMK_timedFnState_s)) return NULL;
    if ((size_t)buffer % tfs_alignment) return NULL;  /* buffer must be properly aligned */
    BMK_resetTimedFnState(r, total_ms, run_ms);
    return r;
}

void BMK_resetTimedFnState(BMK_timedFnState_t* timedFnState, unsigned total_ms, unsigned run_ms)
{
    if (!total_ms) total_ms = 1 ;
    if (!run_ms) run_ms = 1;
    if (run_ms > total_ms) run_ms = total_ms;
    timedFnState->timeSpent_ns = 0;
    timedFnState->timeBudget_ns = (PTime)total_ms * TIMELOOP_NANOSEC / 1000;
    timedFnState->runBudget_ns = (PTime)run_ms * TIMELOOP_NANOSEC / 1000;
    timedFnState->fastestRun.nanoSecPerRun = (double)TIMELOOP_NANOSEC * 2000000000;  /* hopefully large enough : must be larger than any potential measurement */
    timedFnState->fastestRun.sumOfReturn = (size_t)(-1LL);
    timedFnState->nbLoops = 1;
    timedFnState->coolTime = UTIL_getTime();
}

/* Tells if nb of seconds set in timedFnState for all runs is spent.
 * note : this function will return 1 if BMK_benchFunctionTimed() has actually errored. */
int BMK_isCompleted_TimedFn(const BMK_timedFnState_t* timedFnState)
{
    return (timedFnState->timeSpent_ns >= timedFnState->timeBudget_ns);
}


#undef MIN
#define MIN(a,b)   ( (a) < (b) ? (a) : (b) )

#define MINUSABLETIME  (TIMELOOP_NANOSEC / 2)  /* 0.5 seconds */

BMK_runOutcome_t BMK_benchTimedFn(BMK_timedFnState_t* cont,
                                  BMK_benchParams_t p)
{
    PTime const runBudget_ns = cont->runBudget_ns;
    PTime const runTimeMin_ns = runBudget_ns / 2;
    BMK_runTime_t bestRunTime = cont->fastestRun;

    for (;;) {
        BMK_runOutcome_t const runResult = BMK_benchFunction(p, cont->nbLoops);

        if (!BMK_isSuccessful_runOutcome(runResult)) { /* error : move out */
            return runResult;
        }

        {   BMK_runTime_t const newRunTime = BMK_extract_runTime(runResult);
            double const loopDuration_ns = newRunTime.nanoSecPerRun * cont->nbLoops;

            cont->timeSpent_ns += (unsigned long long)loopDuration_ns;

            /* estimate nbLoops for next run to last approximately 1 second */
            if (loopDuration_ns > (runBudget_ns / 50)) {
                double const fastestRun_ns = MIN(bestRunTime.nanoSecPerRun, newRunTime.nanoSecPerRun);
                cont->nbLoops = (unsigned)(runBudget_ns / fastestRun_ns) + 1;
            } else {
                /* previous run was too short : blindly increase workload by x multiplier */
                const unsigned multiplier = 10;
                assert(cont->nbLoops < ((unsigned)-1) / multiplier);  /* avoid overflow */
                cont->nbLoops *= multiplier;
            }

            if (loopDuration_ns < runTimeMin_ns) {
                /* When benchmark run time is too small : don't report results.
                 * increased risks of rounding errors */
                continue;
            }

            if (newRunTime.nanoSecPerRun < bestRunTime.nanoSecPerRun) {
                bestRunTime = newRunTime;
            }
        }
        break;
    }   /* while (!completed) */

    return BMK_setValid_runTime(bestRunTime);
}

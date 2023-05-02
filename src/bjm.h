/*
 * Background Job Manager - submit jobs to a background thread.
 */

#ifndef __BJM_H__
#define __BJM_H__

#include "sds.h"


/* Initialize BJM with the requested number of background threads.
 */
void bjmInit(int numThreads);


/* Provided job functions will be executed on a different thread and passed the provided privdata.
 */
typedef void (*bjmJobFunc)(void *privdata);

/* After registering a function, the returned function handle can be used to submit jobs.
 */
typedef int bjmJobFuncHandle;


/* Register a job function which can process background jobs.  A handle is returned for submitting
 * jobs & gathering metrics.  This function is idempotent - submitting the same function again will
 * return the same handle.  Handle values will be > 0, so this pattern can be used:
 *
 *    static bjmJobFuncHandle myHandle;
 *    if (!myHandle) myHandle = bjmRegisterJobFunc(myFunc);
 *    bjmSubmitJob(myHandle, ...);
 *
 * This co-locates a static variable at the point of job submission, and avoids repeated
 * registration calls.
 */
bjmJobFuncHandle bjmRegisterJobFunc(bjmJobFunc func);


/* Submit a job to BJM.  The provided function will be executed on a background thread.  privdata
 * will be provided as a parameter to the provided function.  For fairness, jobs with different
 * callback functions will be executed in round-robin fashion.  Since jobs are executed across
 * multiple threads, there is no guarantee as to ordering or exclusion between jobs.
 */
void bjmSubmitJob(bjmJobFuncHandle funcHandle, void *privdata);


/* Kill all threads in an unclean way.  Non-recoverable.
 * Only used during collection of debug information.
 */
void bjmKillThreads(void);


/* Count the number of pending/active jobs for the given job function.
 * Note that this value is highly volatile as background threads are processing the jobs.
 */
long bjmPendingJobsOfType(bjmJobFuncHandle funcHandle);


/* Provide metrics data for INFO
 */
sds bjmCatInfo(sds info);

#endif

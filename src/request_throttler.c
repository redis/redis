/*
 * Copyright (c) 2021-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* The request throttler allows suspending / resuming incoming request processing due to several
 * reasons, where each of them may be set indefinitely or for a limited duration
 * Since IsSuspended is part of the hot path, it must be optimized. To avoid re-calculating
 * the global state on every call, it is calculated when a reason state is modified, and only nullified
 * in IsSuspended if the global deadline has passed
 */

 #include "server.h"
 #include "request_throttler.h"

struct requestThrottlerState {
    long long global_suspend_until_ms;
    long long suspend_until_ms[REASON_MAX_NUM];
} throttler_state;

/* Recompute the global suspend deadline as the max of all per-reason
 * deadlines. If any reason is suspended forever, the global deadline is
 * forever too. */
static void requestThrottlerUpdateGlobal(void) {
    long long until = 0;

    for (int i = 0; i < REASON_MAX_NUM; i++) {
        if (throttler_state.suspend_until_ms[i] == REQUEST_THROTTLER_SUSPEND_FOREVER) {
            until = REQUEST_THROTTLER_SUSPEND_FOREVER;
            break;
        }
        if (throttler_state.suspend_until_ms[i] > until)
            until = throttler_state.suspend_until_ms[i];
    }

    throttler_state.global_suspend_until_ms = until;
}

void RequestThrottler_Init(void) {
    memset(&throttler_state, 0, sizeof(throttler_state));
}

void RequestThrottler_Suspend(enum request_throttler_reason reason, long long duration_ms) {
    serverAssert(reason >= 0 && reason < REASON_MAX_NUM);
    serverAssert(duration_ms == REQUEST_THROTTLER_SUSPEND_FOREVER || duration_ms >= 0);

    throttler_state.suspend_until_ms[reason] = duration_ms == REQUEST_THROTTLER_SUSPEND_FOREVER ?
        REQUEST_THROTTLER_SUSPEND_FOREVER : mstime() + duration_ms;

    requestThrottlerUpdateGlobal();
}

void RequestThrottler_Resume(enum request_throttler_reason reason) {
    serverAssert(reason >= 0 && reason < REASON_MAX_NUM);

    throttler_state.suspend_until_ms[reason] = 0;

    requestThrottlerUpdateGlobal();
}

int RequestThrottler_IsSuspended(void) {
    if (throttler_state.global_suspend_until_ms == 0) return 0;
    if (throttler_state.global_suspend_until_ms == REQUEST_THROTTLER_SUSPEND_FOREVER) return 1;

    if (mstime() >= throttler_state.global_suspend_until_ms) {
        /* The global deadline is the max of all per-reason deadlines, so
         * once it has passed every reason has expired too. */
        memset(throttler_state.suspend_until_ms, 0, sizeof(throttler_state.suspend_until_ms));
        throttler_state.global_suspend_until_ms = 0;
        return 0;
    }

    return 1;
}

int RequestThrottler_IsSuspendedByReason(enum request_throttler_reason reason) {
    serverAssert(reason >= 0 && reason < REASON_MAX_NUM);

    if (throttler_state.suspend_until_ms[reason] == 0) return 0;
    if (throttler_state.suspend_until_ms[reason] == REQUEST_THROTTLER_SUSPEND_FOREVER) return 1;

    if (mstime() >= throttler_state.suspend_until_ms[reason]) {
        throttler_state.suspend_until_ms[reason] = 0;
        requestThrottlerUpdateGlobal();
        return 0;
    }

    return 1;
}

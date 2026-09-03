/*
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef REQUEST_THROTTLER_H
#define REQUEST_THROTTLER_H

enum request_throttler_reason {
    REASON_SLAVE_OUTPUT_BUFFER_EXCEEDED = 0,
    REASON_MAX_NUM
};

#define REQUEST_THROTTLER_SUSPEND_FOREVER -1

/* Initialize the request throttler state. Must be called once before any
 * other RequestThrottler_* function. */
void RequestThrottler_Init(void);

/* Suspend request processing for the given reason for duration_ms
 * milliseconds from now, or indefinitely if duration_ms is
 * REQUEST_THROTTLER_SUSPEND_FOREVER. */
void RequestThrottler_Suspend(enum request_throttler_reason reason, long long duration_ms);

/* Clear a previously set suspension for the given reason. */
void RequestThrottler_Resume(enum request_throttler_reason reason);

/* Return 1 if request processing is currently suspended for any reason,
 * 0 otherwise. */
int RequestThrottler_IsSuspended(void);

/* Return 1 if request processing is currently suspended for the given reason,
 * 0 otherwise. */
int RequestThrottler_IsSuspendedByReason(enum request_throttler_reason reason);

#endif /* REQUEST_THROTTLER_H */
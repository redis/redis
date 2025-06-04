/* eventnotifier.h -- An event notifier based on eventfd or pipe.
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef EVENTNOTIFIER_H
#define EVENTNOTIFIER_H

#include "config.h"

#define EN_OK 0
#define EN_ERR -1

typedef struct eventNotifier {
#ifdef HAVE_EVENT_FD
    int efd;
#else
    int pipefd[2];
#endif
} eventNotifier;

eventNotifier* createEventNotifier(void);
int getReadEventFd(struct eventNotifier *en);
int getWriteEventFd(struct eventNotifier *en);
int triggerEventNotifier(struct eventNotifier *en);
int handleEventNotifier(struct eventNotifier *en);
void freeEventNotifier(struct eventNotifier *en);

#endif

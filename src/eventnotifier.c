/* eventnotifier.c -- An event notifier based on eventfd or pipe.
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "eventnotifier.h"

#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef HAVE_EVENT_FD
#include <sys/eventfd.h>
#endif

#include "anet.h"
#include "zmalloc.h"

eventNotifier* createEventNotifier(void) {
    eventNotifier *en = zmalloc(sizeof(eventNotifier));
    if (!en) return NULL;

#ifdef HAVE_EVENT_FD
    if ((en->efd = eventfd(0, EFD_NONBLOCK| EFD_CLOEXEC)) != -1) {
        return en;
    }
#else
    if (anetPipe(en->pipefd, O_CLOEXEC|O_NONBLOCK, O_CLOEXEC|O_NONBLOCK) != -1) {
        return en;
    }
#endif

    /* Clean up if error. */
    zfree(en);
    return NULL;
}

int getReadEventFd(struct eventNotifier *en) {
#ifdef HAVE_EVENT_FD
    return en->efd;
#else
    return en->pipefd[0];
#endif
}

int getWriteEventFd(struct eventNotifier *en) {
#ifdef HAVE_EVENT_FD
    return en->efd;
#else
    return en->pipefd[1];
#endif
}

int triggerEventNotifier(struct eventNotifier *en) {
#ifdef HAVE_EVENT_FD
    uint64_t u = 1;
    if (write(en->efd, &u, sizeof(uint64_t)) == -1) {
        return EN_ERR;
    }
#else
    char buf[1] = {'R'};
    if (write(en->pipefd[1], buf, 1) == -1) {
        return EN_ERR;
    }
#endif
    return EN_OK;
}

int handleEventNotifier(struct eventNotifier *en) {
#ifdef HAVE_EVENT_FD
    uint64_t u;
    if (read(en->efd, &u, sizeof(uint64_t)) == -1) {
        return EN_ERR;
    }
#else
    char buf[1];
    if (read(en->pipefd[0], buf, 1) == -1) {
        return EN_ERR;
    }
#endif
    return EN_OK;
}

void freeEventNotifier(struct eventNotifier *en) {
#ifdef HAVE_EVENT_FD
    close(en->efd);
#else
    close(en->pipefd[0]);
    close(en->pipefd[1]);
#endif

    /* Free memory */
    zfree(en);
}

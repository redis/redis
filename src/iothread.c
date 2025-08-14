/* iothread.c -- The threaded io implementation.
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "server.h"

/* IO threads. */
static IOThread IOThreads[IO_THREADS_MAX_NUM];

/* For main thread */
static list *mainThreadPendingClientsToIOThreads[IO_THREADS_MAX_NUM]; /* Clients to IO threads */
static list *mainThreadProcessingClients[IO_THREADS_MAX_NUM]; /* Clients in processing */
static list *mainThreadPendingClients[IO_THREADS_MAX_NUM]; /* Pending clients from IO threads */
static pthread_mutex_t mainThreadPendingClientsMutexes[IO_THREADS_MAX_NUM]; /* Mutex for pending clients */
static eventNotifier* mainThreadPendingClientsNotifiers[IO_THREADS_MAX_NUM]; /* Notifier for pending clients */

/* Send the clients to the main thread for processing when the number of clients
 * in pending list reaches IO_THREAD_MAX_PENDING_CLIENTS, or check_size is 0. */
static inline void sendPendingClientsToMainThreadIfNeeded(IOThread *t, int check_size) {
    size_t len = listLength(t->pending_clients_to_main_thread);
    if (len == 0 || (check_size && len < IO_THREAD_MAX_PENDING_CLIENTS)) return;

    int running = 0, pending = 0;
    pthread_mutex_lock(&mainThreadPendingClientsMutexes[t->id]);
    pending = listLength(mainThreadPendingClients[t->id]);
    listJoin(mainThreadPendingClients[t->id], t->pending_clients_to_main_thread);
    pthread_mutex_unlock(&mainThreadPendingClientsMutexes[t->id]);
    if (!pending) atomicGetWithSync(server.running, running);

    /* Only notify main thread if it is not running and no pending clients to process,
     * to avoid unnecessary notify/wakeup. If the main thread is running, it will
     * process the clients in beforeSleep. If there are pending clients, we may
     * already notify the main thread if needed. */
    if (!running && !pending) {
        triggerEventNotifier(mainThreadPendingClientsNotifiers[t->id]);
    }
}

/* When IO threads read a complete query of clients or want to free clients, it
 * should remove it from its clients list and put the client in the list to main
 * thread, we will send these clients to main thread in IOThreadBeforeSleep. */
void enqueuePendingClientsToMainThread(client *c, int unbind) {
    /* If the IO thread may no longer manage it, such as closing client, we should
     * unbind client from event loop, so main thread doesn't need to do it costly. */
    if (unbind) connUnbindEventLoop(c->conn);
    /* Just skip if it already is transferred. */
    if (c->io_thread_client_list_node) {
        IOThread *t = &IOThreads[c->tid];
        /* If there are several clients to process, let the main thread handle them ASAP.
         * Since the client being added to the queue may still need to be processed by
         * the IO thread, we must call this before adding it to the queue to avoid
         * races with the main thread. */
        sendPendingClientsToMainThreadIfNeeded(t, 1);
        /* Disable read and write to avoid race when main thread processes. */
        c->io_flags &= ~(CLIENT_IO_READ_ENABLED | CLIENT_IO_WRITE_ENABLED);
        /* Remove the client from IO thread, add it to main thread's pending list. */
        listUnlinkNode(t->clients, c->io_thread_client_list_node);
        listLinkNodeTail(t->pending_clients_to_main_thread, c->io_thread_client_list_node);
        c->io_thread_client_list_node = NULL;
    }
}

/* Unbind connection of client from io thread event loop, write and read handlers
 * also be removed, ensures that we can operate the client safely. */
void unbindClientFromIOThreadEventLoop(client *c) {
    serverAssert(c->tid != IOTHREAD_MAIN_THREAD_ID &&
                 c->running_tid == IOTHREAD_MAIN_THREAD_ID);
    if (!connHasEventLoop(c->conn)) return;
    /* As calling in main thread, we should pause the io thread to make it safe. */
    pauseIOThread(c->tid);
    connUnbindEventLoop(c->conn);
    resumeIOThread(c->tid);
}

/* When main thread is processing a client from IO thread, and wants to keep it,
 * we should unbind connection of client from io thread event loop first,
 * and then bind the client connection into server's event loop. */
void keepClientInMainThread(client *c) {
    serverAssert(c->tid != IOTHREAD_MAIN_THREAD_ID &&
                 c->running_tid == IOTHREAD_MAIN_THREAD_ID);
    /* IO thread no longer manage it. */
    server.io_threads_clients_num[c->tid]--;
    /* Unbind connection of client from io thread event loop. */
    unbindClientFromIOThreadEventLoop(c);
    /* Let main thread to run it, rebind event loop and read handler */
    connRebindEventLoop(c->conn, server.el);
    connSetReadHandler(c->conn, readQueryFromClient);
    c->io_flags |= CLIENT_IO_READ_ENABLED | CLIENT_IO_WRITE_ENABLED;
    c->running_tid = IOTHREAD_MAIN_THREAD_ID;
    c->tid = IOTHREAD_MAIN_THREAD_ID;
    freeClientDeferredObjects(c, 1); /* Free deferred objects. */
    /* Main thread starts to manage it. */
    server.io_threads_clients_num[c->tid]++;
}

/* If the client is managed by IO thread, we should fetch it from IO thread
 * and then main thread will can process it. Just like IO Thread transfers
 * the client to the main thread for processing. */
void fetchClientFromIOThread(client *c) {
    serverAssert(c->tid != IOTHREAD_MAIN_THREAD_ID &&
                 c->running_tid != IOTHREAD_MAIN_THREAD_ID);
    pauseIOThread(c->tid);
    /* Remove the client from clients list of IO thread or main thread. */
    if (c->io_thread_client_list_node) {
        listDelNode(IOThreads[c->tid].clients, c->io_thread_client_list_node);
        c->io_thread_client_list_node = NULL;
    } else {
        list *clients[5] = {
            IOThreads[c->tid].pending_clients,
            IOThreads[c->tid].pending_clients_to_main_thread,
            mainThreadPendingClients[c->tid],
            mainThreadProcessingClients[c->tid],
            mainThreadPendingClientsToIOThreads[c->tid]
        };
        for (int i = 0; i < 5; i++) {
            listNode *ln = listSearchKey(clients[i], c);
            if (ln) {
                listDelNode(clients[i], ln);
                /* Client only can be in one client list. */
                break;
            }
        }
    }
    /* Unbind connection of client from io thread event loop. */
    connUnbindEventLoop(c->conn);
    /* Now main thread can process it. */
    c->running_tid = IOTHREAD_MAIN_THREAD_ID;
    resumeIOThread(c->tid);
    freeClientDeferredObjects(c, 1); /* Free deferred objects. */
}

/* For some clients, we must handle them in the main thread, since there is
 * data race to be processed in IO threads.
 *
 * - Close ASAP, we must free the client in main thread.
 * - Replica, pubsub, monitor, blocked, tracking clients, main thread may
 *   directly write them a reply when conditions are met.
 * - Script command with debug may operate connection directly. */
int isClientMustHandledByMainThread(client *c) {
    if (c->flags & (CLIENT_CLOSE_ASAP | CLIENT_MASTER | CLIENT_SLAVE |
                    CLIENT_PUBSUB | CLIENT_MONITOR | CLIENT_BLOCKED |
                    CLIENT_UNBLOCKED | CLIENT_TRACKING | CLIENT_LUA_DEBUG |
                    CLIENT_LUA_DEBUG_SYNC))
    {
        return 1;
    }
    return 0;
}

/* When the main thread accepts a new client or transfers clients to IO threads,
 * it assigns the client to the IO thread with the fewest clients. */
void assignClientToIOThread(client *c) {
    serverAssert(c->tid == IOTHREAD_MAIN_THREAD_ID);
    /* Find the IO thread with the fewest clients. */
    int min_id = 0;
    int min = INT_MAX;
    for (int i = 1; i < server.io_threads_num; i++) {
        if (server.io_threads_clients_num[i] < min) {
            min = server.io_threads_clients_num[i];
            min_id = i;
        }
    }

    /* Assign the client to the IO thread. */
    server.io_threads_clients_num[c->tid]--;
    c->tid = min_id;
    c->running_tid = min_id;
    server.io_threads_clients_num[min_id]++;

    /* The client running in IO thread needs to have deferred objects array. */
    c->deferred_objects = zmalloc(sizeof(robj*) * CLIENT_MAX_DEFERRED_OBJECTS);

    /* Unbind connection of client from main thread event loop, disable read and
     * write, and then put it in the list, main thread will send these clients
     * to IO thread in beforeSleep. */
    connUnbindEventLoop(c->conn);
    c->io_flags &= ~(CLIENT_IO_READ_ENABLED | CLIENT_IO_WRITE_ENABLED);
    listAddNodeTail(mainThreadPendingClientsToIOThreads[c->tid], c);
}

/* If updating maxclients config, we not only resize the event loop of main thread
 * but also resize the event loop of all io threads, and if one thread is failed,
 * it is failed totally, since a fd can be distributed into any IO thread. */
int resizeAllIOThreadsEventLoops(size_t newsize) {
    int result = AE_OK;
    if (server.io_threads_num <= 1) return result;

    /* To make context safe. */
    pauseAllIOThreads();
    for (int i = 1; i < server.io_threads_num; i++) {
        IOThread *t = &IOThreads[i];
        if (aeResizeSetSize(t->el, newsize) == AE_ERR)
            result = AE_ERR;
    }
    resumeAllIOThreads();
    return result;
}

/* In the main thread, we may want to operate data of io threads, maybe uninstall
 * event handler, access query/output buffer or resize event loop, we need a clean
 * and safe context to do that. We pause io thread in IOThreadBeforeSleep, do some
 * jobs and then resume it. To avoid thread suspended, we use busy waiting to confirm
 * the target status. Besides we use atomic variable to make sure memory visibility
 * and ordering.
 *
 * Make sure that only the main thread can call these function,
 *  - pauseIOThread, resumeIOThread
 *  - pauseAllIOThreads, resumeAllIOThreads
 *  - pauseIOThreadsRange, resumeIOThreadsRange
 *
 * The main thread will pause the io thread, and then wait for the io thread to
 * be paused. The io thread will check the paused status in IOThreadBeforeSleep,
 * and then pause itself.
 *
 * The main thread will resume the io thread, and then wait for the io thread to
 * be resumed. The io thread will check the paused status in IOThreadBeforeSleep,
 * and then resume itself.
 */

/* We may pause the same io thread nestedly, so we need to record the times of
 * pausing, and only when the times of pausing is 0, we can pause the io thread,
 * and only when the times of pausing is 1, we can resume the io thread. */
static int PausedIOThreads[IO_THREADS_MAX_NUM] = {0};

/* Pause the specific range of io threads, and wait for them to be paused. */
void pauseIOThreadsRange(int start, int end) {
    if (!server.io_threads_active) return;
    serverAssert(start >= 1 && end < server.io_threads_num && start <= end);
    serverAssert(pthread_equal(pthread_self(), server.main_thread_id));

    /* Try to make all io threads paused in parallel */
    for (int i = start; i <= end; i++) {
        PausedIOThreads[i]++;
        /* Skip if already paused */
        if (PausedIOThreads[i] > 1) continue;

        int paused;
        atomicGetWithSync(IOThreads[i].paused, paused);
        /* Don't support to call reentrant */
        serverAssert(paused == IO_THREAD_UNPAUSED);
        atomicSetWithSync(IOThreads[i].paused, IO_THREAD_PAUSING);
        /* Just notify io thread, no actual job, since io threads check paused
         * status in IOThreadBeforeSleep, so just wake it up if polling wait. */
        triggerEventNotifier(IOThreads[i].pending_clients_notifier);
    }

    /* Wait for all io threads paused */
    for (int i = start; i <= end; i++) {
        if (PausedIOThreads[i] > 1) continue;
        int paused = IO_THREAD_PAUSING;
        while (paused != IO_THREAD_PAUSED) {
            atomicGetWithSync(IOThreads[i].paused, paused);
        }
    }
}

/* Resume the specific range of io threads, and wait for them to be resumed. */
void resumeIOThreadsRange(int start, int end) {
    if (!server.io_threads_active) return;
    serverAssert(start >= 1 && end < server.io_threads_num && start <= end);
    serverAssert(pthread_equal(pthread_self(), server.main_thread_id));

    for (int i = start; i <= end; i++) {
        serverAssert(PausedIOThreads[i] > 0);
        PausedIOThreads[i]--;
        if (PausedIOThreads[i] > 0) continue;

        int paused;
        /* Check if it is paused, since we must call 'pause' and
         * 'resume' in pairs */
        atomicGetWithSync(IOThreads[i].paused, paused);
        serverAssert(paused == IO_THREAD_PAUSED);
        /* Resume */
        atomicSetWithSync(IOThreads[i].paused, IO_THREAD_RESUMING);
        while (paused != IO_THREAD_UNPAUSED) {
            atomicGetWithSync(IOThreads[i].paused, paused);
        }
    }
}

/* The IO thread checks whether it is being paused, and if so, it pauses itself
 * and waits for resuming, corresponding to the pause/resumeIOThread* functions.
 * Currently, this is only called in IOThreadBeforeSleep, as there are no pending
 * I/O events at this point, with a clean context. */
void handlePauseAndResume(IOThread *t) {
    int paused;
    /* Check if i am being paused. */
    atomicGetWithSync(t->paused, paused);
    if (paused == IO_THREAD_PAUSING) {
        atomicSetWithSync(t->paused, IO_THREAD_PAUSED);
        /* Wait for resuming */
        while (paused != IO_THREAD_RESUMING) {
            atomicGetWithSync(t->paused, paused);
        }
        atomicSetWithSync(t->paused, IO_THREAD_UNPAUSED);
    }
}

/* Pause the specific io thread, and wait for it to be paused. */
void pauseIOThread(int id) {
    pauseIOThreadsRange(id, id);
}

/* Resume the specific io thread, and wait for it to be resumed. */
void resumeIOThread(int id) {
    resumeIOThreadsRange(id, id);
}

/* Pause all io threads, and wait for them to be paused. */
void pauseAllIOThreads(void) {
    pauseIOThreadsRange(1, server.io_threads_num-1);
}

/* Resume all io threads, and wait for them to be resumed. */
void resumeAllIOThreads(void) {
    resumeIOThreadsRange(1, server.io_threads_num-1);
}

/* Add the pending clients to the list of IO threads, and trigger an event to
 * notify io threads to handle. */
int sendPendingClientsToIOThreads(void) {
    int processed = 0;
    for (int i = 1; i < server.io_threads_num; i++) {
        int len = listLength(mainThreadPendingClientsToIOThreads[i]);
        if (len > 0) {
            IOThread *t = &IOThreads[i];
            pthread_mutex_lock(&t->pending_clients_mutex);
            listJoin(t->pending_clients, mainThreadPendingClientsToIOThreads[i]);
            pthread_mutex_unlock(&t->pending_clients_mutex);
            /* Trigger an event, maybe an error is returned when buffer is full
             * if using pipe, but no worry, io thread will handle all clients
             * in list when receiving a notification. */
            triggerEventNotifier(t->pending_clients_notifier);
        }
        processed += len;
    }
    return processed;
}

/* Prefetch the commands from the IO thread. The return value is the number
 * of clients that have been prefetched. */
int prefetchIOThreadCommands(IOThread *t) {
    int len = listLength(mainThreadProcessingClients[t->id]);
    int to_prefetch = determinePrefetchCount(len);
    if (to_prefetch == 0) return 0;

    int clients = 0;
    listIter li;
    listNode *ln;
    listRewind(mainThreadProcessingClients[t->id], &li);
    while((ln = listNext(&li)) && clients++ < to_prefetch) {
        client *c = listNodeValue(ln);
        /* A single command may contain multiple keys. If the batch is full,
         * we stop adding clients to it. */
        if (addCommandToBatch(c) == C_ERR) break;
    }

    /* Prefetch the commands in the batch. */
    prefetchCommands();
    return clients;
}

extern int ProcessingEventsWhileBlocked;

/* Send the pending clients to the IO thread if the number of pending clients
 * is greater than IO_THREAD_MAX_PENDING_CLIENTS, or if size_check is 0. */
static inline void sendPendingClientsToIOThreadIfNeeded(IOThread *t, int size_check) {
    size_t len = listLength(mainThreadPendingClientsToIOThreads[t->id]);
    if (len == 0 || (size_check && len < IO_THREAD_MAX_PENDING_CLIENTS)) return;

    /* If AOF fsync policy is always, we should not let io thread handle these
     * clients now since we don't flush AOF buffer to file and sync yet.
     * So these clients will be delayed to send io threads in beforeSleep after
     * flushAppendOnlyFile. 
     * 
     * If we are in processEventsWhileBlocked, we don't send clients to io threads
     * now, we want to update server.events_processed_while_blocked accurately. */
    if (server.aof_fsync != AOF_FSYNC_ALWAYS && !ProcessingEventsWhileBlocked) {
        int running = 0, pending = 0;
        pthread_mutex_lock(&(t->pending_clients_mutex));
        pending = listLength(t->pending_clients);
        listJoin(t->pending_clients, mainThreadPendingClientsToIOThreads[t->id]);
        pthread_mutex_unlock(&(t->pending_clients_mutex));
        if (!pending) atomicGetWithSync(t->running, running);

        /* Only notify io thread if it is not running and no pending clients to
         * process, to avoid unnecessary notify/wakeup. If the io thread is running,
         * it will process the clients in beforeSleep. If there are pending clients,
         * we may already notify the io thread if needed. */
        if(!running && !pending) triggerEventNotifier(t->pending_clients_notifier);
    }
}

/* The main thread processes the clients from IO threads, these clients may have
 * a complete command to execute or need to be freed. Note that IO threads never
 * free client since this operation access much server data.
 *
 * Please notice that this function may be called reentrantly, i,e, the same goes
 * for handleClientsFromIOThread and processClientsOfAllIOThreads. For example,
 * when processing script command, it may call processEventsWhileBlocked to
 * process new events, if the clients with fired events from the same io thread,
 * it may call this function reentrantly. */
int processClientsFromIOThread(IOThread *t) {
    /* Get the list of clients to process. */
    pthread_mutex_lock(&mainThreadPendingClientsMutexes[t->id]);
    listJoin(mainThreadProcessingClients[t->id], mainThreadPendingClients[t->id]);
    pthread_mutex_unlock(&mainThreadPendingClientsMutexes[t->id]);
    size_t processed = listLength(mainThreadProcessingClients[t->id]);
    if (processed == 0) return 0;

    int prefetch_clients = 0;
    /* We may call processClientsFromIOThread reentrantly, so we need to
     * reset the prefetching batch, besides, users may change the config
     * of prefetch batch size, so we need to reset the prefetching batch. */
    resetCommandsBatch();

    listNode *node = NULL;
    while (listLength(mainThreadProcessingClients[t->id])) {
        /* Prefetch the commands if no clients in the batch. */
        if (prefetch_clients <= 0) prefetch_clients = prefetchIOThreadCommands(t);
        /* Reset the prefetching batch if we have processed all clients. */
        if (--prefetch_clients <= 0) resetCommandsBatch();

        /* Each time we pop up only the first client to process to guarantee
         * reentrancy safety. */
        if (node) zfree(node);
        node = listFirst(mainThreadProcessingClients[t->id]);
        listUnlinkNode(mainThreadProcessingClients[t->id], node);
        client *c = listNodeValue(node);

        /* Make sure the client is neither readable nor writable in io thread to
         * avoid data race. */
        serverAssert(!(c->io_flags & (CLIENT_IO_READ_ENABLED | CLIENT_IO_WRITE_ENABLED)));
        serverAssert(!(c->flags & CLIENT_CLOSE_ASAP));

        /* Let main thread to run it, set running thread id first. */
        c->running_tid = IOTHREAD_MAIN_THREAD_ID;

        /* If a read error occurs, handle it in the main thread first, since we
         * want to print logs about client information before freeing. */
        if (c->read_error) handleClientReadError(c);

        /* The client is asked to close in IO thread. */
        if (c->io_flags & CLIENT_IO_CLOSE_ASAP) {
            freeClient(c);
            continue;
        }

        /* Run cron task for the client per second or it is marked as pending cron. */
        if (c->last_cron_check_time + 1000 <= server.mstime ||
            c->io_flags & CLIENT_IO_PENDING_CRON)
        {
            c->last_cron_check_time = server.mstime;
            if (clientsCronRunClient(c)) continue;
        } else {
            /* Update the client in the mem usage if clientsCronRunClient is not
             * being called, since that function already performs the update. */
            updateClientMemUsageAndBucket(c);
        }

        /* Process the pending command and input buffer. */
        if (!c->read_error && c->io_flags & CLIENT_IO_PENDING_COMMAND) {
            c->flags |= CLIENT_PENDING_COMMAND;
            if (processPendingCommandAndInputBuffer(c) == C_ERR) {
                /* If the client is no longer valid, it must be freed safely. */
                continue;
            }
        }

        /* We may have pending replies if io thread may not finish writing
         * reply to client, so we did not put the client in pending write
         * queue. And we should do that first since we may keep the client
         * in main thread instead of returning to io threads. */
        if (!(c->flags & CLIENT_PENDING_WRITE) && clientHasPendingReplies(c))
            putClientInPendingWriteQueue(c);

        /* The client only can be processed in the main thread, otherwise data
         * race will happen, since we may touch client's data in main thread. */
        if (isClientMustHandledByMainThread(c)) {
            keepClientInMainThread(c);
            continue;
        }

        /* Remove this client from pending write clients queue of main thread,
         * And some clients may do not have reply if CLIENT REPLY OFF/SKIP. */
        if (c->flags & CLIENT_PENDING_WRITE) {
            c->flags &= ~CLIENT_PENDING_WRITE;
            listUnlinkNode(server.clients_pending_write, &c->clients_pending_write_node);
        }
        c->running_tid = c->tid;
        listLinkNodeHead(mainThreadPendingClientsToIOThreads[c->tid], node);
        node = NULL;
    
        /* If there are several clients to process, let io thread handle them ASAP. */
        sendPendingClientsToIOThreadIfNeeded(t, 1);
    }
    if (node) zfree(node);

    /* Send the clients to io thread without pending size check, since main thread
     * may process clients from other io threads, so we need to send them to the
     * io thread to process in prallel. */
    sendPendingClientsToIOThreadIfNeeded(t, 0);

    return processed;
}

/* When the io thread finishes processing the client with the read event, it will
 * notify the main thread through event triggering in IOThreadBeforeSleep. The main
 * thread handles the event through this function. */
void handleClientsFromIOThread(struct aeEventLoop *el, int fd, void *ptr, int mask) {
    UNUSED(el);
    UNUSED(mask);

    IOThread *t = ptr;

    /* Handle fd event first. */
    serverAssert(fd == getReadEventFd(mainThreadPendingClientsNotifiers[t->id]));
    handleEventNotifier(mainThreadPendingClientsNotifiers[t->id]);

    /* Process the clients from IO threads. */
    processClientsFromIOThread(t);
}

/* In the new threaded io design, one thread may process multiple clients, so when
 * an io thread notifies the main thread of an event, there may be multiple clients
 * with commands that need to be processed. But in the event handler function
 * handleClientsFromIOThread may be blocked when processing the specific command,
 * the previous clients can not get a reply, and the subsequent clients can not be
 * processed, so we need to handle this scenario in beforeSleep. The function is to
 * process the commands of subsequent clients from io threads. And another function
 * sendPendingClientsToIOThreads make sure clients from io thread can get replies.
 * See also beforeSleep.
 * 
 * In beforeSleep, we also call this function to handle the clients that are
 * transferred from io threads without notification. */
int processClientsOfAllIOThreads(void) {
    int processed = 0;
    for (int i = 1; i < server.io_threads_num; i++) {
        processed += processClientsFromIOThread(&IOThreads[i]);
    }
    return processed;
}

/* After the main thread processes the clients, it will send the clients back to
 * io threads to handle, and fire an event, the io thread handles the event by
 * this function. */
void handleClientsFromMainThread(struct aeEventLoop *ae, int fd, void *ptr, int mask) {
    UNUSED(ae);
    UNUSED(mask);

    IOThread *t = ptr;

    /* Handle fd event first. */
    serverAssert(fd == getReadEventFd(t->pending_clients_notifier));
    handleEventNotifier(t->pending_clients_notifier);

    /* Process the clients from main thread. */
    processClientsFromMainThread(t);
}

/* Processing clients that have finished executing commands from the main thread.
 * If the client is not binded to the event loop, we should bind it first and
 * install read handler. If the client still has query buffer, we should process
 * the input buffer. If the client has pending reply, we just reply to client,
 * and then install write handler if needed. */
int processClientsFromMainThread(IOThread *t) {
    pthread_mutex_lock(&t->pending_clients_mutex);
    listJoin(t->processing_clients, t->pending_clients);
    pthread_mutex_unlock(&t->pending_clients_mutex);
    size_t processed = listLength(t->processing_clients);
    if (processed == 0) return 0;

    listIter li;
    listNode *ln;
    listRewind(t->processing_clients, &li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        serverAssert(!(c->io_flags & (CLIENT_IO_READ_ENABLED | CLIENT_IO_WRITE_ENABLED)));
        /* Main thread must handle clients with CLIENT_CLOSE_ASAP flag, since
         * we only set io_flags when clients in io thread are freed ASAP. */
        serverAssert(!(c->flags & CLIENT_CLOSE_ASAP));

        /* Link client in IO thread clients list first. */
        serverAssert(c->io_thread_client_list_node == NULL);
        listUnlinkNode(t->processing_clients, ln);
        listLinkNodeTail(t->clients, ln);
        c->io_thread_client_list_node = listLast(t->clients);

        /* The client now is in the IO thread, let's free deferred objects. */
        freeClientDeferredObjects(c, 0);

        /* The client is asked to close, we just let main thread free it. */
        if (c->io_flags & CLIENT_IO_CLOSE_ASAP) {
            enqueuePendingClientsToMainThread(c, 1);
            continue;
        }

        /* Enable read and write and reset some flags. */
        c->io_flags |= CLIENT_IO_READ_ENABLED | CLIENT_IO_WRITE_ENABLED;
        c->io_flags &= ~(CLIENT_IO_PENDING_COMMAND | CLIENT_IO_PENDING_CRON);

        /* Only bind once, we never remove read handler unless freeing client. */
        if (!connHasEventLoop(c->conn)) {
            connRebindEventLoop(c->conn, t->el);
            serverAssert(!connHasReadHandler(c->conn));
            connSetReadHandler(c->conn, readQueryFromClient);
        }

        /* If the client has pending replies, write replies to client. */
        if (clientHasPendingReplies(c)) {
            writeToClient(c, 0);
            if (!(c->io_flags & CLIENT_IO_CLOSE_ASAP) && clientHasPendingReplies(c)) {
                connSetWriteHandler(c->conn, sendReplyToClient);
            }
        }
    }
    /* All clients must are processed. */
    serverAssert(listLength(t->processing_clients) == 0);
    return processed;
}

void IOThreadBeforeSleep(struct aeEventLoop *el) {
    IOThread *t = el->privdata[0];

    /* Handle pending data(typical TLS). */
    connTypeProcessPendingData(el);

    /* If any connection type(typical TLS) still has pending unread data don't sleep at all. */
    int dont_sleep = connTypeHasPendingData(el);

    /* Process clients from main thread, since the main thread may deliver clients
     * without notification during IO thread processing events. */
    if (processClientsFromMainThread(t) > 0) {
        /* If there are clients that are processed, we should not sleep since main
         * thread may want to continue deliverring clients without notification, so
         * IO thread can process them ASAP, and the main thread can avoid unnecessary
         * notification (write fd and wake up) is costly. */
        dont_sleep = 1;
    }
    if (!dont_sleep) {
        atomicSetWithSync(t->running, 0); /* Not running if going to sleep. */
        /* Try to process clients from main thread again, since before we set
         * running to 0, the main thread may deliver clients to this io thread. */
        processClientsFromMainThread(t);
    }
    aeSetDontWait(t->el, dont_sleep);

    /* Check if i am being paused, pause myself and resume. */
    handlePauseAndResume(t);

    /* Send clients to main thread to process, we don't check size here since
     * we want to send all clients to main thread before going to sleeping. */
    sendPendingClientsToMainThreadIfNeeded(t, 0);
}

void IOThreadAfterSleep(struct aeEventLoop *el) {
    IOThread *t = el->privdata[0];

    /* Set the IO thread to running state, so the main thread can deliver
     * clients to it without extra notifications. */
    atomicSetWithSync(t->running, 1);
}

/* Periodically transfer part of clients to the main thread for processing. */
void IOThreadClientsCron(IOThread *t) {
    /* Process at least a few clients while we are at it, even if we need
     * to process less than CLIENTS_CRON_MIN_ITERATIONS to meet our contract
     * of processing each client once per second. */
    int iterations = listLength(t->clients) / CONFIG_DEFAULT_HZ;
    if (iterations < CLIENTS_CRON_MIN_ITERATIONS) {
        iterations = CLIENTS_CRON_MIN_ITERATIONS;
    }

    listIter li;
    listNode *ln;
    listRewind(t->clients, &li);
    while ((ln = listNext(&li)) && iterations--) {
        client *c = listNodeValue(ln);
        /* Mark the client as pending cron, main thread will process it. */
        c->io_flags |= CLIENT_IO_PENDING_CRON;
        enqueuePendingClientsToMainThread(c, 0);
    }
}

/* This is the IO thread timer interrupt, CONFIG_DEFAULT_HZ times per second.
 * The current responsibility is to detect clients that have been stuck in the
 * IO thread for too long and hand them over to the main thread for handling. */
int IOThreadCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    IOThread *t = clientData;

    /* Run cron tasks for the clients in the IO thread. */
    IOThreadClientsCron(t);

    return 1000/CONFIG_DEFAULT_HZ;
}

/* The main function of IO thread, it will run an event loop. The mian thread
 * and IO thread will communicate through event notifier. */
void *IOThreadMain(void *ptr) {
    IOThread *t = ptr;
    char thdname[16];
    snprintf(thdname, sizeof(thdname), "io_thd_%d", t->id);
    redis_set_thread_title(thdname);
    redisSetCpuAffinity(server.server_cpulist);
    makeThreadKillable();
    aeSetBeforeSleepProc(t->el, IOThreadBeforeSleep);
    aeSetAfterSleepProc(t->el, IOThreadAfterSleep);
    aeMain(t->el);
    return NULL;
}

/* Initialize the data structures needed for threaded I/O. */
void initThreadedIO(void) {
    if (server.io_threads_num <= 1) return;

    server.io_threads_active = 1;

    if (server.io_threads_num > IO_THREADS_MAX_NUM) {
        serverLog(LL_WARNING,"Fatal: too many I/O threads configured. "
                             "The maximum number is %d.", IO_THREADS_MAX_NUM);
        exit(1);
    }

    prefetchCommandsBatchInit();

    /* Spawn and initialize the I/O threads. */
    for (int i = 1; i < server.io_threads_num; i++) {
        IOThread *t = &IOThreads[i];
        t->id = i;
        t->el = aeCreateEventLoop(server.maxclients+CONFIG_FDSET_INCR);
        t->el->privdata[0] = t;
        t->pending_clients = listCreate();
        t->processing_clients = listCreate();
        t->pending_clients_to_main_thread = listCreate();
        t->clients = listCreate();
        atomicSetWithSync(t->paused, IO_THREAD_UNPAUSED);
        atomicSetWithSync(t->running, 0);

        pthread_mutexattr_t *attr = NULL;
        #if defined(__linux__) && defined(__GLIBC__)
        attr = zmalloc(sizeof(pthread_mutexattr_t));
        pthread_mutexattr_init(attr);
        pthread_mutexattr_settype(attr, PTHREAD_MUTEX_ADAPTIVE_NP);
        #endif
        pthread_mutex_init(&t->pending_clients_mutex, attr);

        t->pending_clients_notifier = createEventNotifier();
        if (aeCreateFileEvent(t->el, getReadEventFd(t->pending_clients_notifier),
                              AE_READABLE, handleClientsFromMainThread, t) != AE_OK)
        {
            serverLog(LL_WARNING, "Fatal: Can't register file event for IO thread notifications.");
            exit(1);
        }

        /* This is the timer callback of the IO thread, used to gradually handle 
         * some background operations, such as clients cron. */
        if (aeCreateTimeEvent(t->el, 1, IOThreadCron, t, NULL) == AE_ERR) {
            serverLog(LL_WARNING, "Fatal: Can't create event loop timers in IO thread.");
            exit(1);
        }

        /* Create IO thread */
        if (pthread_create(&t->tid, NULL, IOThreadMain, (void*)t) != 0) {
            serverLog(LL_WARNING, "Fatal: Can't initialize IO thread.");
            exit(1);
        }

        /* For main thread */
        mainThreadPendingClientsToIOThreads[i] = listCreate();
        mainThreadPendingClients[i] = listCreate();
        mainThreadProcessingClients[i] = listCreate();
        pthread_mutex_init(&mainThreadPendingClientsMutexes[i], attr);
        mainThreadPendingClientsNotifiers[i] = createEventNotifier();
        if (aeCreateFileEvent(server.el, getReadEventFd(mainThreadPendingClientsNotifiers[i]),
                              AE_READABLE, handleClientsFromIOThread, t) != AE_OK)
        {
            serverLog(LL_WARNING, "Fatal: Can't register file event for main thread notifications.");
            exit(1);
        }
        if (attr) zfree(attr);
    }
}

/* Kill the IO threads, TODO: release the applied resources. */
void killIOThreads(void) {
    if (server.io_threads_num <= 1) return;

    int err, j;
    for (j = 1; j < server.io_threads_num; j++) {
        if (IOThreads[j].tid == pthread_self()) continue;
        if (IOThreads[j].tid && pthread_cancel(IOThreads[j].tid) == 0) {
            if ((err = pthread_join(IOThreads[j].tid,NULL)) != 0) {
                serverLog(LL_WARNING,
                    "IO thread(tid:%lu) can not be joined: %s",
                        (unsigned long)IOThreads[j].tid, strerror(err));
            } else {
                serverLog(LL_WARNING,
                    "IO thread(tid:%lu) terminated",(unsigned long)IOThreads[j].tid);
            }
        }
    }
}

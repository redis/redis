/* blocked.c - generic support for blocking operations like BLPOP & WAIT.
 *
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
 *
 * ---------------------------------------------------------------------------
 *
 * API:
 *
 * blockClient() set the CLIENT_BLOCKED flag in the client, and set the
 * specified block type 'btype' filed to one of BLOCKED_* macros.
 *
 * unblockClient() unblocks the client doing the following:
 * 1) It calls the btype-specific function to cleanup the state.
 * 2) It unblocks the client by unsetting the CLIENT_BLOCKED flag.
 * 3) It puts the client into a list of just unblocked clients that are
 *    processed ASAP in the beforeSleep() event loop callback, so that
 *    if there is some query buffer to process, we do it. This is also
 *    required because otherwise there is no 'readable' event fired, we
 *    already read the pending commands. We also set the CLIENT_UNBLOCKED
 *    flag to remember the client is in the unblocked_clients list.
 *
 * processUnblockedClients() is called inside the beforeSleep() function
 * to process the query buffer from unblocked clients and remove the clients
 * from the blocked_clients queue.
 *
 * replyToBlockedClientTimedOut() is called by the cron function when
 * a client blocked reaches the specified timeout (if the timeout is set
 * to 0, no timeout is processed).
 * It usually just needs to send a reply to the client.
 *
 * When implementing a new type of blocking operation, the implementation
 * should modify unblockClient() and replyToBlockedClientTimedOut() in order
 * to handle the btype-specific behavior of this two functions.
 * If the blocking operation waits for certain keys to change state, the
 * clusterRedirectBlockedClientIfNeeded() function should also be updated.
 */

#include "server.h"
#include "slowlog.h"
#include "latency.h"
#include "monotonic.h"

/* The following structure represents a node in the server.ready_keys list,
 * where we accumulate all the keys that had clients blocked with a blocking
 * operation such as B[LR]POP, but received new data in the context of the
 * last executed command.
 *
 * After the execution of every command or script, we iterate over this list to check
 * if as a result we should serve data to clients blocked, unblocking them.
 * Note that server.ready_keys will not have duplicates as there dictionary
 * also called ready_keys in every structure representing a Redis database,
 * where we make sure to remember if a given key was already added in the
 * server.ready_keys list. */
typedef struct readyList {
    redisDb *db;
    robj *key;
} readyList;

static blocking_type getBlockedTypeByType(int type) {
    switch (type) {
        case OBJ_LIST: return BLOCKED_LIST;
        case OBJ_ZSET: return BLOCKED_ZSET;
        case OBJ_MODULE: return BLOCKED_MODULE;
        case OBJ_STREAM: return BLOCKED_STREAM;
        default: return BLOCKED_NONE;
    }
}

void resetClientBlockingState(client *c) {
    c->bstate.btype = BLOCKED_NONE;
    c->bstate.timeout = 0;
    c->bstate.keys = dictCreate(&objectKeyHeapPointerValueDictType);
    c->bstate.numreplicas = 0;
    c->bstate.reploffset = 0;
}

/* Block a client for the specific operation type. Once the CLIENT_BLOCKED
 * flag is set client query buffer is not longer processed, but accumulated,
 * and will be processed when the client is unblocked. */
void blockClient(client *c, int btype) {
    /* Master client should never be blocked unless pause or module */
    serverAssert(!(c->flags & CLIENT_MASTER &&
                   btype != BLOCKED_MODULE &&
                   btype != BLOCKED_POSTPONE));

    c->flags |= CLIENT_BLOCKED;
    c->bstate.btype = btype;
    server.blocked_clients++;
    server.blocked_clients_by_type[btype]++;
    addClientToTimeoutTable(c);
}

/* This function is called after a client has finished a blocking operation
 * in order to update the total command duration, log the command into
 * the Slow log if needed, and log the reply duration event if needed. */
void updateStatsOnUnblock(client *c, long blocked_us, long reply_us, int had_errors){
    const ustime_t total_cmd_duration = c->duration + blocked_us + reply_us;
    c->lastcmd->microseconds += total_cmd_duration;
    if (had_errors)
        c->lastcmd->failed_calls++;
    if (server.latency_tracking_enabled)
        updateCommandLatencyHistogram(&(c->lastcmd->latency_histogram), total_cmd_duration*1000);
    /* Log the command into the Slow log if needed. */
    slowlogPushCurrentCommand(c, c->lastcmd, total_cmd_duration);
    /* Log the reply duration event. */
    latencyAddSampleIfNeeded("command-unblocking",reply_us/1000);
}

/* This function is called in the beforeSleep() function of the event loop
 * in order to process the pending input buffer of clients that were
 * unblocked after a blocking operation. */
void processUnblockedClients(void) {
    listNode *ln;
    client *c;

    while (listLength(server.unblocked_clients)) {
        ln = listFirst(server.unblocked_clients);
        serverAssert(ln != NULL);
        c = ln->value;
        listDelNode(server.unblocked_clients,ln);
        c->flags &= ~CLIENT_UNBLOCKED;

        /* Process remaining data in the input buffer, unless the client
         * is blocked again. Actually processInputBuffer() checks that the
         * client is not blocked before to proceed, but things may change and
         * the code is conceptually more correct this way. */
        if (!(c->flags & CLIENT_BLOCKED)) {
            /* If we have a queued command, execute it now. */
            if (processPendingCommandAndInputBuffer(c) == C_ERR) {
                c = NULL;
            }
        }
        beforeNextClient(c);
    }
}

/* This function will schedule the client for reprocessing at a safe time.
 *
 * This is useful when a client was blocked for some reason (blocking operation,
 * CLIENT PAUSE, or whatever), because it may end with some accumulated query
 * buffer that needs to be processed ASAP:
 *
 * 1. When a client is blocked, its readable handler is still active.
 * 2. However in this case it only gets data into the query buffer, but the
 *    query is not parsed or executed once there is enough to proceed as
 *    usually (because the client is blocked... so we can't execute commands).
 * 3. When the client is unblocked, without this function, the client would
 *    have to write some query in order for the readable handler to finally
 *    call processQueryBuffer*() on it.
 * 4. With this function instead we can put the client in a queue that will
 *    process it for queries ready to be executed at a safe time.
 */
void queueClientForReprocessing(client *c) {
    /* The client may already be into the unblocked list because of a previous
     * blocking operation, don't add back it into the list multiple times. */
    if (!(c->flags & CLIENT_UNBLOCKED)) {
        c->flags |= CLIENT_UNBLOCKED;
        listAddNodeTail(server.unblocked_clients,c);
    }
}

/* Unblock a client which is currently Blocked on and provided a timeout.
 * In this case since we might have a command pending
 * we want to remove the pending flag to indicate we already responded to the
 * command with timeout reply. */
void unblockClientOnTimeout(client *c) {
    if (c->flags & CLIENT_PENDING_COMMAND)
        c->flags &= ~CLIENT_PENDING_COMMAND;
    unblockClient(c);
}

/* Helper function to unblock a client that's waiting in a blocking operation such as BLPOP.
 * You should never call this function directly, but unblockClient() instead. */
static void unblockClientWaitingData(client *c) {
    dictEntry *de;
    dictIterator *di;
    list *l;
    listNode *pos;

    if (dictSize(c->bstate.keys) == 0)
        return;

    di = dictGetIterator(c->bstate.keys);
    /* The client may wait for multiple keys, so unblock it for every key. */
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);
        pos = dictFetchValue(c->bstate.keys, key);
        /* Remove this client from the list of clients waiting for this key. */
        l = dictFetchValue(c->db->blocking_keys,key);
        serverAssertWithInfo(c,key,l != NULL);
        listUnlinkNode(l,pos);
        /* If the list is empty we need to remove it to avoid wasting memory */
        if (listLength(l) == 0)
            dictDelete(c->db->blocking_keys,key);
    }
    dictReleaseIterator(di);
    dictEmpty(c->bstate.keys, NULL);
}

/* Unblock a client calling the right function depending on the kind
 * of operation the client is blocking for. */
void unblockClient(client *c) {
    if (c->bstate.btype == BLOCKED_LIST ||
        c->bstate.btype == BLOCKED_ZSET ||
        c->bstate.btype == BLOCKED_STREAM) {
        unblockClientWaitingData(c);
    } else if (c->bstate.btype == BLOCKED_WAIT) {
        unblockClientWaitingReplicas(c);
    } else if (c->bstate.btype == BLOCKED_MODULE) {
        if (moduleClientIsBlockedOnKeys(c)) unblockClientWaitingData(c);
        unblockClientFromModule(c);
    } else if (c->bstate.btype == BLOCKED_POSTPONE) {
        listDelNode(server.postponed_clients,c->postponed_list_node);
        c->postponed_list_node = NULL;
    } else if (c->bstate.btype == BLOCKED_SHUTDOWN) {
        /* No special cleanup. */
    } else {
        serverPanic("Unknown btype in unblockClient().");
    }

    /* Reset the client for a new query, unless the client has pending command to process
     * or in case a shutdown operation was canceled and we are still in the processCommand sequence  */
    if (!(c->flags & CLIENT_PENDING_COMMAND) && c->bstate.btype != BLOCKED_SHUTDOWN) {
        freeClientOriginalArgv(c);
        resetClient(c);
    }

    /* Clear the flags, and put the client in the unblocked list so that
     * we'll process new commands in its query buffer ASAP. */
    server.blocked_clients--;
    server.blocked_clients_by_type[c->bstate.btype]--;
    c->flags &= ~CLIENT_BLOCKED;
    c->bstate.btype = BLOCKED_NONE;
    removeClientFromTimeoutTable(c);
    queueClientForReprocessing(c);
}

/* Unblock a client once a specific key became available for it.
 * This function will remove the client from the list of clients blocked on this key
 * and also remove the key from the dictionary of keys this client is blocked on.
 * in case the client has a command pending it will process it immediately.  */
void unblockClientOnKey(client *c, robj *key) {
    list *l;
    listNode *pos;

    pos = dictFetchValue(c->bstate.keys, key);
    /* Remove this client from the list of clients waiting for this key. */
    l = dictFetchValue(c->db->blocking_keys,key);
    serverAssertWithInfo(c,key,l != NULL);
    listUnlinkNode(l,pos);
    /* If the list is empty we need to remove it to avoid wasting memory */
    if (listLength(l) == 0)
        dictDelete(c->db->blocking_keys,key);
    dictDelete(c->bstate.keys,key);


    /* Client is no more blocked on keys */
    if (dictSize(c->bstate.keys) == 0 ||
        // in case of blocking API calls, we might be blocked on several keys.
        // however we should force unblock the entire blocking keys
        c->bstate.btype == BLOCKED_STREAM ||
        c->bstate.btype == BLOCKED_LIST ||
        c->bstate.btype == BLOCKED_ZSET) {

        unblockClient(c);
        /* In case this client was blocked on keys during command
         * we need to
         */
        if (c->flags & CLIENT_PENDING_COMMAND) {
            c->flags &= ~CLIENT_PENDING_COMMAND;
            processCommandAndResetClient(c);
        }
    }
}

/* Unblock a client blocked on the specific key from module context.
 * This function will try to serve the module call, and in case it succeeds,
 * it will add the client to the list of module unblocked clients which will
 * be processed in moduleHandleBlockedClients. */
void moduleUnblockClientOnKey(client *c, robj *key) {

    /* Note that if *this* client cannot be served by this key,
     * it does not mean that another client that is next into the
     * list cannot be served as well: they may be blocked by
     * different modules with different triggers to consider if a key
     * is ready or not. This means we can't exit the loop but need
     * to continue after the first failure. */
    long long prev_error_replies = server.stat_total_error_replies;
    client *old_client = server.current_client;
    server.current_client = c;
    monotime replyTimer;
    elapsedStart(&replyTimer);
    if (!moduleTryServeClientBlockedOnKey(c, key)) return;
    updateStatsOnUnblock(c, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
    if (c->flags & CLIENT_PENDING_COMMAND)
        c->flags &= ~CLIENT_PENDING_COMMAND;
    moduleUnblockClient(c);
    afterCommand(c);
    server.current_client = old_client;
}

/* This function gets called when a blocked client timed out in order to
 * send it a reply of some kind. After this function is called,
 * unblockClient() will be called with the same client as argument. */
void replyToBlockedClientTimedOut(client *c) {
    if (c->bstate.btype == BLOCKED_LIST ||
        c->bstate.btype == BLOCKED_ZSET ||
        c->bstate.btype == BLOCKED_STREAM) {
        addReplyNullArray(c);
    } else if (c->bstate.btype == BLOCKED_WAIT) {
        addReplyLongLong(c,replicationCountAcksByOffset(c->bstate.reploffset));
    } else if (c->bstate.btype == BLOCKED_MODULE) {
        moduleBlockedClientTimedOut(c);
    } else {
        serverPanic("Unknown btype in replyToBlockedClientTimedOut().");
    }
}

/* If one or more clients are blocked on the SHUTDOWN command, this function
 * sends them an error reply and unblocks them. */
void replyToClientsBlockedOnShutdown(void) {
    if (server.blocked_clients_by_type[BLOCKED_SHUTDOWN] == 0) return;
    listNode *ln;
    listIter li;
    listRewind(server.clients, &li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        if (c->flags & CLIENT_BLOCKED && c->bstate.btype == BLOCKED_SHUTDOWN) {
            addReplyError(c, "Errors trying to SHUTDOWN. Check logs.");
            unblockClient(c);
        }
    }
}

/* Mass-unblock clients because something changed in the instance that makes
 * blocking no longer safe. For example clients blocked in list operations
 * in an instance which turns from master to slave is unsafe, so this function
 * is called when a master turns into a slave.
 *
 * The semantics is to send an -UNBLOCKED error to the client, disconnecting
 * it at the same time. */
void disconnectAllBlockedClients(void) {
    listNode *ln;
    listIter li;

    listRewind(server.clients,&li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);

        if (c->flags & CLIENT_BLOCKED) {
            /* POSTPONEd clients are an exception, when they'll be unblocked, the
             * command processing will start from scratch, and the command will
             * be either executed or rejected. (unlike LIST blocked clients for
             * which the command is already in progress in a way. */
            if (c->bstate.btype == BLOCKED_POSTPONE)
                continue;

            addReplyError(c,
                "-UNBLOCKED force unblock from blocking operation, "
                "instance state changed (master -> replica?)");
            unblockClient(c);
            c->flags |= CLIENT_CLOSE_AFTER_REPLY;
        }
    }
}

/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * whenever a key is ready. we iterate over all the clients blocked on this key
 * and try to re-execute the command (in case the key is still available). */
void handleClientsBlockedOnKey(readyList *rl) {

    /* We serve clients in the same order they blocked for
     * this key, from the first blocked to the last. */
    dictEntry *de = dictFind(rl->db->blocking_keys,rl->key);

    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients,&li);

        while((ln = listNext(&li))) {
            client *receiver = listNodeValue(ln);
            robj *o = lookupKeyReadWithFlags(rl->db, rl->key, LOOKUP_NONOTIFY | LOOKUP_NOSTATS);
            /* 1. In case new key was added/touched we need to verify it satisfy the
             *    blocked type, since we might process the wrong key type.
             * 2. We want to serve clients blocked on module keys
             *    regardless of the object type: we don't know what the
             *    module is trying to accomplish right now.
             * 3. In case of XREADGROUP call we will want to unblock on any change in object type
             *    or in case the key was deleted, since the group is no longer valid. */
            if ((o != NULL && (receiver->bstate.btype == getBlockedTypeByType(o->type))) ||
                (o != NULL && (receiver->bstate.btype == BLOCKED_MODULE)) ||
                ((receiver->bstate.btype == BLOCKED_STREAM) && strcasecmp(receiver->lastcmd->fullname, "xreadgroup") == 0))
            {
                    if (receiver->bstate.btype != BLOCKED_MODULE)
                        unblockClientOnKey(receiver, rl->key);
                    else
                        moduleUnblockClientOnKey(receiver, rl->key);
            }

            /* Edge case: If lookupKeyReadWithFlags decides to expire the key we have to
             * take care of the propagation here, because afterCommand might not have been called */
            if (server.also_propagate.numops > 0)
                propagatePendingCommands();
        }
    }
}

/* This function should be called by Redis every time a single command,
 * a MULTI/EXEC block, or a Lua script, terminated its execution after
 * being called by a client. It handles serving clients blocked in all scenarios
 * where a specific key access requires to block until that key is available.
 *
 * All the keys with at least one client blocked that are signaled as ready
 * are accumulated into the server.ready_keys list. This function will run
 * the list and will serve clients accordingly.
 * Note that the function will iterate again and again (for example as a result of serving BLMOVE
 * we can have new blocking clients to serve because of the PUSH side of BLMOVE.)
 *
 * This function is normally "fair", that is, it will serve clients
 * using a FIFO behavior. However this fairness is violated in certain
 * edge cases, that is, when we have clients blocked at the same time
 * in a sorted set and in a list, for the same key (a very odd thing to
 * do client side, indeed!). Because mismatching clients (blocking for
 * a different type compared to the current key type) are moved in the
 * other side of the linked list. However as long as the key starts to
 * be used only for a single type, like virtually any Redis application will
 * do, the function is already fair. */
void handleClientsBlockedOnKeys(void) {
    /* This function is called only when also_propagate is in its basic state
     * (i.e. not from call(), module context, etc.) */
    serverAssert(server.also_propagate.numops == 0);
    int prev_core_propagates = server.core_propagates;
    server.core_propagates = 1;

    while(listLength(server.ready_keys) != 0) {
        list *l;

        /* Point server.ready_keys to a fresh list and save the current one
         * locally. This way as we run the old list we are free to call
         * signalKeyAsReady() that may push new elements in server.ready_keys
         * when handling clients blocked into BLMOVE. */
        l = server.ready_keys;
        server.ready_keys = listCreate();

        while(listLength(l) != 0) {
            listNode *ln = listFirst(l);
            readyList *rl = ln->value;

            /* First of all remove this key from db->ready_keys so that
             * we can safely call signalKeyAsReady() against this key. */
            dictDelete(rl->db->ready_keys,rl->key);

            handleClientsBlockedOnKey(rl);

            /* Free this item. */
            decrRefCount(rl->key);
            zfree(rl);
            listDelNode(l,ln);
        }
        listRelease(l); /* We have the new list on place at this point. */
    }

    serverAssert(server.core_propagates); /* This function should not be re-entrant */

    server.core_propagates = prev_core_propagates;
}

/* block a client due to wait command */
void blockForReplication(client *c, mstime_t timeout, long long offset, long numreplicas) {

    c->bstate.timeout = timeout;
    c->bstate.reploffset = offset;
    c->bstate.numreplicas = numreplicas;
    listAddNodeHead(server.clients_waiting_acks,c);
    blockClient(c,BLOCKED_WAIT);
}

/* Postpone client from executing a command. For example the server might be busy
 * requesting to avoid processing clients commands which will be processed later
 * when the it is ready to accept them. */
void blockPostponeClient(client *c) {
    c->bstate.timeout = 0;
    blockClient(c,BLOCKED_POSTPONE);
    listAddNodeTail(server.postponed_clients, c);
    c->postponed_list_node = listLast(server.postponed_clients);
    /* Mark this client to execute its command */
    c->flags |= CLIENT_PENDING_COMMAND;
}

/* Block client due to shutdown command */
void blockClientShutdown(client *c) {
    blockClient(c, BLOCKED_SHUTDOWN);
    /* Mark this client to execute its command */
}

/* Set a client in blocking mode for the specified key, with the specified timeout.
 * The 'type' argument is BLOCKED_LIST,BLOCKED_ZSET or BLOCKED_STREAM depending on the kind of operation we are
 * waiting for an empty key in order to awake the client. The client is blocked
 * for all the 'numkeys' keys as in the 'keys' argument. */
void blockForKeys(client *c, int btype, robj **keys, int numkeys, mstime_t timeout) {
    dictEntry *de;
    list *l;
    int j;

    c->bstate.timeout = timeout;
    for (j = 0; j < numkeys; j++) {
        /* If the key already exists in the dictionary ignore it. */
        if (dictFetchValue(c->bstate.keys,keys[j])) {
            continue;
        }
        incrRefCount(keys[j]);

        /* And in the other "side", to map keys -> clients */
        de = dictFind(c->db->blocking_keys,keys[j]);
        if (de == NULL) {
            int retval;

            /* For every key we take a list of clients blocked for it */
            l = listCreate();
            retval = dictAdd(c->db->blocking_keys,keys[j],l);
            incrRefCount(keys[j]);
            serverAssertWithInfo(c,keys[j],retval == DICT_OK);
        } else {
            l = dictGetVal(de);
        }
        listAddNodeTail(l,c);
        dictAdd(c->bstate.keys,keys[j],listLast(l));
    }
    c->flags |= CLIENT_PENDING_COMMAND;
    blockClient(c,btype);
}

/* If the specified key has clients blocked waiting for list pushes, this
 * function will put the key reference into the server.ready_keys list.
 * Note that db->ready_keys is a hash table that allows us to avoid putting
 * the same key again and again in the list in case of multiple pushes
 * made by a script or in the context of MULTI/EXEC.
 *
 * The list will be finally processed by handleClientsBlockedOnKeys() */
void signalKeyAsReady(redisDb *db, robj *key, int type) {
    readyList *rl;

    /* Quick returns. */
    int btype = getBlockedTypeByType(type);
    if (btype == BLOCKED_NONE) {
        /* The type can never block. */
        return;
    }
    if (!server.blocked_clients_by_type[btype] &&
        !server.blocked_clients_by_type[BLOCKED_MODULE]) {
        /* No clients block on this type. Note: Blocked modules are represented
         * by BLOCKED_MODULE, even if the intention is to wake up by normal
         * types (list, zset, stream), so we need to check that there are no
         * blocked modules before we do a quick return here. */
        return;
    }

    /* No clients blocking for this key? No need to queue it. */
    if (dictFind(db->blocking_keys,key) == NULL) return;

    /* Key was already signaled? No need to queue it again. */
    if (dictFind(db->ready_keys,key) != NULL) return;

    /* Ok, we need to queue this key into server.ready_keys. */
    rl = zmalloc(sizeof(*rl));
    rl->key = key;
    rl->db = db;
    incrRefCount(key);
    listAddNodeTail(server.ready_keys,rl);

    /* We also add the key in the db->ready_keys dictionary in order
     * to avoid adding it multiple times into a list with a simple O(1)
     * check. */
    incrRefCount(key);
    serverAssert(dictAdd(db->ready_keys,key,NULL) == DICT_OK);
}

/* Since we are unblocking XREADGROUP clients in the event the
 * key was deleted/overwritten we must do the same in case the
 * database was flushed/swapped. */
void scanDatabaseForDeletedStreamsAndSignalKeys(redisDb *emptied, redisDb *replaced_with) {
    /* Optimization: If no clients are in type BLOCKED_STREAM,
     * we can skip this loop. */
    if (!server.blocked_clients_by_type[BLOCKED_STREAM]) return;

    dictEntry *de;
    dictIterator *di = dictGetSafeIterator(emptied->blocking_keys);
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);
        int was_stream = 0, is_stream = 0;

        dictEntry *kde = dictFind(emptied->dict, key->ptr);
        if (kde) {
            robj *value = dictGetVal(kde);
            was_stream = value->type == OBJ_STREAM;
        }
        if (replaced_with) {
            dictEntry *kde = dictFind(replaced_with->dict, key->ptr);
            if (kde) {
                robj *value = dictGetVal(kde);
                is_stream = value->type == OBJ_STREAM;
            }
        }
        /* We want to try to unblock any client using a blocking XREADGROUP */
        if (was_stream && !is_stream)
            signalKeyAsReady(emptied, key, OBJ_STREAM);
    }
    dictReleaseIterator(di);
}


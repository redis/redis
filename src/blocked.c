/* blocked.c - generic support for blocking operations like BLPOP & WAIT.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
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

/* forward declarations */
static void unblockClientWaitingData(client *c);
static void handleClientsBlockedOnKey(readyList *rl);
static void unblockClientOnKey(client *c, robj *key);
static void moduleUnblockClientOnKey(client *c, robj *key);
static void releaseBlockedEntry(client *c, dictEntry *de, int remove_key);

void initClientBlockingState(client *c) {
    c->bstate.btype = BLOCKED_NONE;
    c->bstate.timeout = 0;
    c->bstate.keys = dictCreate(&objectKeyHeapPointerValueDictType);
    c->bstate.numreplicas = 0;
    c->bstate.reploffset = 0;
    c->bstate.unblock_on_nokey = 0;
    c->bstate.async_rm_call_handle = NULL;
}

/* Block a client for the specific operation type. Once the CLIENT_BLOCKED
 * flag is set client query buffer is not longer processed, but accumulated,
 * and will be processed when the client is unblocked. */
void blockClient(client *c, int btype) {
    /* Master client should never be blocked unless pause or module */
    serverAssert(!(c->flags & CLIENT_MASTER &&
                   btype != BLOCKED_MODULE &&
                   btype != BLOCKED_LAZYFREE &&
                   btype != BLOCKED_POSTPONE));

    c->flags |= CLIENT_BLOCKED;
    c->bstate.btype = btype;
    if (!(c->flags & CLIENT_MODULE)) server.blocked_clients++; /* We count blocked client stats on regular clients and not on module clients */
    server.blocked_clients_by_type[btype]++;
    addClientToTimeoutTable(c);
}

/* Usually when a client is unblocked due to being blocked while processing some command
 * he will attempt to reprocess the command which will update the statistics.
 * However in case the client was timed out or in case of module blocked client is being unblocked
 * the command will not be reprocessed and we need to make stats update.
 * This function will make updates to the commandstats, slowlog and monitors.*/
void updateStatsOnUnblock(client *c, long blocked_us, long reply_us, int had_errors){
    const ustime_t total_cmd_duration = c->duration + blocked_us + reply_us;
    c->lastcmd->microseconds += total_cmd_duration;
    c->lastcmd->calls++;
    server.stat_numcommands++;
    if (had_errors)
        c->lastcmd->failed_calls++;
    if (server.latency_tracking_enabled)
        updateCommandLatencyHistogram(&(c->lastcmd->latency_histogram), total_cmd_duration*1000);
    /* Log the command into the Slow log if needed. */
    slowlogPushCurrentCommand(c, c->lastcmd, total_cmd_duration);
    c->duration = 0;
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

        if (c->flags & CLIENT_MODULE) {
            if (!(c->flags & CLIENT_BLOCKED)) {
                moduleCallCommandUnblockedHandler(c);
            }
            continue;
        }

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

/* Unblock a client calling the right function depending on the kind
 * of operation the client is blocking for. */
void unblockClient(client *c, int queue_for_reprocessing) {
    if (c->bstate.btype == BLOCKED_LIST ||
        c->bstate.btype == BLOCKED_ZSET ||
        c->bstate.btype == BLOCKED_STREAM) {
        unblockClientWaitingData(c);
    } else if (c->bstate.btype == BLOCKED_WAIT || c->bstate.btype == BLOCKED_WAITAOF) {
        unblockClientWaitingReplicas(c);
    } else if (c->bstate.btype == BLOCKED_MODULE) {
        if (moduleClientIsBlockedOnKeys(c)) unblockClientWaitingData(c);
        unblockClientFromModule(c);
    } else if (c->bstate.btype == BLOCKED_POSTPONE) {
        listDelNode(server.postponed_clients,c->postponed_list_node);
        c->postponed_list_node = NULL;
    } else if (c->bstate.btype == BLOCKED_SHUTDOWN) {
        /* No special cleanup. */
    } else if (c->bstate.btype == BLOCKED_LAZYFREE) {
        /* No special cleanup. */
    } else {
        serverPanic("Unknown btype in unblockClient().");
    }

    /* Reset the client for a new query, unless the client has pending command to process
     * or in case a shutdown operation was canceled and we are still in the processCommand sequence  */
    if (!(c->flags & CLIENT_PENDING_COMMAND) && c->bstate.btype != BLOCKED_SHUTDOWN) {
        freeClientOriginalArgv(c);
        /* Clients that are not blocked on keys are not reprocessed so we must
         * call reqresAppendResponse here (for clients blocked on key,
         * unblockClientOnKey is called, which eventually calls processCommand,
         * which calls reqresAppendResponse) */
        reqresAppendResponse(c);
        resetClient(c);
    }

    /* Clear the flags, and put the client in the unblocked list so that
     * we'll process new commands in its query buffer ASAP. */
    if (!(c->flags & CLIENT_MODULE)) server.blocked_clients--; /* We count blocked client stats on regular clients and not on module clients */
    server.blocked_clients_by_type[c->bstate.btype]--;
    c->flags &= ~CLIENT_BLOCKED;
    c->bstate.btype = BLOCKED_NONE;
    c->bstate.unblock_on_nokey = 0;
    removeClientFromTimeoutTable(c);
    if (queue_for_reprocessing) queueClientForReprocessing(c);
}

/* This function gets called when a blocked client timed out in order to
 * send it a reply of some kind. After this function is called,
 * unblockClient() will be called with the same client as argument. */
void replyToBlockedClientTimedOut(client *c) {
    if (c->bstate.btype == BLOCKED_LAZYFREE) {
        addReply(c, shared.ok); /* No reason lazy-free to fail */
    } else if (c->bstate.btype == BLOCKED_LIST ||
        c->bstate.btype == BLOCKED_ZSET ||
        c->bstate.btype == BLOCKED_STREAM) {
        addReplyNullArray(c);
        updateStatsOnUnblock(c, 0, 0, 0);
    } else if (c->bstate.btype == BLOCKED_WAIT) {
        addReplyLongLong(c,replicationCountAcksByOffset(c->bstate.reploffset));
    } else if (c->bstate.btype == BLOCKED_WAITAOF) {
        addReplyArrayLen(c,2);
        addReplyLongLong(c,server.fsynced_reploff >= c->bstate.reploffset);
        addReplyLongLong(c,replicationCountAOFAcksByOffset(c->bstate.reploffset));
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
            unblockClient(c, 1);
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

            if (c->bstate.btype == BLOCKED_LAZYFREE) {
                addReply(c, shared.ok); /* No reason lazy-free to fail */
                c->flags &= ~CLIENT_PENDING_COMMAND;
                unblockClient(c, 1);
            } else {

                unblockClientOnError(c,
                                     "-UNBLOCKED force unblock from blocking operation, "
                                     "instance state changed (master -> replica?)");
            }
            c->flags |= CLIENT_CLOSE_AFTER_REPLY;
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

    /* In case we are already in the process of unblocking clients we should
     * not make a recursive call, in order to prevent breaking fairness. */
    static int in_handling_blocked_clients = 0;
    if (in_handling_blocked_clients)
        return;
    in_handling_blocked_clients = 1;

    /* This function is called only when also_propagate is in its basic state
     * (i.e. not from call(), module context, etc.) */
    serverAssert(server.also_propagate.numops == 0);

    /* If a command being unblocked causes another command to get unblocked,
     * like a BLMOVE would do, then the new unblocked command will get processed
     * right away rather than wait for later. */
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
    in_handling_blocked_clients = 0;
}

/* Set a client in blocking mode for the specified key, with the specified timeout.
 * The 'type' argument is BLOCKED_LIST,BLOCKED_ZSET or BLOCKED_STREAM depending on the kind of operation we are
 * waiting for an empty key in order to awake the client. The client is blocked
 * for all the 'numkeys' keys as in the 'keys' argument.
 * The client will unblocked as soon as one of the keys in 'keys' value was updated.
 * the parameter unblock_on_nokey can be used to force client to be unblocked even in the case the key
 * is updated to become unavailable, either by type change (override), deletion or swapdb */
void blockForKeys(client *c, int btype, robj **keys, int numkeys, mstime_t timeout, int unblock_on_nokey) {
    dictEntry *db_blocked_entry, *db_blocked_existing_entry, *client_blocked_entry;
    list *l;
    int j;

    if (!(c->flags & CLIENT_REPROCESSING_COMMAND)) {
        /* If the client is re-processing the command, we do not set the timeout
         * because we need to retain the client's original timeout. */
        c->bstate.timeout = timeout;
    }

    for (j = 0; j < numkeys; j++) {
        /* If the key already exists in the dictionary ignore it. */
        if (!(client_blocked_entry = dictAddRaw(c->bstate.keys,keys[j],NULL))) {
            continue;
        }
        incrRefCount(keys[j]);

        /* And in the other "side", to map keys -> clients */
        db_blocked_entry = dictAddRaw(c->db->blocking_keys,keys[j], &db_blocked_existing_entry);

        /* In case key[j] did not have blocking clients yet, we need to create a new list */
        if (db_blocked_entry != NULL) {
            l = listCreate();
            dictSetVal(c->db->blocking_keys, db_blocked_entry, l);
            incrRefCount(keys[j]);
        } else {
            l = dictGetVal(db_blocked_existing_entry);
        }
        listAddNodeTail(l,c);
        dictSetVal(c->bstate.keys,client_blocked_entry,listLast(l));

        /* We need to add the key to blocking_keys_unblock_on_nokey, if the client
         * wants to be awakened if key is deleted (like XREADGROUP) */
        if (unblock_on_nokey) {
            db_blocked_entry = dictAddRaw(c->db->blocking_keys_unblock_on_nokey, keys[j], &db_blocked_existing_entry);
            if (db_blocked_entry) {
                incrRefCount(keys[j]);
                dictSetUnsignedIntegerVal(db_blocked_entry, 1);
            } else {
                dictIncrUnsignedIntegerVal(db_blocked_existing_entry, 1);
            }
        }
    }
    c->bstate.unblock_on_nokey = unblock_on_nokey;
    /* Currently we assume key blocking will require reprocessing the command.
     * However in case of modules, they have a different way to handle the reprocessing
     * which does not require setting the pending command flag */
    if (btype != BLOCKED_MODULE)
        c->flags |= CLIENT_PENDING_COMMAND;
    blockClient(c,btype);
}

/* Helper function to unblock a client that's waiting in a blocking operation such as BLPOP.
 * Internal function for unblockClient() */
static void unblockClientWaitingData(client *c) {
    dictEntry *de;
    dictIterator *di;

    if (dictSize(c->bstate.keys) == 0)
        return;

    di = dictGetIterator(c->bstate.keys);
    /* The client may wait for multiple keys, so unblock it for every key. */
    while((de = dictNext(di)) != NULL) {
        releaseBlockedEntry(c, de, 0);
    }
    dictReleaseIterator(di);
    dictEmpty(c->bstate.keys, NULL);
}

static blocking_type getBlockedTypeByType(int type) {
    switch (type) {
        case OBJ_LIST: return BLOCKED_LIST;
        case OBJ_ZSET: return BLOCKED_ZSET;
        case OBJ_MODULE: return BLOCKED_MODULE;
        case OBJ_STREAM: return BLOCKED_STREAM;
        default: return BLOCKED_NONE;
    }
}

/* If the specified key has clients blocked waiting for list pushes, this
 * function will put the key reference into the server.ready_keys list.
 * Note that db->ready_keys is a hash table that allows us to avoid putting
 * the same key again and again in the list in case of multiple pushes
 * made by a script or in the context of MULTI/EXEC.
 *
 * The list will be finally processed by handleClientsBlockedOnKeys() */
static void signalKeyAsReadyLogic(redisDb *db, robj *key, int type, int deleted) {
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

    if (deleted) {
        /* Key deleted and no clients blocking for this key? No need to queue it. */
        if (dictFind(db->blocking_keys_unblock_on_nokey,key) == NULL)
            return;
        /* Note: if we made it here it means the key is also present in db->blocking_keys */
    } else {
        /* No clients blocking for this key? No need to queue it. */
        if (dictFind(db->blocking_keys,key) == NULL)
            return;
    }

    dictEntry *de, *existing;
    de = dictAddRaw(db->ready_keys, key, &existing);
    if (de) {
        /* We add the key in the db->ready_keys dictionary in order
         * to avoid adding it multiple times into a list with a simple O(1)
         * check. */
        incrRefCount(key);
    } else {
        /* Key was already signaled? No need to queue it again. */
        return;
    }

    /* Ok, we need to queue this key into server.ready_keys. */
    rl = zmalloc(sizeof(*rl));
    rl->key = key;
    rl->db = db;
    incrRefCount(key);
    listAddNodeTail(server.ready_keys,rl);
}

/* Helper function to wrap the logic of removing a client blocked key entry
 * In this case we would like to do the following:
 * 1. unlink the client from the global DB locked client list
 * 2. remove the entry from the global db blocking list in case the list is empty
 * 3. in case the global list is empty, also remove the key from the global dict of keys
 *    which should trigger unblock on key deletion
 * 4. remove key from the client blocking keys list - NOTE, since client can be blocked on lots of keys,
 *    but unblocked when only one of them is triggered, we would like to avoid deleting each key separately
 *    and instead clear the dictionary in one-shot. this is why the remove_key argument is provided
 *    to support this logic in unblockClientWaitingData
 */
static void releaseBlockedEntry(client *c, dictEntry *de, int remove_key) {
    list *l;
    listNode *pos;
    void *key;
    dictEntry *unblock_on_nokey_entry;

    key = dictGetKey(de);
    pos = dictGetVal(de);
    /* Remove this client from the list of clients waiting for this key. */
    l = dictFetchValue(c->db->blocking_keys, key);
    serverAssertWithInfo(c,key,l != NULL);
    listUnlinkNode(l,pos);
    /* If the list is empty we need to remove it to avoid wasting memory
     * We will also remove the key (if exists) from the blocking_keys_unblock_on_nokey dict.
     * However, in case the list is not empty, we will have to still perform reference accounting
     * on the blocking_keys_unblock_on_nokey and delete the entry in case of zero reference.
     * Why? because it is possible that some more clients are blocked on the same key but without
     * require to be triggered on key deletion, we do not want these to be later triggered by the
     * signalDeletedKeyAsReady. */
    if (listLength(l) == 0) {
        dictDelete(c->db->blocking_keys, key);
        dictDelete(c->db->blocking_keys_unblock_on_nokey,key);
    } else if (c->bstate.unblock_on_nokey) {
        unblock_on_nokey_entry = dictFind(c->db->blocking_keys_unblock_on_nokey,key);
        /* it is not possible to have a client blocked on nokey with no matching entry */
        serverAssertWithInfo(c,key,unblock_on_nokey_entry != NULL);
        if (!dictIncrUnsignedIntegerVal(unblock_on_nokey_entry, -1)) {
            /* in case the count is zero, we can delete the entry */
             dictDelete(c->db->blocking_keys_unblock_on_nokey,key);
        }
    }
    if (remove_key)
        dictDelete(c->bstate.keys, key);
}

void signalKeyAsReady(redisDb *db, robj *key, int type) {
    signalKeyAsReadyLogic(db, key, type, 0);
}

void signalDeletedKeyAsReady(redisDb *db, robj *key, int type) {
    signalKeyAsReadyLogic(db, key, type, 1);
}

/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * whenever a key is ready. we iterate over all the clients blocked on this key
 * and try to re-execute the command (in case the key is still available). */
static void handleClientsBlockedOnKey(readyList *rl) {

    /* We serve clients in the same order they blocked for
     * this key, from the first blocked to the last. */
    dictEntry *de = dictFind(rl->db->blocking_keys,rl->key);

    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients,&li);

        /* Avoid processing more than the initial count so that we're not stuck
         * in an endless loop in case the reprocessing of the command blocks again. */
        long count = listLength(clients);
        while ((ln = listNext(&li)) && count--) {
            client *receiver = listNodeValue(ln);
            robj *o = lookupKeyReadWithFlags(rl->db, rl->key, LOOKUP_NOEFFECTS);
            /* 1. In case new key was added/touched we need to verify it satisfy the
             *    blocked type, since we might process the wrong key type.
             * 2. We want to serve clients blocked on module keys
             *    regardless of the object type: we don't know what the
             *    module is trying to accomplish right now.
             * 3. In case of XREADGROUP call we will want to unblock on any change in object type
             *    or in case the key was deleted, since the group is no longer valid. */
            if ((o != NULL && (receiver->bstate.btype == getBlockedTypeByType(o->type))) ||
                (o != NULL && (receiver->bstate.btype == BLOCKED_MODULE)) ||
                (receiver->bstate.unblock_on_nokey))
            {
                if (receiver->bstate.btype != BLOCKED_MODULE)
                    unblockClientOnKey(receiver, rl->key);
                else
                    moduleUnblockClientOnKey(receiver, rl->key);
            }
        }
    }
}

/* block a client due to wait command */
void blockForReplication(client *c, mstime_t timeout, long long offset, long numreplicas) {
    c->bstate.timeout = timeout;
    c->bstate.reploffset = offset;
    c->bstate.numreplicas = numreplicas;
    listAddNodeHead(server.clients_waiting_acks,c);
    blockClient(c,BLOCKED_WAIT);
}

/* block a client due to waitaof command */
void blockForAofFsync(client *c, mstime_t timeout, long long offset, int numlocal, long numreplicas) {
    c->bstate.timeout = timeout;
    c->bstate.reploffset = offset;
    c->bstate.numreplicas = numreplicas;
    c->bstate.numlocal = numlocal;
    listAddNodeHead(server.clients_waiting_acks,c);
    blockClient(c,BLOCKED_WAITAOF);
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
}

/* Unblock a client once a specific key became available for it.
 * This function will remove the client from the list of clients blocked on this key
 * and also remove the key from the dictionary of keys this client is blocked on.
 * in case the client has a command pending it will process it immediately.  */
static void unblockClientOnKey(client *c, robj *key) {
    dictEntry *de;

    de = dictFind(c->bstate.keys, key);
    releaseBlockedEntry(c, de, 1);

    /* Only in case of blocking API calls, we might be blocked on several keys.
       however we should force unblock the entire blocking keys */
    serverAssert(c->bstate.btype == BLOCKED_STREAM ||
                c->bstate.btype == BLOCKED_LIST   ||
                c->bstate.btype == BLOCKED_ZSET);

    /* We need to unblock the client before calling processCommandAndResetClient
     * because it checks the CLIENT_BLOCKED flag */
    unblockClient(c, 0);
    /* In case this client was blocked on keys during command
     * we need to re process the command again */
    if (c->flags & CLIENT_PENDING_COMMAND) {
        c->flags &= ~CLIENT_PENDING_COMMAND;
        /* We want the command processing and the unblock handler (see RM_Call 'K' option)
         * to run atomically, this is why we must enter the execution unit here before
         * running the command, and exit the execution unit after calling the unblock handler (if exists).
         * Notice that we also must set the current client so it will be available
         * when we will try to send the client side caching notification (done on 'afterCommand'). */
        client *old_client = server.current_client;
        server.current_client = c;
        enterExecutionUnit(1, 0);
        processCommandAndResetClient(c);
        if (!(c->flags & CLIENT_BLOCKED)) {
            if (c->flags & CLIENT_MODULE) {
                moduleCallCommandUnblockedHandler(c);
            } else {
                queueClientForReprocessing(c);
            }
        }
        exitExecutionUnit();
        afterCommand(c);
        server.current_client = old_client;
    }
}

/* Unblock a client blocked on the specific key from module context.
 * This function will try to serve the module call, and in case it succeeds,
 * it will add the client to the list of module unblocked clients which will
 * be processed in moduleHandleBlockedClients. */
static void moduleUnblockClientOnKey(client *c, robj *key) {
    long long prev_error_replies = server.stat_total_error_replies;
    client *old_client = server.current_client;
    server.current_client = c;
    monotime replyTimer;
    elapsedStart(&replyTimer);

    if (moduleTryServeClientBlockedOnKey(c, key)) {
        updateStatsOnUnblock(c, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
        moduleUnblockClient(c);
    }
    /* We need to call afterCommand even if the client was not unblocked
     * in order to propagate any changes that could have been done inside
     * moduleTryServeClientBlockedOnKey */
    afterCommand(c);
    server.current_client = old_client;
}

/* Unblock a client which is currently Blocked on and provided a timeout.
 * The implementation will first reply to the blocked client with null response
 * or, in case of module blocked client the timeout callback will be used.
 * In this case since we might have a command pending
 * we want to remove the pending flag to indicate we already responded to the
 * command with timeout reply. */
void unblockClientOnTimeout(client *c) {
    /* The client has been unlocked (in the moduleUnblocked list), return ASAP. */
    if (c->bstate.btype == BLOCKED_MODULE && isModuleClientUnblocked(c)) return;

    replyToBlockedClientTimedOut(c);
    if (c->flags & CLIENT_PENDING_COMMAND)
        c->flags &= ~CLIENT_PENDING_COMMAND;
    unblockClient(c, 1);
}

/* Unblock a client which is currently Blocked with error.
 * If err_str is provided it will be used to reply to the blocked client */
void unblockClientOnError(client *c, const char *err_str) {
    if (err_str)
        addReplyError(c, err_str);
    updateStatsOnUnblock(c, 0, 0, 1);
    if (c->flags & CLIENT_PENDING_COMMAND)
        c->flags &= ~CLIENT_PENDING_COMMAND;
    unblockClient(c, 1);
}

void blockedBeforeSleep(void) {
    /* Handle precise timeouts of blocked clients. */
    handleBlockedClientsTimeout();

    /* Unblock all the clients blocked for synchronous replication
     * in WAIT or WAITAOF. */
    if (listLength(server.clients_waiting_acks))
        processClientsWaitingReplicas();

    /* Try to process blocked clients every once in while.
     *
     * Example: A module calls RM_SignalKeyAsReady from within a timer callback
     * (So we don't visit processCommand() at all).
     *
     * This may unblock clients, so must be done before processUnblockedClients */
    handleClientsBlockedOnKeys();

    /* Check if there are clients unblocked by modules that implement
     * blocking commands. */
    if (moduleCount())
        moduleHandleBlockedClients();

    /* Try to process pending commands for clients that were just unblocked. */
    if (listLength(server.unblocked_clients))
        processUnblockedClients();
}

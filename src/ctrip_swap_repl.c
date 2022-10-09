/* Copyright (c) 2021, ctrip.com
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
 */

#include "ctrip_swap.h"

/* ----------------------------- repl swap ------------------------------ */
int replClientDiscardDispatchedCommands(client *c) {
    int discarded = 0, scanned = 0;
    listIter li;
    listNode *ln;

    serverAssert(c);

    listRewind(server.repl_worker_clients_used,&li);
    while ((ln = listNext(&li))) {
        client *wc = listNodeValue(ln);
        if (wc->repl_client == c) {
            wc->CLIENT_REPL_CMD_DISCARDED = 1;
            discarded++;
            serverLog(LL_NOTICE, "discarded: cmd_reploff(%lld)", wc->cmd_reploff);
        }
        scanned++;
    }

    if (discarded) {
        serverLog(LL_NOTICE,
            "discard (%d/%d) dispatched but not executed commands for repl client(reploff:%lld, read_reploff:%lld)",
            discarded, scanned, c->reploff, c->read_reploff);
    }

    return discarded;
}

void replClientDiscardSwappingState(client *c) {
    listNode *ln;

    ln = listSearchKey(server.repl_swapping_clients, c);
    if (ln == NULL) return;

    listDelNode(server.repl_swapping_clients,ln);
    c->flags &= ~CLIENT_SWAPPING;
    serverLog(LL_NOTICE, "discarded: swapping repl client (reploff=%lld, read_reploff=%lld)", c->reploff, c->read_reploff);
}

/* Move command from repl client to repl worker client. */
static void replCommandDispatch(client *wc, client *c) {
    if (wc->argv) zfree(wc->argv);

    wc->argc = c->argc, c->argc = 0;
    wc->argv = c->argv, c->argv = NULL;
    wc->cmd = c->cmd;
    wc->lastcmd = c->lastcmd;
    wc->flags = c->flags;
    wc->cmd_reploff = c->cmd_reploff;
    wc->repl_client = c;

    /* Move repl client mstate to worker client if needed. */
    if (c->flags & CLIENT_MULTI) {
        c->flags &= ~CLIENT_MULTI;
        wc->mstate = c->mstate;
        initClientMultiState(c);
    }

    /* Swapping count is dispatched command count. Note that free repl
     * client would be defered untill swapping count drops to 0. */
    c->keyrequests_count++;
}

static void processFinishedReplCommands() {
    listNode *ln;
    client *wc, *c;
    struct redisCommand *backup_cmd;

    serverLog(LL_DEBUG, "> processFinishedReplCommands");

    while ((ln = listFirst(server.repl_worker_clients_used))) {
        wc = listNodeValue(ln);
        if (wc->CLIENT_REPL_SWAPPING) break;
        c = wc->repl_client;

        wc->flags &= ~CLIENT_SWAPPING;
        c->keyrequests_count--;
        listDelNode(server.repl_worker_clients_used, ln);
        listAddNodeTail(server.repl_worker_clients_free, wc);

        /* Discard dispatched but not executed commands like we never reveived, if
         * - repl client is closing: client close defered untill all swapping
         *   dispatched cmds finished, those cmds will be discarded.
         * - repl client is cached: client cached but read_reploff will shirnk
         *   back and dispatched cmd will be discared. */
        if (wc->CLIENT_REPL_CMD_DISCARDED) {
            commandProcessed(wc);
            serverAssert(wc->client_hold_mode == CLIENT_HOLD_MODE_REPL);
            clientUnholdKeys(wc);
            clientReleaseRequestLocks(wc,NULL/*ctx unused*/);
            wc->CLIENT_REPL_CMD_DISCARDED = 0;
            continue;
        } else {
            serverAssert(c->flags&CLIENT_MASTER);
        }

        backup_cmd = c->cmd;
        c->cmd = wc->cmd;
        server.current_client = c;

        if (wc->swap_errcode) {
            rejectCommandFormat(c,"Swap failed (code=%d)",wc->swap_errcode);
            wc->swap_errcode = 0;
        } else {
            call(wc, CMD_CALL_FULL);

            /* post call */
            c->woff = server.master_repl_offset;
            if (listLength(server.ready_keys))
                handleClientsBlockedOnKeys();
        }

        c->db = wc->db;
        c->cmd = backup_cmd;

        commandProcessed(wc);
        clientUnholdKeys(wc);

        serverAssert(wc->client_hold_mode == CLIENT_HOLD_MODE_REPL);

        long long prev_offset = c->reploff;
        /* update reploff */
        if (c->flags&CLIENT_MASTER) {
            /* transaction commands wont dispatch to worker client untill
             * exec (queued by repl client), so worker client wont have 
             * CLIENT_MULTI flag after call(). */
            serverAssert(!(wc->flags & CLIENT_MULTI));
            /* Update the applied replication offset of our master. */
            c->reploff = wc->cmd_reploff;
        }

		/* If the client is a master we need to compute the difference
		 * between the applied offset before and after processing the buffer,
		 * to understand how much of the replication stream was actually
		 * applied to the master state: this quantity, and its corresponding
		 * part of the replication stream, will be propagated to the
		 * sub-replicas and to the replication backlog. */
		if ((c->flags&CLIENT_MASTER)) {
			size_t applied = c->reploff - prev_offset;
			if (applied) {
				if(!server.repl_slave_repl_all){
					replicationFeedSlavesFromMasterStream(server.slaves,
							c->pending_querybuf, applied);
				}
				sdsrange(c->pending_querybuf,applied,-1);
			}
		}

        clientReleaseRequestLocks(wc,NULL/*ctx unused*/);
    }
    serverLog(LL_DEBUG, "< processFinishedReplCommands");
}
     
void replWorkerClientKeyRequestFinished(client *wc, swapCtx *ctx) {
    client *c;
    listNode *ln;
    list *repl_swapping_clients;
    UNUSED(ctx);

    serverLog(LL_DEBUG, "> replWorkerClientSwapFinished client(id=%ld,cmd=%s,key=%s)",
        wc->id,wc->cmd->name,wc->argc <= 1 ? "": (sds)wc->argv[1]->ptr);

    DEBUG_MSGS_APPEND(&ctx->msgs, "request-finished", "errcode=%d",ctx->errcode);

    if (ctx->errcode) clientSwapError(wc,ctx->errcode);
    keyRequestBeforeCall(wc,ctx);

    /* Flag swap finished, note that command processing will be defered to
     * processFinishedReplCommands becasue there might be unfinished preceeding swap. */
    wc->keyrequests_count--;
    if (wc->keyrequests_count == 0) wc->CLIENT_REPL_SWAPPING = 0;

    processFinishedReplCommands();

    /* Dispatch repl command again for repl client blocked waiting free
     * worker repl client, because repl client might already read repl requests
     * into querybuf, read event will not trigger if we do not parse and
     * process again.  */
    if (!listFirst(server.repl_swapping_clients) ||
            !listFirst(server.repl_worker_clients_free)) {
        serverLog(LL_DEBUG, "< replWorkerClientSwapFinished");
        return;
    }

    repl_swapping_clients = server.repl_swapping_clients;
    server.repl_swapping_clients = listCreate();
    while ((ln = listFirst(repl_swapping_clients))) {
        int swap_result;

        c = listNodeValue(ln);
        /* Swapping repl clients are bound to:
         * - have pending parsed but not processed commands
         * - in server.repl_swapping_client list
         * - flag have CLIENT_SWAPPING */
        serverAssert(c->argc);
        serverAssert(c->flags & CLIENT_SWAPPING);

        /* Must make sure swapping clients satistity above constrains. also
         * note that repl client never call(only dispatch). */
        c->flags &= ~CLIENT_SWAPPING;
        swap_result = submitReplClientRequests(c);
        /* replClientSwap return 1 on dispatch fail, -1 on dispatch success,
         * never return 0. */
        if (swap_result > 0) {
            c->flags |= CLIENT_SWAPPING;
        } else {
            commandProcessed(c);
        }

        /* TODO confirm whether server.current_client == NULL possible */
        processInputBuffer(c);

        listDelNode(repl_swapping_clients,ln);
    }
    listRelease(repl_swapping_clients);

    serverLog(LL_DEBUG, "< replWorkerClientSwapFinished");
}

int submitReplWorkerClientRequest(client *wc) {
    getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
    getKeyRequests(wc, &result);
    wc->keyrequests_count = result.num;
    submitClientKeyRequests(wc,&result,replWorkerClientKeyRequestFinished);
    releaseKeyRequests(&result);
    getKeyRequestsFreeResult(&result);
    return result.num;
}

/* Different from original replication stream process, slave.master client
 * might trigger swap and block untill rocksdb IO finish. because there is
 * only one master client so rocksdb IO will be done sequentially, thus slave
 * can't catch up with master. 
 * In order to speed up replication stream processing, slave.master client
 * dispatches command to multiple worker client and execute commands when 
 * rocks IO finishes. Note that replicated commands swap in-parallel but
 * processed in received order. */
int submitReplClientRequests(client *c) {
    client *wc;
    listNode *ln;

    c->cmd_reploff = c->read_reploff - sdslen(c->querybuf) + c->qb_pos;
    serverAssert(!(c->flags & CLIENT_SWAPPING));
    if (!(ln = listFirst(server.repl_worker_clients_free))) {
        /* return swapping if there are no worker to dispatch, so command
         * processing loop would break out.
         * Note that peer client might register no rocks callback but repl
         * stream read and parsed, we need to processInputBuffer again. */
        listAddNodeTail(server.repl_swapping_clients, c);
        /* Note repl client will be flagged CLIENT_SWAPPING when return. */
        return 1;
    }

    wc = listNodeValue(ln);
    serverAssert(wc && !wc->CLIENT_REPL_SWAPPING);

    /* Because c is a repl client, only normal multi {cmd} exec will be
     * received (multiple multi, exec without multi, ... will no happen) */
    if (c->cmd->proc == multiCommand) {
        serverAssert(!(c->flags & CLIENT_MULTI));
        c->flags |= CLIENT_MULTI;
    } else if (c->flags & CLIENT_MULTI &&
            c->cmd->proc != execCommand) {
        serverPanic("command should be already queued.");
    } else {
        /* either vanilla command or transaction are stored in client state,
         * client is ready to dispatch now. */
        replCommandDispatch(wc, c);

        /* swap data for replicated commands, note that command will be
         * processed later in processFinishedReplCommands untill all preceeding
         * commands finished. */
        submitReplWorkerClientRequest(wc);
        wc->CLIENT_REPL_SWAPPING = wc->keyrequests_count;

        listDelNode(server.repl_worker_clients_free, ln);
        listAddNodeTail(server.repl_worker_clients_used, wc);
    }

    /* process repl commands in received order (not swap finished order) so
     * that slave is consistent with master. */
    processFinishedReplCommands();

    /* return dispatched(-1) when repl dispatched command to workers, caller
     * should skip call and continue command processing loop. */
    return -1;
}


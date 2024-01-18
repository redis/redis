/* Slowlog implements a system that is able to remember the latest N
 * queries that took more than M microseconds to execute.
 *
 * The execution time to reach to be logged in the slow log is set
 * using the 'slowlog-log-slower-than' config directive, that is also
 * readable and writable using the CONFIG SET/GET command.
 *
 * Similarly, fatlog remembers the latest N queries that has a response
 * larger than K bytes.
 *
 * The size of the response to reach to be logged in the fat lof is set
 * using the 'fatlog-log-bigger-than' config directive, that is also
 * readable and writable using the CONFIG SET/GET command.
 *
 * Both logs are actually not "logged" in the Redis log file but are
 * accessible thanks to the SLOWLOG/FATLOG command.
 *
 * ----------------------------------------------------------------------------
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
 */

#include "server.h"
#include "slowfatlog.h"

/* Create a new slowlog/fatlog entry.
 * Incrementing the ref count of all the objects retained is up to
 * this function. */
slowfatlogEntry *slowfatlogCreateEntry(client *c, robj **argv, int argc, long long statistic, long long id) {
    slowfatlogEntry *se = zmalloc(sizeof(*se));
    int j, slargc = argc;

    if (slargc > SLOWFATLOG_ENTRY_MAX_ARGC) slargc = SLOWFATLOG_ENTRY_MAX_ARGC;
    se->argc = slargc;
    se->argv = zmalloc(sizeof(robj*)*slargc);
    for (j = 0; j < slargc; j++) {
        /* Logging too many arguments is a useless memory waste, so we stop
         * at SLOWFATLOG_ENTRY_MAX_ARGC, but use the last argument to specify
         * how many remaining arguments there were in the original command. */
        if (slargc != argc && j == slargc-1) {
            se->argv[j] = createObject(OBJ_STRING,
                sdscatprintf(sdsempty(),"... (%d more arguments)",
                argc-slargc+1));
        } else {
            /* Trim too long strings as well... */
            if (argv[j]->type == OBJ_STRING &&
                sdsEncodedObject(argv[j]) &&
                sdslen(argv[j]->ptr) > SLOWFATLOG_ENTRY_MAX_STRING)
            {
                sds s = sdsnewlen(argv[j]->ptr, SLOWFATLOG_ENTRY_MAX_STRING);

                s = sdscatprintf(s,"... (%lu more bytes)",
                    (unsigned long)
                    sdslen(argv[j]->ptr) - SLOWFATLOG_ENTRY_MAX_STRING);
                se->argv[j] = createObject(OBJ_STRING,s);
            } else if (argv[j]->refcount == OBJ_SHARED_REFCOUNT) {
                se->argv[j] = argv[j];
            } else {
                /* Here we need to duplicate the string objects composing the
                 * argument vector of the command, because those may otherwise
                 * end shared with string objects stored into keys. Having
                 * shared objects between any part of Redis, and the data
                 * structure holding the data, is a problem: FLUSHALL ASYNC
                 * may release the shared string object and create a race. */
                se->argv[j] = dupStringObject(argv[j]);
            }
        }
    }
    se->time = time(NULL);
    se->statistic = statistic;
    se->id = id;
    se->peerid = sdsnew(getClientPeerId(c));
    se->cname = c->name ? sdsnew(c->name->ptr) : sdsempty();
    return se;
}

/* Free a slow/fat log entry. The argument is void so that the prototype of this
 * function matches the one of the 'free' method of adlist.c.
 *
 * This function will take care to release all the retained object. */
void slowfatlogFreeEntry(void *septr) {
    slowfatlogEntry *se = septr;
    int j;

    for (j = 0; j < se->argc; j++)
        decrRefCount(se->argv[j]);
    zfree(se->argv);
    sdsfree(se->peerid);
    sdsfree(se->cname);
    zfree(se);
}

/* Initialize the slow log and fat log. This function should be called a single
 * time at server startup. */
void slowfatlogInit(void) {
    server.slowlog = listCreate();
    server.slowlog_entry_id = 0;
    listSetFreeMethod(server.slowlog, slowfatlogFreeEntry);
    server.fatlog = listCreate();
    server.fatlog_entry_id = 0;
    listSetFreeMethod(server.fatlog, slowfatlogFreeEntry);
}

/* Push a new entry into the slow log.
 * This function will make sure to trim the slow log accordingly to the
 * configured max length. */
void slowlogPushEntryIfNeeded(client *c, robj **argv, int argc, long long duration) {
    if (server.slowlog_log_slower_than < 0) return; /* Slowlog disabled */
    if (duration >= server.slowlog_log_slower_than)
        listAddNodeHead(server.slowlog,
                        slowfatlogCreateEntry(c,argv,argc,duration,server.slowlog_entry_id++));

    /* Remove old entries if needed. */
    while (listLength(server.slowlog) > server.slowlog_max_len)
        listDelNode(server.slowlog,listLast(server.slowlog));
}

/* Push a new entry into the fat log.
 * This function will make sure to trim the fat log accordingly to the
 * configured max length. */
void fatlogPushEntryIfNeeded(client *c, robj **argv, int argc, long long statistic) {
    if (server.fatlog_log_bigger_than < 0) return; /* Fatlog disabled */
    if (statistic >= server.fatlog_log_bigger_than)
        listAddNodeHead(server.fatlog,
                        slowfatlogCreateEntry(c,argv,argc,statistic,server.fatlog_entry_id++));

    /* Remove old entries if needed. */
    while (listLength(server.fatlog) > server.fatlog_max_len)
        listDelNode(server.fatlog,listLast(server.fatlog));
}

/* The SLOWLOG/FATLOG command. Implements all the subcommands needed to handle the
 * Redis slow log and fat log. */
void slowfatlogCommand(client *c) {
    const char *slowlog_help[] = {
"GET [<count>]",
"    Return top <count> entries from the slowlog (default: 10, -1 mean all).",
"    Entries are made of:",
"    id, timestamp, time in microseconds, arguments array, client IP and port,",
"    client name",
"LEN",
"    Return the length of the slowlog.",
"RESET",
"    Reset the slowlog.",
NULL
    };
    const char *fatlog_help[] = {
"GET [<count>]",
"    Return top <count> entries from the fatlog (default: 10, -1 mean all).",
"    Entries are made of:",
"    id, timestamp, size in bytes, arguments array, client IP and port,",
"    client name",
"LEN",
"    Return the length of the fatlog.",
"RESET",
"    Reset the fatlog.",
NULL
    };

    list *log_list;
    const char **help_content;
    if (strcasecmp("SLOWLOG", c->argv[0]->ptr) == 0) {
        log_list = server.slowlog;
        help_content = slowlog_help;
    } else {
        log_list = server.fatlog;
        help_content = fatlog_help;
    }

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        addReplyHelp(c, help_content);
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"reset")) {
        listEmpty(log_list);
        addReply(c, shared.ok);
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"len")) {
        addReplyLongLong(c,listLength(log_list));
    } else if ((c->argc == 2 || c->argc == 3) &&
               !strcasecmp(c->argv[1]->ptr,"get"))
    {
        long count = 10;
        listIter li;
        listNode *ln;
        slowfatlogEntry *se;

        if (c->argc == 3) {
            /* Consume count arg. */
            if (getRangeLongFromObjectOrReply(c, c->argv[2], -1,
                    LONG_MAX, &count, "count should be greater than or equal to -1") != C_OK)
                return;

            if (count == -1) {
                /* We treat -1 as a special value, which means to get all slow logs.
                 * Simply set count to the length of server.slowlog/server.fatlog.*/
                count = listLength(log_list);
            }
        }

        if (count > (long)listLength(log_list)) {
            count = listLength(log_list);
        }
        addReplyArrayLen(c, count);
        listRewind(log_list, &li);
        while (count--) {
            int j;

            ln = listNext(&li);
            se = ln->value;
            addReplyArrayLen(c,6);
            addReplyLongLong(c,se->id);
            addReplyLongLong(c,se->time);
            addReplyLongLong(c,se->statistic);
            addReplyArrayLen(c,se->argc);
            for (j = 0; j < se->argc; j++)
                addReplyBulk(c,se->argv[j]);
            addReplyBulkCBuffer(c,se->peerid,sdslen(se->peerid));
            addReplyBulkCBuffer(c,se->cname,sdslen(se->cname));
        }
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

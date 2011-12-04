#include "redis.h"
#include "slowlog.h"

/* Slowlog implements a system that is able to remember the latest N
 * queries that took more than M microseconds to execute.
 *
 * The execution time to reach to be logged in the slow log is set
 * using the 'slowlog-log-slower-than' config directive, that is also
 * readable and writable using the CONFIG SET/GET command.
 *
 * The slow queries log is actually not "logged" in the Redis log file
 * but is accessible thanks to the SLOWLOG command. 
 *
 * slowlog len - returns length of slowlog list
 * slowlog reset - resets slowlog
 * slowlog get n - gets all members of slowlog from 0 - n
 * slowlog get n p -  gets all members of slowlog from n - p
 *
 */

/* Create a new slowlog entry.
 * Incrementing the ref count of all the objects retained is up to
 * this function. */
slowlogEntry *slowlogCreateEntry(robj **argv, int argc, long long duration) {
    slowlogEntry *se = zmalloc(sizeof(*se));
    int j;

    se->argc = argc;
    se->argv = zmalloc(sizeof(robj*)*argc);
    for (j = 0; j < argc; j++) {
        se->argv[j] = argv[j];
        incrRefCount(argv[j]);
    }
    se->time = time(NULL);
    se->duration = duration;
    se->id = server.slowlog_entry_id++;
    return se;
}

/* Free a slow log entry. The argument is void so that the prototype of this
 * function matches the one of the 'free' method of adlist.c.
 *
 * This function will take care to release all the retained object. */
void slowlogFreeEntry(void *septr) {
    slowlogEntry *se = septr;
    int j;

    for (j = 0; j < se->argc; j++)
        decrRefCount(se->argv[j]);
    zfree(se->argv);
    zfree(se);
}

/* Initialize the slow log. This function should be called a single time
 * at server startup. */
void slowlogInit(void) {
    server.slowlog = listCreate();
    server.slowlog_entry_id = 0;
    listSetFreeMethod(server.slowlog,slowlogFreeEntry);
}

/* Push a new entry into the slow log.
 * This function will make sure to trim the slow log accordingly to the
 * configured max length. */
void slowlogPushEntryIfNeeded(robj **argv, int argc, long long duration) {
    if (server.slowlog_log_slower_than < 0) return; /* Slowlog disabled */
    if (duration >= server.slowlog_log_slower_than)
        listAddNodeHead(server.slowlog,slowlogCreateEntry(argv,argc,duration));

    /* Remove old entries if needed. */
    while (listLength(server.slowlog) > server.slowlog_max_len)
        listDelNode(server.slowlog,listLast(server.slowlog));
}

/* Remove all the entries from the current slow log. */
void slowlogReset(void) {
    while (listLength(server.slowlog) > 0)
        listDelNode(server.slowlog,listLast(server.slowlog));
}

/* The SLOWLOG command. Implements all the subcommands needed to handle the
 * Redis slow log. */
void slowlogCommand(redisClient *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"reset")) {
        slowlogReset();
        addReply(c,shared.ok);
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"len")) {
        addReplyLongLong(c,listLength(server.slowlog));
    } else if ((c->argc == 2 || c->argc == 3 || c->argc == 4) &&
               !strcasecmp(c->argv[1]->ptr,"get"))
    {
        long count = 10, start_range = 10, sent = 0, end_range = 0;
        listIter li;
        void *totentries;
        listNode *ln;
        slowlogEntry *se;

        listRewind(server.slowlog,&li);

        if (c->argc == 3){
            if (getLongFromObjectOrReply(c,c->argv[2],&start_range,NULL) != REDIS_OK)
                return;
            count = start_range;
        } else if (c->argc == 4){
            if (getLongFromObjectOrReply(c,c->argv[2],&start_range,NULL) != REDIS_OK)
                return;
            if (getLongFromObjectOrReply(c,c->argv[3],&end_range,NULL) != REDIS_OK)
                return;

            if(start_range > end_range){
                long cpy = end_range;
                end_range = start_range;
                start_range = cpy;
            }

            count = end_range - start_range;

            listNode *ln_in;
            long list_index = 1;
            while(list_index < start_range){
              ln_in = listNext(&li);
              list_index++;
            };
        }

        totentries = addDeferredMultiBulkLength(c);

        while(count-- && (ln = listNext(&li))) {
            int j;

            se = ln->value;
            addReplyMultiBulkLen(c,4);
            addReplyLongLong(c,se->id);
            addReplyLongLong(c,se->time);
            addReplyLongLong(c,se->duration);
            addReplyMultiBulkLen(c,se->argc);
            for (j = 0; j < se->argc; j++)
                addReplyBulk(c,se->argv[j]);
            sent++;
        }
        setDeferredMultiBulkLength(c,totentries,sent);



    } else {
        addReplyError(c,
            "Unknown SLOWLOG subcommand or wrong # of args. Try GET, RESET, LEN.");
    }
}

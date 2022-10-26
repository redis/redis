#include "server.h"
#include <ctrip_gtid.h>
#define GTID_COMMAN_ARGC 3

/**
 * @brief 
 *       redis.conf:   
 *          gtid-enabled yes (open)
 * @return int 
 *  
 */
int isGtidEnabled() {
    return server.gtid_enabled;
}


/**
 * @brief 
 *      1 (yes)
 *      0 (no)
 * @param c 
 * @return int 
 *       
 */
int isGtidExecCommand(client* c) {
    return c->cmd->proc == gtidCommand && c->argc > GTID_COMMAN_ARGC && strcasecmp(c->argv[GTID_COMMAN_ARGC]->ptr, "exec") == 0;
}

/**
* @brief
*      gtid.auto {comment} set k v => gtid {gtid_str} {dbid} {comment}
*
*/
void gtidAutoCommand(client* c) {
    if(strncmp(c->argv[1]->ptr, "/*", 2) != 0) {
        addReplyErrorFormat(c,"gtid.auto comment format error:%s", (char*)c->argv[1]->ptr);
        return;
    }
    int argc = c->argc;
    robj** argv = c->argv;
    struct redisCommand* cmd = c->cmd;
    c->argc = argc - 2;
    c->argv = argv + 2;
    c->cmd = c->lastcmd = lookupCommand(c->argv[0]->ptr);
    if (!c->cmd) {
        sds args = sdsempty();
        int i;
        for (i=1; i < c->argc && sdslen(args) < 128; i++)
            args = sdscatprintf(args, "`%.*s`, ", 128-(int)sdslen(args), (char*)c->argv[i]->ptr);
        serverLog(LL_WARNING, "unknown command `%s`, with args beginning with: %s",
            (char*)c->argv[0]->ptr, args);
        rejectCommandFormat(c,"unknown command `%s`, with args beginning with: %s",
            (char*)c->argv[0]->ptr, args);
        sdsfree(args);
        goto end;
        return;
    } else if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) ||
               (c->argc < -c->cmd->arity)) {
        serverLog(LL_WARNING,"wrong number of arguments for '%s' command",
            c->cmd->name);
        rejectCommandFormat(c,"wrong number of arguments for '%s' command",
            c->cmd->name);
        goto end;
        return;
    }
    c->cmd->proc(c);
    server.dirty++;
end:
    c->argc = argc;
    c->argv = argv;
    c->cmd = cmd;
}


/**
 * @brief 
 *      1. gtid A:1 {db} set k v
 *      2. gtid A:1 {db} exec   
 *          a. fail (clean queue)
 *      3. gtid A:1 {db} \/\*comment\*\/ set k v
 * 
 */
void gtidCommand(client *c) {
    sds gtid = c->argv[1]->ptr;
    long long gno = 0;
    int rpl_sid_len = 0;
    char* rpl_sid = uuidDecode(gtid, sdslen(gtid), &gno, &rpl_sid_len);
    if (rpl_sid == NULL) {
        addReplyErrorFormat(c,"gtid format error:%s", gtid);
        return;
    }
    int id = 0;
    if (getIntFromObjectOrReply(c, c->argv[2], &id, NULL) != C_OK)
        return;
    
    if (selectDb(c, id) == C_ERR) {
        addReplyError(c,"DB index is out of range");
        return;
    }
    uuidSet* uuid_set = gtidSetFindUuidSet(server.gtid_executed, rpl_sid, rpl_sid_len);
    if(uuid_set != NULL && uuidSetContains(uuid_set, gno)) {
        sds args = sdsempty();
        for (int i=1, len=GTID_COMMAN_ARGC + 1; i < len && sdslen(args) < 128; i++) {
            args = sdscatprintf(args, "`%.*s`, ", 128-(int)sdslen(args), (char*)c->argv[i]->ptr);
        }
        addReplyErrorFormat(c,"gtid command is executed, %s",args);
        sdsfree(args);
        if(isGtidExecCommand(c)) {
            //clean multi queue
            discardTransaction(c);
        }
        return;
    }
    int argc = c->argc;
    robj** argv = c->argv;
    
    struct redisCommand* cmd = c->cmd;
    robj** newargv = zmalloc(sizeof(struct robj*) * argc);
    int gtid_argc = 3;
    if(strncmp(c->argv[3]->ptr,"/*", 2) == 0) {
        gtid_argc = 4;
    } 
    c->argc = argc - gtid_argc;
    for(int i = 0; i < c->argc; i++) {
        newargv[i] = argv[i + gtid_argc];
        incrRefCount(newargv[i]);
    }
    c->argv = newargv;
    c->cmd = c->lastcmd = lookupCommand(c->argv[0]->ptr);
    if (!c->cmd) {
        sds args = sdsempty();
        int i;
        for (i=1; i < c->argc && sdslen(args) < 128; i++)
            args = sdscatprintf(args, "`%.*s`, ", 128-(int)sdslen(args), (char*)c->argv[i]->ptr);
        serverLog(LL_WARNING, "unknown command `%s`, with args beginning with: %s",
            (char*)c->argv[0]->ptr, args);
        rejectCommandFormat(c,"unknown command `%s`, with args beginning with: %s",
            (char*)c->argv[0]->ptr, args);
        sdsfree(args);
        goto end;
        return;
    } else if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) ||
               (c->argc < -c->cmd->arity)) {
        serverLog(LL_WARNING,"wrong number of arguments for '%s' command",
            c->cmd->name);
        rejectCommandFormat(c,"wrong number of arguments for '%s' command",
            c->cmd->name);
        goto end;
        return;
    }
    c->cmd->proc(c);
    for(int i = 0; i < c->argc; i++) {
        decrRefCount(c->argv[i]);
    }
    zfree(c->argv);
    c->argv = NULL;
    int result = 0;
    if(uuid_set != NULL) {
        result = uuidSetAdd(uuid_set, gno);
    } else {
        result = gtidSetAdd(server.gtid_executed, rpl_sid, rpl_sid_len, gno);
    }
    serverAssert(result == 1);
end: 
    c->argc = argc;
    c->argv = argv;
    c->cmd = cmd;
}

/**
 * @brief 
 *      when master expire, send gtid command
 * @param db 
 * @param key 
 * @param lazy 
 *  
 */
void propagateGtidExpire(redisDb *db, robj *key, int lazy) {
    int argc = 2 + GTID_COMMAN_ARGC;
    robj *argv[argc];
    argv[0] = shared.gtid;
    char buf[uuidSetEstimatedEncodeBufferSize(server.current_uuid)];
    size_t size = uuidSetNextEncode(server.current_uuid, 1, buf);
    argv[1] = createObject(OBJ_STRING, sdsnewlen(buf, size));
    argv[2] = createObject(OBJ_STRING, sdscatprintf(sdsempty(), 
        "%d", db->id));

    argv[0 + GTID_COMMAN_ARGC] = lazy ? shared.unlink : shared.del;
    argv[1 + GTID_COMMAN_ARGC] = key;

    if (server.aof_state != AOF_OFF)
        feedAppendOnlyFile(server.delCommand,db->id,argv,argc);
    replicationFeedSlaves(server.slaves,db->id,argv,argc);

    decrRefCount(argv[1]);
    decrRefCount(argv[2]);
}

int isGtidInMerge(client* c) {
    return c->gtid_in_merge;
}

/**
 * @brief 
 *      normal command -> gtid command
 *      set k v  -> gtid {gtid_str} set k v
 * @param cmd 
 * @param dbid 
 * @param argv 
 * @param argc 
 * @param flags 
 * @return int 
 */
int execCommandPropagateGtid(struct redisCommand *cmd, int dbid, robj **argv, int argc,
               int flags) {
    if(!isGtidEnabled()) {
        return 0;
    }
    if (cmd == server.gtidCommand) {
        return 0;
    }

    if (cmd == server.gtidLwmCommand) {
        return 0;
    }

    if (cmd == server.gtidMergeStartCommand || cmd == server.gtidMergeEndCommand) {
        return 0;
    }

    if(server.in_exec && cmd != server.execCommand) {
        return 0;
    }

    if (cmd == server.multiCommand) {
        return 0;
    }
    robj *gtidArgv[argc+3]; /*FIXME: overflow? */
    gtidArgv[0] = shared.gtid;
    char buf[uuidSetEstimatedEncodeBufferSize(server.current_uuid)]; /* FIXME: overflow? */
    size_t len = uuidSetNextEncode(server.current_uuid, 1, buf);
    gtidArgv[1] = createObject(OBJ_STRING, sdsnewlen(buf, len));/*TODO opt: use static string object */
    if (cmd == server.execCommand &&  server.db_at_multi != NULL) {
        gtidArgv[2] = createObject(OBJ_STRING, sdscatprintf(sdsempty(), 
        "%d", server.db_at_multi->id));
    } else {
        gtidArgv[2] = createObject(OBJ_STRING, sdscatprintf(sdsempty(), 
        "%d", dbid));
    }
    if(cmd == server.gtidAutoCommand) {
        for(int i = 0; i < argc-1; i++) {
            gtidArgv[i+3] = argv[i+1];
        }
        propagate(server.gtidCommand, dbid, gtidArgv, argc+2, flags);
    } else {
        for(int i = 0; i < argc; i++) {
            gtidArgv[i+3] = argv[i];
        }
        propagate(server.gtidCommand, dbid, gtidArgv, argc+3, flags);
    }
    decrRefCount(gtidArgv[1]);
    decrRefCount(gtidArgv[2]);
    return 1;
}

/**
 * @brief 
 *      gtid expireat command append buffer
 * @param buf 
 * @param gtid 
 * @param cmd 
 * @param key 
 * @param seconds 
 * @return sds 
 */
sds catAppendOnlyGtidExpireAtCommand(sds buf, robj* gtid, robj* dbid, robj* comment, struct redisCommand *cmd,  robj *key, robj *seconds) {
    long long when;
    robj *argv[7];

    /* Make sure we can use strtoll */
    seconds = getDecodedObject(seconds);
    when = strtoll(seconds->ptr,NULL,10);
    /* Convert argument into milliseconds for EXPIRE, SETEX, EXPIREAT */
    if (cmd->proc == expireCommand || cmd->proc == setexCommand ||
        cmd->proc == expireatCommand)
    {
        when *= 1000;
    }
    /* Convert into absolute time for EXPIRE, PEXPIRE, SETEX, PSETEX */
    if (cmd->proc == expireCommand || cmd->proc == pexpireCommand ||
        cmd->proc == setexCommand || cmd->proc == psetexCommand)
    {
        when += mstime();
    }
    decrRefCount(seconds);
    int index = 0, time_index = 0;
    argv[index++] = shared.gtid;
    argv[index++] = gtid;
    argv[index++] = dbid;
    if(comment != NULL) {
        argv[index++] = comment;
    }
    argv[index++] = shared.pexpireat;
    argv[index++] = key;
    time_index = index;
    argv[index++] = createStringObjectFromLongLong(when);
    buf = catAppendOnlyGenericCommand(buf, index, argv);
    decrRefCount(argv[time_index]);
    return buf;
}

/**
 * @brief 
 *        gtid expire => gtid expireat 
 *        gtid setex =>  set  + gtid expireat 
 *        gtid set(ex) => set + gtid expireat
 * @param buf 
 * @param cmd 
 * @param argv 
 * @param argc 
 * @return sds 
 */
sds gtidCommandTranslate(sds buf, struct redisCommand *cmd, robj **argv, int argc) {
    if(cmd == server.gtidCommand) {
        int index = 3;
        if(strncmp(argv[index]->ptr,"/*",2 ) == 0)  {
            index++;
        }
        struct redisCommand *c = lookupCommand(argv[index]->ptr);
        if (c->proc == expireCommand || c->proc == pexpireCommand ||
            c->proc == expireatCommand) {
            /* Translate EXPIRE/PEXPIRE/EXPIREAT into PEXPIREAT */
            if(index == 3) {
                buf = catAppendOnlyGtidExpireAtCommand(buf,argv[1],argv[2],NULL,c,argv[index+1],argv[index+2]);
            } else {
                buf = catAppendOnlyGtidExpireAtCommand(buf,argv[1],argv[2],argv[3],c,argv[index+1],argv[index+2]);
            }
        } else if (c->proc == setCommand && argc > 3 +  GTID_COMMAN_ARGC) {
            robj *pxarg = NULL;
            /* When SET is used with EX/PX argument setGenericCommand propagates them with PX millisecond argument.
            * So since the command arguments are re-written there, we can rely here on the index of PX being 3. */
            if (!strcasecmp(argv[3 + GTID_COMMAN_ARGC]->ptr, "px")) {
                pxarg = argv[4 + GTID_COMMAN_ARGC];
            }
            /* For AOF we convert SET key value relative time in milliseconds to SET key value absolute time in
            * millisecond. Whenever the condition is true it implies that original SET has been transformed
            * to SET PX with millisecond time argument so we do not need to worry about unit here.*/
            if (pxarg) {
                robj *millisecond = getDecodedObject(pxarg);
                long long when = strtoll(millisecond->ptr,NULL,10);
                when += mstime();

                decrRefCount(millisecond);

                robj *newargs[5 + GTID_COMMAN_ARGC];
                for(int i = 0, len = 3 + GTID_COMMAN_ARGC; i < len; i++) {
                    newargs[i] = argv[i];
                }
                newargs[3 + GTID_COMMAN_ARGC] = shared.pxat;
                newargs[4 + GTID_COMMAN_ARGC] = createStringObjectFromLongLong(when);
                buf = catAppendOnlyGenericCommand(buf,5 + GTID_COMMAN_ARGC,newargs);
                decrRefCount(newargs[4 + GTID_COMMAN_ARGC]);
            } else {
                buf = catAppendOnlyGenericCommand(buf,argc,argv);
            }
        } else {
            buf = catAppendOnlyGenericCommand(buf,argc,argv);
        }
    } else {
        buf = catAppendOnlyGenericCommand(buf,argc,argv);
    }
    return buf;
}


/**
 * @brief 
 *      gtid.lwm command
 * @param c 
 */
void gtidLwmCommand(client* c) {
    sds rpl_sid = c->argv[1]->ptr;
    long long rpl_gno = 0;
    sds rpl_gno_str = c->argv[2]->ptr;
    if(string2ll(rpl_gno_str, sdslen(rpl_gno_str), &rpl_gno) == -1) {
        addReply(c, shared.err);
        return;
    }
    gtidSetRaise(server.gtid_executed,rpl_sid, strlen(rpl_sid), rpl_gno);
    server.dirty++;
    addReply(c, shared.ok);
}

/**
 * @brief 
 *      save rdb (add gtid field)
 * @param rdb 
 * @return int 
 */
int rdbSaveGtidInfoAuxFields(rio* rdb) {
    char gtid_str[gtidSetEstimatedEncodeBufferSize(server.gtid_executed)];
    size_t gtid_str_len = gtidSetEncode(server.gtid_executed, gtid_str);
    if (rdbSaveAuxField(rdb, "gtid", 4, gtid_str, gtid_str_len)
        == -1) {
        return -1;
    }   
    return 1;
}

/**
 * @brief 
 * 
 * @param key 
 * @param val 
 * @return int 
 * 
 */
int LoadGtidInfoAuxFields(robj* key, robj* val) {
    if (!strcasecmp(key->ptr, "gtid")) {
        if(server.gtid_executed != NULL) {
            gtidSetFree(server.gtid_executed);
            server.gtid_executed = NULL;
        }
        server.gtid_executed = gtidSetDecode(val->ptr, sdslen(val->ptr));
        server.current_uuid = gtidSetFindUuidSet(server.gtid_executed, server.runid, strlen(server.runid));
        if (server.current_uuid == NULL) {
            gtidSetAdd(server.gtid_executed, server.runid, strlen(server.runid), 0);
            server.current_uuid = gtidSetFindUuidSet(server.gtid_executed, server.runid, strlen(server.runid));
        }
        return 1;
    } 
    return 0;
}

/**
 * @brief 
 *      ctrip.merge_start {gid [crdt]}
 * @param c
 *  
 */
void ctripMergeStartCommand(client* c) {
    //not support crdt gid
    // server.gtid_in_merge = 1;
    c->gtid_in_merge = 1;
    addReply(c, shared.ok);
    server.dirty++;
}

/**
 * @brief 
 *      ctrip.merge_set gid 1 version 1.0
 * @param c 
 */
void ctripMergeSetCommand(client* c) {
    //will set gid bind to client 
    UNUSED(c);
}

/**
 * @brief 
 *      merge key(string) value(robj) expire(long long) lfu_freq lru_idle
 * @param c 
 */
void ctripMergeCommand(client* c) {
    if(c->gtid_in_merge == 0) {
        addReplyErrorFormat(c, "full sync failed");
        return;
    }
    //not support crdt gid

    robj *key = c->argv[1];
    rio payload;
    robj *val = NULL;
    // check val
    if(verifyDumpPayload(c->argv[2]->ptr, sdslen(c->argv[2]->ptr)) == C_ERR) {
        addReplyErrorFormat(c, "value robj load error: %s", (char*)c->argv[2]->ptr);
        goto error;
    }
    int type = -1;
    long long expiretime = -1, now = mstime();
    //check expiretime
    if(!string2ll((sds)c->argv[3]->ptr, sdslen((sds)c->argv[3]->ptr), &expiretime)) {
        addReplyErrorFormat(c, "expiretime string2ll error: %s", (char*)c->argv[3]->ptr);
        goto error;
    }
    //check lfu_freq lru_idle
    long long lfu_freq = -1, lru_idle = -1;
    if(c->argc == 6) {
        if(!string2ll((sds)c->argv[4]->ptr, sdslen(c->argv[4]->ptr), &lfu_freq)) {
            addReplyErrorFormat(c, "lfu_freq string2ll error: %s", (char*)c->argv[4]->ptr);
            goto error;
        }
        if(!string2ll((sds)c->argv[5]->ptr, sdslen(c->argv[5]->ptr), &lru_idle)) {
            addReplyErrorFormat(c, "lru_idle string2ll error: %s", (char*)c->argv[5]->ptr);
            goto error;
        }
    }
    
    rioInitWithBuffer(&payload, c->argv[2]->ptr);
    int load_error = 0;
    if (((type = rdbLoadObjectType(&payload)) == -1) ||
        ((val = rdbLoadObject(type, &payload, payload.io.buffer.ptr, &load_error)) == NULL))
    {
        addReplyErrorFormat(c, "load robj error: %d, key: %s", load_error, (char*)c->argv[2]->ptr);
        sdsfree(payload.io.buffer.ptr);
        goto error;
    }
    
    /* Check if the key already expired. This function is used when loading
        * an RDB file from disk, either at startup, or when an RDB was
        * received from the master. In the latter case, the master is
        * responsible for key expiry. If we would expire keys here, the
        * snapshot taken by the master may not be reflected on the slave.
        * Similarly if the RDB is the preamble of an AOF file, we want to
        * load all the keys as they are, since the log of operations later
        * assume to work in an exact keyspace state. */
    if (iAmMaster() &&
        expiretime != -1 && expiretime < now)
    {
        decrRefCount(val); 
    } else {
        /* Add the new object in the hash table */
        sds keydup = sdsdup(key->ptr); /* moved to db.dict by dbAddRDBLoad */

        int added = dbAddRDBLoad(c->db,keydup,val);
        if (!added) {
            /**
             * When it's set we allow new keys to replace the current
                    keys with the same name.
             */
            dbSyncDelete(c->db,key);
            dbAddRDBLoad(c->db,keydup,val);
        }

        /* Set the expire time if needed */
        if (expiretime != -1) {
            setExpire(NULL,c->db,key,expiretime);
        }

        /* Set usage information (for eviction). */
        long long lru_clock = LRU_CLOCK();
        if(c->argc== 6) {
            objectSetLRUOrLFU(val,lfu_freq,lru_idle,lru_clock,1000);
        }
        /* call key space notification on key loaded for modules only */
        moduleNotifyKeyspaceEvent(NOTIFY_LOADED, "loaded", key, c->db->id);
    }

    /* Loading the database more slowly is useful in order to test
     * certain edge cases. */
    if (server.key_load_delay) usleep(server.key_load_delay);
    server.dirty++;
    addReply(c, shared.ok);
    return;
error:
    if(val != NULL) {
        decrRefCount(val);
    }
    c->gtid_in_merge = 0;
}

/**
 * @brief 
 *      ctrip.merge_end {gtid_set} {gid}
 * @param c 
 */
void ctripMergeEndCommand(client* c) {
    if(c->gtid_in_merge == 0) {
        addReplyErrorFormat(c, "full sync failed");
        return;
    }
    c->gtid_in_merge = 0;
    gtidSet* gtid_set = gtidSetDecode(c->argv[1]->ptr, sdslen(c->argv[1]->ptr));
    gtidSetAppendGtidSet(server.gtid_executed, gtid_set);
    gtidSetFree(gtid_set);
    server.dirty++;
    addReply(c, shared.ok);
}

/**
 * @brief 
 *      robj encode (use test ctrip.merge command)
 * @param c 
 */
void gtidGetRobjCommand(client* c) {
    robj* key = c->argv[1];
    rio payload;
    robj* val;
    if ((val = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp])) == NULL)
        return;

    if (val) {
        createDumpPayload(&payload, val, key);
        addReplyBulkCBuffer(c, payload.io.buffer.ptr, sdslen(payload.io.buffer.ptr));
        sdsfree(payload.io.buffer.ptr);
    } else {
        addReplyNull(c);
    }

}

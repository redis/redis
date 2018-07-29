#include "server.h"

// ---------------- MIGRATE CACHED SOCKET ----------------------------------- //

typedef struct {
    int fd;
    int last_dbid;
    time_t last_use_time;
    const char *name;
    sds auth;
    int busy;
    int error;
    int authenticated;
} migrateCachedSocket;

static sds migrateSocketName(robj *host, robj *port, robj *auth) {
    sds name = sdscatfmt(sdsempty(), "%S:%S", host->ptr, port->ptr);
    if (auth == NULL) {
        return name;
    }
    return sdscatlen(name, "#", 1);
}

static void migrateCloseSocket(migrateCachedSocket *cs) {
    serverAssert(dictDelete(server.migrate_cached_sockets, cs->name) == DICT_OK);
    if (cs->auth != NULL) {
        sdsfree(cs->auth);
    }
    close(cs->fd);
    zfree(cs);
}

void migrateCloseTimedoutSockets(void) {
    dictIterator *di = dictGetSafeIterator(server.migrate_cached_sockets);
    dictEntry *entry;
    while ((entry = dictNext(di)) != NULL) {
        migrateCachedSocket *cs = dictGetVal(entry);
        if (cs->busy || server.unixtime - cs->last_use_time <= server.migrate_socket_cache_timeout) {
            continue;
        }
        migrateCloseSocket(cs);
    }
    dictReleaseIterator(di);
}

static migrateCachedSocket *migrateGetSocketOrReply(client *c, robj *host, robj *port, robj *auth, mstime_t timeout) {
    sds name = migrateSocketName(host, port, auth);
    migrateCachedSocket *cs = dictFetchValue(server.migrate_cached_sockets, name);
    if (cs != NULL) {
        sdsfree(name);
        if (cs->busy) {
            addReplySds(c, sdscatfmt(sdsempty(), "-RETRYLATER target %S:%S is busy.\r\n", host->ptr, port->ptr));
            return NULL;
        }
        serverAssert((cs->auth != NULL && auth != NULL) || (cs->auth == NULL && auth == NULL));

        if (cs->auth != NULL && sdscmp(cs->auth, auth->ptr) != 0) {
            sdsfree(cs->auth);
            cs->auth = sdsdup(auth->ptr);
            cs->authenticated = 0;
        }
        return cs;
    }

    if (server.migrate_socket_cache_items != 0) {
        while (dictSize(server.migrate_cached_sockets) == (unsigned int)server.migrate_socket_cache_items) {
            dictEntry *entry = dictGetRandomKey(server.migrate_cached_sockets);
            migrateCloseSocket(dictGetVal(entry));
        }
    }

    int fd = anetTcpNonBlockConnect(server.neterr, host->ptr, atoi(port->ptr));
    if (fd == -1) {
        sdsfree(name);
        addReplySds(c, sdscatfmt(sdsempty(), "-IOERR Can't connect to target %S:%S: '%s'.\r\n", host->ptr, port->ptr,
                                 server.neterr));
        return NULL;
    }
    anetEnableTcpNoDelay(server.neterr, fd);

    if ((aeWait(fd, AE_WRITABLE, timeout) & AE_WRITABLE) == 0) {
        sdsfree(name);
        close(fd);
        addReplySds(c, sdsnew("-IOERR error or timeout connecting to the client.\r\n"));
        return NULL;
    }

    cs = zmalloc(sizeof(*cs));
    cs->fd = fd;
    cs->last_dbid = -1;
    cs->last_use_time = server.unixtime;
    cs->name = name;
    cs->auth = (auth != NULL ? sdsdup(auth->ptr) : NULL);
    cs->busy = 0;
    cs->error = 0;
    cs->authenticated = 0;
    dictAdd(server.migrate_cached_sockets, name, cs);
    return cs;
}

// ---------------- SYNC COMMANDS ------------------------------------------- //

#define SYNC_WRITE_IOBUF_LEN (64 * 1024)

static int syncWriteBuffer(int fd, sds buffer, mstime_t timeout) {
    ssize_t pos = 0;
    ssize_t len = sdslen(buffer);
    while (pos != len) {
        ssize_t towrite = len - pos;
        if (towrite > SYNC_WRITE_IOBUF_LEN) {
            towrite = SYNC_WRITE_IOBUF_LEN;
        }
        ssize_t written = syncWrite(fd, buffer + pos, towrite, timeout);
        if (written != towrite) {
            return C_ERR;
        }
        pos += written;
    }
    return C_OK;
}

static int syncPingCommand(int fd, mstime_t timeout, sds *reterr) {
    rio cmd;
    rioInitWithBuffer(&cmd, sdsempty());

    const char *cmd_name = "PING";
    serverAssert(rioWriteBulkCount(&cmd, '*', 1));
    serverAssert(rioWriteBulkString(&cmd, cmd_name, strlen(cmd_name)));

    serverAssert(reterr != NULL && *reterr == NULL);

    if (syncWriteBuffer(fd, cmd.io.buffer.ptr, timeout) != C_OK) {
        sdsfree(cmd.io.buffer.ptr);
        *reterr = sdscatfmt(sdsempty(), "-IOERR Command %s failed, sending error '%s'.\r\n", cmd_name, strerror(errno));
        return C_ERR;
    }
    sdsfree(cmd.io.buffer.ptr);

    char buf[4096];
    if (syncReadLine(fd, buf, sizeof(buf), timeout) <= 0) {
        *reterr = sdscatfmt(sdsempty(), "-IOERR Command %s failed, reading error '%s'.\r\n", cmd_name, strerror(errno));
        return C_ERR;
    }
    if (buf[0] != '+') {
        *reterr = sdscatfmt(sdsempty(), "-ERR Command %s failed, target replied: %s.\r\n", cmd_name, buf);
        return C_ERR;
    }
    return C_OK;
}

static int syncAuthCommand(int fd, mstime_t timeout, sds password, sds *reterr) {
    rio cmd;
    rioInitWithBuffer(&cmd, sdsempty());

    const char *cmd_name = "AUTH";
    serverAssert(rioWriteBulkCount(&cmd, '*', 2));
    serverAssert(rioWriteBulkString(&cmd, cmd_name, strlen(cmd_name)));
    serverAssert(rioWriteBulkString(&cmd, password, sdslen(password)));

    serverAssert(reterr != NULL && *reterr == NULL);

    if (syncWriteBuffer(fd, cmd.io.buffer.ptr, timeout) != C_OK) {
        sdsfree(cmd.io.buffer.ptr);
        *reterr = sdscatfmt(sdsempty(), "-IOERR Command %s failed, sending error '%s'.\r\n", cmd_name, strerror(errno));
        return C_ERR;
    }
    sdsfree(cmd.io.buffer.ptr);

    char buf[4096];
    if (syncReadLine(fd, buf, sizeof(buf), timeout) <= 0) {
        *reterr = sdscatfmt(sdsempty(), "-IOERR Command %s failed, reading error '%s'.\r\n", cmd_name, strerror(errno));
        return C_ERR;
    }
    if (buf[0] != '+') {
        *reterr = sdscatfmt(sdsempty(), "-ERR Command %s failed, target replied: %s.\r\n", cmd_name, buf);
        return C_ERR;
    }
    return C_OK;
}

static int syncSelectCommand(int fd, mstime_t timeout, int dbid, sds *reterr) {
    rio cmd;
    rioInitWithBuffer(&cmd, sdsempty());

    const char *cmd_name = "SELECT";
    serverAssert(rioWriteBulkCount(&cmd, '*', 2));
    serverAssert(rioWriteBulkString(&cmd, cmd_name, strlen(cmd_name)));
    serverAssert(rioWriteBulkLongLong(&cmd, dbid));

    serverAssert(reterr != NULL && *reterr == NULL);

    if (syncWriteBuffer(fd, cmd.io.buffer.ptr, timeout) != C_OK) {
        sdsfree(cmd.io.buffer.ptr);
        *reterr = sdscatfmt(sdsempty(), "-IOERR Command %s failed, sending error '%s'.\r\n", cmd_name, strerror(errno));
        return C_ERR;
    }
    sdsfree(cmd.io.buffer.ptr);

    char buf[4096];
    if (syncReadLine(fd, buf, sizeof(buf), timeout) <= 0) {
        *reterr = sdscatfmt(sdsempty(), "-IOERR Command %s failed, reading error '%s'.\r\n", cmd_name, strerror(errno));
        return C_ERR;
    }
    if (buf[0] != '+') {
        *reterr = sdscatfmt(sdsempty(), "-ERR Command %s failed, target replied: %s.\r\n", cmd_name, buf);
        return C_ERR;
    }
    return C_OK;
}

// ---------------- MIGRATE RIO COMMAND ------------------------------------- //

typedef struct {
    rio rio;
    sds payload;
    int seq_num;
    mstime_t timeout;
    int replace;
    int non_blocking;
    struct {
        int fd;
        sds sendbuf;
    } io;
    struct {
        robj *key;
        mstime_t ttl;
    } privdata;
} rioMigrateCommand;

#define RIO_GOTO_IF_ERROR(e)         \
    do {                             \
        if (!(e)) {                  \
            goto rio_failed_cleanup; \
        }                            \
    } while (0)

#define RIO_IOBUF_MAX_LEN (64LL * 1024 * 1024)
#define RIO_IOBUF_AUTO_FLUSH_THRESHOLD (RIO_IOBUF_MAX_LEN - 1024)

static int rioMigrateCommandFlushIOBuffer(rioMigrateCommand *cmd) {
    if (sdslen(cmd->io.sendbuf) != 0) {
        if (syncWriteBuffer(cmd->io.fd, cmd->io.sendbuf, cmd->timeout) != C_OK) {
            return 0;
        }
        sdsclear(cmd->io.sendbuf);
    }
    return 1;
}

static size_t _rioMigrateObjectRead(rio *r, void *buf, size_t len) {
    UNUSED(r);
    UNUSED(buf);
    UNUSED(len);
    serverPanic("Unsupported operation.");
}

static off_t _rioMigrateObjectTell(rio *r) {
    UNUSED(r);
    serverPanic("Unsupported operation.");
}

static int _rioMigrateObjectFlushNonBlockingFragment(rioMigrateCommand *cmd) {
    if (sdslen(cmd->payload) == 0) {
        return 1;
    }
    serverAssert(cmd->non_blocking);

    rio _rio;
    rio *rio = &_rio;
    rioInitWithBuffer(rio, cmd->io.sendbuf);

    robj *key = cmd->privdata.key;
    const char *cmd_name = server.cluster_enabled ? "RESTORE-ASYNC-ASKING" : "RESTORE-ASYNC";

    if (cmd->seq_num != 0) {
        goto rio_fragment_payload;
    }
    RIO_GOTO_IF_ERROR(rioWriteBulkCount(rio, '*', 2));
    RIO_GOTO_IF_ERROR(rioWriteBulkString(rio, cmd_name, strlen(cmd_name)));
    RIO_GOTO_IF_ERROR(rioWriteBulkString(rio, "RESET", 5));

    cmd->seq_num++;

rio_fragment_payload:
    RIO_GOTO_IF_ERROR(rioWriteBulkCount(rio, '*', 4));
    RIO_GOTO_IF_ERROR(rioWriteBulkString(rio, cmd_name, strlen(cmd_name)));
    RIO_GOTO_IF_ERROR(rioWriteBulkString(rio, "PAYLOAD", 7));
    RIO_GOTO_IF_ERROR(rioWriteBulkString(rio, key->ptr, sdslen(key->ptr)));
    RIO_GOTO_IF_ERROR(rioWriteBulkString(rio, cmd->payload, sdslen(cmd->payload)));

    cmd->seq_num++;
    cmd->io.sendbuf = rio->io.buffer.ptr;
    sdsclear(cmd->payload);

    if (sdslen(cmd->io.sendbuf) < RIO_IOBUF_AUTO_FLUSH_THRESHOLD) {
        return 1;
    }
    return rioMigrateCommandFlushIOBuffer(cmd);

rio_failed_cleanup:
    cmd->io.sendbuf = rio->io.buffer.ptr;
    return 0;
}

#define RIO_MIGRATE_COMMAND(r) ((rioMigrateCommand *)((char *)(r)-offsetof(rioMigrateCommand, rio)))

static size_t _rioMigrateObjectWrite(rio *r, const void *buf, size_t len) {
    rioMigrateCommand *cmd = RIO_MIGRATE_COMMAND(r);
    cmd->payload = sdscatlen(cmd->payload, buf, len);
    if (!cmd->non_blocking) {
        return 1;
    }
    if (sdslen(cmd->io.sendbuf) + sdslen(cmd->payload) < RIO_IOBUF_AUTO_FLUSH_THRESHOLD) {
        return 1;
    }
    return _rioMigrateObjectFlushNonBlockingFragment(cmd);
}

static int _rioMigrateObjectFlush(rio *r) {
    rioMigrateCommand *cmd = RIO_MIGRATE_COMMAND(r);
    if (!cmd->non_blocking) {
        serverAssert(cmd->seq_num == 0 && sdslen(cmd->payload) != 0);
    } else {
        if (!_rioMigrateObjectFlushNonBlockingFragment(cmd)) {
            return 0;
        }
        serverAssert(cmd->seq_num >= 2 && sdslen(cmd->payload) == 0);
    }

    rio _rio;
    rio *rio = &_rio;
    rioInitWithBuffer(rio, cmd->io.sendbuf);

    robj *key = cmd->privdata.key;
    mstime_t ttl = cmd->privdata.ttl;

    if (!cmd->non_blocking) {
        const char *cmd_name = server.cluster_enabled ? "RESTORE-ASKING" : "RESTORE";
        RIO_GOTO_IF_ERROR(rioWriteBulkCount(rio, '*', cmd->replace ? 5 : 4));
        RIO_GOTO_IF_ERROR(rioWriteBulkString(rio, cmd_name, strlen(cmd_name)));
        RIO_GOTO_IF_ERROR(rioWriteBulkString(rio, key->ptr, sdslen(key->ptr)));
        RIO_GOTO_IF_ERROR(rioWriteBulkLongLong(rio, ttl));
        RIO_GOTO_IF_ERROR(rioWriteBulkString(rio, cmd->payload, sdslen(cmd->payload)));
        if (cmd->replace) {
            RIO_GOTO_IF_ERROR(rioWriteBulkString(rio, "REPLACE", 7));
        }
        sdsclear(cmd->payload);
    } else {
        const char *cmd_name = server.cluster_enabled ? "RESTORE-ASYNC-ASKING" : "RESTORE-ASYNC";
        RIO_GOTO_IF_ERROR(rioWriteBulkCount(rio, '*', cmd->replace ? 5 : 4));
        RIO_GOTO_IF_ERROR(rioWriteBulkString(rio, cmd_name, strlen(cmd_name)));
        RIO_GOTO_IF_ERROR(rioWriteBulkString(rio, "RESTORE", 7));
        RIO_GOTO_IF_ERROR(rioWriteBulkString(rio, key->ptr, sdslen(key->ptr)));
        RIO_GOTO_IF_ERROR(rioWriteBulkLongLong(rio, ttl));
        if (cmd->replace) {
            RIO_GOTO_IF_ERROR(rioWriteBulkString(rio, "REPLACE", 7));
        }
    }
    serverAssert(sdslen(cmd->payload) == 0);

    cmd->seq_num++;
    cmd->io.sendbuf = rio->io.buffer.ptr;

    if (sdslen(cmd->io.sendbuf) < RIO_IOBUF_AUTO_FLUSH_THRESHOLD) {
        return 1;
    }
    return rioMigrateCommandFlushIOBuffer(cmd);

rio_failed_cleanup:
    cmd->io.sendbuf = rio->io.buffer.ptr;
    return 0;
}

static const rio _rioMigrateObjectIO = {
    .read = _rioMigrateObjectRead,
    .tell = _rioMigrateObjectTell,
    .write = _rioMigrateObjectWrite,
    .flush = _rioMigrateObjectFlush,
    .update_cksum = rioGenericUpdateChecksum,
};

static int rioMigrateCommandObject(rioMigrateCommand *cmd, robj *key, robj *obj, mstime_t ttl) {
    rio *rio = &(cmd->rio);
    rio->cksum = 0;

    cmd->seq_num = 0;
    cmd->privdata.key = key;
    cmd->privdata.ttl = ttl;
    serverAssert(sdslen(cmd->payload) == 0);

    RIO_GOTO_IF_ERROR(rdbSaveObjectType(rio, obj));
    RIO_GOTO_IF_ERROR(rdbSaveObject(rio, obj));

    uint16_t ver = RDB_VERSION;
    memrev16ifbe(&ver);
    RIO_GOTO_IF_ERROR(rioWrite(rio, &ver, sizeof(ver)));

    uint64_t crc = rio->cksum;
    memrev64ifbe(&crc);
    RIO_GOTO_IF_ERROR(rioWrite(rio, &crc, sizeof(crc)));

    RIO_GOTO_IF_ERROR(rioFlush(rio));

    serverAssert(sdslen(cmd->payload) == 0);
    return 1;

rio_failed_cleanup:
    return 0;
}

// ---------------- MIGRATE COMMAND ----------------------------------------- //

#define PROCESS_STATE_NONE 0
#define PROCESS_STATE_DONE 1
#define PROCESS_STATE_QUEUED 2

struct _migrateCommandArgs {
    redisDb *db;
    robj *host;
    robj *port;
    robj *auth;
    int dbid;
    mstime_t timeout;

    int copy;
    int replace;
    int non_blocking;
    int num_keys;

    struct {
        robj *key;
        robj *val;
        mstime_t expireat;
        int num_fragments;
        int success;
    } * kvpairs;

    migrateCachedSocket *socket;
    sds errmsg;

    const char *cmd_name;

    client *client;
    int process_state;
};

static void freeMigrateCommandArgs(migrateCommandArgs *args) {
    if (args->host != NULL) {
        decrRefCount(args->host);
    }
    if (args->port != NULL) {
        decrRefCount(args->port);
    }
    if (args->auth != NULL) {
        decrRefCount(args->auth);
    }
    if (args->kvpairs != NULL) {
        for (int j = 0; j < args->num_keys; j++) {
            robj *key = args->kvpairs[j].key;
            robj *val = args->kvpairs[j].val;
            decrRefCount(key);
            decrRefCountLazyfree(val);
        }
        zfree(args->kvpairs);
    }
    if (args->socket != NULL) {
        if (args->socket->error) {
            migrateCloseSocket(args->socket);
        } else {
            args->socket->busy = 0;
        }
    }
    if (args->errmsg != NULL) {
        sdsfree(args->errmsg);
    }
    zfree(args);
}

// MIGRATE host port key dbid timeout [COPY] [REPLACE] [ASYNC] [AUTH password]
// MIGRATE host port ""  dbid timeout [COPY] [REPLACE] [ASYNC] [AUTH password]
//         KEYS key1 key2 ... keyN
static migrateCommandArgs *initMigrateCommandArgsOrReply(client *c) {
    migrateCommandArgs *args = zcalloc(sizeof(*args));
    int first_key = 3;
    int num_keys = 1;
    for (int j = 6; j < c->argc; j++) {
        if (strcasecmp(c->argv[j]->ptr, "copy") == 0) {
            args->copy = 1;
        } else if (strcasecmp(c->argv[j]->ptr, "replace") == 0) {
            args->replace = 1;
        } else if (strcasecmp(c->argv[j]->ptr, "async") == 0) {
            args->non_blocking = 1;
        } else if (strcasecmp(c->argv[j]->ptr, "auth") == 0) {
            if (j == c->argc - 1) {
                addReply(c, shared.syntaxerr);
                goto failed_cleanup;
            }
            j++;
            args->auth = c->argv[j];
            incrRefCount(args->auth);
        } else if (strcasecmp(c->argv[j]->ptr, "keys") == 0) {
            if (sdslen(c->argv[3]->ptr) != 0) {
                addReplyError(c, "When using MIGRATE KEYS option, the key argument must be set to the empty string");
                goto failed_cleanup;
            }
            first_key = j + 1;
            num_keys = c->argc - first_key;
            goto parsed_options;
        } else {
            addReply(c, shared.syntaxerr);
            goto failed_cleanup;
        }
    }

parsed_options:
    args->host = c->argv[1];
    incrRefCount(args->host);

    args->port = c->argv[2];
    incrRefCount(args->port);

    long dbid;
    long timeout;
    if (getLongFromObjectOrReply(c, c->argv[4], &dbid, NULL) != C_OK ||
        getLongFromObjectOrReply(c, c->argv[5], &timeout, NULL) != C_OK) {
        goto failed_cleanup;
    }

    args->dbid = (int)dbid;
    args->timeout = (timeout <= 0 ? 1000 : timeout);

    args->kvpairs = zcalloc(sizeof(args->kvpairs[0]) * num_keys);

    for (int i = 0; i < num_keys; i++) {
        robj *key = c->argv[first_key + i];
        robj *val = lookupKeyRead(c->db, key);
        if (val == NULL) {
            continue;
        }
        int j = args->num_keys++;
        args->kvpairs[j].key = key;
        args->kvpairs[j].val = val;
        args->kvpairs[j].expireat = getExpire(c->db, key);
        incrRefCount(key);
        incrRefCount(val);
    }

    if (args->num_keys == 0) {
        addReplySds(c, sdsnew("+NOKEY\r\n"));
        goto failed_cleanup;
    }

    migrateCachedSocket *cs = migrateGetSocketOrReply(c, args->host, args->port, args->auth, args->timeout);
    if (cs == NULL) {
        goto failed_cleanup;
    }
    serverAssert(!cs->busy && !cs->error);

    args->socket = cs;
    args->socket->busy = 1;
    args->socket->last_use_time = server.unixtime;

    args->db = c->db;
    args->client = c;
    args->errmsg = NULL;
    args->cmd_name = "MIGRATE";
    args->process_state = PROCESS_STATE_NONE;
    return args;

failed_cleanup:
    freeMigrateCommandArgs(args);
    return NULL;
}

static int migrateGenericCommandSendRequests(migrateCommandArgs *args) {
    serverAssert(args->errmsg == NULL);

    migrateCachedSocket *cs = args->socket;
    if (!cs->authenticated) {
        if (cs->auth != NULL) {
            if (syncAuthCommand(cs->fd, args->timeout, cs->auth, &args->errmsg) != C_OK) {
                goto failed_socket_error;
            }
        } else {
            if (syncPingCommand(cs->fd, args->timeout, &args->errmsg) != C_OK) {
                goto failed_socket_error;
            }
        }
        cs->authenticated = 1;
    }

    if (cs->last_dbid != args->dbid) {
        if (syncSelectCommand(cs->fd, args->timeout, args->dbid, &args->errmsg) != C_OK) {
            goto failed_socket_error;
        }
        cs->last_dbid = args->dbid;
    }

    rioMigrateCommand _cmd = {
        .rio = _rioMigrateObjectIO,
        .payload = sdsMakeRoomFor(sdsempty(), RIO_IOBUF_MAX_LEN),
        .seq_num = 0,
        .timeout = args->timeout,
        .replace = args->replace,
        .non_blocking = args->non_blocking,
        .io =
            {
                .fd = cs->fd,
                .sendbuf = sdsMakeRoomFor(sdsempty(), RIO_IOBUF_MAX_LEN),
            },
    };
    rioMigrateCommand *cmd = &_cmd;

    for (int j = 0; j < args->num_keys; j++) {
        robj *key = args->kvpairs[j].key;
        robj *val = args->kvpairs[j].val;
        mstime_t ttl = 0;
        mstime_t expireat = args->kvpairs[j].expireat;
        if (expireat != -1) {
            ttl = expireat - mstime();
            ttl = (ttl < 1 ? 1 : ttl);
        }
        RIO_GOTO_IF_ERROR(rioMigrateCommandObject(cmd, key, val, ttl));

        args->kvpairs[j].num_fragments = cmd->seq_num;
    }
    RIO_GOTO_IF_ERROR(rioMigrateCommandFlushIOBuffer(cmd));

    sdsfree(cmd->payload);
    sdsfree(cmd->io.sendbuf);

    args->socket->last_use_time = server.unixtime;
    return 1;

rio_failed_cleanup:
    sdsfree(cmd->payload);
    sdsfree(cmd->io.sendbuf);

    serverAssert(args->errmsg == NULL);

    args->errmsg =
        sdscatfmt(sdsempty(), "-ERR Command %s failed, sending error '%s'.\r\n", args->cmd_name, strerror(errno));

failed_socket_error:
    serverAssert(args->errmsg != NULL);

    args->socket->error = 1;
    return 0;
}

static int migrateGenericCommandFetchReplies(migrateCommandArgs *args) {
    serverAssert(args->errmsg == NULL);

    migrateCachedSocket *cs = args->socket;
    for (int j = 0; j < args->num_keys; j++) {
        int errors = 0;
        for (int i = 0; i < args->kvpairs[j].num_fragments; i++) {
            char buf[4096];
            if (syncReadLine(cs->fd, buf, sizeof(buf), args->timeout) <= 0) {
                goto failed_socket_error;
            }
            if (buf[0] != '+') {
                if (args->errmsg == NULL) {
                    args->errmsg =
                        sdscatfmt(sdsempty(), "-ERR Command %s failed, target replied: %s.\r\n", args->cmd_name, buf);
                }
                errors++;
            }
        }
        if (errors == 0) {
            args->kvpairs[j].success = 1;
        }
    }

    args->socket->last_use_time = server.unixtime;
    return 1;

failed_socket_error:
    if (args->errmsg != NULL) {
        sdsfree(args->errmsg);
    }
    args->errmsg =
        sdscatfmt(sdsempty(), "-IOERR Command %s failed, reading error '%s'.\r\n", args->cmd_name, strerror(errno));

    args->socket->error = 1;
    return 0;
}

static void migrateGenericCommandReplyAndPropagate(migrateCommandArgs *args) {
    if (args->client != NULL) {
        client *c = args->client;
        if (args->errmsg != NULL) {
            addReplySds(c, sdsdup(args->errmsg));
        } else {
            addReply(c, shared.ok);
        }
    }

    if (!args->copy) {
        int migrated_keys = 0;
        robj **propargv = zmalloc(sizeof(propargv[0]) * (1 + args->num_keys));
        for (int j = 0; j < args->num_keys; j++) {
            if (!args->kvpairs[j].success) {
                continue;
            }
            migrated_keys++;

            robj *key = args->kvpairs[j].key;
            propargv[migrated_keys] = key;

            dbDelete(args->db, key);
            signalModifiedKey(args->db, key);
            server.dirty++;
        }

        if (migrated_keys == 0) {
            zfree(propargv);
            return;
        }

        if (!args->non_blocking && args->client != NULL) {
            preventCommandPropagation(args->client);
        }

        propargv[0] = createStringObject("DEL", 3);

        propagate(server.delCommand, args->db->id, propargv, 1 + migrated_keys, PROPAGATE_AOF | PROPAGATE_REPL);

        decrRefCount(propargv[0]);
        zfree(propargv);
    }
}

static void migrateCommandThreadAddMigrateJobTail(migrateCommandArgs *args);
static void migrateCommandThreadAddRestoreJobTail(restoreCommandArgs *args);

void migrateCommand(client *c) {
    migrateCommandArgs *args = initMigrateCommandArgsOrReply(c);
    if (args == NULL) {
        return;
    }
    serverAssert(c->migrate_command_args == NULL);

    if (!args->non_blocking) {
        if (migrateGenericCommandSendRequests(args)) {
            migrateGenericCommandFetchReplies(args);
        }
        migrateGenericCommandReplyAndPropagate(args);

        freeMigrateCommandArgs(args);
        return;
    }

    for (int j = 0; j < args->num_keys; j++) {
        robj *key = args->kvpairs[j].key;
        incrRefCount(key);
        serverAssert(dictAdd(args->db->migrating_keys, key, NULL) == DICT_OK);
    }
    c->migrate_command_args = args;

    migrateCommandThreadAddMigrateJobTail(args);

    blockClient(c, BLOCKED_MIGRATE);
}

static void migrateCommandNonBlockingCallback(migrateCommandArgs *args) {
    serverAssert(args->non_blocking && args->process_state == PROCESS_STATE_DONE);

    for (int j = 0; j < args->num_keys; j++) {
        robj *key = args->kvpairs[j].key;
        serverAssert(dictDelete(args->db->migrating_keys, key) == DICT_OK);
    }
    migrateGenericCommandReplyAndPropagate(args);

    if (args->client != NULL) {
        client *c = args->client;
        serverAssert(c->migrate_command_args == args);
        unblockClient(c);
        serverAssert(c->migrate_command_args == NULL && args->client == NULL);
    }

    freeMigrateCommandArgs(args);
}

void unblockClientFromMigrate(client *c) {
    serverAssert(c->migrate_command_args != NULL && c->migrate_command_args->client == c);
    serverAssert(c->migrate_command_args->non_blocking && c->migrate_command_args->process_state != PROCESS_STATE_NONE);
    c->migrate_command_args->client = NULL;
    c->migrate_command_args = NULL;
}

void freeMigrateCommandArgsFromFreeClient(client *c) {
    UNUSED(c);
    serverPanic("Should not arrive here.");
}

int migrateNeedsRedirectClient(client *c) {
    if (dictSize(c->db->migrating_keys) == 0) {
        return 0;
    }

    multiState _ms, *ms = &_ms;
    multiCmd mc;
    if (c->cmd->proc != execCommand) {
        mc.cmd = c->cmd;
        mc.argv = c->argv;
        mc.argc = c->argc;
        ms->commands = &mc;
        ms->count = 1;
    } else if (c->flags & CLIENT_MULTI) {
        ms = &c->mstate;
    } else {
        return 0;
    }

    for (int i = 0; i < ms->count; i++) {
        struct redisCommand *mcmd = ms->commands[i].cmd;
        if (mcmd->flags & CMD_READONLY) {
            continue;
        }
        robj **margv = ms->commands[i].argv;
        int margc = ms->commands[i].argc;

        int numkeys = 0;
        int *keyindex = getKeysFromCommand(mcmd, margv, margc, &numkeys);
        int migrating = 0;
        for (int j = 0; j < numkeys && !migrating; j++) {
            robj *thiskey = margv[keyindex[j]];
            if (dictFind(c->db->migrating_keys, thiskey) != NULL) {
                migrating = 1;
            }
        }
        getKeysFreeResult(keyindex);

        if (migrating) {
            return 1;
        }
    }
    return 0;
}

// ---------------- RESTORE COMMAND ----------------------------------------- //

struct _restoreCommandArgs {
    redisDb *db;
    robj *key;
    robj *obj;
    mstime_t ttl;

    int replace;
    int absttl;
    int non_blocking;

    long long lfu_freq;
    long long lru_idle;
    long long lru_clock;

    list *fragments;
    size_t total_bytes;

    robj *payload;

    time_t last_use_time;
    sds errmsg;

    listNode *link_node;

    const char *cmd_name;

    client *client;
    int process_state;
};

static list *restore_command_args_list = NULL;

static void freeRestoreCommandArgs(restoreCommandArgs *args) {
    if (args->key != NULL) {
        decrRefCount(args->key);
    }
    if (args->obj != NULL) {
        decrRefCountLazyfree(args->obj);
    }

    while (listLength(args->fragments) != 0) {
        listNode *head = listFirst(args->fragments);
        decrRefCount(listNodeValue(head));
        listDelNode(args->fragments, head);
    }
    listRelease(args->fragments);

    if (args->payload != NULL) {
        decrRefCount(args->payload);
    }
    if (args->errmsg != NULL) {
        sdsfree(args->errmsg);
    }
    if (args->link_node != NULL) {
        listDelNode(restore_command_args_list, args->link_node);
    }
    zfree(args);
}

static restoreCommandArgs *initRestoreCommandArgs(client *c, robj *key, int non_blocking) {
    restoreCommandArgs *args = zcalloc(sizeof(*args));
    args->db = c->db;
    args->key = key;
    args->non_blocking = non_blocking;
    args->lfu_freq = -1;
    args->lru_idle = -1;
    args->lru_clock = -1;
    args->fragments = listCreate();
    args->last_use_time = server.unixtime;
    if (server.cluster_enabled) {
        args->cmd_name = non_blocking ? "RESTORE-ASYNC-ASKING" : "RESTORE-ASKING";
    } else {
        args->cmd_name = non_blocking ? "RESTORE-ASYNC" : "RESTORE";
    }
    args->client = c;
    args->process_state = PROCESS_STATE_NONE;

    incrRefCount(key);

    if (restore_command_args_list == NULL) {
        restore_command_args_list = listCreate();
    }
    listAddNodeTail(restore_command_args_list, args);
    args->link_node = listLast(restore_command_args_list);
    return args;
}

static void restoreGenericCommandAddFragment(restoreCommandArgs *args, robj *frag) {
    serverAssert(sdsEncodedObject(frag));
    incrRefCount(frag);
    listAddNodeTail(args->fragments, frag);
    args->total_bytes += sdslen(frag->ptr);
    args->last_use_time = server.unixtime;
}

extern int verifyDumpPayload(unsigned char *p, size_t len);

static int restoreGenericCommandExtractPayload(restoreCommandArgs *args) {
    serverAssert(args->payload == NULL && args->errmsg == NULL);
    if (listLength(args->fragments) == 1) {
        listNode *head = listFirst(args->fragments);
        args->payload = listNodeValue(head);
        listDelNode(args->fragments, head);
    } else {
        sds rawbytes = sdsMakeRoomFor(sdsempty(), args->total_bytes);
        while (listLength(args->fragments) != 0) {
            listNode *head = listFirst(args->fragments);
            rawbytes = sdscatsds(rawbytes, ((robj *)listNodeValue(head))->ptr);
            decrRefCount(listNodeValue(head));
            listDelNode(args->fragments, head);
        }
        args->payload = createObject(OBJ_STRING, rawbytes);
    }

    void *ptr = args->payload->ptr;
    if (verifyDumpPayload(ptr, sdslen(ptr)) != C_OK) {
        args->errmsg = sdscatfmt(sdsempty(), "-ERR DUMP payload version or checksum are wrong.\r\n");
        return 0;
    }
    serverAssert(args->obj == NULL);

    rio payload;
    rioInitWithBuffer(&payload, ptr);

    int type = rdbLoadObjectType(&payload);
    if (type == -1) {
        args->errmsg = sdscatfmt(sdsempty(), "-ERR Bad data format, invalid object type.\r\n");
        return 0;
    }

    args->obj = rdbLoadObject(type, &payload);
    if (args->obj == NULL) {
        args->errmsg = sdscatfmt(sdsempty(), "-ERR Bad data format, invalid object data.\r\n");
        return 0;
    }
    return 1;
}

static void restoreGenericCommandReplyAndPropagate(restoreCommandArgs *args) {
    client *c = args->client;
    if (args->errmsg != NULL) {
        if (c != NULL) {
            addReplySds(c, sdsdup(args->errmsg));
        }
        return;
    }

    if (dictFind(args->db->migrating_keys, args->key) != NULL) {
        if (c != NULL) {
            addReplySds(c, sdscatfmt(sdsempty(), "-RETRYLATER %S is busy.\r\n", args->key->ptr));
        }
        return;
    }

    if (lookupKeyWrite(args->db, args->key) != NULL) {
        if (!args->replace) {
            if (c != NULL) {
                addReply(c, shared.busykeyerr);
            }
            return;
        }
        dbDelete(args->db, args->key);
    }
    incrRefCount(args->obj);
    dbAdd(args->db, args->key, args->obj);

    if (args->ttl != 0) {
        setExpire(c, args->db, args->key, args->absttl ? args->ttl : args->ttl + mstime());
    }
    objectSetLRUOrLFU(args->obj, args->lfu_freq, args->lru_idle, args->lru_clock);

    signalModifiedKey(args->db, args->key);
    server.dirty++;

    if (c != NULL) {
        addReply(c, shared.ok);
    }
    if (c != NULL && !args->non_blocking) {
        preventCommandPropagation(c);
    }

    // RESTORE key ttl serialized-value REPLACE [ASYNC]
    robj *propargv[6];

    propargv[0] = createStringObject("RESTORE", 7);
    propargv[1] = args->key;
    incrRefCount(propargv[1]);
    propargv[2] = createStringObjectFromLongLong(args->ttl);
    propargv[3] = args->payload;
    incrRefCount(propargv[3]);
    propargv[4] = createStringObject("REPLACE", 7);

    int propargc = sizeof(propargv) / sizeof(propargv[0]) - 1;
    if (args->non_blocking) {
        propargv[5] = createStringObject("ASYNC", 5);
        propargc++;
    }

    propagate(server.restoreCommand, args->db->id, propargv, propargc, PROPAGATE_AOF | PROPAGATE_REPL);

    for (int i = 0; i < propargc; i++) {
        decrRefCount(propargv[i]);
    }
}

static void restoreGenericCommandResetIfNeeded(client *c) {
    if (c->restore_command_args == NULL) {
        return;
    }
    serverAssert(c->restore_command_args->client == c);

    restoreCommandArgs *args = c->restore_command_args;
    serverAssert(args->non_blocking && args->process_state == PROCESS_STATE_NONE);

    c->restore_command_args = NULL;

    freeRestoreCommandArgs(args);
}

static void restoreAsyncCommandByCommandArgs(restoreCommandArgs *args) {
    client *c = args->client;
    serverAssert(c != NULL && c->restore_command_args == args);

    migrateCommandThreadAddRestoreJobTail(args);

    blockClient(c, BLOCKED_RESTORE);
}

// RESTORE-ASYNC RESET
// RESTORE-ASYNC PAYLOAD key serialized-fragment
// RESTORE-ASYNC RESTORE key ttl [REPLACE]
void restoreAsyncCommand(client *c) {
    // RESTORE-ASYNC RESET
    if (strcasecmp(c->argv[1]->ptr, "reset") == 0) {
        if (c->argc != 2) {
            goto failed_syntax_error;
        }
        restoreGenericCommandResetIfNeeded(c);
        addReply(c, shared.ok);
        return;
    }

    // RESTORE-ASYNC PAYLOAD key serialized-fragment
    if (strcasecmp(c->argv[1]->ptr, "payload") == 0) {
        if (c->argc != 4) {
            goto failed_syntax_error;
        }
        if (c->restore_command_args == NULL) {
            c->restore_command_args = initRestoreCommandArgs(c, c->argv[2], 1);
        } else if (compareStringObjects(c->argv[2], c->restore_command_args->key) != 0) {
            goto failed_syntax_error;
        }
        restoreCommandArgs *args = c->restore_command_args;

        restoreGenericCommandAddFragment(args, c->argv[3]);

        addReply(c, shared.ok);
        return;
    }

    // RESTORE-ASYNC RESTORE key ttl [REPLACE]
    if (strcasecmp(c->argv[1]->ptr, "restore") == 0) {
        if (c->argc < 4) {
            goto failed_syntax_error;
        }
        int replace = 0;
        for (int j = 4; j < c->argc; j++) {
            if (strcasecmp(c->argv[j]->ptr, "replace") == 0) {
                replace = 1;
            } else {
                goto failed_syntax_error;
            }
        }

        long long ttl;
        if (getLongLongFromObjectOrReply(c, c->argv[3], &ttl, NULL) != C_OK) {
            return;
        } else if (ttl < 0) {
            addReplyError(c, "Invalid TTL value, must be >= 0");
            return;
        }

        if (!replace && lookupKeyWrite(c->db, c->argv[2]) != NULL) {
            addReply(c, shared.busykeyerr);
            return;
        }

        if (c->restore_command_args == NULL) {
            goto failed_syntax_error;
        } else if (compareStringObjects(c->argv[2], c->restore_command_args->key) != 0) {
            goto failed_syntax_error;
        }
        restoreCommandArgs *args = c->restore_command_args;

        args->ttl = ttl;
        args->replace = replace;

        restoreAsyncCommandByCommandArgs(args);
        return;
    }

failed_syntax_error:
    c->flags |= CLIENT_CLOSE_AFTER_REPLY;
    addReply(c, shared.syntaxerr);
}

// RESTORE key ttl serialized-value [REPLACE] [ABSTTL] [IDLETIME lru_idle | FREQ lfu_freq] [ASYNC]
void restoreCommand(client *c) {
    int replace = 0;
    int absttl = 0;
    int non_blocking = 0;
    long long lfu_freq = -1;
    long long lru_idle = -1;
    long long lru_clock = -1;
    for (int j = 4; j < c->argc; j++) {
        if (strcasecmp(c->argv[j]->ptr, "replace") == 0) {
            replace = 1;
        } else if (strcasecmp(c->argv[j]->ptr, "absttl") == 0) {
            absttl = 1;
        } else if (strcasecmp(c->argv[j]->ptr, "idletime") == 0) {
            if (j == c->argc - 1 || lfu_freq != -1) {
                addReply(c, shared.syntaxerr);
                return;
            }
            if (getLongLongFromObjectOrReply(c, c->argv[j + 1], &lru_idle, NULL) != C_OK) {
                return;
            } else if (lru_idle < 0) {
                addReplyError(c, "Invalid IDLETIME value, must be >= 0");
                return;
            } else {
                lru_clock = LRU_CLOCK();
            }
            j++;
        } else if (strcasecmp(c->argv[j]->ptr, "freq") == 0) {
            if (j == c->argc - 1 || lru_idle != -1) {
                addReply(c, shared.syntaxerr);
                return;
            }
            if (getLongLongFromObjectOrReply(c, c->argv[j + 1], &lfu_freq, NULL) != C_OK) {
                return;
            } else if (!(lfu_freq >= 0 && lfu_freq <= 255)) {
                addReplyError(c, "Invalid FREQ value, must be >= 0 and <= 255");
                return;
            }
            j++;
        } else if (strcasecmp(c->argv[j]->ptr, "async") == 0) {
            non_blocking = 1;
        } else {
            addReply(c, shared.syntaxerr);
            return;
        }
    }

    if (non_blocking && (c->flags & CLIENT_LUA) != 0) {
        addReplyError(c, "Option ASYNC is not allowed from scripts.");
        return;
    }

    long long ttl;
    if (getLongLongFromObjectOrReply(c, c->argv[2], &ttl, NULL) != C_OK) {
        return;
    } else if (ttl < 0) {
        addReplyError(c, "Invalid TTL value, must be >= 0");
        return;
    }

    if (!replace && lookupKeyWrite(c->db, c->argv[1]) != NULL) {
        addReply(c, shared.busykeyerr);
        return;
    }
    restoreGenericCommandResetIfNeeded(c);

    restoreCommandArgs *args = initRestoreCommandArgs(c, c->argv[1], non_blocking);

    args->ttl = ttl;
    args->replace = replace;
    args->absttl = absttl;
    args->lfu_freq = lfu_freq;
    args->lru_idle = lru_idle;
    args->lru_clock = lru_clock;

    restoreGenericCommandAddFragment(args, c->argv[3]);

    serverAssert(c->restore_command_args == NULL);

    if (!args->non_blocking) {
        restoreGenericCommandExtractPayload(args);

        restoreGenericCommandReplyAndPropagate(args);

        freeRestoreCommandArgs(args);
        return;
    }
    c->restore_command_args = args;

    restoreAsyncCommandByCommandArgs(args);
}

static void restoreCommandNonBlockingCallback(restoreCommandArgs *args) {
    serverAssert(args->non_blocking && args->process_state == PROCESS_STATE_DONE);

    restoreGenericCommandReplyAndPropagate(args);

    if (args->client != NULL) {
        client *c = args->client;
        serverAssert(c->restore_command_args == args);
        unblockClient(c);
        serverAssert(c->restore_command_args == NULL && args->client == NULL);
    }

    freeRestoreCommandArgs(args);
}

void unblockClientFromRestore(client *c) {
    serverAssert(c->restore_command_args != NULL && c->restore_command_args->client == c);
    serverAssert(c->restore_command_args->non_blocking && c->restore_command_args->process_state != PROCESS_STATE_NONE);
    c->restore_command_args->client = NULL;
    c->restore_command_args = NULL;
}

void freeRestoreCommandArgsFromFreeClient(client *c) { restoreGenericCommandResetIfNeeded(c); }

void restoreCloseTimedoutCommands(void) {
    if (restore_command_args_list != NULL && listLength(restore_command_args_list) != 0) {
        listIter li;
        listNode *ln;
        listRewind(restore_command_args_list, &li);
        while ((ln = listNext(&li))) {
            restoreCommandArgs *args = listNodeValue(ln);
            if (!args->non_blocking || args->process_state != PROCESS_STATE_NONE) {
                continue;
            }
            if (server.unixtime - args->last_use_time <= 300) {
                continue;
            }
            client *c = args->client;
            serverAssert(c != NULL && c->restore_command_args == args);

            restoreGenericCommandResetIfNeeded(c);
        }
    }
}

// ---------------- BACKGROUND THREAD --------------------------------------- //

typedef struct {
    pthread_t thread;
    pthread_attr_t attr;
    pthread_cond_t cond;
    pthread_mutex_t mutex;
    struct {
        list *jobs;
        list *done;
    } migrate, restore;  // Should be guarded by mutex.
    int pipe_fds[2];     // pipe_fds[0]: read by master thread
                         // pipe_fds[1]: written by worker thread
} migrateCommandThread;

static void *migrateCommandThreadMain(void *privdata) {
#if defined(USE_JEMALLOC)
    do {
        unsigned arena_ind = 0;
        int ret = je_mallctl("thread.arena", NULL, NULL, &arena_ind, sizeof(arena_ind));
        if (ret != 0) {
            serverLog(LL_WARNING, "Call je_mallctl to set thread.arena=%d failed: %s", (int)arena_ind, strerror(ret));
        }
    } while (0);
#endif

    migrateCommandThread *p = privdata;

    while (1) {
        migrateCommandArgs *migrate_args = NULL;
        restoreCommandArgs *restore_args = NULL;

        pthread_mutex_lock(&p->mutex);
        {
            while (listLength(p->migrate.jobs) == 0 && listLength(p->restore.jobs) == 0) {
                pthread_cond_wait(&p->cond, &p->mutex);
            }
            if (listLength(p->migrate.jobs) != 0) {
                migrate_args = listNodeValue(listFirst(p->migrate.jobs));
                listDelNode(p->migrate.jobs, listFirst(p->migrate.jobs));
            }
            if (listLength(p->restore.jobs) != 0) {
                restore_args = listNodeValue(listFirst(p->restore.jobs));
                listDelNode(p->restore.jobs, listFirst(p->restore.jobs));
            }
        }
        pthread_mutex_unlock(&p->mutex);

        if (migrate_args != NULL) {
            if (migrateGenericCommandSendRequests(migrate_args)) {
                migrateGenericCommandFetchReplies(migrate_args);
            }
        }
        if (restore_args != NULL) {
            restoreGenericCommandExtractPayload(restore_args);
        }

        pthread_mutex_lock(&p->mutex);
        {
            if (migrate_args != NULL) {
                listAddNodeTail(p->migrate.done, migrate_args);
            }
            if (restore_args != NULL) {
                listAddNodeTail(p->restore.done, restore_args);
            }
        }
        pthread_mutex_unlock(&p->mutex);

        serverAssert(write(p->pipe_fds[1], ".", 1) == 1);
    }
    return NULL;
}

static void migrateCommandThreadCallback(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(fd);
    UNUSED(mask);

    migrateCommandThread *p = privdata;

    char byte;
    int n = read(p->pipe_fds[0], &byte, sizeof(byte));
    if (n != 1) {
        serverAssert(n == -1 && errno == EAGAIN);
    }

    while (1) {
        migrateCommandArgs *migrate_args = NULL;
        restoreCommandArgs *restore_args = NULL;

        pthread_mutex_lock(&p->mutex);
        {
            if (listLength(p->migrate.done) != 0) {
                migrate_args = listNodeValue(listFirst(p->migrate.done));
                listDelNode(p->migrate.done, listFirst(p->migrate.done));
            }
            if (listLength(p->restore.done) != 0) {
                restore_args = listNodeValue(listFirst(p->restore.done));
                listDelNode(p->restore.done, listFirst(p->restore.done));
            }
        }
        pthread_mutex_unlock(&p->mutex);

        if (migrate_args != NULL) {
            migrate_args->process_state = PROCESS_STATE_DONE;
            migrateCommandNonBlockingCallback(migrate_args);
        }
        if (restore_args != NULL) {
            restore_args->process_state = PROCESS_STATE_DONE;
            restoreCommandNonBlockingCallback(restore_args);
        }

        if (migrate_args == NULL && restore_args == NULL) {
            return;
        }
    }
}

static void migrateCommandThreadInit(migrateCommandThread *p) {
    size_t stacksize;
    pthread_attr_init(&p->attr);
    pthread_attr_getstacksize(&p->attr, &stacksize);
    while (stacksize < 4LL * 1024 * 1024) {
        stacksize = (stacksize < 1024) ? 1024 : stacksize * 2;
    }
    pthread_attr_setstacksize(&p->attr, stacksize);
    pthread_cond_init(&p->cond, NULL);
    pthread_mutex_init(&p->mutex, NULL);

    p->migrate.jobs = listCreate();
    p->migrate.done = listCreate();
    p->restore.jobs = listCreate();
    p->restore.done = listCreate();

    if (pipe(p->pipe_fds) != 0) {
        serverPanic("Fatal: create pipe '%s'.", strerror(errno));
        exit(1);
    }
    if (anetNonBlock(NULL, p->pipe_fds[0]) != ANET_OK) {
        serverPanic("Fatal: call anetNonBlock '%s'.", strerror(errno));
        exit(1);
    }
    if (aeCreateFileEvent(server.el, p->pipe_fds[0], AE_READABLE, migrateCommandThreadCallback, p) == AE_ERR) {
        serverPanic("Fatal: call aeCreateFileEvent '%s'.", strerror(errno));
        exit(1);
    }
    int ret = pthread_create(&p->thread, &p->attr, migrateCommandThreadMain, p);
    if (ret != 0) {
        serverPanic("Fatal: call pthread_create '%s'.", strerror(ret));
        exit(1);
    }
}

static migrateCommandThread migrate_command_threads[1];

void migrateBackgroundThreadInit(void) { migrateCommandThreadInit(&migrate_command_threads[0]); }

static void migrateCommandThreadAddMigrateJobTail(migrateCommandArgs *migrate_args) {
    serverAssert(migrate_args->non_blocking && migrate_args->process_state == PROCESS_STATE_NONE);

    migrateCommandThread *p = &migrate_command_threads[0];
    pthread_mutex_lock(&p->mutex);
    {
        migrate_args->process_state = PROCESS_STATE_QUEUED;
        listAddNodeTail(p->migrate.jobs, migrate_args);
        pthread_cond_broadcast(&p->cond);
    }
    pthread_mutex_unlock(&p->mutex);
}

static void migrateCommandThreadAddRestoreJobTail(restoreCommandArgs *restore_args) {
    serverAssert(restore_args->non_blocking && restore_args->process_state == PROCESS_STATE_NONE);

    migrateCommandThread *p = &migrate_command_threads[0];
    pthread_mutex_lock(&p->mutex);
    {
        restore_args->process_state = PROCESS_STATE_QUEUED;
        listAddNodeTail(p->restore.jobs, restore_args);
        pthread_cond_broadcast(&p->cond);
    }
    pthread_mutex_unlock(&p->mutex);
}

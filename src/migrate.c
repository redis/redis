#include "server.h"

// ---------------- MIGRATE CACHED SOCKET ----------------------------------- //

#define MIGRATE_SOCKET_CACHE_ITEMS 64
#define MIGRATE_SOCKET_CACHE_TTL 10  // In seconds.

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
    return sdscat(name, "#");
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
        if (cs->busy || server.unixtime - cs->last_use_time <= MIGRATE_SOCKET_CACHE_TTL) {
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

    if (dictSize(server.migrate_cached_sockets) == MIGRATE_SOCKET_CACHE_ITEMS) {
        dictEntry *entry = dictGetRandomKey(server.migrate_cached_sockets);
        migrateCloseSocket(dictGetVal(entry));
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

static sds syncPingCommand(int fd, mstime_t timeout) {
    rio cmd;
    rioInitWithBuffer(&cmd, sdsempty());

    const char *cmd_name = "PING";
    serverAssert(rioWriteBulkCount(&cmd, '*', 1));
    serverAssert(rioWriteBulkString(&cmd, cmd_name, strlen(cmd_name)));

    if (syncWriteBuffer(fd, cmd.io.buffer.ptr, timeout) != C_OK) {
        sdsfree(cmd.io.buffer.ptr);
        return sdscatfmt(sdsempty(), "-IOERR Command %s failed, sending error '%s'.\r\n", cmd_name, strerror(errno));
    }
    sdsfree(cmd.io.buffer.ptr);

    char buf[4096];
    if (syncReadLine(fd, buf, sizeof(buf), timeout) <= 0) {
        return sdscatfmt(sdsempty(), "-IOERR Command %s failed, reading error '%s'.\r\n", cmd_name, strerror(errno));
    }
    if (buf[0] != '+') {
        return sdscatfmt(sdsempty(), "-ERR Command %s failed, target replied: %s.\r\n", cmd_name, buf);
    }
    return NULL;
}

static sds syncAuthCommand(int fd, mstime_t timeout, sds password) {
    rio cmd;
    rioInitWithBuffer(&cmd, sdsempty());

    const char *cmd_name = "AUTH";
    serverAssert(rioWriteBulkCount(&cmd, '*', 2));
    serverAssert(rioWriteBulkString(&cmd, cmd_name, strlen(cmd_name)));
    serverAssert(rioWriteBulkString(&cmd, password, sdslen(password)));

    if (syncWriteBuffer(fd, cmd.io.buffer.ptr, timeout) != C_OK) {
        sdsfree(cmd.io.buffer.ptr);
        return sdscatfmt(sdsempty(), "-IOERR Command %s failed, sending error '%s'.\r\n", cmd_name, strerror(errno));
    }
    sdsfree(cmd.io.buffer.ptr);

    char buf[4096];
    if (syncReadLine(fd, buf, sizeof(buf), timeout) <= 0) {
        return sdscatfmt(sdsempty(), "-IOERR Command %s failed, reading error '%s'.\r\n", cmd_name, strerror(errno));
    }
    if (buf[0] != '+') {
        return sdscatfmt(sdsempty(), "-ERR Command %s failed, target replied: %s.\r\n", cmd_name, buf);
    }
    return NULL;
}

static sds syncSelectCommand(int fd, mstime_t timeout, int dbid) {
    rio cmd;
    rioInitWithBuffer(&cmd, sdsempty());

    const char *cmd_name = "SELECT";
    serverAssert(rioWriteBulkCount(&cmd, '*', 2));
    serverAssert(rioWriteBulkString(&cmd, cmd_name, strlen(cmd_name)));
    serverAssert(rioWriteBulkLongLong(&cmd, dbid));

    if (syncWriteBuffer(fd, cmd.io.buffer.ptr, timeout) != C_OK) {
        sdsfree(cmd.io.buffer.ptr);
        return sdscatfmt(sdsempty(), "-IOERR Command %s failed, sending error '%s'.\r\n", cmd_name, strerror(errno));
    }
    sdsfree(cmd.io.buffer.ptr);

    char buf[4096];
    if (syncReadLine(fd, buf, sizeof(buf), timeout) <= 0) {
        return sdscatfmt(sdsempty(), "-IOERR Command %s failed, reading error '%s'.\r\n", cmd_name, strerror(errno));
    }
    if (buf[0] != '+') {
        return sdscatfmt(sdsempty(), "-ERR Command %s failed, target replied: %s.\r\n", cmd_name, buf);
    }
    return NULL;
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
        sds buffer;
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

#define RIO_MAX_IOBUF_LEN (64LL * 1024 * 1024)

static int rioMigrateCommandFlushIOBuffer(rioMigrateCommand *cmd) {
    // TODO
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

static const rio _rioMigrateObjectIO = {
    .read = _rioMigrateObjectRead,
    .tell = _rioMigrateObjectTell,
    // .flush = _rioMigrateObjectFlush,
    // .write = _rioMigrateObjectWrite,
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
    memrev64ifbe(&ver);
    RIO_GOTO_IF_ERROR(rioWrite(rio, &ver, sizeof(ver)));

    uint64_t crc = rio->cksum;
    memrev64ifbe(&crc);
    RIO_GOTO_IF_ERROR(rioWrite(rio, &crc, sizeof(crc)));

    RIO_GOTO_IF_ERROR(rioFlush(rio));
    return 1;

rio_failed_cleanup:
    return 0;
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

    // TODO: Not finished yet.
    UNUSED(p);
    return NULL;
}

static void migrateCommandThreadReadEvent(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(fd);
    UNUSED(mask);

    migrateCommandThread *p = privdata;

    // TODO: Not finished yet.
    UNUSED(p);
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
    if (aeCreateFileEvent(server.el, p->pipe_fds[0], AE_READABLE, migrateCommandThreadReadEvent, p) == AE_ERR) {
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

// TODO

void migrateCommand(client *c) {}
void restoreCommand(client *c) {}
void restoreCloseTimedoutCommands(void) {}
void freeMigrateCommandArgsFromFreeClient(client *c) {}
void freeRestoreCommandArgsFromFreeClient(client *c) {}

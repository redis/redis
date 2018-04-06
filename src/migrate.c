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
    sdsfree(cs->auth);
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

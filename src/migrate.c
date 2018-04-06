#include "server.h"

// ---------------- MIGRATE CACHED SOCKET ----------------------------------- //


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
        int ret = je_mallctl("thread.arena", NULL, NULL, &arena_ind,
                             sizeof(arena_ind));
        if (ret != 0) {
            serverLog(LL_WARNING,
                      "Call je_mallctl to set thread.arena=%d failed: %s",
                      (int)arena_ind, strerror(ret));
        }
    } while (0);
#endif

    migrateCommandThread *p = privdata;

    // TODO: Not finished yet.
    UNUSED(p);
    return NULL;
}

static void migrateCommandThreadReadEvent(aeEventLoop *el, int fd,
                                          void *privdata, int mask) {
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
    if (aeCreateFileEvent(server.el, p->pipe_fds[0], AE_READABLE,
                          migrateCommandThreadReadEvent, p) == AE_ERR) {
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

void migrateBackgroundThreadInit(void) {
    migrateCommandThreadInit(&migrate_command_threads[0]);
}

// TODO

void migrateCommand(client *c) {}
void restoreCommand(client *c) {}
void migrateCloseTimedoutSockets(void) {}
void restoreCloseTimedoutCommands(void) {}
void freeMigrateCommandArgsFromFreeClient(client *c) {}
void freeRestoreCommandArgsFromFreeClient(client *c) {}

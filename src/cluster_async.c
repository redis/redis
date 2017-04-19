#include "server.h"

void cleanupClientsForAsyncMigration() {
    /* TODO */
}

void releaseClientFromAsyncMigration(client *c) {
    /* TODO */
    (void)c;
}

void unblockClientFromAsyncMigration(client *c) {
    /* TODO */
    (void)c;
}

int *migrateAsyncGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    /* TODO */
    (void)cmd;
    (void)argv;
    (void)argc;
    (void)numkeys;
    return NULL;
}

void migrateAsyncCommand(client *c) {
    /* TODO */
    (void)c;
}

void migrateAsyncDumpCommand(client *c) {
    /* TODO */
    (void)c;
}

void migrateAsyncFenceCommand(client *c) {
    /* TODO */
    (void)c;
}

void migrateAsyncStatusCommand(client *c) {
    /* TODO */
    (void)c;
}

void migrateAsyncCancelCommand(client *c) {
    /* TODO */
    (void)c;
}

void restoreAsyncCommand(client *c) {
    /* TODO */
    (void)c;
}

void restoreAsyncAuthCommand(client *c) {
    /* TODO */
    (void)c;
}

void restoreAsyncAckCommand(client *c) {
    /* TODO */
    (void)c;
}

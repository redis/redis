#include "server.h"
#include <sys/fcntl.h>


void importDataFinishIntoDb(connection *conn) {
    connSetWriteHandler(conn, NULL);
    connSetReadHandler(conn, NULL);
    if (server.import_data_state == IMPORT_DATA_FINISH_INTO_DB) {
        char buf[128];
        int buflen = snprintf(buf, sizeof(buf), "+FINISH\r\n");
        if (connWrite(conn, buf, buflen) != buflen) {
            linkClient(server.import_data_client);
            freeClientAsync(server.import_data_client);
            server.import_data_state = IMPORT_DATA_FAIL_SEND_RESULT;
        }else{
            server.import_data_state = IMPORT_DATA_BEGIN_INIT;
            linkClient(server.import_data_client);
            connSetReadHandler(conn, readQueryFromClient);
        }
    }
    return;
}

void importDataCommand(client *c) {
    //
    if (server.import_data_state > IMPORT_DATA_BEGIN_INIT) {
        robj *res = createObject(OBJ_STRING, sdsnew("-NOT INIT\r\n"));
        addReply(c, res);
        decrRefCount(res);
        return;
    }
    listDelNode(server.clients, c->client_list_node);
    server.import_data_client = c;
    long long startSlot, endSlot;
    char buf[128];
    getLongLongFromObject(c->argv[1], &startSlot);
    getLongLongFromObject(c->argv[2], &endSlot);
    connSetReadHandler(c->conn, NULL);
    connSetWriteHandler(c->conn, NULL);
    /* Prepare a suitable temp file for bulk transfer */
    char tmpfile[256];
    int dfd;

    snprintf(tmpfile, 256,
             "temp-%d.%ld.rdb", (int) server.unixtime, (long int) getpid());
    dfd = open(tmpfile, O_CREAT | O_WRONLY | O_EXCL, 0644);
    if (dfd == -1) {
        serverLog(LL_WARNING, "Opening the temp file needed for import data: %s",
                  strerror(errno));
        server.import_data_state = IMPORT_DATA_FAIL_OPEN_DFD;
        goto error;
    }
    int buflen = snprintf(buf, sizeof(buf), "+CONTINUE\r\n");
    if (connWrite(c->conn, buf, buflen) != buflen) {
        server.import_data_state = IMPORT_DATA_FAIL_SEND_CONTINUE;
        goto error;
    }
    connSetReadHandler(c->conn, importDataReadSyncBulkPayload);
    server.import_data_transfer_size = -1;
    server.import_data_transfer_read = 0;
    server.import_data_transfer_last_fsync_off = 0;
    server.import_data_transfer_fd = dfd;
    server.import_data_transfer_lastio = server.unixtime;
    server.import_data_transfer_tmpfile = zstrdup(tmpfile);
    return;


    error:
    connSetReadHandler(c->conn, NULL);
    connSetWriteHandler(c->conn, NULL);
    if (dfd != -1) close(dfd);
    linkClient(server.import_data_client);
    freeClientAsync(server.import_data_client);
}
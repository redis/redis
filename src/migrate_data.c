//
// Created by kopoto on 7/24/21.
//


#include "server.h"

void migrateDataWaitTarget(connection *conn) {
    sds reply;
    if (server.migrate_data_state == MIGRATE_DATA_FINISH_RDB) {
        //之前被设置为非阻塞
        anetNonBlock(NULL, conn->fd);
        reply = receiveSynchronousResponse(conn, 300);
        if (!strncmp(reply, "+FINISH", 7)) {
            server.migrate_data_state = MIGRATE_DATA_BEGIN_INCREMENT;
            connSetReadHandler(conn, NULL);
            connSetWriteHandler(conn, migrateDataWaitTarget);
            return;
        } else {
            connSetReadHandler(conn, NULL);
            connSetWriteHandler(conn, NULL);
            server.migrate_data_state = MIGRATE_DATA_FAIL_RECEIVE_ID;
            connClose(conn);
            return;
        }
    }
    if (server.migrate_data_state == MIGRATE_DATA_BEGIN_INCREMENT) {
        int buflen = sdslen(server.migrate_data_buf);
        if (buflen == 0) {
            sdsfree(server.migrate_data_buf);
            connClose(conn);
            server.migrate_data_state = MIGRATE_DATA_INIT;
            return;
        }
        if (connWrite(conn, server.migrate_data_buf, buflen) != buflen) {
            sdsfree(server.migrate_data_buf);
            connClose(conn);
            server.migrate_data_state = MIGRATE_DATA_FAIL_SEND_DATA;
        } else {
            sdsfree(server.migrate_data_buf);
            connClose(conn);
            server.migrate_data_state = MIGRATE_DATA_INIT;
        }
    }
}


void startMigrateData(connection *conn) {
    sds reply;
    char *err = NULL;
    if (server.migrate_data_state == MIGRATE_DATA_BEGIN) {
        connSetWriteHandler(conn, NULL);
        connSetReadHandler(conn, startMigrateData);
        // 表面目标节点准备开始发送数据
        char sSlot[24];
        char eSlot[24];
        ll2string(sSlot, sizeof(sSlot), server.startSlot);
        ll2string(eSlot, sizeof(eSlot), server.endSlot);
        err = sendCommand(conn, 300, "importdata", sSlot, eSlot, NULL);
        if (err) {
            //失败了
            server.migrate_data_state = MIGRATE_DATA_FAIL_NOTICE_TARGET;
            server.startSlot = -1;
            server.endSlot = -1;
            connClose(server.migrate_data_fd);
        } else {
            server.migrate_data_state = MIGRATE_DATA_NOTICE_TARGET;
        }
        return;
    }

    if (server.migrate_data_state == MIGRATE_DATA_NOTICE_TARGET) {
        reply = receiveSynchronousResponse(conn, 300);
        if (!strncmp(reply, "+CONTINUE", 9)) {
            server.migrate_data_state = MIGRATE_DATA_BEGIN_RDB;
            rdbSaveInfo rsi, *rsiptr;
            rsiptr = rdbPopulateSaveInfo(&rsi);
            int res = migrateDataRdbSaveToTargetSockets(rsiptr, conn);
            if (res == C_ERR) {
                serverLog(LL_WARNING, "Unable to connect to target: %s", strerror(errno));
                server.startSlot = -1;
                server.endSlot = -1;
                connClose(server.migrate_data_fd);
                server.migrate_data_state = MIGRATE_DATA_FAIL_START_RDB;
            } else {
                server.migrate_data_state = MIGRATE_DATA_SUCCESS_START_RDB;
                sdsfree(server.migrate_data_buf);
                server.migrate_data_buf = sdsempty();
            }
        } else {
            //TODO关闭
            server.startSlot = -1;
            server.endSlot = -1;
            connClose(server.migrate_data_fd);
            server.migrate_data_state = MIGRATE_DATA_TARGET_NOT_INIT;
        }
    }
}

void migrateDataCommand(client *c) {
    if (server.migrate_data_state > MIGRATE_DATA_INIT) {
        robj *res = createObject(OBJ_STRING, sdsnew("-can not start\r\n"));
        addReply(c, res);
        decrRefCount(res);
        return;
    }

    long long startSlot, endSlot, port;
    robj *key = c->argv[1];
    getLongLongFromObject(c->argv[2], &port);
    getLongLongFromObject(c->argv[3], &startSlot);
    getLongLongFromObject(c->argv[4], &endSlot);
    server.startSlot = startSlot;
    server.endSlot = endSlot;

    server.migrate_data_fd = server.tls_replication ? connCreateTLS() : connCreateSocket();
    if (connConnect(server.migrate_data_fd, key->ptr, port,
                    NET_FIRST_BIND_ADDR, startMigrateData) == C_ERR) {
        connClose(server.migrate_data_fd);
        server.startSlot = -1;
        server.endSlot = -1;
        server.migrate_data_state = MIGRATE_DATA_FAIL_CONNECT_TARGET;
        robj *res = createObject(OBJ_STRING, sdsnew("-Unable to connect to target\r\n"));
        addReply(c, res);
        decrRefCount(res);
        return;
    }

    server.migrate_data_state = MIGRATE_DATA_BEGIN;
    addReply(c, shared.ok);

}
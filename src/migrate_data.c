//
// Created by kopoto on 7/24/21.
//


#include "server.h"

void migrateDataWaitReadTarget(connection *conn) {
    long long start = timeInMilliseconds();
    char buf[1024];
    read(conn->fd, buf, sizeof(buf));
    connSetWriteHandler(conn, migrateDataWaitTarget);
    connSetReadHandler(conn, NULL);
    long long end = timeInMilliseconds();
    if (end - start > 100) {
        serverLog(LL_WARNING, "migrateDataIncrementReadTarget %s cost %lld", buf, (end - start));
    }
}


void migrateDataWaitTarget(connection *conn) {
    long long start = timeInMilliseconds();
    sds reply;
    if (server.migrate_data_state == MIGRATE_DATA_FINISH_RDB) {
        reply = receiveSynchronousResponse(conn, 300);
        if (!strncmp(reply, "+FINISH", 7)) {
            serverLog(LL_WARNING, "target success to finish import rdb data");
            server.migrate_data_state = MIGRATE_DATA_BEGIN_INCREMENT;
            connSetReadHandler(conn, NULL);
            connSetWriteHandler(conn, migrateDataWaitTarget);
            return;
        } else {
            serverLog(LL_WARNING, "target fail to finish migrate data");
            server.migrate_data_state = MIGRATE_DATA_FAIL_RECEIVE_ID;
            goto error;
        }
    }
    if (server.migrate_data_state == MIGRATE_DATA_BEGIN_INCREMENT) {
        serverLog(LL_WARNING, "increment data  %ld",server.migrate_data_list_buf->len);
        if (server.migrate_data_list_buf->len == 0) {
            server.startSlot = -1;
            server.endSlot = -1;
            listRelease(server.migrate_data_list_buf);
            server.migrate_data_state = MIGRATE_DATA_INIT;
            linkClient(server.migrate_data_client);
            freeClientAsync(server.migrate_data_client);
            serverLog(LL_WARNING, "success to finish to send increment data cost %lld",
                      (timeInMilliseconds() - server.migrate_data_begin));
            return;
        }
        sds buf = server.migrate_data_list_buf->head->value;
        int buflen = sdslen(buf);
        if (connWrite(conn, buf, buflen) != buflen) {
            serverLog(LL_WARNING, "fail to send increment data %s", strerror(errno));
            server.migrate_data_state = MIGRATE_DATA_FAIL_SEND_INCREMENT_DATA;
            goto error;
        } else {
            listDelNode(server.migrate_data_list_buf, server.migrate_data_list_buf->head);
            connSetWriteHandler(conn, NULL);
            connSetReadHandler(conn, migrateDataWaitReadTarget);
            long long end = timeInMilliseconds();
            if (end - start > 100) {
                serverLog(LL_WARNING, "migrateDataIncrementReadTarget %s cost %lld", buf, (end - start));
            }
            if (server.migrate_data_list_buf->len == 0) {
                server.startSlot = -1;
                server.endSlot = -1;
                listRelease(server.migrate_data_list_buf);
                server.migrate_data_state = MIGRATE_DATA_INIT;
                linkClient(server.migrate_data_client);
                freeClientAsync(server.migrate_data_client);
                serverLog(LL_WARNING, "success to finish to send increment data cost %lld",
                          (timeInMilliseconds() - server.migrate_data_begin));
                return;
            }
            return;
        }
    }
    error:
    server.startSlot = -1;
    server.endSlot = -1;
    listRelease(server.migrate_data_list_buf);
    linkClient(server.migrate_data_client);
    freeClientAsync(server.migrate_data_client);
    return;
}


void startMigrateData(connection *conn) {
    char *err;
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
            serverLog(LL_WARNING, "fail to notice target for migrate data by rdb");
            server.migrate_data_state = MIGRATE_DATA_FAIL_NOTICE_TARGET;
            goto error;
        } else {
            serverLog(LL_WARNING, "success to notice target to migrate data by rdb");
            server.migrate_data_state = MIGRATE_DATA_NOTICE_TARGET;
            return;
        }
    }

    if (server.migrate_data_state == MIGRATE_DATA_NOTICE_TARGET) {
        sds reply;
        reply = receiveSynchronousResponse(conn, 300);
        if (!strncmp(reply, "+CONTINUE", 9)) {
            sdsfree(reply);
            serverLog(LL_WARNING, "Target able to continue migrate data by rdb");
            server.migrate_data_state = MIGRATE_DATA_BEGIN_RDB;
            rdbSaveInfo rsi, *rsiptr;
            rsiptr = rdbPopulateSaveInfo(&rsi);
            int res = migrateDataRdbSaveToTargetSockets(rsiptr, conn);
            if (res == C_ERR) {
                serverLog(LL_WARNING, "Fail background rdb save to target sockect");
                server.migrate_data_state = MIGRATE_DATA_FAIL_START_RDB;
                goto error;
            } else {
                serverLog(LL_WARNING, "success background rdb save to target sockect");
                server.migrate_data_state = MIGRATE_DATA_SUCCESS_START_RDB;
                server.migrate_data_list_buf = listCreate();
                listSetFreeMethod(server.migrate_data_list_buf, (void (*)(void *)) sdsfree);
                return;
            }
        } else {
            //TODO关闭
            serverLog(LL_WARNING, "target unable to continue migrate data by rdb: %s", reply);
            server.migrate_data_state = MIGRATE_DATA_TARGET_NOT_INIT;
            sdsfree(reply);
            goto error;
        }
    }
    if (err) {
        sdsfree(err);
    }
    return;
    error:
    if (err) {
        sdsfree(err);
    }
    server.startSlot = -1;
    server.endSlot = -1;
    connClose(server.migrate_data_fd);
    return;
}

void migrateDataCommand(client *c) {
    if (server.migrate_data_state > MIGRATE_DATA_INIT) {
        robj *res = createObject(OBJ_STRING, sdsnew("-can not start\r\n"));
        addReply(c, res);
        decrRefCount(res);
        return;
    }
    if (hasActiveChildProcess()) {
        robj *res = createObject(OBJ_STRING, sdsnew("-can not start has child pid\r\n"));
        addReply(c, res);
        decrRefCount(res);
        return;
    }

    long long startSlot, endSlot, port;
    robj *key = c->argv[1];
    getLongLongFromObject(c->argv[2], &port);
    getLongLongFromObject(c->argv[3], &startSlot);
    getLongLongFromObject(c->argv[4], &endSlot);
    server.migrate_data_begin = timeInMilliseconds();
    server.startSlot = startSlot;
    server.endSlot = endSlot;

    server.migrate_data_fd = server.tls_replication ? connCreateTLS() : connCreateSocket();
    if (connConnect(server.migrate_data_fd, key->ptr, (int) port, NET_FIRST_BIND_ADDR, startMigrateData) == C_ERR) {
        serverLog(LL_WARNING, "Unable to connect to target to migrate data by rdb");
        server.migrate_data_state = MIGRATE_DATA_FAIL_CONNECT_TARGET;
        robj *res = createObject(OBJ_STRING, sdsnew("-Unable to connect to target\r\n"));
        addReply(c, res);
        decrRefCount(res);
        goto error;
    }

    server.migrate_data_state = MIGRATE_DATA_BEGIN;
    robj *res = createObject(OBJ_STRING, sdsnew("+try to migrate data by rdb\r\n"));
    addReply(c, res);
    decrRefCount(res);
    return;
    error:
    server.startSlot = -1;
    server.endSlot = -1;
    connClose(server.migrate_data_fd);
    server.migrate_data_begin = 0;
    return;

}
/* ==========================================================================
 * connection.c - connection layer framework
 * --------------------------------------------------------------------------
 * Copyright (C) 2022  zhenwei pi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the
 * following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 * ==========================================================================
 */

#include "server.h"
#include "connection.h"

static ConnectionType *connTypes[CONN_TYPE_MAX];

/* Registers a new connection type */
int connTypeRegister(ConnectionType *ct) {
    const char *typename = ct->get_type(NULL);
    ConnectionType *tmpct;
    int type;

    /* Find an empty slot to store the new connection type */
    for (type = 0; type < CONN_TYPE_MAX; type++) {
        tmpct = connTypes[type];
        if (!tmpct)
            break;

        /* Ignore case, we don't differentiate "tls"/"TLS" */
        if (!strcasecmp(typename, tmpct->get_type(NULL))) {
            serverLog(LL_WARNING, "Connection type %s already registered", typename);
            return C_ERR;
        }
    }

    /* Handle case where no empty slot is found */
    if (type == CONN_TYPE_MAX) {
        serverLog(LL_WARNING, "No available slot for connection type %s", typename);
        return C_ERR;
    }

    serverLog(LL_INFO, "Connection type %s registered", typename);
    connTypes[type] = ct;

    /* Initialize the connection type if needed */
    if (ct->init) {
        ct->init();
    }

    return C_OK;
}

/* Initializes all required connection types */
int connTypeInitialize(void) {
    /* Socket connection type is mandatory */
    if (RedisRegisterConnectionTypeSocket() != C_OK) {
        serverLog(LL_ERROR, "Failed to register socket connection type");
        return C_ERR;
    }

    /* Unix socket connection type is mandatory */
    if (RedisRegisterConnectionTypeUnix() != C_OK) {
        serverLog(LL_ERROR, "Failed to register Unix connection type");
        return C_ERR;
    }

    /* TLS connection type may fail if TLS support is not enabled */
    RedisRegisterConnectionTypeTLS();

    return C_OK;
}

/* Retrieves a connection type by its string name */
ConnectionType *connectionByType(const char *typename) {
    ConnectionType *ct;

    for (int type = 0; type < CONN_TYPE_MAX; type++) {
        ct = connTypes[type];
        if (ct && !strcasecmp(typename, ct->get_type(NULL))) {
            return ct;
        }
    }

    serverLog(LL_WARNING, "Missing implementation of connection type %s", typename);
    return NULL;
}

/* Caches and returns the TCP connection type */
ConnectionType *connectionTypeTcp(void) {
    static ConnectionType *ct_tcp = NULL;

    if (ct_tcp != NULL)
        return ct_tcp;

    ct_tcp = connectionByType(CONN_TYPE_SOCKET);
    serverAssert(ct_tcp != NULL);

    return ct_tcp;
}

/* Caches and returns the TLS connection type, which can be missing */
ConnectionType *connectionTypeTls(void) {
    static ConnectionType *ct_tls = NULL;
    static int cached = 0;

    if (!cached) {
        cached = 1;
        ct_tls = connectionByType(CONN_TYPE_TLS);
    }

    return ct_tls;
}

/* Caches and returns the Unix connection type */
ConnectionType *connectionTypeUnix(void) {
    static ConnectionType *ct_unix = NULL;

    if (ct_unix != NULL)
        return ct_unix;

    ct_unix = connectionByType(CONN_TYPE_UNIX);
    return ct_unix;
}

/* Returns the index of a connection type by its string name */
int connectionIndexByType(const char *typename) {
    ConnectionType *ct;

    for (int type = 0; type < CONN_TYPE_MAX; type++) {
        ct = connTypes[type];
        if (ct && !strcasecmp(typename, ct->get_type(NULL))) {
            return type;
        }
    }

    return -1;
}

/* Cleans up all registered connection types */
void connTypeCleanupAll(void) {
    ConnectionType *ct;

    for (int type = 0; type < CONN_TYPE_MAX; type++) {
        ct = connTypes[type];
        if (!ct)
            break;

        if (ct->cleanup)
            ct->cleanup();
    }
}

/* Checks if any connection type has pending data */
int connTypeHasPendingData(void) {
    ConnectionType *ct;
    int ret = 0;

    for (int type = 0; type < CONN_TYPE_MAX; type++) {
        ct = connTypes[type];
        if (ct && ct->has_pending_data && (ret = ct->has_pending_data())) {
            return ret;
        }
    }

    return ret;
}

/* Processes pending data for each connection type */
int connTypeProcessPendingData(void) {
    ConnectionType *ct;
    int ret = 0;

    for (int type = 0; type < CONN_TYPE_MAX; type++) {
        ct = connTypes[type];
        if (ct && ct->process_pending_data) {
            ret += ct->process_pending_data();
        }
    }

    return ret;
}

/* Generates a string with information about all listeners */
sds getListensInfoString(sds info) {
    for (int j = 0; j < CONN_TYPE_MAX; j++) {
        connListener *listener = &server.listeners[j];
        if (listener->ct == NULL)
            continue;

        info = sdscatfmt(info, "listener%i:name=%s", j, listener->ct->get_type(NULL));
        for (int i = 0; i < listener->count; i++) {
            info = sdscatfmt(info, ",bind=%s", listener->bindaddr[i]);
        }

        if (listener->port)
            info = sdscatfmt(info, ",port=%i", listener->port);

        info = sdscatfmt(info, "\r\n");
    }

    return info;
}

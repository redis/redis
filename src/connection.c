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

int connTypeRegister(ConnectionType *ct) {
    int type = ct->get_type(NULL);

    /* unknown connection type as a fatal error */
    if (type >= CONN_TYPE_MAX) {
        serverPanic("Unsupported connection type %d", type);
    }

    if (connTypes[type] == ct) {
        serverLog(LL_WARNING, "Connection type %d already registered", type);
        return C_OK;
    }

    serverLog(LL_VERBOSE, "Connection type %d registered", type);
    connTypes[type] = ct;

    if (ct->init) {
        ct->init();
    }

    return C_OK;
}

int connTypeInitialize() {
    /* currently socket connection type is necessary  */
    serverAssert(RedisRegisterConnectionTypeSocket() == C_OK);

    /* may fail if without BUILD_TLS=yes */
    RedisRegisterConnectionTypeTLS();

    return C_OK;
}

ConnectionType *connectionByType(int type) {
    ConnectionType *ct;

    serverAssert(type < CONN_TYPE_MAX);

    ct = connTypes[type];
    if (!ct) {
        serverLog(LL_WARNING, "Missing implement of connection type %d", type);
    }

    return ct;
}

void connTypeCleanup(int type) {
    ConnectionType *ct = connectionByType(type);

    if (ct && ct->cleanup) {
        ct->cleanup();
    }
}

void connTypeCleanupAll() {
    int type;

    for (type = 0; type < CONN_TYPE_MAX; type++) {
        connTypeCleanup(type);
    }
}

int connTypeConfigure(int type, void *priv, int reconfigure) {
    ConnectionType *ct = connectionByType(type);

    if (ct && ct->configure) {
        return ct->configure(priv, reconfigure);
    }

    return C_ERR;
}

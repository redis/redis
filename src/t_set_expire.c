/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* Set member expiration (SME): commands and encoding-independent expiry logic.
 *
 * The functions below are placeholders: no set carries an expiration yet, so
 * they must never be reached with a registered set. */

#include "server.h"

/* Retrieve the ExpireMeta associated with the set, used by db->subexpires.
 * The caller is responsible for ensuring that it is indeed attached. */
ExpireMeta *setGetExpireMeta(const eItem set) {
    UNUSED(set);
    serverPanic("Set member expiration is not implemented yet");
}

/* Returns 1 if the set currently has an ExpireMeta attached and can be
 * registered in db->subexpires. */
int setHasSubexpiry(const kvobj *o) {
    serverAssert(o->type == OBJ_SET);
    return 0;
}

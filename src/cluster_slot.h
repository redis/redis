#ifndef __CLUSTER_SLOT_H
#define __CLUSTER_SLOT_H

/*-----------------------------------------------------------------------------
 * Redis cluster slot data structures, defines, exported API.
 *----------------------------------------------------------------------------*/

#define ORDER_BY_KEY_COUNT 1
#define ORDER_BY_INVALID -1

#include "server.h"

int getSlotOrReply(client *c, robj *o);

#endif /* __CLUSTER_SLOT_H */

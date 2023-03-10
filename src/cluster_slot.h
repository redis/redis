#ifndef __CLUSTER_SLOT_H
#define __CLUSTER_SLOT_H

/*-----------------------------------------------------------------------------
 * Redis cluster slot data structures, defines, exported API.
 *----------------------------------------------------------------------------*/

#include "server.h"

int getSlotOrReply(client *c, robj *o);

#endif /* __CLUSTER_SLOT_H */

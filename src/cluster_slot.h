#ifndef __CLUSTER_SLOT_H
#define __CLUSTER_SLOT_H

#define ORDER_BY_KEY_COUNT 1
#define ORDER_BY_CPU_USAGE 2
#define ORDER_BY_MEMORY_USAGE 3
#define ORDER_BY_INVALID -1

#include "server.h"

typedef struct sortedSlotStatEntry {
    int slot;
    uint64_t stat;
} sortedSlotStatEntry;

void addReplySlotStats(client *c, unsigned char *slots);
int checkSlotAssignmentsOrReply(client *c, unsigned char *slots, int del, int start_slot, int end_slot);
int checkSlotStatsOrderByArgumentOrReply(client *c, int *order_by, int *limit, int *desc);
int checkSlotStatsSlotsArgumentOrReply(client *c, unsigned char *slots);
int checkSlotStatsSlotsRangeArgumentOrReply(client *c, unsigned char *slots);
int getSlotOrReply(client *c, robj *o);
void markAssignedSlots(unsigned char *slots);
void sortAndAddReplySlotStats(client *c, int order_by, int limit, int desc);
void sortSlotStats(sortedSlotStatEntry sorted[], int num_slots_assigned, int order_by, int desc);

#endif /* __CLUSTER_SLOT_H */

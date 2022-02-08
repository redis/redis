#ifndef __REDIS_CTRIP_GTID_H
#define __REDIS_CTRIP_GTID_H

#include "server.h"

typedef struct gtid_interval gtid_interval;

typedef long long int rpl_gno;

struct gtid_interval {

    rpl_gno gno_start;

    rpl_gno gno_end;
};

struct uuid_set {

    char rpl_sid[CONFIG_RUN_ID_SIZE+1];

    gtid_interval* gtid_intervals;
};

struct gtid_set {

    uuid_set* uuid_sets;
};

#endif  /* __REDIS_CTRIP_GTID_H */
#include "server.h"

#define HOT_DATA 0
#define COLD_DATA 1
struct ctripRdbLoadResult {
    int type; //HOT_DATA || COLD_DATA
    robj* val;
    sds cold_data;
    int error;
};

void ctripRdbLoadObject(int rdbtype, rio *rdb, sds key, struct ctripRdbLoadResult* result);
int ctripDbAddColdData(redisDb* db, int datatype, sds key, robj* evict, sds cold_data, long long expire_time);


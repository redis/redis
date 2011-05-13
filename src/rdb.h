#ifndef __REDIS_RDB_H
#define __REDIS_RDB_H

#include <stdio.h>
#include "rio.h"

/* TBD: include only necessary headers. */
#include "redis.h"

int rdbLoad(char *filename);
int rdbSaveBackground(char *filename);
void rdbRemoveTempFile(pid_t childpid);
int rdbSave(char *filename);
int rdbSaveObject(rio *rdb, robj *o);
off_t rdbSavedObjectLen(robj *o);
off_t rdbSavedObjectPages(robj *o);
robj *rdbLoadObject(int type, rio *rdb);
void backgroundSaveDoneHandler(int exitcode, int bysignal);
int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val, time_t expireitme, time_t now);
int rdbLoadType(rio *rdb);
time_t rdbLoadTime(rio *rdb);
robj *rdbLoadStringObject(rio *rdb);
int rdbSaveType(rio *rdb, unsigned char type);
int rdbSaveLen(rio *rdb, uint32_t len);

#endif

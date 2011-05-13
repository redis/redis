#ifndef __REDIS_RDB_H
#define __REDIS_RDB_H

#include <stdio.h>
#include "rio.h"

/* TBD: include only necessary headers. */
#include "redis.h"

/* Dup object types to RDB object types. Only reason is readability (are we
 * dealing with RDB types or with in-memory object types?). */
#define REDIS_RDB_TYPE_STRING 0
#define REDIS_RDB_TYPE_LIST   1
#define REDIS_RDB_TYPE_SET    2
#define REDIS_RDB_TYPE_ZSET   3
#define REDIS_RDB_TYPE_HASH   4

/* Object types for encoded objects. */
#define REDIS_RDB_TYPE_HASH_ZIPMAP    9
#define REDIS_RDB_TYPE_LIST_ZIPLIST  10
#define REDIS_RDB_TYPE_SET_INTSET    11
#define REDIS_RDB_TYPE_ZSET_ZIPLIST  12

/* Test if a type is an object type. */
#define rdbIsObjectType(t) ((t >= 0 && t <= 4) || (t >= 9 && t <= 12))

/* Special RDB opcodes (saved/loaded with rdbSaveType/rdbLoadType). */
#define REDIS_RDB_OPCODE_EXPIRETIME 253
#define REDIS_RDB_OPCODE_SELECTDB   254
#define REDIS_RDB_OPCODE_EOF        255

/* Test if a type is an opcode. */
#define rdbIsOpcode(t) (t >= 253 && t <= 255)

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
int rdbSaveObjectType(rio *rdb, robj *o);
int rdbLoadObjectType(rio *rdb);

#endif

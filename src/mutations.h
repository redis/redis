#ifndef __MUTATION_LOG_H
#define __MUTATION_LOG_H

// Forward declaration of redis structs on server.h
struct redisObject;
typedef struct redisObject robj;

struct client;
typedef struct client client;

struct redisDb;
typedef struct redisDb redisDb;
///////////////////////

typedef struct mutationOperation mutationOperation;

/* Creates a mutation operation.
 * A mutation operation consists of a key, and undo function,
 * a function to destroy data to be used by the undo function,
 * and the number of values that will be recorded for the undo
 * function. Those values can be recorded using mutationOperationSetData.
 */
mutationOperation* mutationOperationCreate(robj* key,
                                           void (*undoOperation)(robj* key, void** data, unsigned int nData),
                                           void (*dataDestructor)(void **data, unsigned int nData),
                                           unsigned int nData);

/* Destroys a mutation operation.
 */
void mutationOperationDestroy(mutationOperation *m);

/* Set data for undo function of the the mutation operation.
 */

void mutationOperationSetData(mutationOperation* m, unsigned int idx, void* value);

typedef struct mutationLog mutationLog;

/* Creates a mutation Log.
 */
mutationLog* mutationLogCreate();

/* Destroys a mutation Log.
 */
void mutationLogDestroy(mutationLog* ml);

/* Performs the commit. Discard all mutations since there will be no need to
 * undo them and sends all keyspace events and modified key signals in the
 * same order they were made.
 */
void mutationLogCommit(mutationLog* ml);

/* Performs the rollback. All mutation are undone in reverse order they were
 * made and all keyspace events and modified keys signals are discarded.
 */
void mutationLogRollback(mutationLog* ml);

/* Record a mutation.
 */
void mutationLogRecordMutation(mutationLog* ml, mutationOperation* mo);

/* Records a modified key signal.
 */
void mutationLogRecordModifiedKey(mutationLog* ml, client* c, redisDb *db, robj *key);

/* Records a keyspace event.
 */
void mutationLogRecordKeyspaceEvent(mutationLog* ml, int type, char* event, robj* key, int dbid);
#endif

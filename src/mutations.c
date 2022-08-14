#include <assert.h>
#include <string.h>
#include "zmalloc.h"
#include "mutations.h"

#include "adlist.h"

////////////// Taken from server.h
void incrRefCount(robj* obj);
void decrRefCount(robj* obj);

void notifyKeyspaceEvent(int type, char *event, robj *key, int dbid);
void signalModifiedKey(client *c, redisDb *db, robj *key);
////////////////////////////////////////////////////

struct mutationOperation {
    robj *key;
    void (*undoOperation)(robj *key, void* data[], unsigned int nData);
    void (*dataDestructor)(void *data[], unsigned int nData);
    unsigned int nData;
    void **data;
};

/* Creates a mutation operation.
 * A mutation operation consists of a key, and undo function,
 * a function to destroy data to be used by the undo function,
 * and the number of values that will be recorded for the undo
 * function. Those values can be recorded using mutationOperationSetData.
 */
mutationOperation* mutationOperationCreate(robj* key,
                                           void (*undoOperation)(robj* key, void** data, unsigned int nData),
                                           void (*dataDestructor)(void **data, unsigned int nData),
                                           unsigned int nData) {
    assert(key != NULL);
    assert(undoOperation != NULL);
    assert(nData > 0 && dataDestructor != NULL);
    mutationOperation *m = zmalloc(sizeof(mutationOperation));
    incrRefCount(key);
    m->key = key;
    m->undoOperation = undoOperation;
    m->dataDestructor = dataDestructor;
    m->nData = nData;
    if (m->nData > 0) {
        m->data = zmalloc(sizeof(void*) * nData);
        memset(m->data, 0, sizeof(void*) * nData);
    } else
        m->data = NULL;
    return m;
}

/* Destroys a mutation operation.
 */
void mutationOperationDestroy(mutationOperation *m) {
    if (m == NULL) return;
    if (m->dataDestructor) {
        m->dataDestructor(m->data, m->nData);
        zfree(m->data);
    }
    decrRefCount(m->key);
    zfree(m);
}

/* Set data for undo function of the the mutation operation.
 */
void mutationOperationSetData(mutationOperation* m, unsigned int idx, void* value) {
    assert(idx < m->nData);
    m->data[idx] = value;
}

/* Perform the undo operation of the mutation.
 */
void mutationOperationPerformUndo(mutationOperation* m) {
    m->undoOperation(m->key, m->data, m->nData);
}

typedef struct{
    client *c;
    redisDb *db;
    robj *key;
} modifiedKey;

/* Creates a modified key.
 */
modifiedKey* modifiedKeyCreate(client *c, redisDb *db, robj *key) {
    modifiedKey *notification = zmalloc(sizeof(modifiedKey));
    notification->c = c;
    notification->db = db;
    incrRefCount(key);
    notification->key = key;
    return notification;
}

/* Destroys a modified key.
 */
void modifiedKeyDestroy(modifiedKey *notification) {
    decrRefCount(notification->key);
    zfree(notification);
}

typedef struct {
    int type;
    char *event;
    robj *key;
    int dbid;
} keyspaceEvent;

/* Creates a keyspace event.
 */
keyspaceEvent* keyspaceEventCreate(int type, char *event, robj *key, int dbid) {
    keyspaceEvent *ke = zmalloc(sizeof(keyspaceEvent));
    ke->type = type;
    ke->event = event;
    incrRefCount(key);
    ke->key = key;
    ke->dbid = dbid;
    return ke;
}


/*Destroys a keyspace event.
 */
void keyspaceEventDestroy(keyspaceEvent *event) {
    zfree(event->event);
    decrRefCount(event->key);
    zfree(event);
}

struct mutationLog {
    list *mutations;
    list *keyspaceEvents;
    list *modifiedKeys;

};

/* Creates a mutation Log.
 */
mutationLog* mutationLogCreate() {
    mutationLog* ml = zmalloc(sizeof(mutationLog));
    assert(ml);
    ml->mutations = listCreate();
    listSetFreeMethod(ml->mutations, (void (*)(void*))mutationOperationDestroy);
    ml->keyspaceEvents = listCreate();
    listSetFreeMethod(ml->keyspaceEvents, (void (*)(void*))keyspaceEventDestroy);
    ml->modifiedKeys = listCreate();
    listSetFreeMethod(ml->modifiedKeys, (void (*)(void*))modifiedKeyDestroy);

    return ml;
}

/* Destroys a mutation Log.
 */
void mutationLogDestroy(mutationLog *ml) {
    if (ml == NULL) return;
    listRelease(ml->mutations);
    listRelease(ml->keyspaceEvents);
    listRelease(ml->modifiedKeys);
    zfree(ml);
}

/* Record a mutation by inserting it at the front of the mutations list.
 */
void mutationLogRecordMutation(mutationLog* ml, mutationOperation* mo) {
    assert(ml != NULL);
    assert(mo != NULL);
    assert(listAddNodeHead(ml->mutations, mo) != NULL);
}

/* Performs the commit. Discard all mutations since there will be no need to
 * undo them and sends all keyspace events and modified key signals in the
 * same order they were made.
 */
void mutationLogCommit(mutationLog* ml) {
    if (ml == NULL) return;

    listIter *iter = listGetIterator(ml->keyspaceEvents, AL_START_HEAD);
    listNode *node = listNext(iter);
    while(node != NULL) {
        keyspaceEvent *event = (keyspaceEvent*) node->value;
        notifyKeyspaceEvent(event->type, event->event, event->key, event->dbid);
        node = listNext(iter);
    }

    iter = listGetIterator(ml->modifiedKeys, AL_START_HEAD);
    node = listNext(iter);
    while(node != NULL) {
        modifiedKey *mk = (modifiedKey*) node->value;
        signalModifiedKey(mk->c, mk->db, mk->key);
        node = listNext(iter);
    }

    listEmpty(ml->mutations);
    listEmpty(ml->keyspaceEvents);
    listEmpty(ml->modifiedKeys);
}

/* Performs the rollback. All mutation are undone in reverse order they were
 * made and all keyspace events and modified keys signals are discarded.
 */
void mutationLogRollback(mutationLog* ml) {
    if (ml == NULL) return;

    listIter *iter = listGetIterator(ml->mutations, AL_START_HEAD);
    listNode *node = listNext(iter);
    while(node != NULL) {
        mutationOperationPerformUndo((mutationOperation*) node->value);
        node = listNext(iter);
    }

    listEmpty(ml->mutations);
    listEmpty(ml->keyspaceEvents);
    listEmpty(ml->modifiedKeys);
}

/* Records a modified key signal.
 */
void mutationLogRecordModifiedKey(mutationLog* ml, client* c, redisDb *db, robj *key) {
    assert(ml != NULL);
    modifiedKey *newNotification = modifiedKeyCreate(c, db, key);
    assert(listAddNodeTail(ml->modifiedKeys, newNotification) != NULL);
}

/* Records a keyspace event.
 */
void mutationLogRecordKeyspaceEvent(mutationLog* ml, int type, char* event, robj* key, int dbid) {
    assert(ml != NULL);
    keyspaceEvent *newEvent = keyspaceEventCreate(type, event, key, dbid);
    assert(listAddNodeTail(ml->keyspaceEvents, newEvent) != NULL);
}

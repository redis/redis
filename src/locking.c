#include "redis.h"

/*-----------------------------------------------------------------------------
 * Generic locking (grab and release) commands
 *----------------------------------------------------------------------------*/

int grabLockForKey(redisClient *c, robj *key)
{
	dictEntry *de;

    de = dictFind(c->db->locked_keys,key);
	if (de == NULL) {
		int retval;
		robj *kt;

		kt = dupStringObject(key);
		/* Add the client structure to the locked_keys dict */
		retval = dictAdd(c->db->locked_keys,key,c);
		incrRefCount(key);
		redisAssert(retval == DICT_OK);

		if (c->lock.keys == NULL) {
			c->lock.keys = listCreate();
			listSetMatchMethod(c->lock.keys,listMatchObjects);
		}
		listAddNodeTail(c->lock.keys, kt);
		return 1;
	} else {
		redisClient *locker = dictGetEntryVal(de);
		if (c == locker) {
			/* Silently allow locking an already locked key */
			return 1;
		}
		return 0;
	}
}

int releaseLockForKey(redisClient* c, robj* key) {
	dictEntry *de;
	listNode *ln;

	if (c->lock.keys == NULL) {
		/* If no keys are locked, return */
		return 0;
	} else {
		de = dictFind(c->db->locked_keys,key);
		redisAssert(de != NULL);
		redisClient *locker = dictGetEntryVal(de);
		if (locker == c) {
			/* We are the locker, so release the lock */
			dictDelete(c->db->locked_keys,key);
			ln = listSearchKey(c->lock.keys,key);
			redisAssert(ln != NULL);
			decrRefCount(listNodeValue(ln));
			listDelNode(c->lock.keys,ln);
			return 1;
		} else {
			/* This is not our lock */
			return 0;
		}
	}
}

void handOffLock(redisClient *c, robj *key) {
	struct dictEntry *de;
    redisClient *receiver;
    int numclients;
    list *clients;
    listNode *ln;

	de = dictFind(c->db->blocking_keys,key);
	if (de != NULL) {
		int retval;

		clients = dictGetEntryVal(de);
		numclients = listLength(clients);
		redisAssert(numclients != 0);

		ln = listFirst(clients);
		redisAssert(ln != NULL);
		receiver = listNodeValue(ln);
		/* Hand off the lock to the next client */
		retval = grabLockForKey(receiver,key);
		redisAssert(retval == 1);

		/* This should remove the first element of the "clients" list. */
		unblockClientWaitingData(receiver);
		redisAssert(ln != listFirst(clients));

		/* Tell the waiting client it is unblocked */
		addReply(receiver, shared.ok);
	} else {
		// No waiting clients, remove the string key
		if (dbDelete(c->db, key)) {
			touchWatchedKey(c->db,key);
			server.dirty++;
		}
	}
}
void releaseClientLocks(redisClient *c) {
	if (c->lock.keys) {
		listNode *ln;
		listIter *iter = listGetIterator(c->lock.keys, AL_START_HEAD);
		while ((ln = listNext(iter)) != NULL) {
			robj *key = dupStringObject(listNodeValue(ln));
			releaseLockForKey(c, key);
			handOffLock(c, key);
			decrRefCount(key);
		}
		listReleaseIterator(iter);
		listRelease(c->lock.keys);
	}
}

void grabCommand(redisClient *c) {
	robj *o;
	time_t timeout;
	robj *key;

	if (c->flags & REDIS_MULTI) {
		addReplyError(c,"GRAB inside MULTI is not allowed");
		return;
	}

	if (getTimeoutFromObjectOrReply(c, c->argv[2],&timeout) != REDIS_OK)
		return;

	key = c->argv[1];

	o = lookupKeyWrite(c->db,key);
	if (o != NULL) {
		if (o->type != REDIS_STRING) {
			addReply(c,shared.wrongtypeerr);
			return;
		}
	} else {
		// set a dummy value to enforce the STRING type
		robj *val = createStringObjectFromLongLong(0);

		if (dbAdd(c->db,key,val) == REDIS_ERR) {
			decrRefCount(val);
			addReplyError(c,"GRAB lock failed");
			return;
		}
	}
	if (grabLockForKey(c, key)==1) {
		addReply(c,shared.ok);
	} else {
		blockForKeys(c, c->argv + 1, 1, timeout, NULL);
	}
}

void releaseCommand(redisClient* c) {
	robj *key;

	if (c->flags & REDIS_MULTI) {
		addReplyError(c,"RELEASE inside MULTI is not allowed");
		return;
	}

	key = c->argv[1];

	if (releaseLockForKey(c,key)==1) {
		handOffLock(c, key);
		addReply(c,shared.ok);
	} else {
		addReplyError(c,"RELEASE failed! Key not Locked by us");
	}
}

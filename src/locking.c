#include "redis.h"


#include <assert.h>

dictType lockDictType = {
    dictIdentityHashFunction,   /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    NULL,						/* key compare */
    NULL,						/* key destructor */
    dictListDestructor			/* val destructor */
};

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
		list *l;

		kt = dupStringObject(key);
		/* Add the client structure to the locked_keys dict */
		retval = dictAdd(c->db->locked_keys,key,c);
		incrRefCount(key);
		redisAssert(retval == DICT_OK);

		if (c->lock.keys == NULL) {
			c->lock.keys = dictCreate(&lockDictType,NULL);
		}
		de = dictFind(c->lock.keys,(void *)(long)c->db->id);
		if (de == NULL) {
			l = listCreate();
			listSetMatchMethod(l,listMatchObjects);
			retval = dictAdd(c->lock.keys,(void *)(long)c->db->id,l);
			redisAssert(retval == DICT_OK);
		} else {
			l = dictGetEntryVal(de);
		}
		listAddNodeTail(l,kt);
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
	list *l;

	if (c->lock.keys == NULL) {
		/* If no keys are locked, return */
		return 0;
	} else {
		de = dictFind(c->db->locked_keys,key);
		if (de == NULL)
			return 0;

		redisClient *locker = dictGetEntryVal(de);
		if (locker == c) {
			de = dictFind(c->lock.keys,(void *)(long)c->db->id);
			if (de == NULL) {
				/* we don't have a lock in this DB */
				return 0;
			}
			l = dictGetEntryVal(de);
			redisAssert(l != NULL);
			/* We are the locker, so release the lock */
			dictDelete(c->db->locked_keys,key);
			ln = listSearchKey(l,key);
			redisAssert(ln != NULL);
			decrRefCount(listNodeValue(ln));
			listDelNode(l,ln);
			if (listLength(l) == 0)
				dictDelete(c->lock.keys,(void *)(long)c->db->id);
			if (dictSize(c->lock.keys) == 0) {
				dictRelease(c->lock.keys);
				c->lock.keys = NULL;
			}
			return 1;
		} else {
			/* This is not our lock */
			return 0;
		}
	}
}

void handOffLock(redisClient *c, robj *key) {
	struct dictEntry *de;
	int retval;
    redisClient *receiver;
    list *clients;
	listIter *iter;
    listNode *ln;

	de = dictFind(c->db->blocking_keys,key);
	if (de != NULL) {

		clients = dictGetEntryVal(de);

		iter = listGetIterator(clients, AL_START_HEAD);
        while ((ln = listNext(iter)) != NULL) {
			receiver = listNodeValue(ln);
			if (receiver->block.type != REDIS_BLOCK_LOCK)
				continue;

			/* Hand off the lock to the next client */
			retval = grabLockForKey(receiver,key);
			redisAssert(retval == 1);

			unblockClientWaitingData(receiver, ln);

			/* Tell the waiting client it is unblocked */
			addReply(receiver, shared.ok);
			break;
		}
		listReleaseIterator(iter);
	}
}
void releaseClientLocks(redisClient *c) {
	if (c->lock.keys) {
		dictEntry *de;
		dictIterator *iter;
		int origid = c->db->id;

		/* Iterate all DBs that have locks */
		iter = dictGetIterator(c->lock.keys);
		while ((de = dictNext(iter)) != NULL) {
			listNode *ln;
			list *l = dictGetEntryVal(de);
			int dbnum = (long)dictGetEntryKey(de);

			/* Switch the current DB */
			selectDb(c,dbnum);
			/* Iterate through all locks in THIS db and release them */
			listIter *liter = listGetIterator(l, AL_START_HEAD);
			while ((ln = listNext(liter)) != NULL) {
				robj *key = listNodeValue(ln);
				incrRefCount(key);
				releaseLockForKey(c, key);
				handOffLock(c, key);
				decrRefCount(key);
			}
			listReleaseIterator(liter);
		}
		dictReleaseIterator(iter);
		selectDb(c,origid);
		/* The last lock release should clean this up */
		redisAssert(c->lock.keys == NULL);
	}
}

void grabCommand(redisClient *c) {
	time_t timeout;
	robj *key;

	if (c->flags & REDIS_MULTI) {
		addReplyError(c,"GRAB inside MULTI is not allowed");
		return;
	}

	if (getTimeoutFromObjectOrReply(c, c->argv[2],&timeout) != REDIS_OK)
		return;

	key = c->argv[1];

	if (grabLockForKey(c, key)==1) {
		addReply(c,shared.ok);
	} else {
		blockForKeys(c, c->argv + 1, 1, timeout, NULL, REDIS_BLOCK_LOCK);
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

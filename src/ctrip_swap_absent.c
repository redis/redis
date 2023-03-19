/* Copyright (c) 2023, ctrip.com * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "ctrip_swap.h"

/* extent list api so that list node re-allocate can be avoided. */
void listUnlink(list *list, listNode *node) {
    if (node->prev)
        node->prev->next = node->next;
    else
        list->head = node->next;
    if (node->next)
        node->next->prev = node->prev;
    else
        list->tail = node->prev;
    list->len--;
}

void listLinkHead(list *list, listNode *node) {
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        node->prev = NULL;
        node->next = list->head;
        list->head->prev = node;
        list->head = node;
    }
    list->len++;
}

/* absents cache is specilized version of lru cache which holds most
 * recently accessed keys that definitely not exists in rocksdb. */
dictType absentsCacheDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    dictSdsDestructor,         /* key destructor */
    NULL,                      /* val destructor */
    NULL                       /* allow to expand */
};

absentsCache *absentsCacheNew(size_t capacity) {
    absentsCache *cache = zcalloc(sizeof(absentsCache));
    cache->capacity = capacity;
    cache->map = dictCreate(&absentsCacheDictType,NULL);
    cache->list = listCreate();
    return cache;
}

void absentsCacheFree(absentsCache *cache) {
   if (cache == NULL) return;
   dictRelease(cache->map);
   cache->map = NULL;
   listRelease(cache->list);
   cache->list = NULL;
   zfree(cache);
}

static void absentsCacheTrim(absentsCache *cache) {
    while (listLength(cache->list) > cache->capacity) {
        listNode *ln = listLast(cache->list);
        serverAssert(dictDelete(cache->map,listNodeValue(ln)) == DICT_OK);
        listDelNode(cache->list, ln);
    }
}

int absentsCachePut(absentsCache *cache, sds key) {
    dictEntry *de;
    listNode *ln;

    if ((de = dictFind(cache->map,key))) {
        ln = dictGetVal(de);
        listUnlink(cache->list,ln);
        listLinkHead(cache->list,ln);
        return 0;
    } else {
        listAddNodeHead(cache->list,key);
        dictAdd(cache->map,sdsdup(key),listFirst(cache->list));
        absentsCacheTrim(cache);
        return 1;
    }
}

int absentsCacheDelete(absentsCache *cache, sds key) {
    dictEntry *de;
    listNode *ln;

    if ((de = dictUnlink(cache->map,key))) {
        ln = dictGetVal(de);
        listDelNode(cache->list,ln);
        dictFreeUnlinkedEntry(cache->map,de);
        return 1;
    } else {
        return 0;
    }
}

int absentsCacheGet(absentsCache *cache, sds key) {
    dictEntry *de;
    listNode *ln;

    if ((de = dictFind(cache->map,key))) {
        ln = dictGetVal(de);
        listUnlink(cache->list,ln);
        listLinkHead(cache->list,ln);
        return 1;
    } else {
        return 0;
    }
}

void absentsCacheSetCapacity(absentsCache *cache, size_t capacity) {
    cache->capacity = capacity;
    absentsCacheTrim(cache);
}

#ifdef REDIS_TEST

static int absentsCacheExists(absentsCache *cache, sds key) {
    return dictFind(cache->map,key) != NULL;
}

int swapAbsentTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);
    int error = 0;

    TEST("absent: list link & unlink") {
        listNode *ln;
        list *l = listCreate();

        listAddNodeHead(l, (void*)1);
        ln = listFirst(l);
        listUnlink(l,ln);
        test_assert(listLength(l) == 0 && listFirst(l) == NULL && listLast(l) == NULL);
        listLinkHead(l,ln);
        test_assert(listLength(l) == 1 && listFirst(l) == ln && listLast(l) == ln);

        listAddNodeTail(l, (void*)2);
        listAddNodeTail(l, (void*)3);
        ln = listSearchKey(l, (void*)2);
        listUnlink(l,ln);
        test_assert(listLength(l) == 2);
        listLinkHead(l,ln);
        test_assert(listLength(l) == 3);
        ln = listFirst(l);
        test_assert(listNodeValue(ln) == (void*)2);
        ln = ln->next;
        test_assert(listNodeValue(ln) == (void*)1);
        ln = ln->next;
        test_assert(listNodeValue(ln) == (void*)3);
        test_assert(listLast(l) == ln);

        listRelease(l);
    }

    TEST("absent: lru cache") {
        sds first = sdsnew("1"), second = sdsnew("2"), third = sdsnew("3"), fourth = sdsnew("4");
        absentsCache *cache;

        cache = absentsCacheNew(1);
        test_assert(!absentsCacheExists(cache,first));
        absentsCachePut(cache,first);
        test_assert(absentsCacheExists(cache,first));
        absentsCachePut(cache,second);
        test_assert(!absentsCacheExists(cache,first));
        absentsCacheFree(cache);

        cache = absentsCacheNew(3);
        absentsCachePut(cache,first);
        absentsCachePut(cache,second);
        absentsCachePut(cache,third);
        absentsCachePut(cache,fourth);
        test_assert(!absentsCacheExists(cache,first));
        test_assert(absentsCacheExists(cache,second));
        test_assert(absentsCacheExists(cache,third));
        test_assert(absentsCacheExists(cache,fourth));
        absentsCachePut(cache,first);
        test_assert(absentsCacheExists(cache,first));
        test_assert(!absentsCacheExists(cache,second));
        test_assert(absentsCacheExists(cache,third));
        test_assert(absentsCacheExists(cache,fourth));

        absentsCacheDelete(cache,second);
        test_assert(!absentsCacheExists(cache,second));

        test_assert(absentsCacheGet(cache,second) == 0);
        test_assert(absentsCacheGet(cache,third) == 1);
        test_assert(absentsCacheGet(cache,first) == 1);

        absentsCacheSetCapacity(cache, 1);
        test_assert(cache->capacity == 1);
        test_assert(absentsCacheExists(cache,first));
        test_assert(!absentsCacheExists(cache,fourth));
        absentsCacheFree(cache);

        sdsfree(first), sdsfree(second), sdsfree(third), sdsfree(fourth);
    }

    return error;
}


#endif

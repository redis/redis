/*
 * Copyright (c), Microsoft Open Technologies, Inc.
 * All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef _WIN32
/************************************************************************
 * This module defines copy on write to support
 * saving on a background thread in Windows.
 ************************************************************************/

/* collections are converted to read only arrays */
typedef struct cowListArray {
    size_t numele;
    listNode le[];
} cowListArray;

typedef struct cowDictArray {
    size_t numele;
    dictEntry de[];
} cowDictArray;

typedef struct dictZEntry {
    dictEntry de;
    double score;
} dictZEntry;

typedef struct cowDictZArray {
    size_t numele;
    dictZEntry zde[];
} cowDictZArray;

/* Special read only iterator for dictionary can iterate over
 * regular dictionary encoding or array of entries.
 * Used only for background save with in process copy on write.
 * If the hash needs to be copied, it is converted to a readonly array */
typedef struct roDictIter {
    cowDictArray *ar;
    dict *hdict;
    dictIterator *di;
    size_t pos;
} roDictIter;

/* Special read only iterator for zset hash dictionary can iterate over
 * regular hash table encoding or array of entries.
 * Used only for background save copy on write.
 * If the hash needs to be copied, it is converted to a readonly array */
typedef struct roZDictIter {
    cowDictZArray *ar;
    dict *hdict;
    dictIterator *di;
    size_t pos;
} roZDictIter;

/* Special read only iterator for list can iterate over
 * regular list encoding or array of entries.
 * Used only for background save with in process copy on write.
 * If the list needs to be copied, it is converted to a readonly array */
typedef struct roListIter {
    cowListArray *ar;
    list *olist;
    listIter li;
    size_t pos;
} roListIter;

/* Special read only iterator for hash can iterate over
 * regular hash table encoding or array of entries.
 * Used only for background save with in process copy on write.
 * If the hash needs to be copied, it is converted to a readonly array */
typedef struct roHashIter {
    cowDictArray *ar;
    dict *hdict;
    void *di;       /* using void* because hashTypeIterator defined later */
    int pos;
} roHashIter;


/* current iterators in use.
 * If the current object is converted to an array
 * then the current iterator must be converted as well */
typedef struct bkgdIters {
    roDictIter *curDbDictIter;
    roDictIter *curObjDictIter;
    roZDictIter *curObjZDictIter;
    roListIter *curObjListIter;
    roHashIter *curObjHashIter;
    CRITICAL_SECTION csMigrate;
} bkgditers;


/* structure for top level DB dictionary extensions
   used to change and restore destructor type,
   and to track read only array snapshot */
typedef struct bkgdDbExt {
    dictType *savedType;
    dictType *cowType;
    dictType *readonlyType;
    cowDictArray *dictArray;
    int id;
} bkgdDbExt;

/* wincow functions */
void cowInit();
void cowBkgdSaveStart();
void cowBkgdSaveStop();
void cowLock();
void cowUnlock();
int deferFreeObject(void *obj);
size_t roDBDictSize(int id);
roDictIter *roDBGetIterator(int id);
roDictIter *roDictGetIterator(dict *d, cowDictArray *ro);
dictEntry *roDictNext(roDictIter *iter);
void roDictReleaseIterator(roDictIter *iter);
roZDictIter *roZDictGetIterator(dict *d, cowDictZArray *ro);
dictEntry *roZDictNext(roZDictIter *iter);
void roZDictReleaseIterator(roZDictIter *iter);
roListIter *roListGetIterator(list *l, cowListArray *ro);
void roListRewind(list *l, cowListArray *ro, roListIter *iter);
listNode *roListNext(roListIter *iter);
void roListReleaseIterator(roListIter *iter);

roHashIter *roHashGetIterator(void *subject, cowDictArray *ro);
int roHashNext(roHashIter *iter);
int roHashGetEncoding(roHashIter *iter);
void *roHashGetHashIter(roHashIter *iter);
void roHashGetCurrentFromArray(roHashIter *iter, int what, void **dst);
void roHashReleaseIterator(roHashIter *iter);

void *getRoConvertedObj(void *key, void *o);
void cowReleaseListArray(cowListArray *ar);
void cowReleaseDictArray(cowDictArray *ar);
void cowReleaseDictZArray(cowDictZArray *ar);


/* redis.c functions used in wincow */
int dictEncObjKeyCompare(void *privdata, const void *key1, const void *key2);
unsigned int dictEncObjHash(const void *key);

#else
/* define read only iterator types and methods as normal iterator types and methods */
#define roDictIter             dictIterator
#define roZDictIter            dictIterator
#define roListIter             listIter
#define roDictGetIterator(a,b) dictGetIterator((a))
#define roZDictGetIterator(a,b) dictGetIterator((a))
#define roListRewind(a,b,c)    listRewind((a),(c))
#define roDictNext             dictNext
#define roZDictNext            dictNext
#define roListNext             listNext
#define roDictReleaseIterator  dictReleaseIterator
#define roZDictReleaseIterator dictReleaseIterator
#define cowLock()
#define cowUnlock()
#endif

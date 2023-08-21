/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
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

#include "server.h"
#include "intset.h"  /* Compact integer set structure */

/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/

void sunionDiffGenericCommand(client *c, robj **setkeys, int setnum,
                              robj *dstkey, int op);

/* Factory method to return a set that *can* hold "value". When the object has
 * an integer-encodable value, an intset will be returned. Otherwise a listpack
 * or a regular hash table.
 *
 * The size hint indicates approximately how many items will be added which is
 * used to determine the initial representation. */
robj *setTypeCreate(sds value, size_t size_hint) {
    if (isSdsRepresentableAsLongLong(value,NULL) == C_OK && size_hint <= server.set_max_intset_entries)
        return createIntsetObject();
    if (size_hint <= server.set_max_listpack_entries)
        return createSetListpackObject();

    /* We may oversize the set by using the hint if the hint is not accurate,
     * but we will assume this is acceptable to maximize performance. */
    robj *o = createSetObject();
    dictExpand(o->ptr, size_hint);
    return o;
}

/* Check if the existing set should be converted to another encoding based off the
 * the size hint. */
void setTypeMaybeConvert(robj *set, size_t size_hint) {
    if ((set->encoding == OBJ_ENCODING_LISTPACK && size_hint > server.set_max_listpack_entries)
        || (set->encoding == OBJ_ENCODING_INTSET && size_hint > server.set_max_intset_entries))
    {
        setTypeConvertAndExpand(set, OBJ_ENCODING_HT, size_hint, 1);
    }
}

/* Return the maximum number of entries to store in an intset. */
static size_t intsetMaxEntries(void) {
    size_t max_entries = server.set_max_intset_entries;
    /* limit to 1G entries due to intset internals. */
    if (max_entries >= 1<<30) max_entries = 1<<30;
    return max_entries;
}

/* Converts intset to HT if it contains too many entries. */
static void maybeConvertIntset(robj *subject) {
    serverAssert(subject->encoding == OBJ_ENCODING_INTSET);
    if (intsetLen(subject->ptr) > intsetMaxEntries())
        setTypeConvert(subject,OBJ_ENCODING_HT);
}

/* When you know all set elements are integers, call this to convert the set to
 * an intset. No conversion happens if the set contains too many entries for an
 * intset. */
static void maybeConvertToIntset(robj *set) {
    if (set->encoding == OBJ_ENCODING_INTSET) return; /* already intset */
    if (setTypeSize(set) > intsetMaxEntries()) return; /* can't use intset */
    intset *is = intsetNew();
    char *str;
    size_t len;
    int64_t llval;
    setTypeIterator *si = setTypeInitIterator(set);
    while (setTypeNext(si, &str, &len, &llval) != -1) {
        if (str) {
            /* If the element is returned as a string, we may be able to convert
             * it to integer. This happens for OBJ_ENCODING_HT. */
            serverAssert(string2ll(str, len, (long long *)&llval));
        }
        uint8_t success = 0;
        is = intsetAdd(is, llval, &success);
        serverAssert(success);
    }
    setTypeReleaseIterator(si);
    freeSetObject(set); /* frees the internals but not robj itself */
    set->ptr = is;
    set->encoding = OBJ_ENCODING_INTSET;
}

/* Add the specified sds value into a set.
 *
 * If the value was already member of the set, nothing is done and 0 is
 * returned, otherwise the new element is added and 1 is returned. */
int setTypeAdd(robj *subject, sds value) {
    return setTypeAddAux(subject, value, sdslen(value), 0, 1);
}

/* Add member. This function is optimized for the different encodings. The
 * value can be provided as an sds string (indicated by passing str_is_sds =
 * 1), as string and length (str_is_sds = 0) or as an integer in which case str
 * is set to NULL and llval is provided instead.
 *
 * Returns 1 if the value was added and 0 if it was already a member. */
int setTypeAddAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        if (set->encoding == OBJ_ENCODING_INTSET) {
            uint8_t success = 0;
            set->ptr = intsetAdd(set->ptr, llval, &success);
            if (success) maybeConvertIntset(set);
            return success;
        }
        /* Convert int to string. */
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    serverAssert(str);
    if (set->encoding == OBJ_ENCODING_HT) {
        /* Avoid duping the string if it is an sds string. */
        sds sdsval = str_is_sds ? (sds)str : sdsnewlen(str, len);
        dict *ht = set->ptr;
        void *position = dictFindPositionForInsert(ht, sdsval, NULL);
        if (position) {
            /* Key doesn't already exist in the set. Add it but dup the key. */
            if (sdsval == str) sdsval = sdsdup(sdsval);
            dictInsertAtPosition(ht, sdsval, position);
        } else if (sdsval != str) {
            /* String is already a member. Free our temporary sds copy. */
            sdsfree(sdsval);
        }
        return (position != NULL);
    } else if (set->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        if (p != NULL)
            p = lpFind(lp, p, (unsigned char*)str, len, 0);
        if (p == NULL) {
            /* Not found.  */
            if (lpLength(lp) < server.set_max_listpack_entries &&
                len <= server.set_max_listpack_value &&
                lpSafeToAdd(lp, len))
            {
                if (str == tmpbuf) {
                    /* This came in as integer so we can avoid parsing it again.
                     * TODO: Create and use lpFindInteger; don't go via string. */
                    lp = lpAppendInteger(lp, llval);
                } else {
                    lp = lpAppend(lp, (unsigned char*)str, len);
                }
                set->ptr = lp;
            } else {
                /* Size limit is reached. Convert to hashtable and add. */
                setTypeConvertAndExpand(set, OBJ_ENCODING_HT, lpLength(lp) + 1, 1);
                serverAssert(dictAdd(set->ptr,sdsnewlen(str,len),NULL) == DICT_OK);
            }
            return 1;
        }
    } else if (set->encoding == OBJ_ENCODING_INTSET) {
        long long value;
        if (string2ll(str, len, &value)) {
            uint8_t success = 0;
            set->ptr = intsetAdd(set->ptr,value,&success);
            if (success) {
                maybeConvertIntset(set);
                return 1;
            }
        } else {
            /* Check if listpack encoding is safe not to cross any threshold. */
            size_t maxelelen = 0, totsize = 0;
            unsigned long n = intsetLen(set->ptr);
            if (n != 0) {
                size_t elelen1 = sdigits10(intsetMax(set->ptr));
                size_t elelen2 = sdigits10(intsetMin(set->ptr));
                maxelelen = max(elelen1, elelen2);
                size_t s1 = lpEstimateBytesRepeatedInteger(intsetMax(set->ptr), n);
                size_t s2 = lpEstimateBytesRepeatedInteger(intsetMin(set->ptr), n);
                totsize = max(s1, s2);
            }
            if (intsetLen((const intset*)set->ptr) < server.set_max_listpack_entries &&
                len <= server.set_max_listpack_value &&
                maxelelen <= server.set_max_listpack_value &&
                lpSafeToAdd(NULL, totsize + len))
            {
                /* In the "safe to add" check above we assumed all elements in
                 * the intset are of size maxelelen. This is an upper bound. */
                setTypeConvertAndExpand(set, OBJ_ENCODING_LISTPACK,
                                        intsetLen(set->ptr) + 1, 1);
                unsigned char *lp = set->ptr;
                lp = lpAppend(lp, (unsigned char *)str, len);
                lp = lpShrinkToFit(lp);
                set->ptr = lp;
                return 1;
            } else {
                setTypeConvertAndExpand(set, OBJ_ENCODING_HT,
                                        intsetLen(set->ptr) + 1, 1);
                /* The set *was* an intset and this value is not integer
                 * encodable, so dictAdd should always work. */
                serverAssert(dictAdd(set->ptr,sdsnewlen(str,len),NULL) == DICT_OK);
                return 1;
            }
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

/* Deletes a value provided as an sds string from the set. Returns 1 if the
 * value was deleted and 0 if it was not a member of the set. */
int setTypeRemove(robj *setobj, sds value) {
    return setTypeRemoveAux(setobj, value, sdslen(value), 0, 1);
}

/* Remove a member. This function is optimized for the different encodings. The
 * value can be provided as an sds string (indicated by passing str_is_sds =
 * 1), as string and length (str_is_sds = 0) or as an integer in which case str
 * is set to NULL and llval is provided instead.
 *
 * Returns 1 if the value was deleted and 0 if it was not a member of the set. */
int setTypeRemoveAux(robj *setobj, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        if (setobj->encoding == OBJ_ENCODING_INTSET) {
            int success;
            setobj->ptr = intsetRemove(setobj->ptr,llval,&success);
            return success;
        }
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    if (setobj->encoding == OBJ_ENCODING_HT) {
        sds sdsval = str_is_sds ? (sds)str : sdsnewlen(str, len);
        int deleted = (dictDelete(setobj->ptr, sdsval) == DICT_OK);
        if (deleted && htNeedsResize(setobj->ptr)) dictResize(setobj->ptr);
        if (sdsval != str) sdsfree(sdsval); /* free temp copy */
        return deleted;
    } else if (setobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = setobj->ptr;
        unsigned char *p = lpFirst(lp);
        if (p == NULL) return 0;
        p = lpFind(lp, p, (unsigned char*)str, len, 0);
        if (p != NULL) {
            lp = lpDelete(lp, p, NULL);
            setobj->ptr = lp;
            return 1;
        }
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {
        long long llval;
        if (string2ll(str, len, &llval)) {
            int success;
            setobj->ptr = intsetRemove(setobj->ptr,llval,&success);
            if (success) return 1;
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

/* Check if an sds string is a member of the set. Returns 1 if the value is a
 * member of the set and 0 if it isn't. */
int setTypeIsMember(robj *subject, sds value) {
    return setTypeIsMemberAux(subject, value, sdslen(value), 0, 1);
}

/* Membership checking optimized for the different encodings. The value can be
 * provided as an sds string (indicated by passing str_is_sds = 1), as string
 * and length (str_is_sds = 0) or as an integer in which case str is set to NULL
 * and llval is provided instead.
 *
 * Returns 1 if the value is a member of the set and 0 if it isn't. */
int setTypeIsMemberAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        if (set->encoding == OBJ_ENCODING_INTSET)
            return intsetFind(set->ptr, llval);
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    if (set->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        return p && lpFind(lp, p, (unsigned char*)str, len, 0);
    } else if (set->encoding == OBJ_ENCODING_INTSET) {
        long long llval;
        return string2ll(str, len, &llval) && intsetFind(set->ptr, llval);
    } else if (set->encoding == OBJ_ENCODING_HT && str_is_sds) {
        return dictFind(set->ptr, (sds)str) != NULL;
    } else if (set->encoding == OBJ_ENCODING_HT) {
        sds sdsval = sdsnewlen(str, len);
        int result = dictFind(set->ptr, sdsval) != NULL;
        sdsfree(sdsval);
        return result;
    } else {
        serverPanic("Unknown set encoding");
    }
}

setTypeIterator *setTypeInitIterator(robj *subject) {
    setTypeIterator *si = zmalloc(sizeof(setTypeIterator));
    si->subject = subject;
    si->encoding = subject->encoding;
    if (si->encoding == OBJ_ENCODING_HT) {
        si->di = dictGetIterator(subject->ptr);
    } else if (si->encoding == OBJ_ENCODING_INTSET) {
        si->ii = 0;
    } else if (si->encoding == OBJ_ENCODING_LISTPACK) {
        si->lpi = NULL;
    } else {
        serverPanic("Unknown set encoding");
    }
    return si;
}

void setTypeReleaseIterator(setTypeIterator *si) {
    if (si->encoding == OBJ_ENCODING_HT)
        dictReleaseIterator(si->di);
    zfree(si);
}

/* Move to the next entry in the set. Returns the object at the current
 * position, as a string or as an integer.
 *
 * Since set elements can be internally be stored as SDS strings, char buffers or
 * simple arrays of integers, setTypeNext returns the encoding of the
 * set object you are iterating, and will populate the appropriate pointers
 * (str and len) or (llele) depending on whether the value is stored as a string
 * or as an integer internally.
 *
 * If OBJ_ENCODING_HT is returned, then str points to an sds string and can be
 * used as such. If OBJ_ENCODING_INTSET, then llele is populated and str is
 * pointed to NULL. If OBJ_ENCODING_LISTPACK is returned, the value can be
 * either a string or an integer. If *str is not NULL, then str and len are
 * populated with the string content and length. Otherwise, llele populated with
 * an integer value.
 *
 * Note that str, len and llele pointers should all be passed and cannot
 * be NULL since the function will try to defensively populate the non
 * used field with values which are easy to trap if misused.
 *
 * When there are no more elements -1 is returned. */
int setTypeNext(setTypeIterator *si, char **str, size_t *len, int64_t *llele) {
    if (si->encoding == OBJ_ENCODING_HT) {
        dictEntry *de = dictNext(si->di);
        if (de == NULL) return -1;
        *str = dictGetKey(de);
        *len = sdslen(*str);
        *llele = -123456789; /* Not needed. Defensive. */
    } else if (si->encoding == OBJ_ENCODING_INTSET) {
        if (!intsetGet(si->subject->ptr,si->ii++,llele))
            return -1;
        *str = NULL;
    } else if (si->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = si->subject->ptr;
        unsigned char *lpi = si->lpi;
        if (lpi == NULL) {
            lpi = lpFirst(lp);
        } else {
            lpi = lpNext(lp, lpi);
        }
        if (lpi == NULL) return -1;
        si->lpi = lpi;
        unsigned int l;
        *str = (char *)lpGetValue(lpi, &l, (long long *)llele);
        *len = (size_t)l;
    } else {
        serverPanic("Wrong set encoding in setTypeNext");
    }
    return si->encoding;
}

/* The not copy on write friendly version but easy to use version
 * of setTypeNext() is setTypeNextObject(), returning new SDS
 * strings. So if you don't retain a pointer to this object you should call
 * sdsfree() against it.
 *
 * This function is the way to go for write operations where COW is not
 * an issue. */
sds setTypeNextObject(setTypeIterator *si) {
    int64_t intele;
    char *str;
    size_t len;

    if (setTypeNext(si, &str, &len, &intele) == -1) return NULL;
    if (str != NULL) return sdsnewlen(str, len);
    return sdsfromlonglong(intele);
}

/* Return random element from a non empty set.
 * The returned element can be an int64_t value if the set is encoded
 * as an "intset" blob of integers, or an string.
 *
 * The caller provides three pointers to be populated with the right
 * object. The return value of the function is the object->encoding
 * field of the object and can be used by the caller to check if the
 * int64_t pointer or the str and len pointers were populated, as for
 * setTypeNext. If OBJ_ENCODING_HT is returned, str is pointed to a
 * string which is actually an sds string and it can be used as such.
 *
 * Note that both the str, len and llele pointers should be passed and cannot
 * be NULL. If str is set to NULL, the value is an integer stored in llele. */
int setTypeRandomElement(robj *setobj, char **str, size_t *len, int64_t *llele) {
    if (setobj->encoding == OBJ_ENCODING_HT) {
        dictEntry *de = dictGetFairRandomKey(setobj->ptr);
        *str = dictGetKey(de);
        *len = sdslen(*str);
        *llele = -123456789; /* Not needed. Defensive. */
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {
        *llele = intsetRandom(setobj->ptr);
        *str = NULL; /* Not needed. Defensive. */
    } else if (setobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = setobj->ptr;
        int r = rand() % lpLength(lp);
        unsigned char *p = lpSeek(lp, r);
        unsigned int l;
        *str = (char *)lpGetValue(p, &l, (long long *)llele);
        *len = (size_t)l;
    } else {
        serverPanic("Unknown set encoding");
    }
    return setobj->encoding;
}

/* Pops a random element and returns it as an object. */
robj *setTypePopRandom(robj *set) {
    robj *obj;
    if (set->encoding == OBJ_ENCODING_LISTPACK) {
        /* Find random and delete it without re-seeking the listpack. */
        unsigned int i = 0;
        unsigned char *p = lpNextRandom(set->ptr, lpFirst(set->ptr), &i, 1, 0);
        unsigned int len = 0; /* initialize to silence warning */
        long long llele = 0; /* initialize to silence warning */
        char *str = (char *)lpGetValue(p, &len, &llele);
        if (str)
            obj = createStringObject(str, len);
        else
            obj = createStringObjectFromLongLong(llele);
        set->ptr = lpDelete(set->ptr, p, NULL);
    } else {
        char *str;
        size_t len = 0;
        int64_t llele = 0;
        int encoding = setTypeRandomElement(set, &str, &len, &llele);
        if (str)
            obj = createStringObject(str, len);
        else
            obj = createStringObjectFromLongLong(llele);
        setTypeRemoveAux(set, str, len, llele, encoding == OBJ_ENCODING_HT);
    }
    return obj;
}

unsigned long setTypeSize(const robj *subject) {
    if (subject->encoding == OBJ_ENCODING_HT) {
        return dictSize((const dict*)subject->ptr);
    } else if (subject->encoding == OBJ_ENCODING_INTSET) {
        return intsetLen((const intset*)subject->ptr);
    } else if (subject->encoding == OBJ_ENCODING_LISTPACK) {
        return lpLength((unsigned char *)subject->ptr);
    } else {
        serverPanic("Unknown set encoding");
    }
}

/* Convert the set to specified encoding. The resulting dict (when converting
 * to a hash table) is presized to hold the number of elements in the original
 * set. */
void setTypeConvert(robj *setobj, int enc) {
    setTypeConvertAndExpand(setobj, enc, setTypeSize(setobj), 1);
}

/* Converts a set to the specified encoding, pre-sizing it for 'cap' elements.
 * The 'panic' argument controls whether to panic on OOM (panic=1) or return
 * C_ERR on OOM (panic=0). If panic=1 is given, this function always returns
 * C_OK. */
int setTypeConvertAndExpand(robj *setobj, int enc, unsigned long cap, int panic) {
    setTypeIterator *si;
    serverAssertWithInfo(NULL,setobj,setobj->type == OBJ_SET &&
                             setobj->encoding != enc);

    if (enc == OBJ_ENCODING_HT) {
        dict *d = dictCreate(&setDictType);
        sds element;

        /* Presize the dict to avoid rehashing */
        if (panic) {
            dictExpand(d, cap);
        } else if (dictTryExpand(d, cap) != DICT_OK) {
            dictRelease(d);
            return C_ERR;
        }

        /* To add the elements we extract integers and create redis objects */
        si = setTypeInitIterator(setobj);
        while ((element = setTypeNextObject(si)) != NULL) {
            serverAssert(dictAdd(d,element,NULL) == DICT_OK);
        }
        setTypeReleaseIterator(si);

        freeSetObject(setobj); /* frees the internals but not setobj itself */
        setobj->encoding = OBJ_ENCODING_HT;
        setobj->ptr = d;
    } else if (enc == OBJ_ENCODING_LISTPACK) {
        /* Preallocate the minimum two bytes per element (enc/value + backlen) */
        size_t estcap = cap * 2;
        if (setobj->encoding == OBJ_ENCODING_INTSET && setTypeSize(setobj) > 0) {
            /* If we're converting from intset, we have a better estimate. */
            size_t s1 = lpEstimateBytesRepeatedInteger(intsetMin(setobj->ptr), cap);
            size_t s2 = lpEstimateBytesRepeatedInteger(intsetMax(setobj->ptr), cap);
            estcap = max(s1, s2);
        }
        unsigned char *lp = lpNew(estcap);
        char *str;
        size_t len;
        int64_t llele;
        si = setTypeInitIterator(setobj);
        while (setTypeNext(si, &str, &len, &llele) != -1) {
            if (str != NULL)
                lp = lpAppend(lp, (unsigned char *)str, len);
            else
                lp = lpAppendInteger(lp, llele);
        }
        setTypeReleaseIterator(si);

        freeSetObject(setobj); /* frees the internals but not setobj itself */
        setobj->encoding = OBJ_ENCODING_LISTPACK;
        setobj->ptr = lp;
    } else {
        serverPanic("Unsupported set conversion");
    }
    return C_OK;
}

/* This is a helper function for the COPY command.
 * Duplicate a set object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *setTypeDup(robj *o) {
    robj *set;
    setTypeIterator *si;

    serverAssert(o->type == OBJ_SET);

    /* Create a new set object that have the same encoding as the original object's encoding */
    if (o->encoding == OBJ_ENCODING_INTSET) {
        intset *is = o->ptr;
        size_t size = intsetBlobLen(is);
        intset *newis = zmalloc(size);
        memcpy(newis,is,size);
        set = createObject(OBJ_SET, newis);
        set->encoding = OBJ_ENCODING_INTSET;
    } else if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = o->ptr;
        size_t sz = lpBytes(lp);
        unsigned char *new_lp = zmalloc(sz);
        memcpy(new_lp, lp, sz);
        set = createObject(OBJ_SET, new_lp);
        set->encoding = OBJ_ENCODING_LISTPACK;
    } else if (o->encoding == OBJ_ENCODING_HT) {
        set = createSetObject();
        dict *d = o->ptr;
        dictExpand(set->ptr, dictSize(d));
        si = setTypeInitIterator(o);
        char *str;
        size_t len;
        int64_t intobj;
        while (setTypeNext(si, &str, &len, &intobj) != -1) {
            setTypeAdd(set, (sds)str);
        }
        setTypeReleaseIterator(si);
    } else {
        serverPanic("Unknown set encoding");
    }
    return set;
}

void saddCommand(client *c) {
    robj *set;
    int j, added = 0;

    set = lookupKeyWrite(c->db,c->argv[1]);
    if (checkType(c,set,OBJ_SET)) return;
    
    if (set == NULL) {
        set = setTypeCreate(c->argv[2]->ptr, c->argc - 2);
        dbAdd(c->db,c->argv[1],set);
    } else {
        setTypeMaybeConvert(set, c->argc - 2);
    }

    for (j = 2; j < c->argc; j++) {
        if (setTypeAdd(set,c->argv[j]->ptr)) added++;
    }
    if (added) {
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_SET,"sadd",c->argv[1],c->db->id);
    }
    server.dirty += added;
    addReplyLongLong(c,added);
}

void sremCommand(client *c) {
    robj *set;
    int j, deleted = 0, keyremoved = 0;

    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,OBJ_SET)) return;

    for (j = 2; j < c->argc; j++) {
        if (setTypeRemove(set,c->argv[j]->ptr)) {
            deleted++;
            if (setTypeSize(set) == 0) {
                dbDelete(c->db,c->argv[1]);
                keyremoved = 1;
                break;
            }
        }
    }
    if (deleted) {
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_SET,"srem",c->argv[1],c->db->id);
        if (keyremoved)
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],
                                c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c,deleted);
}

void smoveCommand(client *c) {
    robj *srcset, *dstset, *ele;
    srcset = lookupKeyWrite(c->db,c->argv[1]);
    dstset = lookupKeyWrite(c->db,c->argv[2]);
    ele = c->argv[3];

    /* If the source key does not exist return 0 */
    if (srcset == NULL) {
        addReply(c,shared.czero);
        return;
    }

    /* If the source key has the wrong type, or the destination key
     * is set and has the wrong type, return with an error. */
    if (checkType(c,srcset,OBJ_SET) ||
        checkType(c,dstset,OBJ_SET)) return;

    /* If srcset and dstset are equal, SMOVE is a no-op */
    if (srcset == dstset) {
        addReply(c,setTypeIsMember(srcset,ele->ptr) ?
            shared.cone : shared.czero);
        return;
    }

    /* If the element cannot be removed from the src set, return 0. */
    if (!setTypeRemove(srcset,ele->ptr)) {
        addReply(c,shared.czero);
        return;
    }
    notifyKeyspaceEvent(NOTIFY_SET,"srem",c->argv[1],c->db->id);

    /* Remove the src set from the database when empty */
    if (setTypeSize(srcset) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }

    /* Create the destination set when it doesn't exist */
    if (!dstset) {
        dstset = setTypeCreate(ele->ptr, 1);
        dbAdd(c->db,c->argv[2],dstset);
    }

    signalModifiedKey(c,c->db,c->argv[1]);
    server.dirty++;

    /* An extra key has changed when ele was successfully added to dstset */
    if (setTypeAdd(dstset,ele->ptr)) {
        server.dirty++;
        signalModifiedKey(c,c->db,c->argv[2]);
        notifyKeyspaceEvent(NOTIFY_SET,"sadd",c->argv[2],c->db->id);
    }
    addReply(c,shared.cone);
}

void sismemberCommand(client *c) {
    robj *set;

    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,OBJ_SET)) return;

    if (setTypeIsMember(set,c->argv[2]->ptr))
        addReply(c,shared.cone);
    else
        addReply(c,shared.czero);
}

void smismemberCommand(client *c) {
    robj *set;
    int j;

    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * sets, where SMISMEMBER should respond with a series of zeros. */
    set = lookupKeyRead(c->db,c->argv[1]);
    if (set && checkType(c,set,OBJ_SET)) return;

    addReplyArrayLen(c,c->argc - 2);

    for (j = 2; j < c->argc; j++) {
        if (set && setTypeIsMember(set,c->argv[j]->ptr))
            addReply(c,shared.cone);
        else
            addReply(c,shared.czero);
    }
}

void scardCommand(client *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_SET)) return;

    addReplyLongLong(c,setTypeSize(o));
}

/* Handle the "SPOP key <count>" variant. The normal version of the
 * command is handled by the spopCommand() function itself. */

/* How many times bigger should be the set compared to the remaining size
 * for us to use the "create new set" strategy? Read later in the
 * implementation for more info. */
#define SPOP_MOVE_STRATEGY_MUL 5

void spopWithCountCommand(client *c) {
    long l;
    unsigned long count, size;
    robj *set;

    /* Get the count argument */
    if (getPositiveLongFromObjectOrReply(c,c->argv[2],&l,NULL) != C_OK) return;
    count = (unsigned long) l;

    /* Make sure a key with the name inputted exists, and that it's type is
     * indeed a set. Otherwise, return nil */
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.emptyset[c->resp]))
        == NULL || checkType(c,set,OBJ_SET)) return;

    /* If count is zero, serve an empty set ASAP to avoid special
     * cases later. */
    if (count == 0) {
        addReply(c,shared.emptyset[c->resp]);
        return;
    }

    size = setTypeSize(set);

    /* Generate an SPOP keyspace notification */
    notifyKeyspaceEvent(NOTIFY_SET,"spop",c->argv[1],c->db->id);
    server.dirty += (count >= size) ? size : count;

    /* CASE 1:
     * The number of requested elements is greater than or equal to
     * the number of elements inside the set: simply return the whole set. */
    if (count >= size) {
        /* We just return the entire set */
        sunionDiffGenericCommand(c,c->argv+1,1,NULL,SET_OP_UNION);

        /* Delete the set as it is now empty */
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);

        /* todo: Move the spop notification to be executed after the command logic. */

        /* Propagate this command as a DEL or UNLINK operation */
        robj *aux = server.lazyfree_lazy_server_del ? shared.unlink : shared.del;
        rewriteClientCommandVector(c, 2, aux, c->argv[1]);
        signalModifiedKey(c,c->db,c->argv[1]);
        return;
    }

    /* Case 2 and 3 require to replicate SPOP as a set of SREM commands.
     * Prepare our replication argument vector. Also send the array length
     * which is common to both the code paths. */
    unsigned long batchsize = count > 1024 ? 1024 : count;
    robj **propargv = zmalloc(sizeof(robj *) * (2 + batchsize));
    propargv[0] = shared.srem;
    propargv[1] = c->argv[1];
    unsigned long propindex = 2;
    addReplySetLen(c,count);

    /* Common iteration vars. */
    char *str;
    size_t len;
    int64_t llele;
    unsigned long remaining = size-count; /* Elements left after SPOP. */

    /* If we are here, the number of requested elements is less than the
     * number of elements inside the set. Also we are sure that count < size.
     * Use two different strategies.
     *
     * CASE 2: The number of elements to return is small compared to the
     * set size. We can just extract random elements and return them to
     * the set. */
    if (remaining*SPOP_MOVE_STRATEGY_MUL > count &&
        set->encoding == OBJ_ENCODING_LISTPACK)
    {
        /* Specialized case for listpack. Traverse it only once. */
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        unsigned int index = 0;
        unsigned char **ps = zmalloc(sizeof(char *) * count);
        for (unsigned long i = 0; i < count; i++) {
            p = lpNextRandom(lp, p, &index, count - i, 0);
            unsigned int len;
            str = (char *)lpGetValue(p, &len, (long long *)&llele);

            if (str) {
                addReplyBulkCBuffer(c, str, len);
                propargv[propindex++] = createStringObject(str, len);
            } else {
                addReplyBulkLongLong(c, llele);
                propargv[propindex++] = createStringObjectFromLongLong(llele);
            }
            /* Replicate/AOF this command as an SREM operation */
            if (propindex == 2 + batchsize) {
                alsoPropagate(c->db->id, propargv, propindex, PROPAGATE_AOF | PROPAGATE_REPL);
                for (unsigned long j = 2; j < propindex; j++) {
                    decrRefCount(propargv[j]);
                }
                propindex = 2;
            }

            /* Store pointer for later deletion and move to next. */
            ps[i] = p;
            p = lpNext(lp, p);
            index++;
        }
        lp = lpBatchDelete(lp, ps, count);
        zfree(ps);
        set->ptr = lp;
    } else if (remaining*SPOP_MOVE_STRATEGY_MUL > count) {
        for (unsigned long i = 0; i < count; i++) {
            propargv[propindex] = setTypePopRandom(set);
            addReplyBulk(c, propargv[propindex]);
            propindex++;
            /* Replicate/AOF this command as an SREM operation */
            if (propindex == 2 + batchsize) {
                alsoPropagate(c->db->id, propargv, propindex, PROPAGATE_AOF | PROPAGATE_REPL);
                for (unsigned long j = 2; j < propindex; j++) {
                    decrRefCount(propargv[j]);
                }
                propindex = 2;
            }
        }
    } else {
    /* CASE 3: The number of elements to return is very big, approaching
     * the size of the set itself. After some time extracting random elements
     * from such a set becomes computationally expensive, so we use
     * a different strategy, we extract random elements that we don't
     * want to return (the elements that will remain part of the set),
     * creating a new set as we do this (that will be stored as the original
     * set). Then we return the elements left in the original set and
     * release it. */
        robj *newset = NULL;

        /* Create a new set with just the remaining elements. */
        if (set->encoding == OBJ_ENCODING_LISTPACK) {
            /* Specialized case for listpack. Traverse it only once. */
            newset = createSetListpackObject();
            unsigned char *lp = set->ptr;
            unsigned char *p = lpFirst(lp);
            unsigned int index = 0;
            unsigned char **ps = zmalloc(sizeof(char *) * remaining);
            for (unsigned long i = 0; i < remaining; i++) {
                p = lpNextRandom(lp, p, &index, remaining - i, 0);
                unsigned int len;
                str = (char *)lpGetValue(p, &len, (long long *)&llele);
                setTypeAddAux(newset, str, len, llele, 0);
                ps[i] = p;
                p = lpNext(lp, p);
                index++;
            }
            lp = lpBatchDelete(lp, ps, remaining);
            zfree(ps);
            set->ptr = lp;
        } else {
            while(remaining--) {
                int encoding = setTypeRandomElement(set, &str, &len, &llele);
                if (!newset) {
                    newset = str ? createSetListpackObject() : createIntsetObject();
                }
                setTypeAddAux(newset, str, len, llele, encoding == OBJ_ENCODING_HT);
                setTypeRemoveAux(set, str, len, llele, encoding == OBJ_ENCODING_HT);
            }
        }

        /* Transfer the old set to the client. */
        setTypeIterator *si;
        si = setTypeInitIterator(set);
        while (setTypeNext(si, &str, &len, &llele) != -1) {
            if (str == NULL) {
                addReplyBulkLongLong(c,llele);
                propargv[propindex++] = createStringObjectFromLongLong(llele);
            } else {
                addReplyBulkCBuffer(c, str, len);
                propargv[propindex++] = createStringObject(str, len);
            }
            /* Replicate/AOF this command as an SREM operation */
            if (propindex == 2 + batchsize) {
                alsoPropagate(c->db->id, propargv, propindex, PROPAGATE_AOF | PROPAGATE_REPL);
                for (unsigned long i = 2; i < propindex; i++) {
                    decrRefCount(propargv[i]);
                }
                propindex = 2;
            }
        }
        setTypeReleaseIterator(si);

        /* Assign the new set as the key value. */
        dbReplaceValue(c->db,c->argv[1],newset);
    }

    /* Replicate/AOF the remaining elements as an SREM operation */
    if (propindex != 2) {
        alsoPropagate(c->db->id, propargv, propindex, PROPAGATE_AOF | PROPAGATE_REPL);
        for (unsigned long i = 2; i < propindex; i++) {
            decrRefCount(propargv[i]);
        }
        propindex = 2;
    }
    zfree(propargv);

    /* Don't propagate the command itself even if we incremented the
     * dirty counter. We don't want to propagate an SPOP command since
     * we propagated the command as a set of SREMs operations using
     * the alsoPropagate() API. */
    preventCommandPropagation(c);
    signalModifiedKey(c,c->db,c->argv[1]);
}

void spopCommand(client *c) {
    robj *set, *ele;

    if (c->argc == 3) {
        spopWithCountCommand(c);
        return;
    } else if (c->argc > 3) {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* Make sure a key with the name inputted exists, and that it's type is
     * indeed a set */
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.null[c->resp]))
         == NULL || checkType(c,set,OBJ_SET)) return;

    /* Pop a random element from the set */
    ele = setTypePopRandom(set);

    notifyKeyspaceEvent(NOTIFY_SET,"spop",c->argv[1],c->db->id);

    /* Replicate/AOF this command as an SREM operation */
    rewriteClientCommandVector(c,3,shared.srem,c->argv[1],ele);

    /* Add the element to the reply */
    addReplyBulk(c, ele);
    decrRefCount(ele);

    /* Delete the set if it's empty */
    if (setTypeSize(set) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }

    /* Set has been modified */
    signalModifiedKey(c,c->db,c->argv[1]);
    server.dirty++;
}

/* handle the "SRANDMEMBER key <count>" variant. The normal version of the
 * command is handled by the srandmemberCommand() function itself. */

/* How many times bigger should be the set compared to the requested size
 * for us to don't use the "remove elements" strategy? Read later in the
 * implementation for more info. */
#define SRANDMEMBER_SUB_STRATEGY_MUL 3

/* If client is trying to ask for a very large number of random elements,
 * queuing may consume an unlimited amount of memory, so we want to limit
 * the number of randoms per time. */
#define SRANDFIELD_RANDOM_SAMPLE_LIMIT 1000

void srandmemberWithCountCommand(client *c) {
    long l;
    unsigned long count, size;
    int uniq = 1;
    robj *set;
    char *str;
    size_t len;
    int64_t llele;

    dict *d;

    if (getRangeLongFromObjectOrReply(c,c->argv[2],-LONG_MAX,LONG_MAX,&l,NULL) != C_OK) return;
    if (l >= 0) {
        count = (unsigned long) l;
    } else {
        /* A negative count means: return the same elements multiple times
         * (i.e. don't remove the extracted element after every extraction). */
        count = -l;
        uniq = 0;
    }

    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.emptyarray))
        == NULL || checkType(c,set,OBJ_SET)) return;
    size = setTypeSize(set);

    /* If count is zero, serve it ASAP to avoid special cases later. */
    if (count == 0) {
        addReply(c,shared.emptyarray);
        return;
    }

    /* CASE 1: The count was negative, so the extraction method is just:
     * "return N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures. This case is the only one that also needs to return the
     * elements in random order. */
    if (!uniq || count == 1) {
        addReplyArrayLen(c,count);

        if (set->encoding == OBJ_ENCODING_LISTPACK && count > 1) {
            /* Specialized case for listpack, traversing it only once. */
            unsigned long limit, sample_count;
            limit = count > SRANDFIELD_RANDOM_SAMPLE_LIMIT ? SRANDFIELD_RANDOM_SAMPLE_LIMIT : count;
            listpackEntry *entries = zmalloc(limit * sizeof(listpackEntry));
            while (count) {
                sample_count = count > limit ? limit : count;
                count -= sample_count;
                lpRandomEntries(set->ptr, sample_count, entries);
                for (unsigned long i = 0; i < sample_count; i++) {
                    if (entries[i].sval)
                        addReplyBulkCBuffer(c, entries[i].sval, entries[i].slen);
                    else
                        addReplyBulkLongLong(c, entries[i].lval);
                }
                if (c->flags & CLIENT_CLOSE_ASAP)
                    break;
            }
            zfree(entries);
            return;
        }

        while(count--) {
            setTypeRandomElement(set, &str, &len, &llele);
            if (str == NULL) {
                addReplyBulkLongLong(c,llele);
            } else {
                addReplyBulkCBuffer(c, str, len);
            }
            if (c->flags & CLIENT_CLOSE_ASAP)
                break;
        }
        return;
    }

    /* CASE 2:
     * The number of requested elements is greater than the number of
     * elements inside the set: simply return the whole set. */
    if (count >= size) {
        setTypeIterator *si;
        addReplyArrayLen(c,size);
        si = setTypeInitIterator(set);
        while (setTypeNext(si, &str, &len, &llele) != -1) {
            if (str == NULL) {
                addReplyBulkLongLong(c,llele);
            } else {
                addReplyBulkCBuffer(c, str, len);
            }
            size--;
        }
        setTypeReleaseIterator(si);
        serverAssert(size==0);
        return;
    }

    /* CASE 2.5 listpack only. Sampling unique elements, in non-random order.
     * Listpack encoded sets are meant to be relatively small, so
     * SRANDMEMBER_SUB_STRATEGY_MUL isn't necessary and we rather not make
     * copies of the entries. Instead, we emit them directly to the output
     * buffer.
     *
     * And it is inefficient to repeatedly pick one random element from a
     * listpack in CASE 4. So we use this instead. */
    if (set->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        unsigned int i = 0;
        addReplyArrayLen(c, count);
        while (count) {
            p = lpNextRandom(lp, p, &i, count--, 0);
            unsigned int len;
            str = (char *)lpGetValue(p, &len, (long long *)&llele);
            if (str == NULL) {
                addReplyBulkLongLong(c, llele);
            } else {
                addReplyBulkCBuffer(c, str, len);
            }
            p = lpNext(lp, p);
            i++;
        }
        return;
    }

    /* For CASE 3 and CASE 4 we need an auxiliary dictionary. */
    d = dictCreate(&sdsReplyDictType);

    /* CASE 3:
     * The number of elements inside the set is not greater than
     * SRANDMEMBER_SUB_STRATEGY_MUL times the number of requested elements.
     * In this case we create a set from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requested elements is just
     * a bit less than the number of elements in the set, the natural approach
     * used into CASE 4 is highly inefficient. */
    if (count*SRANDMEMBER_SUB_STRATEGY_MUL > size) {
        setTypeIterator *si;

        /* Add all the elements into the temporary dictionary. */
        si = setTypeInitIterator(set);
        dictExpand(d, size);
        while (setTypeNext(si, &str, &len, &llele) != -1) {
            int retval = DICT_ERR;

            if (str == NULL) {
                retval = dictAdd(d,sdsfromlonglong(llele),NULL);
            } else {
                retval = dictAdd(d, sdsnewlen(str, len), NULL);
            }
            serverAssert(retval == DICT_OK);
        }
        setTypeReleaseIterator(si);
        serverAssert(dictSize(d) == size);

        /* Remove random elements to reach the right count. */
        while (size > count) {
            dictEntry *de;
            de = dictGetFairRandomKey(d);
            dictUnlink(d,dictGetKey(de));
            sdsfree(dictGetKey(de));
            dictFreeUnlinkedEntry(d,de);
            size--;
        }
    }

    /* CASE 4: We have a big set compared to the requested number of elements.
     * In this case we can simply get random elements from the set and add
     * to the temporary set, trying to eventually get enough unique elements
     * to reach the specified count. */
    else {
        unsigned long added = 0;
        sds sdsele;

        dictExpand(d, count);
        while (added < count) {
            setTypeRandomElement(set, &str, &len, &llele);
            if (str == NULL) {
                sdsele = sdsfromlonglong(llele);
            } else {
                sdsele = sdsnewlen(str, len);
            }
            /* Try to add the object to the dictionary. If it already exists
             * free it, otherwise increment the number of objects we have
             * in the result dictionary. */
            if (dictAdd(d,sdsele,NULL) == DICT_OK)
                added++;
            else
                sdsfree(sdsele);
        }
    }

    /* CASE 3 & 4: send the result to the user. */
    {
        dictIterator *di;
        dictEntry *de;

        addReplyArrayLen(c,count);
        di = dictGetIterator(d);
        while((de = dictNext(di)) != NULL)
            addReplyBulkSds(c,dictGetKey(de));
        dictReleaseIterator(di);
        dictRelease(d);
    }
}

/* SRANDMEMBER <key> [<count>] */
void srandmemberCommand(client *c) {
    robj *set;
    char *str;
    size_t len;
    int64_t llele;

    if (c->argc == 3) {
        srandmemberWithCountCommand(c);
        return;
    } else if (c->argc > 3) {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* Handle variant without <count> argument. Reply with simple bulk string */
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp]))
        == NULL || checkType(c,set,OBJ_SET)) return;

    setTypeRandomElement(set, &str, &len, &llele);
    if (str == NULL) {
        addReplyBulkLongLong(c,llele);
    } else {
        addReplyBulkCBuffer(c, str, len);
    }
}

int qsortCompareSetsByCardinality(const void *s1, const void *s2) {
    if (setTypeSize(*(robj**)s1) > setTypeSize(*(robj**)s2)) return 1;
    if (setTypeSize(*(robj**)s1) < setTypeSize(*(robj**)s2)) return -1;
    return 0;
}

/* This is used by SDIFF and in this case we can receive NULL that should
 * be handled as empty sets. */
int qsortCompareSetsByRevCardinality(const void *s1, const void *s2) {
    robj *o1 = *(robj**)s1, *o2 = *(robj**)s2;
    unsigned long first = o1 ? setTypeSize(o1) : 0;
    unsigned long second = o2 ? setTypeSize(o2) : 0;

    if (first < second) return 1;
    if (first > second) return -1;
    return 0;
}

/* SINTER / SMEMBERS / SINTERSTORE / SINTERCARD
 *
 * 'cardinality_only' work for SINTERCARD, only return the cardinality
 * with minimum processing and memory overheads.
 *
 * 'limit' work for SINTERCARD, stop searching after reaching the limit.
 * Passing a 0 means unlimited.
 */
void sinterGenericCommand(client *c, robj **setkeys,
                          unsigned long setnum, robj *dstkey,
                          int cardinality_only, unsigned long limit) {
    robj **sets = zmalloc(sizeof(robj*)*setnum);
    setTypeIterator *si;
    robj *dstset = NULL;
    char *str;
    size_t len;
    int64_t intobj;
    void *replylen = NULL;
    unsigned long j, cardinality = 0;
    int encoding, empty = 0;

    for (j = 0; j < setnum; j++) {
        robj *setobj = lookupKeyRead(c->db, setkeys[j]);
        if (!setobj) {
            /* A NULL is considered an empty set */
            empty += 1;
            sets[j] = NULL;
            continue;
        }
        if (checkType(c,setobj,OBJ_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
    }

    /* Set intersection with an empty set always results in an empty set.
     * Return ASAP if there is an empty set. */
    if (empty > 0) {
        zfree(sets);
        if (dstkey) {
            if (dbDelete(c->db,dstkey)) {
                signalModifiedKey(c,c->db,dstkey);
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",dstkey,c->db->id);
                server.dirty++;
            }
            addReply(c,shared.czero);
        } else if (cardinality_only) {
            addReplyLongLong(c,cardinality);
        } else {
            addReply(c,shared.emptyset[c->resp]);
        }
        return;
    }

    /* Sort sets from the smallest to largest, this will improve our
     * algorithm's performance */
    qsort(sets,setnum,sizeof(robj*),qsortCompareSetsByCardinality);

    /* The first thing we should output is the total number of elements...
     * since this is a multi-bulk write, but at this stage we don't know
     * the intersection set size, so we use a trick, append an empty object
     * to the output list and save the pointer to later modify it with the
     * right length */
    if (dstkey) {
        /* If we have a target key where to store the resulting set
         * create this key with an empty set inside */
        if (sets[0]->encoding == OBJ_ENCODING_INTSET) {
            /* The first set is an intset, so the result is an intset too. The
             * elements are inserted in ascending order which is efficient in an
             * intset. */
            dstset = createIntsetObject();
        } else if (sets[0]->encoding == OBJ_ENCODING_LISTPACK) {
            /* To avoid many reallocs, we estimate that the result is a listpack
             * of approximately the same size as the first set. Then we shrink
             * it or possibly convert it to intset in the end. */
            unsigned char *lp = lpNew(lpBytes(sets[0]->ptr));
            dstset = createObject(OBJ_SET, lp);
            dstset->encoding = OBJ_ENCODING_LISTPACK;
        } else {
            /* We start off with a listpack, since it's more efficient to append
             * to than an intset. Later we can convert it to intset or a
             * hashtable. */
            dstset = createSetListpackObject();
        }
    } else if (!cardinality_only) {
        replylen = addReplyDeferredLen(c);
    }

    /* Iterate all the elements of the first (smallest) set, and test
     * the element against all the other sets, if at least one set does
     * not include the element it is discarded */
    int only_integers = 1;
    si = setTypeInitIterator(sets[0]);
    while((encoding = setTypeNext(si, &str, &len, &intobj)) != -1) {
        for (j = 1; j < setnum; j++) {
            if (sets[j] == sets[0]) continue;
            if (!setTypeIsMemberAux(sets[j], str, len, intobj,
                                    encoding == OBJ_ENCODING_HT))
                break;
        }

        /* Only take action when all sets contain the member */
        if (j == setnum) {
            if (cardinality_only) {
                cardinality++;

                /* We stop the searching after reaching the limit. */
                if (limit && cardinality >= limit)
                    break;
            } else if (!dstkey) {
                if (str != NULL)
                    addReplyBulkCBuffer(c, str, len);
                else
                    addReplyBulkLongLong(c,intobj);
                cardinality++;
            } else {
                if (str && only_integers) {
                    /* It may be an integer although we got it as a string. */
                    if (encoding == OBJ_ENCODING_HT &&
                        string2ll(str, len, (long long *)&intobj))
                    {
                        if (dstset->encoding == OBJ_ENCODING_LISTPACK ||
                            dstset->encoding == OBJ_ENCODING_INTSET)
                        {
                            /* Adding it as an integer is more efficient. */
                            str = NULL;
                        }
                    } else {
                        /* It's not an integer */
                        only_integers = 0;
                    }
                }
                setTypeAddAux(dstset, str, len, intobj, encoding == OBJ_ENCODING_HT);
            }
        }
    }
    setTypeReleaseIterator(si);

    if (cardinality_only) {
        addReplyLongLong(c,cardinality);
    } else if (dstkey) {
        /* Store the resulting set into the target, if the intersection
         * is not an empty set. */
        if (setTypeSize(dstset) > 0) {
            if (only_integers) maybeConvertToIntset(dstset);
            if (dstset->encoding == OBJ_ENCODING_LISTPACK) {
                /* We allocated too much memory when we created it to avoid
                 * frequent reallocs. Therefore, we shrink it now. */
                dstset->ptr = lpShrinkToFit(dstset->ptr);
            }
            setKey(c,c->db,dstkey,dstset,0);
            addReplyLongLong(c,setTypeSize(dstset));
            notifyKeyspaceEvent(NOTIFY_SET,"sinterstore",
                dstkey,c->db->id);
            server.dirty++;
        } else {
            addReply(c,shared.czero);
            if (dbDelete(c->db,dstkey)) {
                server.dirty++;
                signalModifiedKey(c,c->db,dstkey);
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",dstkey,c->db->id);
            }
        }
        decrRefCount(dstset);
    } else {
        setDeferredSetLen(c,replylen,cardinality);
    }
    zfree(sets);
}

/* SINTER key [key ...] */
void sinterCommand(client *c) {
    sinterGenericCommand(c, c->argv+1,  c->argc-1, NULL, 0, 0);
}

/* SINTERCARD numkeys key [key ...] [LIMIT limit] */
void sinterCardCommand(client *c) {
    long j;
    long numkeys = 0; /* Number of keys. */
    long limit = 0;   /* 0 means not limit. */

    if (getRangeLongFromObjectOrReply(c, c->argv[1], 1, LONG_MAX,
                                      &numkeys, "numkeys should be greater than 0") != C_OK)
        return;
    if (numkeys > (c->argc - 2)) {
        addReplyError(c, "Number of keys can't be greater than number of args");
        return;
    }

    for (j = 2 + numkeys; j < c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc - 1) - j;

        if (!strcasecmp(opt, "LIMIT") && moreargs) {
            j++;
            if (getPositiveLongFromObjectOrReply(c, c->argv[j], &limit,
                                                 "LIMIT can't be negative") != C_OK)
                return;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }

    sinterGenericCommand(c, c->argv+2, numkeys, NULL, 1, limit);
}

/* SINTERSTORE destination key [key ...] */
void sinterstoreCommand(client *c) {
    sinterGenericCommand(c, c->argv+2, c->argc-2, c->argv[1], 0, 0);
}

void sunionDiffGenericCommand(client *c, robj **setkeys, int setnum,
                              robj *dstkey, int op) {
    robj **sets = zmalloc(sizeof(robj*)*setnum);
    setTypeIterator *si;
    robj *dstset = NULL;
    char *str;
    size_t len;
    int64_t llval;
    int encoding;
    int j, cardinality = 0;
    int diff_algo = 1;
    int sameset = 0; 

    for (j = 0; j < setnum; j++) {
        robj *setobj = lookupKeyRead(c->db, setkeys[j]);
        if (!setobj) {
            sets[j] = NULL;
            continue;
        }
        if (checkType(c,setobj,OBJ_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
        if (j > 0 && sets[0] == sets[j]) {
            sameset = 1; 
        }
    }

    /* Select what DIFF algorithm to use.
     *
     * Algorithm 1 is O(N*M) where N is the size of the element first set
     * and M the total number of sets.
     *
     * Algorithm 2 is O(N) where N is the total number of elements in all
     * the sets.
     *
     * We compute what is the best bet with the current input here. */
    if (op == SET_OP_DIFF && sets[0] && !sameset) {
        long long algo_one_work = 0, algo_two_work = 0;

        for (j = 0; j < setnum; j++) {
            if (sets[j] == NULL) continue;

            algo_one_work += setTypeSize(sets[0]);
            algo_two_work += setTypeSize(sets[j]);
        }

        /* Algorithm 1 has better constant times and performs less operations
         * if there are elements in common. Give it some advantage. */
        algo_one_work /= 2;
        diff_algo = (algo_one_work <= algo_two_work) ? 1 : 2;

        if (diff_algo == 1 && setnum > 1) {
            /* With algorithm 1 it is better to order the sets to subtract
             * by decreasing size, so that we are more likely to find
             * duplicated elements ASAP. */
            qsort(sets+1,setnum-1,sizeof(robj*),
                qsortCompareSetsByRevCardinality);
        }
    }

    /* We need a temp set object to store our union/diff. If the dstkey
     * is not NULL (that is, we are inside an SUNIONSTORE/SDIFFSTORE operation) then
     * this set object will be the resulting object to set into the target key*/
    dstset = createIntsetObject();

    if (op == SET_OP_UNION) {
        /* Union is trivial, just add every element of every set to the
         * temporary set. */
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* non existing keys are like empty sets */

            si = setTypeInitIterator(sets[j]);
            while ((encoding = setTypeNext(si, &str, &len, &llval)) != -1) {
                cardinality += setTypeAddAux(dstset, str, len, llval, encoding == OBJ_ENCODING_HT);
            }
            setTypeReleaseIterator(si);
        }
    } else if (op == SET_OP_DIFF && sameset) {
        /* At least one of the sets is the same one (same key) as the first one, result must be empty. */
    } else if (op == SET_OP_DIFF && sets[0] && diff_algo == 1) {
        /* DIFF Algorithm 1:
         *
         * We perform the diff by iterating all the elements of the first set,
         * and only adding it to the target set if the element does not exist
         * into all the other sets.
         *
         * This way we perform at max N*M operations, where N is the size of
         * the first set, and M the number of sets. */
        si = setTypeInitIterator(sets[0]);
        while ((encoding = setTypeNext(si, &str, &len, &llval)) != -1) {
            for (j = 1; j < setnum; j++) {
                if (!sets[j]) continue; /* no key is an empty set. */
                if (sets[j] == sets[0]) break; /* same set! */
                if (setTypeIsMemberAux(sets[j], str, len, llval,
                                       encoding == OBJ_ENCODING_HT))
                    break;
            }
            if (j == setnum) {
                /* There is no other set with this element. Add it. */
                cardinality += setTypeAddAux(dstset, str, len, llval, encoding == OBJ_ENCODING_HT);
            }
        }
        setTypeReleaseIterator(si);
    } else if (op == SET_OP_DIFF && sets[0] && diff_algo == 2) {
        /* DIFF Algorithm 2:
         *
         * Add all the elements of the first set to the auxiliary set.
         * Then remove all the elements of all the next sets from it.
         *
         * This is O(N) where N is the sum of all the elements in every
         * set. */
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* non existing keys are like empty sets */

            si = setTypeInitIterator(sets[j]);
            while((encoding = setTypeNext(si, &str, &len, &llval)) != -1) {
                if (j == 0) {
                    cardinality += setTypeAddAux(dstset, str, len, llval,
                                                 encoding == OBJ_ENCODING_HT);
                } else {
                    cardinality -= setTypeRemoveAux(dstset, str, len, llval,
                                                    encoding == OBJ_ENCODING_HT);
                }
            }
            setTypeReleaseIterator(si);

            /* Exit if result set is empty as any additional removal
             * of elements will have no effect. */
            if (cardinality == 0) break;
        }
    }

    /* Output the content of the resulting set, if not in STORE mode */
    if (!dstkey) {
        addReplySetLen(c,cardinality);
        si = setTypeInitIterator(dstset);
        while (setTypeNext(si, &str, &len, &llval) != -1) {
            if (str)
                addReplyBulkCBuffer(c, str, len);
            else
                addReplyBulkLongLong(c, llval);
        }
        setTypeReleaseIterator(si);
        server.lazyfree_lazy_server_del ? freeObjAsync(NULL, dstset, -1) :
                                          decrRefCount(dstset);
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with the result set inside */
        if (setTypeSize(dstset) > 0) {
            setKey(c,c->db,dstkey,dstset,0);
            addReplyLongLong(c,setTypeSize(dstset));
            notifyKeyspaceEvent(NOTIFY_SET,
                op == SET_OP_UNION ? "sunionstore" : "sdiffstore",
                dstkey,c->db->id);
            server.dirty++;
        } else {
            addReply(c,shared.czero);
            if (dbDelete(c->db,dstkey)) {
                server.dirty++;
                signalModifiedKey(c,c->db,dstkey);
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",dstkey,c->db->id);
            }
        }
        decrRefCount(dstset);
    }
    zfree(sets);
}

/* SUNION key [key ...] */
void sunionCommand(client *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,SET_OP_UNION);
}

/* SUNIONSTORE destination key [key ...] */
void sunionstoreCommand(client *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],SET_OP_UNION);
}

/* SDIFF key [key ...] */
void sdiffCommand(client *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,SET_OP_DIFF);
}

/* SDIFFSTORE destination key [key ...] */
void sdiffstoreCommand(client *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],SET_OP_DIFF);
}

void sscanCommand(client *c) {
    robj *set;
    unsigned long cursor;

    if (parseScanCursorOrReply(c,c->argv[2],&cursor) == C_ERR) return;
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.emptyscan)) == NULL ||
        checkType(c,set,OBJ_SET)) return;
    scanGenericCommand(c,set,cursor);
}

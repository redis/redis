#include "redis.h"
#include "pqsort.h" /* Partial qsort for SORT+LIMIT */

redisSortOperation *createSortOperation(int type, robj *pattern) {
    redisSortOperation *so = zmalloc(sizeof(*so));
    so->type = type;
    so->pattern = pattern;
    return so;
}

/* Return the value associated to the key with a name obtained
 * substituting the first occurence of '*' in 'pattern' with 'subst'.
 * The returned object will always have its refcount increased by 1
 * when it is non-NULL. */
robj *lookupKeyByPattern(redisDb *db, robj *pattern, robj *subst) {
    char *p, *f;
    sds spat, ssub;
    robj keyobj, fieldobj, *o;
    int prefixlen, sublen, postfixlen, fieldlen;
    /* Expoit the internal sds representation to create a sds string allocated on the stack in order to make this function faster */
    struct {
        int len;
        int free;
        char buf[REDIS_SORTKEY_MAX+1];
    } keyname, fieldname;

    /* If the pattern is "#" return the substitution object itself in order
     * to implement the "SORT ... GET #" feature. */
    spat = pattern->ptr;
    if (spat[0] == '#' && spat[1] == '\0') {
        incrRefCount(subst);
        return subst;
    }

    /* The substitution object may be specially encoded. If so we create
     * a decoded object on the fly. Otherwise getDecodedObject will just
     * increment the ref count, that we'll decrement later. */
    subst = getDecodedObject(subst);

    ssub = subst->ptr;
    if (sdslen(spat)+sdslen(ssub)-1 > REDIS_SORTKEY_MAX) return NULL;
    p = strchr(spat,'*');
    if (!p) {
        decrRefCount(subst);
        return NULL;
    }

    /* Find out if we're dealing with a hash dereference. */
    if ((f = strstr(p+1, "->")) != NULL) {
        fieldlen = sdslen(spat)-(f-spat);
        /* this also copies \0 character */
        memcpy(fieldname.buf,f+2,fieldlen-1);
        fieldname.len = fieldlen-2;
    } else {
        fieldlen = 0;
    }

    prefixlen = p-spat;
    sublen = sdslen(ssub);
    postfixlen = sdslen(spat)-(prefixlen+1)-fieldlen;
    memcpy(keyname.buf,spat,prefixlen);
    memcpy(keyname.buf+prefixlen,ssub,sublen);
    memcpy(keyname.buf+prefixlen+sublen,p+1,postfixlen);
    keyname.buf[prefixlen+sublen+postfixlen] = '\0';
    keyname.len = prefixlen+sublen+postfixlen;
    decrRefCount(subst);

    /* Lookup substituted key */
    initStaticStringObject(keyobj,((char*)&keyname)+(sizeof(struct sdshdr)));
    o = lookupKeyRead(db,&keyobj);
    if (o == NULL) return NULL;

    if (fieldlen > 0) {
        if (o->type != REDIS_HASH || fieldname.len < 1) return NULL;

        /* Retrieve value from hash by the field name. This operation
         * already increases the refcount of the returned object. */
        initStaticStringObject(fieldobj,((char*)&fieldname)+(sizeof(struct sdshdr)));
        o = hashTypeGetObject(o, &fieldobj);
    } else {
        if (o->type != REDIS_STRING) return NULL;

        /* Every object that this function returns needs to have its refcount
         * increased. sortCommand decreases it again. */
        incrRefCount(o);
    }

    return o;
}

/* sortCompareDeep() performs the recursive comparisons for the multiple BY
 * patterns. Sorting parameters are passed within the objects, since each BY 
 * pattern may have the modifiers ASC, DESC and ALPHA. The use of BY patterns
 * is indicated by a parameter in the global 'server' structure */
int sortCompareDeep(const void *s1, const void *s2, int deep) {
    const redisSortObject *so1 = s1, *so2 = s2;
    int cmp;

    if (!so1->u[deep].sort_alpha) {
        /* Numeric sorting. Here it's trivial as we precomputed scores */
        if (so1->u[deep].score > so2->u[deep].score) {
            cmp = 1;
        } else if (so1->u[deep].score < so2->u[deep].score) {
            cmp = -1;
        } else {
            if( deep < so1->total-1 )
              return sortCompareDeep(s1, s2, deep+1);
            else
              cmp = 0;
        }
    } else {
        /* Alphanumeric sorting */
        if (server.sort_bypattern) {
            if (!so1->u[deep].cmpobj || !so2->u[deep].cmpobj) {
                /* At least one compare object is NULL */
                if (so1->u[deep].cmpobj == so2->u[deep].cmpobj) {
                    if( deep < so1->total-1 )
                        return sortCompareDeep(s1, s2, deep+1);
                    else
                        cmp = 0;
                } else if (so1->u[deep].cmpobj == NULL)
                    cmp = -1;
                else
                    cmp = 1;
            } else {
                /* We have both the objects, use strcoll */
                cmp = strcoll(so1->u[deep].cmpobj->ptr,so2->u[deep].cmpobj->ptr);
                if( cmp == 0 && deep < so1->total-1 )
                    return sortCompareDeep(s1, s2, deep+1);
            }
        } else {
            /* Compare elements directly. */
            cmp = compareStringObjects(so1->obj,so2->obj);
            if( cmp == 0 && deep < so1->total-1 )
                return sortCompareDeep(s1, s2, deep+1);
        }
    }
    return so1->u[deep].sort_desc ? -cmp : cmp;
}

/* sortCompare() is used by qsort in sortCommand(). Initiates the recursive
 * comparisons starting by the first BY pattern */
int sortCompare(const void *s1, const void *s2) {
    return sortCompareDeep( s1, s2, 0 );
}

/* The SORT command is the most complex command in Redis. Warning: this code
 * is optimized for speed and a bit less for readability */
void sortCommand(redisClient *c) {
    list *operations;
    unsigned int outputlen = 0;
    int *desc = NULL, *alpha = NULL;
    int limit_start = 0, limit_count = -1, start, end;
    int j, n, dontsort = 0, numsorts = 0, vectorlen;
    int getop = 0; /* GET operation counter */
    robj *sortval, **sortby = NULL, *storekey = NULL;
    redisSortObject *vector; /* Resulting vector to sort */
    
    sortby = zmalloc( sizeof(robj *) );
    desc = zmalloc( sizeof(int) );
    alpha = zmalloc( sizeof(int) );
    sortby[0] = NULL;
    desc[0] = 0;
    alpha[0] = 0;
    
    /* Lookup the key to sort. It must be of the right types */
    sortval = lookupKeyRead(c->db,c->argv[1]);
    if (sortval && sortval->type != REDIS_SET && sortval->type != REDIS_LIST &&
        sortval->type != REDIS_ZSET)
    {
        addReply(c,shared.wrongtypeerr);
        return;
    }

    /* Create a list of operations to perform for every sorted element.
     * Operations can be GET/DEL/INCR/DECR */
    operations = listCreate();
    listSetFreeMethod(operations,zfree);
    j = 2;

    /* Now we need to protect sortval incrementing its count, in the future
     * SORT may have options able to overwrite/delete keys during the sorting
     * and the sorted key itself may get destroied */
    if (sortval)
        incrRefCount(sortval);
    else
        sortval = createListObject();

    /* The SORT command has an SQL-alike syntax, parse it */
    while(j < c->argc) {
        int leftargs = c->argc-j-1;
        if (!strcasecmp(c->argv[j]->ptr,"asc")) {
            if(numsorts<1) desc[0] = 0;
            else desc[numsorts-1] = 0;
        } else if (!strcasecmp(c->argv[j]->ptr,"desc")) {
            if(numsorts<1) desc[0] = 1;
            else desc[numsorts-1] = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"alpha")) {
            if(numsorts<1) alpha[0] = 1;
            else alpha[numsorts-1] = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"limit") && leftargs >= 2) {
            limit_start = atoi(c->argv[j+1]->ptr);
            limit_count = atoi(c->argv[j+2]->ptr);
            j+=2;
        } else if (!strcasecmp(c->argv[j]->ptr,"store") && leftargs >= 1) {
            storekey = c->argv[j+1];
            j++;
        } else if (!strcasecmp(c->argv[j]->ptr,"by") && leftargs >= 1) {
            sortby = zrealloc(sortby, sizeof(robj *) * (numsorts+1));
            desc = zrealloc(desc, sizeof(int) * (numsorts+1));
            alpha = zrealloc(alpha, sizeof(int) * (numsorts+1));
            sortby[numsorts] = c->argv[j+1];
            desc[numsorts] = 0;
            alpha[numsorts] = 0;
            numsorts ++;
            /* If the BY pattern does not contain '*', i.e. it is constant,
             * we don't need to sort nor to lookup the weight keys. */
            if (strchr(c->argv[j+1]->ptr,'*') == NULL) dontsort = 1;
            j++;
        } else if (!strcasecmp(c->argv[j]->ptr,"get") && leftargs >= 1) {
            listAddNodeTail(operations,createSortOperation(
                REDIS_SORT_GET,c->argv[j+1]));
            getop++;
            j++;
        } else {
            decrRefCount(sortval);
            listRelease(operations);
            addReply(c,shared.syntaxerr);
            return;
        }
        j++;
    }

    /* At least we need one sort */
    if( numsorts == 0 ) numsorts++;

    /* Destructively convert encoded sorted sets for SORT. */
    if (sortval->type == REDIS_ZSET)
        zsetConvert(sortval, REDIS_ENCODING_SKIPLIST);

    /* Load the sorting vector with all the objects to sort */
    switch(sortval->type) {
    case REDIS_LIST: vectorlen = listTypeLength(sortval); break;
    case REDIS_SET: vectorlen =  setTypeSize(sortval); break;
    case REDIS_ZSET: vectorlen = dictSize(((zset*)sortval->ptr)->dict); break;
    default: vectorlen = 0; redisPanic("Bad SORT type"); /* Avoid GCC warning */
    }
    vector = zmalloc(sizeof(redisSortObject)*vectorlen);
    j = 0;

    if (sortval->type == REDIS_LIST) {
        listTypeIterator *li = listTypeInitIterator(sortval,0,REDIS_TAIL);
        listTypeEntry entry;
        while(listTypeNext(li,&entry)) {
            vector[j].obj = listTypeGet(&entry);
            vector[j].u = zmalloc(sizeof(u_sort)*numsorts);
            vector[j].total = numsorts;
            for(n = 0; n < numsorts; n++) {
              vector[j].u[n].sort_alpha = alpha[n];
              vector[j].u[n].sort_desc = desc[n];
              vector[j].u[n].score = 0;
              vector[j].u[n].cmpobj = NULL;
            }
            j++;
        }
        listTypeReleaseIterator(li);
    } else if (sortval->type == REDIS_SET) {
        setTypeIterator *si = setTypeInitIterator(sortval);
        robj *ele;
        while((ele = setTypeNextObject(si)) != NULL) {
            vector[j].obj = ele;
            vector[j].u = zmalloc(sizeof(u_sort)*numsorts);
            vector[j].total = numsorts;
            for(n = 0; n < numsorts; n++) {
              vector[j].u[n].sort_alpha = alpha[n];
              vector[j].u[n].sort_desc = desc[n];
              vector[j].u[n].score = 0;
              vector[j].u[n].cmpobj = NULL;
            }
            j++;
        }
        setTypeReleaseIterator(si);
    } else if (sortval->type == REDIS_ZSET) {
        dict *set = ((zset*)sortval->ptr)->dict;
        dictIterator *di;
        dictEntry *setele;
        di = dictGetIterator(set);
        while((setele = dictNext(di)) != NULL) {
            vector[j].obj = dictGetKey(setele);
            vector[j].u = zmalloc(sizeof(u_sort)*numsorts);
            vector[j].total = numsorts;
            for(n = 0; n < numsorts; n++) {
              vector[j].u[n].sort_alpha = alpha[n];
              vector[j].u[n].sort_desc = desc[n];
              vector[j].u[n].score = 0;
              vector[j].u[n].cmpobj = NULL;
            }
            j++;
        }
        dictReleaseIterator(di);
    } else {
        redisPanic("Unknown type");
    }
    redisAssertWithInfo(c,sortval,j == vectorlen);

    /* Now it's time to load the right scores in the sorting vector */
    if (dontsort == 0) {
        for (j = 0; j < vectorlen; j++) {
            for (n = 0; n < numsorts; n++) {
                robj *byval;
                if (sortby[n]) {
                    /* lookup value to sort by */
                    byval = lookupKeyByPattern(c->db,sortby[n],vector[j].obj);
                    if (!byval) continue;
                } else {
                    /* use object itself to sort by */
                    byval = vector[j].obj;
                }

                if (alpha[n]) {
                    vector[j].u[n].cmpobj = getDecodedObject(byval);
                } else {
                    if (byval->encoding == REDIS_ENCODING_RAW) {
                        vector[j].u[n].score = strtod(byval->ptr,NULL);
                    } else if (byval->encoding == REDIS_ENCODING_INT) {
                        /* Don't need to decode the object if it's
                         * integer-encoded (the only encoding supported) so
                         * far. We can just cast it */
                        vector[j].u[n].score = (long)byval->ptr;
                    } else {
                        redisAssertWithInfo(c,sortval,1 != 1);
                    }
                }
    
                /* when the object was retrieved using lookupKeyByPattern,
                 * its refcount needs to be decreased. */
                if (sortby[n]) {
                    decrRefCount(byval);
                }
            }
        }
    }

    /* We are ready to sort the vector... perform a bit of sanity check
     * on the LIMIT option too. We'll use a partial version of quicksort. */
    start = (limit_start < 0) ? 0 : limit_start;
    end = (limit_count < 0) ? vectorlen-1 : start+limit_count-1;
    if (start >= vectorlen) {
        start = vectorlen-1;
        end = vectorlen-2;
    }
    if (end >= vectorlen) end = vectorlen-1;

    if (dontsort == 0) {
        server.sort_bypattern = sortby[0] ? 1 : 0;
        if (sortby && (start != 0 || end != vectorlen-1))
            pqsort(vector,vectorlen,sizeof(redisSortObject),sortCompare, start,end);
        else
            qsort(vector,vectorlen,sizeof(redisSortObject),sortCompare);
    }

    /* Send command output to the output buffer, performing the specified
     * GET/DEL/INCR/DECR operations if any. */
    outputlen = getop ? getop*(end-start+1) : end-start+1;
    if (storekey == NULL) {
        /* STORE option not specified, sent the sorting result to client */
        addReplyMultiBulkLen(c,outputlen);
        for (j = start; j <= end; j++) {
            listNode *ln;
            listIter li;

            if (!getop) addReplyBulk(c,vector[j].obj);
            listRewind(operations,&li);
            while((ln = listNext(&li))) {
                redisSortOperation *sop = ln->value;
                robj *val = lookupKeyByPattern(c->db,sop->pattern,
                    vector[j].obj);

                if (sop->type == REDIS_SORT_GET) {
                    if (!val) {
                        addReply(c,shared.nullbulk);
                    } else {
                        addReplyBulk(c,val);
                        decrRefCount(val);
                    }
                } else {
                    /* Always fails */
                    redisAssertWithInfo(c,sortval,sop->type == REDIS_SORT_GET);
                }
            }
        }
    } else {
        robj *sobj = createZiplistObject();

        /* STORE option specified, set the sorting result as a List object */
        for (j = start; j <= end; j++) {
            listNode *ln;
            listIter li;

            if (!getop) {
                listTypePush(sobj,vector[j].obj,REDIS_TAIL);
            } else {
                listRewind(operations,&li);
                while((ln = listNext(&li))) {
                    redisSortOperation *sop = ln->value;
                    robj *val = lookupKeyByPattern(c->db,sop->pattern,
                        vector[j].obj);

                    if (sop->type == REDIS_SORT_GET) {
                        if (!val) val = createStringObject("",0);

                        /* listTypePush does an incrRefCount, so we should take care
                         * care of the incremented refcount caused by either
                         * lookupKeyByPattern or createStringObject("",0) */
                        listTypePush(sobj,val,REDIS_TAIL);
                        decrRefCount(val);
                    } else {
                        /* Always fails */
                        redisAssertWithInfo(c,sortval,sop->type == REDIS_SORT_GET);
                    }
                }
            }
        }
        if (outputlen) setKey(c->db,storekey,sobj);
        decrRefCount(sobj);
        server.dirty += outputlen;
        addReplyLongLong(c,outputlen);
    }

    /* Cleanup */
    if (sortval->type == REDIS_LIST || sortval->type == REDIS_SET)
        for (j = 0; j < vectorlen; j++)
            decrRefCount(vector[j].obj);
    decrRefCount(sortval);
    listRelease(operations);
    for (j = 0; j < vectorlen; j++) {
        for (n = 0; n < numsorts; n++) {
            if (alpha[n] && vector[j].u[n].cmpobj)
                decrRefCount(vector[j].u[n].cmpobj);
        }
        zfree(vector[j].u);
    }
    zfree(vector);
    zfree(sortby);
    zfree(alpha);
    zfree(desc);
}



/* Native Top-K commands and object lifecycle.
 * Copyright (c) 2026-Present, Redis Ltd.; RSALv2/SSPLv1/AGPLv3. */
#include "server.h"
#include "topk.h"
#include "probabilistic.h"
#include <math.h>

robj *createTopkObject(void *p) { return createObject(OBJ_TOPK, p); }
void freeTopkObject(robj *o) { TopK_Destroy(o->ptr); }
size_t topkObjectLength(robj *o) {
    TopK *t=o->ptr;
    size_t n=0;
    for (uint32_t i=0; i<t->k; i++) n += t->heap[i].count != 0;
    return n;
}
size_t topkFreeEffort(robj *o) { return (size_t)((TopK *)o->ptr)->k + 3; }
size_t topkAllocSize(robj *o) {
    TopK *t=o->ptr;
    size_t n=zmalloc_size(t)+zmalloc_size(t->data)+zmalloc_size(t->heap);
    for (uint32_t i=0; i<t->k; i++) if (t->heap[i].item) n+=zmalloc_size(t->heap[i].item);
    return n;
}
void topkDismiss(robj *o) {
    TopK *t=o->ptr;
    dismissMemory(t->data, (size_t)t->width*t->depth*sizeof(Bucket));
    dismissMemory(t->heap, (size_t)t->k*sizeof(HeapBucket));
}
robj *topkDup(robj *o) {
    TopK *t=o->ptr, *copy=zmalloc(sizeof(*copy));
    *copy=*t;
    size_t bytes=(size_t)t->width*t->depth*sizeof(Bucket);
    copy->data=zmalloc(bytes); memcpy(copy->data,t->data,bytes);
    bytes=(size_t)t->k*sizeof(HeapBucket);
    copy->heap=zmalloc(bytes); memcpy(copy->heap,t->heap,bytes);
    for (uint32_t i=0; i<t->k; i++) if (t->heap[i].item) {
        bytes=(size_t)t->heap[i].itemlen+1;
        copy->heap[i].item=zmalloc(bytes);
        memcpy(copy->heap[i].item,t->heap[i].item,bytes);
    }
    return createTopkObject(copy);
}
void topkDefrag(robj *o, void *(*defrag)(void *)) {
    void *p=defrag(o->ptr); if (p) o->ptr=p;
    TopK *t=o->ptr;
    p=defrag(t->data); if (p) t->data=p;
    p=defrag(t->heap); if (p) t->heap=p;
    for (uint32_t i=0; i<t->k; i++) if (t->heap[i].item) {
        p=defrag(t->heap[i].item); if (p) t->heap[i].item=p;
    }
}
void topkDigest(unsigned char *digest, robj *o) {
    TopK *t=o->ptr;
    mixDigest(digest,&t->k,sizeof(t->k));
    mixDigest(digest,&t->width,sizeof(t->width));
    mixDigest(digest,&t->depth,sizeof(t->depth));
    mixDigest(digest,&t->decay,sizeof(t->decay));
    mixDigest(digest,t->data,(size_t)t->width*t->depth*sizeof(Bucket));
    for (uint32_t i=0; i<t->k; i++) {
        mixDigest(digest,&t->heap[i].fp,sizeof(t->heap[i].fp));
        mixDigest(digest,&t->heap[i].count,sizeof(t->heap[i].count));
        if (t->heap[i].item) mixDigest(digest,t->heap[i].item,t->heap[i].itemlen);
    }
}
static robj *topkLookup(client *c, int write) {
    robj *o=write ? lookupKeyWrite(c->db,c->argv[1]) : lookupKeyRead(c->db,c->argv[1]);
    if (!o) { addReplyError(c,"TopK: key does not exist"); return NULL; }
    if (checkType(c,o,OBJ_TOPK)) return NULL;
    return o;
}
static void topkModified(client *c, robj *o, size_t oldsize, size_t oldcount) {
    updateKeysizesHist(c->db,OBJ_TOPK,oldcount,topkObjectLength(o));
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db,getKeySlot(c->argv[1]->ptr),o,oldsize,kvobjAllocSize(o));
    keyModified(c,c->db,c->argv[1],o,1);
    notifyKeyspaceEvent(NOTIFY_TOPK,c->cmd->fullname,c->argv[1],c->db->id);
    server.dirty++;
}
void topkReserveCommand(client *c) {
    if (c->argc!=3 && c->argc!=6) { addReplyErrorArity(c); return; }
    if (lookupKeyWrite(c->db,c->argv[1])) { addReplyError(c,"TopK: key already exists"); return; }
    long long dims[3]={0,8,7};
    const char *errors[]={"TopK: invalid k","TopK: invalid width","TopK: invalid depth"};
    for (int i=0; i<(c->argc==6 ? 3 : 1); i++) {
        if (getLongLongFromObject(c->argv[2+i],&dims[i])!=C_OK || dims[i]<1 || dims[i]>UINT32_MAX) {
            addReplyError(c,errors[i]); return;
        }
    }
    double decay=.9;
    if (c->argc==6 && (getDoubleFromObject(c->argv[5],&decay)!=C_OK ||
                       !isfinite(decay) || decay<=0 || decay>1)) {
        addReplyError(c,"TopK: invalid decay value. must be '<= 1' & '> 0'"); return;
    }
    TopK *t=TopK_Create(dims[0],dims[1],dims[2],decay);
    if (!t) { addReplyError(c,"Insufficient memory to create topk data structure"); return; }
    robj *o=createTopkObject(t);
    dbAdd(c->db,c->argv[1],&o);
    topkModified(c,o,server.memory_tracking_enabled ? kvobjAllocSize(o) : 0,0);
    addReply(c,shared.ok);
}
void topkAddCommand(client *c) {
    int incr=!strcasecmp(c->cmd->fullname,"topk.incrby"), step=incr ? 2 : 1;
    if (incr && c->argc%2) { addReplyErrorArity(c); return; }
    robj *o=topkLookup(c,1); if (!o) return;
    TopK *t=o->ptr;
    size_t oldcount=topkObjectLength(o), oldsize=server.memory_tracking_enabled ? kvobjAllocSize(o) : 0;
    int changed=0;
    addReplyArrayLen(c,(c->argc-2)/step);
    for (int i=2; i<c->argc; i+=step) {
        long long n=1;
        if (incr && (getLongLongFromObject(c->argv[i+1],&n)!=C_OK || n<0 || n>100000)) {
            addReplyError(c,"TopK: increment must be an integer greater or equal to 0 and smaller or equal to 100,000");
            continue;
        }
        /* Capture the length before the minimum heap item is potentially evicted. */
        size_t len=t->heap[0].itemlen;
        char *evicted=TopK_Add(t,c->argv[i]->ptr,sdslen(c->argv[i]->ptr),n);
        if (evicted) { addReplyBulkCBuffer(c,evicted,len); zfree(evicted); }
        else addReplyNull(c);
        changed=1;
    }
    if (changed) {
        topkModified(c,o,oldsize,oldcount);
        /* Randomized decay cannot be replayed verbatim. Propagate the exact state.
         * This is O(sketch size); a compact deterministic delta is future work. */
        rio payload;
        createDumpPayload(&payload,o,c->argv[1],c->db->id,DUMP_PAYLOAD_SKIP_KEY_META,0);
        long long expire=getExpire(c->db,c->argv[1]->ptr,o);
        robj *cmd=createStringObject("RESTORE",7);
        robj *ttl=createStringObjectFromLongLong(expire<0 ? 0 : expire);
        robj *data=createStringObject(payload.io.buffer.ptr,sdslen(payload.io.buffer.ptr));
        robj *replace=createStringObject("REPLACE",7), *absolute=createStringObject("ABSTTL",6);
        rewriteClientCommandVector(c,6,cmd,c->argv[1],ttl,data,replace,absolute);
        decrRefCount(cmd); decrRefCount(ttl); decrRefCount(data);
        decrRefCount(replace); decrRefCount(absolute); sdsfree(payload.io.buffer.ptr);
    }
}
void topkQueryCommand(client *c) {
    robj *o=topkLookup(c,0); if (!o) return;
    int count=!strcasecmp(c->cmd->fullname,"topk.count");
    addReplyArrayLen(c,c->argc-2);
    for (int i=2; i<c->argc; i++) {
        long long n=count ? TopK_Count(o->ptr,c->argv[i]->ptr,sdslen(c->argv[i]->ptr)) :
                           TopK_Query(o->ptr,c->argv[i]->ptr,sdslen(c->argv[i]->ptr));
        if (!count && c->resp==3) addReplyBool(c,n); else addReplyLongLong(c,n);
    }
}
void topkListCommand(client *c) {
    if (c->argc>3) { addReplyErrorArity(c); return; }
    int counts=c->argc==3;
    if (counts && (sdslen(c->argv[2]->ptr)!=9 || strcasecmp(c->argv[2]->ptr,"WITHCOUNT"))) {
        addReplyError(c,"TopK: invalid argument"); return;
    }
    robj *o=topkLookup(c,0); if (!o) return;
    TopK *t=o->ptr;
    HeapBucket *list=TopK_List(t);
    size_t n=0; while (n<t->k && list[n].count) n++;
    addReplyArrayLen(c,n*(counts+1));
    for (size_t i=0; i<n; i++) {
        addReplyBulkCBuffer(c,list[i].item,list[i].itemlen);
        if (counts) addReplyLongLong(c,list[i].count);
    }
    zfree(list);
}
void topkInfoCommand(client *c) {
    robj *o=topkLookup(c,0); if (!o) return;
    TopK *t=o->ptr;
    if (c->resp==3) addReplyMapLen(c,4); else addReplyArrayLen(c,8);
    addReplyStatus(c,"k"); addReplyLongLong(c,t->k);
    addReplyStatus(c,"width"); addReplyLongLong(c,t->width);
    addReplyStatus(c,"depth"); addReplyLongLong(c,t->depth);
    addReplyStatus(c,"decay"); addReplyDouble(c,t->decay);
}
uint64_t topkRdbId(void) { return probRdbId("TopK-TYPE",0); }
ssize_t topkRdbSave(rio *rdb, robj *o) {
    TopK *t=o->ptr;
    probIO io={.rdb=rdb};
    addSaved(&io,rdbSaveLen(rdb,topkRdbId()));
    saveUnsigned(&io,t->k); saveUnsigned(&io,t->width); saveUnsigned(&io,t->depth);
    saveDouble(&io,t->decay);
    saveString(&io,(char *)t->data,(size_t)t->width*t->depth*sizeof(Bucket));
    /* Keep the legacy ABI layout, but never persist process pointers/padding. */
    HeapBucket *heap=zcalloc_num(t->k,sizeof(*heap));
    for (uint32_t i=0; i<t->k; i++) {
        heap[i].fp=t->heap[i].fp; heap[i].count=t->heap[i].count; heap[i].itemlen=t->heap[i].itemlen;
    }
    saveString(&io,(char *)heap,(size_t)t->k*sizeof(*heap)); zfree(heap);
    for (uint32_t i=0; i<t->k; i++)
        saveString(&io,t->heap[i].item ? t->heap[i].item : "",
                   t->heap[i].item ? (size_t)t->heap[i].itemlen+1 : 1);
    addSaved(&io,rdbSaveLen(rdb,RDB_MODULE_OPCODE_EOF));
    return io.error ? -1 : io.bytes;
}
robj *topkRdbLoad(rio *rdb, int version) {
    if (version!=0) return NULL;
    probIO io={.rdb=rdb};
    uint64_t k=loadUnsigned(&io), w=loadUnsigned(&io), d=loadUnsigned(&io);
    double decay=loadDouble(&io);
    if (io.error || !k || k>UINT32_MAX || !w || w>UINT32_MAX || !d || d>UINT32_MAX ||
        !isfinite(decay) || decay<=0 || decay>1 || d>SIZE_MAX/w ||
        w*d>SIZE_MAX/sizeof(Bucket) || k>SIZE_MAX/sizeof(HeapBucket)) return NULL;
    TopK *t=zcalloc(sizeof(*t));
    t->k=k; t->width=w; t->depth=d; t->decay=decay;
    size_t len=0;
    t->data=loadString(&io,&len);
    if (io.error || len!=w*d*sizeof(Bucket)) goto error;
    t->heap=loadString(&io,&len);
    if (io.error || len!=k*sizeof(HeapBucket)) {
        zfree(t->heap); t->heap=NULL; goto error;
    }
    for (uint32_t i=0; i<k; i++) t->heap[i].item=NULL;
    for (uint32_t i=0; i<k; i++) {
        char *item=loadString(&io,&len);
        if (io.error || !len || len-1>UINT32_MAX || item[len-1]) { zfree(item); goto error; }
        t->heap[i].itemlen=len-1;
        if (len==1 && !t->heap[i].count) zfree(item);
        else t->heap[i].item=item;
    }
    for (unsigned i=0; i<TOPK_DECAY_LOOKUP_TABLE; i++) t->lookupTable[i]=pow(decay,i);
    if (!loadOpcode(&io,RDB_MODULE_OPCODE_EOF)) goto error;
    return createTopkObject(t);
error:
    TopK_Destroy(t); return NULL;
}
int topkRewriteAof(rio *rdb, robj *key, robj *o, int dbid) {
    return probRewriteAof(rdb,key,o,dbid);
}

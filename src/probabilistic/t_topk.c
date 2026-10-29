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
    /* A nonzero minimum means every heap slot is occupied. Avoid scanning the
     * entire heap twice per update just to maintain the key-size histogram. */
    if (t->heap[0].count) return t->k;
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
static void topkModified(client *c, robj *o, size_t oldsize, size_t oldcount, const char *event) {
    updateKeysizesHist(c->db,OBJ_TOPK,oldcount,topkObjectLength(o));
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db,getKeySlot(c->argv[1]->ptr),o,oldsize,kvobjAllocSize(o));
    keyModified(c,c->db,c->argv[1],o,1);
    notifyKeyspaceEvent(NOTIFY_TOPK,event,c->argv[1],c->db->id);
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
    topkModified(c,o,server.memory_tracking_enabled ? kvobjAllocSize(o) : 0,0,c->cmd->fullname);
    addReply(c,shared.ok);
}
/* Delta format v1: one version byte followed by records containing a little-endian
 * uint32 item length, the item bytes, and (fingerprint, count) uint32 pairs for
 * each row. Row locations follow from the item hash, independent of sketch width.
 * Only successful items are included, in command order. */
static void topkPut32(unsigned char *p, uint32_t n) {
    for (int i=0; i<4; i++) p[i]=(n>>(i*8))&255;
}
static uint32_t topkGet32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
void topkApplyCommand(client *c) {
    const char *event=c->argv[2]->ptr;
    size_t eventlen=sdslen(c->argv[2]->ptr);
    if (!((eventlen==8 && !memcmp(event,"topk.add",8)) ||
          (eventlen==11 && !memcmp(event,"topk.incrby",11)))) {
        addReplyError(c,"TopK: invalid delta operation"); return;
    }
    robj *o=topkLookup(c,1); if (!o) return;
    TopK *t=o->ptr;
    const unsigned char *data=(unsigned char *)c->argv[3]->ptr;
    size_t len=sdslen(c->argv[3]->ptr), rows=(size_t)t->depth*8;
    /* Validate the entire payload before mutating the object. */
    if (len<2 || data[0]!=1) goto invalid;
    for (size_t pos=1; pos<len;) {
        if (len-pos<4) goto invalid;
        size_t itemlen=topkGet32(data+pos); pos+=4;
        if (itemlen>len-pos || rows>len-pos-itemlen) goto invalid;
        pos+=itemlen+rows;
    }
    size_t oldcount=topkObjectLength(o), oldsize=server.memory_tracking_enabled ? kvobjAllocSize(o) : 0;
    Bucket *updates=zmalloc((size_t)t->depth*sizeof(*updates));
    for (size_t pos=1; pos<len;) {
        size_t itemlen=topkGet32(data+pos); pos+=4;
        const char *item=(const char *)data+pos; pos+=itemlen;
        for (uint32_t row=0; row<t->depth; row++,pos+=8) {
            updates[row].fp=topkGet32(data+pos);
            updates[row].count=topkGet32(data+pos+4);
        }
        zfree(TopK_ApplyUpdates(t,item,itemlen,updates));
    }
    zfree(updates);
    topkModified(c,o,oldsize,oldcount,event);
    addReply(c,shared.ok);
    return;
invalid:
    addReplyError(c,"TopK: invalid delta payload");
}
void topkAddCommand(client *c) {
    int incr=!strcasecmp(c->cmd->fullname,"topk.incrby"), step=incr ? 2 : 1;
    if (incr && c->argc%2) { addReplyErrorArity(c); return; }
    robj *o=topkLookup(c,1); if (!o) return;
    TopK *t=o->ptr;
    size_t oldcount=topkObjectLength(o), oldsize=server.memory_tracking_enabled ? kvobjAllocSize(o) : 0;
    int changed=0;
    sds delta=sdsnewlen("\x01",1);
    Bucket *updates=zmalloc((size_t)t->depth*sizeof(*updates));
    addReplyArrayLen(c,(c->argc-2)/step);
    for (int i=2; i<c->argc; i+=step) {
        long long n=1;
        if (incr && (getLongLongFromObject(c->argv[i+1],&n)!=C_OK || n<0 || n>100000)) {
            addReplyError(c,"TopK: increment must be an integer greater or equal to 0 and smaller or equal to 100,000");
            continue;
        }
        /* Capture the length before the minimum heap item is potentially evicted. */
        size_t len=t->heap[0].itemlen;
        size_t itemlen=sdslen(c->argv[i]->ptr);
        char *evicted=TopK_AddWithUpdates(t,c->argv[i]->ptr,itemlen,n,updates);
        size_t pos=sdslen(delta), bytes=4+itemlen+(size_t)t->depth*8;
        delta=sdsMakeRoomFor(delta,bytes);
        unsigned char *record=(unsigned char *)delta+pos;
        topkPut32(record,itemlen); record+=4;
        memcpy(record,c->argv[i]->ptr,itemlen); record+=itemlen;
        for (uint32_t row=0; row<t->depth; row++,record+=8) {
            topkPut32(record,updates[row].fp);
            topkPut32(record+4,updates[row].count);
        }
        sdsIncrLen(delta,bytes);
        if (evicted) { addReplyBulkCBuffer(c,evicted,len); zfree(evicted); }
        else addReplyNull(c);
        changed=1;
    }
    zfree(updates);
    if (changed) {
        topkModified(c,o,oldsize,oldcount,c->cmd->fullname);
        robj *cmd=createStringObject("TOPK._APPLY",11);
        robj *event=createStringObject(c->cmd->fullname,strlen(c->cmd->fullname));
        robj *payload=createObject(OBJ_STRING,delta);
        rewriteClientCommandVector(c,4,cmd,c->argv[1],event,payload);
        decrRefCount(cmd); decrRefCount(event); decrRefCount(payload);
    } else sdsfree(delta);
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
        /* The legacy blob includes a pointer field. Store only a presence marker,
         * so an empty-string item with count zero is distinct from an unused slot. */
        heap[i].item=t->heap[i].item ? (char *)(uintptr_t)1 : NULL;
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
    unsigned char *present=NULL;
    t->k=k; t->width=w; t->depth=d; t->decay=decay;
    size_t len=0;
    t->data=loadString(&io,&len);
    if (io.error || len!=w*d*sizeof(Bucket)) goto error;
    t->heap=loadString(&io,&len);
    if (io.error || len!=k*sizeof(HeapBucket)) {
        zfree(t->heap); t->heap=NULL; goto error;
    }
    present=zmalloc(k);
    for (uint32_t i=0; i<k; i++) {
        present[i]=t->heap[i].item!=NULL;
        t->heap[i].item=NULL;
    }
    for (uint32_t i=0; i<k; i++) {
        char *item=loadString(&io,&len);
        if (io.error || !len || len-1>UINT32_MAX || item[len-1]) { zfree(item); goto error; }
        t->heap[i].itemlen=len-1;
        if (len==1 && !t->heap[i].count && !present[i]) zfree(item);
        else t->heap[i].item=item;
    }
    for (unsigned i=0; i<TOPK_DECAY_LOOKUP_TABLE; i++) t->lookupTable[i]=pow(decay,i);
    if (!loadOpcode(&io,RDB_MODULE_OPCODE_EOF)) goto error;
    zfree(present);
    return createTopkObject(t);
error:
    zfree(present); TopK_Destroy(t); return NULL;
}
int topkRewriteAof(rio *rdb, robj *key, robj *o, int dbid) {
    return probRewriteAof(rdb,key,o,dbid);
}

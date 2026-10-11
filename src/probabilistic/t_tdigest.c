/* Native t-digest commands and persistence.
 * Copyright (c) 2026-Present, Redis Ltd.; RSALv2/SSPLv1/AGPLv3. */
#include "server.h"
#include "tdigest.h"
#include "probabilistic.h"
#include <math.h>

robj *createTdigestObject(void *p) { return createObject(OBJ_TDIGEST,p); }
void freeTdigestObject(robj *o) { td_free(o->ptr); }
size_t tdigestObjectLength(robj *o) {
    uint64_t n=td_size(o->ptr); return n>SIZE_MAX ? SIZE_MAX : n;
}
size_t tdigestFreeEffort(robj *o) { UNUSED(o); return 3; }
size_t tdigestAllocSize(robj *o) {
    td_histogram_t *t=o->ptr;
    return zmalloc_size(t)+zmalloc_size(t->nodes_mean)+zmalloc_size(t->nodes_weight);
}
void tdigestDismiss(robj *o) {
    td_histogram_t *t=o->ptr;
    dismissMemory(t->nodes_mean,(size_t)t->cap*sizeof(double));
    dismissMemory(t->nodes_weight,(size_t)t->cap*sizeof(long long));
}
static td_histogram_t *tdClone(td_histogram_t *t) {
    td_histogram_t *copy=zmalloc(sizeof(*copy)); *copy=*t;
    size_t n=(size_t)t->cap;
    copy->nodes_mean=zmalloc(n*sizeof(double));
    copy->nodes_weight=zmalloc(n*sizeof(long long));
    memcpy(copy->nodes_mean,t->nodes_mean,n*sizeof(double));
    memcpy(copy->nodes_weight,t->nodes_weight,n*sizeof(long long));
    return copy;
}
robj *tdigestDup(robj *o) { return createTdigestObject(tdClone(o->ptr)); }
void tdigestDefrag(robj *o, void *(*defrag)(void *)) {
    void *p=defrag(o->ptr); if (p) o->ptr=p;
    td_histogram_t *t=o->ptr;
    p=defrag(t->nodes_mean); if (p) t->nodes_mean=p;
    p=defrag(t->nodes_weight); if (p) t->nodes_weight=p;
}
void tdigestDigest(unsigned char *digest, robj *o) {
    td_histogram_t *t=o->ptr;
    mixDigest(digest,&t->compression,sizeof(t->compression));
    mixDigest(digest,&t->min,sizeof(t->min)); mixDigest(digest,&t->max,sizeof(t->max));
    mixDigest(digest,&t->merged_nodes,sizeof(t->merged_nodes));
    mixDigest(digest,&t->unmerged_nodes,sizeof(t->unmerged_nodes));
    mixDigest(digest,&t->total_compressions,sizeof(t->total_compressions));
    mixDigest(digest,t->nodes_mean,(t->merged_nodes+t->unmerged_nodes)*sizeof(double));
    mixDigest(digest,t->nodes_weight,(t->merged_nodes+t->unmerged_nodes)*sizeof(long long));
}
static robj *tdLookup(client *c, robj *key, int write) {
    robj *o=write ? lookupKeyWrite(c->db,key) : lookupKeyRead(c->db,key);
    if (!o) { addReplyError(c,"T-Digest: key does not exist"); return NULL; }
    if (checkType(c,o,OBJ_TDIGEST)) return NULL;
    return o;
}
static int tdToken(robj *o, const char *s) {
    return sdslen(o->ptr)==strlen(s) && !strcasecmp(o->ptr,s);
}
static int tdCompression(client *c, robj *arg, long long *n) {
    if (getLongLongFromObjectOrReply(c,arg,n,"T-Digest: error parsing compression parameter")!=C_OK) return C_ERR;
    if (*n<=0) { addReplyError(c,"T-Digest: compression parameter needs to be a positive integer"); return C_ERR; }
    return C_OK;
}
static void tdModified(client *c, robj *o, size_t oldcount, size_t oldsize) {
    updateKeysizesHist(c->db,OBJ_TDIGEST,oldcount,tdigestObjectLength(o));
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db,getKeySlot(c->argv[1]->ptr),o,oldsize,kvobjAllocSize(o));
    keyModified(c,c->db,c->argv[1],o,1);
    notifyKeyspaceEvent(NOTIFY_TDIGEST,c->cmd->fullname,c->argv[1],c->db->id);
    server.dirty++;
}
void tdigestCreateCommand(client *c) {
    if (c->argc!=2 && c->argc!=4) { addReplyErrorArity(c); return; }
    robj *o=lookupKeyWrite(c->db,c->argv[1]);
    if (o) { if (!checkType(c,o,OBJ_TDIGEST)) addReplyError(c,"T-Digest: key already exists"); return; }
    long long compression=100;
    if (c->argc==4) {
        if (!tdToken(c->argv[2],"COMPRESSION")) { addReplyError(c,"T-Digest: wrong keyword"); return; }
        if (tdCompression(c,c->argv[3],&compression)!=C_OK) return;
    }
    td_histogram_t *t=td_new(compression);
    if (!t) { addReplyError(c,"T-Digest: allocation failed"); return; }
    o=createTdigestObject(t); dbAdd(c->db,c->argv[1],&o);
    tdModified(c,o,0,server.memory_tracking_enabled ? kvobjAllocSize(o) : 0);
    addReply(c,shared.ok);
}
void tdigestResetCommand(client *c) {
    robj *o=tdLookup(c,c->argv[1],1); if (!o) return;
    size_t oldcount=tdigestObjectLength(o), oldsize=server.memory_tracking_enabled ? kvobjAllocSize(o) : 0;
    td_reset(o->ptr); tdModified(c,o,oldcount,oldsize); addReply(c,shared.ok);
}
void tdigestAddCommand(client *c) {
    robj *o=tdLookup(c,c->argv[1],1); if (!o) return;
    int n=c->argc-2;
    double *values=zmalloc((size_t)n*sizeof(double));
    for (int i=0; i<n; i++) {
        if (getDoubleFromObject(c->argv[i+2],&values[i])!=C_OK || isnan(values[i])) {
            addReplyError(c,"T-Digest: error parsing val parameter"); zfree(values); return;
        }
        if (!isfinite(values[i])) {
            addReplyError(c,"T-Digest: val parameter needs to be a finite number"); zfree(values); return;
        }
    }
    td_histogram_t *t=o->ptr;
    if (td_size(t)>LLONG_MAX-n) { addReplyError(c,"T-Digest: overflow detected"); zfree(values); return; }
    size_t oldcount=tdigestObjectLength(o), oldsize=server.memory_tracking_enabled ? kvobjAllocSize(o) : 0;
    /* Work on a copy: overflow must not leave an unpropagated partial mutation. */
    td_histogram_t *copy=tdClone(t);
    for (int i=0; i<n; i++) if (td_add(copy,values[i],1)) {
        td_free(copy); zfree(values); addReplyError(c,"T-Digest: overflow detected"); return;
    }
    zfree(values); o->ptr=copy; td_free(t);
    tdModified(c,o,oldcount,oldsize); addReply(c,shared.ok);
}
void tdigestMergeCommand(client *c) {
    robj *o=lookupKeyWrite(c->db,c->argv[1]);
    if (o && checkType(c,o,OBJ_TDIGEST)) return;
    long long n;
    if (getLongLongFromObjectOrReply(c,c->argv[2],&n,"T-Digest: error parsing numkeys")!=C_OK) return;
    if (n<=0) { addReplyError(c,"T-Digest: numkeys needs to be a positive integer"); return; }
    if (n>c->argc-3) { addReplyErrorArity(c); return; }
    long long compression=o ? ((td_histogram_t *)o->ptr)->compression : 0;
    int override=0, explicit_compression=0;
    for (int i=3+n; i<c->argc; i++) {
        if (tdToken(c->argv[i],"OVERRIDE")) override=1;
        else if (tdToken(c->argv[i],"COMPRESSION")) {
            if (++i==c->argc) { addReplyErrorArity(c); return; }
            if (tdCompression(c,c->argv[i],&compression)!=C_OK) return;
            explicit_compression=1;
        } else { addReplyError(c,"T-Digest: wrong keyword"); return; }
    }
    int use_max=!explicit_compression && (!o || override);
    if (use_max) compression=0;
    td_histogram_t **sources=zmalloc((size_t)n*sizeof(*sources));
    for (int i=0; i<n; i++) {
        robj *src=tdLookup(c,c->argv[3+i],0);
        if (!src) { zfree(sources); return; }
        sources[i]=src->ptr;
        if (use_max && sources[i]->compression>compression) compression=sources[i]->compression;
    }
    td_histogram_t *dest=td_new(compression);
    if (!dest) { addReplyError(c,"T-Digest: allocation of destination digest failed"); zfree(sources); return; }
    int failed=0;
    for (int i=(o && !override) ? -1 : 0; i<n; i++) {
        td_histogram_t *copy=tdClone(i<0 ? o->ptr : sources[i]);
        failed=td_merge(dest,copy); td_free(copy);
        if (failed) break;
    }
    zfree(sources);
    if (failed) { td_free(dest); addReplyError(c,"T-Digest: overflow detected"); return; }
    size_t oldcount=o ? tdigestObjectLength(o) : 0;
    size_t oldsize=o && server.memory_tracking_enabled ? kvobjAllocSize(o) : 0;
    if (o) { td_free(o->ptr); o->ptr=dest; }
    else {
        o=createTdigestObject(dest); dbAdd(c->db,c->argv[1],&o);
        oldsize=server.memory_tracking_enabled ? kvobjAllocSize(o) : 0;
    }
    tdModified(c,o,oldcount,oldsize); addReply(c,shared.ok);
}
void tdigestInfoCommand(client *c) {
    robj *o=tdLookup(c,c->argv[1],0); if (!o) return;
    td_histogram_t *t=o->ptr;
    const char *names[]={"Compression","Capacity","Merged nodes","Unmerged nodes","Merged weight",
                        "Unmerged weight","Observations","Total compressions","Memory usage"};
    long long values[]={t->compression,t->cap,t->merged_nodes,t->unmerged_nodes,t->merged_weight,
                        t->unmerged_weight,td_size(t),t->total_compressions,
                        sizeof(*t)+(size_t)t->cap*(sizeof(double)+sizeof(long long))};
    if (c->resp==3) addReplyMapLen(c,9); else addReplyArrayLen(c,18);
    for (int i=0; i<9; i++) { addReplyStatus(c,names[i]); addReplyLongLong(c,values[i]); }
}
void tdigestQueryCommand(client *c) {
    robj *o=tdLookup(c,c->argv[1],0); if (!o) return;
    const char *cmd=c->cmd->fullname;
    int min=!strcasecmp(cmd,"tdigest.min"), max=!strcasecmp(cmd,"tdigest.max");
    if (min || max) {
        addReplyDouble(c,td_size(o->ptr)==0 ? NAN : min ? td_min(o->ptr) : td_max(o->ptr));
        return;
    }
    int trimmed=!strcasecmp(cmd,"tdigest.trimmed_mean");
    int quantile=!strcasecmp(cmd,"tdigest.quantile"), cdf=!strcasecmp(cmd,"tdigest.cdf");
    int byrank=!strcasecmp(cmd,"tdigest.byrank") || !strcasecmp(cmd,"tdigest.byrevrank");
    int reverse=!strcasecmp(cmd,"tdigest.revrank") || !strcasecmp(cmd,"tdigest.byrevrank");
    int n=c->argc-2;
    double *values=zmalloc((size_t)n*sizeof(double));
    for (int i=0; i<n; i++) {
        if (byrank) {
            long long rank;
            if (getLongLongFromObjectOrReply(c,c->argv[i+2],&rank,"T-Digest: error parsing rank")!=C_OK) goto done;
            if (rank<0) { addReplyError(c,"T-Digest: rank needs to be non negative"); goto done; }
            values[i]=rank;
        } else if (getDoubleFromObject(c->argv[i+2],&values[i])!=C_OK || isnan(values[i])) {
            addReplyErrorFormat(c,"T-Digest: error parsing %s",quantile ? "quantile" : cdf ? "cdf" :
                                trimmed ? (i ? "high_cut_percentile" : "low_cut_percentile") : "value");
            goto done;
        }
        if (quantile && (values[i]<0 || values[i]>1)) {
            addReplyError(c,"T-Digest: quantile should be in [0,1]"); goto done;
        }
    }
    if (trimmed) {
        if (values[0]<0 || values[0]>1 || values[1]<0 || values[1]>1) {
            addReplyError(c,"T-Digest: low_cut_percentile and high_cut_percentile should be in [0,1]"); goto done;
        }
        if (values[0]>=values[1]) {
            addReplyError(c,"T-Digest: low_cut_percentile should be lower than high_cut_percentile"); goto done;
        }
    }
    /* Query compression is scratch work, not a hidden write to primary state. */
    td_histogram_t *t=tdClone(o->ptr);
    double size=td_size(t), low=td_min(t), high=td_max(t);
    if (trimmed) addReplyDouble(c,td_trimmed_mean(t,values[0],values[1]));
    else {
        addReplyArrayLen(c,n);
        for (int i=0; i<n; i++) {
            double v=values[i], result;
            if (quantile) result=td_quantile(t,v);
            else if (cdf) result=td_cdf(t,v);
            else if (byrank) result=!size ? NAN : !v ? (reverse ? high : low) :
                v>=size ? (reverse ? -INFINITY : INFINITY) : td_quantile(t,(reverse ? size-v-1 : v)/size);
            else {
                if (!size) result=-2;
                else if (v<low) result=reverse ? size : -1;
                else if (v>high) result=reverse ? -1 : size;
                else {
                    double rank=td_cdf(t,v)*size;
                    result=reverse ? round(size-round(rank)) : ceil(rank-.5);
                }
                addReplyLongLong(c,result>=(double)LLONG_MAX ? LLONG_MAX : (long long)result);
                continue;
            }
            addReplyDouble(c,result);
        }
    }
    td_free(t);
done:
    zfree(values);
}
/* Load legacy version 0 unchanged. Version 1 preserves unmerged nodes and exact
 * integer weights, so saves/replication do not alter the primary's sketch. */
uint64_t tdigestRdbId(void) { return probRdbId("TDIS-TYPE",1); }
ssize_t tdigestRdbSave(rio *rdb, robj *o) {
    td_histogram_t *t=o->ptr;
    probIO io={.rdb=rdb};
    addSaved(&io,rdbSaveLen(rdb,tdigestRdbId()));
    saveDouble(&io,t->compression); saveDouble(&io,t->min); saveDouble(&io,t->max);
    saveUnsigned(&io,t->cap); saveUnsigned(&io,t->merged_nodes); saveUnsigned(&io,t->unmerged_nodes);
    saveUnsigned(&io,t->total_compressions);
    saveUnsigned(&io,t->merged_weight); saveUnsigned(&io,t->unmerged_weight);
    int n=t->merged_nodes+t->unmerged_nodes;
    for (int i=0; i<n; i++) saveDouble(&io,t->nodes_mean[i]);
    for (int i=0; i<n; i++) saveUnsigned(&io,t->nodes_weight[i]);
    addSaved(&io,rdbSaveLen(rdb,RDB_MODULE_OPCODE_EOF));
    return io.error ? -1 : io.bytes;
}
static int tdLoadWeight(probIO *io, int version, long long *out) {
    if (version) {
        uint64_t n=loadUnsigned(io); if (io->error || n>LLONG_MAX) return C_ERR;
        *out=n;
    } else {
        double n=loadDouble(io);
        if (io->error || !isfinite(n) || n<0 || n>=(double)LLONG_MAX || trunc(n)!=n) return C_ERR;
        *out=n;
    }
    return C_OK;
}
robj *tdigestRdbLoad(rio *rdb, int version) {
    if (version>1) return NULL;
    probIO io={.rdb=rdb};
    double compression=loadDouble(&io);
    if (io.error) return NULL;
    td_histogram_t *t=td_new(compression);
    if (!t) return NULL;
    t->min=loadDouble(&io); t->max=loadDouble(&io);
    uint64_t cap=loadUnsigned(&io), merged=loadUnsigned(&io), unmerged=loadUnsigned(&io), compressions=loadUnsigned(&io);
    if (io.error || !isfinite(t->min) || !isfinite(t->max) || cap!=(uint64_t)t->cap ||
        merged>cap || unmerged>cap-merged || compressions>=LLONG_MAX || (!version && unmerged)) goto error;
    t->merged_nodes=merged; t->unmerged_nodes=unmerged; t->total_compressions=compressions;
    if (tdLoadWeight(&io,version,&t->merged_weight)!=C_OK ||
        tdLoadWeight(&io,version,&t->unmerged_weight)!=C_OK ||
        t->merged_weight>LLONG_MAX-t->unmerged_weight || (!version && t->unmerged_weight)) goto error;
    int n=merged+unmerged;
    for (int i=0; i<n; i++) {
        double mean=loadDouble(&io);
        if (io.error || !isfinite(mean) || mean<t->min || mean>t->max ||
            (i>0 && (uint64_t)i<merged && mean<t->nodes_mean[i-1])) goto error;
        t->nodes_mean[i]=mean;
    }
    long long sums[2]={0,0};
    for (int i=0; i<n; i++) {
        long long weight;
        int which=(uint64_t)i>=merged;
        if (tdLoadWeight(&io,version,&weight)!=C_OK || !weight || sums[which]>LLONG_MAX-weight) goto error;
        sums[which]+=weight; t->nodes_weight[i]=weight;
    }
    if (sums[0]!=t->merged_weight || sums[1]!=t->unmerged_weight ||
        (n && t->min>t->max) || !loadOpcode(&io,RDB_MODULE_OPCODE_EOF)) goto error;
    return createTdigestObject(t);
error:
    td_free(t); return NULL;
}
int tdigestRewriteAof(rio *rdb, robj *key, robj *o, int dbid) {
    return probRewriteAof(rdb,key,o,dbid);
}

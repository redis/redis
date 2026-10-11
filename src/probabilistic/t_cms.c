/* Native Count-Min Sketch commands and persistence.
 * Copyright (c) 2026-Present, Redis Ltd.; RSALv2/SSPLv1/AGPLv3.
 * Algorithm derived from RedisBloom v8.11.81, without module APIs. */
#include "server.h"
#include "cms.h"
#include "probabilistic.h"
#include <math.h>

robj *createCmsObject(void *value) { return createObject(OBJ_CMS, value); }
void freeCmsObject(robj *o) { CMS_Destroy(o->ptr); }
size_t cmsObjectLength(robj *o) {
    uint64_t count = ((CMSketch *)o->ptr)->counter;
    return count > SIZE_MAX ? SIZE_MAX : count;
}
size_t cmsFreeEffort(robj *o) { UNUSED(o); return 2; }
size_t cmsAllocSize(robj *o) {
    CMSketch *cms = o->ptr;
    return zmalloc_size(cms) + zmalloc_size(cms->array);
}
void cmsDismiss(robj *o) {
    CMSketch *cms = o->ptr;
    dismissMemory(cms->array, cms->width * cms->depth * cms->cellSize);
}
robj *cmsDup(robj *o) {
    CMSketch *cms = o->ptr;
    CMSketch *copy = zmalloc(sizeof(*copy));
    *copy = *cms;
    size_t bytes = cms->width * cms->depth * cms->cellSize;
    copy->array = zmalloc(bytes);
    memcpy(copy->array, cms->array, bytes);
    return createCmsObject(copy);
}
void cmsDefrag(robj *o, void *(*defrag)(void *)) {
    void *p = defrag(o->ptr);
    if (p) o->ptr = p;
    CMSketch *cms = o->ptr;
    p = defrag(cms->array);
    if (p) cms->array = p;
}
void cmsDigest(unsigned char *digest, robj *o) {
    CMSketch *cms = o->ptr;
    mixDigest(digest, &cms->width, sizeof(cms->width));
    mixDigest(digest, &cms->depth, sizeof(cms->depth));
    mixDigest(digest, &cms->counter, sizeof(cms->counter));
    mixDigest(digest, &cms->cellSize, sizeof(cms->cellSize));
    mixDigest(digest, cms->array, cms->width * cms->depth * cms->cellSize);
}
static robj *cmsLookup(client *c, robj *key, int write) {
    robj *o = write ? lookupKeyWrite(c->db, key) : lookupKeyRead(c->db, key);
    if (!o) { addReplyError(c, "CMS: key does not exist"); return NULL; }
    if (checkType(c, o, OBJ_CMS)) return NULL;
    return o;
}
static void cmsModified(client *c, robj *o, const char *event, size_t oldcount) {
    updateKeysizesHist(c->db, OBJ_CMS, oldcount, cmsObjectLength(o));
    keyModified(c, c->db, c->argv[1], o, 1);
    notifyKeyspaceEvent(NOTIFY_CMS, (char *)event, c->argv[1], c->db->id);
    server.dirty++;
}
void cmsCreateCommand(client *c) {
    if (c->argc != 4 && c->argc != 6) { addReplyErrorArity(c); return; }
    if (lookupKeyWrite(c->db, c->argv[1])) { addReplyError(c, "CMS: key already exists"); return; }
    long long width, depth, cell = CMS_DEFAULT_CELL_SIZE;
    if (!strcasecmp(c->cmd->fullname, "cms.initbydim")) {
        if (getLongLongFromObject(c->argv[2], &width) != C_OK || width < 1) {
            addReplyError(c, "CMS: invalid width"); return;
        }
        if (getLongLongFromObject(c->argv[3], &depth) != C_OK || depth < 1) {
            addReplyError(c, "CMS: invalid depth"); return;
        }
    } else {
        double error, prob;
        if (getDoubleFromObject(c->argv[2], &error) != C_OK || !isfinite(error) || error <= 0 || error >= 1) {
            addReplyError(c, "CMS: invalid overestimation value"); return;
        }
        if (getDoubleFromObject(c->argv[3], &prob) != C_OK || !isfinite(prob) || prob <= 0 || prob >= 1) {
            addReplyError(c, "CMS: invalid prob value"); return;
        }
        double w = ceil(2 / error), d = ceil(log10f(prob) / log10f(0.5));
        if (!isfinite(w) || !isfinite(d) || w >= (double)LLONG_MAX || d >= (double)LLONG_MAX) {
            addReplyError(c, "CMS: invalid init arguments"); return;
        }
        width = w; depth = d;
    }
    if (width < 1 || depth < 1 || (uint64_t)width > SIZE_MAX / (uint64_t)depth) {
        addReplyError(c, "CMS: invalid init arguments"); return;
    }
    if (c->argc == 6) {
        if (sdslen(c->argv[4]->ptr) != 9 || strcasecmp(c->argv[4]->ptr, "CELL_SIZE")) {
            addReplyError(c, "CMS: unknown argument"); return;
        }
        if (getLongLongFromObject(c->argv[5], &cell) != C_OK || !CMS_IS_VALID_CELL_SIZE(cell)) {
            addReplyError(c, "CMS: CELL_SIZE must be 1, 2, 4 or 8"); return;
        }
    }
    CMSketch *cms = NewCMSketch(width, depth, cell);
    if (!cms) { addReplyError(c, "CMS: Insufficient memory to create the key"); return; }
    robj *o = createCmsObject(cms);
    dbAdd(c->db, c->argv[1], &o);
    cmsModified(c, o, c->cmd->fullname, 0);
    addReply(c, shared.ok);
}
void cmsIncrbyCommand(client *c) {
    if (c->argc % 2) { addReplyErrorArity(c); return; }
    robj *o = cmsLookup(c, c->argv[1], 1);
    if (!o) return;
    int n = (c->argc - 2) / 2;
    long long *values = zmalloc(n * sizeof(*values));
    for (int i = 0; i < n; i++) {
        if (getLongLongFromObject(c->argv[3 + i*2], &values[i]) != C_OK) {
            addReplyError(c, "CMS: Cannot parse number"); zfree(values); return;
        }
        if (values[i] == LLONG_MIN) {
            addReplyError(c, "CMS: invalid increment"); zfree(values); return;
        }
    }
    CMSketch *cms = o->ptr;
    size_t oldcount = cmsObjectLength(o);
    int changed = 0;
    addReplyArrayLen(c, n);
    for (int i = 0; i < n; i++) {
        uint64_t count;
        sds item = c->argv[2 + i*2]->ptr;
        CMSStatus status = CMS_IncrBy(cms, item, sdslen(item), values[i], &count);
        if (status == CMS_STATUS_OK) { addReplyLongLong(c, count); changed |= values[i] != 0; }
        else addReplyError(c, status == CMS_STATUS_OVERFLOW ? "CMS: INCRBY overflow" : "CMS: INCRBY underflow");
    }
    zfree(values);
    if (changed) cmsModified(c, o, "cms.incrby", oldcount);
}
void cmsQueryCommand(client *c) {
    robj *o = cmsLookup(c, c->argv[1], 0);
    if (!o) return;
    addReplyArrayLen(c, c->argc - 2);
    for (int i = 2; i < c->argc; i++)
        addReplyLongLong(c, CMS_Query(o->ptr, c->argv[i]->ptr, sdslen(c->argv[i]->ptr)));
}
void cmsInfoCommand(client *c) {
    robj *o = cmsLookup(c, c->argv[1], 0);
    if (!o) return;
    CMSketch *cms = o->ptr;
    if (c->resp == 3) addReplyMapLen(c, 4); else addReplyArrayLen(c, 8);
    addReplyStatus(c, "width"); addReplyLongLong(c, cms->width);
    addReplyStatus(c, "depth"); addReplyLongLong(c, cms->depth);
    addReplyStatus(c, "count"); addReplyLongLong(c, cms->counter);
    addReplyStatus(c, "cell_size"); addReplyLongLong(c, cms->cellSize);
}
void cmsMergeCommand(client *c) {
    robj *o = cmsLookup(c, c->argv[1], 1);
    if (!o) return;
    long long n;
    if (getLongLongFromObjectOrReply(c, c->argv[2], &n, "CMS: invalid numkeys") != C_OK) return;
    if (n <= 0) { addReplyError(c, "CMS: Number of keys must be positive"); return; }
    if (n > c->argc - 3) { addReplyError(c, "CMS: wrong number of keys"); return; }
    int weighted = c->argc != n + 3;
    if (weighted && (c->argc != n * 2 + 4 || sdslen(c->argv[n+3]->ptr) != 7 ||
                     strcasecmp(c->argv[n+3]->ptr, "WEIGHTS"))) {
        addReplyError(c, "CMS: wrong number of keys/weights"); return;
    }
    CMSketch *dest = o->ptr;
    const CMSketch **sources = zmalloc(n * sizeof(*sources));
    long long *weights = zmalloc(n * sizeof(*weights));
    for (int i = 0; i < n; i++) {
        weights[i] = 1;
        if (weighted && getLongLongFromObjectOrReply(c, c->argv[n+4+i], &weights[i],
                                                     "CMS: invalid weight value") != C_OK) goto done;
        robj *src = cmsLookup(c, c->argv[3+i], 0);
        if (!src) goto done;
        sources[i] = src->ptr;
        if (sources[i]->width != dest->width || sources[i]->depth != dest->depth) {
            addReplyError(c, "CMS: width/depth is not equal"); goto done;
        }
        if (sources[i]->cellSize != dest->cellSize) {
            addReplyError(c, "CMS: cell size is not equal"); goto done;
        }
    }
    size_t oldcount = cmsObjectLength(o);
    if (CMS_Merge(dest, n, sources, weights)) { addReplyError(c, "CMS: MERGE overflow"); goto done; }
    cmsModified(c, o, "cms.merge", oldcount);
    addReply(c, shared.ok);
done:
    zfree(sources); zfree(weights);
}
uint64_t cmsRdbId(void) { return probRdbId("CMSk-TYPE", 1); }
ssize_t cmsRdbSave(rio *rdb, robj *o) {
    CMSketch *cms = o->ptr;
    probIO io = {.rdb = rdb};
    addSaved(&io, rdbSaveLen(rdb, cmsRdbId()));
    saveUnsigned(&io, cms->width); saveUnsigned(&io, cms->depth);
    saveUnsigned(&io, cms->counter); saveUnsigned(&io, cms->cellSize);
    saveString(&io, cms->array, cms->width * cms->depth * cms->cellSize);
    addSaved(&io, rdbSaveLen(rdb, RDB_MODULE_OPCODE_EOF));
    return io.error ? -1 : io.bytes;
}
robj *cmsRdbLoad(rio *rdb, int version) {
    if (version > 1) return NULL;
    probIO io = {.rdb = rdb};
    uint64_t width = loadUnsigned(&io), depth = loadUnsigned(&io), count = loadUnsigned(&io);
    uint64_t cell = version ? loadUnsigned(&io) : 4;
    if (io.error || !width || !depth || !CMS_IS_VALID_CELL_SIZE(cell) ||
        width > SIZE_MAX / depth || width * depth > SIZE_MAX / cell ||
        count > INT64_MAX) return NULL;
    size_t len = 0;
    void *array = loadString(&io, &len);
    if (io.error || !array || len != width * depth * cell || !loadOpcode(&io, RDB_MODULE_OPCODE_EOF)) {
        zfree(array); return NULL;
    }
    CMSketch *cms = zmalloc(sizeof(*cms));
    *cms = (CMSketch){.width=width, .depth=depth, .counter=count, .cellSize=cell, .array=array};
    if (CMS_ValidateLoaded(cms)) { CMS_Destroy(cms); return NULL; }
    return createCmsObject(cms);
}
int cmsRewriteAof(rio *rdb, robj *key, robj *o, int dbid) {
    return probRewriteAof(rdb, key, o, dbid);
}

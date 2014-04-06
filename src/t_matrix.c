#include "redis.h"

void addReplyMatrixShape(redisClient *c, matrix *matrix) {
    addReplyMultiBulkLen(c,matrix->dims + 1);
    addReplyBulkLongLong(c,matrix->dims);
    for (int i = 0; i < matrix->dims; i++) {
        addReplyBulkLongLong(c,matrix->shape[i]);
    }
}

void addReplyMatrixContent(redisClient *c, matrix *m) {
    long long i, size = 1, reply_length = 1 + m->dims;

    for (i = 0; i < m->dims; i++) {
        size *= m->shape[i];
    }

    reply_length += size;

    addReplyMultiBulkLen(c,reply_length);
    addReplyBulkLongLong(c,m->dims);

    for (i = 0; i < m->dims; i++) {
        addReplyBulkLongLong(c,m->shape[i]);
    }

    for (i = 0; i < size; i++) {
        addReplyBulkDouble(c,m->values[i]->value);
    }
}

void createMatrixGenericCommand(redisClient *c, double value, int stride) {
  long long dims = c->argc - 2;
  long long shape[dims];
  int i;

  for (i = 0; i < dims; i++) {
    getLongLongFromObjectOrReply(c,c->argv[i + 2],&shape[i],NULL);
  }

  robj *xobj = lookupKeyWrite(c->db,c->argv[1]);

  if (xobj && xobj->type != REDIS_MATRIX) {
    addReply(c,shared.wrongtypeerr);
    return;
  }

  if (!xobj) {
    xobj = createMatrixObject(dims,shape);
    dbAdd(c->db,c->argv[1],xobj);
    incrRefCount(xobj);
  }

  matrix *m = xobj->ptr;

  matrixSetValues(m,value,stride);
  addReplyMatrixShape(c,m);
}

void xgetCommand(redisClient *c) {
    robj *xobj = lookupKeyWrite(c->db,c->argv[1]);
    long long dims = 0;
    dims = (long long)c->argc - 2;
    long long index[dims];

    if (!xobj) {
        addReply(c,shared.nokeyerr);
        return;
    }

    if (xobj && xobj->type != REDIS_MATRIX) {
        addReply(c,shared.wrongtypeerr);
        return;
    }

    matrix *m = xobj->ptr;

    if (dims > m->dims) {
        addReply(c,shared.outofrangeerr);
        return;
    }

    for (int j = 0; j < dims; j++) {
        getLongLongFromObjectOrReply(c,c->argv[j+2],&index[j],NULL);
    }

    matrix *sub = matrixSlice(m, dims, index);

    addReplyMatrixContent(c,sub);
    matrixFree(sub);
}

void xsetCommand(redisClient *c) {
    long long dims = c->argc - 3;
    long long index[dims], shape[dims];
    double value;

    getDoubleFromObjectOrReply(c,c->argv[c->argc-1],&value,NULL);

    for (int j = 0; j < dims; j++) {
        getLongLongFromObjectOrReply(c,c->argv[j+2],&index[j],NULL);
        shape[j] = index[j]+1;
    }

    robj *xobj = lookupKeyWrite(c->db,c->argv[1]);

    if (xobj && xobj->type != REDIS_MATRIX) {
        addReply(c,shared.wrongtypeerr);
        return;
    }

    if (!xobj) {
        xobj = createMatrixObject(dims,shape);
        dbAdd(c->db,c->argv[1],xobj);
        incrRefCount(xobj);
    }

    matrix *m = xobj->ptr;

    if (dims > m->dims) {
        addReply(c,shared.outofrangeerr);
        return;
    }

    matrix *sub = matrixSlice(m, dims, index);

    matrixSetValues(sub,value,1);

    notifyKeyspaceEvent(REDIS_NOTIFY_MATRIX,"xset",c->argv[1],c->db->id);
    server.dirty += 1;

    addReplyMatrixShape(c,m);
    matrixFree(sub);
}

void xzerosCommand(redisClient *c) {
  createMatrixGenericCommand(c,0,1);
}

void xonesCommand(redisClient *c) {
  createMatrixGenericCommand(c,1,1);
}

void xeyeCommand(redisClient *c) {
  long long size;
  getLongLongFromObjectOrReply(c,c->argv[2],&size,NULL);

  createMatrixGenericCommand(c,1,size + 1);
}

#include "matrix.h"
#include "zmalloc.h"
#include <stdio.h>
#include <string.h>

long long reshapedIndex(matrix *m, long long index[]) {
    long long idx = 0;

    for (int i = 0; i < m->dims; i++) {
        idx += index[i];

        if (i < m->dims-1)
            idx *= m->shape[i+1];
    }
    return idx;
}

void matrixPrint(matrix *m) {
    long long i;
    printf("dims:\t%lld\n", m->dims);

    printf("shape:\t[ ");
    for (i = 0; i<m->dims; i++) {
        printf("%lld ", m->shape[i]);
    }
    printf("]\n");

    printf("values:\t[ ");
    for (i = 0; i < m->size; i++) {
        printf("%f ", m->values[i]->value);
    }
    printf("]\n");
}

matrix *matrixZero(long long dims, long long shape[]) {
    int i;
    matrix *matrix = matrixCreate(dims, shape);

    for (i = 0; i < matrix->size; i++) {
        matrix->values[i] = zmalloc(sizeof(scalar));
        matrix->values[i]->value = 0.0;
        matrix->values[i]->reference_count = 1;
    }
    return matrix;
}

matrix *matrixCreate(long long dims, long long shape[]) {
    struct matrix *matrix;
    int i;

    if ((matrix = zmalloc(sizeof(*matrix))) == NULL)
        return NULL;

    matrix->dims = dims;

    if ((matrix->shape = zmalloc(dims * sizeof(long long))) == NULL)
        return NULL;
    memcpy(matrix->shape, shape, dims * sizeof(long long));

    matrix->size = 1;

    for (i = 0; i < dims; i++) matrix->size *= shape[i];

    if ((matrix->values = zmalloc(matrix->size * sizeof(scalar *))) == NULL)
        return NULL;

    return matrix;
}

matrix *matrixSlice(matrix *m, long long dims, long long *index) {
    long long i, j, begin = 1, end = 0, stride = 1, newdims = 0;
    long long beg_idx[dims], end_idx[dims], newshape[dims];
    matrix *sub;

    for (i = 0; i < dims; i++) {
        if (index[i] == -1) {
            newshape[newdims++] = m->shape[i];
            beg_idx[i] = 0;
            end_idx[i] = m->shape[i] - 1;
        } else {
            stride += i * (m->shape[i] - 1);
            beg_idx[i] = index[i];
            end_idx[i] = index[i];
        }
    }

    sub = matrixCreate(newdims, newshape);

    begin = reshapedIndex(m, beg_idx);
    end = reshapedIndex(m, end_idx);

    for (i = begin, j = 0; i <= end; i += stride, j++) {
        scalarRetain(sub->values[j] = m->values[i]);
    }

    return sub;
}

void matrixFree(matrix *matrix) {
    int i;
    for (i = 0; i < matrix->size; i++) {
        scalarRelease(matrix->values[i]);
    }
    zfree(matrix->values);
    zfree(matrix->shape);
    zfree(matrix);
}

void scalarRelease(scalar *scalar) {
    if (scalar->reference_count <= 0) printf("scalarRelease against refcount <= 0");
    if (scalar->reference_count == 1) zfree(scalar);
    else --scalar->reference_count;
}

void scalarRetain(scalar *scalar) {
    ++scalar->reference_count;
}

int matrixSetValues(matrix *matrix, double value, int stride) {
    long long i;

    for (i = 0; i < matrix->size; i += stride) {
        matrix->values[i]->value = value;
    }
    return 1; // TODO: error handling
}

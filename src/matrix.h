#ifndef __MATRIX_H__
#define __MATRIX_H__

typedef struct scalar {
  double value;
  int reference_count;
} scalar;

typedef struct matrix {
    long long dims;
    long long *shape;
    long long size;
    scalar **values;
} matrix;

matrix *matrixZero(long long dims, long long shape[]);
matrix *matrixCreate(long long dims, long long shape[]);
void matrixFree(matrix *matrix);
void matrixPrint(matrix *m);
matrix *matrixSlice(matrix *matrix, long long dims, long long index[]);
void scalarRelease(scalar *scalar);
void scalarRetain(scalar *scalar);
int matrixSetValues(matrix *m, double value, int stride);

#endif /* __MATRIX_H__ */

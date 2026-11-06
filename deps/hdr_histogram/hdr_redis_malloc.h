#ifndef HDR_MALLOC_H__
#define HDR_MALLOC_H__

void *zmalloc(size_t size);
void *zcalloc_num(size_t num, size_t size);
void *zrealloc(void *ptr, size_t size);
void zfree(void *ptr);

#define hdr_malloc zmalloc
#define hdr_calloc zcalloc_num
#define hdr_realloc zrealloc
#define hdr_free zfree
#endif

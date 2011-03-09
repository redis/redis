#ifndef __ENDIAN_H
#define __ENDIAN_H

void memrev16(void *p);
void memrev32(void *p);
void memrev64(void *p);

/* variants of the function doing the actual convertion only if the target
 * host is big endian */
#if (BYTE_ORDER == LITTLE_ENDIAN)
#define memrev16ifbe(p)
#define memrev32ifbe(p)
#define memrev64ifbe(p)
#else
#define memrev16ifbe(p) memrev16(p)
#define memrev32ifbe(p) memrev32(p)
#define memrev64ifbe(p) memrev64(p)
#endif

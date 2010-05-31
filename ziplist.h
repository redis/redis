#define ZIPLIST_HEAD 0
#define ZIPLIST_TAIL 1

unsigned char *ziplistNew(void);
unsigned char *ziplistPush(unsigned char *zl, char *s, unsigned int slen, int where);
unsigned char *ziplistPop(unsigned char *zl, sds *target, int where);
unsigned char *ziplistIndex(unsigned char *zl, int index);
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p);
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p);
unsigned int ziplistGet(unsigned char *p, char **sstr, unsigned int *slen, long long *sval);
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, char *s, unsigned int slen);
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p);
unsigned char *ziplistDeleteRange(unsigned char *zl, unsigned int index, unsigned int num);
unsigned int ziplistCompare(unsigned char *p, char *entry, unsigned int elen);
unsigned int ziplistLen(unsigned char *zl);
unsigned int ziplistSize(unsigned char *zl);

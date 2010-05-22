#define ZIPLIST_HEAD 0
#define ZIPLIST_TAIL 1

unsigned char *ziplistNew(void);
unsigned char *ziplistPush(unsigned char *zl, unsigned char *entry, unsigned int elen, int where);
unsigned char *ziplistPop(unsigned char *zl, sds *target, int where);
unsigned char *ziplistIndex(unsigned char *zl, unsigned int index);
unsigned char *ziplistNext(unsigned char *p);
unsigned int ziplistGet(unsigned char *p, unsigned char **e, unsigned int *elen, long long *v);
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p);
unsigned char *ziplistDeleteRange(unsigned char *zl, unsigned int index, unsigned int num);
unsigned int ziplistCompare(unsigned char *p, unsigned char *entry, unsigned int elen);
unsigned int ziplistLen(unsigned char *zl);
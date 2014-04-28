#ifndef __REDIS_UTIL_H
#define __REDIS_UTIL_H

int stringmatchlen(const char *p, int plen, const char *s, int slen, int nocase);
int stringmatch(const char *p, const char *s, int nocase);
long long memtoll(const char *p, int *err);
int ll2string(char *s, size_t len, long long value);
int string2ll(char *s, size_t slen, long long *value);
int string2l(char *s, size_t slen, long *value);
int d2string(char *buf, size_t len, double value);
void getRandomHexChars(char *p, unsigned int len);
long long ustime(void);
long long mstime(void);

#endif

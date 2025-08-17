#ifndef __BITOPS_H
#define __BITOPS_H

long long redisPopcount(void *s, long count);

#ifdef REDIS_TEST
int bitopsTest(int argc, char *argv[], int flags);
#endif

#endif /* __BITOPS_H */

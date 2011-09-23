#ifndef REDIS_RANDOM_H
#define REDIS_RANDOM_H

int32_t redisLrand48();
void redisSrand48(int32_t seedval);

#define REDIS_LRAND48_MAX INT32_MAX

#endif

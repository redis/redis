#ifndef CRC64_H
#define CRC64_H

#include <stdint.h>

void crc64_init(void);
uint64_t crc64(uint64_t crc, const unsigned char *s, uint64_t l);
int crc64_pclmul_available(void);
void crc64_pclmul_enable(int enable);

#ifdef REDIS_TEST
int crc64Test(int argc, char *argv[], int flags);
#endif

#endif

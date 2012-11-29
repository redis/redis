#ifndef CRC64_H
#define CRC64_H

#include <stdint.h>

uint64_t crc64(uint64_t crc, const unsigned char *s, uint64_t l);

#endif

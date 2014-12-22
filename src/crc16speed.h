#ifndef CRC16SPEED_H
#define CRC16SPEED_H
#include "crcspeed.h"
#include "stdbool.h"

/* Does not require init */
uint16_t crc16(uint16_t crc, const void *data, const uint64_t len);
void crc16speed_cache_table(void);

/* All other crc functions here require _init() before usage. */
bool crc16speed_init(void);
uint16_t crc16_lookup(uint16_t crc, const void *in_data, const uint64_t len);
uint16_t crc16speed(uint16_t crc, const void *s, const uint64_t l);

bool crc16speed_init_big(void);
uint16_t crc16speed_big(uint16_t crc, const void *s, const uint64_t l);

bool crc16speed_init_native(void);
uint16_t crc16speed_native(uint16_t crc, const void *s, const uint64_t l);
#endif

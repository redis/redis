
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define CTRIP_ERR 0
#define CTRIP_OK 1
uint32_t digits10(uint64_t v);
int ll2string(char *dst, size_t dstlen, long long svalue);
int string2ll(const char *s, size_t slen, long long *value);

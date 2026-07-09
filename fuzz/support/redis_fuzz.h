#ifndef REDIS_FUZZ_H
#define REDIS_FUZZ_H

#include "server.h"
#include "sds.h"

#include <stddef.h>
#include <stdint.h>

typedef struct RedisFuzzInput {
    const uint8_t *data;
    size_t size;
    size_t pos;
} RedisFuzzInput;

void redisFuzzInit(void);
void redisFuzzReset(void);
void redisFuzzRunResp(sds resp);

uint8_t redisFuzzByte(RedisFuzzInput *in);
long long redisFuzzChoice(RedisFuzzInput *in, long long count);
long long redisFuzzNumber(RedisFuzzInput *in, long long min, long long max);
sds redisFuzzSlice(RedisFuzzInput *in, size_t maxlen);

void redisFuzzAppendArray(sds *resp, int argc);
void redisFuzzAppendBulk(sds *resp, const char *data, size_t len);
void redisFuzzAppendBulkCString(sds *resp, const char *str);
void redisFuzzAppendBulkSds(sds *resp, sds value);
void redisFuzzAppendSmallNumber(sds *resp, RedisFuzzInput *in);
void redisFuzzAppendKey(sds *resp, RedisFuzzInput *in);

#endif

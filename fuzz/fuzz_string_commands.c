#include "redis_fuzz.h"

static void append_set(sds *resp, RedisFuzzInput *in) {
    redisFuzzAppendArray(resp, 3);
    redisFuzzAppendBulkCString(resp, "SET");
    redisFuzzAppendKey(resp, in);
    sds value = redisFuzzSlice(in, 64);
    redisFuzzAppendBulkSds(resp, value);
    sdsfree(value);
}

static void append_get(sds *resp, RedisFuzzInput *in) {
    redisFuzzAppendArray(resp, 2);
    redisFuzzAppendBulkCString(resp, "GET");
    redisFuzzAppendKey(resp, in);
}

static void append_append(sds *resp, RedisFuzzInput *in) {
    redisFuzzAppendArray(resp, 3);
    redisFuzzAppendBulkCString(resp, "APPEND");
    redisFuzzAppendKey(resp, in);
    sds value = redisFuzzSlice(in, 64);
    redisFuzzAppendBulkSds(resp, value);
    sdsfree(value);
}

static void append_setrange(sds *resp, RedisFuzzInput *in) {
    redisFuzzAppendArray(resp, 4);
    redisFuzzAppendBulkCString(resp, "SETRANGE");
    redisFuzzAppendKey(resp, in);
    redisFuzzAppendSmallNumber(resp, in);
    sds value = redisFuzzSlice(in, 64);
    redisFuzzAppendBulkSds(resp, value);
    sdsfree(value);
}

static void append_getrange(sds *resp, RedisFuzzInput *in) {
    redisFuzzAppendArray(resp, 4);
    redisFuzzAppendBulkCString(resp, "GETRANGE");
    redisFuzzAppendKey(resp, in);
    redisFuzzAppendSmallNumber(resp, in);
    redisFuzzAppendSmallNumber(resp, in);
}

static void append_strlen(sds *resp, RedisFuzzInput *in) {
    redisFuzzAppendArray(resp, 2);
    redisFuzzAppendBulkCString(resp, "STRLEN");
    redisFuzzAppendKey(resp, in);
}

static void append_incrby(sds *resp, RedisFuzzInput *in) {
    redisFuzzAppendArray(resp, 3);
    redisFuzzAppendBulkCString(resp, "INCRBY");
    redisFuzzAppendKey(resp, in);
    redisFuzzAppendSmallNumber(resp, in);
}

static void append_invalid_string_shape(sds *resp, RedisFuzzInput *in) {
    int argc = 1 + (int)redisFuzzChoice(in, 5);
    redisFuzzAppendArray(resp, argc);
    redisFuzzAppendBulkCString(resp, "GETRANGE");
    for (int i = 1; i < argc; i++) {
        sds value = redisFuzzSlice(in, 16);
        redisFuzzAppendBulkSds(resp, value);
        sdsfree(value);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    RedisFuzzInput in = {data, size, 0};
    sds resp = sdsempty();
    int commands = 1 + (int)redisFuzzChoice(&in, 16);

    for (int i = 0; i < commands; i++) {
        switch (redisFuzzChoice(&in, 8)) {
        case 0: append_set(&resp, &in); break;
        case 1: append_get(&resp, &in); break;
        case 2: append_append(&resp, &in); break;
        case 3: append_setrange(&resp, &in); break;
        case 4: append_getrange(&resp, &in); break;
        case 5: append_strlen(&resp, &in); break;
        case 6: append_incrby(&resp, &in); break;
        default: append_invalid_string_shape(&resp, &in); break;
        }
    }

    redisFuzzRunResp(resp);
    return 0;
}

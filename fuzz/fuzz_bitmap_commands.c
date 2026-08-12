#include "redis_fuzz.h"

static void append_bitop_name(sds *resp, RedisFuzzInput *in) {
    static const char *ops[] = {"AND", "OR", "XOR", "NOT", "DIFF", "ONE", "badop"};
    redisFuzzAppendBulkCString(resp, ops[redisFuzzChoice(in, sizeof(ops) / sizeof(ops[0]))]);
}

static void append_bitfield_type(sds *resp, RedisFuzzInput *in) {
    static const char *types[] = {"i1", "u1", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "bad"};
    redisFuzzAppendBulkCString(resp, types[redisFuzzChoice(in, sizeof(types) / sizeof(types[0]))]);
}

static void append_overflow(sds *resp, RedisFuzzInput *in) {
    static const char *modes[] = {"WRAP", "SAT", "FAIL", "bad"};
    redisFuzzAppendBulkCString(resp, modes[redisFuzzChoice(in, sizeof(modes) / sizeof(modes[0]))]);
}

static void append_setbit(sds *resp, RedisFuzzInput *in) {
    redisFuzzAppendArray(resp, 4);
    redisFuzzAppendBulkCString(resp, "SETBIT");
    redisFuzzAppendKey(resp, in);
    redisFuzzAppendSmallNumber(resp, in);
    redisFuzzAppendBulkCString(resp, redisFuzzChoice(in, 3) == 0 ? "bad" : (redisFuzzChoice(in, 2) ? "1" : "0"));
}

static void append_getbit(sds *resp, RedisFuzzInput *in) {
    redisFuzzAppendArray(resp, 3);
    redisFuzzAppendBulkCString(resp, "GETBIT");
    redisFuzzAppendKey(resp, in);
    redisFuzzAppendSmallNumber(resp, in);
}

static void append_bitcount(sds *resp, RedisFuzzInput *in) {
    int with_range = redisFuzzChoice(in, 2);
    int with_mode = with_range && redisFuzzChoice(in, 2);
    redisFuzzAppendArray(resp, with_mode ? 5 : (with_range ? 4 : 2));
    redisFuzzAppendBulkCString(resp, "BITCOUNT");
    redisFuzzAppendKey(resp, in);
    if (with_range) {
        redisFuzzAppendSmallNumber(resp, in);
        redisFuzzAppendSmallNumber(resp, in);
        if (with_mode) redisFuzzAppendBulkCString(resp, redisFuzzChoice(in, 2) ? "BYTE" : "BIT");
    }
}

static void append_bitpos(sds *resp, RedisFuzzInput *in) {
    int argc = 3 + (int)redisFuzzChoice(in, 4);
    redisFuzzAppendArray(resp, argc);
    redisFuzzAppendBulkCString(resp, "BITPOS");
    redisFuzzAppendKey(resp, in);
    redisFuzzAppendBulkCString(resp, redisFuzzChoice(in, 3) == 0 ? "bad" : (redisFuzzChoice(in, 2) ? "1" : "0"));
    for (int i = 3; i < argc; i++) {
        if (i == 5) redisFuzzAppendBulkCString(resp, redisFuzzChoice(in, 2) ? "BYTE" : "BIT");
        else redisFuzzAppendSmallNumber(resp, in);
    }
}

static void append_bitop(sds *resp, RedisFuzzInput *in) {
    int sources = 1 + (int)redisFuzzChoice(in, 4);
    redisFuzzAppendArray(resp, 3 + sources);
    redisFuzzAppendBulkCString(resp, "BITOP");
    append_bitop_name(resp, in);
    redisFuzzAppendKey(resp, in);
    for (int i = 0; i < sources; i++) redisFuzzAppendKey(resp, in);
}

static void append_bitfield(sds *resp, RedisFuzzInput *in, int readonly) {
    int subcommands = 1 + (int)redisFuzzChoice(in, 4);
    int argc = 2;
    uint8_t subcmds[4];
    for (int i = 0; i < subcommands; i++) {
        subcmds[i] = redisFuzzByte(in) % (readonly ? 2 : 4);
        switch (subcmds[i]) {
        case 0:
            argc += 3;
            break;
        case 1:
        case 2:
            argc += 4;
            break;
        default:
            argc += 2;
            break;
        }
    }

    redisFuzzAppendArray(resp, argc);
    redisFuzzAppendBulkCString(resp, readonly ? "BITFIELD_RO" : "BITFIELD");
    redisFuzzAppendKey(resp, in);
    for (int i = 0; i < subcommands; i++) {
        switch (subcmds[i]) {
        case 0:
            redisFuzzAppendBulkCString(resp, "GET");
            append_bitfield_type(resp, in);
            redisFuzzAppendSmallNumber(resp, in);
            break;
        case 1:
            redisFuzzAppendBulkCString(resp, "SET");
            append_bitfield_type(resp, in);
            redisFuzzAppendSmallNumber(resp, in);
            redisFuzzAppendSmallNumber(resp, in);
            break;
        case 2:
            redisFuzzAppendBulkCString(resp, "INCRBY");
            append_bitfield_type(resp, in);
            redisFuzzAppendSmallNumber(resp, in);
            redisFuzzAppendSmallNumber(resp, in);
            break;
        default:
            redisFuzzAppendBulkCString(resp, "OVERFLOW");
            append_overflow(resp, in);
            break;
        }
    }
}

static void append_invalid_bitmap_shape(sds *resp, RedisFuzzInput *in) {
    int argc = 1 + (int)redisFuzzChoice(in, 6);
    redisFuzzAppendArray(resp, argc);
    redisFuzzAppendBulkCString(resp, "BITOP");
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
        case 0: append_setbit(&resp, &in); break;
        case 1: append_getbit(&resp, &in); break;
        case 2: append_bitcount(&resp, &in); break;
        case 3: append_bitpos(&resp, &in); break;
        case 4: append_bitop(&resp, &in); break;
        case 5: append_bitfield(&resp, &in, 0); break;
        case 6: append_bitfield(&resp, &in, 1); break;
        default: append_invalid_bitmap_shape(&resp, &in); break;
        }
    }

    redisFuzzRunResp(resp);
    return 0;
}

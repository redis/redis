#include "redis_fuzz.h"

#include <stdio.h>

static void append_stream_key(sds *resp, RedisFuzzInput *in) {
    static const char *keys[] = {"s0", "s1", "wrong", "missing"};
    redisFuzzAppendBulkCString(resp, keys[redisFuzzChoice(in, sizeof(keys) / sizeof(keys[0]))]);
}

static void append_stream_id(sds *resp, RedisFuzzInput *in, int group_read) {
    static const char *read_ids[] = {"0", "0-0", "1-0", "$", "+", "bad-id"};
    static const char *group_ids[] = {">", "0", "0-0", "1-0", "bad-id"};
    const char **ids = group_read ? group_ids : read_ids;
    size_t count = group_read ? sizeof(group_ids) / sizeof(group_ids[0])
                              : sizeof(read_ids) / sizeof(read_ids[0]);
    redisFuzzAppendBulkCString(resp, ids[redisFuzzChoice(in, count)]);
}

static void append_positive_number(sds *resp, RedisFuzzInput *in) {
    static const char *values[] = {
        "1", "2", "3", "8", "32", "128", "4096",
        "0", "-1", "9223372036854775808", "not-an-int"
    };
    redisFuzzAppendBulkCString(resp, values[redisFuzzChoice(in, sizeof(values) / sizeof(values[0]))]);
}

/*
 * Establish both a pending-entry list and a wrong-type key before the
 * generated commands run. This makes even tiny inputs reach XNACK's stateful
 * paths instead of spending most mutations rediscovering the prerequisite
 * XADD/XGROUP/XREADGROUP sequence.
 */
static void append_setup(sds *resp) {
    redisFuzzAppendArray(resp, 4);
    redisFuzzAppendBulkCString(resp, "DEL");
    redisFuzzAppendBulkCString(resp, "s0");
    redisFuzzAppendBulkCString(resp, "s1");
    redisFuzzAppendBulkCString(resp, "wrong");

    redisFuzzAppendArray(resp, 3);
    redisFuzzAppendBulkCString(resp, "SET");
    redisFuzzAppendBulkCString(resp, "wrong");
    redisFuzzAppendBulkCString(resp, "not-a-stream");

    for (int i = 1; i <= 3; i++) {
        char id[16];
        char value[16];
        snprintf(id, sizeof(id), "%d-0", i);
        snprintf(value, sizeof(value), "v%d", i);
        redisFuzzAppendArray(resp, 5);
        redisFuzzAppendBulkCString(resp, "XADD");
        redisFuzzAppendBulkCString(resp, "s0");
        redisFuzzAppendBulkCString(resp, id);
        redisFuzzAppendBulkCString(resp, "f");
        redisFuzzAppendBulkCString(resp, value);
    }

    redisFuzzAppendArray(resp, 5);
    redisFuzzAppendBulkCString(resp, "XADD");
    redisFuzzAppendBulkCString(resp, "s1");
    redisFuzzAppendBulkCString(resp, "1-0");
    redisFuzzAppendBulkCString(resp, "f");
    redisFuzzAppendBulkCString(resp, "v");

    redisFuzzAppendArray(resp, 5);
    redisFuzzAppendBulkCString(resp, "XGROUP");
    redisFuzzAppendBulkCString(resp, "CREATE");
    redisFuzzAppendBulkCString(resp, "s0");
    redisFuzzAppendBulkCString(resp, "g0");
    redisFuzzAppendBulkCString(resp, "0");

    redisFuzzAppendArray(resp, 5);
    redisFuzzAppendBulkCString(resp, "XGROUP");
    redisFuzzAppendBulkCString(resp, "CREATE");
    redisFuzzAppendBulkCString(resp, "s1");
    redisFuzzAppendBulkCString(resp, "g0");
    redisFuzzAppendBulkCString(resp, "0");

    redisFuzzAppendArray(resp, 9);
    redisFuzzAppendBulkCString(resp, "XREADGROUP");
    redisFuzzAppendBulkCString(resp, "GROUP");
    redisFuzzAppendBulkCString(resp, "g0");
    redisFuzzAppendBulkCString(resp, "c0");
    redisFuzzAppendBulkCString(resp, "COUNT");
    redisFuzzAppendBulkCString(resp, "3");
    redisFuzzAppendBulkCString(resp, "STREAMS");
    redisFuzzAppendBulkCString(resp, "s0");
    redisFuzzAppendBulkCString(resp, ">");
}

static void append_xadd(sds *resp, RedisFuzzInput *in) {
    int fields = 1 + (int)redisFuzzChoice(in, 3);
    redisFuzzAppendArray(resp, 3 + fields * 2);
    redisFuzzAppendBulkCString(resp, "XADD");
    append_stream_key(resp, in);
    redisFuzzAppendBulkCString(resp, redisFuzzChoice(in, 4) ? "*" : "bad-id");
    for (int i = 0; i < fields; i++) {
        sds field = redisFuzzSlice(in, 32);
        sds value = redisFuzzSlice(in, 128);
        redisFuzzAppendBulkSds(resp, field);
        redisFuzzAppendBulkSds(resp, value);
        sdsfree(field);
        sdsfree(value);
    }
}

static void append_xread(sds *resp, RedisFuzzInput *in) {
    int streams = 1 + (int)redisFuzzChoice(in, 2);
    int with_count = redisFuzzChoice(in, 2);
    int with_maxcount = redisFuzzChoice(in, 2);
    int with_maxsize = redisFuzzChoice(in, 2);
    int argc = 2 + streams * 2 + (with_count + with_maxcount + with_maxsize) * 2;

    redisFuzzAppendArray(resp, argc);
    redisFuzzAppendBulkCString(resp, "XREAD");
    if (with_count) {
        redisFuzzAppendBulkCString(resp, "COUNT");
        append_positive_number(resp, in);
    }
    if (with_maxcount) {
        redisFuzzAppendBulkCString(resp, "MAXCOUNT");
        append_positive_number(resp, in);
    }
    if (with_maxsize) {
        redisFuzzAppendBulkCString(resp, "MAXSIZE");
        append_positive_number(resp, in);
    }
    redisFuzzAppendBulkCString(resp, "STREAMS");
    for (int i = 0; i < streams; i++) append_stream_key(resp, in);
    for (int i = 0; i < streams; i++) append_stream_id(resp, in, 0);
}

static void append_xreadgroup(sds *resp, RedisFuzzInput *in) {
    int streams = 1 + (int)redisFuzzChoice(in, 2);
    int with_count = redisFuzzChoice(in, 2);
    int with_maxcount = redisFuzzChoice(in, 2);
    int with_maxsize = redisFuzzChoice(in, 2);
    int with_noack = redisFuzzChoice(in, 2);
    int argc = 5 + streams * 2 + (with_count + with_maxcount + with_maxsize) * 2 + with_noack;

    redisFuzzAppendArray(resp, argc);
    redisFuzzAppendBulkCString(resp, "XREADGROUP");
    redisFuzzAppendBulkCString(resp, "GROUP");
    redisFuzzAppendBulkCString(resp, redisFuzzChoice(in, 5) ? "g0" : "missing-group");
    redisFuzzAppendBulkCString(resp, redisFuzzChoice(in, 2) ? "c0" : "c1");
    if (with_count) {
        redisFuzzAppendBulkCString(resp, "COUNT");
        append_positive_number(resp, in);
    }
    if (with_maxcount) {
        redisFuzzAppendBulkCString(resp, "MAXCOUNT");
        append_positive_number(resp, in);
    }
    if (with_maxsize) {
        redisFuzzAppendBulkCString(resp, "MAXSIZE");
        append_positive_number(resp, in);
    }
    if (with_noack) redisFuzzAppendBulkCString(resp, "NOACK");
    redisFuzzAppendBulkCString(resp, "STREAMS");
    for (int i = 0; i < streams; i++) append_stream_key(resp, in);
    for (int i = 0; i < streams; i++) append_stream_id(resp, in, 1);
}

static void append_xnack(sds *resp, RedisFuzzInput *in) {
    static const char *modes[] = {"SILENT", "FAIL", "FATAL", "bad-mode"};
    static const char *ids[] = {"1-0", "2-0", "3-0", "9-9", "bad-id"};
    int numids = 1 + (int)redisFuzzChoice(in, 3);
    int with_force = redisFuzzChoice(in, 2);
    int with_retry = redisFuzzChoice(in, 2);
    int retry_after_ids = redisFuzzChoice(in, 2);
    int trailing_option = redisFuzzChoice(in, 8) == 0;
    int argc = 6 + numids + with_force + with_retry * 2 + trailing_option;

    redisFuzzAppendArray(resp, argc);
    redisFuzzAppendBulkCString(resp, "XNACK");
    append_stream_key(resp, in);
    redisFuzzAppendBulkCString(resp, redisFuzzChoice(in, 5) ? "g0" : "missing-group");
    redisFuzzAppendBulkCString(resp, modes[redisFuzzChoice(in, sizeof(modes) / sizeof(modes[0]))]);
    if (with_force) redisFuzzAppendBulkCString(resp, "FORCE");
    if (with_retry && !retry_after_ids) {
        redisFuzzAppendBulkCString(resp, "RETRYCOUNT");
        append_positive_number(resp, in);
    }
    redisFuzzAppendBulkCString(resp, redisFuzzChoice(in, 8) ? "IDS" : "NOTIDS");
    if (redisFuzzChoice(in, 5)) {
        char declared[16];
        snprintf(declared, sizeof(declared), "%d", numids);
        redisFuzzAppendBulkCString(resp, declared);
    } else {
        append_positive_number(resp, in);
    }
    for (int i = 0; i < numids; i++) {
        redisFuzzAppendBulkCString(resp, ids[redisFuzzChoice(in, sizeof(ids) / sizeof(ids[0]))]);
    }
    if (with_retry && retry_after_ids) {
        redisFuzzAppendBulkCString(resp, "RETRYCOUNT");
        append_positive_number(resp, in);
    }
    if (trailing_option) redisFuzzAppendBulkCString(resp, "BADOPT");
}

static void append_xclaim(sds *resp, RedisFuzzInput *in) {
    static const char *ids[] = {"1-0", "2-0", "3-0", "9-9", "bad-id"};
    int numids = 1 + (int)redisFuzzChoice(in, 3);
    int with_retry = redisFuzzChoice(in, 2);

    redisFuzzAppendArray(resp, 5 + numids + with_retry * 2);
    redisFuzzAppendBulkCString(resp, "XCLAIM");
    append_stream_key(resp, in);
    redisFuzzAppendBulkCString(resp, "g0");
    redisFuzzAppendBulkCString(resp, redisFuzzChoice(in, 2) ? "c0" : "c1");
    append_positive_number(resp, in);
    for (int i = 0; i < numids; i++) {
        redisFuzzAppendBulkCString(resp, ids[redisFuzzChoice(in, sizeof(ids) / sizeof(ids[0]))]);
    }
    if (with_retry) {
        redisFuzzAppendBulkCString(resp, "RETRYCOUNT");
        append_positive_number(resp, in);
    }
}

static void append_xack(sds *resp, RedisFuzzInput *in) {
    static const char *ids[] = {"1-0", "2-0", "3-0", "9-9", "bad-id"};
    int numids = 1 + (int)redisFuzzChoice(in, 3);
    redisFuzzAppendArray(resp, 3 + numids);
    redisFuzzAppendBulkCString(resp, "XACK");
    append_stream_key(resp, in);
    redisFuzzAppendBulkCString(resp, "g0");
    for (int i = 0; i < numids; i++) {
        redisFuzzAppendBulkCString(resp, ids[redisFuzzChoice(in, sizeof(ids) / sizeof(ids[0]))]);
    }
}

static void append_xdel(sds *resp, RedisFuzzInput *in) {
    static const char *ids[] = {"1-0", "2-0", "3-0", "9-9", "bad-id"};
    int numids = 1 + (int)redisFuzzChoice(in, 3);
    redisFuzzAppendArray(resp, 2 + numids);
    redisFuzzAppendBulkCString(resp, "XDEL");
    append_stream_key(resp, in);
    for (int i = 0; i < numids; i++) {
        redisFuzzAppendBulkCString(resp, ids[redisFuzzChoice(in, sizeof(ids) / sizeof(ids[0]))]);
    }
}

static void append_invalid_stream_shape(sds *resp, RedisFuzzInput *in) {
    int argc = 1 + (int)redisFuzzChoice(in, 8);
    redisFuzzAppendArray(resp, argc);
    redisFuzzAppendBulkCString(resp, redisFuzzChoice(in, 2) ? "XNACK" : "XREADGROUP");
    for (int i = 1; i < argc; i++) {
        sds value = redisFuzzSlice(in, 24);
        redisFuzzAppendBulkSds(resp, value);
        sdsfree(value);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    RedisFuzzInput in = {data, size, 0};
    sds resp = sdsempty();
    int commands = 1 + (int)redisFuzzChoice(&in, 16);

    append_setup(&resp);
    for (int i = 0; i < commands; i++) {
        switch (redisFuzzChoice(&in, 8)) {
        case 0: append_xadd(&resp, &in); break;
        case 1: append_xread(&resp, &in); break;
        case 2: append_xreadgroup(&resp, &in); break;
        case 3: append_xnack(&resp, &in); break;
        case 4: append_xclaim(&resp, &in); break;
        case 5: append_xack(&resp, &in); break;
        case 6: append_xdel(&resp, &in); break;
        default: append_invalid_stream_shape(&resp, &in); break;
        }
    }

    redisFuzzRunResp(resp);
    return 0;
}

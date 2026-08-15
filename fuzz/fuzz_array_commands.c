#include "redis_fuzz.h"

#define ARRAY_FUZZ_MAX_COMMANDS 24
#define ARRAY_FUZZ_MAX_VALUES 12

static void append_array_index(sds *resp, RedisFuzzInput *in) {
    static const char *indices[] = {
        "-1",
        "0",
        "1",
        "2",
        "3",
        "4",
        "5",
        "7",
        "8",
        "9",
        "10",
        "15",
        "16",
        "31",
        "32",
        "63",
        "64",
        "255",
        "256",
        "4095",
        "4096",
        "4097",
        "8191",
        "8192",
        "8388607",
        "8388608",
        "4294967296",
        "18446744073709551614",
        "18446744073709551615",
        "not-an-index"
    };

    if (redisFuzzChoice(in, 5) == 0) {
        sds raw = redisFuzzSlice(in, 24);
        redisFuzzAppendBulkSds(resp, raw);
        sdsfree(raw);
        return;
    }

    redisFuzzAppendBulkCString(resp, indices[redisFuzzChoice(
        in, sizeof(indices) / sizeof(indices[0]))]);
}

static void append_array_count(sds *resp, RedisFuzzInput *in) {
    static const char *counts[] = {
        "-1",
        "0",
        "1",
        "2",
        "3",
        "5",
        "8",
        "12",
        "16",
        "64",
        "4096",
        "1000000",
        "1000001",
        "9223372036854775807",
        "not-a-count"
    };

    if (redisFuzzChoice(in, 5) == 0) {
        sds raw = redisFuzzSlice(in, 20);
        redisFuzzAppendBulkSds(resp, raw);
        sdsfree(raw);
        return;
    }

    redisFuzzAppendBulkCString(resp, counts[redisFuzzChoice(
        in, sizeof(counts) / sizeof(counts[0]))]);
}

static void append_array_value(sds *resp, RedisFuzzInput *in) {
    static const char *values[] = {
        "",
        "0",
        "1",
        "-1",
        "7.9",
        "3.141592653589793",
        "9223372036854775807",
        "-9223372036854775808",
        "nan",
        "inf",
        "a",
        "alpha",
        "alphabet",
        "RedisArray",
        "item-foo-123",
        "item-BAR-456",
        "abcdefgh",
        "a-string-longer-than-the-inline-encoding"
    };

    if (redisFuzzChoice(in, 4) == 0) {
        sds raw = redisFuzzSlice(in, 64);
        redisFuzzAppendBulkSds(resp, raw);
        sdsfree(raw);
        return;
    }

    redisFuzzAppendBulkCString(resp, values[redisFuzzChoice(
        in, sizeof(values) / sizeof(values[0]))]);
}

static void append_search_bound(sds *resp, RedisFuzzInput *in) {
    switch (redisFuzzChoice(in, 4)) {
    case 0:
        redisFuzzAppendBulkCString(resp, "-");
        break;
    case 1:
        redisFuzzAppendBulkCString(resp, "+");
        break;
    default:
        append_array_index(resp, in);
        break;
    }
}

static void append_regex(sds *resp, RedisFuzzInput *in) {
    static const char *patterns[] = {
        "alpha",
        "foo|bar",
        "^(foo|bar)$",
        "^item-(foo|bar)-[0-9]{3}$",
        "a{0,3}",
        "(foo)+",
        "[",
        "(",
        "*",
        "(a)\\1",
        "\\x{1",
        ""
    };

    redisFuzzAppendBulkCString(resp, patterns[redisFuzzChoice(
        in, sizeof(patterns) / sizeof(patterns[0]))]);
}

static void append_glob(sds *resp, RedisFuzzInput *in) {
    static const char *patterns[] = {
        "*",
        "?",
        "*array*",
        "item-??x-*",
        "[a-z]*",
        "[^0-9]*",
        "\\*",
        "[",
        ""
    };

    redisFuzzAppendBulkCString(resp, patterns[redisFuzzChoice(
        in, sizeof(patterns) / sizeof(patterns[0]))]);
}

static void append_arset(sds *resp, RedisFuzzInput *in) {
    int values = 1 + (int)redisFuzzChoice(in, ARRAY_FUZZ_MAX_VALUES);

    redisFuzzAppendArray(resp, 3 + values);
    redisFuzzAppendBulkCString(resp, "ARSET");
    redisFuzzAppendKey(resp, in);
    append_array_index(resp, in);
    for (int i = 0; i < values; i++) append_array_value(resp, in);
}

static void append_armset(sds *resp, RedisFuzzInput *in) {
    int pairs = 1 + (int)redisFuzzChoice(in, 8);

    redisFuzzAppendArray(resp, 2 + 2 * pairs);
    redisFuzzAppendBulkCString(resp, "ARMSET");
    redisFuzzAppendKey(resp, in);
    for (int i = 0; i < pairs; i++) {
        append_array_index(resp, in);
        append_array_value(resp, in);
    }
}

static void append_ardel(sds *resp, RedisFuzzInput *in) {
    int indices = 1 + (int)redisFuzzChoice(in, 8);

    redisFuzzAppendArray(resp, 2 + indices);
    redisFuzzAppendBulkCString(resp, "ARDEL");
    redisFuzzAppendKey(resp, in);
    for (int i = 0; i < indices; i++) append_array_index(resp, in);
}

static void append_ardelrange(sds *resp, RedisFuzzInput *in) {
    int ranges = 1 + (int)redisFuzzChoice(in, 4);

    redisFuzzAppendArray(resp, 2 + 2 * ranges);
    redisFuzzAppendBulkCString(resp, "ARDELRANGE");
    redisFuzzAppendKey(resp, in);
    for (int i = 0; i < ranges; i++) {
        append_array_index(resp, in);
        append_array_index(resp, in);
    }
}

static void append_arinsert(sds *resp, RedisFuzzInput *in) {
    int values = 1 + (int)redisFuzzChoice(in, 8);

    redisFuzzAppendArray(resp, 2 + values);
    redisFuzzAppendBulkCString(resp, "ARINSERT");
    redisFuzzAppendKey(resp, in);
    for (int i = 0; i < values; i++) append_array_value(resp, in);
}

static void append_arring(sds *resp, RedisFuzzInput *in) {
    int values = 1 + (int)redisFuzzChoice(in, 8);

    redisFuzzAppendArray(resp, 3 + values);
    redisFuzzAppendBulkCString(resp, "ARRING");
    redisFuzzAppendKey(resp, in);
    append_array_count(resp, in);
    for (int i = 0; i < values; i++) append_array_value(resp, in);
}

static void append_arseek(sds *resp, RedisFuzzInput *in) {
    redisFuzzAppendArray(resp, 3);
    redisFuzzAppendBulkCString(resp, "ARSEEK");
    redisFuzzAppendKey(resp, in);
    append_array_index(resp, in);
}

static void append_array_get(sds *resp, RedisFuzzInput *in) {
    switch (redisFuzzChoice(in, 3)) {
    case 0:
        redisFuzzAppendArray(resp, 3);
        redisFuzzAppendBulkCString(resp, "ARGET");
        redisFuzzAppendKey(resp, in);
        append_array_index(resp, in);
        break;
    case 1: {
        int indices = 1 + (int)redisFuzzChoice(in, 8);
        redisFuzzAppendArray(resp, 2 + indices);
        redisFuzzAppendBulkCString(resp, "ARMGET");
        redisFuzzAppendKey(resp, in);
        for (int i = 0; i < indices; i++) append_array_index(resp, in);
        break;
    }
    default:
        redisFuzzAppendArray(resp, 4);
        redisFuzzAppendBulkCString(resp, "ARGETRANGE");
        redisFuzzAppendKey(resp, in);
        append_array_index(resp, in);
        append_array_index(resp, in);
        break;
    }
}

static void append_arscan(sds *resp, RedisFuzzInput *in) {
    int with_limit = (int)redisFuzzChoice(in, 2);

    redisFuzzAppendArray(resp, with_limit ? 6 : 4);
    redisFuzzAppendBulkCString(resp, "ARSCAN");
    redisFuzzAppendKey(resp, in);
    append_array_index(resp, in);
    append_array_index(resp, in);
    if (with_limit) {
        redisFuzzAppendBulkCString(resp, "LIMIT");
        append_array_count(resp, in);
    }
}

static void append_argrep(sds *resp, RedisFuzzInput *in) {
    static const char *predicates[] = {"EXACT", "MATCH", "GLOB", "RE"};
    int predicate_count = 1 + (int)redisFuzzChoice(in, 3);
    uint8_t predicate_types[3];
    uint8_t options = redisFuzzByte(in);
    int argc = 4 + 2 * predicate_count;

    for (int i = 0; i < predicate_count; i++)
        predicate_types[i] = redisFuzzByte(in) % 4;
    if (options & 1) argc++;
    if (options & 2) argc++;
    if (options & 4) argc++;
    if (options & 8) argc += 2;

    redisFuzzAppendArray(resp, argc);
    redisFuzzAppendBulkCString(resp, "ARGREP");
    redisFuzzAppendKey(resp, in);
    append_search_bound(resp, in);
    append_search_bound(resp, in);
    for (int i = 0; i < predicate_count; i++) {
        redisFuzzAppendBulkCString(resp, predicates[predicate_types[i]]);
        if (predicate_types[i] == 3)
            append_regex(resp, in);
        else if (predicate_types[i] == 2)
            append_glob(resp, in);
        else
            append_array_value(resp, in);
    }
    if (options & 1)
        redisFuzzAppendBulkCString(resp, options & 16 ? "AND" : "OR");
    if (options & 2)
        redisFuzzAppendBulkCString(resp, "NOCASE");
    if (options & 4)
        redisFuzzAppendBulkCString(resp, "WITHVALUES");
    if (options & 8) {
        redisFuzzAppendBulkCString(resp, "LIMIT");
        append_array_count(resp, in);
    }
}

static void append_arop(sds *resp, RedisFuzzInput *in) {
    static const char *ops[] = {
        "SUM", "MIN", "MAX", "AND", "OR", "XOR", "MATCH", "USED", "bad-op"
    };
    size_t op = (size_t)redisFuzzChoice(in, sizeof(ops) / sizeof(ops[0]));
    int has_match_value = op == 6;

    redisFuzzAppendArray(resp, has_match_value ? 6 : 5);
    redisFuzzAppendBulkCString(resp, "AROP");
    redisFuzzAppendKey(resp, in);
    append_array_index(resp, in);
    append_array_index(resp, in);
    redisFuzzAppendBulkCString(resp, ops[op]);
    if (has_match_value) append_array_value(resp, in);
}

static void append_arlastitems(sds *resp, RedisFuzzInput *in) {
    int rev = (int)redisFuzzChoice(in, 2);

    redisFuzzAppendArray(resp, rev ? 4 : 3);
    redisFuzzAppendBulkCString(resp, "ARLASTITEMS");
    redisFuzzAppendKey(resp, in);
    append_array_count(resp, in);
    if (rev) redisFuzzAppendBulkCString(resp, "REV");
}

static void append_array_metadata(sds *resp, RedisFuzzInput *in) {
    static const char *commands[] = {"ARLEN", "ARCOUNT", "ARNEXT"};
    int command = (int)redisFuzzChoice(in, 4);

    if (command == 3) {
        int full = (int)redisFuzzChoice(in, 2);
        redisFuzzAppendArray(resp, full ? 3 : 2);
        redisFuzzAppendBulkCString(resp, "ARINFO");
        redisFuzzAppendKey(resp, in);
        if (full) redisFuzzAppendBulkCString(resp, "FULL");
        return;
    }

    redisFuzzAppendArray(resp, 2);
    redisFuzzAppendBulkCString(resp, commands[command]);
    redisFuzzAppendKey(resp, in);
}

static void append_key_control(sds *resp, RedisFuzzInput *in) {
    if (redisFuzzChoice(in, 2) == 0) {
        redisFuzzAppendArray(resp, 2);
        redisFuzzAppendBulkCString(resp, "DEL");
        redisFuzzAppendKey(resp, in);
        return;
    }

    redisFuzzAppendArray(resp, 3);
    redisFuzzAppendBulkCString(resp, "SET");
    redisFuzzAppendKey(resp, in);
    append_array_value(resp, in);
}

static void append_invalid_array_shape(sds *resp, RedisFuzzInput *in) {
    static const char *commands[] = {
        "ARSET", "ARMSET", "ARDEL", "ARDELRANGE", "ARGET", "ARGETRANGE",
        "ARSCAN", "ARGREP", "AROP", "ARRING", "ARSEEK", "ARLASTITEMS",
        "ARINFO"
    };
    int argc = 1 + (int)redisFuzzChoice(in, 9);

    redisFuzzAppendArray(resp, argc);
    redisFuzzAppendBulkCString(resp, commands[redisFuzzChoice(
        in, sizeof(commands) / sizeof(commands[0]))]);
    for (int i = 1; i < argc; i++) append_array_value(resp, in);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    RedisFuzzInput in = {data, size, 0};
    sds resp = sdsempty();
    int commands = 1 + (int)redisFuzzChoice(&in, ARRAY_FUZZ_MAX_COMMANDS);

    for (int i = 0; i < commands; i++) {
        switch (redisFuzzChoice(&in, 15)) {
        case 0: append_arset(&resp, &in); break;
        case 1: append_armset(&resp, &in); break;
        case 2: append_ardel(&resp, &in); break;
        case 3: append_ardelrange(&resp, &in); break;
        case 4: append_arinsert(&resp, &in); break;
        case 5: append_arring(&resp, &in); break;
        case 6: append_arseek(&resp, &in); break;
        case 7: append_array_get(&resp, &in); break;
        case 8: append_arscan(&resp, &in); break;
        case 9: append_argrep(&resp, &in); break;
        case 10: append_arop(&resp, &in); break;
        case 11: append_arlastitems(&resp, &in); break;
        case 12: append_array_metadata(&resp, &in); break;
        case 13: append_key_control(&resp, &in); break;
        default: append_invalid_array_shape(&resp, &in); break;
        }
    }

    redisFuzzRunResp(resp);
    return 0;
}

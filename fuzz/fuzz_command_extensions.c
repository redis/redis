#include "redis_fuzz.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define EXT_FUZZ_MAX_COMMANDS 16
#define EXT_FUZZ_MAX_OPTIONS 8
#define EXT_ORACLE_MEMBERS 4
#define EXT_ORACLE_INPUTS 3

#define ORACLE_INCREX_KEY "__fuzz_ext:increx"
#define ORACLE_UNION_RESULT_KEY "__fuzz_ext:union_card"
#define ORACLE_DIFF_RESULT_KEY "__fuzz_ext:diff_card"
#define ORACLE_UNION_ZSET_KEY "__fuzz_ext:zunion"
#define ORACLE_INTER_ZSET_KEY "__fuzz_ext:zinter"

static const char *oracle_set_keys[EXT_ORACLE_INPUTS] = {
    "__fuzz_ext:set0", "__fuzz_ext:set1", "__fuzz_ext:set2"
};

static const char *oracle_zset_keys[EXT_ORACLE_INPUTS] = {
    "__fuzz_ext:zset0", "__fuzz_ext:zset1", "__fuzz_ext:zset2"
};

typedef struct ExtensionOracle {
    unsigned int set_masks[EXT_ORACLE_INPUTS];
    int weights[EXT_ORACLE_INPUTS];
    long limit;
    long long increx_expected;
    unsigned int union_mask;
    unsigned int inter_mask;
    long long union_card_expected;
    long long diff_card_expected;
} ExtensionOracle;

static uint8_t input_byte(const uint8_t *data, size_t size, size_t index) {
    return index < size ? data[index] : 0;
}

static void append_extension_key(sds *resp, RedisFuzzInput *in) {
    static const char *keys[] = {
        "k0", "k1", "k2", "k3", "dst", "src", "alias", "missing"
    };

    redisFuzzAppendBulkCString(resp, keys[redisFuzzChoice(
        in, sizeof(keys) / sizeof(keys[0]))]);
}

static void append_member(sds *resp, RedisFuzzInput *in) {
    static const char *members[] = {
        "0",
        "1",
        "-1",
        "7",
        "42",
        "a",
        "b",
        "shared",
        "member-0",
        "member-1",
        "a-member-longer-than-the-listpack-value-threshold-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        ""
    };

    if (redisFuzzChoice(in, 5) == 0) {
        sds raw = redisFuzzSlice(in, 48);
        redisFuzzAppendBulkSds(resp, raw);
        sdsfree(raw);
        return;
    }

    redisFuzzAppendBulkCString(resp, members[redisFuzzChoice(
        in, sizeof(members) / sizeof(members[0]))]);
}

static void append_numeric(sds *resp, RedisFuzzInput *in) {
    static const char *numbers[] = {
        "-9223372036854775808",
        "-9223372036854775800",
        "-100",
        "-1",
        "-0",
        "0",
        "0.25",
        "1",
        "1.5",
        "10",
        "42.5",
        "100",
        "9223372036854775800",
        "9223372036854775807",
        "1e4932",
        "-1e4932",
        "nan",
        "+inf",
        "-inf",
        "not-a-number"
    };

    if (redisFuzzChoice(in, 5) == 0) {
        sds raw = redisFuzzSlice(in, 32);
        redisFuzzAppendBulkSds(resp, raw);
        sdsfree(raw);
        return;
    }

    redisFuzzAppendBulkCString(resp, numbers[redisFuzzChoice(
        in, sizeof(numbers) / sizeof(numbers[0]))]);
}

static void append_limit(sds *resp, RedisFuzzInput *in) {
    static const char *limits[] = {
        "-1", "0", "1", "2", "3", "8", "32", "1000",
        "9223372036854775807", "not-a-limit"
    };

    redisFuzzAppendBulkCString(resp, limits[redisFuzzChoice(
        in, sizeof(limits) / sizeof(limits[0]))]);
}

static void append_ttl(sds *resp, RedisFuzzInput *in) {
    static const char *ttls[] = {
        "-1",
        "0",
        "1",
        "10",
        "100",
        "100000",
        "2147483647",
        "9999999999",
        "9999999999000",
        "9223372036854775807",
        "not-a-ttl"
    };

    redisFuzzAppendBulkCString(resp, ttls[redisFuzzChoice(
        in, sizeof(ttls) / sizeof(ttls[0]))]);
}

static void append_set_value(sds *resp, RedisFuzzInput *in) {
    redisFuzzAppendArray(resp, 3);
    redisFuzzAppendBulkCString(resp, "SET");
    append_extension_key(resp, in);
    if (redisFuzzChoice(in, 2))
        append_numeric(resp, in);
    else
        append_member(resp, in);
}

static void append_increx_option(sds *resp, RedisFuzzInput *in, uint8_t type) {
    static const char *paired_options[] = {
        "BYINT", "BYFLOAT", "LBOUND", "UBOUND",
        "EX", "PX", "EXAT", "PXAT"
    };

    if (type <= 7) {
        redisFuzzAppendBulkCString(resp, paired_options[type]);
        if (type <= 3)
            append_numeric(resp, in);
        else
            append_ttl(resp, in);
        return;
    }

    switch (type) {
    case 8:
        redisFuzzAppendBulkCString(resp, "SATURATE");
        break;
    case 9:
        redisFuzzAppendBulkCString(resp, "ENX");
        break;
    case 10:
        redisFuzzAppendBulkCString(resp, "PERSIST");
        break;
    default:
        redisFuzzAppendBulkCString(resp, "BADOPTION");
        break;
    }
}

static void append_increx(sds *resp, RedisFuzzInput *in) {
    int option_count = (int)redisFuzzChoice(in, EXT_FUZZ_MAX_OPTIONS + 1);
    uint8_t option_types[EXT_FUZZ_MAX_OPTIONS];
    int argc = 2;

    for (int i = 0; i < option_count; i++) {
        option_types[i] = redisFuzzByte(in) % 12;
        argc += option_types[i] <= 7 ? 2 : 1;
    }

    redisFuzzAppendArray(resp, argc);
    redisFuzzAppendBulkCString(resp, "INCREX");
    append_extension_key(resp, in);
    for (int i = 0; i < option_count; i++)
        append_increx_option(resp, in, option_types[i]);
}

static void append_sadd(sds *resp, RedisFuzzInput *in) {
    int members = 1 + (int)redisFuzzChoice(in, 24);

    redisFuzzAppendArray(resp, 2 + members);
    redisFuzzAppendBulkCString(resp, "SADD");
    append_extension_key(resp, in);
    for (int i = 0; i < members; i++) append_member(resp, in);
}

static void append_set_cardinality(sds *resp, RedisFuzzInput *in, int is_union) {
    int numkeys = 1 + (int)redisFuzzChoice(in, 4);
    int mode = (int)redisFuzzChoice(in, 8);
    int argc = 2 + numkeys;

    if (is_union) {
        if (mode == 1 || mode == 3 || mode == 4) argc++;
        if (mode == 2 || mode == 3 || mode == 4) argc += 2;
        if (mode == 5) argc += 2;
        if (mode == 6) argc += 4;
        if (mode == 7) argc++;
    } else {
        if (mode >= 1 && mode <= 4) argc += 2;
        if (mode == 5) argc += 4;
        if (mode >= 6) argc++;
    }

    redisFuzzAppendArray(resp, argc);
    redisFuzzAppendBulkCString(resp, is_union ? "SUNIONCARD" : "SDIFFCARD");

    char numkeys_buf[16];
    snprintf(numkeys_buf, sizeof(numkeys_buf), "%d", numkeys);
    redisFuzzAppendBulkCString(resp, numkeys_buf);
    for (int i = 0; i < numkeys; i++) append_extension_key(resp, in);

    if (is_union) {
        switch (mode) {
        case 1:
            redisFuzzAppendBulkCString(resp, "APPROX");
            break;
        case 2:
            redisFuzzAppendBulkCString(resp, "LIMIT");
            append_limit(resp, in);
            break;
        case 3:
            redisFuzzAppendBulkCString(resp, "APPROX");
            redisFuzzAppendBulkCString(resp, "LIMIT");
            append_limit(resp, in);
            break;
        case 4:
            redisFuzzAppendBulkCString(resp, "LIMIT");
            append_limit(resp, in);
            redisFuzzAppendBulkCString(resp, "APPROX");
            break;
        case 5:
            redisFuzzAppendBulkCString(resp, "APPROX");
            redisFuzzAppendBulkCString(resp, "APPROX");
            break;
        case 6:
            redisFuzzAppendBulkCString(resp, "LIMIT");
            append_limit(resp, in);
            redisFuzzAppendBulkCString(resp, "LIMIT");
            append_limit(resp, in);
            break;
        case 7:
            redisFuzzAppendBulkCString(resp, "BADOPTION");
            break;
        }
    } else {
        if (mode >= 1 && mode <= 4) {
            redisFuzzAppendBulkCString(resp, "LIMIT");
            append_limit(resp, in);
        } else if (mode == 5) {
            redisFuzzAppendBulkCString(resp, "LIMIT");
            append_limit(resp, in);
            redisFuzzAppendBulkCString(resp, "LIMIT");
            append_limit(resp, in);
        } else if (mode == 6) {
            redisFuzzAppendBulkCString(resp, "APPROX");
        } else if (mode == 7) {
            redisFuzzAppendBulkCString(resp, "BADOPTION");
        }
    }
}

static void append_zadd(sds *resp, RedisFuzzInput *in) {
    int pairs = 1 + (int)redisFuzzChoice(in, 10);

    redisFuzzAppendArray(resp, 2 + 2 * pairs);
    redisFuzzAppendBulkCString(resp, "ZADD");
    append_extension_key(resp, in);
    for (int i = 0; i < pairs; i++) {
        append_numeric(resp, in);
        append_member(resp, in);
    }
}

static void append_weight(sds *resp, RedisFuzzInput *in) {
    static const char *weights[] = {
        "-3", "-1", "-0", "0", "0.5", "1", "2", "3",
        "1e308", "-1e308", "nan", "+inf", "-inf", "bad-weight"
    };

    redisFuzzAppendBulkCString(resp, weights[redisFuzzChoice(
        in, sizeof(weights) / sizeof(weights[0]))]);
}

static void append_zset_count(sds *resp, RedisFuzzInput *in) {
    static const char *commands[] = {
        "ZUNION", "ZINTER", "ZUNIONSTORE", "ZINTERSTORE"
    };
    int command = (int)redisFuzzChoice(in, 4);
    int store = command >= 2;
    int numkeys = 1 + (int)redisFuzzChoice(in, 4);
    uint8_t options = redisFuzzByte(in);
    int with_weights = options & 1;
    int with_scores = !store && (options & 2);
    int weights_first = options & 4;
    int valid_count = redisFuzzChoice(in, 5) != 0;
    int argc = (store ? 3 : 2) + numkeys + 2;

    if (with_weights) argc += 1 + numkeys;
    if (with_scores) argc++;

    redisFuzzAppendArray(resp, argc);
    redisFuzzAppendBulkCString(resp, commands[command]);
    if (store) append_extension_key(resp, in);

    char numkeys_buf[16];
    snprintf(numkeys_buf, sizeof(numkeys_buf), "%d", numkeys);
    redisFuzzAppendBulkCString(resp, numkeys_buf);
    for (int i = 0; i < numkeys; i++) append_extension_key(resp, in);

    if (with_weights && weights_first) {
        redisFuzzAppendBulkCString(resp, "WEIGHTS");
        for (int i = 0; i < numkeys; i++) append_weight(resp, in);
    }
    redisFuzzAppendBulkCString(resp, "AGGREGATE");
    redisFuzzAppendBulkCString(resp, valid_count ? "COUNT" : "BADAGGREGATE");
    if (with_weights && !weights_first) {
        redisFuzzAppendBulkCString(resp, "WEIGHTS");
        for (int i = 0; i < numkeys; i++) append_weight(resp, in);
    }
    if (with_scores) redisFuzzAppendBulkCString(resp, "WITHSCORES");
}

static void append_del(sds *resp, RedisFuzzInput *in) {
    int keys = 1 + (int)redisFuzzChoice(in, 4);

    redisFuzzAppendArray(resp, 1 + keys);
    redisFuzzAppendBulkCString(resp, "DEL");
    for (int i = 0; i < keys; i++) append_extension_key(resp, in);
}

static void append_shape_member(sds *resp, int string_encoding, int index) {
    char member[96];

    if (!string_encoding) {
        snprintf(member, sizeof(member), "%d", index);
    } else if (index == 0) {
        snprintf(member, sizeof(member),
                 "member-%02d-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
                 index);
    } else {
        snprintf(member, sizeof(member), "member-%02d", index);
    }
    redisFuzzAppendBulkCString(resp, member);
}

static void append_shape_sadd(sds *resp, const char *key, int string_encoding,
                              int first, int count) {
    redisFuzzAppendArray(resp, 2 + count);
    redisFuzzAppendBulkCString(resp, "SADD");
    redisFuzzAppendBulkCString(resp, key);
    for (int i = 0; i < count; i++)
        append_shape_member(resp, string_encoding, first + i);
}

static void append_sdiff_shape_sequence(sds *resp, RedisFuzzInput *in) {
    int string_encoding = (int)redisFuzzChoice(in, 2);

    append_shape_sadd(resp, "k0", string_encoding, 0, 32);
    append_shape_sadd(resp, "k1", string_encoding, 0, 1);
    append_shape_sadd(resp, "k2", string_encoding, 1, 1);
    append_shape_sadd(resp, "k3", string_encoding, 2, 1);

    redisFuzzAppendArray(resp, 8);
    redisFuzzAppendBulkCString(resp, "SDIFFCARD");
    redisFuzzAppendBulkCString(resp, "4");
    redisFuzzAppendBulkCString(resp, "k0");
    redisFuzzAppendBulkCString(resp, "k1");
    redisFuzzAppendBulkCString(resp, "k2");
    redisFuzzAppendBulkCString(resp, "k3");
    redisFuzzAppendBulkCString(resp, "LIMIT");
    append_limit(resp, in);

    redisFuzzAppendArray(resp, 9);
    redisFuzzAppendBulkCString(resp, "SUNIONCARD");
    redisFuzzAppendBulkCString(resp, "4");
    redisFuzzAppendBulkCString(resp, "k0");
    redisFuzzAppendBulkCString(resp, "k1");
    redisFuzzAppendBulkCString(resp, "k2");
    redisFuzzAppendBulkCString(resp, "k3");
    redisFuzzAppendBulkCString(resp, "APPROX");
    redisFuzzAppendBulkCString(resp, "LIMIT");
    append_limit(resp, in);
}

static void append_invalid_extension_shape(sds *resp, RedisFuzzInput *in) {
    static const char *commands[] = {
        "INCREX", "SUNIONCARD", "SDIFFCARD", "ZUNION", "ZINTER",
        "ZUNIONSTORE", "ZINTERSTORE"
    };
    int argc = 1 + (int)redisFuzzChoice(in, 10);

    redisFuzzAppendArray(resp, argc);
    redisFuzzAppendBulkCString(resp, commands[redisFuzzChoice(
        in, sizeof(commands) / sizeof(commands[0]))]);
    for (int i = 1; i < argc; i++) {
        if (redisFuzzChoice(in, 2))
            append_numeric(resp, in);
        else
            append_member(resp, in);
    }
}

static void append_bulk_long_long(sds *resp, long long value) {
    char buf[LONG_STR_SIZE];
    int len = snprintf(buf, sizeof(buf), "%lld", value);
    redisFuzzAppendBulk(resp, buf, (size_t)len);
}

static void append_oracle_sadd(sds *resp, const char *key, unsigned int mask) {
    static const char *members[EXT_ORACLE_MEMBERS] = {"a", "b", "c", "d"};
    int count = __builtin_popcount(mask);
    if (count == 0) return;

    redisFuzzAppendArray(resp, 2 + count);
    redisFuzzAppendBulkCString(resp, "SADD");
    redisFuzzAppendBulkCString(resp, key);
    for (int i = 0; i < EXT_ORACLE_MEMBERS; i++) {
        if (mask & (1U << i))
            redisFuzzAppendBulkCString(resp, members[i]);
    }
}

static void append_oracle_zadd(sds *resp, const char *key, unsigned int mask) {
    static const char *members[EXT_ORACLE_MEMBERS] = {"a", "b", "c", "d"};
    int count = __builtin_popcount(mask);
    if (count == 0) return;

    redisFuzzAppendArray(resp, 2 + 2 * count);
    redisFuzzAppendBulkCString(resp, "ZADD");
    redisFuzzAppendBulkCString(resp, key);
    for (int i = 0; i < EXT_ORACLE_MEMBERS; i++) {
        if (!(mask & (1U << i))) continue;
        append_bulk_long_long(resp, i + 1);
        redisFuzzAppendBulkCString(resp, members[i]);
    }
}

static void append_cardinality_oracle_eval(sds *resp, int is_union, long limit) {
    static const char *union_script =
        "redis.call('SET',KEYS[1],redis.call('SUNIONCARD',3,KEYS[2],KEYS[3],KEYS[4],'LIMIT',ARGV[1]));return 1";
    static const char *diff_script =
        "redis.call('SET',KEYS[1],redis.call('SDIFFCARD',3,KEYS[2],KEYS[3],KEYS[4],'LIMIT',ARGV[1]));return 1";

    redisFuzzAppendArray(resp, 8);
    redisFuzzAppendBulkCString(resp, "EVAL");
    redisFuzzAppendBulkCString(resp, is_union ? union_script : diff_script);
    redisFuzzAppendBulkCString(resp, "4");
    redisFuzzAppendBulkCString(resp, is_union ?
        ORACLE_UNION_RESULT_KEY : ORACLE_DIFF_RESULT_KEY);
    for (int i = 0; i < EXT_ORACLE_INPUTS; i++)
        redisFuzzAppendBulkCString(resp, oracle_set_keys[i]);
    append_bulk_long_long(resp, limit);
}

static void append_increx_oracle(sds *resp, long long start, long long increment,
                                 long long lower, long long upper, int reverse_order) {
    redisFuzzAppendArray(resp, 3);
    redisFuzzAppendBulkCString(resp, "SET");
    redisFuzzAppendBulkCString(resp, ORACLE_INCREX_KEY);
    append_bulk_long_long(resp, start);

    redisFuzzAppendArray(resp, 9);
    redisFuzzAppendBulkCString(resp, "INCREX");
    redisFuzzAppendBulkCString(resp, ORACLE_INCREX_KEY);
    if (reverse_order) {
        redisFuzzAppendBulkCString(resp, "UBOUND");
        append_bulk_long_long(resp, upper);
        redisFuzzAppendBulkCString(resp, "SATURATE");
        redisFuzzAppendBulkCString(resp, "BYINT");
        append_bulk_long_long(resp, increment);
        redisFuzzAppendBulkCString(resp, "LBOUND");
        append_bulk_long_long(resp, lower);
    } else {
        redisFuzzAppendBulkCString(resp, "BYINT");
        append_bulk_long_long(resp, increment);
        redisFuzzAppendBulkCString(resp, "LBOUND");
        append_bulk_long_long(resp, lower);
        redisFuzzAppendBulkCString(resp, "UBOUND");
        append_bulk_long_long(resp, upper);
        redisFuzzAppendBulkCString(resp, "SATURATE");
    }
}

static void append_zset_oracle_store(sds *resp, int is_union,
                                     const ExtensionOracle *oracle) {
    redisFuzzAppendArray(resp, 12);
    redisFuzzAppendBulkCString(resp, is_union ? "ZUNIONSTORE" : "ZINTERSTORE");
    redisFuzzAppendBulkCString(resp, is_union ?
        ORACLE_UNION_ZSET_KEY : ORACLE_INTER_ZSET_KEY);
    redisFuzzAppendBulkCString(resp, "3");
    for (int i = 0; i < EXT_ORACLE_INPUTS; i++)
        redisFuzzAppendBulkCString(resp, oracle_zset_keys[i]);
    redisFuzzAppendBulkCString(resp, "WEIGHTS");
    for (int i = 0; i < EXT_ORACLE_INPUTS; i++)
        append_bulk_long_long(resp, oracle->weights[i]);
    redisFuzzAppendBulkCString(resp, "AGGREGATE");
    redisFuzzAppendBulkCString(resp, "COUNT");
}

static long long clamp_long_long(long long value, long long lower,
                                 long long upper) {
    if (value < lower) return lower;
    if (value > upper) return upper;
    return value;
}

static void append_extension_oracle(sds *resp, ExtensionOracle *oracle,
                                    const uint8_t *data, size_t size) {
    static const long long starts[] = {-20, -1, 0, 1, 20};
    static const long long increments[] = {-50, -10, -1, 0, 1, 10, 50};
    static const long long bounds[][2] = {
        {-25, 25}, {-10, 10}, {0, 25}, {-25, 0}
    };

    for (int i = 0; i < EXT_ORACLE_INPUTS; i++) {
        oracle->set_masks[i] = input_byte(data, size, i) & 0xf;
        oracle->weights[i] = (input_byte(data, size, i + 3) % 7) - 3;
    }
    oracle->limit = input_byte(data, size, 6) % 6;

    unsigned int union_mask = oracle->set_masks[0] |
                              oracle->set_masks[1] |
                              oracle->set_masks[2];
    unsigned int diff_mask = oracle->set_masks[0] &
                             ~oracle->set_masks[1] &
                             ~oracle->set_masks[2] & 0xf;
    oracle->union_mask = union_mask;
    oracle->inter_mask = oracle->set_masks[0] &
                         oracle->set_masks[1] &
                         oracle->set_masks[2];
    oracle->union_card_expected = __builtin_popcount(union_mask);
    oracle->diff_card_expected = __builtin_popcount(diff_mask);
    if (oracle->limit > 0) {
        if (oracle->union_card_expected > oracle->limit)
            oracle->union_card_expected = oracle->limit;
        if (oracle->diff_card_expected > oracle->limit)
            oracle->diff_card_expected = oracle->limit;
    }

    for (int i = 0; i < EXT_ORACLE_INPUTS; i++)
        append_oracle_sadd(resp, oracle_set_keys[i], oracle->set_masks[i]);
    append_cardinality_oracle_eval(resp, 1, oracle->limit);
    append_cardinality_oracle_eval(resp, 0, oracle->limit);

    long long start = starts[input_byte(data, size, 7) %
                             (sizeof(starts) / sizeof(starts[0]))];
    long long increment = increments[input_byte(data, size, 8) %
                                     (sizeof(increments) / sizeof(increments[0]))];
    size_t bound_index = input_byte(data, size, 9) %
                         (sizeof(bounds) / sizeof(bounds[0]));
    long long lower = bounds[bound_index][0];
    long long upper = bounds[bound_index][1];
    oracle->increx_expected = clamp_long_long(start + increment, lower, upper);
    append_increx_oracle(resp, start, increment, lower, upper,
                         input_byte(data, size, 10) & 1);

    for (int i = 0; i < EXT_ORACLE_INPUTS; i++)
        append_oracle_zadd(resp, oracle_zset_keys[i], oracle->set_masks[i]);
    append_zset_oracle_store(resp, 1, oracle);
    append_zset_oracle_store(resp, 0, oracle);
}

static kvobj *lookup_oracle_key(const char *name) {
    robj *key = createStringObject(name, strlen(name));
    kvobj *value = lookupKeyRead(&server.db[0], key);
    decrRefCount(key);
    return value;
}

static long long read_oracle_integer(const char *name) {
    kvobj *value = lookup_oracle_key(name);
    long long result;
    if (!value || value->type != OBJ_STRING ||
        getLongLongFromObject(value, &result) != C_OK)
    {
        serverPanic("command extension fuzz oracle key '%s' is not an integer", name);
    }
    return result;
}

static void validate_oracle_zset(const char *name, unsigned int expected_mask,
                                 const ExtensionOracle *oracle) {
    static const char *members[EXT_ORACLE_MEMBERS] = {"a", "b", "c", "d"};
    kvobj *zset = lookup_oracle_key(name);
    unsigned long expected_length = __builtin_popcount(expected_mask);

    if (expected_length == 0) {
        if (zset != NULL)
            serverPanic("command extension fuzz oracle zset '%s' should be absent", name);
        return;
    }
    if (!zset || zset->type != OBJ_ZSET || zsetLength(zset) != expected_length)
        serverPanic("command extension fuzz oracle zset '%s' has wrong type or length", name);

    for (int member = 0; member < EXT_ORACLE_MEMBERS; member++) {
        sds element = sdsnew(members[member]);
        double actual_score = 0;
        int exists = zsetScore(zset, element, &actual_score) == C_OK;
        sdsfree(element);

        int expected_exists = (expected_mask & (1U << member)) != 0;
        if (exists != expected_exists)
            serverPanic("command extension fuzz oracle zset '%s' has wrong membership", name);
        if (!expected_exists) continue;

        int expected_score = 0;
        for (int input = 0; input < EXT_ORACLE_INPUTS; input++) {
            if (oracle->set_masks[input] & (1U << member))
                expected_score += oracle->weights[input];
        }
        if (actual_score != expected_score)
            serverPanic("command extension fuzz oracle zset '%s' has wrong COUNT score", name);
    }
}

static void validate_extension_oracle(void *ctx) {
    ExtensionOracle *oracle = ctx;

    if (read_oracle_integer(ORACLE_INCREX_KEY) != oracle->increx_expected)
        serverPanic("command extension fuzz INCREX oracle mismatch");
    if (read_oracle_integer(ORACLE_UNION_RESULT_KEY) != oracle->union_card_expected)
        serverPanic("command extension fuzz SUNIONCARD oracle mismatch");
    if (read_oracle_integer(ORACLE_DIFF_RESULT_KEY) != oracle->diff_card_expected)
        serverPanic("command extension fuzz SDIFFCARD oracle mismatch");

    validate_oracle_zset(ORACLE_UNION_ZSET_KEY, oracle->union_mask, oracle);
    validate_oracle_zset(ORACLE_INTER_ZSET_KEY, oracle->inter_mask, oracle);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    RedisFuzzInput in = {data, size, 0};
    ExtensionOracle oracle;
    sds resp = sdsempty();
    int commands = 1 + (int)redisFuzzChoice(&in, EXT_FUZZ_MAX_COMMANDS);

    for (int i = 0; i < commands; i++) {
        switch (redisFuzzChoice(&in, 10)) {
        case 0: append_set_value(&resp, &in); break;
        case 1: append_increx(&resp, &in); break;
        case 2: append_sadd(&resp, &in); break;
        case 3: append_set_cardinality(&resp, &in, 1); break;
        case 4: append_set_cardinality(&resp, &in, 0); break;
        case 5: append_zadd(&resp, &in); break;
        case 6: append_zset_count(&resp, &in); break;
        case 7: append_del(&resp, &in); break;
        case 8: append_sdiff_shape_sequence(&resp, &in); break;
        default: append_invalid_extension_shape(&resp, &in); break;
        }
    }

    append_extension_oracle(&resp, &oracle, data, size);
    redisFuzzRunRespWithPostHook(resp, validate_extension_oracle, &oracle);
    return 0;
}

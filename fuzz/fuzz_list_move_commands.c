#include "redis_fuzz.h"
#include "util.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST_FUZZ_KEYS 9
#define LIST_FUZZ_MAX_ELEMENTS 128

static const char *list_keys[LIST_FUZZ_KEYS] = {
    "src", "dst", "same", "feed", "wrong-src", "wrong-dst", "missing",
    "{slot}src", "{slot}dst"
};

typedef enum ListFuzzValueType {
    LIST_FUZZ_NONE,
    LIST_FUZZ_STRING,
    LIST_FUZZ_LIST
} ListFuzzValueType;

typedef enum ListFuzzMoveMode {
    LIST_FUZZ_DEFAULT,
    LIST_FUZZ_COUNT,
    LIST_FUZZ_EXACTLY
} ListFuzzMoveMode;

typedef enum ListFuzzMoveOrdering {
    LIST_FUZZ_OBO,
    LIST_FUZZ_BULK
} ListFuzzMoveOrdering;

typedef struct ListFuzzKey {
    sds name;
    ListFuzzValueType type;
    sds string_value;
    size_t count;
    sds elements[LIST_FUZZ_MAX_ELEMENTS];
} ListFuzzKey;

typedef struct ListFuzzModel {
    ListFuzzKey keys[LIST_FUZZ_KEYS];
    int blocked;
} ListFuzzModel;

static __attribute__((noreturn, noinline))
void listFuzzFail(const char *condition, int line) {
    fprintf(stderr, "list move model mismatch at line %d: %s\n",
            line, condition);
    fflush(stderr);
    abort();
}

#define listFuzzCheck(condition) do { \
    if (!(condition)) listFuzzFail(#condition, __LINE__); \
} while (0)

static void listFuzzKeyClear(ListFuzzKey *key) {
    sdsfree(key->string_value);
    key->string_value = NULL;
    for (size_t i = 0; i < key->count; i++)
        sdsfree(key->elements[i]);
    key->count = 0;
    key->type = LIST_FUZZ_NONE;
}

static void listFuzzModelInit(ListFuzzModel *model) {
    memset(model, 0, sizeof(*model));
    for (size_t i = 0; i < LIST_FUZZ_KEYS; i++)
        model->keys[i].name = sdsnew(list_keys[i]);
}

static void listFuzzModelFree(ListFuzzModel *model) {
    for (size_t i = 0; i < LIST_FUZZ_KEYS; i++) {
        listFuzzKeyClear(&model->keys[i]);
        sdsfree(model->keys[i].name);
    }
}

static void listFuzzSetString(ListFuzzKey *key, sds value) {
    listFuzzKeyClear(key);
    key->type = LIST_FUZZ_STRING;
    key->string_value = sdsdup(value);
}

static void listFuzzInsertCopy(ListFuzzKey *key, size_t index, sds value) {
    if (key->type == LIST_FUZZ_STRING) return;
    if (key->type == LIST_FUZZ_NONE) key->type = LIST_FUZZ_LIST;
    listFuzzCheck(key->count < LIST_FUZZ_MAX_ELEMENTS);
    listFuzzCheck(index <= key->count);
    memmove(&key->elements[index + 1], &key->elements[index],
            (key->count - index) * sizeof(key->elements[0]));
    key->elements[index] = sdsdup(value);
    key->count++;
}

static void listFuzzPush(ListFuzzKey *key, sds value, int where) {
    listFuzzInsertCopy(key, where == LIST_HEAD ? 0 : key->count, value);
}

static void listFuzzPop(ListFuzzKey *key, int where, long count) {
    if (key->type != LIST_FUZZ_LIST || count <= 0) return;
    size_t remove = (unsigned long)count > key->count ?
                    key->count : (size_t)count;

    if (where == LIST_HEAD) {
        for (size_t i = 0; i < remove; i++)
            sdsfree(key->elements[i]);
        memmove(key->elements, &key->elements[remove],
                (key->count - remove) * sizeof(key->elements[0]));
    } else {
        for (size_t i = key->count - remove; i < key->count; i++)
            sdsfree(key->elements[i]);
    }
    key->count -= remove;
    if (key->count == 0) key->type = LIST_FUZZ_NONE;
}

static void listFuzzTrim(ListFuzzKey *key, long start, long end) {
    if (key->type != LIST_FUZZ_LIST) return;

    long length = (long)key->count;
    if (start < 0) start = length + start;
    if (end < 0) end = length + end;
    if (start < 0) start = 0;
    if (start > end || start >= length) {
        listFuzzKeyClear(key);
        return;
    }
    if (end >= length) end = length - 1;

    size_t first = (size_t)start;
    size_t last = (size_t)end;
    size_t new_count = last - first + 1;
    for (size_t i = 0; i < first; i++)
        sdsfree(key->elements[i]);
    for (size_t i = last + 1; i < key->count; i++)
        sdsfree(key->elements[i]);
    memmove(key->elements, &key->elements[first],
            new_count * sizeof(key->elements[0]));
    key->count = new_count;
}

static void listFuzzInsertAroundPivot(ListFuzzKey *key, sds pivot, sds value,
                                      int after)
{
    if (key->type != LIST_FUZZ_LIST) return;
    for (size_t i = 0; i < key->count; i++) {
        if (sdscmplen(key->elements[i], pivot) == 0) {
            listFuzzInsertCopy(key, i + (after != 0), value);
            return;
        }
    }
}

static int listFuzzMove(ListFuzzModel *model, size_t source_index,
                        size_t destination_index, int wherefrom, int whereto,
                        ListFuzzMoveMode mode, long count,
                        ListFuzzMoveOrdering ordering, int blocking)
{
    ListFuzzKey *source = &model->keys[source_index];
    ListFuzzKey *destination = &model->keys[destination_index];

    if (source->type == LIST_FUZZ_STRING) return 0;

    size_t available = source->type == LIST_FUZZ_LIST ? source->count : 0;
    size_t needed = mode == LIST_FUZZ_EXACTLY ? (size_t)count : 1;
    if (available < needed) {
        if (blocking) {
            model->blocked = 1;
            return 1;
        }
        return 0;
    }

    size_t tomove;
    if (mode == LIST_FUZZ_DEFAULT) {
        tomove = 1;
    } else if (mode == LIST_FUZZ_COUNT) {
        tomove = (size_t)count < available ? (size_t)count : available;
    } else {
        tomove = (size_t)count;
    }
    if (source_index != destination_index &&
        destination->type == LIST_FUZZ_STRING)
    {
        return 0;
    }

    sds moved[LIST_FUZZ_MAX_ELEMENTS];
    listFuzzCheck(tomove <= LIST_FUZZ_MAX_ELEMENTS);
    if (wherefrom == LIST_HEAD) {
        for (size_t i = 0; i < tomove; i++)
            moved[i] = source->elements[i];
        memmove(source->elements, &source->elements[tomove],
                (source->count - tomove) * sizeof(source->elements[0]));
    } else {
        for (size_t i = 0; i < tomove; i++)
            moved[i] = source->elements[source->count - 1 - i];
    }
    source->count -= tomove;
    if (source_index != destination_index && source->count == 0)
        source->type = LIST_FUZZ_NONE;

    if ((ordering == LIST_FUZZ_OBO && whereto == LIST_HEAD) ||
        (ordering == LIST_FUZZ_BULK && wherefrom == LIST_TAIL))
    {
        for (size_t i = 0, j = tomove - 1; i < j; i++, j--) {
            sds temporary = moved[i];
            moved[i] = moved[j];
            moved[j] = temporary;
        }
    }

    destination = &model->keys[destination_index];
    if (destination->type == LIST_FUZZ_NONE)
        destination->type = LIST_FUZZ_LIST;
    listFuzzCheck(destination->count + tomove <= LIST_FUZZ_MAX_ELEMENTS);
    if (whereto == LIST_HEAD) {
        memmove(&destination->elements[tomove], destination->elements,
                destination->count * sizeof(destination->elements[0]));
        for (size_t i = 0; i < tomove; i++)
            destination->elements[i] = moved[i];
    } else {
        for (size_t i = 0; i < tomove; i++)
            destination->elements[destination->count + i] = moved[i];
    }
    destination->count += tomove;
    return 0;
}

static void append_key_index(sds *resp, size_t index) {
    redisFuzzAppendBulkCString(resp, list_keys[index % LIST_FUZZ_KEYS]);
}

static size_t append_list_key(sds *resp, RedisFuzzInput *in) {
    size_t index = (size_t)redisFuzzChoice(in, LIST_FUZZ_KEYS);
    append_key_index(resp, index);
    return index;
}

static int append_direction(sds *resp, RedisFuzzInput *in) {
    int direction = redisFuzzChoice(in, 2) ? LIST_TAIL : LIST_HEAD;
    redisFuzzAppendBulkCString(
        resp, direction == LIST_TAIL ? "RIGHT" : "LEFT");
    return direction;
}

static long append_positive_count(sds *resp, RedisFuzzInput *in) {
    static const char *counts[] = {"1", "2", "3", "4", "8", "32"};
    size_t selected = (size_t)redisFuzzChoice(
        in, sizeof(counts) / sizeof(counts[0]) + 1);
    if (selected < sizeof(counts) / sizeof(counts[0])) {
        redisFuzzAppendBulkCString(resp, counts[selected]);
        return (long[]){1, 2, 3, 4, 8, 32}[selected];
    }

    sds maximum = sdsfromlonglong(LONG_MAX);
    redisFuzzAppendBulkSds(resp, maximum);
    sdsfree(maximum);
    return LONG_MAX;
}

static long append_any_count(sds *resp, RedisFuzzInput *in, int *valid) {
    static const char *counts[] = {
        "-9223372036854775808", "-1", "0", "1", "2", "8", "32",
        "9223372036854775807", "not-a-count"
    };
    const char *selected = counts[redisFuzzChoice(
        in, sizeof(counts) / sizeof(counts[0]))];
    long long parsed;

    redisFuzzAppendBulkCString(resp, selected);
    if (string2ll(selected, strlen(selected), &parsed) &&
        parsed >= 0 && (unsigned long long)parsed <= (unsigned long)LONG_MAX)
    {
        *valid = 1;
        return (long)parsed;
    }
    *valid = 0;
    return 0;
}

static sds listFuzzSmallNumber(RedisFuzzInput *in) {
    static const char *special[] = {
        "-9223372036854775808", "-2147483649", "-1", "0", "1", "7",
        "8", "31", "32", "63", "64", "127", "128", "255", "256",
        "511", "512", "1024", "4096", "16384", "9223372036854775807",
        "not-an-int"
    };

    if (redisFuzzChoice(in, 4) == 0)
        return redisFuzzSlice(in, 16);
    return sdsnew(special[redisFuzzChoice(
        in, sizeof(special) / sizeof(special[0]))]);
}

static long append_list_index(sds *resp, RedisFuzzInput *in, int *valid) {
    sds raw = listFuzzSmallNumber(in);
    long long parsed;
    redisFuzzAppendBulkSds(resp, raw);
    if (string2ll(raw, sdslen(raw), &parsed) &&
        parsed >= (long long)LONG_MIN && parsed <= (long long)LONG_MAX)
    {
        *valid = 1;
        sdsfree(raw);
        return (long)parsed;
    }
    *valid = 0;
    sdsfree(raw);
    return 0;
}

static sds append_list_value(sds *resp, RedisFuzzInput *in) {
    static const char *constants[] = {
        "", "0", "-1", "alpha", "9223372036854775807"
    };
    static const size_t repeated_lengths[] = {1, 63, 256, 1024};
    static const unsigned char binary_values[][8] = {
        {0x00},
        {0xff},
        {0x00, 0xff, 0x00, 0xff},
        {0x0d, 0x0a, 0x00, 0x24, 0x2a, 0xff, 0x7f, 0x80}
    };
    static const size_t binary_lengths[] = {1, 1, 4, 8};
    sds value;

    switch (redisFuzzChoice(in, 4)) {
    case 0:
        value = redisFuzzSlice(in, 255);
        break;
    case 1:
        value = sdsnew(constants[redisFuzzChoice(
            in, sizeof(constants) / sizeof(constants[0]))]);
        break;
    case 2: {
        unsigned char repeated[1024];
        size_t length = repeated_lengths[redisFuzzChoice(
            in, sizeof(repeated_lengths) / sizeof(repeated_lengths[0]))];
        unsigned char pattern = redisFuzzByte(in);
        for (size_t i = 0; i < length; i++)
            repeated[i] = (i & 1) ? (unsigned char)~pattern : pattern;
        value = sdsnewlen(repeated, length);
        break;
    }
    default: {
        size_t selected = (size_t)redisFuzzChoice(
            in, sizeof(binary_lengths) / sizeof(binary_lengths[0]));
        value = sdsnewlen(binary_values[selected], binary_lengths[selected]);
        break;
    }
    }

    redisFuzzAppendBulkSds(resp, value);
    return value;
}

static void append_push(sds *resp, RedisFuzzInput *in, ListFuzzModel *model,
                        int left)
{
    int values = 1 + (int)redisFuzzChoice(in, 4);
    redisFuzzAppendArray(resp, 2 + values);
    redisFuzzAppendBulkCString(resp, left ? "LPUSH" : "RPUSH");
    size_t key_index = append_list_key(resp, in);
    for (int i = 0; i < values; i++) {
        sds value = append_list_value(resp, in);
        listFuzzPush(&model->keys[key_index], value,
                     left ? LIST_HEAD : LIST_TAIL);
        sdsfree(value);
    }
}

static void append_pop(sds *resp, RedisFuzzInput *in, ListFuzzModel *model) {
    int where = redisFuzzChoice(in, 2) ? LIST_HEAD : LIST_TAIL;
    int with_count = (int)redisFuzzChoice(in, 2);
    int valid = 1;
    long count = 1;

    redisFuzzAppendArray(resp, with_count ? 3 : 2);
    redisFuzzAppendBulkCString(
        resp, where == LIST_HEAD ? "LPOP" : "RPOP");
    size_t key_index = append_list_key(resp, in);
    if (with_count) count = append_any_count(resp, in, &valid);
    if (valid) listFuzzPop(&model->keys[key_index], where, count);
}

static void append_trim(sds *resp, RedisFuzzInput *in, ListFuzzModel *model) {
    int valid_start, valid_end;
    redisFuzzAppendArray(resp, 4);
    redisFuzzAppendBulkCString(resp, "LTRIM");
    size_t key_index = append_list_key(resp, in);
    long start = append_list_index(resp, in, &valid_start);
    long end = append_list_index(resp, in, &valid_end);
    if (valid_start && valid_end)
        listFuzzTrim(&model->keys[key_index], start, end);
}

static void append_del(sds *resp, RedisFuzzInput *in, ListFuzzModel *model) {
    int keys = 1 + (int)redisFuzzChoice(in, 3);
    redisFuzzAppendArray(resp, 1 + keys);
    redisFuzzAppendBulkCString(resp, "DEL");
    for (int i = 0; i < keys; i++) {
        size_t key_index = append_list_key(resp, in);
        listFuzzKeyClear(&model->keys[key_index]);
    }
}

static void append_wrong_type(sds *resp, RedisFuzzInput *in,
                              ListFuzzModel *model)
{
    redisFuzzAppendArray(resp, 3);
    redisFuzzAppendBulkCString(resp, "SET");
    size_t key_index = append_list_key(resp, in);
    sds value = append_list_value(resp, in);
    listFuzzSetString(&model->keys[key_index], value);
    sdsfree(value);
}

static void append_linsert(sds *resp, RedisFuzzInput *in,
                           ListFuzzModel *model)
{
    int after = (int)redisFuzzChoice(in, 2);
    redisFuzzAppendArray(resp, 5);
    redisFuzzAppendBulkCString(resp, "LINSERT");
    size_t key_index = append_list_key(resp, in);
    redisFuzzAppendBulkCString(resp, after ? "AFTER" : "BEFORE");
    sds pivot = append_list_value(resp, in);
    sds value = append_list_value(resp, in);
    listFuzzInsertAroundPivot(&model->keys[key_index], pivot, value, after);
    sdsfree(pivot);
    sdsfree(value);
}

static void append_lmove(sds *resp, RedisFuzzInput *in, ListFuzzModel *model) {
    redisFuzzAppendArray(resp, 5);
    redisFuzzAppendBulkCString(resp, "LMOVE");
    size_t source = append_list_key(resp, in);
    size_t destination = append_list_key(resp, in);
    int wherefrom = append_direction(resp, in);
    int whereto = append_direction(resp, in);
    listFuzzMove(model, source, destination, wherefrom, whereto,
                 LIST_FUZZ_DEFAULT, 1, LIST_FUZZ_BULK, 0);
}

static void append_lmovem_default(sds *resp, RedisFuzzInput *in,
                                  ListFuzzModel *model)
{
    redisFuzzAppendArray(resp, 5);
    redisFuzzAppendBulkCString(resp, "LMOVEM");
    size_t source = append_list_key(resp, in);
    size_t destination = append_list_key(resp, in);
    int wherefrom = append_direction(resp, in);
    int whereto = append_direction(resp, in);
    listFuzzMove(model, source, destination, wherefrom, whereto,
                 LIST_FUZZ_DEFAULT, 1, LIST_FUZZ_BULK, 0);
}

static void append_lmovem_options(sds *resp, RedisFuzzInput *in,
                                  ListFuzzModel *model, int same_key)
{
    size_t source, destination;
    redisFuzzAppendArray(resp, 8);
    redisFuzzAppendBulkCString(resp, "LMOVEM");
    if (same_key) {
        source = destination =
            (size_t)redisFuzzChoice(in, LIST_FUZZ_KEYS);
        append_key_index(resp, source);
        append_key_index(resp, destination);
    } else {
        source = append_list_key(resp, in);
        destination = append_list_key(resp, in);
    }
    int wherefrom = append_direction(resp, in);
    int whereto = append_direction(resp, in);
    ListFuzzMoveMode mode = redisFuzzChoice(in, 2) ?
                            LIST_FUZZ_EXACTLY : LIST_FUZZ_COUNT;
    redisFuzzAppendBulkCString(
        resp, mode == LIST_FUZZ_EXACTLY ? "EXACTLY" : "COUNT");
    long count = append_positive_count(resp, in);
    ListFuzzMoveOrdering ordering = redisFuzzChoice(in, 2) ?
                                    LIST_FUZZ_BULK : LIST_FUZZ_OBO;
    redisFuzzAppendBulkCString(
        resp, ordering == LIST_FUZZ_BULK ? "BULK" : "OBO");
    listFuzzMove(model, source, destination, wherefrom, whereto,
                 mode, count, ordering, 0);
}

static int append_blmovem(sds *resp, RedisFuzzInput *in,
                          ListFuzzModel *model, int same_key)
{
    static const char *timeouts[] = {"0", "0.000001", "0.001"};
    int with_options = (int)redisFuzzChoice(in, 2);
    size_t source, destination;

    redisFuzzAppendArray(resp, with_options ? 9 : 6);
    redisFuzzAppendBulkCString(resp, "BLMOVEM");
    if (same_key) {
        source = destination =
            (size_t)redisFuzzChoice(in, LIST_FUZZ_KEYS);
        append_key_index(resp, source);
        append_key_index(resp, destination);
    } else {
        source = append_list_key(resp, in);
        destination = append_list_key(resp, in);
    }
    int wherefrom = append_direction(resp, in);
    int whereto = append_direction(resp, in);
    redisFuzzAppendBulkCString(resp, timeouts[redisFuzzChoice(
        in, sizeof(timeouts) / sizeof(timeouts[0]))]);

    ListFuzzMoveMode mode = LIST_FUZZ_DEFAULT;
    ListFuzzMoveOrdering ordering = LIST_FUZZ_BULK;
    long count = 1;
    if (with_options) {
        mode = redisFuzzChoice(in, 2) ?
               LIST_FUZZ_EXACTLY : LIST_FUZZ_COUNT;
        redisFuzzAppendBulkCString(
            resp, mode == LIST_FUZZ_EXACTLY ? "EXACTLY" : "COUNT");
        count = append_positive_count(resp, in);
        ordering = redisFuzzChoice(in, 2) ?
                   LIST_FUZZ_BULK : LIST_FUZZ_OBO;
        redisFuzzAppendBulkCString(
            resp, ordering == LIST_FUZZ_BULK ? "BULK" : "OBO");
    }
    return listFuzzMove(model, source, destination, wherefrom, whereto,
                        mode, count, ordering, 1);
}

static void append_invalid_lmovem(sds *resp, RedisFuzzInput *in) {
    size_t variant = (size_t)redisFuzzChoice(in, 10);
    if (variant == 0) {
        redisFuzzAppendArray(resp, 4);
        redisFuzzAppendBulkCString(resp, "LMOVEM");
        append_key_index(resp, 0);
        append_key_index(resp, 1);
        redisFuzzAppendBulkCString(resp, "LEFT");
        return;
    }
    if (variant == 1) {
        redisFuzzAppendArray(resp, 7);
        redisFuzzAppendBulkCString(resp, "LMOVEM");
        append_key_index(resp, 0);
        append_key_index(resp, 1);
        redisFuzzAppendBulkCString(resp, "LEFT");
        redisFuzzAppendBulkCString(resp, "RIGHT");
        redisFuzzAppendBulkCString(resp, "COUNT");
        redisFuzzAppendBulkCString(resp, "2");
        return;
    }
    if (variant == 2) {
        redisFuzzAppendArray(resp, 9);
        redisFuzzAppendBulkCString(resp, "LMOVEM");
        append_key_index(resp, 0);
        append_key_index(resp, 1);
        redisFuzzAppendBulkCString(resp, "LEFT");
        redisFuzzAppendBulkCString(resp, "RIGHT");
        redisFuzzAppendBulkCString(resp, "COUNT");
        redisFuzzAppendBulkCString(resp, "2");
        redisFuzzAppendBulkCString(resp, "BULK");
        redisFuzzAppendBulkCString(resp, "EXTRA");
        return;
    }

    redisFuzzAppendArray(resp, 8);
    redisFuzzAppendBulkCString(resp, "LMOVEM");
    append_key_index(resp, 0);
    append_key_index(resp, 1);
    redisFuzzAppendBulkCString(resp, variant == 6 ? "MIDDLE" : "LEFT");
    redisFuzzAppendBulkCString(resp, variant == 7 ? "MIDDLE" : "RIGHT");
    redisFuzzAppendBulkCString(resp, variant == 3 ? "SOME" :
                                    variant == 9 ? "EXACTLY" : "COUNT");
    redisFuzzAppendBulkCString(resp, variant == 4 ? "0" :
                                    variant == 8 ? "-1" :
                                    variant == 9 ? "not-a-count" : "2");
    redisFuzzAppendBulkCString(resp, variant == 5 ? "ORDER" : "BULK");
}

static void append_invalid_blmovem(sds *resp, RedisFuzzInput *in) {
    size_t variant = (size_t)redisFuzzChoice(in, 13);
    if (variant == 0) {
        redisFuzzAppendArray(resp, 5);
        redisFuzzAppendBulkCString(resp, "BLMOVEM");
        append_key_index(resp, 0);
        append_key_index(resp, 1);
        redisFuzzAppendBulkCString(resp, "LEFT");
        redisFuzzAppendBulkCString(resp, "RIGHT");
        return;
    }
    if (variant == 1) {
        redisFuzzAppendArray(resp, 8);
        redisFuzzAppendBulkCString(resp, "BLMOVEM");
        append_key_index(resp, 0);
        append_key_index(resp, 1);
        redisFuzzAppendBulkCString(resp, "LEFT");
        redisFuzzAppendBulkCString(resp, "RIGHT");
        redisFuzzAppendBulkCString(resp, "0");
        redisFuzzAppendBulkCString(resp, "COUNT");
        redisFuzzAppendBulkCString(resp, "2");
        return;
    }
    if (variant == 2) {
        redisFuzzAppendArray(resp, 10);
        redisFuzzAppendBulkCString(resp, "BLMOVEM");
        append_key_index(resp, 0);
        append_key_index(resp, 1);
        redisFuzzAppendBulkCString(resp, "LEFT");
        redisFuzzAppendBulkCString(resp, "RIGHT");
        redisFuzzAppendBulkCString(resp, "0");
        redisFuzzAppendBulkCString(resp, "COUNT");
        redisFuzzAppendBulkCString(resp, "2");
        redisFuzzAppendBulkCString(resp, "BULK");
        redisFuzzAppendBulkCString(resp, "EXTRA");
        return;
    }

    redisFuzzAppendArray(resp, 9);
    redisFuzzAppendBulkCString(resp, "BLMOVEM");
    append_key_index(resp, 0);
    append_key_index(resp, 1);
    redisFuzzAppendBulkCString(resp, variant == 6 ? "MIDDLE" : "LEFT");
    redisFuzzAppendBulkCString(resp, variant == 7 ? "MIDDLE" : "RIGHT");
    redisFuzzAppendBulkCString(resp, variant == 3 ? "not-a-timeout" :
                                    variant == 4 ? "-1" :
                                    variant == 8 ? "nan" : "0");
    redisFuzzAppendBulkCString(resp, variant == 5 ? "SOME" :
                                    variant == 11 ? "EXACTLY" : "COUNT");
    redisFuzzAppendBulkCString(resp, variant == 9 ? "0" :
                                    variant == 10 ? "-1" :
                                    variant == 11 ? "not-a-count" : "2");
    redisFuzzAppendBulkCString(resp, variant == 12 ? "ORDER" : "BULK");
}

static void append_inspection(sds *resp, RedisFuzzInput *in) {
    switch (redisFuzzChoice(in, 3)) {
    case 0:
        redisFuzzAppendArray(resp, 2);
        redisFuzzAppendBulkCString(resp, "LLEN");
        append_list_key(resp, in);
        break;
    case 1:
        redisFuzzAppendArray(resp, 4);
        redisFuzzAppendBulkCString(resp, "LRANGE");
        append_list_key(resp, in);
        redisFuzzAppendSmallNumber(resp, in);
        redisFuzzAppendSmallNumber(resp, in);
        break;
    default:
        redisFuzzAppendArray(resp, 2);
        redisFuzzAppendBulkCString(resp, "TYPE");
        append_list_key(resp, in);
        break;
    }
}

static void listFuzzAssertModel(client *c, void *ctx) {
    ListFuzzModel *model = ctx;
    listFuzzCheck(!!(c->flags & CLIENT_BLOCKED) == !!model->blocked);

    for (size_t i = 0; i < LIST_FUZZ_KEYS; i++) {
        ListFuzzKey *expected = &model->keys[i];
        robj key_object;
        initStaticStringObject(key_object, expected->name);
        kvobj *actual = lookupKeyReadWithFlags(
            c->db, &key_object, LOOKUP_NOEFFECTS);

        if (expected->type == LIST_FUZZ_NONE) {
            listFuzzCheck(actual == NULL);
            continue;
        }
        listFuzzCheck(actual != NULL);
        if (expected->type == LIST_FUZZ_STRING) {
            robj expected_object;
            initStaticStringObject(expected_object, expected->string_value);
            listFuzzCheck(actual->type == OBJ_STRING);
            listFuzzCheck(equalStringObjects(actual, &expected_object));
            continue;
        }

        listFuzzCheck(actual->type == OBJ_LIST);
        listFuzzCheck(listTypeLength(actual) == expected->count);
        listTypeIterator iterator;
        listTypeEntry entry;
        listTypeInitIterator(&iterator, actual, 0, LIST_TAIL);
        for (size_t j = 0; j < expected->count; j++) {
            listFuzzCheck(listTypeNext(&iterator, &entry));
            robj *actual_element = listTypeGet(&entry);
            robj expected_element;
            initStaticStringObject(expected_element, expected->elements[j]);
            listFuzzCheck(equalStringObjects(actual_element,
                                             &expected_element));
            decrRefCount(actual_element);
        }
        listFuzzCheck(!listTypeNext(&iterator, &entry));
        listTypeResetIterator(&iterator);
    }
}

static void assert_blocking_state_clean(void) {
    listFuzzCheck(server.blocked_clients == 0);
    listFuzzCheck(listLength(server.unblocked_clients) == 0);
    listFuzzCheck(listLength(server.ready_keys) == 0);

    for (int type = 0; type < BLOCKED_NUM; type++)
        listFuzzCheck(server.blocked_clients_by_type[type] == 0);
    for (int dbid = 0; dbid < server.dbnum; dbid++) {
        listFuzzCheck(dictSize(server.db[dbid].blocking_keys) == 0);
        listFuzzCheck(dictSize(
            server.db[dbid].blocking_keys_unblock_on_nokey) == 0);
        listFuzzCheck(dictSize(server.db[dbid].ready_keys) == 0);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    RedisFuzzInput in = {data, size, 0};
    ListFuzzModel model;
    sds resp = sdsempty();
    int commands = 1 + (int)redisFuzzChoice(&in, 24);

    listFuzzModelInit(&model);
    for (int i = 0; i < commands && !model.blocked; i++) {
        switch (redisFuzzChoice(&in, 16)) {
        case 0: append_push(&resp, &in, &model, 1); break;
        case 1: append_push(&resp, &in, &model, 0); break;
        case 2: append_pop(&resp, &in, &model); break;
        case 3: append_trim(&resp, &in, &model); break;
        case 4: append_del(&resp, &in, &model); break;
        case 5: append_wrong_type(&resp, &in, &model); break;
        case 6: append_linsert(&resp, &in, &model); break;
        case 7: append_lmove(&resp, &in, &model); break;
        case 8: append_lmovem_default(&resp, &in, &model); break;
        case 9: append_lmovem_options(&resp, &in, &model, 0); break;
        case 10: append_lmovem_options(&resp, &in, &model, 1); break;
        case 11: append_blmovem(&resp, &in, &model, 0); break;
        case 12: append_blmovem(&resp, &in, &model, 1); break;
        case 13: append_invalid_lmovem(&resp, &in); break;
        case 14: append_invalid_blmovem(&resp, &in); break;
        default: append_inspection(&resp, &in); break;
        }
    }

    redisFuzzRunRespWithInspect(resp, listFuzzAssertModel, &model);
    listFuzzModelFree(&model);
    assert_blocking_state_clean();
    return 0;
}

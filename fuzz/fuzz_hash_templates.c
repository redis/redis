#include "redis_fuzz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HASH_FUZZ_MUTABLE_KEYS 4
#define HASH_FUZZ_SHARED_KEYS 2
#define HASH_FUZZ_KEYS (HASH_FUZZ_MUTABLE_KEYS + HASH_FUZZ_SHARED_KEYS)
#define HASH_FUZZ_FIELDSETS 4
#define HASH_FUZZ_MAX_FIELDS 32
#define HASH_FUZZ_MAX_FIELDSET_FIELDS 8
#define HASH_FUZZ_MAX_TOKEN 96

static const char *hash_fuzz_key_names[HASH_FUZZ_KEYS] = {
    "hash:0", "hash:1", "hash:2", "hash:3",
    "hash:shared:0", "hash:shared:1"
};

static const char *hash_fuzz_fieldset_names[HASH_FUZZ_FIELDSETS] = {
    "fieldset:0", "fieldset:1", "fieldset:2", "fieldset:3"
};

typedef enum HashFuzzValueType {
    HASH_FUZZ_NONE,
    HASH_FUZZ_STRING,
    HASH_FUZZ_HASH
} HashFuzzValueType;

typedef struct HashFuzzPair {
    sds field;
    sds value;
} HashFuzzPair;

typedef struct HashFuzzKey {
    sds name;
    HashFuzzValueType type;
    sds string_value;
    size_t count;
    HashFuzzPair pairs[HASH_FUZZ_MAX_FIELDS];
} HashFuzzKey;

typedef struct HashFuzzFieldset {
    int prepared;
    size_t count;
    sds fields[HASH_FUZZ_MAX_FIELDSET_FIELDS];
} HashFuzzFieldset;

typedef struct HashFuzzModel {
    HashFuzzKey keys[HASH_FUZZ_KEYS];
    HashFuzzFieldset fieldsets[HASH_FUZZ_FIELDSETS];
} HashFuzzModel;

static __attribute__((noreturn, noinline))
void hashFuzzFail(const char *condition, int line) {
    fprintf(stderr, "hash template model mismatch at line %d: %s\n",
            line, condition);
    fflush(stderr);
    abort();
}

#define hashFuzzCheck(condition) do { \
    if (!(condition)) hashFuzzFail(#condition, __LINE__); \
} while (0)

static void hashFuzzKeyClear(HashFuzzKey *key) {
    sdsfree(key->string_value);
    key->string_value = NULL;
    for (size_t i = 0; i < key->count; i++) {
        sdsfree(key->pairs[i].field);
        sdsfree(key->pairs[i].value);
    }
    key->count = 0;
    key->type = HASH_FUZZ_NONE;
}

static void hashFuzzFieldsetClear(HashFuzzFieldset *fieldset) {
    for (size_t i = 0; i < fieldset->count; i++)
        sdsfree(fieldset->fields[i]);
    fieldset->count = 0;
    fieldset->prepared = 0;
}

static void hashFuzzModelInit(HashFuzzModel *model) {
    memset(model, 0, sizeof(*model));
    for (size_t i = 0; i < HASH_FUZZ_KEYS; i++)
        model->keys[i].name = sdsnew(hash_fuzz_key_names[i]);
}

static void hashFuzzModelFree(HashFuzzModel *model) {
    for (size_t i = 0; i < HASH_FUZZ_KEYS; i++) {
        hashFuzzKeyClear(&model->keys[i]);
        sdsfree(model->keys[i].name);
    }
    for (size_t i = 0; i < HASH_FUZZ_FIELDSETS; i++)
        hashFuzzFieldsetClear(&model->fieldsets[i]);
}

static void hashFuzzFieldsetSet(HashFuzzFieldset *fieldset, sds *fields,
                                size_t count)
{
    hashFuzzFieldsetClear(fieldset);
    hashFuzzCheck(count <= HASH_FUZZ_MAX_FIELDSET_FIELDS);
    for (size_t i = 0; i < count; i++)
        fieldset->fields[i] = sdsdup(fields[i]);
    fieldset->count = count;
    fieldset->prepared = 1;
}

static void hashFuzzKeySetString(HashFuzzKey *key, sds value) {
    hashFuzzKeyClear(key);
    key->type = HASH_FUZZ_STRING;
    key->string_value = sdsdup(value);
}

static void hashFuzzKeySetHash(HashFuzzKey *key, sds *fields, sds *values,
                               size_t count)
{
    hashFuzzKeyClear(key);
    hashFuzzCheck(count <= HASH_FUZZ_MAX_FIELDS);
    key->type = HASH_FUZZ_HASH;
    for (size_t i = 0; i < count; i++) {
        key->pairs[i].field = sdsdup(fields[i]);
        key->pairs[i].value = sdsdup(values[i]);
    }
    key->count = count;
}

static long hashFuzzFindField(const HashFuzzKey *key, sds field) {
    for (size_t i = 0; i < key->count; i++) {
        if (sdscmplen(key->pairs[i].field, field) == 0)
            return (long)i;
    }
    return -1;
}

static void hashFuzzKeySetField(HashFuzzKey *key, sds field, sds value) {
    if (key->type == HASH_FUZZ_STRING) return;
    if (key->type == HASH_FUZZ_NONE) key->type = HASH_FUZZ_HASH;

    long index = hashFuzzFindField(key, field);
    if (index >= 0) {
        sdsfree(key->pairs[index].value);
        key->pairs[index].value = sdsdup(value);
        return;
    }

    hashFuzzCheck(key->count < HASH_FUZZ_MAX_FIELDS);
    key->pairs[key->count].field = sdsdup(field);
    key->pairs[key->count].value = sdsdup(value);
    key->count++;
}

static void hashFuzzKeyDeleteField(HashFuzzKey *key, sds field) {
    if (key->type != HASH_FUZZ_HASH) return;

    long index = hashFuzzFindField(key, field);
    if (index < 0) return;

    sdsfree(key->pairs[index].field);
    sdsfree(key->pairs[index].value);
    key->count--;
    if ((size_t)index != key->count)
        key->pairs[index] = key->pairs[key->count];
    if (key->count == 0)
        key->type = HASH_FUZZ_NONE;
}

static void hashFuzzKeyCopy(HashFuzzKey *destination,
                            const HashFuzzKey *source)
{
    hashFuzzKeyClear(destination);
    if (source->type == HASH_FUZZ_STRING) {
        destination->type = HASH_FUZZ_STRING;
        destination->string_value = sdsdup(source->string_value);
    } else if (source->type == HASH_FUZZ_HASH) {
        destination->type = HASH_FUZZ_HASH;
        for (size_t i = 0; i < source->count; i++) {
            destination->pairs[i].field = sdsdup(source->pairs[i].field);
            destination->pairs[i].value = sdsdup(source->pairs[i].value);
        }
        destination->count = source->count;
    }
}

static sds hashFuzzTaggedSlice(RedisFuzzInput *in, uint8_t tag) {
    sds suffix = redisFuzzSlice(in, HASH_FUZZ_MAX_TOKEN - 1);
    sds value = sdsnewlen(&tag, 1);
    value = sdscatsds(value, suffix);
    sdsfree(suffix);
    return value;
}

static sds hashFuzzFieldForKey(RedisFuzzInput *in, const HashFuzzKey *key) {
    if (key->type == HASH_FUZZ_HASH && key->count &&
        redisFuzzChoice(in, 3) == 0)
    {
        return sdsdup(key->pairs[redisFuzzChoice(in, key->count)].field);
    }
    return hashFuzzTaggedSlice(in, (uint8_t)(1 + redisFuzzChoice(in, 254)));
}

static void hashFuzzAppendKey(sds *resp, size_t key_index) {
    redisFuzzAppendBulkCString(resp, hash_fuzz_key_names[key_index]);
}

static void hashFuzzAppendFieldset(sds *resp, size_t fieldset_index) {
    redisFuzzAppendBulkCString(resp, hash_fuzz_fieldset_names[fieldset_index]);
}

static void hashFuzzAppendConfig(sds *resp, RedisFuzzInput *in) {
    static const char *entry_limits[] = {"512", "0", "1", "4"};
    static const char *value_limits[] = {"64", "0", "8", "256"};
    static const char *template_limits[] = {"0", "1", "2", "8"};

    redisFuzzAppendArray(resp, 4);
    redisFuzzAppendBulkCString(resp, "CONFIG");
    redisFuzzAppendBulkCString(resp, "SET");
    redisFuzzAppendBulkCString(resp, "hash-max-listpack-entries");
    redisFuzzAppendBulkCString(
        resp, entry_limits[redisFuzzChoice(in, sizeof(entry_limits) /
                                                  sizeof(entry_limits[0]))]);

    redisFuzzAppendArray(resp, 4);
    redisFuzzAppendBulkCString(resp, "CONFIG");
    redisFuzzAppendBulkCString(resp, "SET");
    redisFuzzAppendBulkCString(resp, "hash-max-listpack-value");
    redisFuzzAppendBulkCString(
        resp, value_limits[redisFuzzChoice(in, sizeof(value_limits) /
                                                  sizeof(value_limits[0]))]);

    redisFuzzAppendArray(resp, 4);
    redisFuzzAppendBulkCString(resp, "CONFIG");
    redisFuzzAppendBulkCString(resp, "SET");
    redisFuzzAppendBulkCString(resp, "hash-min-template-entries");
    redisFuzzAppendBulkCString(
        resp, template_limits[redisFuzzChoice(in, sizeof(template_limits) /
                                                     sizeof(template_limits[0]))]);
}

static void hashFuzzAppendPrepare(sds *resp, size_t fieldset_index,
                                  sds *fields, size_t count)
{
    redisFuzzAppendArray(resp, 3 + (int)count);
    redisFuzzAppendBulkCString(resp, "HIMPORT");
    redisFuzzAppendBulkCString(resp, "PREPARE");
    hashFuzzAppendFieldset(resp, fieldset_index);
    for (size_t i = 0; i < count; i++)
        redisFuzzAppendBulkSds(resp, fields[i]);
}

static void hashFuzzAppendImportSet(sds *resp, size_t key_index,
                                    size_t fieldset_index, sds *values,
                                    size_t count)
{
    redisFuzzAppendArray(resp, 4 + (int)count);
    redisFuzzAppendBulkCString(resp, "HIMPORT");
    redisFuzzAppendBulkCString(resp, "SET");
    hashFuzzAppendKey(resp, key_index);
    hashFuzzAppendFieldset(resp, fieldset_index);
    for (size_t i = 0; i < count; i++)
        redisFuzzAppendBulkSds(resp, values[i]);
}

static void hashFuzzSeedSharedTemplate(sds *resp, RedisFuzzInput *in,
                                       HashFuzzModel *model)
{
    sds fields[4];
    sds reversed_fields[4];
    sds values[4];
    sds reversed_values[4];

    fields[0] = sdsempty();
    fields[1] = sdsnew("a");
    fields[2] = hashFuzzTaggedSlice(in, 0x80);
    fields[3] = hashFuzzTaggedSlice(in, 0x81);
    for (size_t i = 0; i < 4; i++)
        values[i] = redisFuzzSlice(in, HASH_FUZZ_MAX_TOKEN);
    for (size_t i = 0; i < 4; i++) {
        reversed_fields[i] = fields[3 - i];
        reversed_values[i] = values[3 - i];
    }

    hashFuzzAppendPrepare(resp, 0, fields, 4);
    hashFuzzAppendPrepare(resp, 1, reversed_fields, 4);
    hashFuzzAppendImportSet(resp, HASH_FUZZ_MUTABLE_KEYS, 0, values, 4);
    hashFuzzAppendImportSet(resp, HASH_FUZZ_MUTABLE_KEYS + 1, 1,
                            reversed_values, 4);

    hashFuzzFieldsetSet(&model->fieldsets[0], fields, 4);
    hashFuzzFieldsetSet(&model->fieldsets[1], reversed_fields, 4);
    hashFuzzKeySetHash(&model->keys[HASH_FUZZ_MUTABLE_KEYS],
                       fields, values, 4);
    hashFuzzKeySetHash(&model->keys[HASH_FUZZ_MUTABLE_KEYS + 1],
                       reversed_fields, reversed_values, 4);

    for (size_t i = 0; i < 4; i++) {
        sdsfree(fields[i]);
        sdsfree(values[i]);
    }
}

static void hashFuzzPrepareNew(sds *resp, RedisFuzzInput *in,
                               HashFuzzModel *model)
{
    size_t fieldset_index = redisFuzzChoice(in, HASH_FUZZ_FIELDSETS);
    size_t count = 1 + redisFuzzChoice(in, HASH_FUZZ_MAX_FIELDSET_FIELDS);
    sds fields[HASH_FUZZ_MAX_FIELDSET_FIELDS];

    for (size_t i = 0; i < count; i++) {
        if (i == 0 && redisFuzzChoice(in, 6) == 0)
            fields[i] = sdsempty();
        else
            fields[i] = hashFuzzTaggedSlice(in, (uint8_t)(i + 1));
    }

    hashFuzzAppendPrepare(resp, fieldset_index, fields, count);
    hashFuzzFieldsetSet(&model->fieldsets[fieldset_index], fields, count);
    for (size_t i = 0; i < count; i++)
        sdsfree(fields[i]);
}

static void hashFuzzPrepareReordered(sds *resp, RedisFuzzInput *in,
                                     HashFuzzModel *model)
{
    size_t source_index = redisFuzzChoice(in, HASH_FUZZ_FIELDSETS);
    HashFuzzFieldset *source = &model->fieldsets[source_index];
    if (!source->prepared) {
        hashFuzzPrepareNew(resp, in, model);
        return;
    }

    size_t destination_index = redisFuzzChoice(in, HASH_FUZZ_FIELDSETS);
    sds fields[HASH_FUZZ_MAX_FIELDSET_FIELDS];
    for (size_t i = 0; i < source->count; i++)
        fields[i] = sdsdup(source->fields[source->count - 1 - i]);

    hashFuzzAppendPrepare(resp, destination_index, fields, source->count);
    hashFuzzFieldsetSet(&model->fieldsets[destination_index],
                        fields, source->count);
    for (size_t i = 0; i < source->count; i++)
        sdsfree(fields[i]);
}

static void hashFuzzPrepareDuplicate(sds *resp, RedisFuzzInput *in) {
    size_t fieldset_index = redisFuzzChoice(in, HASH_FUZZ_FIELDSETS);
    sds duplicate = redisFuzzSlice(in, HASH_FUZZ_MAX_TOKEN);
    sds fields[3] = {
        duplicate,
        duplicate,
        hashFuzzTaggedSlice(in, 0xfe)
    };

    hashFuzzAppendPrepare(resp, fieldset_index, fields, 3);
    sdsfree(fields[2]);
    sdsfree(duplicate);
}

static void hashFuzzImportSet(sds *resp, RedisFuzzInput *in,
                              HashFuzzModel *model, int mismatch)
{
    size_t key_index = redisFuzzChoice(in, HASH_FUZZ_MUTABLE_KEYS);
    size_t fieldset_index = redisFuzzChoice(in, HASH_FUZZ_FIELDSETS);
    HashFuzzFieldset *fieldset = &model->fieldsets[fieldset_index];
    size_t count;

    if (!fieldset->prepared) {
        count = 1 + redisFuzzChoice(in, 3);
    } else if (mismatch) {
        count = fieldset->count == 1 ? 2 : fieldset->count - 1;
    } else {
        count = fieldset->count;
    }

    sds values[HASH_FUZZ_MAX_FIELDSET_FIELDS + 1];
    for (size_t i = 0; i < count; i++)
        values[i] = redisFuzzSlice(in, HASH_FUZZ_MAX_TOKEN);

    hashFuzzAppendImportSet(resp, key_index, fieldset_index, values, count);
    if (fieldset->prepared && !mismatch &&
        model->keys[key_index].type != HASH_FUZZ_STRING)
    {
        hashFuzzKeySetHash(&model->keys[key_index], fieldset->fields,
                           values, count);
    }
    for (size_t i = 0; i < count; i++)
        sdsfree(values[i]);
}

static void hashFuzzDiscard(sds *resp, RedisFuzzInput *in,
                            HashFuzzModel *model)
{
    size_t fieldset_index = redisFuzzChoice(in, HASH_FUZZ_FIELDSETS);
    redisFuzzAppendArray(resp, 3);
    redisFuzzAppendBulkCString(resp, "HIMPORT");
    redisFuzzAppendBulkCString(resp, "DISCARD");
    hashFuzzAppendFieldset(resp, fieldset_index);
    hashFuzzFieldsetClear(&model->fieldsets[fieldset_index]);
}

static void hashFuzzDiscardAll(sds *resp, HashFuzzModel *model) {
    redisFuzzAppendArray(resp, 2);
    redisFuzzAppendBulkCString(resp, "HIMPORT");
    redisFuzzAppendBulkCString(resp, "DISCARDALL");
    for (size_t i = 0; i < HASH_FUZZ_FIELDSETS; i++)
        hashFuzzFieldsetClear(&model->fieldsets[i]);
}

static void hashFuzzClientReset(sds *resp, HashFuzzModel *model) {
    redisFuzzAppendArray(resp, 1);
    redisFuzzAppendBulkCString(resp, "RESET");
    for (size_t i = 0; i < HASH_FUZZ_FIELDSETS; i++)
        hashFuzzFieldsetClear(&model->fieldsets[i]);
}

static void hashFuzzHset(sds *resp, RedisFuzzInput *in,
                         HashFuzzModel *model)
{
    size_t key_index = redisFuzzChoice(in, HASH_FUZZ_MUTABLE_KEYS);
    sds field = hashFuzzFieldForKey(in, &model->keys[key_index]);
    sds value = redisFuzzSlice(in, HASH_FUZZ_MAX_TOKEN);

    redisFuzzAppendArray(resp, 4);
    redisFuzzAppendBulkCString(resp, "HSET");
    hashFuzzAppendKey(resp, key_index);
    redisFuzzAppendBulkSds(resp, field);
    redisFuzzAppendBulkSds(resp, value);
    hashFuzzKeySetField(&model->keys[key_index], field, value);

    sdsfree(field);
    sdsfree(value);
}

static void hashFuzzHdel(sds *resp, RedisFuzzInput *in,
                         HashFuzzModel *model)
{
    size_t key_index = redisFuzzChoice(in, HASH_FUZZ_MUTABLE_KEYS);
    sds field = hashFuzzFieldForKey(in, &model->keys[key_index]);

    redisFuzzAppendArray(resp, 3);
    redisFuzzAppendBulkCString(resp, "HDEL");
    hashFuzzAppendKey(resp, key_index);
    redisFuzzAppendBulkSds(resp, field);
    hashFuzzKeyDeleteField(&model->keys[key_index], field);

    sdsfree(field);
}

static void hashFuzzHget(sds *resp, RedisFuzzInput *in,
                         const HashFuzzModel *model)
{
    size_t key_index = redisFuzzChoice(in, HASH_FUZZ_KEYS);
    sds field = hashFuzzFieldForKey(in, &model->keys[key_index]);

    redisFuzzAppendArray(resp, 3);
    redisFuzzAppendBulkCString(resp, "HGET");
    hashFuzzAppendKey(resp, key_index);
    redisFuzzAppendBulkSds(resp, field);

    sdsfree(field);
}

static void hashFuzzHgetall(sds *resp, RedisFuzzInput *in) {
    redisFuzzAppendArray(resp, 2);
    redisFuzzAppendBulkCString(resp, "HGETALL");
    hashFuzzAppendKey(resp, redisFuzzChoice(in, HASH_FUZZ_KEYS));
}

static void hashFuzzCopy(sds *resp, RedisFuzzInput *in,
                         HashFuzzModel *model)
{
    size_t source_index = redisFuzzChoice(in, HASH_FUZZ_KEYS);
    size_t destination_index = redisFuzzChoice(in, HASH_FUZZ_MUTABLE_KEYS);
    int replace = redisFuzzChoice(in, 2);

    redisFuzzAppendArray(resp, replace ? 4 : 3);
    redisFuzzAppendBulkCString(resp, "COPY");
    hashFuzzAppendKey(resp, source_index);
    hashFuzzAppendKey(resp, destination_index);
    if (replace) redisFuzzAppendBulkCString(resp, "REPLACE");

    if (source_index == destination_index ||
        model->keys[source_index].type == HASH_FUZZ_NONE ||
        (!replace && model->keys[destination_index].type != HASH_FUZZ_NONE))
    {
        return;
    }
    hashFuzzKeyCopy(&model->keys[destination_index],
                    &model->keys[source_index]);
}

static void hashFuzzDump(sds *resp, RedisFuzzInput *in) {
    redisFuzzAppendArray(resp, 2);
    redisFuzzAppendBulkCString(resp, "DUMP");
    hashFuzzAppendKey(resp, redisFuzzChoice(in, HASH_FUZZ_KEYS));
}

static void hashFuzzDel(sds *resp, RedisFuzzInput *in,
                        HashFuzzModel *model)
{
    size_t key_index = redisFuzzChoice(in, HASH_FUZZ_MUTABLE_KEYS);
    redisFuzzAppendArray(resp, 2);
    redisFuzzAppendBulkCString(resp, "DEL");
    hashFuzzAppendKey(resp, key_index);
    hashFuzzKeyClear(&model->keys[key_index]);
}

static void hashFuzzSetString(sds *resp, RedisFuzzInput *in,
                              HashFuzzModel *model)
{
    size_t key_index = redisFuzzChoice(in, HASH_FUZZ_MUTABLE_KEYS);
    sds value = redisFuzzSlice(in, HASH_FUZZ_MAX_TOKEN);

    redisFuzzAppendArray(resp, 3);
    redisFuzzAppendBulkCString(resp, "SET");
    hashFuzzAppendKey(resp, key_index);
    redisFuzzAppendBulkSds(resp, value);
    hashFuzzKeySetString(&model->keys[key_index], value);

    sdsfree(value);
}

static void hashFuzzInvalidHimport(sds *resp, RedisFuzzInput *in) {
    switch (redisFuzzChoice(in, 5)) {
    case 0:
        redisFuzzAppendArray(resp, 1);
        redisFuzzAppendBulkCString(resp, "HIMPORT");
        break;
    case 1:
        redisFuzzAppendArray(resp, 2);
        redisFuzzAppendBulkCString(resp, "HIMPORT");
        redisFuzzAppendBulkCString(resp, "UNKNOWN");
        break;
    case 2:
        redisFuzzAppendArray(resp, 3);
        redisFuzzAppendBulkCString(resp, "HIMPORT");
        redisFuzzAppendBulkCString(resp, "PREPARE");
        hashFuzzAppendFieldset(resp,
            redisFuzzChoice(in, HASH_FUZZ_FIELDSETS));
        break;
    case 3:
        redisFuzzAppendArray(resp, 3);
        redisFuzzAppendBulkCString(resp, "HIMPORT");
        redisFuzzAppendBulkCString(resp, "DISCARDALL");
        redisFuzzAppendBulkCString(resp, "extra");
        break;
    default:
        redisFuzzAppendArray(resp, 4);
        redisFuzzAppendBulkCString(resp, "HIMPORT");
        redisFuzzAppendBulkCString(resp, "SET");
        hashFuzzAppendKey(resp,
            redisFuzzChoice(in, HASH_FUZZ_MUTABLE_KEYS));
        hashFuzzAppendFieldset(resp,
            redisFuzzChoice(in, HASH_FUZZ_FIELDSETS));
        break;
    }
}

static void hashFuzzAssertModel(client *c, void *ctx) {
    HashFuzzModel *model = ctx;
    kvobj *shared[HASH_FUZZ_SHARED_KEYS] = {NULL, NULL};

    for (size_t i = 0; i < HASH_FUZZ_KEYS; i++) {
        HashFuzzKey *expected = &model->keys[i];
        robj key_object;
        initStaticStringObject(key_object, expected->name);
        kvobj *actual = lookupKeyReadWithFlags(c->db, &key_object,
                                                LOOKUP_NOEFFECTS);

        if (expected->type == HASH_FUZZ_NONE) {
            hashFuzzCheck(actual == NULL);
            continue;
        }
        hashFuzzCheck(actual != NULL);

        if (expected->type == HASH_FUZZ_STRING) {
            robj expected_object;
            initStaticStringObject(expected_object, expected->string_value);
            hashFuzzCheck(actual->type == OBJ_STRING);
            hashFuzzCheck(equalStringObjects(actual, &expected_object));
            continue;
        }

        hashFuzzCheck(actual->type == OBJ_HASH);
        hashFuzzCheck(hashTypeLength(actual, 0) == expected->count);
        for (size_t j = 0; j < expected->count; j++) {
            robj *actual_value = NULL;
            robj expected_value;
            initStaticStringObject(expected_value, expected->pairs[j].value);
            hashFuzzCheck(hashTypeGetValueObject(
                c->db, actual, expected->pairs[j].field,
                HFE_LAZY_ACCESS_EXPIRED, &actual_value, NULL, NULL));
            hashFuzzCheck(actual_value != NULL);
            hashFuzzCheck(equalStringObjects(actual_value, &expected_value));
            decrRefCount(actual_value);
        }

        if (i >= HASH_FUZZ_MUTABLE_KEYS)
            shared[i - HASH_FUZZ_MUTABLE_KEYS] = actual;
    }

    hashFuzzCheck(shared[0] != NULL && shared[1] != NULL);
    hashFuzzCheck((shared[0]->encoding == OBJ_ENCODING_TMPL_LP ||
                   shared[0]->encoding == OBJ_ENCODING_TMPL_ARRAY));
    hashFuzzCheck((shared[1]->encoding == OBJ_ENCODING_TMPL_LP ||
                   shared[1]->encoding == OBJ_ENCODING_TMPL_ARRAY));
    hashFuzzCheck(hashTypeGetTemplate(shared[0]) ==
                  hashTypeGetTemplate(shared[1]));
    hashFuzzCheck(hashTypeGetTemplate(shared[0])->key_refcount >= 2);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    RedisFuzzInput in = {data, size, 0};
    HashFuzzModel model;
    sds resp = sdsempty();

    hashFuzzModelInit(&model);
    hashFuzzAppendConfig(&resp, &in);
    hashFuzzSeedSharedTemplate(&resp, &in, &model);

    int commands = 1 + (int)redisFuzzChoice(&in, 24);
    for (int i = 0; i < commands; i++) {
        switch (redisFuzzChoice(&in, 16)) {
        case 0: hashFuzzPrepareNew(&resp, &in, &model); break;
        case 1: hashFuzzPrepareReordered(&resp, &in, &model); break;
        case 2: hashFuzzPrepareDuplicate(&resp, &in); break;
        case 3: hashFuzzImportSet(&resp, &in, &model, 0); break;
        case 4: hashFuzzImportSet(&resp, &in, &model, 1); break;
        case 5: hashFuzzDiscard(&resp, &in, &model); break;
        case 6: hashFuzzDiscardAll(&resp, &model); break;
        case 7: hashFuzzClientReset(&resp, &model); break;
        case 8: hashFuzzHset(&resp, &in, &model); break;
        case 9: hashFuzzHdel(&resp, &in, &model); break;
        case 10: hashFuzzHget(&resp, &in, &model); break;
        case 11: hashFuzzHgetall(&resp, &in); break;
        case 12: hashFuzzCopy(&resp, &in, &model); break;
        case 13: hashFuzzDump(&resp, &in); break;
        case 14:
            if (redisFuzzChoice(&in, 2))
                hashFuzzDel(&resp, &in, &model);
            else
                hashFuzzSetString(&resp, &in, &model);
            break;
        default: hashFuzzInvalidHimport(&resp, &in); break;
        }
    }

    redisFuzzRunRespWithInspect(resp, hashFuzzAssertModel, &model);
    hashFuzzModelFree(&model);
    return 0;
}

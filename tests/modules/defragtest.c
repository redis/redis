/* A module that implements defrag callback mechanisms.
 */

#include "redismodule.h"
#include <stdlib.h>
#include <string.h>

#define UNUSED(V) ((void) V)

static RedisModuleType *FragType;

struct FragObject {
    unsigned long len;
    void **values;
    int maxstep;
};

/* Make sure we get the expected cursor */
unsigned long int last_set_cursor = 0;

unsigned long int datatype_attempts = 0;
unsigned long int datatype_defragged = 0;
unsigned long int datatype_raw_defragged = 0;
unsigned long int datatype_resumes = 0;
unsigned long int datatype_wrong_cursor = 0;
unsigned long int defrag_started = 0;
unsigned long int defrag_ended = 0;
unsigned long int global_strings_attempts = 0;
unsigned long int global_strings_defragged = 0;
unsigned long int global_dicts_resumes = 0;  /* Number of dict defragmentation resumed from a previous break */
unsigned long int global_subdicts_resumes = 0;  /* Number of subdict defragmentation resumed from a previous break */
unsigned long int global_dicts_attempts = 0; /* Number of attempts to defragment dictionary */
unsigned long int global_dicts_defragged = 0; /* Number of dictionaries successfully defragmented */
unsigned long int global_dicts_items_defragged = 0; /* Number of dictionaries items successfully defragmented */

unsigned long global_strings_len = 0;
RedisModuleString **global_strings = NULL;

unsigned long global_dicts_len = 0;
RedisModuleDict **global_dicts = NULL;

static void createGlobalStrings(RedisModuleCtx *ctx, unsigned long count)
{
    global_strings_len = count;
    global_strings = RedisModule_Alloc(sizeof(RedisModuleString *) * count);

    for (unsigned long i = 0; i < count; i++) {
        global_strings[i] = RedisModule_CreateStringFromLongLong(ctx, i);
    }
}

static int defragGlobalStrings(RedisModuleDefragCtx *ctx)
{
    unsigned long cursor = 0;
    RedisModule_DefragCursorGet(ctx, &cursor);

    if (!global_strings_len) return 0; /* strings is empty. */
    RedisModule_Assert(cursor < global_strings_len);
    for (; cursor < global_strings_len; cursor++) {
        RedisModuleString *str = global_strings[cursor];
        if (!str) continue;
        RedisModuleString *new = RedisModule_DefragRedisModuleString(ctx, str);
        global_strings_attempts++;
        if (new != NULL) {
            global_strings[cursor] = new;
            global_strings_defragged++;
        }

        if (RedisModule_DefragShouldStop(ctx)) {
            RedisModule_DefragCursorSet(ctx, cursor);
            return 1;
        }
    }
    return 0;
}

static void createFragGlobalStrings(RedisModuleCtx *ctx) {
    for (unsigned long i = 0; i < global_strings_len; i++) {
        if (i % 2 == 1) {
            RedisModule_FreeString(ctx, global_strings[i]);
            global_strings[i] = NULL;
        }
    }
}

static void createGlobalDicts(RedisModuleCtx *ctx, unsigned long count) {
    global_dicts_len = count;
    global_dicts = RedisModule_Alloc(sizeof(RedisModuleDict *) * count);

    /* Create some nested dictionaries:
     * - Each main dict contains some subdicts.
     * - Each sub-dict contains some strings. */
    for (unsigned long i = 0; i < count; i++) {
        RedisModuleDict *dict = RedisModule_CreateDict(ctx);
        for (unsigned long j = 0; j < 10; j++) {
            /* Create sub dict. */
            RedisModuleDict *subdict = RedisModule_CreateDict(ctx);
            for (unsigned long k = 0; k < 10; k++) {
                RedisModuleString *str = RedisModule_CreateStringFromULongLong(ctx, k);
                RedisModule_DictSet(subdict, str, str);
            }

            RedisModuleString *key = RedisModule_CreateStringFromULongLong(ctx, j);
            RedisModule_DictSet(dict, key, subdict);
            RedisModule_FreeString(ctx, key);
        }
        global_dicts[i] = dict;
    }
}

static void freeFragGlobalSubDict(RedisModuleCtx *ctx, RedisModuleDict *subdict) {
    char *key;
    size_t keylen;
    RedisModuleString *str;
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(subdict, "^", NULL, 0);
    while ((key = RedisModule_DictNextC(iter, &keylen, (void**)&str))) {
        RedisModule_FreeString(ctx, str);
    }
    RedisModule_FreeDict(ctx, subdict);
    RedisModule_DictIteratorStop(iter);
}

static void createFragGlobalDicts(RedisModuleCtx *ctx) {
    char *key;
    size_t keylen;
    RedisModuleDict *subdict;

    for (unsigned long i = 0; i < global_dicts_len; i++) {
        RedisModuleDict *dict = global_dicts[i];
        if (!dict) continue;

        /* Handle dictionaries differently based on their index in global_dicts array:
         * 1. For odd indices (i % 2 == 1): Remove the entire dictionary.
         * 2. For even indices: Keep the dictionary but remove half of its items. */
        if (i % 2 == 1) {
            RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(dict, "^", NULL, 0);
            while ((key = RedisModule_DictNextC(iter, &keylen, (void**)&subdict))) {
                freeFragGlobalSubDict(ctx, subdict);
            }
            RedisModule_FreeDict(ctx, dict);
            global_dicts[i] = NULL;
            RedisModule_DictIteratorStop(iter);
        } else {
            int key_index = 0;
            RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(dict, "^", NULL, 0);
            while ((key = RedisModule_DictNextC(iter, &keylen, (void**)&subdict))) {
                if (key_index++ % 2 == 1) {
                    freeFragGlobalSubDict(ctx, subdict);
                    RedisModule_DictReplaceC(dict, key, keylen, NULL);
                }
            }
            RedisModule_DictIteratorStop(iter);
        }
    }
}

static int defragGlobalSubDictValueCB(RedisModuleDefragCtx *ctx, void *data, unsigned char *key, size_t keylen, void **newptr) {
    REDISMODULE_NOT_USED(key);
    REDISMODULE_NOT_USED(keylen);
    if (!data) return 0;
    *newptr = RedisModule_DefragAlloc(ctx, data);
    return 0;
}

static int defragGlobalDictValueCB(RedisModuleDefragCtx *ctx, void *data, unsigned char *key, size_t keylen, void **newptr) {
    REDISMODULE_NOT_USED(key);
    REDISMODULE_NOT_USED(keylen);
    static RedisModuleString *seekTo = NULL;
    RedisModuleDict *subdict = data;
    if (!subdict) return 0;
    if (seekTo != NULL) global_subdicts_resumes++;

    *newptr = RedisModule_DefragRedisModuleDict(ctx, subdict, defragGlobalSubDictValueCB, &seekTo);
    if (*newptr) global_dicts_items_defragged++;
    /* Return 1 if seekTo is not NULL, indicating this node needs more defrag work. */
    return seekTo != NULL;
}

static int defragGlobalDicts(RedisModuleDefragCtx *ctx) {
    static RedisModuleString *seekTo = NULL;
    static unsigned long dict_index = 0;
    unsigned long cursor = 0;

    RedisModule_DefragCursorGet(ctx, &cursor);
    if (cursor == 0) { /* Start a new defrag. */
        if (seekTo) {
            RedisModule_FreeString(NULL, seekTo);
            seekTo = NULL;
        }
        dict_index = 0;
    } else {
        global_dicts_resumes++;
    }

    if (!global_dicts_len) return 0; /* dicts is empty. */
    RedisModule_Assert(dict_index < global_dicts_len);
    for (; dict_index < global_dicts_len; dict_index++) {
        RedisModuleDict *dict = global_dicts[dict_index];
        if (!dict) continue;
        RedisModuleDict *new = RedisModule_DefragRedisModuleDict(ctx, dict, defragGlobalDictValueCB, &seekTo);
        global_dicts_attempts++;
        if (new != NULL) {
            global_dicts[dict_index] = new;
            global_dicts_defragged++;
        }

        if (seekTo != NULL) {
            /* Set cursor to 1 to indicate defragmentation is not finished. */
            RedisModule_DefragCursorSet(ctx, 1);
            return 1;
        }
    }

    /* Set cursor to 0 to indicate completion. */
    dict_index = 0;
    RedisModule_DefragCursorSet(ctx, 0);
    return 0;
}

typedef enum { DEFRAG_NOT_START, DEFRAG_STRING, DEFRAG_DICT } defrag_module_stage;
static int defragGlobal(RedisModuleDefragCtx *ctx) {
    static defrag_module_stage stage = DEFRAG_NOT_START;
    if (stage == DEFRAG_NOT_START) {
        stage = DEFRAG_STRING; /* Start a new global defrag. */
    }

    if (stage == DEFRAG_STRING) {
        if (defragGlobalStrings(ctx) != 0) return 1;
        stage = DEFRAG_DICT;
    }
    if (stage == DEFRAG_DICT) {
        if (defragGlobalDicts(ctx) != 0) return 1;
        stage = DEFRAG_NOT_START;
    }
    return 0;
}

static void defragStart(RedisModuleDefragCtx *ctx) {
    REDISMODULE_NOT_USED(ctx);
    defrag_started++;
}

static void defragEnd(RedisModuleDefragCtx *ctx) {
    REDISMODULE_NOT_USED(ctx);
    defrag_ended++;
}

static void FragInfo(RedisModuleInfoCtx *ctx, int for_crash_report) {
    REDISMODULE_NOT_USED(for_crash_report);

    RedisModule_InfoAddSection(ctx, "stats");
    RedisModule_InfoAddFieldLongLong(ctx, "datatype_attempts", datatype_attempts);
    RedisModule_InfoAddFieldLongLong(ctx, "datatype_defragged", datatype_defragged);
    RedisModule_InfoAddFieldLongLong(ctx, "datatype_raw_defragged", datatype_raw_defragged);
    RedisModule_InfoAddFieldLongLong(ctx, "datatype_resumes", datatype_resumes);
    RedisModule_InfoAddFieldLongLong(ctx, "datatype_wrong_cursor", datatype_wrong_cursor);
    RedisModule_InfoAddFieldLongLong(ctx, "global_strings_attempts", global_strings_attempts);
    RedisModule_InfoAddFieldLongLong(ctx, "global_strings_defragged", global_strings_defragged);
    RedisModule_InfoAddFieldLongLong(ctx, "global_dicts_resumes", global_dicts_resumes);
    RedisModule_InfoAddFieldLongLong(ctx, "global_subdicts_resumes", global_subdicts_resumes);
    RedisModule_InfoAddFieldLongLong(ctx, "global_dicts_attempts", global_dicts_attempts);
    RedisModule_InfoAddFieldLongLong(ctx, "global_dicts_defragged", global_dicts_defragged);
    RedisModule_InfoAddFieldLongLong(ctx, "global_dicts_items_defragged", global_dicts_items_defragged);
    RedisModule_InfoAddFieldLongLong(ctx, "defrag_started", defrag_started);
    RedisModule_InfoAddFieldLongLong(ctx, "defrag_ended", defrag_ended);
}

struct FragObject *createFragObject(unsigned long len, unsigned long size, int maxstep) {
    struct FragObject *o = RedisModule_Alloc(sizeof(*o));
    o->len = len;
    o->values = RedisModule_Alloc(sizeof(RedisModuleString*) * len);
    o->maxstep = maxstep;

    for (unsigned long i = 0; i < len; i++) {
        o->values[i] = RedisModule_Calloc(1, size);
    }

    return o;
}

/* FRAG.RESETSTATS */
static int fragResetStatsCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    datatype_attempts = 0;
    datatype_defragged = 0;
    datatype_raw_defragged = 0;
    datatype_resumes = 0;
    datatype_wrong_cursor = 0;
    global_strings_attempts = 0;
    global_strings_defragged = 0;
    global_dicts_resumes = 0;
    global_subdicts_resumes = 0;
    global_dicts_attempts = 0;
    global_dicts_defragged = 0;
    global_dicts_items_defragged = 0;
    defrag_started = 0;
    defrag_ended = 0;

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* FRAG.CREATE key len size maxstep */
static int fragCreateCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 5)
        return RedisModule_WrongArity(ctx);

    RedisModuleKey *key = RedisModule_OpenKey(ctx,argv[1],
                                              REDISMODULE_READ|REDISMODULE_WRITE);
    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_EMPTY)
    {
        return RedisModule_ReplyWithError(ctx, "ERR key exists");
    }

    long long len;
    if ((RedisModule_StringToLongLong(argv[2], &len) != REDISMODULE_OK)) {
        return RedisModule_ReplyWithError(ctx, "ERR invalid len");
    }

    long long size;
    if ((RedisModule_StringToLongLong(argv[3], &size) != REDISMODULE_OK)) {
        return RedisModule_ReplyWithError(ctx, "ERR invalid size");
    }

    long long maxstep;
    if ((RedisModule_StringToLongLong(argv[4], &maxstep) != REDISMODULE_OK)) {
        return RedisModule_ReplyWithError(ctx, "ERR invalid maxstep");
    }

    struct FragObject *o = createFragObject(len, size, maxstep);
    RedisModule_ModuleTypeSetValue(key, FragType, o);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    RedisModule_CloseKey(key);

    return REDISMODULE_OK;
}

/* FRAG.create_frag_global len */
static int fragCreateGlobalCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    if (argc != 2)
        return RedisModule_WrongArity(ctx);

    long long glen;
    if ((RedisModule_StringToLongLong(argv[1], &glen) != REDISMODULE_OK)) {
        return RedisModule_ReplyWithError(ctx, "ERR invalid len");
    }

    createGlobalStrings(ctx, glen);
    createGlobalDicts(ctx, glen);
    createFragGlobalStrings(ctx);
    createFragGlobalDicts(ctx);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

void FragFree(void *value) {
    struct FragObject *o = value;

    for (unsigned long i = 0; i < o->len; i++)
        RedisModule_Free(o->values[i]);
    RedisModule_Free(o->values);
    RedisModule_Free(o);
}

size_t FragFreeEffort(RedisModuleString *key, const void *value) {
    REDISMODULE_NOT_USED(key);

    const struct FragObject *o = value;
    return o->len;
}

int FragDefrag(RedisModuleDefragCtx *ctx, RedisModuleString *key, void **value) {
    unsigned long i = 0;
    int steps = 0;

    int dbid = RedisModule_GetDbIdFromDefragCtx(ctx);
    RedisModule_Assert(dbid != -1);

    RedisModule_Log(NULL, "notice", "Defrag key: %s", RedisModule_StringPtrLen(key, NULL));

    /* Attempt to get cursor, validate it's what we're exepcting */
    if (RedisModule_DefragCursorGet(ctx, &i) == REDISMODULE_OK) {
        if (i > 0) datatype_resumes++;

        /* Validate we're expecting this cursor */
        if (i != last_set_cursor) datatype_wrong_cursor++;
    } else {
        if (last_set_cursor != 0) datatype_wrong_cursor++;
    }

    /* Attempt to defrag the object itself */
    datatype_attempts++;
    struct FragObject *o = RedisModule_DefragAlloc(ctx, *value);
    if (o == NULL) {
        /* Not defragged */
        o = *value;
    } else {
        /* Defragged */
        *value = o;
        datatype_defragged++;
    }

    /* Deep defrag now */
    for (; i < o->len; i++) {
        datatype_attempts++;
        void *new = RedisModule_DefragAlloc(ctx, o->values[i]);
        if (new) {
            o->values[i] = new;
            datatype_defragged++;
        }

        if ((o->maxstep && ++steps > o->maxstep) ||
            ((i % 64 == 0) && RedisModule_DefragShouldStop(ctx)))
        {
            RedisModule_DefragCursorSet(ctx, i);
            last_set_cursor = i;
            return 1;
        }
    }

    /* Defrag the values array itself using RedisModule_DefragAllocRaw
     * and RedisModule_DefragFreeRaw for testing purposes. */
    void *new_values = RedisModule_DefragAllocRaw(ctx, o->len * sizeof(void*));
    memcpy(new_values, o->values, o->len * sizeof(void*));
    RedisModule_DefragFreeRaw(ctx, o->values);
    o->values = new_values;
    datatype_raw_defragged++;

    last_set_cursor = 0;
    return 0;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "defragtest", 1, REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_GetTypeMethodVersion() < REDISMODULE_TYPE_METHOD_VERSION) {
        return REDISMODULE_ERR;
    }

    RedisModuleTypeMethods tm = {
            .version = REDISMODULE_TYPE_METHOD_VERSION,
            .free = FragFree,
            .free_effort = FragFreeEffort,
            .defrag = FragDefrag
    };

    FragType = RedisModule_CreateDataType(ctx, "frag_type", 0, &tm);
    if (FragType == NULL) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "frag.create",
                                  fragCreateCommand, "write deny-oom", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "frag.create_frag_global",
        fragCreateGlobalCommand, "write deny-oom", 1, 1, 1) == REDISMODULE_ERR)
    return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "frag.resetstats",
                                  fragResetStatsCommand, "write deny-oom", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModule_RegisterInfoFunc(ctx, FragInfo);
    RedisModule_RegisterDefragFunc2(ctx, defragGlobal);
    RedisModule_RegisterDefragCallbacks(ctx, defragStart, defragEnd);

    return REDISMODULE_OK;
}

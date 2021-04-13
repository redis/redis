/* A module that implements defrag callback mechanisms.
 */

#define REDISMODULE_EXPERIMENTAL_API
#include "redismodule.h"

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
unsigned long int datatype_resumes = 0;
unsigned long int datatype_wrong_cursor = 0;
unsigned long int global_attempts = 0;
unsigned long int global_defragged = 0;

int global_strings_len = 0;
RedisModuleString **global_strings = NULL;

static void createGlobalStrings(RedisModuleCtx *ctx, int count)
{
    global_strings_len = count;
    global_strings = RedisModule_Alloc(sizeof(RedisModuleString *) * count);

    for (int i = 0; i < count; i++) {
        global_strings[i] = RedisModule_CreateStringFromLongLong(ctx, i);
    }
}

static int defragGlobalStrings(RedisModuleDefragCtx *ctx)
{
    for (int i = 0; i < global_strings_len; i++) {
        RedisModuleString *new = RedisModule_DefragRedisModuleString(ctx, global_strings[i]);
        global_attempts++;
        if (new != NULL) {
            global_strings[i] = new;
            global_defragged++;
        }
    }

    return 0;
}

static void FragInfo(RedisModuleInfoCtx *ctx, int for_crash_report) {
    REDISMODULE_NOT_USED(for_crash_report);

    RedisModule_InfoAddSection(ctx, "stats");
    RedisModule_InfoAddFieldLongLong(ctx, "datatype_attempts", datatype_attempts);
    RedisModule_InfoAddFieldLongLong(ctx, "datatype_defragged", datatype_defragged);
    RedisModule_InfoAddFieldLongLong(ctx, "datatype_resumes", datatype_resumes);
    RedisModule_InfoAddFieldLongLong(ctx, "datatype_wrong_cursor", datatype_wrong_cursor);
    RedisModule_InfoAddFieldLongLong(ctx, "global_attempts", global_attempts);
    RedisModule_InfoAddFieldLongLong(ctx, "global_defragged", global_defragged);
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
    datatype_resumes = 0;
    datatype_wrong_cursor = 0;
    global_attempts = 0;
    global_defragged = 0;

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
    REDISMODULE_NOT_USED(key);
    unsigned long i = 0;
    int steps = 0;

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

    long long glen;
    if (argc != 1 || RedisModule_StringToLongLong(argv[0], &glen) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    createGlobalStrings(ctx, glen);

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

    if (RedisModule_CreateCommand(ctx, "frag.resetstats",
                                  fragResetStatsCommand, "write deny-oom", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModule_RegisterInfoFunc(ctx, FragInfo);
    RedisModule_RegisterDefragFunc(ctx, defragGlobalStrings);

    return REDISMODULE_OK;
}

#include "redismodule.h"
#include <assert.h>
#include <errno.h>
#include <strings.h>

/* LIST.GETALL key */
int list_getall(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    if (RedisModule_ListIteratorStart(key, 0) != REDISMODULE_OK) {
        RedisModule_CloseKey(key);
        switch (errno) {
        case ENOTSUP: return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        default: return RedisModule_ReplyWithError(ctx, "ERR ListIteratorStart failed");
        }
    }

    RedisModule_ReplyWithArray(ctx, RedisModule_ValueLength(key));
    RedisModuleString *elem;
    while (RedisModule_ListIteratorNext(key, &elem) == REDISMODULE_OK) {
        RedisModule_ReplyWithString(ctx, elem);
        RedisModule_FreeString(ctx, elem);
    }
    assert(errno == ENOENT); /* no more elements in list */
    /* RedisModule_ListIteratorStop(key); //implicit, done by RM_CloseKey */
    /* RedisModule_CloseKey(key); //implicit, done by auto memory */
    return REDISMODULE_OK;
}

/* LIST.EDIT key [REVERSE] cmdstr [value ..]
 *
 * cmdstr is a string of the following characters:
 *
 *     k -- keep (advences to the next element)
 *     d -- delete (advances to the next element)
 *     i -- insert value from args
 *
 * The number of inserts (i.e. the number of occurrences of i in cmdstr) should
 * correspond to the number of args after cmdstr.
 *
 * The reply is the number of edits (inserts + deletes) performed.
 */
int list_edit(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) return RedisModule_WrongArity(ctx);
    int argpos = 1; /* the next arg */

    /* key */
    int keymode = REDISMODULE_READ | REDISMODULE_WRITE;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[argpos++], keymode);

    /* REVERSE */
    int reverse = 0;
    if (argc >= 4 &&
        !strcasecmp(RedisModule_StringPtrLen(argv[argpos], NULL), "REVERSE")) {
        reverse = 1;
        argpos++;
    }

    /* cmdstr */
    size_t cmdstr_len;
    const char *cmdstr = RedisModule_StringPtrLen(argv[argpos++], &cmdstr_len);

    /* Start iterator */
    int flags = reverse ? REDISMODULE_LIST_REVERSE : 0;
    if (RedisModule_ListIteratorStart(key, flags) != REDISMODULE_OK) {
        RedisModule_CloseKey(key);
        switch (errno) {
        case ENOTSUP:
            return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        default:
            return RedisModule_ReplyWithError(ctx, "ERR ListIteratorStart failed");
        }
    }

    /* Iterate over the chars in cmdstr (edit instructions) */
    long long num_edits = 0;
    RedisModuleString *elem;
    int next_result = RedisModule_ListIteratorNext(key, &elem);
    size_t cmdpos; /* char position with cmdstr */
    for (cmdpos = 0;
         cmdpos < cmdstr_len && next_result == REDISMODULE_OK;
         cmdpos++) {
        switch (cmdstr[cmdpos]) {
        case 'i': /* insert */
            if (argpos < argc) {
                RedisModuleString *value = argv[argpos++];
                int where = reverse ? REDISMODULE_LIST_AFTER
                                    : REDISMODULE_LIST_BEFORE;
                RedisModule_ListIteratorInsert(key, where, value);
                num_edits++;
            }
            /* Don't advance the iterator. We allow multiple inserts before
             * delete or keep. */
            break;
        case 'd': /* delete */
            RedisModule_ListIteratorDelete(key);
            num_edits++;
            /* Fallthrough */
        case 'k': /* keep */
            /* Free current elem and advance iterator */
            RedisModule_FreeString(ctx, elem);
            next_result = RedisModule_ListIteratorNext(key, &elem);
            break;
        }
    }
    if (next_result == REDISMODULE_OK) RedisModule_FreeString(ctx, elem);
    RedisModule_ListIteratorStop(key);

    /* Any inserts after the last element = push */
    for (; cmdpos < cmdstr_len; cmdpos++) {
        if (cmdstr[cmdpos] == 'i' && argpos < argc) {
            RedisModuleString *value = argv[argpos++];
            int where = reverse ? REDISMODULE_LIST_HEAD : REDISMODULE_LIST_TAIL;
            RedisModule_ListPush(key, where, value);
            num_edits++;
        }
    }
    RedisModule_ReplyWithLongLong(ctx, num_edits);
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

/* LIST.GET key index */
int list_get(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) return RedisModule_WrongArity(ctx);
    long long index;
    if (RedisModule_StringToLongLong(argv[2], &index) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "ERR index must be a number");
        return REDISMODULE_OK;
    }
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    RedisModuleString *value = RedisModule_ListGet(key, index);
    RedisModule_CloseKey(key);
    if (value) {
        RedisModule_ReplyWithString(ctx, value);
        RedisModule_FreeString(ctx, value);
    } else {
        assert(errno == EDOM);
        RedisModule_ReplyWithError(ctx, "ERR index out of range");
    }
    return REDISMODULE_OK;
}

/* LIST.SET key index value */
int list_set(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) return RedisModule_WrongArity(ctx);
    long long index;
    if (RedisModule_StringToLongLong(argv[2], &index) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "ERR index must be a number");
        return REDISMODULE_OK;
    }
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    if (RedisModule_ListSet(key, index, argv[3]) == REDISMODULE_OK) {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        assert(errno == EDOM);
        RedisModule_ReplyWithError(ctx, "ERR index out of range");
    }
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

/* LIST.INSERT key index value
 *
 * If index is negative, value is inserted after, otherwise before the element
 * at index.
 */
int list_insert(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) return RedisModule_WrongArity(ctx);
    long long index;
    if (RedisModule_StringToLongLong(argv[2], &index) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "ERR index must be a number");
        return REDISMODULE_OK;
    }
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    int where = (index >= 0) ? REDISMODULE_LIST_BEFORE : REDISMODULE_LIST_AFTER;
    if (RedisModule_ListInsert(key, where, index, argv[3]) == REDISMODULE_OK) {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        assert(errno == EDOM);
        RedisModule_ReplyWithError(ctx, "ERR index out of range");
    }
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

/* LIST.DELETE key index */
int list_delete(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) return RedisModule_WrongArity(ctx);
    long long index;
    if (RedisModule_StringToLongLong(argv[2], &index) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "ERR index must be a number");
        return REDISMODULE_OK;
    }
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    if (RedisModule_ListDelete(key, index) == REDISMODULE_OK) {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        assert(errno == EDOM);
        RedisModule_ReplyWithError(ctx, "ERR index out of range");
    }
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "list", 1, REDISMODULE_APIVER_1) == REDISMODULE_OK &&
        RedisModule_CreateCommand(ctx, "list.getall", list_getall, "",
                                  1, 1, 1) == REDISMODULE_OK &&
        RedisModule_CreateCommand(ctx, "list.edit", list_edit, "",
                                  1, 1, 1) == REDISMODULE_OK &&
        RedisModule_CreateCommand(ctx, "list.get", list_get, "",
                                  1, 1, 1) == REDISMODULE_OK &&
        RedisModule_CreateCommand(ctx, "list.set", list_set, "",
                                  1, 1, 1) == REDISMODULE_OK &&
        RedisModule_CreateCommand(ctx, "list.insert", list_insert, "",
                                  1, 1, 1) == REDISMODULE_OK &&
        RedisModule_CreateCommand(ctx, "list.delete", list_delete, "",
                                  1, 1, 1) == REDISMODULE_OK) {
        return REDISMODULE_OK;
    } else {
        return REDISMODULE_ERR;
    }
}

#include "redismodule.h"
#include <strings.h>
#include <errno.h>
#include <stdlib.h>

/* If a string is ":deleted:", the special value for deleted hash fields is
 * returned; otherwise the input string is returned. */
static RedisModuleString *value_or_delete(RedisModuleString *s) {
    if (!strcasecmp(RedisModule_StringPtrLen(s, NULL), ":delete:"))
        return REDISMODULE_HASH_DELETE;
    else
        return s;
}

/* HASH.SET key flags field1 value1 [field2 value2 ..]
 *
 * Sets 1-4 fields. Returns the same as RedisModule_HashSet().
 * Flags is a string of "nxi" where n = NX, x = XX, i = count inserts.
 * To delete a field, use the value ":delete:".
 */
int hash_set(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 5 || argc % 2 == 0 || argc > 11)
        return RedisModule_WrongArity(ctx);

    RedisModule_AutoMemory(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);

    size_t flags_len;
    const char *flags_str = RedisModule_StringPtrLen(argv[2], &flags_len);
    int flags = REDISMODULE_HASH_NONE;
    for (size_t i = 0; i < flags_len; i++) {
        switch (flags_str[i]) {
        case 'n': flags |= REDISMODULE_HASH_NX; break;
        case 'x': flags |= REDISMODULE_HASH_XX; break;
        case 'i': flags |= REDISMODULE_HASH_COUNT_INSERTS; break;
        }
    }

    /* This is why varargs sucks... */
    int result;
    errno = 0;
    if (argc == 5) {
        result = RedisModule_HashSet(key, flags,
                                     argv[3], value_or_delete(argv[4]),
                                     NULL);
    } else if (argc == 7) {
        result = RedisModule_HashSet(key, flags,
                                     argv[3], value_or_delete(argv[4]),
                                     argv[5], value_or_delete(argv[6]),
                                     NULL);
    } else if (argc == 9) {
        result = RedisModule_HashSet(key, flags,
                                     argv[3], value_or_delete(argv[4]),
                                     argv[5], value_or_delete(argv[6]),
                                     argv[7], value_or_delete(argv[8]),
                                     NULL);
    } else if (argc == 11) {
        result = RedisModule_HashSet(key, flags,
                                     argv[3], value_or_delete(argv[4]),
                                     argv[5], value_or_delete(argv[6]),
                                     argv[7], value_or_delete(argv[8]),
                                     argv[9], value_or_delete(argv[10]),
                                     NULL);
    } else {
        return RedisModule_ReplyWithError(ctx, "ERR too many fields");
    }

    /* Check errno */
    if (errno == ENOTSUP) {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    } else if (flags & REDISMODULE_HASH_COUNT_INSERTS) {
        int numfields = (argc - 3) / 2;
        if (result != numfields) {
            if (flags & REDISMODULE_HASH_NX)
                RedisModule_Assert(errno == EEXIST);
            if (flags & REDISMODULE_HASH_XX)
                RedisModule_Assert(errno == ENOENT);
        }
    }

    return RedisModule_ReplyWithLongLong(ctx, result);
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "hash", 1, REDISMODULE_APIVER_1) ==
        REDISMODULE_OK &&
        RedisModule_CreateCommand(ctx, "hash.set", hash_set, "",
                                  1, 1, 1) == REDISMODULE_OK) {
        return REDISMODULE_OK;
    } else {
        return REDISMODULE_ERR;
    }
}

#include "redismodule.h"
#include <strings.h>
#include <errno.h>
#include <stdlib.h>

#define UNUSED(x) (void)(x)

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
 * Flags is a string of "nxa" where n = NX, x = XX, a = COUNT_ALL.
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
        case 'a': flags |= REDISMODULE_HASH_COUNT_ALL; break;
        }
    }

    /* Test some varargs. (In real-world, use a loop and set one at a time.) */
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
    if (result == 0) {
        if (errno == ENOTSUP)
            return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        else
            RedisModule_Assert(errno == ENOENT);
    }

    return RedisModule_ReplyWithLongLong(ctx, result);
}

RedisModuleKey* openKeyWithMode(RedisModuleCtx *ctx, RedisModuleString *keyName, int mode) {
    int supportedMode = RedisModule_GetOpenKeyModesAll();
    if (!(supportedMode & REDISMODULE_READ) || ((supportedMode & mode)!=mode)) {
        RedisModule_ReplyWithError(ctx, "OpenKey mode is not supported");
        return NULL;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, keyName, REDISMODULE_READ | mode);
    if (!key) {
        RedisModule_ReplyWithError(ctx, "key not found");
        return NULL;
    }

    return key;
}

int test_open_key_subexpired_hget(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc<3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = openKeyWithMode(ctx, argv[1], REDISMODULE_OPEN_KEY_ACCESS_EXPIRED);
    if (!key) return REDISMODULE_OK;

    RedisModuleString *value;
    RedisModule_HashGet(key,REDISMODULE_HASH_NONE,argv[2],&value,NULL);

    /* return the value */
    if (value) {
        RedisModule_ReplyWithString(ctx, value);
        RedisModule_FreeString(ctx, value);
    } else {
        RedisModule_ReplyWithNull(ctx);
    }
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

int test_open_key_hget_expire(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc<3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = openKeyWithMode(ctx, argv[1], REDISMODULE_OPEN_KEY_ACCESS_EXPIRED);
    if (!key) return REDISMODULE_OK;

    mstime_t expireAt;
    
    /* Let's test here that we get error if using invalid flags combination */
    RedisModule_Assert(
            RedisModule_HashGet(key,
                                REDISMODULE_HASH_EXISTS |
                                REDISMODULE_HASH_EXPIRE_TIME,
                                argv[2], &expireAt, NULL) == REDISMODULE_ERR);    
    
    /* Now let's get the expire time */
    RedisModule_HashGet(key, REDISMODULE_HASH_EXPIRE_TIME,argv[2],&expireAt,NULL);
    RedisModule_ReplyWithLongLong(ctx, expireAt);
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

/* Test variadic function to get two expiration times */
int test_open_key_hget_two_expire(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc<3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = openKeyWithMode(ctx, argv[1], REDISMODULE_OPEN_KEY_ACCESS_EXPIRED);
    if (!key) return REDISMODULE_OK;

    mstime_t expireAt1, expireAt2;
    RedisModule_HashGet(key,REDISMODULE_HASH_EXPIRE_TIME,argv[2],&expireAt1,argv[3],&expireAt2,NULL);
    
    /* return the two expire time */
    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithLongLong(ctx, expireAt1);
    RedisModule_ReplyWithLongLong(ctx, expireAt2);
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

int test_open_key_hget_min_expire(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc!=2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = openKeyWithMode(ctx, argv[1], REDISMODULE_READ);
    if (!key) return REDISMODULE_OK;

    volatile mstime_t minExpire = RedisModule_HashFieldMinExpire(key);
    RedisModule_ReplyWithLongLong(ctx, minExpire);
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

int  numReplies;
void ScanCallback(RedisModuleKey *key, RedisModuleString *field, RedisModuleString *value, void *privdata) {
    UNUSED(key);
    RedisModuleCtx *ctx = (RedisModuleCtx *)privdata;

    /* Reply with the field and value (or NULL for sets) */
    RedisModule_ReplyWithString(ctx, field);
    if (value) {
        RedisModule_ReplyWithString(ctx, value);
    } else {
        RedisModule_ReplyWithCString(ctx, "(null)");
    }
    numReplies+=2;
}

int test_open_key_access_expired_hscan(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = openKeyWithMode(ctx, argv[1], REDISMODULE_OPEN_KEY_ACCESS_EXPIRED);

    if (!key)
        return RedisModule_ReplyWithError(ctx, "ERR key not exists");

    /* Verify it is a hash */
    if (RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_HASH) {
        RedisModule_CloseKey(key);
        return RedisModule_ReplyWithError(ctx, "ERR key is not a hash");
    }

    /* Scan the hash and reply pairs of key-value */
    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
    numReplies = 0;
    RedisModuleScanCursor *cursor = RedisModule_ScanCursorCreate();
    while (RedisModule_ScanKey(key, cursor, ScanCallback, ctx));
    RedisModule_ScanCursorDestroy(cursor);
    RedisModule_CloseKey(key);
    RedisModule_ReplySetArrayLength(ctx, numReplies);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "hash", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "hash.set", hash_set, "write", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "hash.hget_expired", test_open_key_subexpired_hget,"", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "hash.hscan_expired", test_open_key_access_expired_hscan,"", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "hash.hget_expire", test_open_key_hget_expire,"", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "hash.hget_two_expire", test_open_key_hget_two_expire,"", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "hash.hget_min_expire", test_open_key_hget_min_expire,"", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

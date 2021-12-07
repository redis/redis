#include "redismodule.h"

#include <string.h>
#include <strings.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>

/* Command which adds a stream entry with automatic ID, like XADD *.
 *
 * Syntax: STREAM.ADD key field1 value1 [ field2 value2 ... ]
 *
 * The response is the ID of the added stream entry or an error message.
 */
int stream_add(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2 || argc % 2 != 0) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    RedisModuleStreamID id;
    if (RedisModule_StreamAdd(key, REDISMODULE_STREAM_ADD_AUTOID, &id,
                              &argv[2], (argc-2)/2) == REDISMODULE_OK) {
        RedisModuleString *id_str = RedisModule_CreateStringFromStreamID(ctx, &id);
        RedisModule_ReplyWithString(ctx, id_str);
        RedisModule_FreeString(ctx, id_str);
    } else {
        RedisModule_ReplyWithError(ctx, "ERR StreamAdd failed");
    }
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

/* Command which adds a stream entry N times.
 *
 * Syntax: STREAM.ADD key N field1 value1 [ field2 value2 ... ]
 *
 * Returns the number of successfully added entries.
 */
int stream_addn(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3 || argc % 2 == 0) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    long long n, i;
    if (RedisModule_StringToLongLong(argv[2], &n) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "N must be a number");
        return REDISMODULE_OK;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    for (i = 0; i < n; i++) {
        if (RedisModule_StreamAdd(key, REDISMODULE_STREAM_ADD_AUTOID, NULL,
                                  &argv[3], (argc-3)/2) == REDISMODULE_ERR)
            break;
    }
    RedisModule_ReplyWithLongLong(ctx, i);
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

/* STREAM.DELETE key stream-id */
int stream_delete(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) return RedisModule_WrongArity(ctx);
    RedisModuleStreamID id;
    if (RedisModule_StringToStreamID(argv[2], &id) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx, "Invalid stream ID");
    }
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    if (RedisModule_StreamDelete(key, &id) == REDISMODULE_OK) {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        RedisModule_ReplyWithError(ctx, "ERR StreamDelete failed");
    }
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

/* STREAM.RANGE key start-id end-id
 *
 * Returns an array of stream items. Each item is an array on the form
 * [stream-id, [field1, value1, field2, value2, ...]].
 *
 * A funny side-effect used for testing RM_StreamIteratorDelete() is that if any
 * entry has a field named "selfdestruct", the stream entry is deleted. It is
 * however included in the results of this command.
 */
int stream_range(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RedisModuleStreamID startid, endid;
    if (RedisModule_StringToStreamID(argv[2], &startid) != REDISMODULE_OK ||
        RedisModule_StringToStreamID(argv[3], &endid) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "Invalid stream ID");
        return REDISMODULE_OK;
    }

    /* If startid > endid, we swap and set the reverse flag. */
    int flags = 0;
    if (startid.ms > endid.ms ||
        (startid.ms == endid.ms && startid.seq > endid.seq)) {
        RedisModuleStreamID tmp = startid;
        startid = endid;
        endid = tmp;
        flags |= REDISMODULE_STREAM_ITERATOR_REVERSE;
    }

    /* Open key and start iterator. */
    int openflags = REDISMODULE_READ | REDISMODULE_WRITE;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], openflags);
    if (RedisModule_StreamIteratorStart(key, flags,
                                        &startid, &endid) != REDISMODULE_OK) {
        /* Key is not a stream, etc. */
        RedisModule_ReplyWithError(ctx, "ERR StreamIteratorStart failed");
        RedisModule_CloseKey(key);
        return REDISMODULE_OK;
    }

    /* Check error handling: Delete current entry when no current entry. */
    assert(RedisModule_StreamIteratorDelete(key) ==
           REDISMODULE_ERR);
    assert(errno == ENOENT);

    /* Check error handling: Fetch fields when no current entry. */
    assert(RedisModule_StreamIteratorNextField(key, NULL, NULL) ==
           REDISMODULE_ERR);
    assert(errno == ENOENT);

    /* Return array. */
    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_LEN);
    RedisModule_AutoMemory(ctx);
    RedisModuleStreamID id;
    long numfields;
    long len = 0;
    while (RedisModule_StreamIteratorNextID(key, &id,
                                            &numfields) == REDISMODULE_OK) {
        RedisModule_ReplyWithArray(ctx, 2);
        RedisModuleString *id_str = RedisModule_CreateStringFromStreamID(ctx, &id);
        RedisModule_ReplyWithString(ctx, id_str);
        RedisModule_ReplyWithArray(ctx, numfields * 2);
        int delete = 0;
        RedisModuleString *field, *value;
        for (long i = 0; i < numfields; i++) {
            assert(RedisModule_StreamIteratorNextField(key, &field, &value) ==
                   REDISMODULE_OK);
            RedisModule_ReplyWithString(ctx, field);
            RedisModule_ReplyWithString(ctx, value);
            /* check if this is a "selfdestruct" field */
            size_t field_len;
            const char *field_str = RedisModule_StringPtrLen(field, &field_len);
            if (!strncmp(field_str, "selfdestruct", field_len)) delete = 1;
        }
        if (delete) {
            assert(RedisModule_StreamIteratorDelete(key) == REDISMODULE_OK);
        }
        /* check error handling: no more fields to fetch */
        assert(RedisModule_StreamIteratorNextField(key, &field, &value) ==
               REDISMODULE_ERR);
        assert(errno == ENOENT);
        len++;
    }
    RedisModule_ReplySetArrayLength(ctx, len);
    RedisModule_StreamIteratorStop(key);
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

/*
 * STREAM.TRIM key (MAXLEN (=|~) length | MINID (=|~) id)
 */
int stream_trim(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 5) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    /* Parse args */
    int trim_by_id = 0; /* 0 = maxlen, 1 = minid */
    long long maxlen;
    RedisModuleStreamID minid;
    size_t arg_len;
    const char *arg = RedisModule_StringPtrLen(argv[2], &arg_len);
    if (!strcasecmp(arg, "minid")) {
        trim_by_id = 1;
        if (RedisModule_StringToStreamID(argv[4], &minid) != REDISMODULE_OK) {
            RedisModule_ReplyWithError(ctx, "ERR Invalid stream ID");
            return REDISMODULE_OK;
        }
    } else if (!strcasecmp(arg, "maxlen")) {
        if (RedisModule_StringToLongLong(argv[4], &maxlen) == REDISMODULE_ERR) {
            RedisModule_ReplyWithError(ctx, "ERR Maxlen must be a number");
            return REDISMODULE_OK;
        }
    } else {
        RedisModule_ReplyWithError(ctx, "ERR Invalid arguments");
        return REDISMODULE_OK;
    }

    /* Approx or exact */
    int flags;
    arg = RedisModule_StringPtrLen(argv[3], &arg_len);
    if (arg_len == 1 && arg[0] == '~') {
        flags = REDISMODULE_STREAM_TRIM_APPROX;
    } else if (arg_len == 1 && arg[0] == '=') {
        flags = 0;
    } else {
        RedisModule_ReplyWithError(ctx, "ERR Invalid approx-or-exact mark");
        return REDISMODULE_OK;
    }

    /* Trim */
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_WRITE);
    long long trimmed;
    if (trim_by_id) {
        trimmed = RedisModule_StreamTrimByID(key, flags, &minid);
    } else {
        trimmed = RedisModule_StreamTrimByLength(key, flags, maxlen);
    }

    /* Return result */
    if (trimmed < 0) {
        RedisModule_ReplyWithError(ctx, "ERR Trimming failed");
    } else {
        RedisModule_ReplyWithLongLong(ctx, trimmed);
    }
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "stream", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "stream.add", stream_add, "write",
                                  1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "stream.addn", stream_addn, "write",
                                  1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "stream.delete", stream_delete, "write",
                                  1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "stream.range", stream_range, "write",
                                  1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "stream.trim", stream_trim, "write",
                                  1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

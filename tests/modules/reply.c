/* 
 * A module the tests RM_ReplyWith family of commands
 */

#include "redismodule.h"
#include <math.h>

int rw_string(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    return RedisModule_ReplyWithString(ctx, argv[1]);
}

int rw_cstring(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);

    return RedisModule_ReplyWithSimpleString(ctx, "A simple string");
}

int rw_int(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    long long integer;
    if (RedisModule_StringToLongLong(argv[1], &integer) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "Arg cannot be parsed as an integer");

    return RedisModule_ReplyWithLongLong(ctx, integer);
}

/* When one argument is given, it is returned as a double,
 * when two arguments are given, it returns a/b. */
int rw_double(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc==1)
        return RedisModule_ReplyWithDouble(ctx, NAN);

    if (argc != 2 && argc != 3) return RedisModule_WrongArity(ctx);

    double dbl, dbl2;
    if (RedisModule_StringToDouble(argv[1], &dbl) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "Arg cannot be parsed as a double");
    if (argc == 3) {
        if (RedisModule_StringToDouble(argv[2], &dbl2) != REDISMODULE_OK)
            return RedisModule_ReplyWithError(ctx, "Arg cannot be parsed as a double");
        dbl /= dbl2;
    }

    return RedisModule_ReplyWithDouble(ctx, dbl);
}

int rw_longdouble(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    long double longdbl;
    if (RedisModule_StringToLongDouble(argv[1], &longdbl) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "Arg cannot be parsed as a double");

    return RedisModule_ReplyWithLongDouble(ctx, longdbl);
}

int rw_bignumber(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    size_t bignum_len;
    const char *bignum_str = RedisModule_StringPtrLen(argv[1], &bignum_len);

    return RedisModule_ReplyWithBigNumber(ctx, bignum_str, bignum_len);
}

int rw_array(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    long long integer;
    if (RedisModule_StringToLongLong(argv[1], &integer) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "Arg cannot be parsed as a integer");

    RedisModule_ReplyWithArray(ctx, integer);
    for (int i = 0; i < integer; ++i) {
        RedisModule_ReplyWithLongLong(ctx, i);
    }

    return REDISMODULE_OK;
}

int rw_map(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    long long integer;
    if (RedisModule_StringToLongLong(argv[1], &integer) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "Arg cannot be parsed as a integer");

    RedisModule_ReplyWithMap(ctx, integer);
    for (int i = 0; i < integer; ++i) {
        RedisModule_ReplyWithLongLong(ctx, i);
        RedisModule_ReplyWithDouble(ctx, i * 1.5);
    }

    return REDISMODULE_OK;
}

int rw_set(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    long long integer;
    if (RedisModule_StringToLongLong(argv[1], &integer) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "Arg cannot be parsed as a integer");

    RedisModule_ReplyWithSet(ctx, integer);
    for (int i = 0; i < integer; ++i) {
        RedisModule_ReplyWithLongLong(ctx, i);
    }

    return REDISMODULE_OK;
}

int rw_attribute(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    long long integer;
    if (RedisModule_StringToLongLong(argv[1], &integer) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "Arg cannot be parsed as a integer");

    if (RedisModule_ReplyWithAttribute(ctx, integer) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx, "Attributes aren't supported by RESP 2");
    }

    for (int i = 0; i < integer; ++i) {
        RedisModule_ReplyWithLongLong(ctx, i);
        RedisModule_ReplyWithDouble(ctx, i * 1.5);
    }

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int rw_bool(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);

    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithBool(ctx, 0);
    return RedisModule_ReplyWithBool(ctx, 1);
}

int rw_null(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);

    return RedisModule_ReplyWithNull(ctx);
}

int rw_error(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);

    return RedisModule_ReplyWithError(ctx, "An error");
}

int rw_error_format(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) return RedisModule_WrongArity(ctx);

    return RedisModule_ReplyWithErrorFormat(ctx,
                                            RedisModule_StringPtrLen(argv[1], NULL),
                                            RedisModule_StringPtrLen(argv[2], NULL));
}

int rw_verbatim(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    size_t verbatim_len;
    const char *verbatim_str = RedisModule_StringPtrLen(argv[1], &verbatim_len);

    return RedisModule_ReplyWithVerbatimString(ctx, verbatim_str, verbatim_len);
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "replywith", 1, REDISMODULE_APIVER_1) != REDISMODULE_OK)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"rw.string",rw_string,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.cstring",rw_cstring,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.bignumber",rw_bignumber,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.int",rw_int,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.double",rw_double,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.longdouble",rw_longdouble,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.array",rw_array,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.map",rw_map,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.attribute",rw_attribute,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.set",rw_set,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.bool",rw_bool,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.null",rw_null,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.error",rw_error,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.error_format",rw_error_format,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"rw.verbatim",rw_verbatim,"",0,0,0) != REDISMODULE_OK)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

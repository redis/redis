/* redisassert.h -- Drop in replacements assert.h that prints the stack trace
 *                  in the Redis logs.
 *
 * This file should be included instead of "assert.h" inside libraries used by
 * Redis that are using assertions, so instead of Redis disappearing with
 * SIGABORT, we get the details and stack trace inside the log file.
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef __REDIS_ASSERT_H__
#define __REDIS_ASSERT_H__

#include "stdlib.h"
#include "config.h"

#define assert(_e) (likely((_e))?(void)0 : (_serverAssert(#_e,__FILE__,__LINE__),redis_unreachable()))
#define panic(...) _serverPanic(__FILE__,__LINE__,__VA_ARGS__),redis_unreachable()

/* We can print the stacktrace, so our assert is defined this way: */
#define serverAssertWithInfo(_c,_o,_e) (likely(_e)?(void)0 : (_serverAssertWithInfo(_c,_o,#_e,__FILE__,__LINE__),redis_unreachable()))
#define serverAssert(_e) (likely(_e)?(void)0 : (_serverAssert(#_e,__FILE__,__LINE__),redis_unreachable()))
#define serverPanic(...) _serverPanic(__FILE__,__LINE__,__VA_ARGS__),redis_unreachable()

/* The following macros provide assertions that are only executed during test builds and should be used to add
 * assertions that are too computationally expensive or dangerous to run during normal operations.  */
#ifdef DEBUG_ASSERTIONS
#define debugAssert(_e) assert(_e)
#define debugServerAssert(...) serverAssert(__VA_ARGS__)
#define debugServerAssertWithInfo(...) serverAssertWithInfo(__VA_ARGS__)
#else
#define debugAssert(_e) ((void)0)
#define debugServerAssert(...)
#define debugServerAssertWithInfo(...)
#endif

/* forward declaration */
typedef struct client client;
typedef struct redisObject robj;

void _serverAssert(const char *estr, const char *file, int line);
void _serverAssertWithInfo(const client *c, const robj *o, const char *estr, const char *file, int line);
void _serverPanic(const char *file, int line, const char *msg, ...);

#endif

/* This file contains debugging macros to be used when investigating issues.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2016-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef _REDIS_DEBUGMACRO_H_
#define _REDIS_DEBUGMACRO_H_

#include <stdio.h>
#define D(...)                                                               \
    do {                                                                     \
        FILE *fp = fopen("/tmp/log.txt","a");                                \
        fprintf(fp,"%s:%s:%d:\t", __FILE__, __func__, __LINE__);             \
        fprintf(fp,__VA_ARGS__);                                             \
        fprintf(fp,"\n");                                                    \
        fclose(fp);                                                          \
    } while (0)

#endif /* _REDIS_DEBUGMACRO_H_ */

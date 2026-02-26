/*
 * Copyright (c) 2022-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef __SYSCHECK_H
#define __SYSCHECK_H

#include "sds.h"
#include "config.h"

int syscheck(void);
#ifdef __linux__
int checkXenClocksource(sds *error_msg);
int checkTHPEnabled(sds *error_msg);
int checkOvercommit(sds *error_msg);
#ifdef __arm64__
int checkLinuxMadvFreeForkBug(sds *error_msg);
#endif
#endif

#endif

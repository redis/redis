/*
 * Copyright (c) 2023, Viktor Soderqvist <viktor.soderqvist@est.tech>
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 * To make it easier to map each part of the format string with each argument,
 * this file provides a way to write
 *
 *     printf("xxx %s yyy %s zzz %s end\n", arg1, arg2, arg3);
 *
 * as
 *
 *     #define FMTARGS                                       \
 *         FMTARG("xxx %s ", arg1)                           \
 *         FMTARG("yyy %s ", arg2)                           \
 *         FMTARG("zzz %s ", arg3)                           \
 *         FMT("end\n")
 *
 *     printf(
 *            #include "fmtargs.h"
 *            );
 *
 *     #undef FMTARGS
 */

#ifndef FMTARGS
#error "FMTARGS not defined"
#endif

#ifdef FMTARG
#error "FMTARG already defined"
#endif

#ifdef FMT
#error "FMT already defined"
#endif

#define FMTARG(_fmt, _arg) _fmt
#define FMT(_fmt) _fmt
FMTARGS
#undef FMTARG
#undef FMT

#define FMTARG(_fmt, _arg) , _arg
#define FMT(_fmt)
FMTARGS
#undef FMTARG
#undef FMT

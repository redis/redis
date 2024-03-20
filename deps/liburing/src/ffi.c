/* SPDX-License-Identifier: MIT */
#define IOURINGINLINE

#ifdef __clang__
// clang doesn't seem to particularly like that we're including a header that
// deliberately contains function definitions so we explicitly silence it
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#endif

#include "liburing.h"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

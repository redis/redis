#ifndef TRACE_H
#define TRACE_H

#ifdef USE_USDT
// If USDT is enabled, use the generated header
#include "redis_dtrace.h"
#else
// If USDT isn't enabled, create stub macros
#define REDIS_CALL_START(arg0)
#define REDIS_CALL_START_ENABLED() (0)
#define REDIS_CALL_END(arg0)
#define REDIS_CALL_END_ENABLED() (0)
#endif

#endif

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

#ifdef __GNUC__
#  define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define UNLIKELY(x) !!(x)
#endif

#define REDIS_USDT_PROBE_HOOK(name, ...) if (UNLIKELY(REDIS_##name##_ENABLED())) { REDIS_##name(__VA_ARGS__); }

#define TRACE_CALL_START(arg0)\
  REDIS_USDT_PROBE_HOOK(CALL_START, arg0)

#define TRACE_CALL_END(arg0)\
  REDIS_USDT_PROBE_HOOK(CALL_END, arg0)

#endif

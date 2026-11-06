/* Minimal TRE configuration for Redis.
 *
 * We use TRE as a byte-oriented regex matcher for ARGREP. Redis SDS values are
 * binary-safe byte strings, so we intentionally keep the dependency build
 * simple: no wide-char path, no multibyte locale handling, and no approximate
 * matching engine.
 */

#define HAVE_SYS_TYPES_H 1

#define TRE_VERSION "redis-vendored"
#define TRE_VERSION_1 0
#define TRE_VERSION_2 0
#define TRE_VERSION_3 0

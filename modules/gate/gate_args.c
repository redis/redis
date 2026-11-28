/*
 * Gate data type: command argument grammar.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * The grammar is specified in gate_args.h and, for clients, in the command
 * reference of README.md. Notes on how this file honours it:
 *
 * ARGUMENTS ARE UNTRUSTED BYTES. An argument is a (pointer, length) pair: it
 * is not NUL terminated and may hold any byte, NULs included. Nothing here
 * reads past 'len' or treats an argument as a C string. Numbers are copied
 * into a fixed stack buffer after a length check, tokens are compared byte
 * by byte, and whatever is echoed in an error message is rendered by
 * gateArgsQuote() first, so a message is one bounded line of text no matter
 * what the client sent.
 *
 * GIVEN IS NOT THE SAME AS NON-DEFAULT. Several rules depend on whether an
 * option was written, not on its value: duplicates, the SCORE mode
 * rejections, COLDAUDIT following AUDIT, dimensions that get a default only
 * when their input is configured. They are all decided from the 'seen' flags
 * of the walk, never by comparing a field with its default: "TEXTDIM 8192"
 * written on a gate without the TEXT input is an error although 8192 is the
 * default.
 *
 * RANGES ARE CHECKED BEFORE NARROWING. Every integer is parsed into a
 * uint64_t and compared with its limits there. Only then is it stored into a
 * narrower config field, so "THRESHOLDS 4294967298" is out of range instead
 * of silently becoming 2.
 *
 * WHO CHECKS WHAT. This file checks what only a parser can know: the syntax,
 * which options were given, and the limits that make a narrowing store
 * lossless. The invariants of a configuration (ALPHA inside (0,1), a class
 * target not above ALPHA, MAXMEMORY large enough for the model, ...) have a
 * single owner, gateConfigValidate(), which gateParseCreate() runs last and
 * whose message reaches the client unchanged.
 *
 * FAILURES LEAVE NOTHING BEHIND. Each parser has one failure exit that
 * releases whatever it built and zeroes its output, so a caller may ignore
 * the output of a failed parse or release it again, as it prefers. Outputs
 * of the scalar helpers are written only on success.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gate_args.h"

/* Strict numbers rely on isfinite() really classifying its argument. Under
 * -ffast-math (-ffinite-math-only) the compiler may assume that NaN and Inf
 * do not exist, fold the checks away and let "1e999" through. */
#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__
#error "gate_args.c must not be compiled with -ffast-math / -ffinite-math-only"
#endif

/* ------------------------------ Error messages --------------------------- */

#ifdef __GNUC__
static int gateArgsFail(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
#endif

/* Store a message and return GATE_ERR, so that a failure site is a single
 * "return gateArgsFail(...)". A NULL or empty buffer just drops the text. */
static int gateArgsFail(char *err, size_t errlen, const char *fmt, ...) {
    if (err != NULL && errlen > 0) {
        va_list ap;

        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return GATE_ERR;
}

static int gateArgsNoMemory(char *err, size_t errlen) {
    return gateArgsFail(err, errlen, "out of memory");
}

/* A parser that succeeds must not leave the text of an older failure in a
 * buffer the caller reuses. */
static void gateArgsClearError(char *err, size_t errlen) {
    if (err != NULL && errlen > 0) err[0] = '\0';
}

/* Bytes of an argument shown in a message. A whole class label fits, so the
 * label a client has to fix is never abbreviated. */
#define GATE_QUOTE_MAX 64
#define GATE_QUOTE_BUF (GATE_QUOTE_MAX + 4) /* content, "...", NUL */
_Static_assert(GATE_QUOTE_MAX >= GATE_MAX_LABEL_BYTES, "a label must be quoted in full");

/* Render an argument for an error message into 'buf' (GATE_QUOTE_BUF bytes)
 * and return 'buf'. The argument is arbitrary client data while the message
 * becomes a RESP error line, so only printable ASCII and well formed UTF-8
 * of non-control code points are copied; every other byte becomes '?'. At
 * most GATE_QUOTE_MAX bytes are shown, a cut is marked with "..." and never
 * splits a multi-byte sequence.
 *
 * gateValidName() restricted to one candidate sequence is exactly the test
 * "these bytes are one printable code point", which keeps a single
 * definition of the gate's UTF-8 policy. */
static const char *gateArgsQuote(char *buf, const gateArg *a) {
    size_t i = 0, o = 0;

    while (i < a->len) {
        unsigned char c = (unsigned char)a->ptr[i];
        size_t n = 1;

        if (c >= 0xc2 && c <= 0xdf) n = 2;
        else if (c >= 0xe0 && c <= 0xef) n = 3;
        else if (c >= 0xf0 && c <= 0xf4) n = 4;
        if (n > a->len - i) n = 1; /* truncated sequence: judge the lead byte alone */

        if (o + n > GATE_QUOTE_MAX) break;
        if (gateValidName(a->ptr + i, n, n)) {
            memcpy(buf + o, a->ptr + i, n);
            o += n;
            i += n;
        } else {
            buf[o++] = '?';
            i++;
        }
    }
    if (i < a->len) {
        memcpy(buf + o, "...", 3);
        o += 3;
    }
    buf[o] = '\0';
    return buf;
}

/* --------------------------- Scalar helpers ------------------------------ */

/* Longest numeric argument accepted. The shortest decimal that round trips
 * any double needs at most 24 characters ("-2.2250738585072014e-308"), so
 * this leaves room for padded or over-precise input while the stack copy
 * keeps a fixed size. */
#define GATE_NUMBER_MAX_BYTES 63

/* The accepted language is exactly the decimal floating point syntax
 *
 *     [+-] (digits [. [digits]] | . digits) [(e|E) [+-] digits]
 *
 * enforced in two steps. The alphabet check rejects everything strtod()
 * would accept beyond that syntax: leading white space, hex floats, "inf",
 * "infinity" and "nan" all need a character outside the alphabet, and so
 * does an embedded NUL, which would otherwise end the copy early and let
 * "1\0junk" pass as 1. Then strtod() must consume the whole copy, which
 * rejects every malformed arrangement of the alphabet ("1e", "--1", "1.2.3").
 *
 * The value is the correctly rounded double. A magnitude too large for a
 * double is rejected (strtod() returns an infinity). One too small is not an
 * error: it rounds to a denormal or to zero, like any other decimal that has
 * no exact representation; a caller that needs a positive value checks the
 * range, and reports it better than a parse error would.
 *
 * strtod() honours LC_NUMERIC, which Redis never changes from "C". Should an
 * embedder do so, a locale with another radix character makes inputs that
 * contain '.' fail the consumption test: they are rejected, never misread. */
int gateParseDouble(const char *s, size_t len, double *out) {
    char buf[GATE_NUMBER_MAX_BYTES + 1];
    char *end;
    double v;
    size_t i;

    if (s == NULL || len == 0 || len > GATE_NUMBER_MAX_BYTES) return 0;
    for (i = 0; i < len; i++) {
        char c = s[i];

        if (!((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' ||
              c == 'e' || c == 'E'))
            return 0;
    }
    memcpy(buf, s, len);
    buf[len] = '\0';
    v = strtod(buf, &end);
    if (end != buf + len || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

int gateParseU64(const char *s, size_t len, uint64_t *out) {
    uint64_t v = 0;
    size_t i;

    /* UINT64_MAX has 20 digits. Zero padding is accepted inside that width
     * only, which also bounds the loop for hostile lengths. */
    if (s == NULL || len == 0 || len > 20) return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        unsigned d;

        if (c < '0' || c > '9') return 0;
        d = (unsigned)(c - '0');
        if (v > (UINT64_MAX - d) / 10) return 0; /* v * 10 + d would wrap */
        v = v * 10 + d;
    }
    *out = v;
    return 1;
}

/* Only 'a'-'z' are folded, by hand: toupper() depends on the locale, and a
 * blanket "c & ~0x20" would also make '`' equal to '@' and '{' to '['. The
 * token doubles as the length check, so an argument with an embedded NUL can
 * never match. */
int gateArgIs(const gateArg *a, const char *token) {
    size_t i;

    for (i = 0; i < a->len; i++) {
        unsigned char c = (unsigned char)a->ptr[i];

        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        if (token[i] == '\0' || c != (unsigned char)token[i]) return 0;
    }
    return token[i] == '\0';
}

#define GATE_HEX64_DIGITS 16
_Static_assert(GATE_DOUBLE_HEX_LEN == GATE_HEX64_DIGITS, "a double travels as one 64 bit word");
_Static_assert(sizeof(double) == sizeof(uint64_t), "IEEE-754 binary64 doubles are assumed");
_Static_assert(GATE_OBSERVATION_ID_MAX >= GATE_HEX64_DIGITS + 1 + 20 + 1,
               "GATE_OBSERVATION_ID_MAX must hold the longest id and its terminator");

void gateFormatHex64(char *buf, uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < GATE_HEX64_DIGITS; i++)
        buf[i] = digits[(v >> (60 - 4 * i)) & 0xf];
    buf[GATE_HEX64_DIGITS] = '\0';
}

/* Exactly 16 hex digits. Both cases are accepted here and therefore by the
 * three parsers built on it: the header says so for observation ids, and
 * one hex grammar is simpler to reason about than two. The formatters only
 * ever write lower case. */
int gateParseHex64(const char *s, size_t len, uint64_t *out) {
    uint64_t v = 0;
    int i;

    if (s == NULL || len != GATE_HEX64_DIGITS) return 0;
    for (i = 0; i < GATE_HEX64_DIGITS; i++) {
        unsigned char c = (unsigned char)s[i];
        unsigned d;

        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A') + 10;
        else return 0;
        v = (v << 4) | d;
    }
    *out = v;
    return 1;
}

/* The bit pattern is moved with memcpy(): reading a double through a
 * uint64_t lvalue would break the aliasing rules. */
void gateFormatDoubleBits(char *buf, double v) {
    uint64_t bits;

    memcpy(&bits, &v, sizeof(bits));
    gateFormatHex64(buf, bits);
}

/* The patterns of the infinities and of every NaN are refused. This file
 * never hands a non-finite number to its caller, and that holds for a value
 * replicated as bits just as for one typed as a decimal: the primary only
 * replicates values it realized and validated, so such a pattern can only
 * come from a corrupted stream and must stop there. The sign of zero and
 * denormals are preserved exactly. */
int gateParseDoubleBits(const char *s, size_t len, double *out) {
    uint64_t bits;
    double v;

    if (!gateParseHex64(s, len, &bits)) return 0;
    memcpy(&v, &bits, sizeof(v));
    if (!isfinite(v)) return 0;
    *out = v;
    return 1;
}

/* Returns the length of the id (without its terminator), or 0 after storing
 * an empty string when 'buflen' is too small; GATE_OBSERVATION_ID_MAX bytes
 * are always enough. Formatting is total: seq 0 is written like any other
 * value although the parser rejects it, because 0 means "no observation"
 * and is never sent to a client. Written by hand rather than with snprintf()
 * so that the text cannot depend on the locale. */
size_t gateFormatObservationId(char *buf, size_t buflen, uint64_t epoch, uint64_t seq) {
    char digits[20]; /* least significant first */
    size_t ndigits = 0, len, i;

    do {
        digits[ndigits++] = (char)('0' + seq % 10);
        seq /= 10;
    } while (seq != 0);

    len = GATE_HEX64_DIGITS + 1 + ndigits;
    if (buf == NULL || buflen < len + 1) {
        if (buf != NULL && buflen > 0) buf[0] = '\0';
        return 0;
    }
    gateFormatHex64(buf, epoch);
    buf[GATE_HEX64_DIGITS] = '-';
    for (i = 0; i < ndigits; i++)
        buf[GATE_HEX64_DIGITS + 1 + i] = digits[ndigits - 1 - i];
    buf[len] = '\0';
    return len;
}

/* An id has exactly one spelling per (epoch, seq) apart from the case of the
 * hex digits: a seq with leading zeros is refused, so two different strings
 * can never name the same observation with the same epoch spelling. The
 * first digit being non-zero also gives seq >= 1. */
int gateParseObservationId(const char *s, size_t len, uint64_t *epoch, uint64_t *seq) {
    uint64_t e, q;

    if (s == NULL || len < GATE_HEX64_DIGITS + 2) return 0;
    if (!gateParseHex64(s, GATE_HEX64_DIGITS, &e)) return 0;
    if (s[GATE_HEX64_DIGITS] != '-' || s[GATE_HEX64_DIGITS + 1] == '0') return 0;
    if (!gateParseU64(s + GATE_HEX64_DIGITS + 1, len - GATE_HEX64_DIGITS - 1, &q)) return 0;
    *epoch = e;
    *seq = q;
    return 1;
}

/* ------------------------ Shared grammar pieces --------------------------- */

static int gateArgsAllDigits(const gateArg *a) {
    size_t i;

    for (i = 0; i < a->len; i++)
        if (a->ptr[i] < '0' || a->ptr[i] > '9') return 0;
    return a->len > 0;
}

/* Parse the unsigned decimal argument 'a' and require lo <= value <= hi.
 * 'name' and 'suffix' name it in messages ("MAXPENDING" + "", "CLASSES" +
 * " count"), 'unit' follows the limits (" ms"). This is the only place where
 * an integer argument is accepted, and it compares the full 64 bit value:
 * callers may narrow '*out' to any type that holds 'hi'.
 *
 * A digits-only argument that gateParseU64() refuses is too large for 64
 * bits (or zero padded past 20 digits), so it is reported as out of range
 * rather than as not being a number. */
static int gateArgsInteger(const gateArg *a, const char *name, const char *suffix,
                           uint64_t lo, uint64_t hi, const char *unit, uint64_t *out,
                           char *err, size_t errlen)
{
    uint64_t v;

    if (gateParseU64(a->ptr, a->len, &v)) {
        if (v >= lo && v <= hi) {
            *out = v;
            return GATE_OK;
        }
    } else if (!gateArgsAllDigits(a)) {
        return gateArgsFail(err, errlen, "%s%s must be an unsigned integer", name, suffix);
    }
    if (lo == 0)
        return gateArgsFail(err, errlen, "%s%s is out of range: must be at most %llu%s",
                            name, suffix, (unsigned long long)hi, unit);
    return gateArgsFail(err, errlen, "%s%s is out of range: must be between %llu and %llu%s",
                        name, suffix, (unsigned long long)lo, (unsigned long long)hi, unit);
}

/* Parse the count that announces a list ("CLASSES 3 a b c") and require the
 * whole list to be present: 'per_entry' arguments for each of the entries,
 * out of the 'after' arguments that follow the count. The count is what
 * delimits the list, so that its entries may spell anything, option names
 * included; consequently the only way for a list to be short is to run into
 * the end of the command, which is what the message says. */
static int gateArgsCount(const gateArg *a, const char *name, uint64_t lo, uint64_t hi,
                         unsigned per_entry, int after, uint32_t *count,
                         char *err, size_t errlen)
{
    uint64_t n, need;

    if (gateArgsInteger(a, name, " count", lo, hi, "", &n, err, errlen) != GATE_OK)
        return GATE_ERR;
    need = n * per_entry; /* n <= hi, a small hard limit: no overflow */
    if ((uint64_t)after < need)
        return gateArgsFail(err, errlen,
                            "%s count %llu needs %llu argument(s) after it but only %d follow",
                            name, (unsigned long long)n, (unsigned long long)need, after);
    *count = (uint32_t)n;
    return GATE_OK;
}

/* Every list length is bounded by one of these, which keeps 'need' above
 * and the int index arithmetic of the parsers far from overflow. */
_Static_assert(GATE_MAX_CLASSES <= 65536 && GATE_MAX_FEATURES_PER_REQUEST <= 65536,
               "list counts must stay small enough for int index arithmetic");

/* NUL terminated copy of an argument that already passed gateValidName(),
 * hence is non-empty, short and free of NULs: the copy is faithful. */
static char *gateArgsDup(const gateArg *a) {
    char *copy = gate_malloc(a->len + 1);

    if (copy == NULL) return NULL;
    memcpy(copy, a->ptr, a->len);
    copy[a->len] = '\0';
    return copy;
}

/* The request section named by 'a' (TEXT, VECTOR or FEATURES) as its
 * GATE_INPUT_* bit, with the canonical spelling in '*name'; 0 if 'a' is not
 * a section token. Shared by INPUTS and by the request parser so that a gate
 * can only be configured with sections a request can carry. */
static uint32_t gateArgsSection(const gateArg *a, const char **name) {
    static const struct {
        const char *name;
        uint32_t bit;
    } sections[] = {
        {"TEXT", GATE_INPUT_TEXT},
        {"VECTOR", GATE_INPUT_VECTOR},
        {"FEATURES", GATE_INPUT_FEATURES},
    };
    size_t i;

    for (i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
        if (gateArgIs(a, sections[i].name)) {
            *name = sections[i].name;
            return sections[i].bit;
        }
    }
    return 0;
}

/* -------------------------------- GCREATE -------------------------------- */

typedef enum gateCreateOption {
    GATE_OPT_MODE = 0,
    GATE_OPT_ALPHA,
    GATE_OPT_DELTA,
    GATE_OPT_AUDIT,
    GATE_OPT_COLDAUDIT,
    GATE_OPT_THRESHOLDS,
    GATE_OPT_CLASSES,
    GATE_OPT_CLASSALPHA,
    GATE_OPT_ALWAYSDEFER,
    GATE_OPT_INPUTS,
    GATE_OPT_TEXTDIM,
    GATE_OPT_VECTORDIM,
    GATE_OPT_FEATUREDIM,
    GATE_OPT_LR,
    GATE_OPT_L2,
    GATE_OPT_TEACHER,
    GATE_OPT_SCHEMAVERSION,
    GATE_OPT_MAXMEMORY,
    GATE_OPT_MAXPENDING,
    GATE_OPT_OBSERVATIONTTL,
    GATE_OPT_CERTTTL,
    GATE_OPT_DRIFTMARGIN,
    GATE_OPT_ONMISSING,
    GATE_OPT_EXPAND,
    GATE_OPT_COUNT
} gateCreateOption;

typedef struct gateCreateOptionSpec {
    const char *name;
    int min_args;     /* fewest arguments the option can be followed by: its
                       * value, or a count and the shortest legal list */
    uint64_t lo, hi;  /* integer options: accepted range of the value, or of
                       * the count for a list */
    const char *unit; /* integer options: text after the limits in messages */
} gateCreateOptionSpec;

static const gateCreateOptionSpec gateCreateOptions[GATE_OPT_COUNT] = {
    [GATE_OPT_MODE] = {"MODE", 1, 0, 0, ""},
    [GATE_OPT_ALPHA] = {"ALPHA", 1, 0, 0, ""},
    [GATE_OPT_DELTA] = {"DELTA", 1, 0, 0, ""},
    [GATE_OPT_AUDIT] = {"AUDIT", 1, 0, 0, ""},
    [GATE_OPT_COLDAUDIT] = {"COLDAUDIT", 1, 0, 0, ""},
    [GATE_OPT_THRESHOLDS] = {"THRESHOLDS", 1, GATE_MIN_THRESHOLDS, GATE_MAX_THRESHOLDS, ""},
    [GATE_OPT_CLASSES] = {"CLASSES", 1 + GATE_MIN_CLASSES, GATE_MIN_CLASSES, GATE_MAX_CLASSES, ""},
    [GATE_OPT_CLASSALPHA] = {"CLASSALPHA", 1 + 2, 1, GATE_MAX_CLASSES, ""},
    [GATE_OPT_ALWAYSDEFER] = {"ALWAYSDEFER", 1 + 1, 1, GATE_MAX_CLASSES, ""},
    [GATE_OPT_INPUTS] = {"INPUTS", 1 + 1, 1, 3, ""},
    [GATE_OPT_TEXTDIM] = {"TEXTDIM", 1, 1, GATE_MAX_TEXT_DIM, ""},
    [GATE_OPT_VECTORDIM] = {"VECTORDIM", 1, 1, GATE_MAX_VECTOR_DIM, ""},
    [GATE_OPT_FEATUREDIM] = {"FEATUREDIM", 1, 1, GATE_MAX_FEATURE_DIM, ""},
    [GATE_OPT_LR] = {"LR", 1, 0, 0, ""},
    [GATE_OPT_L2] = {"L2", 1, 0, 0, ""},
    [GATE_OPT_TEACHER] = {"TEACHER", 1, 0, 0, ""},
    [GATE_OPT_SCHEMAVERSION] = {"SCHEMAVERSION", 1, 0, 0, ""},
    /* No lower bound here: the smallest usable MAXMEMORY depends on the rest
     * of the configuration, and gateConfigValidate() names it in bytes. */
    [GATE_OPT_MAXMEMORY] = {"MAXMEMORY", 1, 0, GATE_MAX_MEMORY_LIMIT, " bytes"},
    [GATE_OPT_MAXPENDING] = {"MAXPENDING", 1, 1, GATE_MAX_PENDING_LIMIT, ""},
    [GATE_OPT_OBSERVATIONTTL] = {"OBSERVATIONTTL", 1, 1, GATE_MAX_TTL_MS, " ms"},
    [GATE_OPT_CERTTTL] = {"CERTTTL", 1, 1, GATE_MAX_TTL_MS, " ms"},
    [GATE_OPT_DRIFTMARGIN] = {"DRIFTMARGIN", 1, 0, 0, ""},
    [GATE_OPT_ONMISSING] = {"ONMISSING", 1, 0, 0, ""},
    [GATE_OPT_EXPAND] = {"EXPAND", 1, 0, 0, ""},
};

/* What makes the narrowing stores of the walk lossless: every value that
 * passed its range check fits the config field it is stored into. */
_Static_assert(GATE_MAX_THRESHOLDS <= UINT32_MAX && GATE_MAX_CLASSES <= UINT32_MAX &&
               GATE_MAX_TEXT_DIM <= UINT32_MAX && GATE_MAX_VECTOR_DIM <= UINT32_MAX &&
               GATE_MAX_FEATURE_DIM <= UINT32_MAX && GATE_MAX_PENDING_LIMIT <= UINT32_MAX,
               "a hard limit does not fit its uint32_t config field");
_Static_assert(GATE_MAX_TTL_MS > 0 && GATE_MAX_TTL_MS <= INT64_MAX,
               "GATE_MAX_TTL_MS does not fit an int64_t config field");

typedef struct gateCreateState {
    const gateArg *argv;
    int argc;
    gateConfig *cfg;
    uint8_t seen[GATE_OPT_COUNT]; /* the option was written */
    /* The class policy is resolved after the walk, because CLASSES may come
     * later in the command: until then only where its lists are is known. */
    const gateArg *classalpha;    /* 'nclassalpha' label / alpha pairs */
    uint32_t nclassalpha;
    const gateArg *alwaysdefer;   /* 'nalwaysdefer' labels */
    uint32_t nalwaysdefer;
    char *err;
    size_t errlen;
} gateCreateState;

static int gateCreateLookup(const gateArg *a) {
    int opt;

    for (opt = 0; opt < GATE_OPT_COUNT; opt++)
        if (gateArgIs(a, gateCreateOptions[opt].name)) return opt;
    return -1;
}

static int gateCreateDouble(gateCreateState *st, gateCreateOption opt, const gateArg *a,
                            double *out)
{
    if (!gateParseDouble(a->ptr, a->len, out))
        return gateArgsFail(st->err, st->errlen, "%s must be a finite decimal number",
                            gateCreateOptions[opt].name);
    return GATE_OK;
}

static int gateCreateInteger(gateCreateState *st, gateCreateOption opt, const gateArg *a,
                             uint64_t *out)
{
    const gateCreateOptionSpec *spec = &gateCreateOptions[opt];

    return gateArgsInteger(a, spec->name, "", spec->lo, spec->hi, spec->unit, out,
                           st->err, st->errlen);
}

/* The count of a list option, with the list checked to be complete. 'after'
 * counts the arguments that follow the count. */
static int gateCreateCount(gateCreateState *st, gateCreateOption opt, const gateArg *a,
                           unsigned per_entry, int after, uint32_t *count)
{
    const gateCreateOptionSpec *spec = &gateCreateOptions[opt];

    return gateArgsCount(a, spec->name, spec->lo, spec->hi, per_entry, after, count,
                         st->err, st->errlen);
}

/* TEACHER / SCHEMAVERSION: validate and store a copy. */
static int gateCreateVersion(gateCreateState *st, gateCreateOption opt, const gateArg *a,
                             char **out)
{
    if (!gateValidName(a->ptr, a->len, GATE_MAX_VERSION_BYTES))
        return gateArgsFail(st->err, st->errlen,
                            "%s must be 1 to %u bytes of UTF-8 without control characters",
                            gateCreateOptions[opt].name, GATE_MAX_VERSION_BYTES);
    *out = gateArgsDup(a);
    if (*out == NULL) return gateArgsNoMemory(st->err, st->errlen);
    return GATE_OK;
}

static int gateCreateClasses(gateCreateState *st, const gateArg *labels, uint32_t count) {
    gateConfig *cfg = st->cfg;
    char quote[GATE_QUOTE_BUF];
    uint32_t i, j;

    /* nclasses is set together with the zero-filled array: from here on
     * gateConfigFree() releases a partially filled list correctly. */
    cfg->labels = gate_calloc(count, sizeof(char *));
    if (cfg->labels == NULL) return gateArgsNoMemory(st->err, st->errlen);
    cfg->nclasses = count;

    for (i = 0; i < count; i++) {
        const gateArg *label = &labels[i];

        if (!gateValidName(label->ptr, label->len, GATE_MAX_LABEL_BYTES))
            return gateArgsFail(st->err, st->errlen,
                                "class labels must be 1 to %u bytes of UTF-8 without "
                                "control characters", GATE_MAX_LABEL_BYTES);
        for (j = 0; j < i; j++) {
            if (labels[j].len == label->len &&
                memcmp(labels[j].ptr, label->ptr, label->len) == 0)
                return gateArgsFail(st->err, st->errlen, "duplicate class label '%s'",
                                    gateArgsQuote(quote, label));
        }
        cfg->labels[i] = gateArgsDup(label);
        if (cfg->labels[i] == NULL) return gateArgsNoMemory(st->err, st->errlen);
    }
    return GATE_OK;
}

static int gateCreateInputs(gateCreateState *st, const gateArg *inputs, uint32_t count) {
    gateConfig *cfg = st->cfg;
    char quote[GATE_QUOTE_BUF];
    uint32_t i;

    for (i = 0; i < count; i++) {
        const char *name = NULL;
        uint32_t bit = gateArgsSection(&inputs[i], &name);

        if (bit == 0)
            return gateArgsFail(st->err, st->errlen,
                                "INPUTS accepts TEXT, VECTOR or FEATURES, not '%s'",
                                gateArgsQuote(quote, &inputs[i]));
        if (cfg->schema.inputs & bit)
            return gateArgsFail(st->err, st->errlen, "input %s listed more than once", name);
        cfg->schema.inputs |= bit;
    }
    return GATE_OK;
}

/* First pass: read the options in command order into the config. Values
 * are stored as they are read; nothing is "partially applied" by that,
 * because every failure of gateParseCreate() releases the whole config. */
static int gateCreateWalk(gateCreateState *st) {
    const gateArg *argv = st->argv;
    gateConfig *cfg = st->cfg;
    char quote[GATE_QUOTE_BUF];
    int i = 0;

    while (i < st->argc) {
        const gateCreateOptionSpec *spec;
        const gateArg *val;
        int avail = st->argc - i - 1; /* arguments after the option token */
        int used = 1;                 /* ... of which this option consumes */
        int rc = GATE_OK;
        int opt = gateCreateLookup(&argv[i]);
        uint32_t count = 0;
        uint64_t u = 0;
        double d = 0;

        if (opt < 0)
            return gateArgsFail(st->err, st->errlen, "unknown option '%s'",
                                gateArgsQuote(quote, &argv[i]));
        spec = &gateCreateOptions[opt];
        if (st->seen[opt])
            return gateArgsFail(st->err, st->errlen, "option %s given more than once",
                                spec->name);
        st->seen[opt] = 1;
        if (avail < spec->min_args)
            return gateArgsFail(st->err, st->errlen, "option %s requires %d argument(s)",
                                spec->name, spec->min_args);
        val = &argv[i + 1];

        switch ((gateCreateOption)opt) {
        case GATE_OPT_MODE:
            if (gateArgIs(val, "SCORE")) cfg->mode = GATE_MODE_SCORE;
            else if (gateArgIs(val, "CLASSIFIER")) cfg->mode = GATE_MODE_CLASSIFIER;
            else rc = gateArgsFail(st->err, st->errlen, "MODE must be SCORE or CLASSIFIER");
            break;
        case GATE_OPT_ALPHA:
            rc = gateCreateDouble(st, opt, val, &cfg->alpha);
            break;
        case GATE_OPT_DELTA:
            rc = gateCreateDouble(st, opt, val, &cfg->delta);
            break;
        case GATE_OPT_AUDIT:
            rc = gateCreateDouble(st, opt, val, &cfg->audit);
            break;
        case GATE_OPT_COLDAUDIT:
            rc = gateCreateDouble(st, opt, val, &cfg->cold_audit);
            break;
        case GATE_OPT_LR:
            rc = gateCreateDouble(st, opt, val, &cfg->lr);
            break;
        case GATE_OPT_L2:
            rc = gateCreateDouble(st, opt, val, &cfg->l2);
            break;
        case GATE_OPT_DRIFTMARGIN:
            /* Checked here and not left to the validator: in the config a
             * negative margin is the encoding of "not given", so a negative
             * argument would silently select the default. For the same
             * reason "-0" is stored as +0, which no sign test can mistake
             * for that encoding. */
            rc = gateCreateDouble(st, opt, val, &d);
            if (rc == GATE_OK && !(d >= 0.0 && d <= 1.0))
                rc = gateArgsFail(st->err, st->errlen, "DRIFTMARGIN must be between 0 and 1");
            if (rc == GATE_OK) cfg->drift_margin = d == 0.0 ? 0.0 : d;
            break;
        case GATE_OPT_THRESHOLDS:
            rc = gateCreateInteger(st, opt, val, &u);
            if (rc == GATE_OK) cfg->nthresholds = (uint32_t)u;
            break;
        case GATE_OPT_TEXTDIM:
            rc = gateCreateInteger(st, opt, val, &u);
            if (rc == GATE_OK) cfg->schema.text_dim = (uint32_t)u;
            break;
        case GATE_OPT_VECTORDIM:
            rc = gateCreateInteger(st, opt, val, &u);
            if (rc == GATE_OK) cfg->schema.vector_dim = (uint32_t)u;
            break;
        case GATE_OPT_FEATUREDIM:
            rc = gateCreateInteger(st, opt, val, &u);
            if (rc == GATE_OK) cfg->schema.feature_dim = (uint32_t)u;
            break;
        case GATE_OPT_MAXMEMORY:
            rc = gateCreateInteger(st, opt, val, &u);
            if (rc == GATE_OK) cfg->max_memory = u;
            break;
        case GATE_OPT_MAXPENDING:
            rc = gateCreateInteger(st, opt, val, &u);
            if (rc == GATE_OK) cfg->max_pending = (uint32_t)u;
            break;
        case GATE_OPT_OBSERVATIONTTL:
            rc = gateCreateInteger(st, opt, val, &u);
            if (rc == GATE_OK) cfg->observation_ttl_ms = (int64_t)u;
            break;
        case GATE_OPT_CERTTTL:
            rc = gateCreateInteger(st, opt, val, &u);
            if (rc == GATE_OK) cfg->cert_ttl_ms = (int64_t)u;
            break;
        case GATE_OPT_TEACHER:
            rc = gateCreateVersion(st, opt, val, &cfg->teacher);
            break;
        case GATE_OPT_SCHEMAVERSION:
            rc = gateCreateVersion(st, opt, val, &cfg->schema_version);
            break;
        case GATE_OPT_CLASSES:
            rc = gateCreateCount(st, opt, val, 1, avail - 1, &count);
            if (rc == GATE_OK) rc = gateCreateClasses(st, val + 1, count);
            used = 1 + (int)count;
            break;
        case GATE_OPT_INPUTS:
            rc = gateCreateCount(st, opt, val, 1, avail - 1, &count);
            if (rc == GATE_OK) rc = gateCreateInputs(st, val + 1, count);
            used = 1 + (int)count;
            break;
        case GATE_OPT_CLASSALPHA:
            rc = gateCreateCount(st, opt, val, 2, avail - 1, &count);
            st->classalpha = val + 1;
            st->nclassalpha = count;
            used = 1 + 2 * (int)count;
            break;
        case GATE_OPT_ALWAYSDEFER:
            rc = gateCreateCount(st, opt, val, 1, avail - 1, &count);
            st->alwaysdefer = val + 1;
            st->nalwaysdefer = count;
            used = 1 + (int)count;
            break;
        case GATE_OPT_ONMISSING:
            if (gateArgIs(val, "REVOKE")) cfg->on_missing = GATE_ONMISSING_REVOKE;
            else if (gateArgIs(val, "ERROR")) cfg->on_missing = GATE_ONMISSING_ERROR;
            else rc = gateArgsFail(st->err, st->errlen, "ONMISSING must be REVOKE or ERROR");
            break;
        case GATE_OPT_EXPAND:
            if (gateArgIs(val, "NO")) cfg->expand = 0;
            else if (gateArgIs(val, "YES")) cfg->expand = 1;
            else rc = gateArgsFail(st->err, st->errlen, "EXPAND must be NO or YES");
            break;
        case GATE_OPT_COUNT:
            break; /* not an option: gateCreateLookup() never returns it */
        }
        if (rc != GATE_OK) return GATE_ERR;
        i += 1 + used;
    }
    return GATE_OK;
}

/* Turn the CLASSALPHA / ALWAYSDEFER lists remembered by the walk into the
 * per-class arrays, now that the labels and ALPHA are final. Whether a
 * class target is acceptable (0 < target <= ALPHA) and whether some class
 * is left to serve are config invariants, checked by the validator. */
static int gateCreateClassPolicy(gateCreateState *st) {
    gateConfig *cfg = st->cfg;
    uint8_t has_alpha[GATE_MAX_CLASSES] = {0};
    char quote[GATE_QUOTE_BUF];
    uint32_t k = cfg->nclasses, i;

    if (!st->seen[GATE_OPT_CLASSALPHA] && !st->seen[GATE_OPT_ALWAYSDEFER]) return GATE_OK;
    if (k == 0)
        return gateArgsFail(st->err, st->errlen, "CLASSALPHA and ALWAYSDEFER require CLASSES");

    cfg->class_alpha = gate_malloc(sizeof(double) * k);
    cfg->class_defer = gate_calloc(k, sizeof(uint8_t));
    if (cfg->class_alpha == NULL || cfg->class_defer == NULL)
        return gateArgsNoMemory(st->err, st->errlen);
    cfg->class_mode = 1;
    for (i = 0; i < k; i++) cfg->class_alpha[i] = cfg->alpha;

    for (i = 0; i < st->nclassalpha; i++) {
        const gateArg *label = &st->classalpha[2 * i];
        const gateArg *target = &st->classalpha[2 * i + 1];
        int idx = gateConfigLabelIndex(cfg, label->ptr, label->len);

        if (idx < 0)
            return gateArgsFail(st->err, st->errlen, "unknown class label '%s'",
                                gateArgsQuote(quote, label));
        if (has_alpha[idx])
            return gateArgsFail(st->err, st->errlen,
                                "class label '%s' listed more than once in CLASSALPHA",
                                gateArgsQuote(quote, label));
        if (!gateParseDouble(target->ptr, target->len, &cfg->class_alpha[idx]))
            return gateArgsFail(st->err, st->errlen,
                                "CLASSALPHA for '%s' must be a finite decimal number",
                                gateArgsQuote(quote, label));
        has_alpha[idx] = 1;
    }
    for (i = 0; i < st->nalwaysdefer; i++) {
        const gateArg *label = &st->alwaysdefer[i];
        int idx = gateConfigLabelIndex(cfg, label->ptr, label->len);

        if (idx < 0)
            return gateArgsFail(st->err, st->errlen, "unknown class label '%s'",
                                gateArgsQuote(quote, label));
        if (cfg->class_defer[idx])
            return gateArgsFail(st->err, st->errlen,
                                "class label '%s' listed more than once in ALWAYSDEFER",
                                gateArgsQuote(quote, label));
        /* A risk target for a class that is never accepted is a
         * contradiction; refusing it is better than picking a winner. */
        if (has_alpha[idx])
            return gateArgsFail(st->err, st->errlen,
                                "class label '%s' cannot be in both CLASSALPHA and ALWAYSDEFER",
                                gateArgsQuote(quote, label));
        cfg->class_defer[idx] = 1;
    }
    return GATE_OK;
}

/* Second pass: everything that needs the whole command. */
static int gateCreateFinish(gateCreateState *st) {
    static const gateCreateOption classifier_only[] = {
        GATE_OPT_INPUTS, GATE_OPT_TEXTDIM, GATE_OPT_VECTORDIM,
        GATE_OPT_FEATUREDIM, GATE_OPT_LR, GATE_OPT_L2,
    };
    static const gateArg default_version = {GATE_DEFAULT_VERSION,
                                            sizeof(GATE_DEFAULT_VERSION) - 1};
    gateConfig *cfg = st->cfg;
    size_t i;

    if (!st->seen[GATE_OPT_MODE])
        return gateArgsFail(st->err, st->errlen, "missing required option MODE");
    if (!st->seen[GATE_OPT_ALPHA])
        return gateArgsFail(st->err, st->errlen, "missing required option ALPHA");

    if (cfg->mode == GATE_MODE_SCORE) {
        /* Rejected by name instead of ignored: a client that sets LR on a
         * SCORE gate believes something is being trained. */
        for (i = 0; i < sizeof(classifier_only) / sizeof(classifier_only[0]); i++)
            if (st->seen[classifier_only[i]])
                return gateArgsFail(st->err, st->errlen, "SCORE mode does not accept %s",
                                    gateCreateOptions[classifier_only[i]].name);
    } else if (st->seen[GATE_OPT_INPUTS]) {
        /* The schema invariant "a dimension is 0 exactly when its modality
         * is not configured" is established here, from what was written.
         * Without INPUTS there is nothing to pair the dimensions with: the
         * validator reports the missing INPUTS, which is the actual error. */
        gateSchema *s = &cfg->schema;

        if (s->inputs & GATE_INPUT_TEXT) {
            if (!st->seen[GATE_OPT_TEXTDIM]) s->text_dim = GATE_DEFAULT_TEXT_DIM;
        } else if (st->seen[GATE_OPT_TEXTDIM]) {
            return gateArgsFail(st->err, st->errlen, "TEXTDIM requires the TEXT input");
        }
        if (s->inputs & GATE_INPUT_VECTOR) {
            /* No default: the dimension of an embedding is a property of
             * the client's model, and a guess would reject every request. */
            if (!st->seen[GATE_OPT_VECTORDIM])
                return gateArgsFail(st->err, st->errlen, "the VECTOR input requires VECTORDIM");
        } else if (st->seen[GATE_OPT_VECTORDIM]) {
            return gateArgsFail(st->err, st->errlen, "VECTORDIM requires the VECTOR input");
        }
        if (s->inputs & GATE_INPUT_FEATURES) {
            if (!st->seen[GATE_OPT_FEATUREDIM]) s->feature_dim = GATE_DEFAULT_FEATURE_DIM;
        } else if (st->seen[GATE_OPT_FEATUREDIM]) {
            return gateArgsFail(st->err, st->errlen, "FEATUREDIM requires the FEATURES input");
        }
    }

    if (!st->seen[GATE_OPT_COLDAUDIT]) cfg->cold_audit = cfg->audit;

    if (cfg->teacher == NULL) {
        cfg->teacher = gateArgsDup(&default_version);
        if (cfg->teacher == NULL) return gateArgsNoMemory(st->err, st->errlen);
    }
    if (cfg->schema_version == NULL) {
        cfg->schema_version = gateArgsDup(&default_version);
        if (cfg->schema_version == NULL) return gateArgsNoMemory(st->err, st->errlen);
    }

    return gateCreateClassPolicy(st);
}

int gateParseCreate(const gateArg *argv, int argc, gateConfig *cfg,
                    char *err, size_t errlen)
{
    gateCreateState st;

    gateArgsClearError(err, errlen);
    gateConfigInit(cfg);
    memset(&st, 0, sizeof(st));
    st.argv = argv;
    st.argc = argc;
    st.cfg = cfg;
    st.err = err;
    st.errlen = errlen;

    if (gateCreateWalk(&st) != GATE_OK ||
        gateCreateFinish(&st) != GATE_OK ||
        gateConfigValidate(cfg, err, errlen) != GATE_OK)
    {
        gateConfigFree(cfg);
        return GATE_ERR;
    }
    return GATE_OK;
}

/* ----------------------- Request sections (CLASSIFIER) -------------------- */

void gateRequestFree(gateRequest *req) {
    if (req == NULL) return;
    gate_free(req->features);
    memset(req, 0, sizeof(*req));
}

/* FEATURES count name value [name value ...]. 'after' counts the arguments
 * that follow the count. Repeated names are not looked for here: that needs
 * the hashing of gateFeaturize(), which reports them. */
static int gateSectionsFeatures(const gateArg *count_arg, int after, gateRequest *req,
                                uint32_t *npairs, char *err, size_t errlen)
{
    const gateArg *pairs = count_arg + 1;
    char quote[GATE_QUOTE_BUF];
    uint32_t count, i;

    if (gateArgsCount(count_arg, "FEATURES", 0, GATE_MAX_FEATURES_PER_REQUEST, 2, after,
                      &count, err, errlen) != GATE_OK)
        return GATE_ERR;

    /* Allocated even for zero pairs (gate_malloc() never asks for 0 bytes):
     * a present section then always has a non-NULL array, which consumers
     * may hand to memcpy() or qsort() together with a zero count. */
    req->features = gate_malloc(sizeof(gateNamedFeature) * count);
    if (req->features == NULL) return gateArgsNoMemory(err, errlen);

    for (i = 0; i < count; i++) {
        const gateArg *name = &pairs[2 * i], *value = &pairs[2 * i + 1];
        double v;

        if (!gateValidName(name->ptr, name->len, GATE_MAX_NAME_BYTES))
            return gateArgsFail(err, errlen,
                                "feature names must be 1 to %u bytes of UTF-8 without "
                                "control characters", GATE_MAX_NAME_BYTES);
        if (!gateParseDouble(value->ptr, value->len, &v))
            return gateArgsFail(err, errlen,
                                "value of feature '%s' must be a finite decimal number",
                                gateArgsQuote(quote, name));
        if (!(fabs(v) <= GATE_MAX_FEATURE_VALUE))
            return gateArgsFail(err, errlen,
                                "value of feature '%s' is out of range: must be between "
                                "-%g and %g", gateArgsQuote(quote, name),
                                GATE_MAX_FEATURE_VALUE, GATE_MAX_FEATURE_VALUE);
        req->features[i].name = name->ptr;
        req->features[i].name_len = name->len;
        req->features[i].value = v;
    }
    req->in.features = req->features;
    req->in.nfeatures = count;
    *npairs = count;
    return GATE_OK;
}

/* On failure '*pos' is left where it was, and the request is empty. */
int gateParseSections(const gateArg *argv, int argc, int *pos, gateRequest *req,
                      char *err, size_t errlen)
{
    int i = *pos;

    gateArgsClearError(err, errlen);
    memset(req, 0, sizeof(*req));

    while (i >= 0 && i < argc) {
        const gateArg *val;
        const char *name = NULL;
        int avail = argc - i - 1; /* arguments after the section token */
        uint32_t bit = gateArgsSection(&argv[i], &name);
        uint32_t npairs = 0;

        /* The caller's own grammar continues at the first foreign token. A
         * section's value is consumed positionally before this test is made
         * again, so a text that happens to read "VECTOR" is just a text. */
        if (bit == 0) break;

        if (req->in.present & bit) {
            gateArgsFail(err, errlen, "section %s given more than once", name);
            goto fail;
        }
        if (avail < 1) {
            gateArgsFail(err, errlen, "section %s requires 1 argument(s)", name);
            goto fail;
        }
        req->in.present |= bit;
        val = &argv[i + 1];

        if (bit == GATE_INPUT_TEXT) {
            if (val->len > GATE_MAX_TEXT_BYTES) {
                gateArgsFail(err, errlen, "TEXT must be at most %u bytes", GATE_MAX_TEXT_BYTES);
                goto fail;
            }
            req->in.text = val->ptr;
            req->in.text_len = val->len;
            i += 2;
        } else if (bit == GATE_INPUT_VECTOR) {
            /* Raw bytes. Their number depends on the schema, which this
             * layer does not see: gateFeaturize() checks it. */
            req->in.vector = (const unsigned char *)val->ptr;
            req->in.vector_len = val->len;
            i += 2;
        } else {
            if (gateSectionsFeatures(val, avail - 1, req, &npairs, err, errlen) != GATE_OK)
                goto fail;
            i += 2 + 2 * (int)npairs;
        }
    }
    *pos = i;
    return GATE_OK;

fail:
    gateRequestFree(req);
    return GATE_ERR;
}

/* ------------------------------- Targets --------------------------------- */

/* PROBS must sum to 1 within this absolute tolerance (see gate_args.h). */
#define GATE_PROBS_TOLERANCE 1e-3

void gateTargetFree(gateTarget *t) {
    if (t == NULL) return;
    gate_free(t->q);
    memset(t, 0, sizeof(*t));
}

/* The target forms this call accepts, as text for messages; NULL if none.
 * LABEL needs classes; PROBS needs the classifier that learns from them. */
static const char *gateTargetAccepted(const gateConfig *cfg, int allow_classes,
                                      int allow_correct)
{
    int label = allow_classes && cfg->nclasses > 0;
    int probs = label && cfg->mode == GATE_MODE_CLASSIFIER;

    if (probs) return allow_correct ? "LABEL, PROBS or CORRECT" : "LABEL or PROBS";
    if (label) return allow_correct ? "LABEL or CORRECT" : "LABEL";
    return allow_correct ? "CORRECT" : NULL;
}

/* A known target keyword that this call does not accept: say so, and say
 * what it does accept, because the usual cause is feedback of the other
 * mode's kind (LABEL sent to a SCORE gate, CORRECT to a CLASSIFIER). */
static int gateTargetRefused(const gateConfig *cfg, int allow_classes, int allow_correct,
                             const char *keyword, char *err, size_t errlen)
{
    const char *accepted = gateTargetAccepted(cfg, allow_classes, allow_correct);

    if (accepted == NULL)
        return gateArgsFail(err, errlen, "%s is not accepted here", keyword);
    return gateArgsFail(err, errlen, "%s is not accepted here: expected %s", keyword, accepted);
}

static int gateTargetLabel(const gateConfig *cfg, const gateArg *argv, int avail,
                           gateTarget *t, char *err, size_t errlen)
{
    char quote[GATE_QUOTE_BUF];
    int idx;

    if (avail < 1) return gateArgsFail(err, errlen, "LABEL requires a class label");
    idx = gateConfigLabelIndex(cfg, argv[0].ptr, argv[0].len);
    if (idx < 0)
        return gateArgsFail(err, errlen, "unknown class label '%s'",
                            gateArgsQuote(quote, &argv[0]));

    t->q = gate_calloc(cfg->nclasses, sizeof(double)); /* all bits zero is +0.0 */
    if (t->q == NULL) return gateArgsNoMemory(err, errlen);
    t->q[idx] = 1.0;
    t->fb.kind = GATE_FEEDBACK_LABEL;
    t->fb.label = idx;
    return GATE_OK;
}

static int gateTargetProbs(const gateConfig *cfg, const gateArg *argv, int avail,
                           gateTarget *t, char *err, size_t errlen)
{
    char quote[GATE_QUOTE_BUF];
    uint32_t k = cfg->nclasses, i, best = 0;
    double sum = 0.0;

    if (avail < 0 || (uint32_t)avail < k)
        return gateArgsFail(err, errlen, "PROBS requires %u values", k);
    t->q = gate_malloc(sizeof(double) * k);
    if (t->q == NULL) return gateArgsNoMemory(err, errlen);

    /* Exactly K arguments are values, whatever they look like. If one is
     * not a probability the message still leads with the count, since the
     * usual cause is a list that is one short and ran into the next token. */
    for (i = 0; i < k; i++) {
        double p;

        if (!gateParseDouble(argv[i].ptr, argv[i].len, &p) || !(p >= 0.0))
            return gateArgsFail(err, errlen,
                                "PROBS requires %u values: '%s' is not a finite "
                                "non-negative number", k, gateArgsQuote(quote, &argv[i]));
        t->q[i] = p;
        sum += p;
    }
    /* The values are finite and non-negative, so the sum is never a NaN; a
     * sum that overflowed to infinity fails this test like any other. */
    if (!(fabs(sum - 1.0) <= GATE_PROBS_TOLERANCE))
        return gateArgsFail(err, errlen, "PROBS must sum to 1");

    /* The reference label is taken from the renormalized values, the ones
     * the caller trains on and replicates: whoever recomputes the argmax
     * from them gets this label. Strict '>' keeps the lowest index on ties. */
    for (i = 0; i < k; i++) t->q[i] /= sum;
    for (i = 1; i < k; i++)
        if (t->q[i] > t->q[best]) best = i;

    t->fb.kind = GATE_FEEDBACK_PROBS;
    t->fb.label = (int32_t)best;
    t->fb.probs = t->q;
    return GATE_OK;
}

static int gateTargetCorrect(const gateArg *argv, int avail, gateTarget *t,
                             char *err, size_t errlen)
{
    /* One character, so that "01", "+1" or "1.0" are not a second and a
     * third way to spell a verdict. */
    if (avail < 1 || argv[0].len != 1 || (argv[0].ptr[0] != '0' && argv[0].ptr[0] != '1'))
        return gateArgsFail(err, errlen, "CORRECT must be 0 or 1");
    t->fb.kind = GATE_FEEDBACK_CORRECT;
    t->fb.label = -1; /* no class is involved */
    t->fb.correct = argv[0].ptr[0] == '1';
    return GATE_OK;
}

/* On failure '*pos' is left where it was, and the target is empty. */
int gateParseTarget(const gateConfig *cfg, const gateArg *argv, int argc, int *pos,
                    int allow_classes, int allow_correct, gateTarget *t,
                    char *err, size_t errlen)
{
    const gateArg *keyword, *values;
    int i = *pos, avail, used, rc;

    gateArgsClearError(err, errlen);
    memset(t, 0, sizeof(*t));

    if (i >= 0 && i < argc) {
        int probs;

        keyword = &argv[i];
        values = keyword + 1;
        avail = argc - i - 1; /* arguments after the keyword */
        probs = gateArgIs(keyword, "PROBS");

        if (probs || gateArgIs(keyword, "LABEL")) {
            const char *name = probs ? "PROBS" : "LABEL";

            if (!allow_classes)
                return gateTargetRefused(cfg, allow_classes, allow_correct, name, err, errlen);
            if (cfg->nclasses == 0)
                return gateArgsFail(err, errlen, "%s requires a gate with CLASSES", name);
            if (probs && cfg->mode != GATE_MODE_CLASSIFIER)
                return gateArgsFail(err, errlen, "PROBS is only accepted by CLASSIFIER gates");
            if (probs) {
                rc = gateTargetProbs(cfg, values, avail, t, err, errlen);
                used = 1 + (int)cfg->nclasses;
            } else {
                rc = gateTargetLabel(cfg, values, avail, t, err, errlen);
                used = 2;
            }
        } else if (gateArgIs(keyword, "CORRECT")) {
            if (!allow_correct)
                return gateTargetRefused(cfg, allow_classes, allow_correct, "CORRECT",
                                         err, errlen);
            rc = gateTargetCorrect(values, avail, t, err, errlen);
            used = 2;
        } else {
            rc = GATE_ERR;
            used = 0;
        }
        if (rc == GATE_OK) {
            *pos = i + used;
            return GATE_OK;
        }
        if (used != 0) {
            gateTargetFree(t);
            return GATE_ERR;
        }
    }

    /* No target at all: the end of the command, or some other token. */
    {
        const char *accepted = gateTargetAccepted(cfg, allow_classes, allow_correct);

        return gateArgsFail(err, errlen, "expected %s",
                            accepted != NULL ? accepted : "LABEL, PROBS or CORRECT");
    }
}

/* ------------------------------ Small enums ------------------------------ */

int gateParseResetScope(const gateArg *a, gateResetScope *scope) {
    if (gateArgIs(a, "CANDIDATE")) *scope = GATE_RESET_CANDIDATE;
    else if (gateArgIs(a, "CALIBRATION")) *scope = GATE_RESET_CALIBRATION;
    else if (gateArgIs(a, "MODEL")) *scope = GATE_RESET_MODEL;
    else if (gateArgIs(a, "ALL")) *scope = GATE_RESET_ALL;
    else return 0;
    return 1;
}
